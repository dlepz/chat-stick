#pragma once

#include <Arduino.h>

namespace SmartBrickFont {

struct Glyph {
  uint16_t bitmapIndex;
  uint8_t boxW;
  uint8_t boxH;
  int8_t ofsX;
  int8_t ofsY;
};

constexpr int kFirstChar = 32;
constexpr int kLastChar = 126;
constexpr int kCellW = 16;
constexpr int kCellH = 32;
constexpr int kLineHeight = 30;
constexpr int kBaseline = 6;
constexpr int kGlyphCount = 96;

extern const uint8_t kBitmap[] PROGMEM;
extern const Glyph kGlyphs[] PROGMEM;

const Glyph *glyphFor(char c);
bool glyphPixelOn(const Glyph &glyph, int x, int y);

} // namespace SmartBrickFont
