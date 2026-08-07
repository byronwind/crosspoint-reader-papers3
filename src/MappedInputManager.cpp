#include "MappedInputManager.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cstdlib>

#include "CrossPointSettings.h"
#include "components/UITheme.h"

bool MappedInputManager::isNavDirectionSwapped() const {
  // Key the swap on the orientation the screen is *actually* rendered at, not the persisted reader
  // setting. The reader (and its modal menus) render rotated, so navigation/labels flip there; the
  // home and settings UI render in portrait, so they never flip even when a rotated reader is configured.
  const auto orientation = renderer.getOrientation();
  return SETTINGS.frontButtonFollowOrientation &&
         (orientation == GfxRenderer::PortraitInverted || orientation == GfxRenderer::LandscapeCounterClockwise);
}

MappedInputManager::Button MappedInputManager::mapScreenDirection(const Button button) const {
  // Rows follow GfxRenderer::Orientation's declared order.
  static constexpr Button directions[][4] = {
      {Button::Left, Button::Right, Button::Up, Button::Down},
      {Button::Down, Button::Up, Button::Left, Button::Right},
      {Button::Right, Button::Left, Button::Down, Button::Up},
      {Button::Up, Button::Down, Button::Right, Button::Left},
  };

  uint8_t direction = 0;
  switch (button) {
    case Button::ScreenLeft:
      direction = 0;
      break;
    case Button::ScreenRight:
      direction = 1;
      break;
    case Button::ScreenUp:
      direction = 2;
      break;
    case Button::ScreenDown:
      direction = 3;
      break;
    default:
      return button;
  }

  const uint8_t orientation =
      SETTINGS.frontButtonFollowOrientation ? static_cast<uint8_t>(renderer.getOrientation()) : 0;
  return directions[orientation][direction];
}

bool MappedInputManager::mapButton(const Button button, bool (HalGPIO::*fn)(uint8_t) const) const {
  const auto sideLayout = SETTINGS.sideButtonLayout;

  switch (button) {
    case Button::Back:
      // Logical Back maps to user-configured front button.
      return (gpio.*fn)(SETTINGS.frontButtonBack);
    case Button::Confirm:
      // Logical Confirm maps to user-configured front button.
      return (gpio.*fn)(SETTINGS.frontButtonConfirm);
    case Button::Left:
      // Logical Left maps to user-configured front button.
      return (gpio.*fn)(SETTINGS.frontButtonLeft);
    case Button::Right:
      // Logical Right maps to user-configured front button.
      return (gpio.*fn)(SETTINGS.frontButtonRight);
    case Button::Up:
      // Side buttons remain fixed for Up/Down.
      return (gpio.*fn)(HalGPIO::BTN_UP);
    case Button::Down:
      // Side buttons remain fixed for Up/Down.
      return (gpio.*fn)(HalGPIO::BTN_DOWN);
    case Button::Power:
      // Power button bypasses remapping.
      return (gpio.*fn)(HalGPIO::BTN_POWER);
    case Button::PageBack:
      // Reader page navigation uses side buttons and can be swapped via settings.
      switch (sideLayout) {
        case CrossPointSettings::PREV_NEXT:
          return (gpio.*fn)(HalGPIO::BTN_UP);
        case CrossPointSettings::NEXT_PREV:
          return (gpio.*fn)(HalGPIO::BTN_DOWN);
        case CrossPointSettings::SIDE_BUTTONS_DISABLED:
        default:
          return false;
      }
    case Button::PageForward:
      // Reader page navigation uses side buttons and can be swapped via settings.
      switch (sideLayout) {
        case CrossPointSettings::PREV_NEXT:
          return (gpio.*fn)(HalGPIO::BTN_DOWN);
        case CrossPointSettings::NEXT_PREV:
          return (gpio.*fn)(HalGPIO::BTN_UP);
        case CrossPointSettings::SIDE_BUTTONS_DISABLED:
        default:
          return false;
      }
    case Button::NavNext:
      // Logical "next item" navigation: side Down + front Right, with the control axis flipped in
      // INVERTED / LANDSCAPE_CCW (frontButtonFollowOrientation) so it matches the rotated hint labels.
      return isNavDirectionSwapped() ? (mapButton(Button::Up, fn) || mapButton(Button::Left, fn))
                                     : (mapButton(Button::Down, fn) || mapButton(Button::Right, fn));
    case Button::NavPrevious:
      // Logical "previous item" navigation: side Up + front Left, axis-flipped in the same orientations.
      return isNavDirectionSwapped() ? (mapButton(Button::Down, fn) || mapButton(Button::Right, fn))
                                     : (mapButton(Button::Up, fn) || mapButton(Button::Left, fn));
    case Button::ScreenLeft:
    case Button::ScreenRight:
    case Button::ScreenUp:
    case Button::ScreenDown:
      return mapButton(mapScreenDirection(button), fn);
  }

  return false;
}

namespace {
constexpr float LEFT_EDGE_BACK_GESTURE_FRAC_X = 0.25f;
constexpr float BOTTOM_EDGE_BACK_GESTURE_FRAC_Y = 0.14f;
constexpr float TOP_EDGE_MENU_GESTURE_FRAC_Y = 0.14f;
constexpr unsigned long TOUCH_DOWN_SELECT_DELAY_MS = 90;
constexpr unsigned long TOUCH_HELD_OVERRIDE_WINDOW_MS = 250;
// Stationary contact time on a virtual button before it activates without
// waiting for lift-off. Matches the feel of list touch-down selection and is
// long enough that a flick (home/back swipe) exceeds the tap slop first.
constexpr unsigned long VIRTUAL_BUTTON_DWELL_MS = 100;
}  // namespace

bool MappedInputManager::hasTouch() const { return gpio.hasTouch(); }

void MappedInputManager::update() const {
  gpio.update();
  pollVirtualButtons();
}

void MappedInputManager::pollVirtualButtons() const {
  synthPressMask = 0;
  synthReleaseMask = 0;
  synthTapConsumed = false;
  if (!SETTINGS.showVirtualButtons || !gpio.hasTouch()) {
    dwellActivated = false;
    return;
  }

  // The hint bar drawn last frame marks the virtual-button zone as live; the
  // render task clears the latch at the start of each frame, so a stale bar
  // never stays tappable after a screen without a bar is rendered.
  const auto& theme = UITheme::getInstance().getTheme();
  if (!theme.virtualBarDrawnNow()) {
    dwellActivated = false;
    return;
  }

  const auto fire = [this](const int i) {
    // Bar position i shows the label of raw front button i (BTN_BACK..BTN_RIGHT),
    // so a press there synthesizes that exact hardware button (remap-safe).
    const uint8_t bit = static_cast<uint8_t>(1u << i);
    synthPressMask |= bit;
    synthReleaseMask |= bit;
    synthTapConsumed = true;
    rememberTouchHeldTime();
  };

  float nx = 0.0f;
  float ny = 0.0f;
  if (gpio.wasTouchTap(nx, ny)) {
    // Contact ended: a quick tap activates on release; a dwell-activated
    // contact just gets its release swallowed.
    const int i = virtualButtonAt(nx, ny);
    if (i >= 0) {
      if (!dwellActivated) fire(i);
      else synthTapConsumed = true;
    }
    dwellActivated = false;
    return;
  }

  unsigned long heldMs = 0;
  if (gpio.isTouchTapCandidate(nx, ny, heldMs)) {
    if (!dwellActivated && heldMs >= VIRTUAL_BUTTON_DWELL_MS) {
      const int i = virtualButtonAt(nx, ny);
      if (i >= 0) {
        // Dwell activation: fire while the finger is still down.
        dwellActivated = true;
        fire(i);
      }
    }
    return;
  }

  // No live contact left: a dwell flag from a contact that turned into a
  // swipe dies here.
  dwellActivated = false;
}

int MappedInputManager::virtualButtonAt(const float nx, const float ny) const {
  const auto& theme = UITheme::getInstance().getTheme();
  // The bar is drawn in portrait space regardless of the live orientation.
  int px = 0;
  int py = 0;
  renderer.tapToPortrait(nx, ny, px, py);
  for (int i = 0; i < 4; i++) {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    if (!theme.virtualButtonRect(renderer, i, x, y, w, h)) continue;
    if (px >= x && px < x + w && py >= y && py < y + h) return i;
  }
  return -1;
}

bool MappedInputManager::wasSynthButton(const uint8_t mask, const Button button) const {
  if (!mask) return false;
  const auto hit = [&mask](const uint8_t hw) { return (mask & (1u << hw)) != 0; };
  switch (button) {
    case Button::Back:
      return hit(SETTINGS.frontButtonBack);
    case Button::Confirm:
      return hit(SETTINGS.frontButtonConfirm);
    case Button::Left:
      return hit(SETTINGS.frontButtonLeft);
    case Button::Right:
      return hit(SETTINGS.frontButtonRight);
    case Button::NavNext:
      // Mirror mapButton's NavNext composition (side buttons never synthesize).
      return isNavDirectionSwapped() ? wasSynthButton(mask, Button::Left) : wasSynthButton(mask, Button::Right);
    case Button::NavPrevious:
      return isNavDirectionSwapped() ? wasSynthButton(mask, Button::Right) : wasSynthButton(mask, Button::Left);
    case Button::ScreenLeft:
    case Button::ScreenRight:
    case Button::ScreenUp:
    case Button::ScreenDown:
      return wasSynthButton(mask, mapScreenDirection(button));
    default:
      return false;
  }
}

void MappedInputManager::rememberTouchHeldTime() const {
  touchHeldOverrideValid = true;
  touchHeldOverrideMs = gpio.lastTouchHeldMs();
  touchHeldOverrideAt = millis();
}

bool MappedInputManager::wasScreenTapped(int& x, int& y) const {
  if (synthTapConsumed) return false;
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.wasTouchTap(nx, ny)) return false;
  renderer.tapToLogical(nx, ny, x, y);
  rememberTouchHeldTime();
  return true;
}

bool MappedInputManager::wasScreenTouchDown(int& x, int& y) const {
  float nx = 0.0f;
  float ny = 0.0f;
  unsigned long heldMs = 0;
  if (!gpio.isTouchTapCandidate(nx, ny, heldMs)) return false;
  if (heldMs < TOUCH_DOWN_SELECT_DELAY_MS) return false;
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::isScreenTouchHeld(int& x, int& y) const {
  // Live contact position while the finger is down (no tap-slop gate) — drag tracking.
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.isTouchHeldAt(nx, ny)) return false;
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::wasTapInRect(const int x, const int y, const int width, const int height) const {
  int tx = 0;
  int ty = 0;
  return wasScreenTapped(tx, ty) && tx >= x && tx < x + width && ty >= y && ty < y + height;
}

bool MappedInputManager::listItemFromPoint(const int x, const int y, int& index, const int itemCount,
                                           const int selectedIndex, const int listTop, const int listHeight,
                                           const bool hasSubtitle) const {
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

bool MappedInputManager::wasListItemTapped(int& index, const int itemCount, const int selectedIndex, const int listTop,
                                           const int listHeight, const bool hasSubtitle) const {
  int tx = 0;
  int ty = 0;
  return wasScreenTapped(tx, ty) &&
         listItemFromPoint(tx, ty, index, itemCount, selectedIndex, listTop, listHeight, hasSubtitle);
}

bool MappedInputManager::wasListItemTouchedDown(int& index, const int itemCount, const int selectedIndex,
                                                const int listTop, const int listHeight, const bool hasSubtitle) const {
  int tx = 0;
  int ty = 0;
  return wasScreenTouchDown(tx, ty) &&
         listItemFromPoint(tx, ty, index, itemCount, selectedIndex, listTop, listHeight, hasSubtitle);
}

MappedInputManager::RowTouch MappedInputManager::rowTouch(int& row, const int top, const int rowStep,
                                                          const int rowCount, const int xStart, const int xEnd,
                                                          const int rowHeight) const {
  if (rowStep <= 0 || rowCount <= 0) return RowTouch::None;
  const auto hit = [&](const int x, const int y) {
    if (x < xStart || x >= xEnd || y < top) return false;
    const int r = (y - top) / rowStep;
    if (r >= rowCount) return false;
    if (rowHeight > 0 && (y - top) % rowStep >= rowHeight) return false;
    row = r;
    return true;
  };
  int x = 0;
  int y = 0;
  if (wasScreenTouchDown(x, y) && hit(x, y)) return RowTouch::Down;
  if (wasScreenTapped(x, y) && hit(x, y)) return RowTouch::Tap;
  return RowTouch::None;
}

MappedInputManager::RowTouch MappedInputManager::colTouch(int& col, const int left, const int colStep,
                                                          const int colCount, const int yStart, const int yEnd,
                                                          const int colWidth) const {
  if (colStep <= 0 || colCount <= 0) return RowTouch::None;
  const auto hit = [&](const int x, const int y) {
    if (y < yStart || y >= yEnd || x < left) return false;
    const int c = (x - left) / colStep;
    if (c >= colCount) return false;
    if (colWidth > 0 && (x - left) % colStep >= colWidth) return false;
    col = c;
    return true;
  };
  int x = 0;
  int y = 0;
  if (wasScreenTouchDown(x, y) && hit(x, y)) return RowTouch::Down;
  if (wasScreenTapped(x, y) && hit(x, y)) return RowTouch::Tap;
  return RowTouch::None;
}

bool MappedInputManager::decodeSwipe(int& sx, int& sy, int& ex, int& ey) const {
  float nxs = 0.0f;
  float nys = 0.0f;
  float nxe = 0.0f;
  float nye = 0.0f;
  if (!gpio.wasSwipe(nxs, nys, nxe, nye)) return false;
  renderer.tapToLogical(nxs, nys, sx, sy);
  renderer.tapToLogical(nxe, nye, ex, ey);
  return true;
}

MappedInputManager::SwipeDir MappedInputManager::wasSwipe() const {
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return SwipeDir::None;
  const int dx = ex - sx;
  const int dy = ey - sy;
  if (std::abs(dx) >= std::abs(dy)) {
    return dx < 0 ? SwipeDir::Left : SwipeDir::Right;
  }
  return dy < 0 ? SwipeDir::Up : SwipeDir::Down;
}

bool MappedInputManager::wasBackGesture() const {
  // Back = left-to-right swipe starting near the left edge. Edge-anchored so that
  // mid-screen horizontal swipes stay available to activities that consume
  // SwipeDir::Left/Right (e.g. percent selection, image viewer).
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  const bool hit = sx <= renderer.getScreenWidth() * LEFT_EDGE_BACK_GESTURE_FRAC_X && ex > sx &&
                   std::abs(ex - sx) > std::abs(ey - sy);
  if (hit) rememberTouchHeldTime();
  return hit;
}

bool MappedInputManager::wasMenuGesture() const {
  // Downward swipe starting at the top edge (mirror of the bottom-edge home gesture).
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  const int topEdgeBottom = static_cast<int>(renderer.getScreenHeight() * TOP_EDGE_MENU_GESTURE_FRAC_Y);
  const bool hit = sy <= topEdgeBottom && ey > sy && std::abs(ey - sy) > std::abs(ex - sx);
  if (hit) rememberTouchHeldTime();
  return hit;
}

bool MappedInputManager::wasHomeGesture() const {
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (decodeSwipe(sx, sy, ex, ey)) {
    const int bottomEdgeTop =
        renderer.getScreenHeight() - static_cast<int>(renderer.getScreenHeight() * BOTTOM_EDGE_BACK_GESTURE_FRAC_Y);
    if (sy >= bottomEdgeTop && ey < sy && std::abs(ey - sy) > std::abs(ex - sx)) {
      rememberTouchHeldTime();
      return true;
    }
  }
  return false;
}

bool MappedInputManager::wasPressed(const Button button) const {
  if (button == Button::Back && wasBackGesture()) return true;
  if (wasSynthButton(synthPressMask, button)) return true;
  return mapButton(button, &HalGPIO::wasPressed);
}

bool MappedInputManager::wasReleased(const Button button) const {
  if (button == Button::Back && wasBackGesture()) return true;
  if (wasSynthButton(synthReleaseMask, button)) return true;
  return mapButton(button, &HalGPIO::wasReleased);
}

bool MappedInputManager::isPressed(const Button button) const { return mapButton(button, &HalGPIO::isPressed); }

bool MappedInputManager::wasAnyPressed() const { return synthPressMask != 0 || gpio.wasAnyPressed(); }

bool MappedInputManager::wasAnyReleased() const { return gpio.wasAnyReleased(); }

unsigned long MappedInputManager::getHeldTime() const {
  if (!gpio.wasAnyPressed() && !gpio.wasAnyReleased() && touchHeldOverrideValid &&
      millis() - touchHeldOverrideAt <= TOUCH_HELD_OVERRIDE_WINDOW_MS) {
    return touchHeldOverrideMs;
  }
  touchHeldOverrideValid = false;
  return gpio.getHeldTime();
}

MappedInputManager::Labels MappedInputManager::mapLabels(const char* back, const char* confirm, const char* previous,
                                                         const char* next) const {
  // Swap previous/next labels to match the page turn direction swap in INVERTED and LANDSCAPE_CCW.
  const bool swapLabels = isNavDirectionSwapped();
  const char* leftLabel = swapLabels ? next : previous;
  const char* rightLabel = swapLabels ? previous : next;

  return mapFrontLabels(back, confirm, leftLabel, rightLabel);
}

MappedInputManager::Labels MappedInputManager::mapDirectionalLabels(const char* back, const char* confirm,
                                                                    const char* left, const char* right, const char* up,
                                                                    const char* down) const {
  const auto labelForButton = [&](const Button rawButton) {
    if (mapScreenDirection(Button::ScreenLeft) == rawButton) return left;
    if (mapScreenDirection(Button::ScreenRight) == rawButton) return right;
    if (mapScreenDirection(Button::ScreenUp) == rawButton) return up;
    if (mapScreenDirection(Button::ScreenDown) == rawButton) return down;
    return "";
  };
  return mapFrontLabels(back, confirm, labelForButton(Button::Left), labelForButton(Button::Right));
}

MappedInputManager::Labels MappedInputManager::mapFrontLabels(const char* back, const char* confirm, const char* left,
                                                              const char* right) const {
  // Build the label order based on the configured hardware mapping.
  auto labelForHardware = [&](uint8_t hw) -> const char* {
    // Compare against configured logical roles and return the matching label.
    if (hw == SETTINGS.frontButtonBack) {
      return back;
    }
    if (hw == SETTINGS.frontButtonConfirm) {
      return confirm;
    }
    if (hw == SETTINGS.frontButtonLeft) {
      return left;
    }
    if (hw == SETTINGS.frontButtonRight) {
      return right;
    }
    return "";
  };

  return {labelForHardware(HalGPIO::BTN_BACK), labelForHardware(HalGPIO::BTN_CONFIRM),
          labelForHardware(HalGPIO::BTN_LEFT), labelForHardware(HalGPIO::BTN_RIGHT)};
}

int MappedInputManager::getPressedFrontButton() const {
  // Scan the raw front buttons in hardware order.
  // This bypasses remapping so the remap activity can capture physical presses.
  if (gpio.wasPressed(HalGPIO::BTN_BACK)) {
    return HalGPIO::BTN_BACK;
  }
  if (gpio.wasPressed(HalGPIO::BTN_CONFIRM)) {
    return HalGPIO::BTN_CONFIRM;
  }
  if (gpio.wasPressed(HalGPIO::BTN_LEFT)) {
    return HalGPIO::BTN_LEFT;
  }
  if (gpio.wasPressed(HalGPIO::BTN_RIGHT)) {
    return HalGPIO::BTN_RIGHT;
  }
  return -1;
}
