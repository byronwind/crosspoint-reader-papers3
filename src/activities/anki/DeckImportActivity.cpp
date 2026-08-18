// Guarded so non-ANKIEINK builds never compile this translation unit, which
// would otherwise pull in lib/anki and its SQLite dependency.
#ifdef ANKIEINK

#include "DeckImportActivity.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>
#include <cstring>

#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int kProgressBarHeight = 14;
constexpr int kProgressBarMargin = 40;
constexpr uint32_t kImportTaskStackSize = 16384;

// Localized label for the phase code written by onImportProgress().
const char* phaseLabel(int code) {
  switch (code) {
    case 1:
      return tr(STR_ANKI_PHASE_PARSING);
    case 2:
      return tr(STR_ANKI_PHASE_IMPORTING);
    case 3:
      return tr(STR_ANKI_PHASE_MEDIA);
    default:
      return tr(STR_ANKI_PHASE_EXTRACTING);
  }
}

// Mirror of the phase strings emitted by ApkgImporter::importApkg().
int phaseCodeOf(const char* phase) {
  if (phase == nullptr) return 0;
  if (std::strcmp(phase, "parsing") == 0) return 1;
  if (std::strcmp(phase, "importing") == 0) return 2;
  if (std::strcmp(phase, "media") == 0) return 3;
  return 0;
}
}  // namespace

void DeckImportActivity::startImport() {
  importing = true;
  importComplete = false;
  importFailed = false;
  progressCurrent = 0;
  progressTotal = 0;
  currentPhaseCode = 0;
  progressPhase = phaseLabel(0);
  taskDone.store(false, std::memory_order_relaxed);
  phaseCode.store(0, std::memory_order_relaxed);

  // Pin the import to core 0 so it never preempts the render task (core 1).
  const BaseType_t created =
      xTaskCreatePinnedToCore(&DeckImportActivity::importTaskTrampoline, "AnkiImport", kImportTaskStackSize, this,
                              1,  // Priority
                              nullptr, 0);
  if (created != pdPASS) {
    LOG_ERR("ANKI", "Failed to create import task");
    result.success = false;
    result.error = "Failed to create import task";
    importing = false;
    importFailed = true;
    requestUpdate();
  }
}

void DeckImportActivity::importTaskTrampoline(void* arg) {
  auto* self = static_cast<DeckImportActivity*>(arg);
  self->result = anki::ApkgImporter::importApkg(
      self->apkgPath, self->store, self->sdMediaDir,
      [self](uint32_t current, uint32_t total, const char* phase) {
        self->onImportProgress(current, total, phase);
      });
  // Release-publish the result writes so the main task can read them safely.
  self->taskDone.store(true, std::memory_order_release);
  self->requestUpdate();
  vTaskDelete(nullptr);
}

void DeckImportActivity::onImportProgress(uint32_t current, uint32_t total, const char* phase) {
  // Called on the background task; only atomics and scalar members are touched
  // here — the localized phase string is rebuilt by loop() on the main task.
  progressCurrent = current;
  progressTotal = total;
  phaseCode.store(phaseCodeOf(phase), std::memory_order_relaxed);
  requestUpdate();
}

void DeckImportActivity::onImportDone(const anki::ApkgImporter::ImportResult& res) {
  if (res.success) {
    importComplete = true;
  } else {
    importFailed = true;
    progressPhase = res.error.empty() ? tr(STR_ANKI_IMPORT_FAILED) : res.error;
  }
}

void DeckImportActivity::onCancel() {
  // ApkgImporter has no cancellation hook yet; the Back key is ignored while
  // importing so the background task can finish writing the database instead
  // of being orphaned mid-transaction.
}

void DeckImportActivity::onEnter() {
  Activity::onEnter();
  startImport();
  requestUpdate();  // replaceActivity() does not auto-render
}

void DeckImportActivity::onExit() { Activity::onExit(); }

void DeckImportActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (importing) {
      onCancel();
      return;
    }
    activityManager.goToAnkiDecks();
    return;
  }

  if (importing) {
    // Poll the background task; taskDone's acquire load orders the result
    // writes from importTaskTrampoline's release store.
    if (taskDone.load(std::memory_order_acquire)) {
      importing = false;
      onImportDone(result);
      requestUpdate();
      return;
    }
    // Refresh the localized phase label when the background task moved on.
    const int code = phaseCode.load(std::memory_order_relaxed);
    if (code != currentPhaseCode) {
      currentPhaseCode = code;
      progressPhase = phaseLabel(code);
      requestUpdate();
    }
    return;
  }

  // Done/failed: any confirm returns to the deck list.
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    activityManager.goToAnkiDecks();
    return;
  }
}

void DeckImportActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 importing ? tr(STR_ANKI_IMPORTING)
                           : (importComplete ? tr(STR_ANKI_IMPORT_DONE) : tr(STR_ANKI_IMPORT_FAILED)));

  const int centerY = pageHeight / 2 - 30;

  if (importing) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - 24, progressPhase.c_str());

    const int barX = kProgressBarMargin;
    const int barY = centerY + 8;
    const int barWidth = pageWidth - kProgressBarMargin * 2;
    renderer.drawRect(barX, barY, barWidth, kProgressBarHeight);
    if (progressTotal > 0) {
      const int fill =
          static_cast<int>(static_cast<uint64_t>(progressCurrent) * static_cast<uint32_t>(barWidth) / progressTotal);
      if (fill > 0) {
        renderer.fillRect(barX, barY, fill, kProgressBarHeight);
      }
      char buf[48];
      snprintf(buf, sizeof(buf), "%u / %u", progressCurrent, progressTotal);
      renderer.drawCenteredText(UI_10_FONT_ID, barY + kProgressBarHeight + 8, buf);
    }
  } else if (importComplete) {
    char buf[64];
    snprintf(buf, sizeof(buf), tr(STR_FMT_IMPORT_RESULT), result.cardsImported, result.mediaImported);
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - 12, buf);
  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - 12, progressPhase.c_str());
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

#endif  // ANKIEINK
