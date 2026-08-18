#pragma once

#include <cstdint>
#include <functional>
#include <string>

class GfxRenderer;

namespace anki {

/**
 * HtmlToEpd — Simplified HTML to E-Paper Display renderer.
 *
 * Parses a subset of HTML and renders it to a GfxRenderer frame buffer.
 * Designed for E-Ink readability: high contrast, limited styling,
 * optimized for the 960x540 display.
 *
 * Supported HTML tags:
 *   Text: b, i, u, br, p, div, span, h1-h6
 *   Lists: ul, ol, li
 *   Tables: table, tr, td, th (simple grid only)
 *   Media: img (from SD card path)
 *   Ignored: script, style, link, meta
 *
 * CSS support: inline style attributes only (color, font-size, text-align).
 * No external stylesheets.
 */
class HtmlToEpd {
 public:
  /// Layout configuration
  struct Layout {
    int x = 0;           ///< Left edge in pixels
    int y = 0;           ///< Top edge in pixels (the UNSCROLLED origin)
    int width = 960;     ///< Available width
    int height = 440;    ///< Available height (540 - status bar - action bar)
    int padding = 16;    ///< Inner padding
    int lineHeight = 0;  ///< Line height (0 = auto from font)
    bool centerText = false;  ///< Center each line horizontally (card content)
    /// Route ALL text through the SD fallback registered for each built-in
    /// font id (no-op when none is registered). Used for Anki cards so the
    /// Latin words/example sentences match the user-selected CJK family
    /// instead of mixing two typefaces on one card.
    bool useFallbackFont = false;
    /// Vertical scroll offset for content taller than the box. The visible box
    /// stays put (x/y/width/height); the content cursor starts `scrollY` px
    /// above the box top, and lines scrolled out the top are skipped rather
    /// than drawn outside the box.
    int scrollY = 0;
  };

  /// Font configuration
  struct FontConfig {
    int baseFontSize = 24;      ///< Base font size in pixels
    int headingScale = 150;     ///< Heading scale percentage (150 = 1.5x)
    const char* fontFamily = "NotoSans";  ///< Font family name
  };

  /// Image resolver callback: given HTML src, returns SD card path
  using ImageResolver = std::function<std::string(const std::string& src)>;

  /// Render HTML content to the GfxRenderer
  /// @param renderer  Target renderer
  /// @param html      HTML string to render
  /// @param layout    Layout bounds
  /// @param font      Font configuration
  /// @param resolver  Image path resolver (may be null)
  /// @return          Total height consumed in pixels
  static int render(GfxRenderer& renderer, const std::string& html, const Layout& layout,
                    const FontConfig& font, const ImageResolver& resolver = nullptr);

  /// Measure the height the content will consume when rendered, using the
  /// same layout engine as render() (real font metrics, block-tag spacing).
  /// Nothing is drawn. Only x/width/padding/useFallbackFont of the layout
  /// matter; height/scrollY are ignored (measurement is unconstrained).
  static int measure(GfxRenderer& renderer, const std::string& html, const Layout& layout, const FontConfig& font);

  /// Strip all HTML tags, returning plain text
  static std::string stripTags(const std::string& html);

  /// Check if HTML contains any images
  static bool containsImages(const std::string& html);
};

}  // namespace anki
