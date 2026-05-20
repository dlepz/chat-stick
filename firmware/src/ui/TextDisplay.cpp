#include "TextDisplay.h"

#include "../Config.h"
#include <M5Unified.h>

namespace {
constexpr uint16_t COLOR_BLACK = 0x0000;
constexpr uint16_t COLOR_WHITE = 0xFFFF;
constexpr uint16_t COLOR_GRAY = 0x7BEF;
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
} // namespace

void TextDisplay::init() {
  M5.Display.setRotation(1);
  M5.Display.setFont(&fonts::AsciiFont8x16);
  M5.Display.setTextSize(1);
  M5.Display.fillScreen(COLOR_BLACK);
  _canvas.setColorDepth(16);
  _canvas.createSprite(SCREEN_WIDTH_PX, SCREEN_HEIGHT_PX);
  _canvasReady = true;
}

void TextDisplay::setBrightness(uint8_t brightness) {
  M5.Display.setBrightness(brightness);
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
      _canvas.pushSprite(&M5.Display, 0, 0);
    }
    return;
  }

  const bool hasHeader = !state.headerLeft.isEmpty() || !state.headerRight.isEmpty();
  const bool hasFooterText =
      !state.footerLeft.isEmpty() || !state.footerRight.isEmpty();
  if (hasHeader) {
    drawLine(0, mergeEdgeText(state.headerLeft, state.headerRight), COLOR_GRAY);
  }

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

  if (_canvasReady) {
    _canvas.pushSprite(&M5.Display, 0, 0);
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

String TextDisplay::fitLine(const String &text) const {
  String out;
  out.reserve(kCharsPerLine);

  for (int i = 0; i < static_cast<int>(text.length()) && out.length() < kCharsPerLine;
       i++) {
    const char c = text[i];
    out += (c >= 32 && c <= 126) ? c : ' ';
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
  if (_canvasReady) {
    _canvas.setTextColor(color);
    _canvas.setCursor(4, row * LINE_HEIGHT);
    _canvas.print(fitLine(text));
    return;
  }

  M5.Display.setTextColor(color);
  M5.Display.setCursor(4, row * LINE_HEIGHT);
  M5.Display.print(fitLine(text));
}

void TextDisplay::drawGlyphAtRight(int row, char glyph, uint16_t color) const {
  const int x = 4 + (kCharsPerLine - 1) * 8;
  const int y = row * LINE_HEIGHT;
  if (_canvasReady) {
    _canvas.setTextColor(color);
    _canvas.setCursor(x, y);
    _canvas.print(glyph);
    return;
  }
  M5.Display.setTextColor(color);
  M5.Display.setCursor(x, y);
  M5.Display.print(glyph);
}

void TextDisplay::drawPageIndicator(int pageIndex, int pageCount) const {
  if (pageCount <= 1) {
    return;
  }
  const bool lastPage = pageIndex >= pageCount - 1;
  drawGlyphAtRight(kFooterRow, lastPage ? 'o' : 'v', COLOR_GRAY);
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
