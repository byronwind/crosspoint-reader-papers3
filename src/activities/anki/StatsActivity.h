#pragma once

#ifdef ANKIEINK

#include "activities/Activity.h"
#include "DeckStore.h"
#include "util/ButtonNavigator.h"

/**
 * StatsActivity — Review statistics dashboard.
 *
 * Shows today's progress, 30-day trend, and per-deck breakdown.
 * Rendered as simple bar charts using GfxRenderer primitives.
 */
class StatsActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  anki::DeckStore& store;

  // Stats data
  uint32_t todayReviews = 0;
  uint32_t todayNew = 0;
  uint32_t totalCards = 0;
  uint32_t totalDue = 0;

  void loadStats();

 public:
  explicit StatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, anki::DeckStore& store)
      : Activity("Statistics", renderer, mappedInput), store(store) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};

#endif  // ANKIEINK
