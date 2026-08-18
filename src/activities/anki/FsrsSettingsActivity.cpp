// Guarded so non-ANKIEINK builds never compile this translation unit, which
// would otherwise pull in lib/anki and its SQLite dependency.
#ifdef ANKIEINK

#include "FsrsSettingsActivity.h"
#include "ListTouch.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "FsrsConfigStore.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
enum ParamId : int {
  kRetention = 0,
  kNewLimit,
  kReviewLimit,
  kMaxInterval,
  kLearnSteps,
  kReset,
  kParamCount,
};

const char* paramTitle(int index) {
  switch (index) {
    case kRetention:
      return tr(STR_ANKI_FSRS_RETENTION);
    case kNewLimit:
      return tr(STR_ANKI_FSRS_NEW_LIMIT);
    case kReviewLimit:
      return tr(STR_ANKI_FSRS_REVIEW_LIMIT);
    case kMaxInterval:
      return tr(STR_ANKI_FSRS_MAX_INTERVAL);
    case kLearnSteps:
      return tr(STR_ANKI_FSRS_LEARN_STEPS);
    default:
      return tr(STR_ANKI_FSRS_RESET);
  }
}

// Editing step per parameter.
float paramStep(int index) {
  switch (index) {
    case kRetention:
      return 0.01f;
    case kMaxInterval:
      return 10.0f;
    default:
      return 1.0f;
  }
}

float paramValue(const fsrs::FsrsConfig& cfg, int index) {
  switch (index) {
    case kRetention:
      return cfg.requestRetention;
    case kNewLimit:
      return static_cast<float>(cfg.dailyNewLimit);
    case kReviewLimit:
      return static_cast<float>(cfg.dailyReviewLimit);
    case kMaxInterval:
      return static_cast<float>(cfg.maximumInterval);
    case kLearnSteps:
      return static_cast<float>(cfg.learnStepCount);
    default:
      return 0.0f;
  }
}

void setParamValue(fsrs::FsrsConfig& cfg, int index, float value) {
  switch (index) {
    case kRetention:
      cfg.requestRetention = std::clamp(value, 0.70f, 0.99f);
      break;
    case kNewLimit:
      cfg.dailyNewLimit = static_cast<uint16_t>(std::clamp(value, 0.0f, 999.0f));
      break;
    case kReviewLimit:
      cfg.dailyReviewLimit = static_cast<uint16_t>(std::clamp(value, 0.0f, 999.0f));
      break;
    case kMaxInterval:
      cfg.maximumInterval = static_cast<uint32_t>(std::clamp(value, 1.0f, 36500.0f));
      break;
    case kLearnSteps:
      cfg.setLearnStepPreset(static_cast<uint8_t>(std::clamp(value, 2.0f, 3.0f)));
      break;
    default:
      break;
  }
}

std::string paramText(const fsrs::FsrsConfig& cfg, int index) {
  char buf[24];
  switch (index) {
    case kRetention:
      snprintf(buf, sizeof(buf), "%.0f%%", cfg.requestRetention * 100.0f);
      return std::string(buf);
    case kNewLimit:
      snprintf(buf, sizeof(buf), "%u", cfg.dailyNewLimit);
      return std::string(buf);
    case kReviewLimit:
      snprintf(buf, sizeof(buf), "%u", cfg.dailyReviewLimit);
      return std::string(buf);
    case kMaxInterval:
      snprintf(buf, sizeof(buf), "%u d", cfg.maximumInterval);
      return std::string(buf);
    case kLearnSteps: {
      // "1m 10m" (2 steps) or "1m 6m 10m" (3 steps)
      std::string text;
      for (uint8_t i = 0; i < cfg.learnStepCount; ++i) {
        char step[8];
        snprintf(step, sizeof(step), "%dm", static_cast<int>(cfg.learnSteps[i]));
        if (!text.empty()) text += " ";
        text += step;
      }
      return text;
    }
    default:
      return "";
  }
}
}  // namespace

void FsrsSettingsActivity::onEnter() {
  Activity::onEnter();
  loadConfig();
  requestUpdate();  // replaceActivity() does not auto-render
}

void FsrsSettingsActivity::onExit() { Activity::onExit(); }

void FsrsSettingsActivity::loadConfig() { config = anki::FsrsConfigStore::getInstance().get(); }

void FsrsSettingsActivity::saveConfig() { anki::FsrsConfigStore::getInstance().set(config); }

void FsrsSettingsActivity::onOptimizeTap() {
  // TODO: Phase 2 — FsrsOptimizer over review_logs, then persist new weights.
}

void FsrsSettingsActivity::onResetDefaults() {
  anki::FsrsConfigStore::getInstance().resetDefaults();
  loadConfig();
  editing = false;
  requestUpdate();
}

void FsrsSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (editing) {
      editing = false;  // discard the in-progress edit
      requestUpdate();
    } else {
      activityManager.goToAnkiDecks();
    }
    return;
  }

  if (editing) {
    buttonNavigator.onNext([this] {
      editValue += paramStep(selectedIndex);
      setParamValue(config, selectedIndex, editValue);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this] {
      editValue -= paramStep(selectedIndex);
      setParamValue(config, selectedIndex, editValue);
      requestUpdate();
    });
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      editing = false;
      saveConfig();
      requestUpdate();
    }
    return;
  }

  auto activateSelected = [this] {
    if (selectedIndex == kReset) {
      onResetDefaults();
      return;
    }
    editing = true;
    editValue = paramValue(config, selectedIndex);
    requestUpdate();
  };

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    activateSelected();
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight -
                            metrics.verticalSpacing * 2;
  switch (anki_list_touch::handle(*this, mappedInput, selectedIndex, kParamCount, contentTop, contentHeight, false)) {
    case anki_list_touch::Result::Activated:
      activateSelected();
      return;
    case anki_list_touch::Result::Consumed:
      return;
    case anki_list_touch::Result::None:
      break;
  }

  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, kParamCount);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, kParamCount);
    requestUpdate();
  });
}

void FsrsSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_ANKI_FSRS_TITLE));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight}, kParamCount, selectedIndex,
               [](int index) -> std::string { return paramTitle(index); }, nullptr, nullptr,
               [this](int index) -> std::string {
                 // Editing row gets an arrow marker so the active param is
                 // obvious while its value changes.
                 if (editing && index == selectedIndex) {
                   return "→ " + paramText(config, index);
                 }
                 return paramText(config, index);
               },
               editing /* highlightValue */);

  const auto labels = editing ? mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "-", "+")
                              : mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

#endif  // ANKIEINK
