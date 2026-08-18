#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "DeckStore.h"

namespace anki {

/// Progress callback for APKG import
/// @param current  Current card index being imported
/// @param total    Total cards to import
/// @param phase    Current phase ("extracting", "parsing", "importing", "media")
using ImportProgressCallback = std::function<void(uint32_t current, uint32_t total, const char* phase)>;

/**
 * ApkgImporter — Stream-parses .apkg files (ZIP containers).
 *
 * Extracts collection.anki2 (SQLite), reads cards/notes/col tables,
 * maps Anki schema to local schema, and copies media files to SD card.
 *
 * Uses miniz for ZIP decompression (already available in lib/miniz/).
 *
 * Import pipeline:
 *   1. Extract collection.anki2 to temp file on SD card
 *   2. Open as read-only SQLite, read col.conf → FsrsConfig
 *   3. Read notes table → map flds to front_html/back_html
 *   4. Read cards table → create local cards + card_states
 *   5. Read media JSON → copy media files from ZIP to SD card
 *   6. Cleanup temp file
 *
 * Memory: processes cards in batches of 100 to limit PSRAM usage.
 */
class ApkgImporter {
 public:
  /// Import result summary
  struct ImportResult {
    bool success = false;
    uint32_t deckId = 0;
    uint32_t cardsImported = 0;
    uint32_t mediaImported = 0;
    uint32_t cardsSkipped = 0;
    std::string error;
  };

  /// Import an .apkg file from SD card
  /// @param apkgPath   Path to .apkg file on SD card
  /// @param store      DeckStore to import into
  /// @param sdMediaDir Base directory for media files (e.g., "/sd/AnkiEInk/media")
  /// @param progress   Optional progress callback
  /// @return           Import result summary
  static ImportResult importApkg(const std::string& apkgPath, DeckStore& store,
                                 const std::string& sdMediaDir,
                                 ImportProgressCallback progress = nullptr);

  /// Validate that a file is a valid .apkg (ZIP with collection.anki2)
  static bool validateApkg(const std::string& path);

  /// Get the deck name from an .apkg file without importing
  static std::string peekDeckName(const std::string& path);

 private:
  /// Extract collection.anki2 from ZIP to temp path
  static bool extractCollection(const std::string& zipPath, const std::string& tempPath);

  /// Parse Anki notes/fields into local card format
  static bool parseNoteFields(const std::string& flds, std::string& frontHtml, std::string& backHtml);

  /// Copy media files from ZIP to SD card with bucketed paths
  static uint32_t extractMedia(const std::string& zipPath, const std::string& mediaJson,
                               const std::string& sdMediaDir, DeckStore& store);
};

}  // namespace anki
