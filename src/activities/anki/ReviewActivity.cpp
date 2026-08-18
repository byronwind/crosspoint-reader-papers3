// Guarded so non-ANKIEINK builds never compile this translation unit, which
// would otherwise pull in lib/anki and its SQLite dependency.
#ifdef ANKIEINK

#include "ReviewActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <vector>

#include "AnkiPaths.h"
#include "FsrsConfigStore.h"
#include "FsrsQueue.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "HtmlToEpd.h"

namespace {
// Layout bands (portrait UI; heights in px).
constexpr int kStatusBarHeight = 40;
constexpr int kRatingBarHeight = 56;
constexpr int kIntervalPreviewHeight = 22;
constexpr int kContentPadding = 8;
constexpr int kCardMargin = 24;   // centered card frame side margin
constexpr int kCardPadding = 16;  // inner padding (must match HtmlToEpd measure)

// "0.5" days -> "12min"; "10" -> "10d"; "45" -> "1.5mo"
std::string formatInterval(float days) {
  char buf[24];
  if (days < 1.0f) {
    snprintf(buf, sizeof(buf), "%dmin", static_cast<int>(days * 1440.0f + 0.5f));
  } else if (days < 60.0f) {
    snprintf(buf, sizeof(buf), "%dd", static_cast<int>(days + 0.5f));
  } else {
    snprintf(buf, sizeof(buf), "%.1fmo", days / 30.0f);
  }
  return std::string(buf);
}

const char* ratingLabel(int index) {
  switch (index) {
    case 0:
      return tr(STR_ANKI_RATE_AGAIN);
    case 1:
      return tr(STR_ANKI_RATE_HARD);
    case 2:
      return tr(STR_ANKI_RATE_GOOD);
    default:
      return tr(STR_ANKI_RATE_EASY);
  }
}

// Cards render with useFallbackFont, so every character (Latin included)
// routes through one SD font. A card carries far more unique codepoints than
// the 8-slot overflow ring can hold, so without prewarming every redraw —
// and every scroll frame — re-reads evicted glyphs one-by-one from SD.
// Batch-load the card's glyphs into the font's mini cache once per card/face
// change; the mini cache persists across redraws until Review exits.
void prewarmCardFonts(GfxRenderer& renderer, const std::string& html) {
  const int cardFont = renderer.getFallbackFontId(NOTOSANS_18_FONT_ID);
  auto* fcm = renderer.getFontCacheManager();
  if (cardFont == 0 || !renderer.isSdCardFont(cardFont) || !fcm) return;
  std::string text = anki::HtmlToEpd::stripTags(html);
  // wrappedText() truncates its last line with an ellipsis; include it so the
  // truncation's width measurement doesn't trigger an on-demand glyph load.
  text += " \xE2\x80\xA6";
  fcm->prewarmCache(cardFont, text.c_str(), 0x01);
  // h4-h6 headings resolve to the 14px family instead of the base 18px one.
  const int headingFont = renderer.getFallbackFontId(NOTOSANS_14_FONT_ID);
  if (headingFont != 0 && headingFont != cardFont && html.find("<h") != std::string::npos) {
    fcm->prewarmCache(headingFont, text.c_str(), 0x01);
  }
}

// Display-time separators for glued card markup (the imported DB stays
// untouched):
//  1. Deck fields glue the headword's closing bracket straight onto the
//     definition ("【bereave】v.剥夺..."), which reads as one run-on blob.
//  2. The WordNet block glues the headword span onto the gloss
//     (<span class="hd">evoke</span>call forth...); the deck's CSS used to
//     style .hd apart, which this renderer drops.
// Insert a space at those boundaries unless whitespace/a tag already follows.
std::string addCardSeparators(const std::string& html) {
  std::string out;
  out.reserve(html.size() + 32);
  for (size_t i = 0; i < html.size(); ++i) {
    out.push_back(html[i]);
    const unsigned char c = static_cast<unsigned char>(html[i]);
    // // '】' = U+3011 = E3 80 91
    // if (c == 0xE3 && i + 3 < html.size() && html[i + 1] == '\x80' && html[i + 2] == '\x91') {
    //   const char next = html[i + 3];
    //   if (next != ' ' && next != '\t' && next != '\n' && next != '\r' && next != '<') {
    //     out.push_back(' ');
    //   }
    //   continue;
    // }
    // "</span>" immediately followed by a word character or multibyte (CJK)
    // start: the span's styling used to provide the visual separation.
    if (c == '<' && html.compare(i, 7, "</span>") == 0) {
      out.append("/span>");  // '<' was already pushed above
      i += 6;                      // loop's ++i lands on the char after '>'
      if (i + 1 < html.size()) {
        const unsigned char next = static_cast<unsigned char>(html[i + 1]);
        const bool wordChar = (next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') ||
                              (next >= '0' && next <= '9') || next >= 0x80;
        if (wordChar) out.push_back(' ');
      }
    }
  }
  return out;
}
}  // namespace

void ReviewActivity::onEnter() {
  Activity::onEnter();
  const uint32_t now = static_cast<uint32_t>(time(nullptr));

  // Session queue via FsrsQueue: due reviews (daily limit) plus new cards
  // (daily limit). getDueCards() is global, so due cards are filtered by deck
  // here; FsrsQueue applies the configured limits.
  const fsrs::FsrsConfig cfg = anki::FsrsConfigStore::getInstance().get();
  fsrs::FsrsQueue queue(cfg);
  std::vector<uint32_t> deckDue;
  for (uint32_t id : ANKI_STORE.getDueCards(now, 200)) {
    const auto card = ANKI_STORE.getCard(id);
    if (card.id != 0 && card.deckId == deckId) {
      deckDue.push_back(id);
    }
  }
  const auto session = queue.buildSession(now, deckDue, ANKI_STORE.getNewCards(deckId, 999));
  reviewQueue = session.newIds;
  reviewQueue.insert(reviewQueue.end(), session.reviewIds.begin(), session.reviewIds.end());

  totalDue = static_cast<int>(reviewQueue.size());
  completedCount = 0;
  currentIndex = 0;
  loadNextCard();
  requestUpdate();  // replaceActivity() does not auto-render
}

void ReviewActivity::onExit() { Activity::onExit(); }

void ReviewActivity::loadNextCard() {
  if (currentIndex >= static_cast<int>(reviewQueue.size())) {
    // Session complete (or the deck had nothing to review).
    currentCard = {};
    currentFsrsState = {};
    showingFront = true;
    showRatingButtons = false;
    requestUpdate();
    return;
  }
  currentCard = ANKI_STORE.getCard(reviewQueue[currentIndex]);
  currentFsrsState = ANKI_STORE.getCardState(reviewQueue[currentIndex]);
  showingFront = true;
  showRatingButtons = false;
  ratingSelection = 2;  // default highlight on Good
  computePreviewIntervals();
  cardScrollY = 0;
  measureCardContent();
  requestUpdate();
}

void ReviewActivity::computePreviewIntervals() {
  const fsrs::FsrsScheduler scheduler(anki::FsrsConfigStore::getInstance().get());
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  intervalAgain = scheduler.schedule(currentFsrsState, fsrs::Rating::Again, now).interval;
  intervalHard = scheduler.schedule(currentFsrsState, fsrs::Rating::Hard, now).interval;
  intervalGood = scheduler.schedule(currentFsrsState, fsrs::Rating::Good, now).interval;
  intervalEasy = scheduler.schedule(currentFsrsState, fsrs::Rating::Easy, now).interval;
}

void ReviewActivity::onRate(fsrs::Rating rating) {
  if (currentIndex >= static_cast<int>(reviewQueue.size())) {
    return;
  }

  // Snapshot for Phase-2 undo (M8).
  lastUndo.cardId = currentCard.id;
  lastUndo.previousState = currentFsrsState;
  lastUndo.previousIndex = currentIndex;
  canUndo = true;

  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  const fsrs::FsrsScheduler scheduler(anki::FsrsConfigStore::getInstance().get());
  const fsrs::SchedulingResult res = scheduler.schedule(currentFsrsState, rating, now);

  // Refresh elapsed days so the next retrievability computation is accurate.
  fsrs::CardState prev = currentFsrsState;
  prev.elapsedDays = currentFsrsState.lastReview > 0
                         ? static_cast<float>(now - currentFsrsState.lastReview) / 86400.0f
                         : 0.0f;

  ANKI_STORE.updateCardState(currentCard.id, res.newState);
  ANKI_STORE.logReview(currentCard.id, rating, prev.elapsedDays, prev, res.newState);
  ANKI_STORE.updateDailyStats(rating, 0, prev.state == fsrs::State::New);

  completedCount++;
  currentIndex++;
  loadNextCard();
}

void ReviewActivity::onUndo() {
  // TODO: Phase 2 (M8) — restore lastUndo.previousState and jump back to
  // lastUndo.previousIndex.
}

void ReviewActivity::onFlip() {
  if (!showingFront || currentIndex >= static_cast<int>(reviewQueue.size())) {
    return;
  }
  showingFront = false;
  showRatingButtons = true;
  ratingSelection = 2;
  cardScrollY = 0;
  measureCardContent();
  requestUpdate();
}

void ReviewActivity::animateCurtainFlip() {
  // TODO: Phase 2 (M7) — curtain wipe animation; flipping is instant for MVP.
}

void ReviewActivity::loop() {
  const bool sessionOver = currentIndex >= static_cast<int>(reviewQueue.size());

  // Session complete / empty deck: any key returns to the deck list.
  if (sessionOver) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      activityManager.goToAnkiDecks();
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    // Rated cards are persisted immediately, so leaving mid-session is safe.
    activityManager.goToAnkiDecks();
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  if (showingFront) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      onFlip();
      return;
    }
    const int contentHeight = pageHeight - kStatusBarHeight - metrics.buttonHintsHeight - kRatingBarHeight -
                              kIntervalPreviewHeight - kContentPadding;
    const int scrollStep = std::max(contentHeight / 2, 32);
    // Side Up/Down and vertical swipes scroll long cards; taps anywhere flip.
    if (cardScrollable) {
      if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
        scrollCardBy(scrollStep);
        return;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
        scrollCardBy(-scrollStep);
        return;
      }
      const auto swipe = mappedInput.wasSwipe();
      if (swipe == MappedInputManager::SwipeDir::Up) {
        scrollCardBy(scrollStep);
        return;
      }
      if (swipe == MappedInputManager::SwipeDir::Down) {
        scrollCardBy(-scrollStep);
        return;
      }
    }
    // Tap anywhere in the content area flips the card.
    if (mappedInput.wasTapInRect(0, kStatusBarHeight, pageWidth, contentHeight)) {
      onFlip();
    }
    return;
  }

  // Back side: move the rating highlight, then confirm.
  buttonNavigator.onNext([this] {
    ratingSelection = ButtonNavigator::nextIndex(ratingSelection, 4);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    ratingSelection = ButtonNavigator::previousIndex(ratingSelection, 4);
    requestUpdate();
  });
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    onRate(static_cast<fsrs::Rating>(ratingSelection + 1));
    return;
  }
  const int contentHeight = pageHeight - kStatusBarHeight - metrics.buttonHintsHeight - kRatingBarHeight -
                            kIntervalPreviewHeight - kContentPadding;
  const int scrollStep = std::max(contentHeight / 2, 32);
  // Side buttons are taken by the rating bar, so long cards scroll via
  // vertical swipes or taps on the left/right card edges.
  if (cardScrollable) {
    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up) {
      scrollCardBy(scrollStep);
      return;
    }
    if (swipe == MappedInputManager::SwipeDir::Down) {
      scrollCardBy(-scrollStep);
      return;
    }
    constexpr int kEdgeTapWidth = 40;
    if (mappedInput.wasTapInRect(0, kStatusBarHeight, kEdgeTapWidth, contentHeight)) {
      scrollCardBy(-scrollStep);
      return;
    }
    if (mappedInput.wasTapInRect(pageWidth - kEdgeTapWidth, kStatusBarHeight, kEdgeTapWidth, contentHeight)) {
      scrollCardBy(scrollStep);
      return;
    }
  }
  // Tap a rating bar quarter to rate directly.
  const int barY = pageHeight - metrics.buttonHintsHeight - kRatingBarHeight;
  for (int i = 0; i < 4; ++i) {
    if (mappedInput.wasTapInRect(i * pageWidth / 4, barY, pageWidth / 4, kRatingBarHeight)) {
      onRate(static_cast<fsrs::Rating>(i + 1));
      return;
    }
  }
}

void ReviewActivity::render(RenderLock&& lock) {
  const bool sessionOver = currentIndex >= static_cast<int>(reviewQueue.size());
  const bool showRate = showRatingButtons && !sessionOver;
  const auto labels = mappedInput.mapLabels(
      tr(STR_BACK), sessionOver ? "" : (showingFront ? tr(STR_ANKI_SHOW_ANSWER) : tr(STR_SELECT)),
      showRate ? tr(STR_DIR_UP) : "", showRate ? tr(STR_DIR_DOWN) : "");

  // SD mini caches rebuild wholesale when the requested codepoint set is not
  // covered, and a rebuild evicts everything loaded before — so batch all
  // strings of one font size into a single prewarm call, before any drawing.
  // (The activity opens with requestUpdate() rendering before the first
  // measureCardContent(), so chrome prewarming must live here.) Without this,
  // the chrome glyphs fall through to the 8-slot overflow ring every render.
  // '…' is truncatedText()'s ellipsis; the button hints draw with
  // SMALL_FONT_ID, not UI_10.
  std::string ui10Text = deckName;
  ui10Text += " \xE2\x80\xA6";
  if (sessionOver) {
    ui10Text += ' ';
    ui10Text += tr(STR_ANKI_APKG_HINT);
    ui10Text += ' ';
    ui10Text += tr(STR_ANKI_SESSION_EXIT_HINT);
    renderer.prewarmText(NOTOSANS_18_FONT_ID,
                         totalDue == 0 ? tr(STR_ANKI_NO_CARDS) : tr(STR_ANKI_SESSION_DONE));
  }
  renderer.prewarmText(UI_10_FONT_ID, ui10Text.c_str());
  std::string hintText;
  for (const char* label : {labels.btn1, labels.btn2, labels.btn3, labels.btn4}) {
    if (label[0] != '\0') {
      if (!hintText.empty()) hintText += ' ';
      hintText += label;
    }
  }
  hintText += " \xE2\x80\xA6";  // truncated hint labels end in an ellipsis
  renderer.prewarmText(SMALL_FONT_ID, hintText.c_str());
  std::string ui12Text;
  for (int i = 0; i < 4; ++i) {
    if (i > 0) ui12Text += ' ';
    ui12Text += ratingLabel(i);
  }
  renderer.prewarmText(UI_12_FONT_ID, ui12Text.c_str());

  renderer.clearScreen();

  if (sessionOver) {
    renderSessionComplete(lock);
  } else {
    renderStatusBar(lock);
    renderCardContent(lock);
    if (showRatingButtons) {
      renderRatingBar(lock);
    }
  }

  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void ReviewActivity::renderStatusBar(RenderLock&) {
  const int pageWidth = renderer.getScreenWidth();
  char progress[24];
  snprintf(progress, sizeof(progress), "%d/%d", currentIndex + 1, totalDue);
  const int nameWidth = pageWidth - renderer.getTextWidth(UI_10_FONT_ID, progress) - 36;
  const std::string name = renderer.truncatedText(UI_10_FONT_ID, deckName.c_str(), nameWidth);
  renderer.drawText(UI_10_FONT_ID, 12, 14, name.c_str());
  renderer.drawText(UI_10_FONT_ID, pageWidth - renderer.getTextWidth(UI_10_FONT_ID, progress) - 12, 14, progress);
  renderer.drawLine(0, kStatusBarHeight, pageWidth, kStatusBarHeight);
}

int ReviewActivity::maxCardScroll() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageHeight = renderer.getScreenHeight();
  const int contentTop = kStatusBarHeight + kContentPadding;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - kRatingBarHeight -
                            kIntervalPreviewHeight - kContentPadding;
  return std::max(0, cardContentHeight - (contentHeight - 2 * kCardPadding));
}

void ReviewActivity::measureCardContent() {
  if (currentIndex >= static_cast<int>(reviewQueue.size())) {
    cardContentHeight = 0;
    cardScrollable = false;
    cardScrollY = 0;
    return;
  }
  const int pageWidth = renderer.getScreenWidth();
  anki::HtmlToEpd::FontConfig font;
  font.baseFontSize = 18;  // must match renderCardContent()
  const std::string html =
      showingFront ? currentCard.frontHtml : addCardSeparators(currentCard.backHtml);
  prewarmCardFonts(renderer, html);
  // Measure with the render engine itself: the old width/line-height estimate
  // ignored block-tag spacing (<br>, <p>, <li> gaps), underreporting the
  // height of definition-style backs and capping the scroll short of the
  // tail. x/width/padding/fallback font must match renderCardContent().
  anki::HtmlToEpd::Layout layout;
  layout.x = kCardMargin;
  layout.width = pageWidth - 2 * kCardMargin;
  layout.padding = kCardPadding;
  layout.useFallbackFont = true;
  cardContentHeight = anki::HtmlToEpd::measure(renderer, html, layout, font);
  cardScrollable = maxCardScroll() > 0;
  cardScrollY = std::min(cardScrollY, maxCardScroll());
}

void ReviewActivity::scrollCardBy(int delta) {
  const int max = maxCardScroll();
  const int prev = cardScrollY;
  cardScrollY = std::max(0, std::min(max, cardScrollY + delta));
  if (cardScrollY != prev) requestUpdate();
}

void ReviewActivity::renderCardContent(RenderLock&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int contentTop = kStatusBarHeight + kContentPadding;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - kRatingBarHeight -
                            kIntervalPreviewHeight - kContentPadding;

  // Centered card frame: fixed border, long content scrolls inside it.
  const int cardWidth = pageWidth - 2 * kCardMargin;
  renderer.fillRect(kCardMargin, contentTop, cardWidth, contentHeight, false);
  renderer.drawRect(kCardMargin, contentTop, cardWidth, contentHeight);

  anki::HtmlToEpd::Layout layout;
  layout.x = kCardMargin;
  layout.width = cardWidth;
  layout.height = contentHeight - 2 * kCardPadding;
  layout.padding = kCardPadding;
  // Front shows a single word — centered like a flashcard. The answer side
  // holds definition/example lines that read better left-aligned.
  layout.centerText = showingFront;
  // Whole card (Latin included) renders in the user-selected SD font family
  // instead of mixing the built-in Noto Sans with the CJK fallback.
  layout.useFallbackFont = true;
  // layout.y is the UNSCROLLED origin; the renderer keeps the visible box
  // fixed and skips lines scrolled out the top (subtracting cardScrollY here
  // used to push the whole box — and its clip bottom — up, letting lines leak
  // over the frame top).
  layout.y = contentTop + kCardPadding;
  layout.scrollY = cardScrollY;
  if (showingFront && !cardScrollable && cardContentHeight < layout.height) {
    // Vertically center the short question side; the answer side keeps a
    // normal top-aligned flow (like a dictionary entry).
    layout.y += (layout.height - cardContentHeight) / 2;
  }

  anki::HtmlToEpd::FontConfig font;
  font.baseFontSize = 18;  // card body slightly larger than UI text

  // Media references like "1.jpg" resolve under the shared media directory.
  auto resolver = [](const std::string& src) -> std::string {
    if (src.empty()) return {};
    if (src[0] == '/') return src;
    return std::string(anki::kAnkiMediaDir) + "/" + src;
  };

  const std::string html =
      showingFront ? currentCard.frontHtml : addCardSeparators(currentCard.backHtml);
  anki::HtmlToEpd::render(renderer, html, layout, font, resolver);

  // Scroll indicator at the right edge of the frame (only when scrollable).
  if (cardScrollable) {
    const int max = maxCardScroll();
    const int trackX = kCardMargin + cardWidth - 7;
    const int trackTop = contentTop + 8;
    const int trackH = contentHeight - 16;
    renderer.drawLine(trackX, trackTop, trackX, trackTop + trackH);
    const int viewH = contentHeight - 2 * kCardPadding;
    const int thumbH = std::max(24, trackH * viewH / cardContentHeight);
    const int thumbY = trackTop + (trackH - thumbH) * cardScrollY / max;
    renderer.fillRect(trackX - 1, thumbY, 3, thumbH);
  }
}

void ReviewActivity::renderRatingBar(RenderLock&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int barY = pageHeight - metrics.buttonHintsHeight - kRatingBarHeight;

  // Interval previews above the bar, one per rating, centered on each button.
  // drawText y is the text top; place it one full line above the bar with a
  // small gap so the descender never touches the buttons (PaperS3's UI_10 is
  // 12pt, so the old fixed +6px offset collided with the bar top).
  const std::vector<std::string> previews = {formatInterval(intervalAgain), formatInterval(intervalHard),
                                             formatInterval(intervalGood), formatInterval(intervalEasy)};
  const int previewY = barY - renderer.getLineHeight(UI_10_FONT_ID) - 4;
  for (int i = 0; i < 4; ++i) {
    const int tw = renderer.getTextWidth(UI_10_FONT_ID, previews[i].c_str());
    const int cx = (pageWidth * (2 * i + 1)) / 8;
    renderer.drawText(UI_10_FONT_ID, cx - tw / 2, previewY, previews[i].c_str());
  }

  GUI.drawButtonMenu(renderer, Rect{8, barY, pageWidth - 16, kRatingBarHeight - 4}, 4, ratingSelection,
                     [](int index) { return std::string(ratingLabel(index)); }, nullptr);
}

void ReviewActivity::renderSessionComplete(RenderLock&) {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int mid = pageHeight / 2;

  // CJK fallback fonts carry a much taller ascender than the Latin built-ins
  // (Noto Sans CJK ~1.16em vs ~0.75em), so fixed offsets between stacked
  // lines overlap. Space them by measured line height plus a fixed gap.
  const int titleH = renderer.getLineHeight(NOTOSANS_18_FONT_ID);
  const int hintH = renderer.getLineHeight(UI_10_FONT_ID);

  if (totalDue == 0) {
    renderer.drawCenteredText(NOTOSANS_18_FONT_ID, mid - titleH - 12, tr(STR_ANKI_NO_CARDS));
    renderer.drawCenteredText(UI_10_FONT_ID, mid + 8, tr(STR_ANKI_APKG_HINT));
    return;
  }
  renderer.drawCenteredText(NOTOSANS_18_FONT_ID, mid - titleH - 12, tr(STR_ANKI_SESSION_DONE));
  char buf[48];
  snprintf(buf, sizeof(buf), tr(STR_FMT_SESSION_STATS), completedCount, totalDue);
  renderer.drawCenteredText(UI_10_FONT_ID, mid + 8, buf);
  renderer.drawCenteredText(UI_10_FONT_ID, mid + 8 + hintH + 8, tr(STR_ANKI_SESSION_EXIT_HINT));
}

#endif  // ANKIEINK
