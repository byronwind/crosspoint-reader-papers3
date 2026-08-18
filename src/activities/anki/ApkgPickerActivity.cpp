// Guarded so non-ANKIEINK builds never compile this translation unit, which
// would otherwise pull in lib/anki and its SQLite dependency.
#ifdef ANKIEINK

#include "ApkgPickerActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <cstdio>

#include "AnkiPaths.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// "1234567" -> "1.2 MB" / "860 KB" / "512 B"
std::string formatSize(uint64_t bytes) {
  char buf[24];
  if (bytes >= 1024 * 1024) {
    snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024) {
    snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
  }
  return std::string(buf);
}
}  // namespace

void ApkgPickerActivity::collectApkgFromDir(const char* dir, std::vector<ApkgPickerActivity::ApkgFile>& out) {
  auto dirHandle = Storage.open(dir);
  if (!dirHandle || !dirHandle.isDirectory()) {
    return;
  }
  dirHandle.rewindDirectory();
  for (auto file = dirHandle.openNextFile(); file; file = dirHandle.openNextFile()) {
    if (file.isDirectory()) {
      continue;
    }
    char name[128];
    file.getName(name, sizeof(name));
    if (!FsHelpers::checkFileExtension(std::string_view(name), ".apkg")) {
      continue;
    }
    out.push_back({std::string(dir) + "/" + name, name, file.fileSize()});
  }
  dirHandle.close();
}

void ApkgPickerActivity::onEnter() {
  Activity::onEnter();
  scanForApkg();
  selectedIndex = 0;
  requestUpdate();  // replaceActivity() does not auto-render
}

void ApkgPickerActivity::onExit() { Activity::onExit(); }

void ApkgPickerActivity::scanForApkg() {
  apkgFiles.clear();
  // The dedicated Anki folder first, then the SD root as a fallback.
  collectApkgFromDir((std::string(anki::kAnkiRootDir) + "/apkg").c_str(), apkgFiles);
  collectApkgFromDir("/", apkgFiles);
}

void ApkgPickerActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    activityManager.goToAnkiDecks();
    return;
  }

  auto activateSelected = [this] {
    if (!apkgFiles.empty() && selectedIndex >= 0 && selectedIndex < static_cast<int>(apkgFiles.size())) {
      activityManager.goToAnkiImport(apkgFiles[selectedIndex].path);
    }
  };

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    activateSelected();
    return;
  }

  const int itemCount = static_cast<int>(apkgFiles.size());
  if (itemCount > 0) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight =
        renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
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
}

void ApkgPickerActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_ANKI_PICK_APKG));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  const int itemCount = static_cast<int>(apkgFiles.size());

  if (itemCount == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 16, tr(STR_ANKI_NO_APKG));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 8, tr(STR_ANKI_APKG_HINT));
  } else {
    GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight}, itemCount, selectedIndex,
                 [this](int index) -> std::string { return apkgFiles[index].name; },
                 [this](int index) -> std::string { return formatSize(apkgFiles[index].size); });
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

#endif  // ANKIEINK
