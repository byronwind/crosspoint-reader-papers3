#pragma once

#ifdef ANKIEINK

#include <PersistableStore.h>

#include "FsrsTypes.h"

namespace anki {

/**
 * FsrsConfigStore — persisted FSRS configuration (/.crosspoint/fsrs.json).
 *
 * Uses the same CRTP PersistableStore pattern as CrossPointSettings /
 * OpdsServerStore: a JSON document on the littlefs-backed /.crosspoint
 * directory, loaded once at boot and saved on every change.
 */
class FsrsConfigStore : public PersistableStore<FsrsConfigStore> {
 public:
  /// Current configuration (defaults until loadFromFile() at boot).
  fsrs::FsrsConfig get() const { return config_; }

  /// Replace the configuration and persist it.
  void set(const fsrs::FsrsConfig& config);

  /// Restore the FSRS V5 paper defaults and persist them.
  void resetDefaults();

 private:
  friend class PersistableStore<FsrsConfigStore>;
  FsrsConfigStore() = default;

  static const char* getFilePath() { return "/.crosspoint/fsrs.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  fsrs::FsrsConfig config_;
};

}  // namespace anki

#endif  // ANKIEINK
