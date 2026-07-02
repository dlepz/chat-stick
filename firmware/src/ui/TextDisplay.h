#pragma once

#include "../state/StateTypes.h"
#include <Arduino.h>
#include <M5Unified.h>

class TextDisplay {
public:
  static constexpr int kCharsPerLine = 29;
  static constexpr int kLines = 8;

  void init();
  void setBrightness(uint8_t brightness);
  void render(const DisplayState &state);
  int pageCountForText(const String &text) const;

private:
  static constexpr int kBodyRows = 6;
  static constexpr int kFooterRow = 7;

  mutable M5Canvas _canvas;
  bool _canvasReady = false;
  mutable M5Canvas _eyeCanvas;
  mutable bool _eyeCanvasReady = false;
  static constexpr int kEyeCanvasW = 220;
  static constexpr int kEyeCanvasH = 110;
  mutable float _faceEyeX = 0.0f;
  mutable float _faceEyeY = 0.0f;
  mutable float _faceRx = 24.0f;
  mutable float _faceRy = 45.0f;
  mutable float _faceBaseYOffset = 0.0f;
  mutable float _faceAngryBlend = 0.0f;
  mutable float _faceAngryVel = 0.0f;
  mutable float _faceEepyBlend = 0.0f;
  mutable float _faceEepyVel = 0.0f;
  mutable int _faceSilenceFrames = 1000;
  mutable uint32_t _nextBlinkAtMs = 0;
  mutable bool _blinkScheduleSeeded = false;
  mutable bool _blinkDoubleQueued = false;
  mutable bool _blinkPending = false;
  mutable int16_t _blinkLeftCx = 0;
  mutable int16_t _blinkRightCx = 0;
  mutable int16_t _blinkCy = 0;
  mutable int16_t _blinkRxL = 0;
  mutable int16_t _blinkRxR = 0;
  mutable int16_t _blinkRyMax = 0;
  mutable int16_t _blinkClipX = 0;
  mutable int16_t _blinkClipY = 0;
  mutable int16_t _blinkClipW = 0;
  mutable int16_t _blinkClipH = 0;
  mutable uint16_t _blinkColor = 0xFFFF;

  String fitLine(const String &text) const;
  String mergeEdgeText(const String &left, const String &right) const;
  String spaces(int count) const;
  int wrapBodyText(const String &text, String out[], int maxRows) const;
  void drawLine(int row, const String &text, uint16_t color) const;
  void drawGlyphAtRight(int row, char glyph, uint16_t color) const;
  void drawRecordingProgress(float progress) const;
  void drawPageIndicator(int pageIndex, int pageCount) const;
  void drawTurnFeedback(const String &colorName) const;
  void drawScene(const DisplayState &state) const;
  void drawLittleGuyScene(const DisplayState &state) const;
  void drawGermanFlagScene(const DisplayState &state) const;
  void drawExpressiveEyesScene(const DisplayState &state) const;
  void runSmoothBlink() const;
  void drawMenu(const DisplayState &state) const;
};
