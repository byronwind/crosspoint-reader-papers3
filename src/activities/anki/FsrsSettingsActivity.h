#pragma once

#ifdef ANKIEINK

#include "activities/Activity.h"
#include "FsrsScheduler.h"
#include "util/ButtonNavigator.h"

/**
 * FsrsSettingsActivity — FSRS parameter configuration.
 *
 * Allows adjusting:
 *   - request_retention (0.70 ~ 0.99)
 *   - daily_new_limit (0 ~ 999)
 *   - daily_review_limit (0 ~ 999)
 *   - maximum_interval (1 ~ 365)
 *   - Parameter vector w[0..12] (advanced)
 *   - Trigger manual optimization
 */
class FsrsSettingsActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  fsrs::FsrsConfig config;
  int selectedIndex = 0;
  bool editing = false;
  float editValue = 0;

  void loadConfig();
  void saveConfig();
  void onOptimizeTap();
  void onResetDefaults();

 public:
  explicit FsrsSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("FSRS Settings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};

#endif  // ANKIEINK
