#pragma once

#ifdef ANKIEINK

#include <vector>

#include "activities/Activity.h"
#include "DeckStore.h"
#include "util/ButtonNavigator.h"

/**
 * DeckListActivity — Displays all imported decks with card counts.
 *
 * Layout:
 *   ┌─────────────────────────────────────┐
 *   │  AnkiEInk              [Import]     │  ← Header
 *   ├─────────────────────────────────────┤
 *   │  > Deck Name A                      │
 *   │    120 cards · 15 due · 5 new       │
 *   │  > Deck Name B                      │
 *   │    80 cards · 0 due · 0 new         │
 *   │  ...                                │
 *   ├─────────────────────────────────────┤
 *   │  [Import APKG]  [Settings] [Delete] │  ← Action bar
 *   └─────────────────────────────────────┘
 *
 * Touch: tap deck → ReviewActivity, tap Import → DeckImportActivity,
 * tap Delete → ConfirmationActivity, then cascade-delete the selected deck
 */
class DeckListActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  std::vector<anki::DeckInfo> decks;
  bool loaded = false;
  int pendingDeleteIndex = -1;

  void loadDecks();
  void onDeckSelected(int index);
  void onImportTap();
  void onStatsTap();
  void onSettingsTap();
  void onDeleteTap();
  void onDeleteConfirmationResult(const ActivityResult& result);

 public:
  explicit DeckListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Anki Decks", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};

#endif  // ANKIEINK
