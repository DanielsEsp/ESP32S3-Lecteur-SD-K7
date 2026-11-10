

// ESP32-S3 N16R8 + Waveshare 320x480 tactile + lecteur SD séparé
// Lecteur de fichiers audio de la carte SD (/rec), interface portrait avec fond et galets
// Dossier du sketch : lecteur_sd/  ->  lecteur_sd.ino, types.h
// SPIFFS : /fond.jpg  /galetGauche.bmp  /galetDroit.bmp  /play.bmp
// Carte SD : fichiers .mp3 / .aac / .ogg dans le dossier /rec

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <FS.h>
#include <SD.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include "U8g2_for_TFT_eSPI.h"
#include <TJpg_Decoder.h>
#include <Audio.h>

#include "types.h"                        // Scroller, RecEntry, Zone

// =====================  À ADAPTER  =====================
// I2S -> MAX98357A
#define I2S_BCLK 5
#define I2S_LRC  6
#define I2S_DOUT 4

// Tactile FT6336U (I2C 0x38)
#define TP_ADDR 0x38
#define TP_SDA  8
#define TP_SCL  9
#define TP_RST  16

// Carte SD (bus SPI séparé de l'écran)
#define SD_CS   38
#define SD_MOSI 40
#define SD_SCK  41
#define SD_MISO 42
#define SD_FREQ 4000000

// 1 = interface (écran, tactile, galets) sur le coeur 0, audio seul sur le coeur 1
#define UI_ON_CORE0 1

// Animation des galets : intervalle en ms (plus grand = plus léger pour le processeur)
#define GALET_MS 45

// Icône play.bmp
#define PLAY_BLACK_TOL  24   // noir "presque noir" accepté comme transparent (0 = noir pur seulement)
#define PLAY_ROUND_MASK 1    // 1 = tout ce qui est hors du disque (les coins) devient transparent

// Couleurs
#define COL_FARTIST tft.color565(60, 235, 255)
#define COL_FTITLE  TFT_WHITE
#define COL_FBAR    tft.color565(60, 235, 255)
// ======================================================

#define SCR_W 320                         // portrait en permanence (rotation 0)
#define SCR_H 480

TFT_eSPI tft = TFT_eSPI();
U8g2_for_TFT_eSPI u8f;
Audio audio;
Preferences prefs;

enum Page { PAGE_LIST, PAGE_PLAYER };
volatile Page page = PAGE_LIST;

// ---- Liste des fichiers (PSRAM) ----
const int MAX_REC = 400;
RecEntry* recList = nullptr;
int       recCount = 0;
bool      sdOk = false;
SPIClass  sdSPI(FSPI);                    // l'écran utilise HSPI (USE_HSPI_PORT)

int topIdx = 0;
int volume = 6;                           // 0..21
uint32_t volSaveAt = 0;                   // sauvegarde différée du volume

// Message temporaire sur la liste
char     toastMsg[48] = "";
uint32_t toastUntil = 0;

// ---- Mise en page : liste (portrait) ----
const int HEADER_H = 36;
const int LIST_Y   = 40;
const int ROW_H    = 36;
const int ROWS     = 10;                  // 40 .. 400
const int STATUS_Y = 414;
const int BTN_Y    = 430;
const int BTN_H    = 44;
const int BTN_W    = 96;                  // 3 boutons : Up, Dn, MAJ

// Appui long (suppression)
bool     pressActive = false;
uint32_t pressT0 = 0;
int      pressIdx = -1;
bool     confirmAct = false;
int      confirmIdx = -1;

// ---- Lecture d'un fichier ----
volatile bool playFileReq = false;        // demande de lecture (tâche audio)
volatile bool pauseReq    = false;        // demande pause/reprise
volatile bool stopReq     = false;        // demande d'arrêt (tâche audio)
volatile bool volDirty    = false;
volatile int  fileResult  = 0;            // 0 = en cours, 1 = ouvert, -1 = échec
volatile uint32_t audCur = 0, audDur = 0; // temps écoulé / durée, publiés par la tâche audio
char     playFilePath[128] = "";
int      fileIdx = -1;
bool     fileOpened = false, filePaused = false, fileEnded = false, fileFailShown = false;
bool     shuffleMode = false;             // lecture aléatoire
uint32_t fileRunMs = 0;
String   fArtist = "", fTitle = "";
uint32_t fpLastCur = 0xFFFFFFFFu, fpLastDur = 0xFFFFFFFFu;
bool     fpForce = true;

// ---- Fond décodé en PSRAM ----
#define BG_W 320
#define BG_H 480
uint16_t* bgBuf = nullptr;

Scroller scFArtist, scFTitle;

// ======================================================================
//                              POLICES / TEXTES
// ======================================================================
#define FONT_N 3
const uint8_t* const FONTS[FONT_N] = {
  u8g2_font_profont22_tf,
  u8g2_font_profont17_tf,
  u8g2_font_profont15_tf
};

void chopUtf8(String& s) {
  if (!s.length()) return;
  int i = s.length() - 1;
  while (i > 0 && (s[i] & 0xC0) == 0x80) i--;
  s.remove(i);
}

// Texte fixe (réduit puis tronqué)
void drawTextFit(const String& text, int x, int yc, int maxW, uint16_t color, int firstFont) {
  String t = text;
  u8f.setForegroundColor(color);
  int f = firstFont;
  for (; f < FONT_N; f++) {
    u8f.setFont(FONTS[f]);
    if (u8f.getUTF8Width(t.c_str()) <= maxW) break;
  }
  if (f >= FONT_N) {
    u8f.setFont(FONTS[FONT_N - 1]);
    while (t.length() > 1 && u8f.getUTF8Width((t + "~").c_str()) > maxW) chopUtf8(t);
    t += "~";
  }
  int asc = u8f.getFontAscent(), desc = u8f.getFontDescent();
  u8f.setCursor(x, yc + (asc + desc) / 2);
  u8f.print(t);
}

// ---------------- Texte défilant ----------------
void scrollInit(Scroller& s, int x, int y, int w, int h, uint16_t color, uint16_t bg, int fFirst, int fLast) {
  s.x = x; s.y = y; s.w = w; s.h = h; s.color = color; s.bg = bg;
  s.fFirst = fFirst; s.fLast = fLast;
}

void scrollRender(Scroller& s) {
  tft.setViewport(s.x, s.y, s.w, s.h);
  if (s.bgImg && bgBuf) {                 // fond image restauré ligne par ligne
    for (int r = 0; r < s.h; r++)
      tft.pushImage(0, r, s.w, 1, bgBuf + (s.y + r) * BG_W + s.x);
  } else {
    tft.fillRect(0, 0, s.w, s.h, s.bg);
  }
  u8f.setFont(FONTS[s.font]);
  u8f.setFontMode(1);                     // 1 = fond transparent (pas de rectangle noir derrière les lettres)
  u8f.setForegroundColor(s.color);
  int asc = u8f.getFontAscent(), desc = u8f.getFontDescent();
  u8f.setCursor(s.scrolling ? -s.offset : 0, s.h / 2 + (asc + desc) / 2);
  u8f.print(s.text);
  tft.resetViewport();
}

void scrollSet(Scroller& s, const String& text) {
  s.text = text.length() ? text : String("-");
  int f = s.fFirst;
  for (; f <= s.fLast; f++) {
    u8f.setFont(FONTS[f]);
    if (u8f.getUTF8Width(s.text.c_str()) <= s.w) break;
  }
  if (f > s.fLast) { f = s.fLast; u8f.setFont(FONTS[f]); s.scrolling = true; }
  else s.scrolling = false;
  s.font = f;
  s.textW = u8f.getUTF8Width(s.text.c_str());
  s.offset = 0; s.dir = 1;
  s.pauseUntil = millis() + 1500;
  s.lastStep = millis();
  scrollRender(s);
}

void scrollTick(Scroller& s) {
  if (!s.scrolling) return;
  uint32_t now = millis();
  if (now < s.pauseUntil || now - s.lastStep < 35) return;
  s.lastStep = now;
  int maxOff = s.textW - s.w + 8;
  s.offset += s.dir * 2;
  if (s.offset >= maxOff) { s.offset = maxOff; s.dir = -1; s.pauseUntil = now + 1200; }
  else if (s.offset <= 0) { s.offset = 0;      s.dir = 1;  s.pauseUntil = now + 1200; }
  scrollRender(s);
}

// ======================================================================
//                              CARTE SD
// ======================================================================
bool sdInit() {
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdOk = SD.begin(SD_CS, sdSPI, SD_FREQ);
  Serial.println(sdOk ? "SD OK" : "SD absente ou erreur");
  return sdOk;
}

bool isAudioName(const char* n) {
  size_t l = strlen(n);
  if (l < 5) return false;
  const char* e = n + l - 4;
  return !strcasecmp(e, ".mp3") || !strcasecmp(e, ".aac") || !strcasecmp(e, ".ogg");
}

int cmpRec(const void* a, const void* b) {
  const RecEntry* x = (const RecEntry*)a;
  const RecEntry* y = (const RecEntry*)b;
  if (x->t != y->t) return (x->t > y->t) ? -1 : 1;        // plus récent d'abord
  return strcasecmp(x->name, y->name);
}

// À n'appeler que lorsque l'audio est arrêté (la SD n'est pas partagée entre tâches)
void recScan() {
  recCount = 0;
  if (!recList) return;
  if (!sdOk && !sdInit()) return;

  File dir = SD.open("/rec");
  if (!dir) { Serial.println("/rec absent"); return; }
  if (!dir.isDirectory()) { dir.close(); return; }

  File f = dir.openNextFile();
  while (f) {
    if (!f.isDirectory() && recCount < MAX_REC) {
      const char* full = f.name();
      const char* nm = strrchr(full, '/');
      nm = nm ? nm + 1 : full;
      if (nm[0] != '_' && nm[0] != '.' && isAudioName(nm)) {
        RecEntry& e = recList[recCount++];
        strlcpy(e.name, nm, sizeof(e.name));
        e.size = (uint32_t)f.size();
        e.t = (uint32_t)f.getLastWrite();
      }
    }
    f.close();
    f = dir.openNextFile();
  }
  dir.close();
  qsort(recList, recCount, sizeof(RecEntry), cmpRec);
  Serial.printf("%d fichiers\n", recCount);
}

void recDelete(int idx) {
  if (idx < 0 || idx >= recCount) return;
  String p = String("/rec/") + recList[idx].name;
  bool ok = SD.remove(p.c_str());
  Serial.printf("Suppression %s : %s\n", p.c_str(), ok ? "OK" : "echec");
  recScan();
}

// Nom de fichier -> "Artiste" / "Titre" (sans extension)
void fileNameParts(const char* name, String& artist, String& title) {
  char base[100];
  strlcpy(base, name, sizeof(base));
  char* d = strrchr(base, '.');
  if (d) *d = 0;
  String s(base);
  int p = s.indexOf(" - ");
  if (p > 0) { artist = s.substring(0, p); title = s.substring(p + 3); }
  else       { artist = ""; title = s; }
}

void fmtSize(char* out, size_t n, uint32_t sz) {
  if (sz >= 1048576UL) snprintf(out, n, "%.1f Mo", sz / 1048576.0f);
  else                 snprintf(out, n, "%u Ko", (unsigned)(sz / 1024));
}

void fmtTime(char* out, size_t n, uint32_t s) {
  if (s >= 3600) snprintf(out, n, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)((s / 60) % 60), (unsigned)(s % 60));
  else           snprintf(out, n, "%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

void toast(const char* m) {
  strlcpy(toastMsg, m, sizeof(toastMsg));
  toastUntil = millis() + 3000;
}

// Fichier au hasard, différent de celui en cours
int randomIdx() {
  if (recCount <= 1) return 0;
  int i;
  do { i = (int)(esp_random() % (uint32_t)recCount); } while (i == fileIdx);
  return i;
}

// ======================================================================
//                    AUDIO (tâche dédiée, coeur 1)
// ======================================================================
void audioTask(void*) {
  for (;;) {
    if (stopReq) {
      audio.stopSong();
      audCur = 0; audDur = 0;
      stopReq = false;                                  // signale à l'interface : arrêt effectué
    }
    if (playFileReq) {
      playFileReq = false;
      audCur = 0; audDur = 0;
      audio.stopSong();
      audio.setVolume(volume);
      fileResult = audio.connecttoFS(SD, playFilePath) ? 1 : -1;
    }
    if (pauseReq) { pauseReq = false; audio.pauseResume(); }
    if (volDirty) { volDirty = false; audio.setVolume(volume); }

    // Temps de lecture : mesuré ici, dans la tâche audio, qui seule touche au fichier.
    // L'interface lit simplement audCur / audDur (aucun accès SD depuis l'interface).
    static uint32_t lastInfo = 0;
    if (fileResult == 1 && millis() - lastInfo >= 500) {
      lastInfo = millis();
      audCur = audio.getAudioCurrentTime();
      audDur = audio.getAudioFileDuration();
    }

    audio.loop();
    vTaskDelay(1);
  }
}

// ======================================================================
//                              TACTILE
// ======================================================================
void touchInit() {
  if (TP_RST >= 0) {
    pinMode(TP_RST, OUTPUT);
    digitalWrite(TP_RST, LOW);  delay(10);
    digitalWrite(TP_RST, HIGH); delay(100);
  }
  Wire.begin(TP_SDA, TP_SCL, 400000);
}

// Retourne le nombre de doigts (0, 1 ou 2) ; x,y = premier doigt, repère portrait
int readTouch(uint16_t &x, uint16_t &y) {
  Wire.beginTransmission(TP_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return 0;
  if (Wire.requestFrom((uint8_t)TP_ADDR, (uint8_t)5) != 5) return 0;

  uint8_t n  = Wire.read() & 0x0F;
  uint8_t xh = Wire.read();
  uint8_t xl = Wire.read();
  uint8_t yh = Wire.read();
  uint8_t yl = Wire.read();
  if (n == 0 || n > 2) return 0;

  x = ((xh & 0x0F) << 8) | xl;
  y = ((yh & 0x0F) << 8) | yl;
  return n;
}

// ======================================================================
//                         PAGE LISTE (portrait)
// ======================================================================
void drawListHeader() {
  tft.fillRect(0, 0, SCR_W, HEADER_H, TFT_NAVY);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("Lecteur SD", 10, HEADER_H / 2, 4);
  char v[16];
  snprintf(v, sizeof(v), "Vol %d", volume);
  tft.setTextDatum(MR_DATUM);
  tft.drawString(v, SCR_W - 10, HEADER_H / 2, 2);
}

void drawList() {
  tft.fillRect(0, LIST_Y, SCR_W, STATUS_Y + 14 - LIST_Y, TFT_BLACK);
  tft.setTextSize(1);

  if (recCount == 0) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(sdOk ? "Aucun fichier" : "Carte SD absente", SCR_W / 2, LIST_Y + 5 * ROW_H, 4);
    return;
  }

  for (int r = 0; r < ROWS; r++) {
    int idx = topIdx + r;
    if (idx >= recCount) break;
    int y = LIST_Y + r * ROW_H;
    tft.drawFastHLine(0, y + ROW_H - 1, SCR_W, TFT_DARKGREY);

    char base[100];
    strlcpy(base, recList[idx].name, sizeof(base));
    char* dot = strrchr(base, '.');
    if (dot) *dot = 0;
    drawTextFit(String(base), 8, y + ROW_H / 2, SCR_W - 90, TFT_WHITE, 1);

    char sz[16];
    fmtSize(sz, sizeof(sz), recList[idx].size);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setTextDatum(MR_DATUM);
    tft.drawString(sz, SCR_W - 6, y + ROW_H / 2, 2);
  }

  // Ligne d'état
  String ctx = "Appui long : supprimer";
  uint16_t ctxCol = TFT_LIGHTGREY;
  if (millis() < toastUntil) { ctx = toastMsg; ctxCol = TFT_ORANGE; }
  drawTextFit(ctx, 6, STATUS_Y, 190, ctxCol, 2);

  char buf[32];
  snprintf(buf, sizeof(buf), "%d-%d / %d", topIdx + 1, min(recCount, topIdx + ROWS), recCount);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setTextDatum(MR_DATUM);
  tft.drawString(buf, SCR_W - 6, STATUS_Y, 2);
}

void drawListButtons() {
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(1);
  const char* lbl[3] = { "Up", "Dn", "MAJ" };
  for (int k = 0; k < 3; k++) {
    int x = 8 + k * (BTN_W + 8);
    tft.fillRoundRect(x, BTN_Y, BTN_W, BTN_H, 8, TFT_DARKCYAN);
    tft.setTextColor(TFT_WHITE, TFT_DARKCYAN);
    tft.drawString(lbl[k], x + BTN_W / 2, BTN_Y + BTN_H / 2, 4);
  }
}

void redrawList() {
  tft.fillScreen(TFT_BLACK);
  drawListHeader();
  drawList();
  drawListButtons();
}

void drawConfirm() {
  tft.fillRoundRect(20, 150, 280, 170, 10, TFT_BLACK);
  tft.drawRoundRect(20, 150, 280, 170, 10, TFT_WHITE);
  tft.setTextSize(1);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Supprimer ?", 160, 180, 4);
  char base[100];
  strlcpy(base, recList[confirmIdx].name, sizeof(base));
  char* dot = strrchr(base, '.');
  if (dot) *dot = 0;
  drawTextFit(String(base), 30, 225, 260, TFT_CYAN, 1);

  tft.fillRoundRect(35, 255, 120, 50, 8, TFT_RED);
  tft.fillRoundRect(165, 255, 120, 50, 8, TFT_DARKCYAN);
  tft.setTextColor(TFT_WHITE, TFT_RED);
  tft.drawString("Oui", 95, 280, 4);
  tft.setTextColor(TFT_WHITE, TFT_DARKCYAN);
  tft.drawString("Non", 225, 280, 4);
}

// ======================================================================
//        PAGE LECTEUR (portrait 320x480, fond + galets, SPIFFS)
// ======================================================================
const Zone Z_ARTIST = {  68,  67, 180, 22 };
const Zone Z_TITLE  = {  68, 104, 180, 22 };
const Zone Z_PLAY   = { 125, 330,  70, 70 };
const Zone Z_PREV   = {  42, 338,  56, 56 };
const Zone Z_NEXT   = { 221, 338,  56, 56 };
const Zone Z_RAND   = {  20, 418,  56, 56 };   // lecture aléatoire
const Zone Z_BACK   = { 105, 418,  56, 56 };   // retour à la liste
const Zone Z_VMOINS = { 175, 418,  56, 56 };
const Zone Z_VPLUS  = { 245, 418,  56, 56 };
const Zone Z_BAR    = {  34, 298, 254, 10 };

#define F_FOND   "/fond.jpg"
#define F_GAUCHE "/galetGauche.bmp"
#define F_DROIT  "/galetDroit.bmp"
#define F_PLAY   "/play.bmp"

// Galets (centres sur fond.jpg)
#define GL_X 89
#define GL_Y 183
#define GR_X 235
#define GR_Y 183
const float VIT_GAUCHE = 3.0f;           // degrés par image
const float VIT_DROIT  = 5.0f;           // un peu plus vite
// Tampon de recomposition : 124 de large (diagonale d'un galet), 118 de haut,
// démarre à y=127 pour ne jamais effacer la ligne du titre (y 67..126)
#define TAMP_W   124
#define TAMP_H   118
#define TAMP_TOP 127

// Icône play (65x65) : affichée pendant la lecture, fond d'origine restauré en pause
#define PLAY_X 128
#define PLAY_Y 332
#define PLAY_S 65

TFT_eSprite galetG = TFT_eSprite(&tft);
TFT_eSprite galetD = TFT_eSprite(&tft);
TFT_eSprite tampon = TFT_eSprite(&tft);
TFT_eSprite spritePlay = TFT_eSprite(&tft);
TFT_eSprite tamponPlay = TFT_eSprite(&tft);
bool uiOk = false;
bool playOk = false;                     // play.bmp chargé
bool playIconShown = false;
float angG = 0, angD = 0;

// --- Décodage de fond.jpg directement dans la PSRAM ---
bool bgOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (!bgBuf) return 0;
  for (int r = 0; r < h; r++) {
    int yy = y + r;
    if (yy < 0 || yy >= BG_H) continue;
    int cw = w;
    if (x + cw > BG_W) cw = BG_W - x;
    if (cw <= 0) continue;
    memcpy(bgBuf + yy * BG_W + x, bitmap + r * w, cw * 2);
  }
  return 1;
}

// --- BMP 24/32 bits (SPIFFS) -> TFT_eSprite 16 bits ---
// tol > 0 : les pixels dont r, g et b sont tous <= tol sont forcés au noir pur (transparent)
bool loadBmpSprite(TFT_eSprite& s, const char* path, int tol) {
  File f = SPIFFS.open(path, "r");
  if (!f) { Serial.printf("BMP introuvable (SPIFFS) : %s\n", path); return false; }
  uint8_t h[54];
  if (f.read(h, 54) != 54 || h[0] != 'B' || h[1] != 'M') { f.close(); return false; }
  uint32_t off; int32_t w, ht; uint16_t bpp; uint32_t comp;
  memcpy(&off, h + 10, 4); memcpy(&w, h + 18, 4); memcpy(&ht, h + 22, 4);
  memcpy(&bpp, h + 28, 2); memcpy(&comp, h + 30, 4);
  bool bottomUp = ht > 0;
  int absH = abs(ht);
  if (w <= 0 || w > 128 || absH <= 0 || absH > 128 ||
      !(bpp == 24 || bpp == 32) || !(comp == 0 || comp == 3)) {
    Serial.printf("BMP non gere : %s (%dx%d, %u bits)\n", path, (int)w, (int)ht, (unsigned)bpp);
    f.close();
    return false;
  }
  s.setColorDepth(16);
  if (!s.createSprite(w, absH)) { f.close(); return false; }
  s.fillSprite(0);
  int bp = bpp / 8;
  uint32_t rowSize = ((bpp * w + 31) / 32) * 4;
  uint8_t row[128 * 4 + 4];
  for (int y = 0; y < absH; y++) {
    int srcRow = bottomUp ? (absH - 1 - y) : y;
    f.seek(off + (uint32_t)srcRow * rowSize);
    if (f.read(row, rowSize) != (int)rowSize) break;
    for (int x = 0; x < w; x++) {
      uint8_t b = row[x * bp], g = row[x * bp + 1], r = row[x * bp + 2];
      uint16_t c = tft.color565(r, g, b);
      if (tol > 0 && r <= tol && g <= tol && b <= tol) c = 0;   // presque noir -> transparent
      s.drawPixel(x, y, c);
    }
  }
  f.close();
  return true;
}

// --- Chargement unique du fond, des galets et de l'icône play (SPIFFS) ---
bool uiLoad() {
  if (uiOk) return true;

  if (!bgBuf) bgBuf = (uint16_t*)heap_caps_malloc(BG_W * BG_H * 2, MALLOC_CAP_SPIRAM);
  if (!bgBuf) { Serial.println("PSRAM insuffisante pour le fond"); return false; }

  File f = SPIFFS.open(F_FOND, "r");
  if (!f) { Serial.println("/fond.jpg introuvable (SPIFFS)"); return false; }
  size_t len = f.size();
  uint8_t* jb = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
  if (!jb) { f.close(); return false; }
  f.read(jb, len);
  f.close();

  memset(bgBuf, 0, BG_W * BG_H * 2);
  uint16_t w = 0, h = 0;
  TJpgDec.getJpgSize(&w, &h, jb, len);
  if (w != BG_W || h != BG_H) Serial.printf("fond.jpg : %dx%d (attendu 320x480)\n", w, h);
  TJpgDec.drawJpg(0, 0, jb, len);
  free(jb);

  if (!loadBmpSprite(galetG, F_GAUCHE, 0)) return false;
  if (!loadBmpSprite(galetD, F_DROIT, 0))  return false;
  galetG.setPivot(galetG.width() / 2, galetG.height() / 2);
  galetD.setPivot(galetD.width() / 2, galetD.height() / 2);
  tampon.setColorDepth(16);
  if (!tampon.createSprite(TAMP_W, TAMP_H)) return false;

  // Icône play : non bloquante si absente
  playOk = loadBmpSprite(spritePlay, F_PLAY, PLAY_BLACK_TOL);
  if (playOk) {
    int cw = spritePlay.width(), ch = spritePlay.height();
#if PLAY_ROUND_MASK
    float cx = (cw - 1) / 2.0f, cy = (ch - 1) / 2.0f, rr = cw / 2.0f;
    for (int yy = 0; yy < ch; yy++)
      for (int xx = 0; xx < cw; xx++) {
        float dx = xx - cx, dy = yy - cy;
        if (dx * dx + dy * dy > rr * rr) spritePlay.drawPixel(xx, yy, 0);   // coins : transparent
      }
#endif
    spritePlay.setPivot(cw / 2, ch / 2);
    tamponPlay.setColorDepth(16);
    playOk = tamponPlay.createSprite(cw, ch);
    if (playOk) tamponPlay.setPivot(cw / 2, ch / 2);
  }
  if (!playOk) Serial.println("play.bmp absent : pas d'icone de lecture");

  uiOk = true;
  Serial.println("Interface lecteur chargee (SPIFFS)");
  return true;
}

// Remet le fond (image) sur un rectangle
void restoreBg(int x, int y, int w, int h) {
  if (!bgBuf) { tft.fillRect(x, y, w, h, TFT_BLACK); return; }
  for (int r = 0; r < h; r++) tft.pushImage(x, y + r, w, 1, bgBuf + (y + r) * BG_W + x);
}

// Icône play : visible pendant la lecture, fond d'origine en pause / fin
void updatePlayIcon(bool force) {
  if (!uiOk || !playOk) return;
  bool want = fileOpened && !filePaused && !fileEnded && fileResult != -1;
  if (!force && want == playIconShown) return;
  playIconShown = want;
  if (want) {
    // Même méthode que les galets : fond copié dans un tampon, icône posée avec noir transparent
    uint16_t* d = (uint16_t*)tamponPlay.getPointer();
    int tw = tamponPlay.width(), th = tamponPlay.height();
    for (int r = 0; r < th; r++)
      memcpy(d + r * tw, bgBuf + (PLAY_Y + r) * BG_W + PLAY_X, tw * 2);
    spritePlay.pushRotated(&tamponPlay, 0, 0);       // 0 = couleur transparente
    tamponPlay.pushSprite(PLAY_X, PLAY_Y);
  } else {
    restoreBg(PLAY_X, PLAY_Y, PLAY_S, PLAY_S);
  }
}

// Un galet : fond recopié dans le tampon, galet tourné (noir transparent), envoi à l'écran
void drawGalet(TFT_eSprite& g, int cx, int cy, float ang) {
  if (!uiOk) return;
  int zx = cx - TAMP_W / 2;
  int zy = TAMP_TOP;
  uint16_t* d = (uint16_t*)tampon.getPointer();
  for (int r = 0; r < TAMP_H; r++)
    memcpy(d + r * TAMP_W, bgBuf + (zy + r) * BG_W + zx, TAMP_W * 2);
  tampon.setPivot(cx - zx, cy - zy);
  g.pushRotated(&tampon, (int16_t)ang, 0);       // 0 = noir pur transparent
  tampon.pushSprite(zx, zy);
}

void drawGalets() {
  drawGalet(galetG, GL_X, GL_Y, angG);
  drawGalet(galetD, GR_X, GR_Y, angD);
}

void drawFileFields() {
  scrollSet(scFArtist, fArtist);
  scrollSet(scFTitle, fTitle);
}

void drawFileProgress() {
  // Valeurs mesurées par la tâche audio : aucun accès à la carte SD depuis ici
  uint32_t cur = (fileResult == 1) ? audCur : 0;
  uint32_t dur = (fileResult == 1) ? audDur : 0;
  if (fileEnded && dur) cur = dur;
  if (!fpForce && cur == fpLastCur && dur == fpLastDur) return;
  fpForce = false;
  fpLastCur = cur;
  fpLastDur = dur;

  float fr = dur ? min(1.0f, (float)cur / (float)dur) : 0.0f;

  // Barre de progression (cyan)
  restoreBg(Z_BAR.x, Z_BAR.y, Z_BAR.w, Z_BAR.h);
  int fw = (int)(Z_BAR.w * fr);
  if (fw > 0) tft.fillRect(Z_BAR.x, Z_BAR.y, fw, Z_BAR.h, COL_FBAR);

  // Temps écoulé / durée, juste sous la barre
  restoreBg(Z_BAR.x, 311, Z_BAR.w, 16);
  char t1[16], t2[16];
  fmtTime(t1, sizeof(t1), cur);
  if (dur) fmtTime(t2, sizeof(t2), dur); else strlcpy(t2, "--:--", sizeof(t2));
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE);               // fond transparent
  tft.setTextDatum(ML_DATUM);
  tft.drawString(t1, Z_BAR.x, 319, 2);
  tft.setTextDatum(MR_DATUM);
  tft.drawString(t2, Z_BAR.x + Z_BAR.w, 319, 2);
}

void fileShow() {                            // éléments variables (le fond n'est pas redessiné)
  if (page != PAGE_PLAYER) return;
  drawFileFields();
  fpForce = true;
  drawFileProgress();
  drawGalets();
  updatePlayIcon(true);
}

void redrawFile() {                          // écran complet
  if (bgBuf) tft.pushImage(0, 0, BG_W, BG_H, bgBuf);
  else       tft.fillScreen(TFT_BLACK);
  playIconShown = false;
  fileShow();
}

// Ouvre un fichier : la lecture démarre dans la tâche audio
void fileOpen(int idx) {
  if (idx < 0 || idx >= recCount) return;
  fileIdx = idx;

  fileNameParts(recList[idx].name, fArtist, fTitle);
  snprintf(playFilePath, sizeof(playFilePath), "/rec/%s", recList[idx].name);

  fileOpened = true;
  filePaused = false;
  fileEnded = false;
  fileFailShown = false;
  fileResult = 0;
  fileRunMs = millis();
  fpForce = true;
  playFileReq = true;                        // en dernier : la tâche audio lit les variables ci-dessus
}

void goFile(int idx) {
  if (idx < 0 || idx >= recCount) return;
  if (!uiLoad()) {
    toast("Fond / galets manquants (SPIFFS)");
    drawList();
    return;
  }
  shuffleMode = false;                       // choix manuel : lecture dans l'ordre
  fileOpen(idx);
  angG = 0; angD = 0;
  page = PAGE_PLAYER;
  redrawFile();
}

// Retour à la liste : on attend l'arrêt réel de l'audio avant de toucher à la SD
void leaveFile() {
  fileOpened = false;
  filePaused = false;
  shuffleMode = false;
  playFileReq = false;
  stopReq = true;
  uint32_t t0 = millis();
  while (stopReq && millis() - t0 < 2000) delay(5);

  page = PAGE_LIST;
  redrawList();
}

// Appelé à chaque tour sur la page lecteur
void fileLoop() {
  uint32_t now = millis();

  if (fileOpened) {
    if (fileResult == -1 && !fileFailShown) {
      fileFailShown = true;
      scrollSet(scFTitle, "Fichier illisible");
    } else if (fileResult == 1 && !filePaused && !fileEnded) {
      if (audio.isRunning()) fileRunMs = now;
      else if (now - fileRunMs > 2500) {                 // fin du fichier
        if (shuffleMode && recCount > 0) {               // aléatoire : on enchaîne au hasard
          fileOpen(randomIdx());
          fileShow();
        } else if (fileIdx + 1 < recCount) {
          fileOpen(fileIdx + 1);
          fileShow();
        } else {
          fileEnded = true;
          fpForce = true;
          drawFileProgress();
        }
      }
    }
  }

  updatePlayIcon(false);                                 // icône play / fond d'origine selon l'état

  scrollTick(scFArtist);
  scrollTick(scFTitle);

  // Rotation des galets : uniquement pendant la lecture
  static uint32_t lastAnim = 0;
  if (now - lastAnim >= GALET_MS) {
    lastAnim = now;
    if (fileResult == 1 && !filePaused && !fileEnded) {
      angG += VIT_GAUCHE; if (angG >= 360) angG -= 360;
      angD += VIT_DROIT;  if (angD >= 360) angD -= 360;
      drawGalets();
    }
  }

  static uint32_t lastProg = 0;
  if (now - lastProg >= 500) { lastProg = now; drawFileProgress(); }
}

// ======================================================================
//                          GESTION DU TOUCHER
// ======================================================================
void volumeChanged() {
  volDirty = true;
  volSaveAt = millis() + 2000;
}

void handleTouch() {
  static uint32_t last = 0;
  static bool waitRelease = false;

  uint16_t x = 0, y = 0;
  int n = readTouch(x, y);

  // --- Boîte de confirmation ---
  if (confirmAct) {
    if (n == 0) { waitRelease = false; return; }
    if (waitRelease || n != 1) return;
    if (y >= 255 && y < 305) {
      if (x >= 35 && x < 155) {                        // Oui
        recDelete(confirmIdx);
        confirmAct = false; waitRelease = true;
        if (topIdx >= recCount) topIdx = max(0, recCount - ROWS);
        redrawList();
      } else if (x >= 165 && x < 285) {                // Non
        confirmAct = false; waitRelease = true;
        redrawList();
      }
    }
    return;
  }

  // --- Page lecteur ---
  if (page == PAGE_PLAYER) {
    if (n == 0) { waitRelease = false; return; }
    if (waitRelease) return;
    if (n >= 2) {                                      // 2 doigts : retour à la liste
      waitRelease = true;
      leaveFile();
      return;
    }
    if (millis() - last < 250) return;
    last = millis();

    if (inZone(x, y, Z_VMOINS)) {                      // volume : répétition si on maintient
      volume = max(0, volume - 1);  volumeChanged();
    } else if (inZone(x, y, Z_VPLUS)) {
      volume = min(21, volume + 1); volumeChanged();
    } else if (inZone(x, y, Z_PLAY)) {                 // lecture / pause
      waitRelease = true;
      if (fileEnded) { fileOpen(fileIdx); fileShow(); }
      else if (fileResult == 1) {
        pauseReq = true;
        filePaused = !filePaused;
      }
    } else if (inZone(x, y, Z_PREV)) {                 // précédent
      waitRelease = true;
      if (fileIdx > 0) { fileOpen(fileIdx - 1); fileShow(); }
    } else if (inZone(x, y, Z_NEXT)) {                 // suivant (au hasard en mode aléatoire)
      waitRelease = true;
      if (shuffleMode && recCount > 0) { fileOpen(randomIdx()); fileShow(); }
      else if (fileIdx + 1 < recCount) { fileOpen(fileIdx + 1); fileShow(); }
    } else if (inZone(x, y, Z_RAND)) {                 // lecture aléatoire
      waitRelease = true;
      if (recCount > 0) {
        shuffleMode = true;
        fileOpen(randomIdx());
        fileShow();
      }
    } else if (inZone(x, y, Z_BACK)) {                 // retour à la liste
      waitRelease = true;
      leaveFile();
    }
    return;
  }

  // --- Page liste : tap court = lecture, appui long = suppression ---
  if (n >= 2) pressActive = false;
  if (n == 0 && pressActive) {                         // relâché avant 0,7 s
    pressActive = false;
    waitRelease = false;
    if (pressIdx >= 0 && pressIdx < recCount) goFile(pressIdx);
    return;
  }
  if (n == 0) { waitRelease = false; return; }
  if (waitRelease) return;

  if (pressActive) {
    if (millis() - pressT0 >= 700) {                   // appui long détecté
      pressActive = false;
      waitRelease = true;
      confirmIdx = pressIdx;
      confirmAct = true;
      drawConfirm();
    }
    return;
  }
  if (n != 1) return;

  if (y >= LIST_Y && y < LIST_Y + ROWS * ROW_H) {
    int idx = topIdx + (y - LIST_Y) / ROW_H;
    if (idx < recCount) { pressActive = true; pressT0 = millis(); pressIdx = idx; }
    return;
  }

  if (y >= BTN_Y && y < BTN_Y + BTN_H) {
    if (millis() - last < 250) return;
    last = millis();
    int k = (x - 8) / (BTN_W + 8);
    if (k < 0 || k > 2) return;
    if (k == 0) topIdx = max(0, topIdx - ROWS);
    else if (k == 1) { if (topIdx + ROWS < recCount) topIdx += ROWS; }
    else { recScan(); topIdx = 0; }
    drawList();
  }
}

// ======================================================================
//                           BOUCLE INTERFACE
// ======================================================================
void uiLoop() {
  handleTouch();

  if (toastUntil && millis() >= toastUntil) {
    toastUntil = 0;
    if (page == PAGE_LIST && !confirmAct) drawList();
  }

  if (page == PAGE_PLAYER) fileLoop();

  if (volSaveAt && millis() >= volSaveAt) {            // volume mémorisé (sans écriture flash à chaque appui)
    volSaveAt = 0;
    prefs.begin("lecteur", false);
    prefs.putInt("vol", volume);
    prefs.end();
  }

  delay(10);
}

void uiTask(void*) {
  for (;;) uiLoop();
}

// ======================================================================
//                              SETUP / LOOP
// ======================================================================
void setup() {
  Serial.begin(115200);
  Serial.printf("PSRAM libre : %u octets\n", (unsigned)ESP.getFreePsram());

  prefs.begin("lecteur", true);
  volume = constrain(prefs.getInt("vol", 6), 0, 21);
  prefs.end();

  tft.init();
  u8f.begin(tft);
  u8f.setFontMode(1);
  tft.setRotation(0);                     // portrait en permanence
  tft.fillScreen(TFT_BLACK);
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(bgOutput);
  touchInit();

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Chargement...", SCR_W / 2, SCR_H / 2, 4);

  if (!SPIFFS.begin(true)) Serial.println("Erreur SPIFFS !");
  sdInit();

  recList = (RecEntry*)heap_caps_malloc(MAX_REC * sizeof(RecEntry), MALLOC_CAP_SPIRAM);
  if (!recList) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("Erreur memoire (PSRAM)", SCR_W / 2, SCR_H / 2, 4);
    while (true) delay(1000);
  }

  scrollInit(scFArtist, Z_ARTIST.x, Z_ARTIST.y, Z_ARTIST.w, Z_ARTIST.h, COL_FARTIST, TFT_BLACK, 1, 1);
  scrollInit(scFTitle,  Z_TITLE.x,  Z_TITLE.y,  Z_TITLE.w,  Z_TITLE.h,  COL_FTITLE,  TFT_BLACK, 1, 1);
  scFArtist.bgImg = true;                 // le fond est restauré depuis fond.jpg
  scFTitle.bgImg  = true;

  uiLoad();                               // fond + galets + play (réessayé à l'ouverture d'un fichier si échec)

  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(volume);
  xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 3, nullptr, 1);

  recScan();
  page = PAGE_LIST;
  redrawList();

#if UI_ON_CORE0
  xTaskCreatePinnedToCore(uiTask, "ui", 16384, nullptr, 1, nullptr, 0);
#endif
}

void loop() {
#if UI_ON_CORE0
  vTaskDelete(nullptr);                   // l'interface tourne dans sa propre tâche (coeur 0)
#else
  uiLoop();
#endif
}


//
