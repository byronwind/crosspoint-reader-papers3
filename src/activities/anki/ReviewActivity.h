#pragma once

#ifdef ANKIEINK

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "DeckStore.h"
#include "FsrsScheduler.h"
#include "util/ButtonNavigator.h"

/**
 * ReviewActivity — Core review session interface.
 *
 * Layout (reviewing):
 *   ┌─────────────────────────────────────┐
 *   │  Deck Name    12/45    [Undo]       │  ← Status bar (40px)
 *   ├─────────────────────────────────────┤
 *   │                                     │
 *   │     Card front/back content         │  ← Content area (440px)
 *   │     (rendered via HtmlToEpd)        │
 *   │                                     │
 *   ├─────────────────────────────────────┤
 *   │  [Again] [Hard] [Good] [Easy]       │  ← Rating bar (60px)
 *   │   1min   6min   10d    30d         │  ← Interval preview
 *   └─────────────────────────────────────┘
 *
 * States: Front → Back → (rate) → Next card
 * Supports: undo last rating, undo session, curtain animation on flip
 */
class ReviewActivity final : public Activity {
  ButtonNavigator buttonNavigator;

  // Session state
  uint32_t deckId = 0;
  std::string deckName;
  std::vector<uint32_t> reviewQueue;  // Ordered card IDs for this session
  int currentIndex = 0;
  int completedCount = 0;
  int totalDue = 0;

  // Current card state
  anki::CardInfo currentCard;
  fsrs::CardState currentFsrsState;
  bool showingFront = true;   // true = front, false = back
  bool showRatingButtons = false;
  int ratingSelection = 2;    // 0=Again 1=Hard 2=Good 3=Easy

  // Undo support
  struct UndoSnapshot {
    uint32_t cardId;
    fsrs::CardState previousState;
    int previousIndex;
  };
  UndoSnapshot lastUndo;
  bool canUndo = false;

  // Curtain animation
  bool curtainAnimating = false;
  int curtainProgress = 0;  // 0..100

  // Rating interval previews (computed before display)
  float intervalAgain = 0;
  float intervalHard = 0;
  float intervalGood = 0;
  float intervalEasy = 0;

  // Long-card vertical scrolling: content scrolls inside the fixed centered
  // card frame. cardContentHeight is HtmlToEpd's measured full height.
  int cardScrollY = 0;
  int cardContentHeight = 0;
  bool cardScrollable = false;
  int maxCardScroll() const;
  void measureCardContent();
  void scrollCardBy(int delta);

  void loadNextCard();
  void computePreviewIntervals();
  void onRate(fsrs::Rating rating);
  void onUndo();
  void onFlip();
  void animateCurtainFlip();
  void renderStatusBar(RenderLock& lock);
  void renderCardContent(RenderLock& lock);
  void renderRatingBar(RenderLock& lock);
  void renderSessionComplete(RenderLock& lock);

 public:
  explicit ReviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, uint32_t deckId,
                          std::string deckName)
      : Activity("Review", renderer, mappedInput), deckId(deckId), deckName(std::move(deckName)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};

#endif  // ANKIEINK
