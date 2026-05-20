#pragma once

#include "../state/StateTypes.h"
#include <Arduino.h>
#include <M5Unified.h>

class TextDisplay {
public:
  static constexpr int kCharsPerLine = 29;
  static constexpr int kLines = 8;
  // Body image bounding box, matching designs.md "Images" / Layout sections.
  static constexpr int kImageX = 4;
  static constexpr int kImageY = 4;
  static constexpr int kImageW = 232;
  static constexpr int kImageH = 112;

  void init();
  void setBrightness(uint8_t brightness);
  void render(const DisplayState &state);
  int pageCountForText(const String &text) const;

  // Store a 1-bit packed bitmap (MSB first) for the body image area. Pixel
  // dimensions must match kImageW x kImageH; mismatches return false.
  bool setImage(const uint8_t *packed, size_t packedLen, int width, int height);
  void clearImage();
  bool hasImage() const { return _imageBuffer != nullptr; }

private:
  static constexpr int kBodyRows = 6;
  static constexpr int kFooterRow = 7;

  mutable M5Canvas _canvas;
  bool _canvasReady = false;

  uint8_t *_imageBuffer = nullptr;
  size_t _imageBufferSize = 0;
  int _imageWidth = 0;
  int _imageHeight = 0;

  String fitLine(const String &text) const;
  String mergeEdgeText(const String &left, const String &right) const;
  String spaces(int count) const;
  int wrapBodyText(const String &text, String out[], int maxRows) const;
  void drawLine(int row, const String &text, uint16_t color) const;
  void drawGlyphAtRight(int row, char glyph, uint16_t color) const;
  void drawPageIndicator(int pageIndex, int pageCount) const;
  void drawMenu(const DisplayState &state) const;
  void drawStoredImage() const;
  void drawAlarm(const DisplayState &state) const;
  void drawBellIcon(int cx, int cy, uint16_t color) const;
};
