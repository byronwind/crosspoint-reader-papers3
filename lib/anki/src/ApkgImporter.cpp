// Guarded so non-ANKIEINK builds compile this as an empty translation unit
// (see DeckStore.cpp for why).
#ifdef ANKIEINK

#include "ApkgImporter.h"

#include <sqlite3.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "AnkiPaths.h"
#include "HalStorage.h"
#include "SdFatVfs.h"
#include "ZipFile.h"

namespace anki {
namespace {

constexpr const char* kCollectionEntry = "collection.anki2";
constexpr const char* kMediaEntry = "media";
constexpr size_t kImportBatchSize = 50;

/// Parse the APKG media mapping JSON — `{"1": "foo.jpg", "2": "bar.mp3"}` —
/// into a numeric-id -> filename map. The format is simple and stable enough
/// that a hand-rolled scanner beats pulling in a JSON parser.
bool parseMediaJson(const char* json, std::unordered_map<std::string, std::string>& out) {
  if (!json) return false;
  const char* p = json;
  while ((p = strchr(p, '"')) != nullptr) {
    ++p;
    const char* keyEnd = strchr(p, '"');
    if (!keyEnd) break;
    const std::string key(p, static_cast<size_t>(keyEnd - p));
    p = keyEnd + 1;
    const char* colon = strchr(p, ':');
    if (!colon) break;
    p = colon + 1;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != '"') break;
    ++p;
    const char* valEnd = strchr(p, '"');
    if (!valEnd) break;
    out.emplace(key, std::string(p, static_cast<size_t>(valEnd - p)));
    p = valEnd + 1;
  }
  return !out.empty();
}

/// Remove `[sound:file.mp3]` tags (Anki audio markers) from a field string.
/// Decks are imported for reading only until an on-device player exists;
/// leaving the raw tag text would render as literal "[sound:...]" on cards.
void stripSoundTags(std::string& s) {
  size_t pos = 0;
  while ((pos = s.find("[sound:", pos)) != std::string::npos) {
    const size_t end = s.find(']', pos);
    if (end == std::string::npos) {
      s.erase(pos);
      break;
    }
    s.erase(pos, end - pos + 1);
  }
}

/// Decode `\uXXXX` JSON escapes (Anki stores non-ASCII deck names escaped)
/// into UTF-8. Handles BMP code points and surrogate pairs; any other text
/// passes through untouched.
void decodeUnicodeEscapes(std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    const bool looksEscaped =
        s[i] == '\\' && i + 6 <= s.size() && s[i + 1] == 'u';
    if (looksEscaped) {
      char* end = nullptr;
      const unsigned long cp = strtoul(s.c_str() + i + 2, &end, 16);
      if (end == s.c_str() + i + 6) {
        unsigned long final = cp;
        size_t consumed = 6;
        // Surrogate pair: \uD800-\uDBFF immediately followed by \uDC00-\uDFFF.
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 12 <= s.size() && s[i + 6] == '\\' &&
            s[i + 7] == 'u') {
          const unsigned long lo = strtoul(s.c_str() + i + 8, &end, 16);
          if (end == s.c_str() + i + 12 && lo >= 0xDC00 && lo <= 0xDFFF) {
            final = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            consumed = 12;
          }
        }
        if (final < 0x80) {
          out.push_back(static_cast<char>(final));
        } else if (final < 0x800) {
          out.push_back(static_cast<char>(0xC0 | (final >> 6)));
          out.push_back(static_cast<char>(0x80 | (final & 0x3F)));
        } else if (final < 0x10000) {
          out.push_back(static_cast<char>(0xE0 | (final >> 12)));
          out.push_back(static_cast<char>(0x80 | ((final >> 6) & 0x3F)));
          out.push_back(static_cast<char>(0x80 | (final & 0x3F)));
        } else {
          out.push_back(static_cast<char>(0xF0 | (final >> 18)));
          out.push_back(static_cast<char>(0x80 | ((final >> 12) & 0x3F)));
          out.push_back(static_cast<char>(0x80 | ((final >> 6) & 0x3F)));
          out.push_back(static_cast<char>(0x80 | (final & 0x3F)));
        }
        i += consumed;
        continue;
      }
    }
    out.push_back(s[i]);
    ++i;
  }
  s = std::move(out);
}

/// Extract the first deck name from the `col.decks` JSON blob:
/// `{"1": {"name": "Default", ...}, ...}`.
std::string firstDeckNameFromJson(const char* json) {
  const char* p = strstr(json, "\"name\"");
  if (!p) return {};
  p = strchr(p, ':');
  if (!p) return {};
  p = strchr(p, '"');
  if (!p) return {};
  ++p;
  const char* end = strchr(p, '"');
  if (!end) return {};
  std::string name(p, static_cast<size_t>(end - p));
  decodeUnicodeEscapes(name);
  return name;
}

// SQLite invokes this every N virtual-machine opcodes while a statement runs.
// A big JOIN scan (or a commit fsync on a slow SD card) can run past the
// watchdog window inside a SINGLE sqlite3_step, where the per-row vTaskDelay
// in the import loop cannot reach. Yielding from inside the VM keeps the idle
// task fed. Throttled to ~once per 30 ms so we don't surrender the CPU on
// every invocation. Return 0 to continue; nonzero would abort the statement.
int sqliteYieldProgress(void*) {
  static TickType_t lastYield = 0;
  const TickType_t now = xTaskGetTickCount();
  if (now - lastYield >= pdMS_TO_TICKS(30)) {
    lastYield = now;
    vTaskDelay(1);
  }
  return 0;
}

#ifdef SIMULATOR
// The desktop simulator links the host sqlite3 with its default unix VFS,
// which needs real filesystem paths; map SD-logical paths onto the simulated
// SD root. On the device the SdFat VFS understands SD paths directly.
std::string sqlitePath(const std::string& sdPath) { return Storage.hostPath(sdPath.c_str()); }
#else
const std::string& sqlitePath(const std::string& sdPath) { return sdPath; }
#endif

}  // namespace

bool ApkgImporter::validateApkg(const std::string& path) {
  ZipFile zip(path);
  bool found = false;
  zip.enumerateFileEntries([&](std::string_view name, uint32_t, uint32_t) {
    if (name == kCollectionEntry) found = true;
  });
  return found;
}

std::string ApkgImporter::peekDeckName(const std::string& path) {
  registerSdFatVfs();
  if (!Storage.ensureDirectoryExists(kAnkiTmpDir)) return {};

  const std::string tmp = std::string(kAnkiTmpDir) + "/peek.anki2";
  Storage.remove(tmp.c_str());
  if (!extractCollection(path, tmp)) return {};

  std::string name;
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(sqlitePath(tmp).c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT decks FROM col LIMIT 1", -1, &stmt, nullptr) == SQLITE_OK) {
      if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* json = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (json) name = firstDeckNameFromJson(json);
      }
      sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
  }
  Storage.remove(tmp.c_str());
  return name;
}

ApkgImporter::ImportResult ApkgImporter::importApkg(const std::string& apkgPath, DeckStore& store,
                                      const std::string& sdMediaDir,
                                      ImportProgressCallback progress) {
  ImportResult result;
  registerSdFatVfs();

  // Self-heal: if boot-time init failed (e.g. the /AnkiEInk directory was
  // missing before the mkdir fix), re-open the local database here so imports
  // can still proceed. init() is idempotent when already open.
  if (!store.isOpen() && !store.init(kAnkiDbPath)) {
    result.error = "Local database unavailable";
    return result;
  }

  if (progress) progress(0, 0, "extracting");
  if (!validateApkg(apkgPath)) {
    result.error = "Not a valid APKG file (missing collection.anki2)";
    return result;
  }
  if (!Storage.ensureDirectoryExists(kAnkiTmpDir)) {
    result.error = "Cannot create temp directory";
    return result;
  }

  // 1. Extract collection.anki2 to a temp file on the SD card, then open it
  //    read-only through the SdFat VFS.
  const std::string tempDb = std::string(kAnkiTmpDir) + "/import.anki2";
  Storage.remove(tempDb.c_str());  // stale copy from a crashed import
  if (!extractCollection(apkgPath, tempDb)) {
    result.error = "Failed to extract collection.anki2";
    return result;
  }

  sqlite3* src = nullptr;
  if (sqlite3_open_v2(sqlitePath(tempDb).c_str(), &src, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    result.error = std::string("Failed to open collection.anki2: ") + sqlite3_errmsg(src);
    if (src) sqlite3_close(src);
    Storage.remove(tempDb.c_str());
    return result;
  }

  // Let long-running statements on the source DB yield from inside the VM (see
  // sqliteYieldProgress) so a single slow step can't starve the idle task.
  sqlite3_progress_handler(src, 256, sqliteYieldProgress, nullptr);

  // 2. Deck name from col.decks.
  if (progress) progress(0, 0, "parsing");
  std::string deckName;
  {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(src, "SELECT decks FROM col LIMIT 1", -1, &stmt, nullptr) == SQLITE_OK) {
      if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* json = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (json) deckName = firstDeckNameFromJson(json);
      }
      sqlite3_finalize(stmt);
    }
  }
  if (deckName.empty()) deckName = "Imported Deck";

  DeckInfo deck;
  deck.name = deckName;
  deck.createdAt = static_cast<uint32_t>(time(nullptr));
  deck.updatedAt = deck.createdAt;
  result.deckId = store.upsertDeck(deck);
  if (result.deckId == 0) {
    result.error = "Failed to create deck in local database";
    sqlite3_close(src);
    Storage.remove(tempDb.c_str());
    return result;
  }

  // 3. Cards: join with notes so flds/tags come in one pass. Anki ids are
  //    int64 millisecond timestamps that exceed uint32, so local cards get
  //    fresh ids and the source note id is not preserved (dedup/re-import is
  //    a Phase-1 concern). Small batches keep the transient heap footprint low.
  //
  //    Deliberately NO "ORDER BY c.id": sorting the full joined result either
  //    spills to a temp file on the SD card (a long, unyielding operation) or,
  //    under temp_store=MEMORY, pins the entire result set in the heap — a 2+ MB
  //    deck exhausts the ~320 KB heap and starves the idle task. Cards have no
  //    ordering dependency (each gets a fresh local id), so natural scan order
  //    (cards by rowid, notes looked up by primary key) is fine.
  if (progress) progress(0, 0, "importing");
  const char* kCardSql =
      "SELECT c.type, c.ivl, c.reps, c.lapses, n.flds, n.tags "
      "FROM cards c JOIN notes n ON n.id = c.nid";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(src, kCardSql, -1, &stmt, nullptr) != SQLITE_OK) {
    result.error = "collection.anki2 has an unexpected schema (cards/notes join failed)";
    sqlite3_close(src);
    Storage.remove(tempDb.c_str());
    return result;
  }

  std::vector<CardInfo> batch;
  std::vector<fsrs::CardState> batchStates;
  batch.reserve(kImportBatchSize);
  batchStates.reserve(kImportBatchSize);
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  uint32_t rowCount = 0;

  auto flushBatch = [&](std::vector<uint32_t>* ids) -> bool {
    if (batch.empty()) return true;
    if (!store.insertCards(batch, ids) || (ids && ids->size() != batch.size())) {
      result.error = "Failed to write cards to local database";
      return false;
    }
    // Write the carried scheduling states in ONE transaction. The per-card
    // updateCardState() is autocommit, so a 50-card batch used to cost ~50 SD
    // fsyncs back-to-back with no yield in between — long enough to starve the
    // idle task and trip the watchdog. One transaction = one commit/fsync.
    std::vector<uint32_t> stateIds;
    std::vector<fsrs::CardState> stateVals;
    for (size_t i = 0; i < batch.size(); i++) {
      if (batchStates[i].state != fsrs::State::New) {
        stateIds.push_back((*ids)[i]);
        stateVals.push_back(batchStates[i]);
      }
    }
    if (!stateIds.empty()) {
      store.updateCardStates(stateIds, stateVals);
    }
    result.cardsImported += static_cast<uint32_t>(batch.size());
    batch.clear();
    batchStates.clear();
    // Yield every batch so the idle task can feed the watchdog on large decks.
    vTaskDelay(1);
    return true;
  };

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const int type = sqlite3_column_int(stmt, 0);
    const int64_t ivl = sqlite3_column_int64(stmt, 1);
    const int reps = sqlite3_column_int(stmt, 2);
    const int lapses = sqlite3_column_int(stmt, 3);
    const char* flds = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    const char* tags = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));

    CardInfo card;
    card.deckId = result.deckId;
    card.noteId = 0;
    parseNoteFields(flds ? flds : "", card.frontHtml, card.backHtml);
    card.tags = tags ? tags : "";
    card.createdAt = now;
    batch.push_back(std::move(card));

    // Carry scheduling state over for cards Anki has already learned.
    fsrs::CardState state{};
    if (type == 1) {
      state.state = fsrs::State::Learning;
    } else if (type == 2) {
      state.state = fsrs::State::Review;
    } else if (type == 3) {
      state.state = fsrs::State::Relearning;
    }
    if (state.state != fsrs::State::New) {
      // Anki `ivl` is in days (positive) or seconds (negative, learning).
      // Review due is a day count, so approximate with now + ivl days.
      state.due = ivl > 0 ? now + static_cast<uint32_t>(ivl) * 86400u : now;
      state.reps = static_cast<uint32_t>(reps);
      state.lapses = static_cast<uint32_t>(lapses);
    }
    batchStates.push_back(state);

    if (batch.size() >= kImportBatchSize) {
      std::vector<uint32_t> ids;
      if (!flushBatch(&ids)) {
        sqlite3_finalize(stmt);
        sqlite3_close(src);
        Storage.remove(tempDb.c_str());
        return result;
      }
    }
    // Yield every 32 rows so the idle task can feed the watchdog even when a
    // 100-row batch takes long under slow SD I/O (the flushBatch yield alone
    // is too coarse: two consecutive batches could exceed the WDT timeout).
    if ((++rowCount & 31) == 0) {
      vTaskDelay(1);
    }
    if (progress) progress(result.cardsImported, 0, "importing");
  }

  std::vector<uint32_t> tailIds;
  if (!flushBatch(&tailIds)) {
    sqlite3_finalize(stmt);
    sqlite3_close(src);
    Storage.remove(tempDb.c_str());
    return result;
  }
  sqlite3_finalize(stmt);
  sqlite3_close(src);
  Storage.remove(tempDb.c_str());

  // 4. Media: copy ZIP entries referenced by the media mapping to the SD card.
  if (progress) progress(0, 0, "media");
  ZipFile zip(apkgPath);
  size_t mediaJsonSize = 0;
  uint8_t* mediaJsonBuf = zip.readFileToMemory(kMediaEntry, &mediaJsonSize, /*trailingNullByte=*/true);
  if (mediaJsonBuf) {
    result.mediaImported =
        extractMedia(apkgPath, std::string(reinterpret_cast<char*>(mediaJsonBuf), mediaJsonSize),
                     sdMediaDir, store);
    free(mediaJsonBuf);
  }

  result.success = true;
  return result;
}

bool ApkgImporter::extractCollection(const std::string& zipPath, const std::string& tempPath) {
  ZipFile zip(zipPath);
  HalFile out;
  if (!Storage.openFileForWrite("APKG", tempPath, out)) return false;
  const bool ok = zip.readFileToStream(kCollectionEntry, out, 4096);
  out.flush();
  out.close();
  if (!ok) Storage.remove(tempPath.c_str());
  return ok;
}

bool ApkgImporter::parseNoteFields(const std::string& flds, std::string& frontHtml, std::string& backHtml) {
  // Anki note fields are \x1f-separated; field 0 is the front (sfld) and the
  // rest the back. Template rendering ({{Front}} etc.) is out of scope for
  // the Phase-0 prototype — content is stored as HTML and rendered at review.
  //
  // Two Anki-isms need normalizing before storage:
  //  - extra fields (phonetics, examples, notes) arrive separated by \x1f;
  //    join them with <br> instead of leaking a control character that has
  //    no glyph and renders as U+FFFD at review time.
  //  - [sound:file.mp3] tags are stripped until a player exists (see
  //    stripSoundTags), otherwise they would show as literal text on cards.
  const size_t sep = flds.find('\x1f');
  std::string front = sep == std::string::npos ? flds : flds.substr(0, sep);
  stripSoundTags(front);
  frontHtml = std::move(front);

  std::string back;
  if (sep != std::string::npos) {
    size_t start = sep + 1;
    for (;;) {
      const size_t next = flds.find('\x1f', start);
      if (next == std::string::npos) {
        if (start < flds.size()) {
          if (!back.empty()) back += "<br>";
          back.append(flds, start, std::string::npos);
        }
        break;
      }
      if (!back.empty()) back += "<br>";
      back.append(flds, start, next - start);
      start = next + 1;
    }
  }
  stripSoundTags(back);
  backHtml = std::move(back);
  return true;
}

uint32_t ApkgImporter::extractMedia(const std::string& zipPath, const std::string& mediaJson,
                                    const std::string& sdMediaDir, DeckStore& store) {
  std::unordered_map<std::string, std::string> map;
  if (!parseMediaJson(mediaJson.c_str(), map) || map.empty()) return 0;
  if (!Storage.ensureDirectoryExists(sdMediaDir.c_str())) return 0;

  ZipFile zip(zipPath);
  if (!zip.loadAllFileStatSlims()) return 0;  // sizes cached for O(1) lookups

  uint32_t extracted = 0;
  uint32_t mediaWritten = 0;
  zip.enumerateFilePaths([&](std::string_view entry) {
    const std::string key(entry);
    const auto it = map.find(key);
    if (it == map.end()) return;
    const std::string& filename = it->second;
    // Guard against path traversal from a crafted APKG.
    if (filename.find('/') != std::string::npos || filename.find("..") != std::string::npos) return;

    size_t size = 0;
    if (!zip.getInflatedFileSize(key.c_str(), &size)) return;

    const std::string dest = sdMediaDir + "/" + filename;
    HalFile out;
    if (!Storage.openFileForWrite("APKG", dest, out)) return;
    const bool ok = zip.readFileToStream(key.c_str(), out, 2048);
    out.flush();
    out.close();
    if (!ok) {
      Storage.remove(dest.c_str());
      return;
    }
    store.insertMedia(filename, dest, static_cast<uint32_t>(size));
    extracted++;
    // Yield every 16 media files so the idle task can feed the watchdog.
    if ((++mediaWritten & 15) == 0) {
      vTaskDelay(1);
    }
  });
  return extracted;
}

}  // namespace anki

#endif  // ANKIEINK
