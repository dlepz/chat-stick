#pragma once

#include "../Config.h"
#include "../state/StateTypes.h"
#include <Arduino.h>

/**
 * @brief Converts a DisplayState snapshot into pixels for the AMOLED.
 *
 * TextDisplay is intentionally a renderer, not an app-state owner. The
 * controller decides what should be shown; this class handles fixed-cell text
 * layout, menu drawing, optional bitmap drawing, and framebuffer flushing.
 *
 * All body text uses SmartBrickFont as a monospace grid. That is why most text
 * layout is expressed as rows and character cells instead of pixel widths.
 */
class TextDisplay {
public:
  /// Fixed font cell width in pixels.
  static constexpr int kCellW = 16;

  /// Fixed font cell height in pixels.
  static constexpr int kCellH = 32;

  /// Horizontal content inset.
  static constexpr int kInsetX = 16;

  /// Vertical content inset.
  static constexpr int kInsetY = 32;

  /// Drawable content width after insets.
  static constexpr int kContentW = SCREEN_WIDTH_PX - 2 * kInsetX;

  /// Maximum monospace characters per text row.
  static constexpr int kCharsPerLine = kContentW / kCellW;

  /// Maximum chat rows visible per page.
  static constexpr int kChatRows = 11;

  /// Height of a single menu row.
  static constexpr int kMenuRowH = 64;

  /// Top Y coordinate of the menu body.
  static constexpr int kMenuBodyY = 128;

  /// Vertical text offset inside a menu row.
  static constexpr int kMenuRowTextYOffset = 8;

  /// Y coordinate of the page indicator dots.
  static constexpr int kDotY = SCREEN_HEIGHT_PX - 20;

  /// Horizontal spacing between page indicator dots.
  static constexpr int kDotSpacing = 12;

  /// Maximum number of page indicator dots drawn.
  static constexpr int kDotCountMax = 8;

  /// Stored image display width.
  static constexpr int kImageW = 232;

  /// Stored image display height.
  static constexpr int kImageH = 112;

  /// Stored image left offset.
  static constexpr int kImageX = (SCREEN_WIDTH_PX - kImageW) / 2;

  /// Stored image top offset.
  static constexpr int kImageY = (SCREEN_HEIGHT_PX - kImageH) / 2;

  /// Initialize display buffers and hardware state.
  void init();

  /**
   * @brief Set display brightness.
   * @param brightness Backlight level.
   */
  void setBrightness(uint8_t brightness);

  /**
   * @brief Render a complete UI frame.
   * @param state UI state snapshot to draw.
   */
  void render(const DisplayState &state);

  /**
   * @brief Count body pages required to render a block of text.
   * @param text Body text to measure.
   * @return Number of pages required.
   */
  int pageCountForText(const String &text) const;

  /**
   * @brief Count wrapped rows required to render a block of text.
   * @param text Body text to measure.
   * @return Number of wrapped rows.
   */
  int wrappedRowCount(const String &text) const;

  /**
   * @brief Store a 1-bit packed bitmap for the body image area.
   * @param packed Packed bitmap bytes, MSB first.
   * @param packedLen Number of bytes in packed.
   * @param width Bitmap width in pixels.
   * @param height Bitmap height in pixels.
   * @return True when the bitmap dimensions match the supported image area.
   */
  bool setImage(const uint8_t *packed, size_t packedLen, int width, int height);

  /// Remove any stored body image.
  void clearImage();

  /// Whether a stored image is currently available for rendering.
  bool hasImage() const { return _imageBuffer != nullptr; }

private:
  /// Footer baseline Y coordinate.
  static constexpr int kFooterY = SCREEN_HEIGHT_PX - kCellH;

  /// Current framebuffer written during render.
  uint16_t *_framebuffer = nullptr;

  /// Previous framebuffer used for diff-based flushes.
  mutable uint16_t *_previousFramebuffer = nullptr;

  /// Whether _previousFramebuffer contains valid frame data.
  mutable bool _hasPreviousFrame = false;

  /// Packed 1-bit body image buffer.
  uint8_t *_imageBuffer = nullptr;

  /// Byte length of the packed body image buffer.
  size_t _imageBufferSize = 0;

  /// Stored image width in pixels.
  int _imageWidth = 0;

  /// Stored image height in pixels.
  int _imageHeight = 0;

  /// Fill the current framebuffer with a solid color.
  void clearFrame(uint16_t color) const;

  /// Flush the framebuffer to the display, optionally forcing a full refresh.
  void flushFrame(bool forceFull = false) const;

  /// Write a single pixel into the framebuffer.
  void putPixel(int x, int y, uint16_t color) const;

  /// Fill a rectangle in the framebuffer.
  void fillRect(int x, int y, int w, int h, uint16_t color) const;

  /// Draw a rectangle outline in the framebuffer.
  void drawRect(int x, int y, int w, int h, uint16_t color) const;

  /// Fill a circle in the framebuffer.
  void fillCircle(int cx, int cy, int radius, uint16_t color) const;

  /// Draw plain text starting at pixel coordinates.
  void drawText(int x, int y, const String &text, uint16_t color,
                int maxChars = kCharsPerLine) const;

  /// Measure rendered text width in pixels.
  int textPixelWidth(const String &text) const;

  /// Trim a string so it fits on one rendered line.
  String fitLine(const String &text) const;

  /// Merge left and right edge text into a single line.
  String mergeEdgeText(const String &left, const String &right) const;

  /// Build a string containing a fixed number of spaces.
  String spaces(int count) const;

  /// Wrap body text into display rows.
  int wrapBodyText(const String &text, String out[], int maxRows) const;

  /// Draw one text line at a row index.
  void drawLine(int row, const String &text, uint16_t color) const;

  /// Draw left and right aligned text on the same row.
  void drawEdgeLine(int row, const String &left, const String &right,
                    uint16_t color) const;

  /// Draw the page indicator dots.
  void drawPageIndicator(int pageIndex, int pageCount) const;

  /// Draw the menu overlay.
  void drawMenu(const DisplayState &state) const;

  /// Draw the currently stored body image.
  void drawStoredImage() const;
};
