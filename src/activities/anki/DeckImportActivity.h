#pragma once

#ifdef ANKIEINK

#include <atomic>
#include <string>

#include "activities/Activity.h"
#include "ApkgImporter.h"
#include "DeckStore.h"

/**
 * DeckImportActivity — APKG import progress screen.
 *
 * Shows a progress bar and status text during import.
 * Runs the import on a background task to keep UI responsive.
 *
 * Layout:
 *   ┌─────────────────────────────────────┐
 *   │  Importing Deck                     │
 *   ├─────────────────────────────────────┤
 *   │                                     │
 *   │  Deck Name: English Vocabulary      │
 *   │                                     │
 *   │  ████████████░░░░░░░░  65%          │
 *   │  Importing cards: 650/1000          │
 *   │                                     │
 *   │  Media files: 45 copied             │
 *   │                                     │
 *   ├─────────────────────────────────────┤
 *   │  [Cancel]                           │
 *   └─────────────────────────────────────┘
 */
class DeckImportActivity final : public Activity {
  std::string apkgPath;
  anki::DeckStore& store;
  std::string sdMediaDir;

  // Import state
  bool importing = false;
  bool importComplete = false;
  bool importFailed = false;
  uint32_t progressCurrent = 0;
  uint32_t progressTotal = 0;
  std::string progressPhase;
  anki::ApkgImporter::ImportResult result;

  // Written by the background import task, read by the main task:
  // taskDone (release/acquire) publishes the writes to the other members.
  std::atomic<bool> taskDone{false};
  std::atomic<int> phaseCode{0};
  int currentPhaseCode = 0;  // last phase shown (main task only)

  void startImport();
  void onImportProgress(uint32_t current, uint32_t total, const char* phase);
  void onImportDone(const anki::ApkgImporter::ImportResult& result);
  void onCancel();
  static void importTaskTrampoline(void* arg);

 public:
  explicit DeckImportActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                              std::string apkgPath, anki::DeckStore& store, std::string sdMediaDir)
      : Activity("Import", renderer, mappedInput),
        apkgPath(std::move(apkgPath)),
        store(store),
        sdMediaDir(std::move(sdMediaDir)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // Keep the CPU at full speed while the import task runs; power saving would
  // slow SD/SQLite I/O so much that the watchdog could trip (the import task
  // only yields every N rows/batches).
  bool preventAutoSleep() override { return importing; }
};

#endif  // ANKIEINK
