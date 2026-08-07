#include "TxtReaderActivity.h"

#include <BidiUtils.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Serialization.h>
#include <Utf8.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ProgressFile.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "TxtReaderChapterSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t CHUNK_SIZE = 8 * 1024;  // 8KB chunk for reading
// GBK chunks are read shorter because transcodeGbkInPlace() appends the UTF-8
// result behind the raw bytes in the same buffer: raw + utf8 (worst case 3/2x
// expansion) must fit within CHUNK_SIZE + 1.
constexpr size_t GBK_RAW_CHUNK_SIZE = CHUNK_SIZE * 2 / 5;
// Cache file magic and version
constexpr uint32_t CACHE_MAGIC = 0x54585449;  // "TXTI"
constexpr uint8_t CACHE_VERSION = 4;          // Increment when cache format changes
}  // namespace

void TxtReaderActivity::onEnter() {
  Activity::onEnter();

  if (!txt) {
    return;
  }

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  txt->setupCacheDir();

  // Save current txt as last opened file and add to recent books
  auto filePath = txt->getPath();
  auto fileName = filePath.substr(filePath.rfind('/') + 1);
  APP_STATE.openEpubPath = filePath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(filePath, fileName, "", "");

  // Trigger first update
  requestUpdate();
}

void TxtReaderActivity::onExit() {
  Activity::onExit();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  pageOffsets.clear();
  currentPageLines.clear();
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  txt.reset();
}

void TxtReaderActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || ReaderUtils::isTouchMenuGesture(mappedInput)) {
    openChapterSelection();
    return;
  }

  if (ReaderUtils::handleBackNavigation(mappedInput, activityManager, txt ? txt->getPath().c_str() : "",
                                        {this, [](void* ctx) { static_cast<TxtReaderActivity*>(ctx)->onGoHome(); }})) {
    return;
  }

  const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);
  auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  prevTriggered = prevTriggered || touch.prev;
  nextTriggered = nextTriggered || touch.next;
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (prevTriggered && currentPage > 0) {
    currentPage--;
    requestUpdate();
  } else if (nextTriggered) {
    if (currentPage < totalPages - 1) {
      currentPage++;
      requestUpdate();
    } else {
      onGoHome();
    }
  }
}

void TxtReaderActivity::openChapterSelection() {
  if (!txt) {
    return;
  }
  if (!initialized) {
    initializeReader();
  }
  if (pageOffsets.empty()) {
    return;
  }

  uint32_t chapterCount = 0;
  bool hasCachedIndex = false;
  {
    HalFile chapterFile;
    hasCachedIndex = txt->openChapterIndex(chapterFile, textEncoding, chapterCount);
  }
  if (!hasCachedIndex) {
    auto* scratch = static_cast<uint8_t*>(malloc(CHUNK_SIZE + 1));
    if (!scratch) {
      return;
    }
    const auto& popupMetrics = UITheme::getInstance().getMetrics();
    const auto popupStyle = popupMetrics.popupTextBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    RenderLock lock(*this);
    renderer.prewarmText(UI_12_FONT_ID, tr(STR_INDEXING), popupStyle);
    renderer.prewarmText(UI_12_FONT_ID, tr(STR_INDEX_FAILED), popupStyle);
    GUI.drawPopup(renderer, tr(STR_INDEXING));
    pagesUntilFullRefresh = 1;
    const bool built = txt->buildChapterIndex(textEncoding, scratch, CHUNK_SIZE + 1, chapterCount);
    free(scratch);
    if (!built) {
      LOG_ERR("TRS", "Failed to build TXT chapter index");
      renderer.clearScreen();
      GUI.drawPopup(renderer, tr(STR_INDEX_FAILED));
      return;
    }
  }

  const uint32_t currentOffset = static_cast<uint32_t>(pageOffsets[currentPage]);
  startActivityForResult(
      std::make_unique<TxtReaderChapterSelectionActivity>(renderer, mappedInput, *txt, currentOffset),
      [this](const ActivityResult& result) {
        const auto* selected = std::get_if<TxtOffsetResult>(&result.data);
        if (result.isCancelled || !selected) {
          requestUpdate();
          return;
        }
        // The full page index is always available here, so the chapter offset
        // maps straight to a page via binary search (no lazy-index Direct mode
        // like upstream CrossMux needs).
        const auto nextPage =
            std::upper_bound(pageOffsets.begin(), pageOffsets.end(), static_cast<size_t>(selected->sourceOffset));
        currentPage = nextPage == pageOffsets.begin() ? 0 : static_cast<int>(nextPage - pageOffsets.begin() - 1);
        requestUpdate();
      });
}

void TxtReaderActivity::initializeReader() {
  if (initialized) {
    return;
  }

  // Store current settings for cache validation
  cachedFontId = SETTINGS.getReaderFontId();
  cachedScreenMargin = SETTINGS.screenMargin;
  cachedParagraphAlignment = SETTINGS.paragraphAlignment;

  // Calculate viewport dimensions
  renderer.getOrientedViewableTRBL(&cachedOrientedMarginTop, &cachedOrientedMarginRight, &cachedOrientedMarginBottom,
                                   &cachedOrientedMarginLeft);
  cachedOrientedMarginTop += cachedScreenMargin;
  cachedOrientedMarginLeft += cachedScreenMargin;
  cachedOrientedMarginRight += cachedScreenMargin;
  cachedOrientedMarginBottom +=
      std::max(cachedScreenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));

  viewportWidth = renderer.getScreenWidth() - cachedOrientedMarginLeft - cachedOrientedMarginRight;
  const int viewportHeight = renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom;
  const int lineHeight = renderer.getLineHeight(cachedFontId);

  linesPerPage = viewportHeight / lineHeight;
  if (linesPerPage < 1) linesPerPage = 1;

  LOG_DBG("TRS", "Viewport: %dx%d, lines per page: %d", viewportWidth, viewportHeight, linesPerPage);

  // Detect source encoding before indexing: GBK files need transcoding and
  // paginate differently than a raw UTF-8 parse of the same bytes.
  probeTextEncoding();

  // Try to load cached page index first
  if (!loadPageIndexCache()) {
    // Cache not found, build page index
    buildPageIndex();
    // Pure-ASCII prefixes defer detection until a non-ASCII byte shows up mid
    // build; if the file turned out to be GBK, pages indexed before detection
    // parsed raw GBK bytes as UTF-8 garbage - rebuild once with transcoding.
    if (textEncoding == txt_encoding::Encoding::Gbk) {
      LOG_DBG("TRS", "GBK detected during first index pass, rebuilding");
      buildPageIndex();
    }
    // Save to cache for next time
    savePageIndexCache();
  }

  // Load saved progress
  loadProgress();

  initialized = true;
}

void TxtReaderActivity::probeTextEncoding() {
  const size_t fileSize = txt->getFileSize();
  if (fileSize == 0) {
    return;
  }
  const size_t sampleLength = std::min(CHUNK_SIZE, fileSize);
  auto* sample = static_cast<uint8_t*>(malloc(sampleLength));
  if (!sample) {
    return;
  }
  if (txt->readContent(sample, 0, sampleLength)) {
    textEncoding = txt_encoding::detect(sample, sampleLength, sampleLength == fileSize);
    LOG_DBG("TRS", "TXT encoding probe: %u", static_cast<unsigned>(textEncoding));
  }
  free(sample);
}

void TxtReaderActivity::buildPageIndex() {
  pageOffsets.clear();
  pageOffsets.push_back(0);  // First page starts at offset 0

  size_t offset = 0;
  const size_t fileSize = txt->getFileSize();

  LOG_DBG("TRS", "Building page index for %zu bytes...", fileSize);

  // Prewarm the popup glyphs before drawPopup(): the message is CJK on SD-card
  // fonts and drawPopup() measures + draws it while holding the render lock.
  const auto& popupMetrics = UITheme::getInstance().getMetrics();
  const auto popupStyle = popupMetrics.popupTextBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  const char* indexingText = tr(STR_INDEXING);
  renderer.prewarmText(UI_12_FONT_ID, indexingText, popupStyle);
  GUI.drawPopup(renderer, indexingText);

  while (offset < fileSize) {
    std::vector<std::string> tempLines;
    size_t nextOffset = offset;

    if (!loadPageAtOffset(offset, tempLines, nextOffset)) {
      break;
    }

    if (nextOffset <= offset) {
      // No progress made, avoid infinite loop
      break;
    }

    offset = nextOffset;
    if (offset < fileSize) {
      pageOffsets.push_back(offset);
    }

    // Yield to other tasks periodically
    if (pageOffsets.size() % 20 == 0) {
      vTaskDelay(1);
    }
  }

  totalPages = pageOffsets.size();
  LOG_DBG("TRS", "Built page index: %d pages", totalPages);
}

bool TxtReaderActivity::loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset) {
  outLines.clear();
  const size_t fileSize = txt->getFileSize();

  if (offset >= fileSize) {
    return false;
  }

  // Read a chunk from file (GBK reads a shorter raw chunk, see GBK_RAW_CHUNK_SIZE)
  const bool isGbk = (textEncoding == txt_encoding::Encoding::Gbk);
  const size_t readLimit = isGbk ? GBK_RAW_CHUNK_SIZE : CHUNK_SIZE;
  size_t chunkSize = std::min(readLimit, fileSize - offset);
  auto* buffer = static_cast<uint8_t*>(malloc(CHUNK_SIZE + 1));
  if (!buffer) {
    LOG_ERR("TRS", "Failed to allocate %zu bytes", CHUNK_SIZE + 1);
    return false;
  }

  if (!txt->readContent(buffer, offset, chunkSize)) {
    free(buffer);
    return false;
  }
  const bool isLastChunk = (offset + chunkSize == fileSize);

  // Deferred detection: pure-ASCII prefixes are ambiguous between UTF-8 and
  // GBK, so the probe at reader init may have returned Unknown.
  if (textEncoding == txt_encoding::Encoding::Unknown) {
    const auto detected = txt_encoding::detect(buffer, chunkSize, isLastChunk);
    if (detected != txt_encoding::Encoding::Unknown) {
      textEncoding = detected;
      LOG_DBG("TRS", "TXT encoding detected late: %u at offset %zu", static_cast<unsigned>(detected), offset);
    }
  }

  if (textEncoding == txt_encoding::Encoding::Gbk) {
    // Transcode in place: the UTF-8 result lands at the buffer start and the
    // parser below works on UTF-8. chunkSize becomes the UTF-8 length while
    // page offsets stay raw file offsets (mapped back via gbkSourceLength).
    const auto result = txt_encoding::transcodeGbkInPlace(buffer, chunkSize, CHUNK_SIZE + 1, isLastChunk);
    if (result.rawLength == 0 || result.utf8Length == 0) {
      free(buffer);
      return false;
    }
    chunkSize = result.utf8Length;
  } else {
    buffer[chunkSize] = '\0';
  }

  // Prime the SD card font's advance table with this chunk's codepoints.
  // Without this, every getTextAdvanceX() call in the wrap loop below triggers
  // on-demand glyph loads through the 8-slot overflow ring buffer, which
  // thrashes for any text with more than 8 unique chars (i.e. all English),
  // floods the heap with short-lived bitmap allocations, and eventually
  // corrupts FreeRTOS state. The advance table persists across calls per
  // font, so the cost amortizes to ~ASCII-size after the first chunk.
  if (renderer.isSdCardFont(cachedFontId)) {
    renderer.ensureSdCardFontReady(cachedFontId, reinterpret_cast<const char*>(buffer), /*styleMask=*/0x01);
  }

  // Parse lines from buffer
  size_t pos = 0;

  while (pos < chunkSize && static_cast<int>(outLines.size()) < linesPerPage) {
    // Find end of line
    size_t lineEnd = pos;
    while (lineEnd < chunkSize && buffer[lineEnd] != '\n') {
      lineEnd++;
    }

    // Check if we have a complete line (isLastChunk is evaluated in raw file
    // space, so it is correct for both UTF-8 and transcoded GBK buffers)
    bool lineComplete = (lineEnd < chunkSize) || isLastChunk;

    if (!lineComplete && static_cast<int>(outLines.size()) > 0) {
      // Incomplete line and we already have some lines, stop here
      break;
    }

    // Calculate the actual length of line content in the buffer (excluding newline)
    size_t lineContentLen = lineEnd - pos;

    // Check for carriage return
    bool hasCR = (lineContentLen > 0 && buffer[pos + lineContentLen - 1] == '\r');
    size_t displayLen = hasCR ? lineContentLen - 1 : lineContentLen;

    // Extract line content for display (without CR/LF)
    std::string line(reinterpret_cast<char*>(buffer + pos), displayLen);

    // Track position within this source line (in bytes from pos)
    size_t lineBytePos = 0;

    // Emit at least one visual line for each source line (including blank lines),
    // then continue with wrapping when needed.
    do {
      if (line.empty()) {
        outLines.emplace_back();
        break;
      }

      int lineWidth = renderer.getTextAdvanceX(cachedFontId, line.c_str(), EpdFontFamily::REGULAR);

      if (lineWidth <= viewportWidth) {
        outLines.push_back(line);
        lineBytePos = displayLen;  // Consumed entire display content
        line.clear();
        break;
      }

      // Find break point by binary-searching the largest char-boundary prefix
      // whose width fits, then preferring the last space within it (same result
      // as the previous linear scan, which decremented one char at a time and
      // re-measured the whole prefix per step — O(n^2) per line. Space-less CJK
      // text always hit that path and stalled index building for minutes).
      std::vector<size_t> boundaries;
      boundaries.push_back(0);
      for (size_t i = 0; i < line.length();) {
        i++;
        while (i < line.length() && (line[i] & 0xC0) == 0x80) {
          i++;  // skip UTF-8 continuation bytes
        }
        boundaries.push_back(i);
      }
      size_t lo = 0, hi = boundaries.size() - 1, fitIdx = 0;
      while (lo <= hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const int w = renderer.getTextAdvanceX(cachedFontId, line.substr(0, boundaries[mid]).c_str(),
                                               EpdFontFamily::REGULAR);
        if (w <= viewportWidth) {
          fitIdx = mid;
          if (mid >= boundaries.size() - 1) break;
          lo = mid + 1;
        } else {
          if (mid == 0) break;
          hi = mid - 1;
        }
      }
      size_t breakPos = boundaries[fitIdx];
      if (breakPos > 0) {
        // Try to break at the last space within the fitting prefix
        const size_t spacePos = line.rfind(' ', breakPos - 1);
        if (spacePos != std::string::npos && spacePos > 0) {
          breakPos = spacePos;
        }
      }

      if (breakPos == 0) {
        breakPos = 1;
      }

      outLines.push_back(line.substr(0, breakPos));

      // Skip space at break point
      size_t skipChars = breakPos;
      if (breakPos < line.length() && line[breakPos] == ' ') {
        skipChars++;
      }
      lineBytePos += skipChars;
      line = line.substr(skipChars);
    } while (!line.empty() && static_cast<int>(outLines.size()) < linesPerPage);

    // Determine how much of the source buffer we consumed
    if (line.empty()) {
      // Fully consumed this source line, move past the newline
      pos = lineEnd + 1;
    } else {
      // Partially consumed - page is full mid-line
      // Move pos to where we stopped in the line (NOT past the line)
      pos = pos + lineBytePos;
      break;
    }
  }

  // Ensure we make progress even if calculations go wrong
  if (pos == 0 && !outLines.empty()) {
    // Fallback: at minimum, consume something to avoid infinite loop
    pos = 1;
  }

  // pos is a UTF-8 buffer position; map it back to raw GBK bytes so page
  // offsets always index the source file as stored on disk.
  const size_t consumed =
      textEncoding == txt_encoding::Encoding::Gbk ? txt_encoding::gbkSourceLength(buffer, pos) : pos;
  nextOffset = offset + consumed;

  // Make sure we don't go past the file
  if (nextOffset > fileSize) {
    nextOffset = fileSize;
  }

  free(buffer);

  return !outLines.empty();
}

void TxtReaderActivity::render(RenderLock&&) {
  if (!txt) {
    return;
  }

  // Initialize reader if not done
  if (!initialized) {
    initializeReader();
  }

  if (pageOffsets.empty()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Bounds check
  if (currentPage < 0) currentPage = 0;
  if (currentPage >= totalPages) currentPage = totalPages - 1;

  // Load current page content
  size_t offset = pageOffsets[currentPage];
  size_t nextOffset;
  currentPageLines.clear();
  loadPageAtOffset(offset, currentPageLines, nextOffset);

  renderer.clearScreen();
  renderPage();

  // Save progress
  saveProgress();
}

void TxtReaderActivity::renderPage() {
  const int lineHeight = renderer.getLineHeight(cachedFontId);
  const int contentWidth = viewportWidth;

  // Render text lines with alignment
  auto renderLines = [&]() {
    int y = cachedOrientedMarginTop;
    for (const auto& line : currentPageLines) {
      if (!line.empty()) {
        int x = cachedOrientedMarginLeft;
        const bool lineIsRtl = BidiUtils::startsWithRtl(line.c_str(), BidiUtils::RTL_PARAGRAPH_PROBE_DEPTH);
        uint8_t effectiveAlignment = cachedParagraphAlignment;
        if (lineIsRtl && (effectiveAlignment == CrossPointSettings::LEFT_ALIGN ||
                          effectiveAlignment == CrossPointSettings::JUSTIFIED)) {
          effectiveAlignment = CrossPointSettings::RIGHT_ALIGN;
        }
        const int textWidth = renderer.getTextAdvanceX(cachedFontId, line.c_str(), EpdFontFamily::REGULAR);

        // Apply text alignment
        switch (effectiveAlignment) {
          case CrossPointSettings::LEFT_ALIGN:
          default:
            // x already set to left margin
            break;
          case CrossPointSettings::CENTER_ALIGN: {
            x = cachedOrientedMarginLeft + (contentWidth - textWidth) / 2;
            break;
          }
          case CrossPointSettings::RIGHT_ALIGN: {
            x = cachedOrientedMarginLeft + contentWidth - textWidth;
            break;
          }
          case CrossPointSettings::JUSTIFIED:
            // For plain text, justified is treated as left-aligned
            // (true justification would require word spacing adjustments)
            break;
        }

        renderer.drawText(cachedFontId, x, y, line.c_str());
      }
      y += lineHeight;
    }
  };

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  renderLines();  // scan pass — text accumulated, no drawing
  scope.endScanAndPrewarm();

  // BW rendering
  renderLines();
  renderStatusBar();

  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  if (SETTINGS.textAntiAliasing) {
    ReaderUtils::renderAntiAliased(renderer, [&renderLines]() { renderLines(); });
  }
  // scope destructor clears font cache via FontCacheManager
}

void TxtReaderActivity::renderStatusBar() const {
  const float progress = totalPages > 0 ? (currentPage + 1) * 100.0f / totalPages : 0;
  std::string title;
  if (SETTINGS.statusBarSpec().showsTitle()) {
    title = txt->getTitle();
  }
  GUI.drawStatusBar(renderer, progress, currentPage + 1, totalPages, title);
}

void TxtReaderActivity::saveProgress() const {
  uint8_t data[4];
  data[0] = currentPage & 0xFF;
  data[1] = (currentPage >> 8) & 0xFF;
  data[2] = 0;
  data[3] = 0;
  if (!ProgressFile::writeAtomic(txt->getCachePath(), data, sizeof(data))) {
    LOG_ERR("TRS", "Failed to save progress: page %d", currentPage);
  }
}

void TxtReaderActivity::loadProgress() {
  HalFile f;
  if (Storage.openFileForRead("TRS", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] + (data[1] << 8);
      if (currentPage >= totalPages) {
        currentPage = totalPages - 1;
      }
      if (currentPage < 0) {
        currentPage = 0;
      }
      LOG_DBG("TRS", "Loaded progress: page %d/%d", currentPage, totalPages);
    }
  }
}

bool TxtReaderActivity::loadPageIndexCache() {
  // Cache file format (using serialization module):
  // - uint32_t: magic "TXTI"
  // - uint8_t: cache version
  // - uint32_t: file size (to validate cache)
  // - int32_t: viewport width
  // - int32_t: lines per page
  // - int32_t: font ID (to invalidate cache on font change)
  // - int32_t: screen margin (to invalidate cache on margin change)
  // - uint8_t: paragraph alignment (to invalidate cache on alignment change)
  // - uint8_t: source text encoding (to invalidate cache on encoding change)
  // - uint32_t: total pages count
  // - N * uint32_t: page offsets

  std::string cachePath = txt->getCachePath() + "/index.bin";
  HalFile f;
  if (!Storage.openFileForRead("TRS", cachePath, f)) {
    LOG_DBG("TRS", "No page index cache found");
    return false;
  }

  // Read and validate header using serialization module
  uint32_t magic;
  serialization::readPod(f, magic);
  if (magic != CACHE_MAGIC) {
    LOG_DBG("TRS", "Cache magic mismatch, rebuilding");
    return false;
  }

  uint8_t version;
  serialization::readPod(f, version);
  if (version != CACHE_VERSION) {
    LOG_DBG("TRS", "Cache version mismatch (%d != %d), rebuilding", version, CACHE_VERSION);
    return false;
  }

  uint32_t fileSize;
  serialization::readPod(f, fileSize);
  if (fileSize != txt->getFileSize()) {
    LOG_DBG("TRS", "Cache file size mismatch, rebuilding");
    return false;
  }

  int32_t cachedWidth;
  serialization::readPod(f, cachedWidth);
  if (cachedWidth != viewportWidth) {
    LOG_DBG("TRS", "Cache viewport width mismatch, rebuilding");
    return false;
  }

  int32_t cachedLines;
  serialization::readPod(f, cachedLines);
  if (cachedLines != linesPerPage) {
    LOG_DBG("TRS", "Cache lines per page mismatch, rebuilding");
    return false;
  }

  int32_t fontId;
  serialization::readPod(f, fontId);
  if (fontId != cachedFontId) {
    LOG_DBG("TRS", "Cache font ID mismatch (%d != %d), rebuilding", fontId, cachedFontId);
    return false;
  }

  int32_t margin;
  serialization::readPod(f, margin);
  if (margin != cachedScreenMargin) {
    LOG_DBG("TRS", "Cache screen margin mismatch, rebuilding");
    return false;
  }

  uint8_t alignment;
  serialization::readPod(f, alignment);
  if (alignment != cachedParagraphAlignment) {
    LOG_DBG("TRS", "Cache paragraph alignment mismatch, rebuilding");
    return false;
  }

  uint8_t encodingValue;
  serialization::readPod(f, encodingValue);
  if (!txt_encoding::isSerializedValueValid(encodingValue)) {
    LOG_DBG("TRS", "Cache encoding value invalid, rebuilding");
    return false;
  }
  const auto cachedEncoding = static_cast<txt_encoding::Encoding>(encodingValue);
  if (textEncoding != txt_encoding::Encoding::Unknown && cachedEncoding != textEncoding) {
    LOG_DBG("TRS", "Cache encoding mismatch (%u != %u), rebuilding", static_cast<unsigned>(cachedEncoding),
            static_cast<unsigned>(textEncoding));
    return false;
  }
  // Pure-ASCII files probe as Unknown; adopt the cached encoding so GBK files
  // with an ASCII prefix still transcode from the first chunk.
  if (textEncoding == txt_encoding::Encoding::Unknown) {
    textEncoding = cachedEncoding;
  }

  uint32_t numPages;
  serialization::readPod(f, numPages);

  // Read page offsets
  pageOffsets.clear();
  pageOffsets.reserve(numPages);

  for (uint32_t i = 0; i < numPages; i++) {
    uint32_t offset;
    serialization::readPod(f, offset);
    pageOffsets.push_back(offset);
  }

  totalPages = pageOffsets.size();
  LOG_DBG("TRS", "Loaded page index cache: %d pages", totalPages);
  return true;
}

void TxtReaderActivity::savePageIndexCache() const {
  std::string cachePath = txt->getCachePath() + "/index.bin";
  HalFile f;
  if (!Storage.openFileForWrite("TRS", cachePath, f)) {
    LOG_ERR("TRS", "Failed to save page index cache");
    return;
  }

  // Write header using serialization module
  serialization::writePod(f, CACHE_MAGIC);
  serialization::writePod(f, CACHE_VERSION);
  serialization::writePod(f, static_cast<uint32_t>(txt->getFileSize()));
  serialization::writePod(f, static_cast<int32_t>(viewportWidth));
  serialization::writePod(f, static_cast<int32_t>(linesPerPage));
  serialization::writePod(f, static_cast<int32_t>(cachedFontId));
  serialization::writePod(f, static_cast<int32_t>(cachedScreenMargin));
  serialization::writePod(f, cachedParagraphAlignment);
  serialization::writePod(f, static_cast<uint8_t>(textEncoding));
  serialization::writePod(f, static_cast<uint32_t>(pageOffsets.size()));

  // Write page offsets
  for (size_t offset : pageOffsets) {
    serialization::writePod(f, static_cast<uint32_t>(offset));
  }

  LOG_DBG("TRS", "Saved page index cache: %d pages", totalPages);
}

ScreenshotInfo TxtReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Txt;
  if (txt) {
    const std::string t = txt->getTitle();
    snprintf(info.title, sizeof(info.title), "%s", t.c_str());
  }
  info.currentPage = currentPage + 1;
  info.totalPages = totalPages;
  info.progressPercent = totalPages > 0 ? static_cast<int>((currentPage + 1) * 100.0f / totalPages + 0.5f) : 0;
  if (info.progressPercent > 100) info.progressPercent = 100;
  return info;
}
