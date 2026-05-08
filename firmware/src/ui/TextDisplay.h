#pragma once

#include "../Config.h"
#include "../state/StateTypes.h"
#include <Arduino.h>

class TextDisplay {
public:
  static constexpr int kCellW = 16;
  static constexpr int kCellH = 32;
  static constexpr int kInsetX = 16;
  static constexpr int kInsetY = 32;
  static constexpr int kContentW = SCREEN_WIDTH_PX - 2 * kInsetX;
  static constexpr int kCharsPerLine = kContentW / kCellW;
  static constexpr int kChatRows = 11;
  static constexpr int kMenuRowH = 64;
  static constexpr int kMenuBodyY = 128;
  static constexpr int kMenuRowTextYOffset = 8;
  static constexpr int kDotY = SCREEN_HEIGHT_PX - 20;
  static constexpr int kDotSpacing = 12;
  static constexpr int kDotCountMax = 8;
  static constexpr int kImageW = 232;
  static constexpr int kImageH = 112;
  static constexpr int kImageX = (SCREEN_WIDTH_PX - kImageW) / 2;
  static constexpr int kImageY = (SCREEN_HEIGHT_PX - kImageH) / 2;

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
  static constexpr int kFooterY = SCREEN_HEIGHT_PX - kCellH;

  uint16_t *_framebuffer = nullptr;
  mutable uint16_t *_previousFramebuffer = nullptr;
  mutable bool _hasPreviousFrame = false;
  uint8_t *_imageBuffer = nullptr;
  size_t _imageBufferSize = 0;
  int _imageWidth = 0;
  int _imageHeight = 0;

  void clearFrame(uint16_t color) const;
  void flushFrame(bool forceFull = false) const;
  void putPixel(int x, int y, uint16_t color) const;
  void fillRect(int x, int y, int w, int h, uint16_t color) const;
  void drawRect(int x, int y, int w, int h, uint16_t color) const;
  void fillCircle(int cx, int cy, int radius, uint16_t color) const;
  void drawText(int x, int y, const String &text, uint16_t color,
                int maxChars = kCharsPerLine) const;
  int textPixelWidth(const String &text) const;
  String fitLine(const String &text) const;
  String mergeEdgeText(const String &left, const String &right) const;
  String spaces(int count) const;
  int wrapBodyText(const String &text, String out[], int maxRows) const;
  void drawLine(int row, const String &text, uint16_t color) const;
  void drawEdgeLine(int row, const String &left, const String &right,
                    uint16_t color) const;
  void drawPageIndicator(int pageIndex, int pageCount) const;
  void drawMenu(const DisplayState &state) const;
  void drawStoredImage() const;
};
