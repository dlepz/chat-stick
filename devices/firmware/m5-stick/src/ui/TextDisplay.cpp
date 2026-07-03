#include "TextDisplay.h"

#include "../Config.h"
#include <M5Unified.h>
#include <math.h>
#include <string.h>

namespace {
constexpr uint16_t COLOR_BLACK = 0x0000;
constexpr uint16_t COLOR_WHITE = 0xFFFF;
constexpr uint16_t COLOR_GRAY = 0x7BEF;
constexpr uint16_t COLOR_DARK_GRAY = 0x39E7;
constexpr uint16_t COLOR_RED = 0xF800;
constexpr uint16_t COLOR_YELLOW = 0xFFE0;
constexpr uint16_t COLOR_GREEN = 0x07E0;
constexpr int LINE_HEIGHT = 16;

// Tilted bell glyph for the alarm screen — 1-bit packed, MSB-first, row-major.
// Generated from designs/bell-source.jpg at 16x16.
constexpr int kBellW = 16;
constexpr int kBellH = 16;
constexpr int kBellRowBytes = 2;
constexpr uint8_t kBellBits[] = {
    0x38, 0x00,
    0x28, 0x00,
    0x3F, 0x00,
    0x3F, 0x80,
    0x3F, 0xC0,
    0x7F, 0xC0,
    0x7F, 0xE0,
    0x7F, 0xC0,
    0x3F, 0xFC,
    0x3F, 0xC0,
    0x3F, 0x00,
    0x39, 0x80,
    0x31, 0xC0,
    0x21, 0xF0,
    0x00, 0x00,
    0x00, 0x00,
};

// Custom 8x16 glyphs grafted onto AsciiFont8x16 at unused control codepoints.
// Each row is a single byte, MSB = leftmost pixel. The glyph occupies one
// 8x16 character cell so it flows through the normal text layout.
constexpr uint8_t kGlyphTriangleDownBits[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xFE, // X X X X X X X .
    0x7C, // . X X X X X . .
    0x38, // . . X X X . . .
    0x10, // . . . X . . . .
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
constexpr uint8_t kGlyphBulletFilledBits[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x38, // . . X X X . . .
    0x7C, // . X X X X X . .
    0x7C, // . X X X X X . .
    0x7C, // . X X X X X . .
    0x38, // . . X X X . . .
    0x00, 0x00, 0x00, 0x00, 0x00,
};
constexpr uint8_t kGlyphBulletHollowBits[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x38, // . . X X X . . .
    0x44, // . X . . . X . .
    0x44, // . X . . . X . .
    0x44, // . X . . . X . .
    0x38, // . . X X X . . .
    0x00, 0x00, 0x00, 0x00, 0x00,
};
} // namespace

void TextDisplay::init() {
  M5.Display.setRotation(1);
  M5.Display.setFont(&fonts::AsciiFont8x16);
  M5.Display.setTextSize(1);
  M5.Display.fillScreen(COLOR_BLACK);
  _canvas.setColorDepth(16);
  _canvasReady = _canvas.createSprite(SCREEN_WIDTH_PX, SCREEN_HEIGHT_PX) != nullptr;
  if (_canvasReady) {
    _previousCanvas = static_cast<uint16_t *>(ps_malloc(kCanvasBytes));
    if (!_previousCanvas) {
      _previousCanvas = static_cast<uint16_t *>(malloc(kCanvasBytes));
    }
    if (!_previousCanvas) {
      Serial.println("[Display] Dirty buffer unavailable; full canvas flush");
    }
  }
  _eyeCanvas.setColorDepth(16);
  _eyeCanvasReady =
      _eyeCanvas.createSprite(kEyeCanvasW, kEyeCanvasH) != nullptr;
}

void TextDisplay::setBrightness(uint8_t brightness) {
  M5.Display.setBrightness(brightness);
  if (brightness > 0) {
    _hasPreviousCanvas = false;
  }
}

void TextDisplay::render(const DisplayState &state) {
  if (_canvasReady) {
    _canvas.fillScreen(COLOR_BLACK);
    _canvas.setFont(&fonts::AsciiFont8x16);
    _canvas.setTextSize(1);
  } else {
    M5.Display.fillScreen(COLOR_BLACK);
    M5.Display.setFont(&fonts::AsciiFont8x16);
    M5.Display.setTextSize(1);
  }

  if (state.alarmActive) {
    drawAlarm(state);
    if (_canvasReady) {
      flushCanvas();
    }
    return;
  }

  if (state.showReactiveFace) {
    drawExpressiveEyesScene(state);
    drawTurnFeedback(state.turnFeedbackColor);
    if (_canvasReady) {
      flushCanvas();
    }
    if (_blinkPending) {
      runSmoothBlink();
    }
    return;
  }

  if (state.showScene) {
    drawScene(state);
    if (_canvasReady) {
      flushCanvas();
    }
    return;
  }

  const bool hasHeader = !state.headerLeft.isEmpty() || !state.headerRight.isEmpty();
  const bool hasFooterText =
      !state.footerLeft.isEmpty() || !state.footerRight.isEmpty();
  if (hasHeader) {
    drawLine(0, mergeEdgeText(state.headerLeft, state.headerRight), COLOR_GRAY);
  }
  drawTurnFeedback(state.turnFeedbackColor);

  if (state.showMenu) {
    drawMenu(state);
  } else {
    const int bodyStart = hasHeader ? 1 : 0;
    const int bodyEnd = kLines - 1;
    const int bodyRows = bodyEnd - bodyStart;
    const bool imagePage = state.imagePresent && hasImage();
    String wrapped[32];
    const int wrappedCount = wrapBodyText(state.bodyText, wrapped, 32);
    const int textPageCount = max(1, (wrappedCount + bodyRows - 1) / bodyRows);
    const int totalPages =
        (imagePage ? 1 : 0) + (state.bodyText.isEmpty() ? 0 : textPageCount);
    const int pageCount = max(1, totalPages);
    const int safePageIndex =
        constrain(state.pageIndex, 0, max(0, pageCount - 1));
    const uint16_t bodyColor = state.bodyDim ? COLOR_GRAY : COLOR_WHITE;

    if (imagePage && safePageIndex == 0) {
      drawStoredImage();
    } else {
      const int textPageIndex = imagePage ? safePageIndex - 1 : safePageIndex;
      for (int i = 0; i < bodyRows; i++) {
        const int lineIndex = textPageIndex * bodyRows + i;
        drawLine(bodyStart + i,
                 lineIndex < wrappedCount ? wrapped[lineIndex] : "", bodyColor);
      }
    }
    if (pageCount > 1) {
      drawPageIndicator(safePageIndex, pageCount);
    }
  }

  if (hasFooterText) {
    drawLine(kFooterRow, mergeEdgeText(state.footerLeft, state.footerRight),
             COLOR_GRAY);
  }

  if (state.showRecordingProgress) {
    drawRecordingProgress(state.recordingProgress);
  }

  if (_canvasReady) {
    flushCanvas();
  }
}

bool TextDisplay::setImage(const uint8_t *packed, size_t packedLen, int width,
                           int height) {
  if (!packed || width != kImageW || height != kImageH) {
    Serial.printf("[Display] Image rejected: %dx%d (expected %dx%d)\n", width,
                  height, kImageW, kImageH);
    return false;
  }
  const size_t expectedBytes = static_cast<size_t>((width * height + 7) / 8);
  if (packedLen < expectedBytes) {
    Serial.printf("[Display] Image too short: %u bytes (expected %u)\n",
                  static_cast<unsigned>(packedLen),
                  static_cast<unsigned>(expectedBytes));
    return false;
  }

  if (_imageBufferSize < expectedBytes) {
    if (_imageBuffer) free(_imageBuffer);
    _imageBuffer = static_cast<uint8_t *>(malloc(expectedBytes));
    if (!_imageBuffer) {
      _imageBufferSize = 0;
      Serial.println("[Display] Failed to allocate image buffer");
      return false;
    }
    _imageBufferSize = expectedBytes;
  }
  memcpy(_imageBuffer, packed, expectedBytes);
  _imageWidth = width;
  _imageHeight = height;
  return true;
}

void TextDisplay::clearImage() {
  if (_imageBuffer) {
    free(_imageBuffer);
    _imageBuffer = nullptr;
  }
  _imageBufferSize = 0;
  _imageWidth = 0;
  _imageHeight = 0;
}

void TextDisplay::drawStoredImage() const {
  if (!_imageBuffer) return;
  const int w = _imageWidth;
  const int h = _imageHeight;
  for (int y = 0; y < h; y++) {
    const int rowStart = y * w;
    for (int x = 0; x < w; x++) {
      const int bit = rowStart + x;
      const uint8_t byte = _imageBuffer[bit >> 3];
      const bool on = (byte >> (7 - (bit & 7))) & 1;
      if (!on) continue; // background already black
      if (_canvasReady) {
        _canvas.drawPixel(kImageX + x, kImageY + y, COLOR_WHITE);
      } else {
        M5.Display.drawPixel(kImageX + x, kImageY + y, COLOR_WHITE);
      }
    }
  }
}

int TextDisplay::pageCountForText(const String &text) const {
  String wrapped[32];
  const int wrappedCount = wrapBodyText(text, wrapped, 32);
  return max(1, (wrappedCount + kBodyRows - 1) / kBodyRows);
}

String TextDisplay::layoutTextForReveal(const String &text) const {
  String wrapped[32];
  const int wrappedCount = wrapBodyText(text, wrapped, 32);
  String out;

  for (int i = 0; i < wrappedCount; i++) {
    if (i > 0) {
      out += '\n';
    }
    out += wrapped[i];
  }

  return out;
}

int TextDisplay::wrappedRowCount(const String &text) const {
  if (text.isEmpty()) {
    return 0;
  }
  String wrapped[32];
  return wrapBodyText(text, wrapped, 32);
}

String TextDisplay::fitLine(const String &text) const {
  String out;
  out.reserve(kCharsPerLine);

  for (int i = 0; i < static_cast<int>(text.length()) && out.length() < kCharsPerLine;
       i++) {
    const char c = text[i];
    if ((c >= 32 && c <= 126) || c == kGlyphTriangleDown ||
        c == kGlyphBulletFilled || c == kGlyphBulletHollow) {
      out += c;
    } else {
      out += ' ';
    }
  }

  return out;
}

String TextDisplay::mergeEdgeText(const String &left, const String &right) const {
  const String safeLeft = fitLine(left);
  const String safeRight = fitLine(right);

  if (safeLeft.isEmpty()) return safeRight;
  if (safeRight.isEmpty()) return safeLeft;

  if (safeLeft.length() + safeRight.length() >= kCharsPerLine) {
    const int reservedForRight = safeRight.length() + 1;
    const int leftBudget = max(0, kCharsPerLine - reservedForRight);
    return fitLine(safeLeft.substring(0, leftBudget)) + " " + safeRight;
  }

  const int spaces = kCharsPerLine - safeLeft.length() - safeRight.length();
  return safeLeft + this->spaces(spaces) + safeRight;
}

String TextDisplay::spaces(int count) const {
  String out;
  for (int i = 0; i < count; i++) {
    out += ' ';
  }
  return out;
}

int TextDisplay::wrapBodyText(const String &text, String out[], int maxRows) const {
  for (int i = 0; i < maxRows; i++) {
    out[i] = "";
  }

  int row = 0;
  String line;
  String word;

  auto flushLine = [&]() {
    if (row >= maxRows) {
      return;
    }
    out[row++] = fitLine(line);
    line = "";
  };

  auto appendWord = [&](const String &token) {
    if (token.isEmpty()) {
      return;
    }

    if (line.isEmpty()) {
      if (token.length() <= kCharsPerLine) {
        line = token;
        return;
      }

      int start = 0;
      while (start < token.length() && row < maxRows) {
        out[row++] = fitLine(token.substring(start, start + kCharsPerLine));
        start += kCharsPerLine;
      }
      line = "";
      return;
    }

    const String candidate = line + " " + token;
    if (candidate.length() <= kCharsPerLine) {
      line = candidate;
      return;
    }

    flushLine();
    if (row >= maxRows) {
      return;
    }

    if (token.length() <= kCharsPerLine) {
      line = token;
      return;
    }

    int start = 0;
    while (start < token.length() && row < maxRows) {
      out[row++] = fitLine(token.substring(start, start + kCharsPerLine));
      start += kCharsPerLine;
    }
    line = "";
  };

  for (int i = 0; i <= static_cast<int>(text.length()) && row < maxRows; i++) {
    const char c = i < static_cast<int>(text.length()) ? text[i] : '\n';
    if (c == '\n') {
      appendWord(word);
      word = "";
      flushLine();
      continue;
    }

    if (c == ' ') {
      appendWord(word);
      word = "";
      continue;
    }

    if ((c >= 32 && c <= 126) || c == kGlyphTriangleDown ||
        c == kGlyphBulletFilled || c == kGlyphBulletHollow) {
      word += c;
    }
  }

  if (row < maxRows) {
    appendWord(word);
    if (!line.isEmpty() && row < maxRows) {
      out[row] = fitLine(line);
      row++;
    }
  }

  return max(1, row);
}

void TextDisplay::flushCanvas(bool forceFull) {
  if (!_canvasReady) {
    return;
  }

  uint16_t *current = static_cast<uint16_t *>(_canvas.getBuffer());
  if (!current) {
    return;
  }

  if (forceFull || !_previousCanvas || !_hasPreviousCanvas) {
    _canvas.pushSprite(&M5.Display, 0, 0);
    if (_previousCanvas) {
      memcpy(_previousCanvas, current, kCanvasBytes);
      _hasPreviousCanvas = true;
    }
    return;
  }

  constexpr size_t kRowBytes = SCREEN_WIDTH_PX * sizeof(uint16_t);
  int minY = SCREEN_HEIGHT_PX;
  int maxY = -1;

  for (int y = 0; y < SCREEN_HEIGHT_PX; y++) {
    const size_t offset = static_cast<size_t>(y) * SCREEN_WIDTH_PX;
    if (memcmp(current + offset, _previousCanvas + offset, kRowBytes) == 0) {
      continue;
    }
    minY = min(minY, y);
    maxY = max(maxY, y);
  }

  if (maxY < minY) {
    return;
  }

  int minX = SCREEN_WIDTH_PX;
  int maxX = -1;
  for (int y = minY; y <= maxY; y++) {
    const size_t offset = static_cast<size_t>(y) * SCREEN_WIDTH_PX;
    const uint16_t *row = current + offset;
    const uint16_t *prev = _previousCanvas + offset;
    if (memcmp(row, prev, kRowBytes) == 0) {
      continue;
    }

    int left = 0;
    while (left < SCREEN_WIDTH_PX && row[left] == prev[left]) {
      left++;
    }

    int right = SCREEN_WIDTH_PX - 1;
    while (right >= left && row[right] == prev[right]) {
      right--;
    }

    minX = min(minX, left);
    maxX = max(maxX, right);
  }

  if (maxX < minX) {
    return;
  }

  const int dirtyW = maxX - minX + 1;
  const int dirtyH = maxY - minY + 1;
  const int dirtyPixels = dirtyW * dirtyH;
  const int screenPixels = SCREEN_WIDTH_PX * SCREEN_HEIGHT_PX;

  if (dirtyPixels > screenPixels / 3) {
    _canvas.pushSprite(&M5.Display, 0, 0);
    memcpy(_previousCanvas, current, kCanvasBytes);
    _hasPreviousCanvas = true;
    return;
  }

  for (int y = minY; y <= maxY; y++) {
    const uint16_t *row =
        current + static_cast<size_t>(y) * SCREEN_WIDTH_PX + minX;
    M5.Display.pushImage(minX, y, dirtyW, 1, row);
  }

  for (int y = minY; y <= maxY; y++) {
    const size_t offset = static_cast<size_t>(y) * SCREEN_WIDTH_PX + minX;
    memcpy(_previousCanvas + offset, current + offset,
           dirtyW * sizeof(uint16_t));
  }
}

void TextDisplay::drawLine(int row, const String &text, uint16_t color) const {
  const String fitted = fitLine(text);
  const int yTop = row * LINE_HEIGHT;
  int x = 4;
  for (int i = 0; i < static_cast<int>(fitted.length()); i++) {
    drawCharCell(x, yTop, fitted[i], color);
    x += 8;
  }
}

void TextDisplay::drawCharCell(int x, int yTop, char c, uint16_t color) const {
  if (c == kGlyphTriangleDown) {
    drawBitmapGlyph(x, yTop, kGlyphTriangleDownBits, color);
    return;
  }
  if (c == kGlyphBulletFilled) {
    drawBitmapGlyph(x, yTop, kGlyphBulletFilledBits, color);
    return;
  }
  if (c == kGlyphBulletHollow) {
    drawBitmapGlyph(x, yTop, kGlyphBulletHollowBits, color);
    return;
  }
  if (_canvasReady) {
    _canvas.setTextColor(color);
    _canvas.setCursor(x, yTop);
    _canvas.print(c);
    return;
  }
  M5.Display.setTextColor(color);
  M5.Display.setCursor(x, yTop);
  M5.Display.print(c);
}

void TextDisplay::drawBitmapGlyph(int x, int yTop, const uint8_t *bits,
                                  uint16_t color) const {
  for (int row = 0; row < LINE_HEIGHT; row++) {
    const uint8_t byte = bits[row];
    if (byte == 0) continue;
    for (int col = 0; col < 8; col++) {
      if (!((byte >> (7 - col)) & 1)) continue;
      if (_canvasReady) {
        _canvas.drawPixel(x + col, yTop + row, color);
      } else {
        M5.Display.drawPixel(x + col, yTop + row, color);
      }
    }
  }
}

void TextDisplay::drawGlyphAtRight(int row, char glyph, uint16_t color) const {
  const int x = 4 + (kCharsPerLine - 1) * 8;
  const int y = row * LINE_HEIGHT;
  drawCharCell(x, y, glyph, color);
}

void TextDisplay::drawRecordingProgress(float progress) const {
  const int barWidth = 8;
  const int margin = 2;
  const int x = SCREEN_WIDTH_PX - barWidth - margin;
  const int y = margin;
  const int height = SCREEN_HEIGHT_PX - (margin * 2);
  const int clampedHeight =
      constrain(static_cast<int>(height * constrain(progress, 0.0f, 1.0f)), 0,
                height);

  if (_canvasReady) {
    _canvas.drawRect(x, y, barWidth, height, COLOR_GRAY);
  } else {
    M5.Display.drawRect(x, y, barWidth, height, COLOR_GRAY);
  }
  if (clampedHeight <= 2) {
    return;
  }
  if (_canvasReady) {
    _canvas.fillRect(x + 1, y + height - clampedHeight + 1, barWidth - 2,
                     clampedHeight - 2, COLOR_RED);
  } else {
    M5.Display.fillRect(x + 1, y + height - clampedHeight + 1, barWidth - 2,
                        clampedHeight - 2, COLOR_RED);
  }
}

void TextDisplay::drawTurnFeedback(const String &colorName) const {
  if (colorName.isEmpty()) {
    return;
  }

  uint16_t color = COLOR_DARK_GRAY;
  if (colorName == "green") {
    color = COLOR_GREEN;
  } else if (colorName == "yellow") {
    color = COLOR_YELLOW;
  } else if (colorName == "red") {
    color = COLOR_RED;
  } else if (colorName == "gray") {
    color = COLOR_GRAY;
  }

  const int size = 10;
  const int x = SCREEN_WIDTH_PX - size - 2;
  const int y = 3;
  if (_canvasReady) {
    _canvas.fillRect(x, y, size, size, color);
    _canvas.drawRect(x, y, size, size, COLOR_WHITE);
    return;
  }
  M5.Display.fillRect(x, y, size, size, color);
  M5.Display.drawRect(x, y, size, size, COLOR_WHITE);
}

void TextDisplay::drawScene(const DisplayState &state) const {
  if (state.sceneKind == 1) {
    drawGermanFlagScene(state);
    return;
  }
  drawLittleGuyScene(state);
}

void TextDisplay::drawLittleGuyScene(const DisplayState &state) const {
  constexpr uint16_t COLOR_SKY = 0x867D;
  constexpr uint16_t COLOR_CLOUD = 0xF7BE;
  constexpr uint16_t COLOR_GRASS = 0x55E6;
  constexpr uint16_t COLOR_DARK_GRASS = 0x2C84;
  constexpr uint16_t COLOR_SKIN = 0xFEA0;
  constexpr uint16_t COLOR_HAIR = 0x7B40;
  constexpr uint16_t COLOR_SHIRT = 0x347F;
  constexpr uint16_t COLOR_PANTS = 0x1A8B;
  constexpr uint16_t COLOR_DOG = 0xCBE8;
  constexpr uint16_t COLOR_DOG_DARK = 0x8243;
  constexpr uint16_t COLOR_FLOWER = 0xF81F;

  auto fillRectPx = [&](int x, int y, int w, int h, uint16_t color) {
    if (_canvasReady) {
      _canvas.fillRect(x, y, w, h, color);
    } else {
      M5.Display.fillRect(x, y, w, h, color);
    }
  };
  auto drawLinePx = [&](int x0, int y0, int x1, int y1, uint16_t color) {
    if (_canvasReady) {
      _canvas.drawLine(x0, y0, x1, y1, color);
    } else {
      M5.Display.drawLine(x0, y0, x1, y1, color);
    }
  };
  auto fillTrianglePx = [&](int x0, int y0, int x1, int y1, int x2, int y2,
                            uint16_t color) {
    if (_canvasReady) {
      _canvas.fillTriangle(x0, y0, x1, y1, x2, y2, color);
    } else {
      M5.Display.fillTriangle(x0, y0, x1, y1, x2, y2, color);
    }
  };
  auto drawTextPx = [&](int x, int y, const String &text, uint16_t color) {
    if (_canvasReady) {
      _canvas.setFont(&fonts::AsciiFont8x16);
      _canvas.setTextSize(1);
      _canvas.setTextColor(color);
      _canvas.setCursor(x, y);
      _canvas.print(text);
    } else {
      M5.Display.setFont(&fonts::AsciiFont8x16);
      M5.Display.setTextSize(1);
      M5.Display.setTextColor(color);
      M5.Display.setCursor(x, y);
      M5.Display.print(text);
    }
  };

  const int frame = state.sceneFrame;
  const int wiggle = (frame / 3) % 2;
  const int cloudTravel = SCREEN_WIDTH_PX + 90;
  const int cloudA = (frame * 4) % cloudTravel - 70;
  const int cloudB = (frame * 3 + 150) % cloudTravel - 70;
  const int cloudC = (frame * 2 + 280) % cloudTravel - 70;

  fillRectPx(0, 0, SCREEN_WIDTH_PX, SCREEN_HEIGHT_PX, COLOR_SKY);
  fillRectPx(0, 82, SCREEN_WIDTH_PX, SCREEN_HEIGHT_PX - 82, COLOR_GRASS);
  fillTrianglePx(0, 74, SCREEN_WIDTH_PX, 62, SCREEN_WIDTH_PX, 92,
                 COLOR_GRASS);
  fillTrianglePx(0, 96, 84, 80, 0, SCREEN_HEIGHT_PX, COLOR_DARK_GRASS);

  auto drawCloud = [&](int x, int y) {
    fillRectPx(x + 8, y + 10, 36, 10, COLOR_CLOUD);
    fillRectPx(x + 14, y + 4, 14, 12, COLOR_CLOUD);
    fillRectPx(x + 26, y, 16, 16, COLOR_CLOUD);
    fillRectPx(x + 42, y + 8, 14, 12, COLOR_CLOUD);
  };
  drawCloud(cloudA, 20);
  drawCloud(cloudB, 34);
  drawCloud(cloudC, 12);

  const int px = 64;
  const int py = 42;
  fillRectPx(px + 14, py + 0, 20, 8, COLOR_HAIR);
  fillRectPx(px + 10, py + 8, 28, 24, COLOR_SKIN);
  fillRectPx(px + 10, py + 8, 6, 10, COLOR_HAIR);
  fillRectPx(px + 18, py + 17, 3, 3, COLOR_BLACK);
  fillRectPx(px + 29, py + 17, 3, 3, COLOR_BLACK);
  fillRectPx(px + 22, py + 26, 8, 2, COLOR_DOG_DARK);
  fillRectPx(px + 12, py + 32, 24, 30, COLOR_SHIRT);
  fillRectPx(px + 5, py + 37, 8, 22, COLOR_SKIN);
  if (wiggle) {
    fillRectPx(px + 36, py + 25, 8, 26, COLOR_SKIN);
    fillRectPx(px + 42, py + 21, 10, 8, COLOR_SKIN);
  } else {
    fillRectPx(px + 36, py + 37, 8, 22, COLOR_SKIN);
  }
  fillRectPx(px + 14, py + 62, 8, 28, COLOR_PANTS);
  fillRectPx(px + 27, py + 62, 8, 28, COLOR_PANTS);
  fillRectPx(px + 10, py + 88, 14, 5, COLOR_DOG_DARK);
  fillRectPx(px + 25, py + 88, 14, 5, COLOR_DOG_DARK);

  const int dx = 124;
  const int dy = 86;
  fillRectPx(dx + 8, dy + 8, 34, 16, COLOR_DOG);
  fillRectPx(dx + 38, dy + 2, 16, 16, COLOR_DOG);
  fillRectPx(dx + 40, dy, 6, 6, COLOR_DOG_DARK);
  fillRectPx(dx + 50, dy + 7, 3, 3, COLOR_BLACK);
  fillRectPx(dx + 53, dy + 11, 5, 3, COLOR_DOG_DARK);
  fillRectPx(dx + 12, dy + 23, 5, 12, COLOR_DOG_DARK);
  fillRectPx(dx + 32, dy + 23, 5, 12, COLOR_DOG_DARK);
  if (wiggle) {
    drawLinePx(dx + 8, dy + 11, dx, dy + 3, COLOR_DOG_DARK);
    drawLinePx(dx + 7, dy + 12, dx - 1, dy + 4, COLOR_DOG_DARK);
  } else {
    drawLinePx(dx + 8, dy + 12, dx, dy + 19, COLOR_DOG_DARK);
    drawLinePx(dx + 7, dy + 13, dx - 1, dy + 20, COLOR_DOG_DARK);
  }

  fillRectPx(30, 111, 3, 10, COLOR_DARK_GRASS);
  fillRectPx(26, 107, 10, 6, COLOR_FLOWER);
  fillRectPx(204, 104, 3, 10, COLOR_DARK_GRASS);
  fillRectPx(200, 100, 10, 6, COLOR_WHITE);
  fillRectPx(214, 119, 18, 3, COLOR_DARK_GRASS);
  fillRectPx(12, 125, 24, 3, COLOR_DARK_GRASS);

  String prompt;
  switch (state.appState) {
  case AppState::Connecting:
    prompt = "Connecting...";
    break;
  case AppState::Recording:
    prompt = "Listening...";
    break;
  case AppState::Thinking:
    prompt = "Thinking...";
    break;
  case AppState::Playing:
    prompt = "Speaking...";
    break;
  case AppState::Ready:
    prompt = ((frame / 12) % 2) == 0 ? "Tap A start" : "Hold A talk";
    break;
  case AppState::ConfirmReset:
    prompt = "Reset?";
    break;
  case AppState::Alarm:
    prompt = "Alarm";
    break;
  case AppState::Error:
  default:
    prompt = "Offline";
    break;
  }

  drawTextPx(6, 4, "Quiz Masters", COLOR_DARK_GRASS);
  drawTextPx(6, 116, prompt, COLOR_DARK_GRASS);
  drawTextPx(174, 116, "B back", COLOR_DARK_GRASS);
}

void TextDisplay::drawGermanFlagScene(const DisplayState &state) const {
  constexpr uint16_t COLOR_GOLD = 0xFFE0;
  constexpr uint16_t COLOR_DARK_RED = 0x9000;
  constexpr uint16_t COLOR_DARK_GOLD = 0xAD20;
  constexpr uint16_t COLOR_BG = 0x18E3;
  constexpr uint16_t COLOR_GRID = 0x39E7;
  constexpr uint16_t COLOR_FLAG_GRAY = 0x8410;

  auto fillRectPx = [&](int x, int y, int w, int h, uint16_t color) {
    if (_canvasReady) {
      _canvas.fillRect(x, y, w, h, color);
    } else {
      M5.Display.fillRect(x, y, w, h, color);
    }
  };
  auto drawLinePx = [&](int x0, int y0, int x1, int y1, uint16_t color) {
    if (_canvasReady) {
      _canvas.drawLine(x0, y0, x1, y1, color);
    } else {
      M5.Display.drawLine(x0, y0, x1, y1, color);
    }
  };
  auto drawTextPx = [&](int x, int y, const String &text, uint16_t color) {
    if (_canvasReady) {
      _canvas.setFont(&fonts::AsciiFont8x16);
      _canvas.setTextSize(1);
      _canvas.setTextColor(color);
      _canvas.setCursor(x, y);
      _canvas.print(text);
    } else {
      M5.Display.setFont(&fonts::AsciiFont8x16);
      M5.Display.setTextSize(1);
      M5.Display.setTextColor(color);
      M5.Display.setCursor(x, y);
      M5.Display.print(text);
    }
  };

  fillRectPx(0, 0, SCREEN_WIDTH_PX, SCREEN_HEIGHT_PX, COLOR_BG);
  for (int x = 0; x < SCREEN_WIDTH_PX; x += 12) {
    drawLinePx(x, 0, x, SCREEN_HEIGHT_PX, COLOR_GRID);
  }
  for (int y = 0; y < SCREEN_HEIGHT_PX; y += 12) {
    drawLinePx(0, y, SCREEN_WIDTH_PX, y, COLOR_GRID);
  }

  const int frame = state.sceneFrame;
  const int wave = (frame / 5) % 3;
  const int flagX = 34;
  const int flagY = 28;
  const int cell = 8;
  const int cols = 20;
  const int rowsPerStripe = 3;

  fillRectPx(flagX - 10, flagY - 10, 5, 96, COLOR_FLAG_GRAY);
  fillRectPx(flagX - 18, flagY + 84, 24, 6, COLOR_FLAG_GRAY);
  fillRectPx(flagX - 14, flagY - 15, 13, 8, COLOR_GOLD);

  for (int row = 0; row < rowsPerStripe * 3; row++) {
    const uint16_t stripe = row < rowsPerStripe
                                ? COLOR_BLACK
                                : (row < rowsPerStripe * 2 ? COLOR_RED
                                                           : COLOR_GOLD);
    const uint16_t shade = row < rowsPerStripe
                               ? COLOR_BLACK
                               : (row < rowsPerStripe * 2 ? COLOR_DARK_RED
                                                          : COLOR_DARK_GOLD);
    for (int col = 0; col < cols; col++) {
      const int offset = ((col + wave) % 4 == 0) ? 2 : 0;
      const int x = flagX + col * cell;
      const int y = flagY + row * cell + offset;
      fillRectPx(x, y, cell, cell, stripe);
      fillRectPx(x, y + cell - 2, cell, 2, shade);
    }
  }

  if ((frame / 10) % 2 == 0) {
    fillRectPx(202, 18, 6, 6, COLOR_GOLD);
    fillRectPx(214, 34, 4, 4, COLOR_WHITE);
    fillRectPx(20, 100, 4, 4, COLOR_RED);
  } else {
    fillRectPx(210, 24, 4, 4, COLOR_GOLD);
    fillRectPx(198, 42, 6, 6, COLOR_WHITE);
    fillRectPx(24, 94, 6, 6, COLOR_RED);
  }

  drawTextPx(6, 4, "Deutsch", COLOR_WHITE);
  drawTextPx(6, 116, "German flag", COLOR_WHITE);
  drawTextPx(174, 116, "B back", COLOR_WHITE);
}

void TextDisplay::runSmoothBlink() const {
  constexpr int kBlinkFrames = 20;
  constexpr uint16_t kFrameDelayMs = 4;
  constexpr uint16_t kIdleMinMs = 1500;
  constexpr uint16_t kIdleMaxMs = 4500;
  constexpr uint16_t kDoubleBlinkGapMs = 140;
  constexpr int kHalfFrames = kBlinkFrames / 2;

  if (!_eyeCanvasReady || _blinkClipW <= 0 || _blinkClipH <= 0) {
    _blinkPending = false;
    _nextBlinkAtMs = millis() + random(kIdleMinMs, kIdleMaxMs);
    return;
  }

  const int localCxL = _blinkLeftCx - _blinkClipX;
  const int localCxR = _blinkRightCx - _blinkClipX;
  const int localCy = _blinkCy - _blinkClipY;

  for (int blinkState = 0; blinkState < kBlinkFrames; blinkState++) {
    float p;
    float openness;
    if (blinkState < kHalfFrames) {
      p = static_cast<float>(blinkState) / static_cast<float>(kHalfFrames);
      openness = 1.0f - p * p;
    } else {
      p = static_cast<float>(blinkState - kHalfFrames) /
          static_cast<float>(kHalfFrames);
      openness = p * (2.0f - p);
    }
    const int ry = max(1, static_cast<int>(_blinkRyMax * openness));
    _eyeCanvas.fillScreen(COLOR_BLACK);
    _eyeCanvas.fillEllipse(localCxL, localCy, _blinkRxL, ry, _blinkColor);
    _eyeCanvas.fillEllipse(localCxR, localCy, _blinkRxR, ry, _blinkColor);
    _eyeCanvas.pushSprite(&M5.Display, _blinkClipX, _blinkClipY);
    delay(kFrameDelayMs);
  }

  _blinkPending = false;
  const uint32_t endMs = millis();
  if (_blinkDoubleQueued) {
    _blinkDoubleQueued = false;
    _nextBlinkAtMs = endMs + kDoubleBlinkGapMs;
  } else {
    _nextBlinkAtMs = endMs + random(kIdleMinMs, kIdleMaxMs);
    if (random(10) == 0) {
      _blinkDoubleQueued = true;
    }
  }
}

void TextDisplay::drawExpressiveEyesScene(const DisplayState &state) const {
  const int frame = static_cast<int>(
      state.sceneFrame * constrain(state.faceAnimSpeed, 0.25f, 3.0f));
  const int cycle = frame % 150;
  const float rawVoiceLevel = constrain(state.voiceLevel, 0.0f, 1.0f);
  const float lookX = constrain(state.eyeLookX, -1.0f, 1.0f);
  const float lookY = constrain(state.eyeLookY, -1.0f, 1.0f);

  auto fillRectPx = [&](int x, int y, int w, int h, uint16_t color) {
    if (_canvasReady) {
      _canvas.fillRect(x, y, w, h, color);
    } else {
      M5.Display.fillRect(x, y, w, h, color);
    }
  };
  auto fillEllipsePx = [&](int x, int y, int radiusX, int radiusY,
                           uint16_t color) {
    if (radiusX <= 0 || radiusY <= 0) return;
    if (_canvasReady) {
      _canvas.fillEllipse(x, y, radiusX, radiusY, color);
    } else {
      M5.Display.fillEllipse(x, y, radiusX, radiusY, color);
    }
  };
  auto fillTrianglePx = [&](int x0, int y0, int x1, int y1, int x2, int y2,
                            uint16_t color) {
    if (_canvasReady) {
      _canvas.fillTriangle(x0, y0, x1, y1, x2, y2, color);
    } else {
      M5.Display.fillTriangle(x0, y0, x1, y1, x2, y2, color);
    }
  };

  fillRectPx(0, 0, SCREEN_WIDTH_PX, SCREEN_HEIGHT_PX, COLOR_BLACK);

  const float targetAngry = state.faceEmotion == 1 ? 1.0f : 0.0f;
  const float angryForce = (targetAngry - _faceAngryBlend) * 0.04f;
  _faceAngryVel = (_faceAngryVel + angryForce) * 0.93f;
  _faceAngryBlend += _faceAngryVel;

  const float targetEepy = state.faceEmotion == 2 ? 1.0f : 0.0f;
  const float eepyForce = (targetEepy - _faceEepyBlend) * 0.085f;
  _faceEepyVel = (_faceEepyVel + eepyForce) * 0.89f;
  _faceEepyBlend += _faceEepyVel;

  if (rawVoiceLevel > 0.01f) {
    _faceSilenceFrames = 0;
  } else {
    _faceSilenceFrames = min(1000, _faceSilenceFrames + 1);
  }

  const float angryBlend = constrain(_faceAngryBlend, 0.0f, 1.0f);
  const float eepyBlend = constrain(_faceEepyBlend, 0.0f, 1.0f);
  const float baseRx = 24.0f + (10.0f * angryBlend) + (4.0f * eepyBlend);
  const float baseRy = 45.0f - (7.0f * angryBlend) - (40.0f * eepyBlend);

  float targetXOffset = 0.0f;
  float targetYOffset = 0.0f;
  float targetRx = baseRx;
  float targetRy = baseRy;
  float targetBaseYOffset = sinf(frame / 8.0f) * (2.0f * (1.0f - eepyBlend));

  if (fabsf(lookX) > 0.05f || fabsf(lookY) > 0.05f) {
    targetXOffset = lookX * 22.0f;
    targetYOffset = lookY * 15.0f;
  } else if (rawVoiceLevel > 0.0f) {
    targetXOffset = sinf(frame / 6.0f) * 14.0f * rawVoiceLevel;
    targetYOffset = cosf(frame / 9.0f) * 8.0f * rawVoiceLevel;
  } else if (cycle > 120 && cycle <= 140 && _faceSilenceFrames > 60) {
    float t = 0.0f;
    if (cycle <= 125) t = (cycle - 120) / 5.0f;
    else if (cycle <= 135) t = 1.0f;
    else t = 1.0f - ((cycle - 135) / 5.0f);
    const float ease = t < 0.5f ? 2.0f * t * t
                                 : -1.0f + (4.0f - 2.0f * t) * t;
    targetXOffset = -15.0f * ease;
  }

  if (rawVoiceLevel > 0.7f) {
    targetRx = baseRx + 4.0f;
    targetRy = baseRy + 3.0f;
    targetBaseYOffset -= 6.0f;
  } else if (rawVoiceLevel > 0.2f) {
    targetRx = baseRx + 1.0f;
    targetRy = baseRy + 1.0f;
    targetBaseYOffset -= 2.0f;
  } else if (rawVoiceLevel > 0.0f || _faceSilenceFrames < 40) {
    float intensity = rawVoiceLevel <= 0.0f
                          ? 1.0f - (_faceSilenceFrames / 40.0f)
                          : 1.0f;
    intensity = constrain(intensity, 0.0f, 1.0f);
    targetRx = baseRx + (4.0f * intensity);
    targetRy = baseRy - (baseRy * 0.6f * intensity);
    targetBaseYOffset += (3.0f * intensity);
  } else if (cycle > 60 && cycle <= 110 && eepyBlend < 0.5f) {
    float t = 0.0f;
    if (cycle <= 70) t = (cycle - 60) / 10.0f;
    else if (cycle <= 100) t = 1.0f;
    else t = 1.0f - ((cycle - 100) / 10.0f);
    const float ease = t < 0.5f ? 2.0f * t * t
                                 : -1.0f + (4.0f - 2.0f * t) * t;
    targetRx += (16.0f * ease * (1.0f - angryBlend));
    targetRy -= (32.0f * ease * (1.0f - angryBlend));
    targetYOffset += (5.0f * ease);
    targetBaseYOffset -= (2.0f * ease);
  }

  if (eepyBlend > 0.01f) {
    if (rawVoiceLevel > 0.0f) {
      targetRx = baseRx + (2.0f * rawVoiceLevel * (1.0f - eepyBlend));
      targetRy = baseRy + (2.0f * rawVoiceLevel * (1.0f - eepyBlend));
    }
    targetBaseYOffset += (15.0f * eepyBlend);
  }

  _faceEyeX += (targetXOffset - _faceEyeX) * 0.05f;
  _faceEyeY += (targetYOffset - _faceEyeY) * 0.05f;
  _faceRx += (targetRx - _faceRx) * 0.075f;
  _faceRy += (targetRy - _faceRy) * 0.075f;
  _faceBaseYOffset += (targetBaseYOffset - _faceBaseYOffset) * 0.05f;

  {
    constexpr uint16_t kIdleMinMs = 1500;
    constexpr uint16_t kIdleMaxMs = 4500;
    const uint32_t now = millis();
    const bool blinkAllowed = rawVoiceLevel <= 0.0f && _faceSilenceFrames > 40 &&
                              eepyBlend < 0.1f && angryBlend < 0.1f;
    if (!_blinkScheduleSeeded) {
      _nextBlinkAtMs = now + random(kIdleMinMs, kIdleMaxMs);
      _blinkScheduleSeeded = true;
    }
    if (!_blinkPending) {
      if (blinkAllowed && now >= _nextBlinkAtMs) {
        _blinkPending = true;
      } else if (!blinkAllowed) {
        _nextBlinkAtMs = now + random(kIdleMinMs, kIdleMaxMs);
      }
    }
  }

  const float safeRxF = max(0.1f, _faceRx);
  const float safeRyF = max(0.1f, _faceRy);
  const uint8_t gb = static_cast<uint8_t>(255.0f * (1.0f - angryBlend));
  const uint16_t eyeColor = M5.Display.color565(255, gb, gb);
  const float perspective = constrain(state.facePerspective, 0.0f, 3.0f);
  const float yaw = (_faceEyeX / 22.0f) * 0.35f * perspective;
  const float pitch = (_faceEyeY / 15.0f) * 0.25f * perspective;
  const float cyaw = cosf(yaw);
  const float syaw = sinf(yaw);
  const float cpitch = cosf(pitch);
  const float spitch = sinf(pitch);
  struct ProjectedPoint {
    int x;
    int y;
    float scale;
  };
  auto project3D = [&](float x, float y, float z) -> ProjectedPoint {
    const float py = y + _faceBaseYOffset;
    const float p1x = x;
    const float p1y = py * cpitch - z * spitch;
    const float p1z = py * spitch + z * cpitch;
    const float p2x = p1x * cyaw - p1z * syaw;
    const float p2y = p1y;
    const float p2z = p1x * syaw + p1z * cyaw;
    const float focal = 250.0f / max(0.5f, perspective);
    const float zOff = 120.0f;
    const float scale = focal / (focal + p2z + zOff);
    return {static_cast<int>((SCREEN_WIDTH_PX / 2.0f) + p2x * scale),
            static_cast<int>((SCREEN_HEIGHT_PX / 2.0f) + p2y * scale),
            scale};
  };

  const float eyeSpacing = constrain(state.faceEyeSpacing, 36.0f, 70.0f);
  const ProjectedPoint leftEyePt = project3D(-eyeSpacing, 0.0f, 20.0f);
  const ProjectedPoint rightEyePt = project3D(eyeSpacing, 0.0f, 20.0f);
  const int cxL = leftEyePt.x;
  const int cyL = leftEyePt.y;
  const int cxR = rightEyePt.x;
  const int cyR = rightEyePt.y;
  const int safeRxL =
      max(1, static_cast<int>(safeRxF * leftEyePt.scale * 1.55f));
  const int safeRyL =
      max(1, static_cast<int>(safeRyF * leftEyePt.scale * 1.55f));
  const int safeRxR =
      max(1, static_cast<int>(safeRxF * rightEyePt.scale * 1.55f));
  const int safeRyR =
      max(1, static_cast<int>(safeRyF * rightEyePt.scale * 1.55f));

  if (_blinkPending) {
    _blinkLeftCx = static_cast<int16_t>(cxL);
    _blinkRightCx = static_cast<int16_t>(cxR);
    _blinkCy = static_cast<int16_t>((cyL + cyR) / 2);
    _blinkRxL = static_cast<int16_t>(safeRxL);
    _blinkRxR = static_cast<int16_t>(safeRxR);
    _blinkRyMax = static_cast<int16_t>(max(safeRyL, safeRyR));
    _blinkColor = eyeColor;

    const int leftEdge = cxL - safeRxL - 2;
    const int rightEdge = cxR + safeRxR + 2;
    const int topEdge = _blinkCy - _blinkRyMax - 2;
    const int botEdge = _blinkCy + _blinkRyMax + 2;
    const int clipX = max(0, leftEdge);
    const int clipY = max(0, topEdge);
    const int clipW = min(static_cast<int>(SCREEN_WIDTH_PX), rightEdge) - clipX;
    const int clipH = min(static_cast<int>(SCREEN_HEIGHT_PX), botEdge) - clipY;
    _blinkClipX = static_cast<int16_t>(clipX);
    _blinkClipY = static_cast<int16_t>(clipY);
    _blinkClipW = static_cast<int16_t>(max(0, clipW));
    _blinkClipH = static_cast<int16_t>(max(0, clipH));
  }

  if (eepyBlend > 0.1f) {
    fillEllipsePx(cxL, cyL, safeRxL, safeRyL, eyeColor);
    fillEllipsePx(cxR, cyR, safeRxR, safeRyR, eyeColor);
    const int cutoutShiftY = static_cast<int>(4.0f * eepyBlend);
    const int cutRyL = max(1, static_cast<int>(safeRyL * 0.6f * eepyBlend));
    const int cutRyR = max(1, static_cast<int>(safeRyR * 0.6f * eepyBlend));
    fillEllipsePx(cxL, cyL + cutoutShiftY, safeRxL, cutRyL, COLOR_BLACK);
    fillEllipsePx(cxR, cyR + cutoutShiftY, safeRxR, cutRyR, COLOR_BLACK);
    if (eepyBlend > 0.5f) {
      const ProjectedPoint g1 = project3D(70.0f + sinf(frame / 10.0f) * 2.0f,
                                          -40.0f + cosf(frame / 10.0f) * 2.0f,
                                          15.0f);
      fillRectPx(g1.x, g1.y - 2, 1, 5, COLOR_WHITE);
      fillRectPx(g1.x - 2, g1.y, 5, 1, COLOR_WHITE);
      const ProjectedPoint g2 = project3D(-60.0f,
                                          30.0f + sinf(frame / 12.0f) * 2.0f,
                                          20.0f);
      fillEllipsePx(g2.x, g2.y, 2, 2, COLOR_GRAY);
      const ProjectedPoint mouth = project3D(0.0f, 35.0f, 10.0f);
      fillEllipsePx(mouth.x, mouth.y, 4, 5,
                    M5.Display.color565(200, 230, 255));
    }
  } else {
    fillEllipsePx(cxL, cyL, safeRxL, safeRyL, eyeColor);
    fillEllipsePx(cxR, cyR, safeRxR, safeRyR, eyeColor);
    if (angryBlend > 0.01f) {
      const int avgRy = max(1, (safeRyL + safeRyR) / 2);
      const int browTop = -avgRy - 10;
      const float dropInner = browTop + (avgRy * 2.4f * angryBlend);
      const float dropOuter = browTop + (avgRy * 1.0f * angryBlend);
      const ProjectedPoint pOuterL = project3D(-120.0f, dropOuter, 25.0f);
      const ProjectedPoint pNose = project3D(0.0f, dropInner, 0.0f);
      const ProjectedPoint pOuterR = project3D(120.0f, dropOuter, 25.0f);
      const ProjectedPoint pTopL = project3D(-120.0f, -150.0f, 25.0f);
      const ProjectedPoint pTopNose = project3D(0.0f, -150.0f, 0.0f);
      const ProjectedPoint pTopR = project3D(120.0f, -150.0f, 25.0f);
      fillTrianglePx(pOuterL.x, pOuterL.y, pNose.x, pNose.y, pTopL.x,
                     pTopL.y, COLOR_BLACK);
      fillTrianglePx(pNose.x, pNose.y, pTopL.x, pTopL.y, pTopNose.x,
                     pTopNose.y, COLOR_BLACK);
      fillTrianglePx(pOuterR.x, pOuterR.y, pNose.x, pNose.y, pTopR.x,
                     pTopR.y, COLOR_BLACK);
      fillTrianglePx(pNose.x, pNose.y, pTopR.x, pTopR.y, pTopNose.x,
                     pTopNose.y, COLOR_BLACK);
    }
  }
}

void TextDisplay::drawPageIndicator(int pageIndex, int pageCount) const {
  if (pageCount <= 1) {
    return;
  }
  // Up to 8 dots — solid for current page, hollow for the rest. Past that,
  // fall back to a single down-triangle (more) or hollow bullet (last).
  constexpr int kMaxDots = 8;
  String indicator;
  if (pageCount <= kMaxDots) {
    for (int i = 0; i < pageCount; i++) {
      indicator += (i == pageIndex) ? kGlyphBulletFilled : kGlyphBulletHollow;
    }
  } else {
    indicator += (pageIndex >= pageCount - 1) ? kGlyphBulletHollow
                                              : kGlyphTriangleDown;
  }
  const int charCount = static_cast<int>(indicator.length());
  const int yTop = kFooterRow * LINE_HEIGHT;
  const int xStart = 4 + (kCharsPerLine - charCount) * 8;
  // Footer text was already drawn; clear behind the indicator so multi-dot
  // rows don't overlap with footerLeft / footerRight.
  if (_canvasReady) {
    _canvas.fillRect(xStart, yTop, charCount * 8, LINE_HEIGHT, COLOR_BLACK);
  } else {
    M5.Display.fillRect(xStart, yTop, charCount * 8, LINE_HEIGHT, COLOR_BLACK);
  }
  int x = xStart;
  for (int i = 0; i < charCount; i++) {
    drawCharCell(x, yTop, indicator[i], COLOR_GRAY);
    x += 8;
  }
}

void TextDisplay::drawBellIcon(int cx, int cy, uint16_t color) const {
  const int originX = cx - kBellW / 2;
  const int originY = cy - kBellH / 2;
  for (int y = 0; y < kBellH; y++) {
    const uint8_t *row = kBellBits + y * kBellRowBytes;
    for (int x = 0; x < kBellW; x++) {
      const uint8_t byte = row[x >> 3];
      if (!((byte >> (7 - (x & 7))) & 1)) continue;
      if (_canvasReady) {
        _canvas.drawPixel(originX + x, originY + y, color);
      } else {
        M5.Display.drawPixel(originX + x, originY + y, color);
      }
    }
  }
}

void TextDisplay::drawAlarm(const DisplayState &state) const {
  const String title = state.alarmTitle.isEmpty() ? String("ALARM") : state.alarmTitle;
  const String safeTitle = fitLine(title);
  const bool hasDetail = !state.alarmDetail.isEmpty();

  // Bell + title share a row, vertically centered. Detail line sits below when
  // present; when there's no detail, the whole composition sits dead-center.
  const int titlePixelWidth = static_cast<int>(safeTitle.length()) * 16;
  const int gap = 8;
  const int rowWidth = kBellW + gap + titlePixelWidth;
  const int rowX = max(0, (SCREEN_WIDTH_PX - rowWidth) / 2);
  const int rowTopY = hasDetail ? 44 : 60;
  const int bellCx = rowX + kBellW / 2;
  const int bellCy = rowTopY + kBellH / 2;
  drawBellIcon(bellCx, bellCy, COLOR_WHITE);

  const int titleX = rowX + kBellW + gap;
  const int titleY = rowTopY;
  if (_canvasReady) {
    _canvas.setTextColor(COLOR_WHITE);
    _canvas.setTextSize(2);
    _canvas.setCursor(titleX, titleY);
    _canvas.print(safeTitle);
    _canvas.setTextSize(1);
  } else {
    M5.Display.setTextColor(COLOR_WHITE);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(titleX, titleY);
    M5.Display.print(safeTitle);
    M5.Display.setTextSize(1);
  }

  if (hasDetail) {
    const String safeDetail = fitLine(state.alarmDetail);
    const int detailX =
        max(0, (SCREEN_WIDTH_PX - static_cast<int>(safeDetail.length()) * 8) / 2);
    if (_canvasReady) {
      _canvas.setTextColor(COLOR_GRAY);
      _canvas.setCursor(detailX, 76);
      _canvas.print(safeDetail);
    } else {
      M5.Display.setTextColor(COLOR_GRAY);
      M5.Display.setCursor(detailX, 76);
      M5.Display.print(safeDetail);
    }
  }
}

void TextDisplay::drawMenu(const DisplayState &state) const {
  const int startRow = kLines - state.menuItemCount;
  for (int i = 0; i < state.menuItemCount; i++) {
    const int row = startRow + i;
    const bool selected = i == state.menuSelectedIndex;
    const String prefix = selected ? "> " : "  ";
    drawLine(row, prefix + state.menuItems[i],
             selected ? COLOR_WHITE : COLOR_GRAY);
  }

  if (state.menuHasMoreAbove) {
    drawGlyphAtRight(startRow - 1 >= 1 ? startRow - 1 : 1, 'v', COLOR_GRAY);
  }
  if (state.menuHasMoreBelow) {
    drawGlyphAtRight(kLines - 1, 'v', COLOR_GRAY);
  }
}
