#pragma once

#include <Txt.h>

#include <string>

#include "activities/UiListActivity.h"

class TxtReaderChapterSelectionActivity final : public UiListActivity {
  const Txt& txt;
  HalFile chapterFile;
  uint32_t chapterCount = 0;
  uint32_t currentOffset = 0;

  // Windowed row buffers: chapter titles are read from the SD-backed chapter
  // index (readChapter), so only the rows around the viewport are
  // materialized. The window follows nav.top via itemsWindowFirst (see
  // fui::ListProps); refreshing it also batch-prewarms the window's CJK
  // fallback glyphs, so each page of the list pays one bounded SD pass and
  // repaints stay RAM-only.
  static constexpr int CHAPTER_WINDOW = 24;
  std::string windowLabels[CHAPTER_WINDOW];
  freeink::ui::ListItem windowItems[CHAPTER_WINDOW];
  int windowStart = -1;
  int windowCount = 0;
  void refreshChapterWindow(int start);

  int listCount() const override { return static_cast<int>(chapterCount); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  // Back cancels with a result here.
  bool handleButtons() override;
  // Header is drawn inside the safe area (not full-width like the base).
  void drawChrome() override;

 public:
  explicit TxtReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const Txt& txt,
                                             uint32_t currentOffset);

  void onEnter() override;
  void onExit() override;
};
