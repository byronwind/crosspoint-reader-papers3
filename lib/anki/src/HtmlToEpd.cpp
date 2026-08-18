// Guarded so non-ANKIEINK builds never compile this translation unit, which
// would otherwise pull in lib/anki and its dependencies.
#ifdef ANKIEINK

#include "HtmlToEpd.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <strings.h>
#include <vector>

#include "../../../src/fontIds.h"

namespace anki {
namespace {

// Nearest built-in Noto Sans size for a requested pixel size (the card fonts
// come in 12/14/16/18; anything larger clamps to 18).
int fontIdForSize(int size) {
  if (size <= 13) return NOTOSANS_12_FONT_ID;
  if (size <= 15) return NOTOSANS_14_FONT_ID;
  if (size <= 17) return NOTOSANS_16_FONT_ID;
  return NOTOSANS_18_FONT_ID;
}

// Decode HTML entities (&amp; &lt; &gt; &quot; &#39; &nbsp; &#DDD; &#xHH;).
void decodeEntities(const char* begin, const char* end, std::string& out) {
  const char* p = begin;
  while (p < end) {
    if (*p != '&') {
      out.push_back(*p++);
      continue;
    }
    const char* semi = static_cast<const char*>(memchr(p, ';', static_cast<size_t>(end - p)));
    if (semi == nullptr || semi - p > 10) {
      out.push_back(*p++);
      continue;
    }
    const std::string_view ent(p + 1, static_cast<size_t>(semi - p - 1));
    if (ent == "amp") {
      out.push_back('&');
    } else if (ent == "lt") {
      out.push_back('<');
    } else if (ent == "gt") {
      out.push_back('>');
    } else if (ent == "quot") {
      out.push_back('"');
    } else if (ent == "apos") {
      out.push_back('\'');
    } else if (ent == "nbsp") {
      out.push_back(' ');
    } else if (!ent.empty() && ent[0] == '#') {
      unsigned long cp = 0;
      if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')) {
        cp = strtoul(std::string(ent.substr(2)).c_str(), nullptr, 16);
      } else {
        cp = strtoul(std::string(ent.substr(1)).c_str(), nullptr, 10);
      }
      // Emit the code point as UTF-8 (only BMP needed for card text).
      if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
      } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
      } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
      }
    } else {
      out.append(ent.data(), ent.size());
    }
    p = semi + 1;
  }
}

// Collapse runs of whitespace (HTML semantics: all whitespace is a single
// space, except we keep leading/trailing intact so the caller controls
// spacing).
void collapseWhitespace(std::string& text) {
  std::string out;
  out.reserve(text.size());
  bool lastWasSpace = false;
  for (char c : text) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      if (!lastWasSpace) out.push_back(' ');
      lastWasSpace = true;
    } else {
      out.push_back(c);
      lastWasSpace = false;
    }
  }
  text = std::move(out);
}

bool endsWithIgnoreCase(const std::string& s, const char* suffix) {
  const size_t slen = strlen(suffix);
  if (s.size() < slen) return false;
  return strcasecmp(s.c_str() + s.size() - slen, suffix) == 0;
}

// Streaming layout engine: consumes tags/text from the HTML and draws
// word-wrapped runs into a bounded box on the GfxRenderer.
class HtmlLayout {
 public:
  HtmlLayout(GfxRenderer& renderer, const HtmlToEpd::Layout& layout, const HtmlToEpd::FontConfig& font,
             const HtmlToEpd::ImageResolver& resolver, bool measureOnly = false)
      : r(renderer), lay(layout), resolver(resolver), baseFont(fontIdForSize(font.baseFontSize)), measure(measureOnly) {
    x = layout.x + layout.padding;
    // The visible box (top..bottom) is fixed; scrolling moves the content
    // cursor up, and lines that end up above `top` are skipped, never drawn
    // outside the box. Measure mode runs unconstrained so the cursor walks
    // the whole document.
    top = layout.y + layout.padding;
    y = top - layout.scrollY;
    right = layout.x + layout.width - layout.padding;
    bottom = measure ? INT_MAX : layout.y + layout.height - layout.padding;
    baseFont = resolveFont(baseFont);
    currentFont = baseFont;
  }

  /// Feed one raw HTML chunk (tags included). Caller splits at arbitrary
  /// boundaries; state is carried across calls.
  void feed(const char* begin, const char* end) {
    const char* p = begin;
    while (p < end) {
      if (*p == '<') {
        const char* tagEnd = static_cast<const char*>(memchr(p, '>', static_cast<size_t>(end - p)));
        if (tagEnd == nullptr) break;  // malformed; treat the rest as text
        handleTag(p + 1, tagEnd, end);
        p = tagEnd + 1;
      } else {
        const char* next = static_cast<const char*>(memchr(p, '<', static_cast<size_t>(end - p)));
        const char* segEnd = next == nullptr ? end : next;
        pending.append(p, static_cast<size_t>(segEnd - p));
        p = segEnd;
      }
    }
  }

  /// Flush any pending text and report the height used.
  int finish() {
    flush();
    return y - (lay.y + lay.padding - lay.scrollY);
  }

 private:
  GfxRenderer& r;
  const HtmlToEpd::Layout& lay;
  const HtmlToEpd::ImageResolver& resolver;
  int baseFont;

  int x = 0, y = 0, top = 0, right = 0, bottom = 0;
  int currentFont = 0;
  const bool measure = false;  ///< true = advance the cursor without drawing
  std::string pending;   // raw text awaiting decode + wrap + draw
  std::string prefix;    // list item bullet/number for the next run
  bool orderedList = false;
  int orderedIndex = 0;
  bool overflowed = false;

  bool roomFor(int h) const { return y + h <= bottom; }

  // When requested (Anki cards), swap a built-in font id for the SD fallback
  // registered for it so Latin and CJK text share one typeface; falls back
  // to the original id when no SD family is loaded.
  int resolveFont(const int fontId) const {
    if (!lay.useFallbackFont) return fontId;
    const int fb = r.getFallbackFontId(fontId);
    return fb != 0 ? fb : fontId;
  }

  void handleTag(const char* tagStart, const char* tagEnd, const char* htmlEnd) {
    const char* t = tagStart;
    bool closing = false;
    if (*t == '/') {
      closing = true;
      ++t;
    }
    const char* nameStart = t;
    while (t < tagEnd && (isalnum(static_cast<unsigned char>(*t)))) ++t;
    const std::string_view name(nameStart, static_cast<size_t>(t - nameStart));
    const char* attr = t;

    if (name.empty()) return;

    // Skip script/style bodies entirely (their text must never render).
    if (!closing && (name == "script" || name == "style")) {
      const std::string closer = "</" + std::string(name) + ">";
      const char* closeAt = strstr(tagEnd + 1, closer.c_str());
      if (closeAt != nullptr) {
        feed(closeAt + closer.size(), htmlEnd);
      }
      return;
    }

    if (closing) {
      if (name == "h1" || name == "h2" || name == "h3" || name == "h4" || name == "h5" || name == "h6") {
        flush();
        currentFont = baseFont;
      } else if (name == "ul" || name == "ol") {
        orderedList = false;
      } else if (name == "p" || name == "div" || name == "li" || name == "tr" || name == "table") {
        flush();
        y += r.getLineHeight(currentFont) / 2;  // paragraph gap
      }
      return;
    }

    if (name == "br") {
      flush();
      y += r.getLineHeight(currentFont);
      return;
    }
    if (name == "p" || name == "div" || name == "section" || name == "table") {
      flush();
      y += r.getLineHeight(currentFont) / 2;
      return;
    }
    if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') {
      flush();
      currentFont = resolveFont(fontIdForSize(name[1] <= '3' ? 18 : 14));
      y += r.getLineHeight(currentFont) / 2;
      return;
    }
    if (name == "ul") {
      flush();
      orderedList = false;
      return;
    }
    if (name == "ol") {
      flush();
      orderedList = true;
      orderedIndex = 0;
      return;
    }
    if (name == "li") {
      flush();
      char buf[16];
      if (orderedList) {
        snprintf(buf, sizeof(buf), "%d. ", ++orderedIndex);
        prefix = buf;
      } else {
        prefix = "• ";
      }
      return;
    }
    if (name == "tr") {
      flush();
      y += r.getLineHeight(currentFont) / 2;
      return;
    }
    if (name == "td" || name == "th") {
      pending.append("  ", 2);  // simple cell separator
      return;
    }
    if (name == "img") {
      flush();
      drawImage(attr, tagEnd);
      return;
    }
    // Inline tags (b/i/u/strong/em/span/font/...): styling is out of scope
    // for the EPD card renderer; text continues in the same run.
  }

  void flush() {
    if (pending.empty()) {
      if (!prefix.empty()) prefix.clear();
      return;
    }
    std::string text;
    decodeEntities(pending.data(), pending.data() + pending.size(), text);
    collapseWhitespace(text);
    pending.clear();
    if (!prefix.empty()) {
      text = prefix + text;
      prefix.clear();
    }
    if (text.empty()) return;

    const int maxWidth = right - x;
    const int lineHeight = r.getLineHeight(currentFont);
    if (maxWidth <= 0 || lineHeight <= 0) {
      overflowed = true;
      return;
    }
    const std::vector<std::string> lines = r.wrappedText(currentFont, text.c_str(), maxWidth, 64);
    for (const auto& line : lines) {
      if (y < top) {
        y += lineHeight;  // scrolled out the top of the box (no partial lines)
        continue;
      }
      if (!roomFor(lineHeight)) {
        overflowed = true;
        return;
      }
      int drawX = x;
      if (lay.centerText) {
        // Center short lines; full-width lines keep their left edge.
        const int lineWidth = r.getTextWidth(currentFont, line.c_str());
        if (lineWidth < maxWidth) drawX = x + (maxWidth - lineWidth) / 2;
      }
      if (!measure) r.drawText(currentFont, drawX, y, line.c_str());
      y += lineHeight;
    }
  }

  void drawImage(const char* attrStart, const char* tagEnd) {
    // Find src="..." (or src='...') inside the tag.
    const std::string_view attrs(attrStart, static_cast<size_t>(tagEnd - attrStart));
    size_t pos = attrs.find("src");
    if (pos == std::string_view::npos) return;
    pos = attrs.find_first_of("\"'", pos);
    if (pos == std::string_view::npos) return;
    const char quote = attrs[pos];
    const size_t valStart = pos + 1;
    const size_t valEnd = attrs.find(quote, valStart);
    if (valEnd == std::string_view::npos) return;
    const std::string src(attrs.substr(valStart, valEnd - valStart));
    if (src.empty()) return;

    std::string path;
    if (resolver) path = resolver(src);
    if (measure || path.empty()) {
      // Measure mode accounts one placeholder line without touching the SD
      // card; real bitmap heights can't be known without loading the file.
      placeholder(src);
      return;
    }

    // Only 1-bit/grayscale BMPs can be drawn directly from the SD card;
    // anything else degrades to a placeholder for now (Phase-2 card polish).
    if (endsWithIgnoreCase(path, ".bmp")) {
      HalFile file;
      if (Storage.openFileForRead("IMG", path.c_str(), file)) {
        Bitmap bitmap(file, true);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          const int availW = right - x;
          const int availH = bottom - y;
          const float scale =
              std::min(static_cast<float>(availW) / static_cast<float>(bitmap.getWidth()),
                       static_cast<float>(availH) / static_cast<float>(bitmap.getHeight()));
          if (scale >= 1.0f) {
            if (y < top) {
              // Partially scrolled out the top; keep the cursor moving but
              // don't draw outside the box (bitmap clipping isn't supported).
              y += static_cast<int>(static_cast<float>(bitmap.getHeight())) + r.getLineHeight(currentFont) / 2;
              return;
            }
            int imgX = x;
            if (lay.centerText) imgX = x + (availW - bitmap.getWidth()) / 2;
            r.drawBitmap(bitmap, imgX, y, bitmap.getWidth(), bitmap.getHeight());
            y += static_cast<int>(static_cast<float>(bitmap.getHeight())) + r.getLineHeight(currentFont) / 2;
            return;
          }
        }
      }
    }
    placeholder(src);
  }

  void placeholder(const std::string& src) {
    std::string text = "[Image: ";
    text += src;
    text += "]";
    const int lineHeight = r.getLineHeight(currentFont);
    if (y < top) {
      y += lineHeight;  // scrolled out the top of the box (no partial lines)
    } else if (roomFor(lineHeight)) {
      if (!measure) r.drawText(currentFont, x, y, text.c_str());
      y += lineHeight;
    } else {
      overflowed = true;
    }
  }
};

}  // namespace

int HtmlToEpd::render(GfxRenderer& renderer, const std::string& html, const Layout& layout,
                      const FontConfig& font, const ImageResolver& resolver) {
  HtmlLayout engine(renderer, layout, font, resolver);
  engine.feed(html.data(), html.data() + html.size());
  return engine.finish();
}

int HtmlToEpd::measure(GfxRenderer& renderer, const std::string& html, const Layout& layout, const FontConfig& font) {
  // Same engine as render(), so block-tag spacing, list prefixes, heading
  // sizes and real font metrics all match exactly — no height drift between
  // the scroll extent and what actually draws.
  HtmlLayout engine(renderer, layout, font, nullptr, true);
  engine.feed(html.data(), html.data() + html.size());
  return engine.finish();
}

std::string HtmlToEpd::stripTags(const std::string& html) {
  std::string out;
  out.reserve(html.size());
  const char* p = html.data();
  const char* end = p + html.size();
  std::string run;
  while (p < end) {
    if (*p == '<') {
      const char* close = static_cast<const char*>(memchr(p, '>', static_cast<size_t>(end - p)));
      p = close == nullptr ? end : close + 1;
      continue;
    }
    const char* next = static_cast<const char*>(memchr(p, '<', static_cast<size_t>(end - p)));
    const char* runEnd = next == nullptr ? end : next;
    run.clear();
    decodeEntities(p, runEnd, run);
    if (!out.empty() && !run.empty()) out.push_back(' ');  // keep runs apart
    out.append(run);
    p = runEnd;
  }
  collapseWhitespace(out);
  return out;
}

}  // namespace anki

#endif  // ANKIEINK
