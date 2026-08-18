#include "FsrsScheduler.h"

#include <algorithm>
#include <cmath>

namespace fsrs {

// Seconds per day
static constexpr float SECS_PER_DAY = 86400.0f;
// Seconds per minute
static constexpr float SECS_PER_MIN = 60.0f;

FsrsScheduler::FsrsScheduler(const FsrsConfig& config) : config_(config) {}

float FsrsScheduler::initialStability(Rating rating) const {
  // S0(G) = w[G-1], clamped to > 0
  return std::max(config_.w[ratingIndex(rating)], 0.01f);
}

float FsrsScheduler::initialDifficulty(Rating rating) const {
  // FSRS-5+: D0(G) = w[4] - e^(w[5] * (G - 1)) + 1, clamped to [1, 10].
  // w[4] = D0(1), the initial difficulty after a first rating of Again.
  float d = config_.w[4] - std::exp(config_.w[5] * (static_cast<int>(rating) - 1)) + 1.0f;
  return std::clamp(d, 1.0f, 10.0f);
}

float FsrsScheduler::nextDifficulty(float difficulty, Rating rating) const {
  // Linear damping: D' = D + deltaD * (10 - D) / 9, deltaD = -w[6] * (G - 3)
  float deltaD = -config_.w[6] * (static_cast<int>(rating) - 3);
  float dPrime = difficulty + deltaD * (10.0f - difficulty) / 9.0f;
  // Mean reversion toward D0(4) (FSRS-5+ target) to avoid ease hell
  float target = initialDifficulty(Rating::Easy);
  float d = config_.w[7] * target + (1.0f - config_.w[7]) * dPrime;
  return std::clamp(d, 1.0f, 10.0f);
}

float FsrsScheduler::nextStabilitySuccess(float stability, float difficulty, float retrievability,
                                           Rating rating) const {
  // FSRS-5/6 stability increase on successful recall:
  //   S'_r = S * (1 + e^w[8] * (11 - D) * S^(-w[9]) *
  //               (e^(w[10] * (1 - R)) - 1) * hardPenalty * easyBonus)
  // with hardPenalty = w[15] (if Hard) and easyBonus = w[16] (if Easy).
  const float hardPenalty = (rating == Rating::Hard) ? config_.w[15] : 1.0f;
  const float easyBonus = (rating == Rating::Easy) ? config_.w[16] : 1.0f;
  float increase = std::exp(config_.w[8]) * (11.0f - difficulty) *
                   std::pow(stability, -config_.w[9]) *
                   (std::exp(config_.w[10] * (1.0f - retrievability)) - 1.0f) *
                   hardPenalty * easyBonus;
  return std::max(stability * (1.0f + increase), 0.01f);
}

float FsrsScheduler::nextStabilitySameDay(float stability, Rating rating) const {
  // FSRS-6 short-term (same-day) stability:
  //   S' = S * e^(w[17] * (G - 3 + w[18])) * S^(-w[19])
  // The S^(-w[19]) term makes growth converge instead of compounding on
  // repeated same-day reviews. Successful ratings must never shrink S.
  float sInc = std::exp(config_.w[17] * (static_cast<int>(rating) - 3 + config_.w[18])) *
               std::pow(stability, -config_.w[19]);
  if (rating != Rating::Again) {
    sInc = std::max(sInc, 1.0f);
  }
  return std::max(stability * sInc, 0.01f);
}

float FsrsScheduler::nextStabilityFailure(float stability, float difficulty, float retrievability) const {
  // FSRS-4.5+ post-lapse stability:
  //   S'_f = w[11] * D^(-w[12]) * ((S + 1)^w[13] - 1) * e^(w[14] * (1 - R))
  float newStability =
      config_.w[11] *
      std::pow(difficulty, -config_.w[12]) *
      (std::pow(stability + 1.0f, config_.w[13]) - 1.0f) *
      std::exp(config_.w[14] * (1.0f - retrievability));

  // Stability after failure should be less than current stability
  return std::clamp(newStability, 0.01f, stability);
}

float FsrsScheduler::forgettingFactor() const {
  // FSRS-6: factor = 0.9^(-1/w[20]) - 1, ensuring R(S, S) = 90%
  return std::pow(0.9f, -1.0f / config_.w[20]) - 1.0f;
}

float FsrsScheduler::computeInterval(float stability) const {
  // Solve R(t, S) = requestRetention for t:
  //   I = (S / factor) * (RR^(-1/w[20]) - 1)
  // At RR = 0.9 this reduces to I = S (stability IS the 90% interval).
  const float factor = forgettingFactor();
  float interval = stability / factor * (std::pow(config_.requestRetention, -1.0f / config_.w[20]) - 1.0f);
  return std::max(interval, 0.01f);
}

float FsrsScheduler::clampInterval(float interval) const {
  return std::clamp(interval, 1.0f, static_cast<float>(config_.maximumInterval));
}

float FsrsScheduler::retrievability(float stability, float elapsedDays) const {
  // FSRS-6 power forgetting curve with trainable decay:
  //   R(t, S) = (1 + factor * t / S)^(-w[20])
  if (stability <= 0.0f) return 0.0f;
  return std::pow(1.0f + forgettingFactor() * elapsedDays / stability, -config_.w[20]);
}

SchedulingResult FsrsScheduler::scheduleLearning(const CardState& current, Rating rating, uint32_t now,
                                                  bool isRelearning) const {
  SchedulingResult result;
  result.newState = current;
  result.newState.reps++;
  result.newState.lastReview = now;

  const float* steps;
  uint8_t stepCount;
  if (isRelearning) {
    steps = config_.relearnSteps;
    stepCount = config_.relearnStepCount;
  } else {
    steps = config_.learnSteps;
    stepCount = config_.learnStepCount;
  }

  if (rating == Rating::Again) {
    // Reset to first step
    float stepMinutes = (stepCount > 0) ? steps[0] : 1.0f;
    result.newState.due = now + static_cast<uint32_t>(stepMinutes * SECS_PER_MIN);
    result.interval = stepMinutes * SECS_PER_MIN / SECS_PER_DAY;
    if (isRelearning) {
      result.newState.state = State::Relearning;
      result.newState.lapses++;
    } else {
      result.newState.state = State::Learning;
    }
    // Update difficulty but keep stability low
    result.newState.difficulty = nextDifficulty(current.difficulty, rating);
    result.newState.stability = initialStability(rating);
  } else {
    // Advance along the step ladder or graduate (Anki semantics): Hard
    // climbs one step, Good climbs to the final step. With the default two
    // steps {1m, 10m} both resolve to steps[1]; with three steps {1m, 6m,
    // 10m} Hard lands on 6m while Good lands on 10m.
    float stepMinutes;
    if (rating == Rating::Hard) {
      stepMinutes = (stepCount > 1) ? steps[1] : steps[0];
    } else {
      stepMinutes = steps[stepCount - 1];
    }
    if (rating == Rating::Easy && !isRelearning) {
      // Graduate immediately with longer interval
      result.newState.state = State::Review;
      float s = initialStability(rating);
      result.newState.stability = s;
      result.newState.difficulty = nextDifficulty(current.difficulty, rating);
      float intervalDays = clampInterval(computeInterval(s));
      result.newState.due = now + static_cast<uint32_t>(intervalDays * SECS_PER_DAY);
      result.interval = intervalDays;
      result.newState.scheduledDays = intervalDays;
    } else {
      // Move to next learning step
      float interval = stepMinutes * SECS_PER_MIN / SECS_PER_DAY;
      result.newState.due = now + static_cast<uint32_t>(stepMinutes * SECS_PER_MIN);
      result.interval = interval;
      result.newState.stability = initialStability(rating);
      result.newState.difficulty = nextDifficulty(current.difficulty, rating);
      // Ensure state is Learning (not New) for cards entering the learning phase
      result.newState.state = isRelearning ? State::Relearning : State::Learning;
      if (isRelearning) {
        // Graduate from relearning back to review
        result.newState.state = State::Review;
        float s = std::max(current.stability * 0.5f, initialStability(rating));
        result.newState.stability = s;
        float intervalDays = clampInterval(computeInterval(s));
        result.newState.due = now + static_cast<uint32_t>(intervalDays * SECS_PER_DAY);
        result.interval = intervalDays;
        result.newState.scheduledDays = intervalDays;
      }
    }
  }

  result.newState.elapsedDays = (now - current.lastReview) / SECS_PER_DAY;
  return result;
}

SchedulingResult FsrsScheduler::scheduleReview(const CardState& current, Rating rating, uint32_t now) const {
  SchedulingResult result;
  result.newState = current;
  result.newState.reps++;
  result.newState.lastReview = now;
  result.newState.elapsedDays = (now - current.lastReview) / SECS_PER_DAY;

  float elapsed = result.newState.elapsedDays;
  float r = retrievability(current.stability, elapsed);

  // Update difficulty
  result.newState.difficulty = nextDifficulty(current.difficulty, rating);

  if (rating == Rating::Again) {
    // Failure: enter relearning
    result.newState.state = State::Relearning;
    result.newState.lapses++;
    result.newState.stability = nextStabilityFailure(current.stability, current.difficulty, r);
    // Relearning step
    float stepMinutes = (config_.relearnStepCount > 0) ? config_.relearnSteps[0] : 10.0f;
    result.newState.due = now + static_cast<uint32_t>(stepMinutes * SECS_PER_MIN);
    result.interval = stepMinutes * SECS_PER_MIN / SECS_PER_DAY;
    result.newState.scheduledDays = 0;
  } else {
    // Success: remain in review. Reviews spaced less than a day apart take
    // the FSRS-6 short-term stability path instead of the recall formula.
    result.newState.state = State::Review;
    if (elapsed < 1.0f) {
      result.newState.stability = nextStabilitySameDay(current.stability, rating);
    } else {
      result.newState.stability = nextStabilitySuccess(current.stability, current.difficulty, r, rating);
    }
    float intervalDays = clampInterval(computeInterval(result.newState.stability));
    result.newState.due = now + static_cast<uint32_t>(intervalDays * SECS_PER_DAY);
    result.interval = intervalDays;
    result.newState.scheduledDays = intervalDays;
  }

  return result;
}

SchedulingResult FsrsScheduler::schedule(const CardState& current, Rating rating, uint32_t now) const {
  switch (current.state) {
    case State::New:
    case State::Learning:
      return scheduleLearning(current, rating, now, false);

    case State::Review:
      return scheduleReview(current, rating, now);

    case State::Relearning:
      return scheduleLearning(current, rating, now, true);

    default:
      // Should not happen; treat as new
      return scheduleLearning(current, rating, now, false);
  }
}

}  // namespace fsrs
