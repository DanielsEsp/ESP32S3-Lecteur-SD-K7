

#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

// Texte défilant
struct Scroller {
  String text;
  int x = 0, y = 0, w = 0, h = 0;
  uint16_t color = TFT_WHITE, bg = TFT_BLACK;
  int fFirst = 0, fLast = 1;
  int font = 0, textW = 0, offset = 0, dir = 1;
  bool scrolling = false;
  bool bgImg = false;          // le fond du texte est restauré depuis fond.jpg (PSRAM)
  uint32_t pauseUntil = 0, lastStep = 0;
};

// Fichier de la carte SD (liste)
struct RecEntry {
  char name[96];
  uint32_t size;
  uint32_t t;        // date de dernière modification
};

// Zone rectangulaire (tactile / affichage)
struct Zone { int x, y, w, h; };

static inline bool inZone(int x, int y, const Zone& z) {
  return x >= z.x && x < z.x + z.w && y >= z.y && y < z.y + z.h;
}


//
