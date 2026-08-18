#pragma once

#include <cstdint>

namespace fsrs {

/// FSRS card states
enum class State : uint8_t {
  New = 0,        ///< Never reviewed
  Learning = 1,   ///< Initial learning phase (short intervals)
  Review = 2,     ///< Long-term review cycle
  Relearning = 3  ///< Forgotten, re-learning after lapse
};

/// User rating for a review (1-4 scale)
enum class Rating : uint8_t {
  Again = 1,  ///< Failed to recall
  Hard = 2,   ///< Recalled with significant difficulty
  Good = 3,   ///< Recalled with moderate effort
  Easy = 4    ///< Recalled effortlessly
};

/// Persistent card state (stored in card_states table)
struct CardState {
  State state = State::New;
  float stability = 0.0f;    ///< Memory stability (half-life in days)
  float difficulty = 0.0f;   ///< Intrinsic difficulty [1, 10]
  uint32_t due = 0;          ///< Next review due date (Unix timestamp)
  uint32_t lastReview = 0;   ///< Last review timestamp
  float elapsedDays = 0.0f;  ///< Days since last review
  float scheduledDays = 0.0f;///< Scheduled interval at last review
  uint32_t reps = 0;         ///< Total review count
  uint32_t lapses = 0;       ///< Number of times forgotten (Again)
};

/// Result of a single scheduling operation
struct SchedulingResult {
  CardState newState;
  float interval;  ///< Days until next review
};

/// FSRS algorithm configuration
struct FsrsConfig {
  /// 21-dimensional parameter vector (w[0]..w[20])
  /// Defaults from FSRS-6 (open-spaced-repetition, 2025)
  float w[21] = {
    0.212f, 1.2931f, 2.3065f, 8.2956f,  // w[0..3]: initial stability for Again/Hard/Good/Easy
    6.4133f,  ///< w[4]: initial difficulty base (D0(1) = w[4])
    0.8334f,  ///< w[5]: initial difficulty rating exponent
    3.0194f,  ///< w[6]: difficulty update delta scale
    0.001f,   ///< w[7]: difficulty mean reversion weight
    1.8722f,  ///< w[8]: recall stability base exponent
    0.1666f,  ///< w[9]: recall stability S exponent
    0.796f,   ///< w[10]: recall stability retrievability exponent
    1.4835f,  ///< w[11]: post-lapse stability base
    0.0614f,  ///< w[12]: post-lapse stability difficulty exponent
    0.2629f,  ///< w[13]: post-lapse stability S exponent
    1.6483f,  ///< w[14]: post-lapse stability retrievability exponent
    0.6014f,  ///< w[15]: hard penalty multiplier
    1.8729f,  ///< w[16]: easy bonus multiplier
    0.5425f,  ///< w[17]: same-day stability rating exponent
    0.0912f,  ///< w[18]: same-day stability rating offset
    0.0658f,  ///< w[19]: same-day stability decay exponent
    0.1542f   ///< w[20]: forgetting curve decay exponent
  };

  float requestRetention = 0.9f;  ///< Target long-term retention rate [0.70, 0.99]
  uint32_t maximumInterval = 36500; ///< Maximum interval in days (~100 years)
  uint16_t dailyNewLimit = 20;    ///< Max new cards per day
  uint16_t dailyReviewLimit = 200;///< Max review cards per day

  /// Learning step intervals (minutes), one entry per step. Presets are
  /// applied via setLearnStepPreset(); entries past learnStepCount are unused.
  float learnSteps[3] = {1.0f, 10.0f, 0.0f};
  uint8_t learnStepCount = 2;

  /// Relearning step intervals (minutes)
  float relearnSteps[1] = {10.0f};
  uint8_t relearnStepCount = 1;

  /// Configure the learning-step ladder from a preset:
  ///  - 2 steps: Anki's default {1m, 10m}
  ///  - 3 steps: the common {1m, 6m, 10m} variant
  void setLearnStepPreset(uint8_t count) {
    learnStepCount = (count >= 3) ? 3 : 2;
    if (learnStepCount == 3) {
      learnSteps[0] = 1.0f;
      learnSteps[1] = 6.0f;
      learnSteps[2] = 10.0f;
    } else {
      learnSteps[0] = 1.0f;
      learnSteps[1] = 10.0f;
      learnSteps[2] = 0.0f;
    }
  }
};

/// Convert Rating to 1-based integer for array indexing
inline int ratingIndex(Rating r) {
  return static_cast<int>(r) - 1;
}

}  // namespace fsrs
