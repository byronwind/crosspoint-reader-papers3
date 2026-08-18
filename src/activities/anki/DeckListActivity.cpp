// Guarded so non-ANKIEINK builds never compile this translation unit, which
// would otherwise pull in lib/anki and its SQLite dependency.
#ifdef ANKIEINK

#include "DeckListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>
#include <memory>

#include "activities/ActivityManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Action bar (Import APKG button) geometry, between the list and button hints.
constexpr int kActionBarHeight = 44;
constexpr int kActionBarMargin = 16;

// "120 cards · 5 new" (localized via STR_FMT_DECK_SUMMARY)
std::string deckSummary(const anki::DeckInfo& deck) {
  char buf[64];
  snprintf(buf, sizeof(buf), tr(STR_FMT_DECK_SUMMARY), deck.cardCount, deck.newCount);
  return std::string(buf);
}
}  // namespace

void DeckListActivity::onEnter() {
  Activity::onEnter();
  loadDecks();
  requestUpdate();  // replaceActivity() does not auto-render
}

void DeckListActivity::onExit() { Activity::onExit(); }

void DeckListActivity::loadDecks() {
  decks = ANKI_STORE.listDecks();
  if (selectedIndex >= static_cast<int>(decks.size())) {
    selectedIndex = decks.empty() ? 0 : static_cast<int>(decks.size()) - 1;
  }
  loaded = true;
}

void DeckListActivity::loop() {
  auto activateSelected = [this] { onDeckSelected(selectedIndex); };

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    activityManager.goHome();
    return;
  }

  const int itemCount = static_cast<int>(decks.size());

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (itemCount > 0) {
      activateSelected();
    } else {
      onImportTap();
    }
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight -
                            metrics.verticalSpacing * 2 - kActionBarHeight - metrics.verticalSpacing;

  if (itemCount > 0) {
    switch (handleListTouch(selectedIndex, itemCount, contentTop, contentHeight, true)) {
      case ListTouchResult::Activated:
        activateSelected();
        return;
      case ListTouchResult::Consumed:
        return;
      case ListTouchResult::None:
        break;
    }

    const int pageItems = GUI.getListPageItems(contentHeight, true);
    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up) {
      selectedIndex = ButtonNavigator::nextPageIndex(selectedIndex, itemCount, pageItems);
      requestUpdate();
      return;
    }
    if (swipe == MappedInputManager::SwipeDir::Down) {
      selectedIndex = ButtonNavigator::previousPageIndex(selectedIndex, itemCount, pageItems);
      requestUpdate();
      return;
    }

    buttonNavigator.onNext([this, itemCount] {
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, itemCount);
      requestUpdate();
    });

    buttonNavigator.onPrevious([this, itemCount] {
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, itemCount);
      requestUpdate();
    });
  }

  // Action bar: [Import APKG], [Settings] and [Delete] sit between the list
  // and the button hints.
  const int actionBarY = renderer.getScreenHeight() - metrics.buttonHintsHeight - kActionBarHeight -
                         metrics.verticalSpacing;
  const int actionBarWidth = renderer.getScreenWidth() - kActionBarMargin * 2;
  const int buttonWidth = actionBarWidth / 3;
  if (mappedInput.wasTapInRect(kActionBarMargin, actionBarY, buttonWidth, kActionBarHeight)) {
    onImportTap();
  } else if (mappedInput.wasTapInRect(kActionBarMargin + buttonWidth, actionBarY, buttonWidth, kActionBarHeight)) {
    onSettingsTap();
  } else if (mappedInput.wasTapInRect(kActionBarMargin + buttonWidth * 2, actionBarY, actionBarWidth - buttonWidth * 2,
                                      kActionBarHeight)) {
    onDeleteTap();
  }
}

void DeckListActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_ANKI_DECKS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2 -
                            kActionBarHeight - metrics.verticalSpacing;
  const int itemCount = static_cast<int>(decks.size());

  if (itemCount == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, tr(STR_ANKI_NO_DECKS));
  } else {
    GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight}, itemCount, selectedIndex,
                 [this](int index) -> std::string { return decks[index].name; },
                 [this](int index) -> std::string { return deckSummary(decks[index]); });
  }

  // Action bar buttons
  const int actionBarY = pageHeight - metrics.buttonHintsHeight - kActionBarHeight - metrics.verticalSpacing;
  GUI.drawButtonMenu(renderer, Rect{kActionBarMargin, actionBarY, pageWidth - kActionBarMargin * 2, kActionBarHeight},
                     3, 0, [](int index) {
                       if (index == 0) return std::string(tr(STR_ANKI_IMPORT));
                       if (index == 1) return std::string(tr(STR_ANKI_SETTINGS));
                       return std::string(tr(STR_DELETE));
                     },
                     nullptr);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void DeckListActivity::onDeckSelected(int index) {
  if (index < 0 || index >= static_cast<int>(decks.size())) {
    return;
  }
  activityManager.goToAnkiReview(decks[index].id, decks[index].name);
}

void DeckListActivity::onImportTap() { activityManager.goToAnkiPickApkg(); }

void DeckListActivity::onStatsTap() {
  // TODO: Phase 2 — navigate to StatsActivity
}

void DeckListActivity::onSettingsTap() { activityManager.goToAnkiFsrsSettings(); }

void DeckListActivity::onDeleteTap() {
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(decks.size())) {
    return;
  }
  pendingDeleteIndex = selectedIndex;
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, std::string(tr(STR_DELETE)),
                                             decks[selectedIndex].name),
      [this](const ActivityResult& result) { onDeleteConfirmationResult(result); });
}

void DeckListActivity::onDeleteConfirmationResult(const ActivityResult& result) {
  if (!result.isCancelled && pendingDeleteIndex >= 0 && pendingDeleteIndex < static_cast<int>(decks.size())) {
    const anki::DeckInfo& deck = decks[pendingDeleteIndex];
    if (ANKI_STORE.deleteDeck(deck.id)) {
      LOG_DBG("AnkiDecks", "Deleted deck '%s' (id=%u)", deck.name.c_str(), deck.id);
    } else {
      LOG_ERR("AnkiDecks", "Failed to delete deck '%s' (id=%u)", deck.name.c_str(), deck.id);
    }
    loadDecks();
  }
  pendingDeleteIndex = -1;
  requestUpdate();
}

#endif  // ANKIEINK
