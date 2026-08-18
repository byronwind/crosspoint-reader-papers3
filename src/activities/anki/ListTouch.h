#pragma once

#include <algorithm>
#include <cstdint>

#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "components/UITheme.h"

// Row hit-test helpers for the Anki list screens. Upstream moved its lists
// onto the UiListActivity/FreeInkUI row-action framework and removed the old
// rectangle-based list touch API from Activity/MappedInputManager; the Anki
// screens still draw their own GUI.drawList content, so they keep the
// equivalent behavior locally.
namespace anki_list_touch {

enum class Result : uint8_t {
  None,      // touch didn't hit a row
  Consumed,  // press landed on a row (selection follows)
  Activated  // tap released on a row
};

inline bool listItemFromPoint(const int x, const int y, int& index, const int itemCount, const int selectedIndex,
                              const int listTop, const int listHeight, const bool hasSubtitle) {
  (void)x;
  if (itemCount <= 0) return false;
  if (y < listTop || y >= listTop + listHeight) return false;

  const auto& theme = UITheme::getInstance().getTheme();
  const int rowStep = theme.getListRowStep(hasSubtitle);
  if (rowStep <= 0) return false;

  const int pageItems = theme.getListPageItems(listHeight, hasSubtitle);
  if (pageItems <= 0) return false;
  const int pageStart = std::max(0, selectedIndex / pageItems) * pageItems;
  const int row = (y - listTop) / rowStep;
  const int tapped = pageStart + row;
  if (row < 0 || row >= pageItems || tapped >= itemCount) return false;
  index = tapped;
  return true;
}

inline Result handle(Activity& activity, MappedInputManager& mappedInput, int& selectedIndex, const int itemCount,
                     const int listTop, const int listHeight, const bool hasSubtitle) {
  int touched = -1;
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTouchDown(tx, ty) &&
      listItemFromPoint(tx, ty, touched, itemCount, selectedIndex, listTop, listHeight, hasSubtitle)) {
    if (selectedIndex != touched) {
      selectedIndex = touched;
      activity.requestUpdate();
    }
    return Result::Consumed;
  }
  if (mappedInput.wasScreenTapped(tx, ty) &&
      listItemFromPoint(tx, ty, touched, itemCount, selectedIndex, listTop, listHeight, hasSubtitle)) {
    selectedIndex = touched;
    return Result::Activated;
  }
  return Result::None;
}

}  // namespace anki_list_touch
