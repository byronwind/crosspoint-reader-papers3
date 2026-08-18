// Guarded so non-ANKIEINK builds compile this as an empty translation unit
// (see DeckStore.cpp for why).
#ifdef ANKIEINK

#include "FsrsConfigStore.h"

#include <ArduinoJson.h>
#include <Logging.h>

namespace anki {

void FsrsConfigStore::set(const fsrs::FsrsConfig& config) {
  config_ = config;
  saveToFile();
}

void FsrsConfigStore::resetDefaults() {
  config_ = fsrs::FsrsConfig{};
  saveToFile();
}

void FsrsConfigStore::toJson(JsonDocument& doc) const {
  JsonObject root = doc.to<JsonObject>();
  root["requestRetention"] = config_.requestRetention;
  root["maximumInterval"] = config_.maximumInterval;
  root["dailyNewLimit"] = config_.dailyNewLimit;
  root["dailyReviewLimit"] = config_.dailyReviewLimit;
  root["learnStepCount"] = config_.learnStepCount;
  root["relearnStepCount"] = config_.relearnStepCount;

  JsonArray learnSteps = root["learnSteps"].to<JsonArray>();
  for (uint8_t i = 0; i < config_.learnStepCount; ++i) {
    learnSteps.add(config_.learnSteps[i]);
  }
  JsonArray relearnSteps = root["relearnSteps"].to<JsonArray>();
  for (uint8_t i = 0; i < config_.relearnStepCount; ++i) {
    relearnSteps.add(config_.relearnSteps[i]);
  }
  JsonArray w = root["w"].to<JsonArray>();
  for (int i = 0; i < 21; ++i) {
    w.add(config_.w[i]);
  }
}

bool FsrsConfigStore::fromJson(JsonVariantConst doc) {
  if (!doc.is<JsonObjectConst>()) {
    return false;
  }
  fsrs::FsrsConfig cfg;

  // Clamp persisted values into the documented ranges so a hand-edited file
  // cannot push the scheduler into nonsense territory. All clamp() operands
  // are cast to one type (xtensa stdint aliases differ per width).
  cfg.requestRetention = std::clamp(doc["requestRetention"] | cfg.requestRetention, 0.70f, 0.99f);
  cfg.maximumInterval =
      std::clamp(static_cast<uint32_t>(doc["maximumInterval"] | cfg.maximumInterval), static_cast<uint32_t>(1),
                 static_cast<uint32_t>(36500));
  cfg.dailyNewLimit =
      std::clamp(static_cast<uint16_t>(doc["dailyNewLimit"] | cfg.dailyNewLimit), static_cast<uint16_t>(0),
                 static_cast<uint16_t>(999));
  cfg.dailyReviewLimit =
      std::clamp(static_cast<uint16_t>(doc["dailyReviewLimit"] | cfg.dailyReviewLimit), static_cast<uint16_t>(0),
                 static_cast<uint16_t>(999));

  // Learning steps are preset ladders (2 = {1m, 10m}, 3 = {1m, 6m, 10m});
  // rebuild from the persisted count so a hand-edited step array can never
  // diverge from the count or carry out-of-range values.
  const uint8_t learnCount = std::clamp(static_cast<uint8_t>(doc["learnStepCount"] | cfg.learnStepCount),
                                        static_cast<uint8_t>(2), static_cast<uint8_t>(3));
  cfg.setLearnStepPreset(learnCount);

  const uint8_t relearnCount = std::clamp(static_cast<uint8_t>(doc["relearnStepCount"] | cfg.relearnStepCount),
                                          static_cast<uint8_t>(1), static_cast<uint8_t>(1));
  cfg.relearnStepCount = relearnCount;
  const JsonArrayConst relearnSteps = doc["relearnSteps"].as<JsonArrayConst>();
  for (uint8_t i = 0; i < relearnCount; ++i) {
    cfg.relearnSteps[i] = relearnSteps[i] | cfg.relearnSteps[i];
  }

  // Legacy 13-element (pre-FSRS-6) arrays load fine: out-of-range indices
  // yield null variants and fall back to the FSRS-6 defaults below.
  const JsonArrayConst w = doc["w"].as<JsonArrayConst>();
  for (int i = 0; i < 21; ++i) {
    const float v = w[i] | cfg.w[i];
    cfg.w[i] = v >= 0.0f ? v : cfg.w[i];  // negative weights are invalid
  }

  config_ = cfg;
  return true;
}

}  // namespace anki

#endif  // ANKIEINK
