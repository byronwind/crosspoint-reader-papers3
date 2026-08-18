#pragma once

#include <cstdint>
#include "FsrsTypes.h"

namespace fsrs {

/**
 * FsrsScheduler — FSRS-6 spaced repetition scheduling engine.
 *
 * Computes new stability, difficulty, and next interval for a card
 * given its current state and the user's rating.
 *
 * All floating-point math uses float (single precision) to leverage
 * ESP32-S3 hardware FPU. Each schedule() call completes in < 50ms.
 *
 * Reference: https://github.com/open-spaced-repetition/awesome-fsrs/wiki/The-Algorithm
 */
class FsrsScheduler {
 public:
  explicit FsrsScheduler(const FsrsConfig& config = FsrsConfig{});

  /**
   * Schedule a card after a review.
   * @param current  Current card state
   * @param rating   User's rating (Again/Hard/Good/Easy)
   * @param now      Current time (Unix epoch seconds)
   * @return         New card state and interval in days
   */
  SchedulingResult schedule(const CardState& current, Rating rating, uint32_t now) const;

  /// Compute initial stability for a new card given the rating
  float initialStability(Rating rating) const;

  /// Compute initial difficulty for a new card given the rating
  float initialDifficulty(Rating rating) const;

  /// Compute next stability after a successful recall (elapsed >= 1 day)
  float nextStabilitySuccess(float stability, float difficulty, float retrievability, Rating rating) const;

  /// Compute next stability after a same-day review (FSRS-6 short-term path)
  float nextStabilitySameDay(float stability, Rating rating) const;

  /// Compute next stability after a failure (Again)
  float nextStabilityFailure(float stability, float difficulty, float retrievability) const;

  /// Compute next difficulty after a review
  float nextDifficulty(float difficulty, Rating rating) const;

  /// Compute the interval (in days) for the target retention rate
  float computeInterval(float stability) const;

  /// Compute retrievability (probability of recall) at elapsed days
  float retrievability(float stability, float elapsedDays) const;

  /// Clamp interval to [1, maximumInterval]
  float clampInterval(float interval) const;

  const FsrsConfig& config() const { return config_; }

 private:
  FsrsConfig config_;

  /// Forgetting-curve factor: 0.9^(-1/w[20]) - 1, so R(S, S) = 90%
  float forgettingFactor() const;

  /// Handle learning/relearning step transitions
  SchedulingResult scheduleLearning(const CardState& current, Rating rating, uint32_t now, bool isRelearning) const;

  /// Handle review state scheduling
  SchedulingResult scheduleReview(const CardState& current, Rating rating, uint32_t now) const;
};

}  // namespace fsrs
