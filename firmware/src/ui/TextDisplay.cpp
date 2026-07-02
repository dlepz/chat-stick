#include "TextDisplay.h"

#include "../Config.h"
#include <M5Unified.h>
#include <math.h>

namespace {
constexpr uint16_t COLOR_BLACK = 0x0000;
constexpr uint16_t COLOR_WHITE = 0xFFFF;
constexpr uint16_t COLOR_GRAY = 0x7BEF;
constexpr uint16_t COLOR_DARK_GRAY = 0x39E7;
constexpr uint16_t COLOR_RED = 0xF800;
constexpr uint16_t COLOR_YELLOW = 0xFFE0;
constexpr uint16_t COLOR_GREEN = 0x07E0;
constexpr uint16_t COLOR_BLUE = 0x041F;
constexpr int LINE_HEIGHT = 16;
constexpr int PAGE_DOT_RADIUS = 2;
} // namespace

void TextDisplay::init() {
  M5.Display.setRotation(1);
  M5.Display.setFont(&fonts::AsciiFont8x16);
  M5.Display.setTextSize(1);
  M5.Display.fillScreen(COLOR_BLACK);
  _canvas.setColorDepth(16);
  _canvas.createSprite(SCREEN_WIDTH_PX, SCREEN_HEIGHT_PX);
  _canvasReady = true;
  _eyeCanvas.setColorDepth(16);
  _eyeCanvasReady = (_eyeCanvas.createSprite(kEyeCanvasW, kEyeCanvasH) != nullptr);
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

  if (state.showReactiveFace) {
    drawExpressiveEyesScene(state);
    drawTurnFeedback(state.turnFeedbackColor);
    if (_canvasReady) {
      _canvas.pushSprite(&M5.Display, 0, 0);
    }
    if (_blinkPending) {
      runSmoothBlink();
    }
    return;
  }

  if (state.showScene) {
    drawScene(state);
    if (_canvasReady) {
      _canvas.pushSprite(&M5.Display, 0, 0);
    }
    return;
  }

  drawLine(0, mergeEdgeText(state.headerLeft, state.headerRight), COLOR_GRAY);
  drawTurnFeedback(state.turnFeedbackColor);
  if (state.showMenu) {
    drawMenu(state);
  } else {
    String wrapped[32];
    const int wrappedCount = wrapBodyText(state.bodyText, wrapped, 32);
    const int pageCount =
        max(1, (wrappedCount + kBodyRows - 1) / kBodyRows);
    const int safePageIndex =
        constrain(state.pageIndex, 0, max(0, pageCount - 1));

    for (int i = 0; i < kBodyRows; i++) {
      const int lineIndex = safePageIndex * kBodyRows + i;
      drawLine(i + 1, lineIndex < wrappedCount ? wrapped[lineIndex] : "",
               COLOR_WHITE);
    }
    if (pageCount > 1) {
      drawPageIndicator(safePageIndex, pageCount);
    }
  }

  drawLine(kFooterRow, mergeEdgeText(state.footerLeft, state.footerRight),
           COLOR_GRAY);

  if (state.showRecordingProgress) {
    drawRecordingProgress(state.recordingProgress);
  }

  if (_canvasReady) {
    _canvas.pushSprite(&M5.Display, 0, 0);
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
  if (clampedHeight > 2) {
    if (_canvasReady) {
      _canvas.fillRect(x + 1, y + height - clampedHeight + 1, barWidth - 2,
                       clampedHeight - 2, COLOR_RED);
    } else {
      M5.Display.fillRect(x + 1, y + height - clampedHeight + 1, barWidth - 2,
                          clampedHeight - 2, COLOR_RED);
    }
  }
}

void TextDisplay::drawPageIndicator(int pageIndex, int pageCount) const {
  if (pageCount <= 1) {
    return;
  }

  const int totalWidth = pageCount * (PAGE_DOT_RADIUS * 2 + 4) - 4;
  int x = (SCREEN_WIDTH_PX - totalWidth) / 2;
  const int y = SCREEN_HEIGHT_PX - 8;
  for (int i = 0; i < pageCount; i++) {
    const uint16_t color = i == pageIndex ? COLOR_WHITE : COLOR_DARK_GRAY;
    if (_canvasReady) {
      _canvas.fillCircle(x + PAGE_DOT_RADIUS, y, PAGE_DOT_RADIUS, color);
    } else {
      M5.Display.fillCircle(x + PAGE_DOT_RADIUS, y, PAGE_DOT_RADIUS, color);
    }
    x += PAGE_DOT_RADIUS * 2 + 4;
  }
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

  // Little friend.
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

  // Tiny dog.
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

  // A few blocky flowers in the grass.
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
  case AppState::Error:
  default:
    prompt = "Offline";
    break;
  }

  drawTextPx(6, 4, "Quiz Masters", COLOR_DARK_GRASS);
  drawTextPx(6, 116, prompt, COLOR_DARK_GRASS);
  drawTextPx(174, 116, "B back", COLOR_DARK_GRASS);
}

// Tight inner loop for the blink animation. Runs after the main canvas push
// completes; bypasses the canvas and writes directly to the LCD via batched
// SPI (startWrite/endWrite). Adapted from dmtrKovalenko/esp32-smooth-eye-blinking
// — same asymmetric quadratic easing, but uses LovyanGFX partial-region writes
// instead of allocating a separate ellipse buffer.
void TextDisplay::runSmoothBlink() const {
  // Frame-counted timing ported directly from
  // dmtrKovalenko/esp32-smooth-eye-blinking: BLINK_FRAMES=20 with FRAME_DELAY
  // between each frame, asymmetric quadratic easing across the two halves.
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

  // Anchor the eye sprite at clipX/clipY (top-left of the bounding box) and
  // translate eye centers into sprite-local coords. The sprite is fixed-size
  // (220x110) but only the [0..clipW, 0..clipH] portion holds meaningful
  // pixels — the rest stays black and overdraws the (already-black) screen
  // background harmlessly.
  const int localCxL = _blinkLeftCx - _blinkClipX;
  const int localCxR = _blinkRightCx - _blinkClipX;
  const int localCy = _blinkCy - _blinkClipY;

  for (int blinkState = 0; blinkState < kBlinkFrames; blinkState++) {
    float p, openness;
    if (blinkState < kHalfFrames) {
      p = static_cast<float>(blinkState) / static_cast<float>(kHalfFrames);
      openness = 1.0f - p * p;
    } else {
      p = static_cast<float>(blinkState - kHalfFrames) / static_cast<float>(kHalfFrames);
      openness = p * (2.0f - p);
    }
    const int ry = max(1, static_cast<int>(_blinkRyMax * openness));

    // Render the entire frame to the eye sprite (RAM) first — fast in-memory
    // operations, no LCD traffic. Then push the whole sprite as a single DMA
    // setAddrWindow + writePixels burst, so the LCD never shows a partially
    // drawn frame. This is exactly what the reference repo does.
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
    if (random(10) == 0) _blinkDoubleQueued = true;
  }
}

void TextDisplay::drawExpressiveEyesScene(const DisplayState &state) const {
  const int frame = static_cast<int>(state.sceneFrame * constrain(state.faceAnimSpeed, 0.25f, 3.0f));
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

  // Emotion blend physics, ported from the HTML harness. Force/damping rescaled
  // for the ~30 fps reactive-face render rate so settling time stays the same
  // as the original 7 Hz cadence.
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

  // Base dimensions.
  const float baseRx = 24.0f + (10.0f * angryBlend) + (4.0f * eepyBlend);
  const float baseRy = 45.0f - (7.0f * angryBlend) - (40.0f * eepyBlend);

  float targetXOffset = 0.0f;
  float targetYOffset = 0.0f;
  float targetRx = baseRx;
  float targetRy = baseRy;
  float targetBaseYOffset = sinf(frame / 8.0f) * (2.0f * (1.0f - eepyBlend));

  // Gaze targeting: the web harness uses mouse position. On-device, use IMU
  // tilt when present; otherwise use speech darting/idle look-around.
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

  // Audio reactivity.
  if (rawVoiceLevel > 0.7f) {
    targetRx = baseRx + 4.0f;
    targetRy = baseRy + 3.0f;
    targetBaseYOffset -= 6.0f;
  } else if (rawVoiceLevel > 0.2f) {
    targetRx = baseRx + 1.0f;
    targetRy = baseRy + 1.0f;
    targetBaseYOffset -= 2.0f;
  } else if (rawVoiceLevel > 0.0f || _faceSilenceFrames < 40) {
    float intensity = 1.0f;
    if (rawVoiceLevel <= 0.0f) intensity = 1.0f - (_faceSilenceFrames / 40.0f);
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

  // Eepy overrides.
  if (eepyBlend > 0.01f) {
    if (rawVoiceLevel > 0.0f) {
      targetRx = baseRx + (2.0f * rawVoiceLevel * (1.0f - eepyBlend));
      targetRy = baseRy + (2.0f * rawVoiceLevel * (1.0f - eepyBlend));
    }
    targetBaseYOffset += (15.0f * eepyBlend);

    if (eepyBlend > 0.5f) {
      const int g1x = 190 + static_cast<int>(sinf(frame / 10.0f) * 2.0f);
      const int g1y = 40 + static_cast<int>(cosf(frame / 10.0f) * 2.0f);
      fillRectPx(g1x, g1y - 2, 1, 5, COLOR_WHITE);
      fillRectPx(g1x - 2, g1y, 5, 1, COLOR_WHITE);

      const int g2x = 50 + static_cast<int>(cosf(frame / 12.0f) * 2.0f);
      const int g2y = 110 + static_cast<int>(sinf(frame / 12.0f) * 2.0f);
      fillEllipsePx(g2x, g2y, 2, 2, COLOR_GRAY);
    }
  }

  // LERP state. Factors rescaled (was 0.2/0.3) for the ~30 fps reactive-face
  // render rate so settling time matches the original 7 Hz cadence — the
  // payoff is ~4x more interpolation steps per second, much smoother motion.
  _faceEyeX += (targetXOffset - _faceEyeX) * 0.05f;
  _faceEyeY += (targetYOffset - _faceEyeY) * 0.05f;
  _faceRx += (targetRx - _faceRx) * 0.075f;
  _faceRy += (targetRy - _faceRy) * 0.075f;
  _faceBaseYOffset += (targetBaseYOffset - _faceBaseYOffset) * 0.05f;

  // Blink is now rendered after the canvas push as a tight inner loop in
  // runSmoothBlink(); the scene draws fully-open eyes and only schedules.
  {
    constexpr uint16_t kIdleMinMs = 1500;
    constexpr uint16_t kIdleMaxMs = 4500;
    const uint32_t now = millis();
    // Skip blinks when angry — the brow triangles live on the main canvas, so
    // the blink overlay (eyes only) would erase them mid-animation.
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
  const int safeRx = max(1, static_cast<int>(safeRxF));
  const int safeRy = max(1, static_cast<int>(safeRyF));

  // White -> red based on angry blend. RGB565 approximation.
  const uint8_t gb = static_cast<uint8_t>(255.0f * (1.0f - angryBlend));
  const uint16_t eyeColor = M5.Display.color565(255, gb, gb);

  // Pseudo-3D projection inspired by the harness. We cannot draw true filled
  // Bezier paths with arbitrary clipping on LovyanGFX, but projecting each eye
  // center and radius gives the same dimensional feel: the face yaws/pitches
  // as the gaze moves and the nearer eye becomes subtly larger.
  // facePerspective is the dimensionality knob: scales how far the head turns
  // (yaw/pitch) and tightens the focal length, so a single value controls both
  // the rotation amount and how dramatic the foreshortening looks.
  const float perspective = constrain(state.facePerspective, 0.0f, 3.0f);
  const float yaw = (_faceEyeX / 22.0f) * 0.35f * perspective;
  const float pitch = (_faceEyeY / 15.0f) * 0.25f * perspective;
  const float cyaw = cosf(yaw);
  const float syaw = sinf(yaw);
  const float cpitch = cosf(pitch);
  const float spitch = sinf(pitch);
  struct ProjectedPoint { int x; int y; float scale; };
  auto project3D = [&](float x, float y, float z) -> ProjectedPoint {
    const float py = y + _faceBaseYOffset;
    const float p1x = x;
    const float p1y = py * cpitch - z * spitch;
    const float p1z = py * spitch + z * cpitch;
    const float p2x = p1x * cyaw - p1z * syaw;
    const float p2y = p1y;
    const float p2z = p1x * syaw + p1z * cyaw;
    // Lower focal = stronger perspective foreshortening. Tighten as the
    // perspective knob increases so high values feel more "wide-angle".
    const float focal = 250.0f / max(0.5f, perspective);
    const float zOff = 120.0f;
    const float scale = focal / (focal + p2z + zOff);
    return {
        static_cast<int>((SCREEN_WIDTH_PX / 2.0f) + p2x * scale),
        static_cast<int>((SCREEN_HEIGHT_PX / 2.0f) + p2y * scale),
        scale,
    };
  };

  const float eyeSpacing = constrain(state.faceEyeSpacing, 36.0f, 70.0f);
  const ProjectedPoint leftEyePt = project3D(-eyeSpacing, 0.0f, 20.0f);
  const ProjectedPoint rightEyePt = project3D(eyeSpacing, 0.0f, 20.0f);
  const int cxL = leftEyePt.x;
  const int cyL = leftEyePt.y;
  const int cxR = rightEyePt.x;
  const int cyR = rightEyePt.y;
  const float leftVisualScale = leftEyePt.scale * 1.55f;
  const float rightVisualScale = rightEyePt.scale * 1.55f;
  const int safeRxL = max(1, static_cast<int>(safeRxF * leftVisualScale));
  const int safeRyL = max(1, static_cast<int>(safeRyF * leftVisualScale));
  const int safeRxR = max(1, static_cast<int>(safeRxF * rightVisualScale));
  const int safeRyR = max(1, static_cast<int>(safeRyF * rightVisualScale));

  // Cache eye geometry for the post-push smooth blink loop.
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
    // Eepy: crescent eyes by drawing black ellipse cutouts.
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
      fillEllipsePx(mouth.x, mouth.y, 4, 5, M5.Display.color565(200, 230, 255));
    }
  } else {
    // Default/angry: draw full projected eyes, then use projected triangle
    // masks like the supplied LovyanGFX-oriented harness.
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

      fillTrianglePx(pOuterL.x, pOuterL.y, pNose.x, pNose.y, pTopL.x, pTopL.y, COLOR_BLACK);
      fillTrianglePx(pNose.x, pNose.y, pTopL.x, pTopL.y, pTopNose.x, pTopNose.y, COLOR_BLACK);
      fillTrianglePx(pOuterR.x, pOuterR.y, pNose.x, pNose.y, pTopR.x, pTopR.y, COLOR_BLACK);
      fillTrianglePx(pNose.x, pNose.y, pTopR.x, pTopR.y, pTopNose.x, pTopNose.y, COLOR_BLACK);
    }
  }
}

void TextDisplay::drawGermanFlagScene(const DisplayState &state) const {
  constexpr uint16_t COLOR_BLACK = 0x0000;
  constexpr uint16_t COLOR_RED = 0xF800;
  constexpr uint16_t COLOR_GOLD = 0xFFE0;
  constexpr uint16_t COLOR_DARK_RED = 0x9000;
  constexpr uint16_t COLOR_DARK_GOLD = 0xAD20;
  constexpr uint16_t COLOR_BG = 0x18E3;
  constexpr uint16_t COLOR_GRID = 0x39E7;
  constexpr uint16_t COLOR_WHITE = 0xFFFF;
  constexpr uint16_t COLOR_GRAY = 0x8410;

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

  // Pixel grid backdrop.
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

  // Flag pole and base.
  fillRectPx(flagX - 10, flagY - 10, 5, 96, COLOR_GRAY);
  fillRectPx(flagX - 18, flagY + 84, 24, 6, COLOR_GRAY);
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
      // A tiny darker edge gives the blocks a pixel-art tile feel.
      fillRectPx(x, y + cell - 2, cell, 2, shade);
    }
  }

  // A few chunky sparkles.
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
