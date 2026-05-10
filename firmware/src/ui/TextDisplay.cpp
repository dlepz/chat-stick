#include "TextDisplay.h"

#include "../Config.h"
#include "../diag/Log.h"
#include "../hal/Board.h"
#include "SmartBrickFont.h"
#include <esp_heap_caps.h>

namespace {
// RGB565 colors used by the minimal monochrome UI.
constexpr uint16_t COLOR_BLACK = 0x0000;
constexpr uint16_t COLOR_WHITE = 0xFFFF;
constexpr uint16_t COLOR_GRAY = 0x7BEF;

constexpr size_t kFramebufferBytes =
    static_cast<size_t>(SCREEN_WIDTH_PX) * SCREEN_HEIGHT_PX * sizeof(uint16_t);
} // namespace

void TextDisplay::init() {
  auto &display = Board::display();
  if (!display.begin()) {
    Log::client("Display", "display.begin failed");
  }

  // Prefer PSRAM for the front buffer. It is large enough for the full screen
  // and keeps rendering independent from SPI display timing.
  _framebuffer = static_cast<uint16_t *>(
      heap_caps_malloc(kFramebufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!_framebuffer) {
    _framebuffer = static_cast<uint16_t *>(
        heap_caps_malloc(kFramebufferBytes, MALLOC_CAP_8BIT));
  }
  if (_framebuffer) {
    Log::client("Display", "framebuffer bytes=%u",
                static_cast<unsigned>(kFramebufferBytes));
    // The previous buffer lets flushFrame send only changed rows/regions after
    // the first full-screen paint.
    _previousFramebuffer = static_cast<uint16_t *>(heap_caps_malloc(
        kFramebufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!_previousFramebuffer) {
      _previousFramebuffer = static_cast<uint16_t *>(
          heap_caps_malloc(kFramebufferBytes, MALLOC_CAP_8BIT));
    }
    if (!_previousFramebuffer) {
      Log::client("Display", "dirty-rect buffer unavailable; full flush");
    }
  } else {
    Log::client("Display", "framebuffer allocation failed; drawing directly");
  }

  clearFrame(COLOR_BLACK);
  flushFrame(true);
}

void TextDisplay::setBrightness(uint8_t brightness) {
  const bool wasOff = Board::displayBrightness() == 0;
  Board::setDisplayBrightness(brightness);

  if (wasOff && brightness > 0 && _framebuffer) {
    // The SH8601's GRAM can be unreliable after displayOff/light sleep. The
    // dirty-rect cache still thinks unchanged pixels are present, so repaint
    // the whole last frame when the panel wakes.
    delay(20);
    flushFrame(true);
  }
}

void TextDisplay::render(const DisplayState &state) {
  // Rendering is full-scene into the backing buffer every time. flushFrame()
  // decides whether to push the full buffer or just the changed rectangle.
  clearFrame(COLOR_BLACK);

  const bool hasHeader =
      !state.headerLeft.isEmpty() || !state.headerRight.isEmpty();
  const bool hasFooterText =
      !state.footerLeft.isEmpty() || !state.footerRight.isEmpty();

  if (state.showMenu) {
    if (hasHeader) {
      drawEdgeLine(0, state.headerLeft, state.headerRight, COLOR_GRAY);
    }
    drawMenu(state);
  } else {
    // Body text is wrapped before pagination. If an image is present it owns
    // page 0, and text pages are shifted by one.
    const bool imagePage = state.imagePresent && hasImage();
    String wrapped[128];
    const int wrappedCount = wrapBodyText(state.bodyText, wrapped, 128);
    const int textPageCount =
        max(1, (wrappedCount + kChatRows - 1) / kChatRows);
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
      for (int i = 0; i < kChatRows; i++) {
        const int lineIndex = textPageIndex * kChatRows + i;
        const String line = lineIndex < wrappedCount ? wrapped[lineIndex] : "";
        drawLine(i, line, bodyColor);
      }
    }

    if (pageCount > 1 && !hasFooterText) {
      drawPageIndicator(safePageIndex, pageCount);
    }
  }

  if (hasFooterText) {
    drawText(kInsetX, kFooterY,
             mergeEdgeText(state.footerLeft, state.footerRight), COLOR_GRAY);
  }

  flushFrame();
}

bool TextDisplay::setImage(const uint8_t *packed, size_t packedLen, int width,
                           int height) {
  // Images are stored as 1-bit packed masks and drawn in white on black. The
  // server already scales/generates the bitmap to this fixed display slot.
  if (!packed || width != kImageW || height != kImageH) {
    Log::client("Display", "image rejected %dx%d expected=%dx%d", width, height,
                kImageW, kImageH);
    return false;
  }
  const size_t expectedBytes = static_cast<size_t>((width * height + 7) / 8);
  if (packedLen < expectedBytes) {
    Log::client("Display", "image too short bytes=%u expected=%u",
                static_cast<unsigned>(packedLen),
                static_cast<unsigned>(expectedBytes));
    return false;
  }

  if (_imageBufferSize < expectedBytes) {
    if (_imageBuffer) {
      free(_imageBuffer);
    }
    _imageBuffer = static_cast<uint8_t *>(malloc(expectedBytes));
    if (!_imageBuffer) {
      _imageBufferSize = 0;
      Log::client("Display", "failed to allocate image buffer");
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

void TextDisplay::clearFrame(uint16_t color) const {
  if (!_framebuffer) {
    Board::display().fillScreen(color);
    return;
  }

  const size_t pixels = static_cast<size_t>(SCREEN_WIDTH_PX) * SCREEN_HEIGHT_PX;
  for (size_t i = 0; i < pixels; i++) {
    _framebuffer[i] = color;
  }
}

void TextDisplay::flushFrame(bool forceFull) const {
  if (!_framebuffer) {
    return;
  }

  auto &display = Board::display();
  if (forceFull || !_previousFramebuffer || !_hasPreviousFrame) {
    // First paint, explicit full paint, or no previous buffer: push the whole
    // framebuffer and seed the dirty-rect baseline.
    display.draw16bitRGBBitmap(0, 0, _framebuffer, SCREEN_WIDTH_PX,
                               SCREEN_HEIGHT_PX);
    if (_previousFramebuffer) {
      memcpy(_previousFramebuffer, _framebuffer, kFramebufferBytes);
      _hasPreviousFrame = true;
    }
    return;
  }

  int minX = SCREEN_WIDTH_PX;
  int minY = SCREEN_HEIGHT_PX;
  int maxX = -1;
  int maxY = -1;

  // Find one bounding rectangle around all changed pixels. The display library
  // accepts contiguous RGB565 spans, so later we push this rectangle row by
  // row.
  for (int y = 0; y < SCREEN_HEIGHT_PX; y++) {
    const uint16_t *row =
        _framebuffer + static_cast<size_t>(y) * SCREEN_WIDTH_PX;
    const uint16_t *prev =
        _previousFramebuffer + static_cast<size_t>(y) * SCREEN_WIDTH_PX;
    for (int x = 0; x < SCREEN_WIDTH_PX; x++) {
      if (row[x] == prev[x]) {
        continue;
      }
      if (x < minX) {
        minX = x;
      }
      if (x > maxX) {
        maxX = x;
      }
      if (y < minY) {
        minY = y;
      }
      if (y > maxY) {
        maxY = y;
      }
    }
  }

  if (maxX < minX || maxY < minY) {
    return;
  }

  const int dirtyW = maxX - minX + 1;
  const int dirtyH = maxY - minY + 1;
  const int dirtyPixels = dirtyW * dirtyH;
  const int screenPixels = SCREEN_WIDTH_PX * SCREEN_HEIGHT_PX;

  if (dirtyPixels > screenPixels / 3) {
    // Large dirty regions are faster and simpler as a full-screen transfer.
    display.draw16bitRGBBitmap(0, 0, _framebuffer, SCREEN_WIDTH_PX,
                               SCREEN_HEIGHT_PX);
    memcpy(_previousFramebuffer, _framebuffer, kFramebufferBytes);
    _hasPreviousFrame = true;
    return;
  }

  for (int y = minY; y <= maxY; y++) {
    // draw16bitRGBBitmap needs a contiguous source span, so transfer one dirty
    // row at a time instead of copying the rectangle into a temporary buffer.
    uint16_t *row =
        _framebuffer + static_cast<size_t>(y) * SCREEN_WIDTH_PX + minX;
    display.draw16bitRGBBitmap(minX, y, row, dirtyW, 1);
  }

  for (int y = minY; y <= maxY; y++) {
    const size_t offset = static_cast<size_t>(y) * SCREEN_WIDTH_PX + minX;
    memcpy(_previousFramebuffer + offset, _framebuffer + offset,
           dirtyW * sizeof(uint16_t));
  }
}

void TextDisplay::putPixel(int x, int y, uint16_t color) const {
  if (x < 0 || y < 0 || x >= SCREEN_WIDTH_PX || y >= SCREEN_HEIGHT_PX) {
    return;
  }

  if (_framebuffer) {
    _framebuffer[static_cast<size_t>(y) * SCREEN_WIDTH_PX + x] = color;
  } else {
    Board::display().drawPixel(x, y, color);
  }
}

void TextDisplay::fillRect(int x, int y, int w, int h, uint16_t color) const {
  if (w <= 0 || h <= 0 || x >= SCREEN_WIDTH_PX || y >= SCREEN_HEIGHT_PX ||
      x + w <= 0 || y + h <= 0) {
    return;
  }

  const int x0 = max(0, x);
  const int y0 = max(0, y);
  const int x1 = min(SCREEN_WIDTH_PX, x + w);
  const int y1 = min(SCREEN_HEIGHT_PX, y + h);

  if (!_framebuffer) {
    Board::display().fillRect(x0, y0, x1 - x0, y1 - y0, color);
    return;
  }

  for (int yy = y0; yy < y1; yy++) {
    uint16_t *row = _framebuffer + static_cast<size_t>(yy) * SCREEN_WIDTH_PX;
    for (int xx = x0; xx < x1; xx++) {
      row[xx] = color;
    }
  }
}

void TextDisplay::drawRect(int x, int y, int w, int h, uint16_t color) const {
  fillRect(x, y, w, 1, color);
  fillRect(x, y + h - 1, w, 1, color);
  fillRect(x, y, 1, h, color);
  fillRect(x + w - 1, y, 1, h, color);
}

void TextDisplay::fillCircle(int cx, int cy, int radius, uint16_t color) const {
  const int r2 = radius * radius;
  for (int y = -radius; y <= radius; y++) {
    for (int x = -radius; x <= radius; x++) {
      if (x * x + y * y <= r2) {
        putPixel(cx + x, cy + y, color);
      }
    }
  }
}

void TextDisplay::drawText(int x, int y, const String &text, uint16_t color,
                           int maxChars) const {
  const int count = maxChars < 0
                        ? static_cast<int>(text.length())
                        : min(static_cast<int>(text.length()), maxChars);

  for (int i = 0; i < count; i++) {
    const char c = text[i] >= 32 && text[i] <= 126 ? text[i] : ' ';
    SmartBrickFont::Glyph glyph;
    memcpy_P(&glyph, SmartBrickFont::glyphFor(c), sizeof(glyph));

    // Glyph boxes are positioned relative to the fixed text cell, then adjusted
    // by the font baseline metrics so different glyph heights align cleanly.
    const int glyphLeft = x + i * SmartBrickFont::kCellW + glyph.ofsX;
    const int glyphTop =
        y + (SmartBrickFont::kLineHeight - SmartBrickFont::kBaseline) -
        glyph.boxH - glyph.ofsY;

    for (int gy = 0; gy < glyph.boxH; gy++) {
      for (int gx = 0; gx < glyph.boxW; gx++) {
        if (SmartBrickFont::glyphPixelOn(glyph, gx, gy)) {
          putPixel(glyphLeft + gx, glyphTop + gy, color);
        }
      }
    }
  }
}

int TextDisplay::textPixelWidth(const String &text) const {
  return fitLine(text).length() * kCellW;
}

void TextDisplay::drawStoredImage() const {
  if (!_imageBuffer) {
    return;
  }
  // Packed image bits are MSB-first. Only set bits are drawn because the frame
  // has already been cleared to black.
  const int w = _imageWidth;
  const int h = _imageHeight;
  for (int y = 0; y < h; y++) {
    const int rowStart = y * w;
    for (int x = 0; x < w; x++) {
      const int bit = rowStart + x;
      const uint8_t byte = _imageBuffer[bit >> 3];
      const bool on = (byte >> (7 - (bit & 7))) & 1;
      if (on) {
        putPixel(kImageX + x, kImageY + y, COLOR_WHITE);
      }
    }
  }
}

int TextDisplay::pageCountForText(const String &text) const {
  String wrapped[128];
  const int wrappedCount = wrapBodyText(text, wrapped, 128);
  return max(1, (wrappedCount + kChatRows - 1) / kChatRows);
}

int TextDisplay::wrappedRowCount(const String &text) const {
  if (text.isEmpty()) {
    return 0;
  }
  String wrapped[128];
  return wrapBodyText(text, wrapped, 128);
}

String TextDisplay::fitLine(const String &text) const {
  // Text rendering only supports printable ASCII. Unsupported bytes are shown
  // as spaces so layout width remains stable.
  String out;
  out.reserve(kCharsPerLine);

  for (int i = 0;
       i < static_cast<int>(text.length()) && out.length() < kCharsPerLine;
       i++) {
    const char c = text[i];
    out += (c >= 32 && c <= 126) ? c : ' ';
  }

  return out;
}

String TextDisplay::mergeEdgeText(const String &left,
                                  const String &right) const {
  const String safeLeft = fitLine(left);
  const String safeRight = fitLine(right);

  if (safeLeft.isEmpty()) {
    return safeRight;
  }
  if (safeRight.isEmpty()) {
    return safeLeft;
  }

  if (safeLeft.length() + safeRight.length() >= kCharsPerLine) {
    // Preserve the right edge label when the pair does not fit. It commonly
    // carries compact status like page count or menu title.
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

int TextDisplay::wrapBodyText(const String &text, String out[],
                              int maxRows) const {
  // The wrapper is word-based for normal text and falls back to hard breaks for
  // words longer than the display row. Explicit newlines always flush a row.
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

      // A single overlong word at line start is split directly into rows.
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

    // Same overlong-word fallback after flushing the previous line.
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

    if (c >= 32 && c <= 126) {
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

void TextDisplay::drawLine(int row, const String &text, uint16_t color) const {
  drawText(kInsetX, kInsetY + row * kCellH, fitLine(text), color);
}

void TextDisplay::drawEdgeLine(int row, const String &left, const String &right,
                               uint16_t color) const {
  const int y = kInsetY + row * kCellH;
  const String safeLeft = fitLine(left);
  const String safeRight = fitLine(right);
  if (!safeLeft.isEmpty()) {
    drawText(kInsetX, y, safeLeft, color);
  }
  if (!safeRight.isEmpty()) {
    drawText(SCREEN_WIDTH_PX - kInsetX - textPixelWidth(safeRight), y,
             safeRight, color);
  }
}

void TextDisplay::drawPageIndicator(int pageIndex, int pageCount) const {
  if (pageCount <= 1) {
    return;
  }

  const int visible = min(pageCount, kDotCountMax);
  const int active = constrain(pageIndex, 0, visible - 1);
  const int totalW = (visible - 1) * kDotSpacing + 6;
  const int startX = (SCREEN_WIDTH_PX - totalW) / 2;

  for (int i = 0; i < visible; i++) {
    const bool isActive = i == active;
    const int size = isActive ? 6 : 4;
    const int left = startX + i * kDotSpacing;
    const int radius = size / 2;
    fillCircle(left + radius, kDotY + radius, radius,
               isActive ? COLOR_WHITE : COLOR_GRAY);
  }
}

void TextDisplay::drawMenu(const DisplayState &state) const {
  for (int i = 0; i < state.menuItemCount; i++) {
    const bool selected = i == state.menuSelectedIndex;
    const int textY = kMenuBodyY + i * kMenuRowH + kMenuRowTextYOffset;
    const int touchY = textY - ((kMenuRowH - kCellH) / 2);
    const uint16_t color = selected ? COLOR_WHITE : COLOR_GRAY;

    if (selected) {
      fillRect(0, touchY + 6, 4, kMenuRowH - 12, COLOR_GRAY);
      drawText(SCREEN_WIDTH_PX - kInsetX - kCellW, textY, ">", COLOR_WHITE, 1);
    }
    drawText(kInsetX, textY, fitLine(state.menuItems[i]), color);
  }

  const int glyphX = SCREEN_WIDTH_PX - kInsetX - kCellW;
  if (state.menuHasMoreAbove) {
    drawText(glyphX, kMenuBodyY - kCellH, "^", COLOR_GRAY, 1);
  }
  if (state.menuHasMoreBelow) {
    drawText(glyphX, kMenuBodyY + MAX_MENU_VISIBLE_ITEMS * kMenuRowH, "v",
             COLOR_GRAY, 1);
  }
}
