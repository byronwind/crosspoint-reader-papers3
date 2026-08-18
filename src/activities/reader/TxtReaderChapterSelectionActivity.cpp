#include "TxtReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UIScale.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

TxtReaderChapterSelectionActivity::TxtReaderChapterSelectionActivity(GfxRenderer& renderer,
                                                                     MappedInputManager& mappedInput, const Txt& txt,
                                                                     uint32_t currentOffset)
    : UiListActivity("TxtReaderChapterSelection", renderer, mappedInput), txt(txt), currentOffset(currentOffset) {}

void TxtReaderChapterSelectionActivity::onEnter() {
  UiListActivity::onEnter();
  txt_encoding::Encoding encoding = txt_encoding::Encoding::Unknown;
  if (!txt.openChapterIndex(chapterFile, encoding, chapterCount)) {
    LOG_ERR("TRC", "Failed to open TXT chapter index");
    chapterCount = 0;
  } else if (chapterCount > 0) {
    uint32_t currentChapter = 0;
    if (txt.findChapterForOffset(chapterFile, chapterCount, currentOffset, currentChapter)) {
      nav.selected = static_cast<int>(currentChapter);
    }
  }
}

void TxtReaderChapterSelectionActivity::onExit() {
  UiListActivity::onExit();
  chapterFile.close();
}

// Materialize the ListItem/label window starting at `start` (clamped). The
// titles are SD reads (readChapter), so this runs only when the viewport
// leaves the current window. Finishes with a batch prewarm of the window's
// CJK fallback glyphs -- one bounded SD pass per list page; repaints inside
// the window stay RAM-only.
void TxtReaderChapterSelectionActivity::refreshChapterWindow(const int start) {
  const int total = listCount();
  int clamped = start;
  if (clamped > total - CHAPTER_WINDOW) clamped = total - CHAPTER_WINDOW;
  if (clamped < 0) clamped = 0;
  if (clamped == windowStart) return;

  windowCount = total - clamped < CHAPTER_WINDOW ? total - clamped : CHAPTER_WINDOW;
  for (int i = 0; i < windowCount; i++) {
    txt_chapter_index::Record chapter;
    windowLabels[i].clear();
    if (txt.readChapter(chapterFile, chapterCount, static_cast<uint32_t>(clamped + i), chapter)) {
      windowLabels[i] = chapter.title;
    }
    fui::ListItem item;
    item.label = windowLabels[i].c_str();
    item.actionValue = static_cast<int16_t>(clamped + i);
    windowItems[i] = item;
  }
  windowStart = clamped;

  struct PrewarmCtx {
    const std::string* labels;
    int count;
  } prewarmCtx{windowLabels, windowCount};
  renderer.prewarmFallbackText(
      uiScaleSpec().bodyFontId,
      [](const void* ctx, uint32_t i) -> const char* {
        const auto* c = static_cast<const PrewarmCtx*>(ctx);
        return i < static_cast<uint32_t>(c->count) ? c->labels[i].c_str() : nullptr;
      },
      &prewarmCtx, static_cast<uint32_t>(windowCount));
}

void TxtReaderChapterSelectionActivity::activateIndex(const int index) {
  txt_chapter_index::Record chapter;
  if (index < 0 || !txt.readChapter(chapterFile, chapterCount, static_cast<uint32_t>(index), chapter)) {
    return;
  }
  // The activated row leaves this screen (finish); a lingering flash would gray
  // an unrelated element on the next render.
  app.clearTapFlash();
  nav.selected = index;
  setResult(TxtOffsetResult{chapter.sourceOffset});
  finish();
}

bool TxtReaderChapterSelectionActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return true;
  }
  return UiListActivity::handleButtons();
}

void TxtReaderChapterSelectionActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content: the safe area minus the header band drawChrome paints the title in.
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                      static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (listCount() == 0) {
    screen.centeredText(tr(STR_NO_CHAPTERS), screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.count = static_cast<uint16_t>(listCount());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  syncListViewport(screen, props);
  // Materialize the row window for the final viewport (syncListViewport just
  // applied follow/clamping to nav.top) and hand list() the window with its
  // absolute base index.
  refreshChapterWindow(nav.top);
  props.items = windowItems;
  props.itemsWindowFirst = static_cast<uint16_t>(windowStart);
  screen.list(props);
}

void TxtReaderChapterSelectionActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight},
                 tr(STR_SELECT_CHAPTER));
}
