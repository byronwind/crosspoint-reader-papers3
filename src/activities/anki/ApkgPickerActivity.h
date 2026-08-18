#pragma once

#ifdef ANKIEINK

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * ApkgPickerActivity — Lists .apkg files found on the SD card for import.
 *
 * Scans /AnkiEInk/apkg/ plus the SD root for *.apkg files; selecting one
 * navigates to DeckImportActivity.
 *
 * Layout:
 *   ┌─────────────────────────────────────┐
 *   │  Select APKG                        │  ← Header
 *   ├─────────────────────────────────────┤
 *   │  > English.apkg                     │
 *   │    1.2 MB                           │
 *   │  > Japanese.apkg                    │
 *   │    860 KB                           │
 *   │  ...                                │
 *   ├─────────────────────────────────────┤
 *   │  [Back] [Select] [Up] [Down]        │  ← Button hints
 *   └─────────────────────────────────────┘
 */
class ApkgPickerActivity final : public Activity {
  struct ApkgFile {
    std::string path;
    std::string name;
    uint64_t size = 0;
  };

  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  std::vector<ApkgFile> apkgFiles;

  void scanForApkg();
  // Collect *.apkg files from a directory (class-private so it can access ApkgFile).
  static void collectApkgFromDir(const char* dir, std::vector<ApkgFile>& out);

 public:
  explicit ApkgPickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Select APKG", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};

#endif  // ANKIEINK
