// Guarded so non-ANKIEINK builds compile this as an empty translation unit:
// LDF pulls lib/anki into every env (the ANKIEINK-guarded include in
// ActivityManager.cpp is still scanned), and sqlite3.h only exists in the
// m5papers3-anki env.
#ifdef ANKIEINK

#include "DeckStore.h"

#include <sqlite3.h>
#include <string.h>
#include <time.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>

#include <HalStorage.h>
#include <Logging.h>
#ifndef SIMULATOR
#include <SDCardManager.h>
#endif

#include "AnkiDbSchema.h"
#include "AnkiPaths.h"
#include "SdFatVfs.h"

namespace anki {
namespace {

// SQLite calls this every N virtual-machine opcodes while a statement runs.
// APKG-import batch commits on the SD card can exceed the watchdog window
// inside a single sqlite3_step, where no loop-level vTaskDelay can reach;
// yielding from inside the VM keeps the idle task fed. Throttled to ~30 ms so
// normal (short) queries aren't slowed. Requires SQLITE_OMIT_PROGRESS_CALLBACK
// to be undefined — scripts/patch_sqlite3esp32.py re-enables it. Return 0 to
// continue; nonzero would abort the statement.
int yieldProgress(void*) {
  static TickType_t lastYield = 0;
  const TickType_t now = xTaskGetTickCount();
  if (now - lastYield >= pdMS_TO_TICKS(30)) {
    lastYield = now;
    vTaskDelay(1);
  }
  return 0;
}

}  // namespace

// ── Lifecycle ──

DeckStore& DeckStore::getInstance() {
  static DeckStore instance;
  return instance;
}

bool DeckStore::init(const char* dbPath) {
  if (db_) close();

  // Route SQLite file I/O through the SdFat-backed VFS so the database lives
  // on the SD card (SDCardManager's FsVolume). esp32_arduino_sqlite3_lib is
  // compiled with SQLITE_OMIT_WAL, so crash safety comes from the rollback
  // journal instead of WAL — fine for low-frequency review writes.
  // On the desktop simulator registerSdFatVfs() is a no-op: the host sqlite3
  // uses its default unix VFS, so sqlite3_open needs a real filesystem path
  // (openPath below) instead of the SD-logical one.
  registerSdFatVfs();

#ifdef SIMULATOR
  const std::string openPathStorage = Storage.hostPath(dbPath);
  const char* openPath = openPathStorage.c_str();
#else
  const char* openPath = dbPath;
#endif

  // A fresh SD card has no /AnkiEInk directory; sqlite3_open fails when the
  // parent directory of the database is missing, so create it first (mkdir is
  // recursive). Idempotent, so it is safe on every boot.
  if (!Storage.ensureDirectoryExists(kAnkiRootDir)) {
    LOG_ERR("ANKI", "Cannot create %s", kAnkiRootDir);
    return false;
  }

  int rc = sqlite3_open(openPath, reinterpret_cast<sqlite3**>(&db_));
  if (rc != SQLITE_OK) {
    LOG_ERR("ANKI", "Cannot open %s: %s (rc=%d)", dbPath,
            db_ ? sqlite3_errmsg(reinterpret_cast<sqlite3*>(db_)) : "no handle", rc);
    sqlite3_close(reinterpret_cast<sqlite3*>(db_));
    db_ = nullptr;
    return false;
  }

  auto* db = reinterpret_cast<sqlite3*>(db_);

  // sqlite3_open is lazy: a corrupt/stale file from a crashed run only
  // surfaces on the first statement (SQLITE_NOTADB). Probe the file right
  // away and rebuild it instead of failing every init from then on (the
  // PRAGMAs below used to swallow the error, leaving a poisoned file).
  rc = sqlite3_exec(db, "PRAGMA schema_version;", nullptr, nullptr, nullptr);
  if (rc == SQLITE_NOTADB) {
    LOG_ERR("ANKI", "Corrupt database file detected, recreating %s", dbPath);
    sqlite3_close(db);
    db_ = nullptr;
    Storage.remove(dbPath);
    Storage.remove(std::string(dbPath).append("-journal").c_str());
    rc = sqlite3_open(openPath, reinterpret_cast<sqlite3**>(&db_));
    if (rc != SQLITE_OK) {
      LOG_ERR("ANKI", "Cannot recreate %s: %s (rc=%d)", dbPath,
              db_ ? sqlite3_errmsg(reinterpret_cast<sqlite3*>(db_)) : "no handle", rc);
      sqlite3_close(reinterpret_cast<sqlite3*>(db_));
      db_ = nullptr;
      return false;
    }
    db = reinterpret_cast<sqlite3*>(db_);
  } else if (rc != SQLITE_OK) {
    LOG_ERR("ANKI", "Database probe failed: %s (rc=%d)", sqlite3_errmsg(db), rc);
    sqlite3_close(db);
    db_ = nullptr;
    return false;
  }

  sqlite3_exec(reinterpret_cast<sqlite3*>(db_), "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
  sqlite3_exec(reinterpret_cast<sqlite3*>(db_), "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);

  // Let long-running statements on this connection yield from inside the VM
  // (see yieldProgress) so APKG-import batch writes can't starve the idle task.
  sqlite3_progress_handler(db, 256, yieldProgress, nullptr);

  // Create schema
  char* errMsg = nullptr;
  rc = sqlite3_exec(reinterpret_cast<sqlite3*>(db_), CREATE_SCHEMA_SQL, nullptr, nullptr, &errMsg);
  if (rc != SQLITE_OK) {
#ifdef SIMULATOR
    LOG_ERR("ANKI", "Schema creation failed: %s (rc=%d, ext=0x%x)",
            errMsg ? errMsg : "no message", rc,
            sqlite3_extended_errcode(reinterpret_cast<sqlite3*>(db_)));
#else
    LOG_ERR("ANKI", "Schema creation failed: %s (rc=%d, ext=0x%x) SD free=%llu/%llu bytes",
            errMsg ? errMsg : "no message", rc,
            sqlite3_extended_errcode(reinterpret_cast<sqlite3*>(db_)),
            static_cast<unsigned long long>(SDCardManager::getInstance().sdUsedBytes()),
            static_cast<unsigned long long>(SDCardManager::getInstance().sdTotalBytes()));
#endif
    if (errMsg) sqlite3_free(errMsg);
    close();
    return false;
  }
  LOG_DBG("ANKI", "DeckStore init OK");
  return true;
}

void DeckStore::close() {
  if (db_) {
    sqlite3_close(reinterpret_cast<sqlite3*>(db_));
    db_ = nullptr;
  }
}

bool DeckStore::isOpen() const { return db_ != nullptr; }

// ── Deck operations ──

uint32_t DeckStore::upsertDeck(const DeckInfo& deck) {
  auto* db = reinterpret_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "INSERT INTO decks (id, name, description, cardCount, newCount, createdAt, updatedAt) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?) "
                    "ON CONFLICT(id) DO UPDATE SET name=excluded.name, description=excluded.description, "
                    "cardCount=excluded.cardCount, newCount=excluded.newCount, updatedAt=excluded.updatedAt";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    LOG_ERR("ANKI", "upsertDeck prepare failed: %s", sqlite3_errmsg(db));
    return 0;
  }

  if (deck.id) {
    sqlite3_bind_int64(stmt, 1, deck.id);
  } else {
    sqlite3_bind_null(stmt, 1);  // auto-assign rowid
  }
  sqlite3_bind_text(stmt, 2, deck.name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, deck.description.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 4, deck.cardCount);
  sqlite3_bind_int(stmt, 5, deck.newCount);
  sqlite3_bind_int64(stmt, 6, deck.createdAt);
  sqlite3_bind_int64(stmt, 7, deck.updatedAt);

  bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  if (!ok) LOG_ERR("ANKI", "upsertDeck step failed: %s", sqlite3_errmsg(db));
  sqlite3_finalize(stmt);
  return ok ? (deck.id ? deck.id : static_cast<uint32_t>(sqlite3_last_insert_rowid(db))) : 0;
}

std::vector<DeckInfo> DeckStore::listDecks() const {
  std::vector<DeckInfo> result;
  auto* db = reinterpret_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;
  // Counts are computed live instead of reading decks.cardCount/newCount:
  // the importer creates decks with both at 0 and never updates them, so the
  // stored values would show "0 cards" forever. New = getNewCards() semantics
  // (no card_states row, or state 0), counted as total minus started cards;
  // the subtraction rides idx_card_states_state instead of an unindexed
  // LEFT JOIN on card_states(cardId).
  const char* sql =
      "SELECT d.id, d.name, d.description, "
      "(SELECT COUNT(*) FROM cards c WHERE c.deckId = d.id), "
      "(SELECT COUNT(*) FROM cards c WHERE c.deckId = d.id) - "
      "(SELECT COUNT(*) FROM card_states cs WHERE cs.state != 0 "
      "AND EXISTS (SELECT 1 FROM cards c WHERE c.id = cs.cardId AND c.deckId = d.id)), "
      "d.createdAt, d.updatedAt FROM decks d";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
    return result;

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    DeckInfo d;
    d.id = static_cast<uint32_t>(sqlite3_column_int64(stmt, 0));
    d.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    d.description = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    d.cardCount = static_cast<uint32_t>(sqlite3_column_int(stmt, 3));
    d.newCount = static_cast<uint32_t>(sqlite3_column_int(stmt, 4));
    d.createdAt = static_cast<uint32_t>(sqlite3_column_int64(stmt, 5));
    d.updatedAt = static_cast<uint32_t>(sqlite3_column_int64(stmt, 6));
    result.push_back(std::move(d));
  }
  sqlite3_finalize(stmt);
  return result;
}

DeckInfo DeckStore::getDeck(uint32_t deckId) const {
  DeckInfo d;
  auto* db = reinterpret_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "SELECT id, name, description, cardCount, newCount, createdAt, updatedAt FROM decks WHERE id=?";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return d;

  sqlite3_bind_int64(stmt, 1, deckId);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    d.id = static_cast<uint32_t>(sqlite3_column_int64(stmt, 0));
    d.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    d.description = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    d.cardCount = static_cast<uint32_t>(sqlite3_column_int(stmt, 3));
    d.newCount = static_cast<uint32_t>(sqlite3_column_int(stmt, 4));
    d.createdAt = static_cast<uint32_t>(sqlite3_column_int64(stmt, 5));
    d.updatedAt = static_cast<uint32_t>(sqlite3_column_int64(stmt, 6));
  }
  sqlite3_finalize(stmt);
  return d;
}

bool DeckStore::deleteDeck(uint32_t deckId) {
  auto* db = reinterpret_cast<sqlite3*>(db_);

  // Cascade explicitly rather than trusting PRAGMA foreign_keys: the pragma
  // is per-connection state set once at init, and this keeps the cleanup
  // atomic and obvious regardless. Order follows the FK chain
  // (review_logs/card_states → cards → decks), all in one transaction.
  bool ok = sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr) == SQLITE_OK;

  const char* stmts[] = {
      "DELETE FROM review_logs WHERE cardId IN (SELECT id FROM cards WHERE deckId=?)",
      "DELETE FROM card_states WHERE cardId IN (SELECT id FROM cards WHERE deckId=?)",
      "DELETE FROM cards WHERE deckId=?",
      "DELETE FROM decks WHERE id=?",
  };
  for (const char* sql : stmts) {
    if (!ok) break;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
      ok = false;
      break;
    }
    sqlite3_bind_int64(stmt, 1, deckId);
    ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
  }

  sqlite3_exec(db, ok ? "COMMIT;" : "ROLLBACK;", nullptr, nullptr, nullptr);
  return ok;
}

// ── Card operations ──

uint32_t DeckStore::insertCard(const CardInfo& card) {
  auto* db = reinterpret_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO cards (deckId, noteId, frontHtml, backHtml, tags, mediaRefs, createdAt) "
      "VALUES (?, ?, ?, ?, ?, ?, ?)";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;

  sqlite3_bind_int64(stmt, 1, card.deckId);
  sqlite3_bind_int64(stmt, 2, card.noteId);
  sqlite3_bind_text(stmt, 3, card.frontHtml.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, card.backHtml.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, card.tags.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6, card.mediaRefs.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 7, card.createdAt);

  bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  return ok ? static_cast<uint32_t>(sqlite3_last_insert_rowid(db)) : 0;
}

bool DeckStore::insertCards(const std::vector<CardInfo>& cards, std::vector<uint32_t>* outIds) {
  auto* db = reinterpret_cast<sqlite3*>(db_);
  sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO cards (deckId, noteId, frontHtml, backHtml, tags, mediaRefs, createdAt) "
      "VALUES (?, ?, ?, ?, ?, ?, ?)";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }

  if (outIds) outIds->clear();
  for (const auto& card : cards) {
    sqlite3_reset(stmt);
    sqlite3_bind_int64(stmt, 1, card.deckId);
    sqlite3_bind_int64(stmt, 2, card.noteId);
    sqlite3_bind_text(stmt, 3, card.frontHtml.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, card.backHtml.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, card.tags.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, card.mediaRefs.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 7, card.createdAt);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      sqlite3_finalize(stmt);
      sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
      return false;
    }
    if (outIds) outIds->push_back(static_cast<uint32_t>(sqlite3_last_insert_rowid(db)));
  }

  sqlite3_finalize(stmt);
  sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
  return true;
}

CardInfo DeckStore::getCard(uint32_t cardId) const {
  CardInfo c;
  auto* db = reinterpret_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "SELECT id, deckId, noteId, frontHtml, backHtml, tags, mediaRefs, createdAt FROM cards WHERE id=?";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return c;

  sqlite3_bind_int64(stmt, 1, cardId);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    c.id = static_cast<uint32_t>(sqlite3_column_int64(stmt, 0));
    c.deckId = static_cast<uint32_t>(sqlite3_column_int64(stmt, 1));
    c.noteId = static_cast<uint32_t>(sqlite3_column_int64(stmt, 2));
    c.frontHtml = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    c.backHtml = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    c.tags = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
    c.mediaRefs = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
    c.createdAt = static_cast<uint32_t>(sqlite3_column_int64(stmt, 7));
  }
  sqlite3_finalize(stmt);
  return c;
}

std::vector<CardInfo> DeckStore::listCards(uint32_t deckId) const {
  std::vector<CardInfo> result;
  auto* db = reinterpret_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT id, deckId, noteId, frontHtml, backHtml, tags, mediaRefs, createdAt FROM cards WHERE deckId=?";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;

  sqlite3_bind_int64(stmt, 1, deckId);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    CardInfo c;
    c.id = static_cast<uint32_t>(sqlite3_column_int64(stmt, 0));
    c.deckId = static_cast<uint32_t>(sqlite3_column_int64(stmt, 1));
    c.noteId = static_cast<uint32_t>(sqlite3_column_int64(stmt, 2));
    c.frontHtml = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    c.backHtml = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    c.tags = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
    c.mediaRefs = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
    c.createdAt = static_cast<uint32_t>(sqlite3_column_int64(stmt, 7));
    result.push_back(std::move(c));
  }
  sqlite3_finalize(stmt);
  return result;
}

// ── FSRS state operations ──

fsrs::CardState DeckStore::getCardState(uint32_t cardId) const {
  fsrs::CardState state{};
  auto* db = reinterpret_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT state, stability, difficulty, due, lastReview, elapsedDays, scheduledDays, reps, lapses "
      "FROM card_states WHERE cardId=?";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return state;

  sqlite3_bind_int64(stmt, 1, cardId);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    state.state = static_cast<fsrs::State>(sqlite3_column_int(stmt, 0));
    state.stability = sqlite3_column_double(stmt, 1);
    state.difficulty = sqlite3_column_double(stmt, 2);
    state.due = static_cast<uint32_t>(sqlite3_column_int64(stmt, 3));
    state.lastReview = static_cast<uint32_t>(sqlite3_column_int64(stmt, 4));
    state.elapsedDays = sqlite3_column_double(stmt, 5);
    state.scheduledDays = sqlite3_column_double(stmt, 6);
    state.reps = static_cast<uint16_t>(sqlite3_column_int(stmt, 7));
    state.lapses = static_cast<uint16_t>(sqlite3_column_int(stmt, 8));
  }
  sqlite3_finalize(stmt);
  return state;
}

bool DeckStore::updateCardState(uint32_t cardId, const fsrs::CardState& state) {
  auto* db = reinterpret_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO card_states (cardId, state, stability, difficulty, due, lastReview, "
      "elapsedDays, scheduledDays, reps, lapses) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(cardId) DO UPDATE SET state=excluded.state, stability=excluded.stability, "
      "difficulty=excluded.difficulty, due=excluded.due, lastReview=excluded.lastReview, "
      "elapsedDays=excluded.elapsedDays, scheduledDays=excluded.scheduledDays, "
      "reps=excluded.reps, lapses=excluded.lapses";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

  sqlite3_bind_int64(stmt, 1, cardId);
  sqlite3_bind_int(stmt, 2, static_cast<int>(state.state));
  sqlite3_bind_double(stmt, 3, state.stability);
  sqlite3_bind_double(stmt, 4, state.difficulty);
  sqlite3_bind_int64(stmt, 5, state.due);
  sqlite3_bind_int64(stmt, 6, state.lastReview);
  sqlite3_bind_double(stmt, 7, state.elapsedDays);
  sqlite3_bind_double(stmt, 8, state.scheduledDays);
  sqlite3_bind_int(stmt, 9, state.reps);
  sqlite3_bind_int(stmt, 10, state.lapses);

  bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  return ok;
}

bool DeckStore::updateCardStates(const std::vector<uint32_t>& cardIds,
                                 const std::vector<fsrs::CardState>& states) {
  if (cardIds.size() != states.size()) return false;
  if (cardIds.empty()) return true;

  auto* db = reinterpret_cast<sqlite3*>(db_);
  // One transaction for the whole batch: the autocommit-per-row path costs an
  // SD fsync per card, which during APKG import (50+ cards/batch) blocks the
  // calling task for seconds and starves the idle task.
  sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO card_states (cardId, state, stability, difficulty, due, lastReview, "
      "elapsedDays, scheduledDays, reps, lapses) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(cardId) DO UPDATE SET state=excluded.state, stability=excluded.stability, "
      "difficulty=excluded.difficulty, due=excluded.due, lastReview=excluded.lastReview, "
      "elapsedDays=excluded.elapsedDays, scheduledDays=excluded.scheduledDays, "
      "reps=excluded.reps, lapses=excluded.lapses";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }

  bool ok = true;
  for (size_t i = 0; i < cardIds.size(); i++) {
    const auto& s = states[i];
    sqlite3_reset(stmt);
    sqlite3_bind_int64(stmt, 1, cardIds[i]);
    sqlite3_bind_int(stmt, 2, static_cast<int>(s.state));
    sqlite3_bind_double(stmt, 3, s.stability);
    sqlite3_bind_double(stmt, 4, s.difficulty);
    sqlite3_bind_int64(stmt, 5, s.due);
    sqlite3_bind_int64(stmt, 6, s.lastReview);
    sqlite3_bind_double(stmt, 7, s.elapsedDays);
    sqlite3_bind_double(stmt, 8, s.scheduledDays);
    sqlite3_bind_int(stmt, 9, s.reps);
    sqlite3_bind_int(stmt, 10, s.lapses);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      ok = false;
      break;
    }
  }
  sqlite3_finalize(stmt);
  sqlite3_exec(db, ok ? "COMMIT;" : "ROLLBACK;", nullptr, nullptr, nullptr);
  return ok;
}

std::vector<uint32_t> DeckStore::getDueCards(uint32_t nowTimestamp, uint16_t limit) const {
  std::vector<uint32_t> result;
  auto* db = reinterpret_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT cs.cardId FROM card_states cs "
      "WHERE cs.due <= ? AND cs.state != 0 ORDER BY cs.due LIMIT ?";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;

  sqlite3_bind_int64(stmt, 1, nowTimestamp);
  sqlite3_bind_int(stmt, 2, limit);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    result.push_back(static_cast<uint32_t>(sqlite3_column_int64(stmt, 0)));
  }
  sqlite3_finalize(stmt);
  return result;
}

std::vector<uint32_t> DeckStore::getNewCards(uint32_t deckId, uint16_t limit) const {
  std::vector<uint32_t> result;
  auto* db = reinterpret_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT c.id FROM cards c "
      "LEFT JOIN card_states cs ON c.id = cs.cardId "
      "WHERE c.deckId = ? AND (cs.cardId IS NULL OR cs.state = 0) LIMIT ?";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;

  sqlite3_bind_int64(stmt, 1, deckId);
  sqlite3_bind_int(stmt, 2, limit);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    result.push_back(static_cast<uint32_t>(sqlite3_column_int64(stmt, 0)));
  }
  sqlite3_finalize(stmt);
  return result;
}

// ── Review log operations ──

bool DeckStore::logReview(uint32_t cardId, fsrs::Rating rating, float elapsedDays,
                          const fsrs::CardState& prevState, const fsrs::CardState& newState) {
  auto* db = reinterpret_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO review_logs (cardId, rating, elapsedDays, previousStability, previousDifficulty, "
      "newStability, newDifficulty, reviewedAt) VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

  sqlite3_bind_int64(stmt, 1, cardId);
  sqlite3_bind_int(stmt, 2, static_cast<int>(rating));
  sqlite3_bind_double(stmt, 3, elapsedDays);
  sqlite3_bind_double(stmt, 4, prevState.stability);
  sqlite3_bind_double(stmt, 5, prevState.difficulty);
  sqlite3_bind_double(stmt, 6, newState.stability);
  sqlite3_bind_double(stmt, 7, newState.difficulty);
  sqlite3_bind_int64(stmt, 8, static_cast<int64_t>(time(nullptr)));

  bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  return ok;
}

std::vector<fsrs::CardState> DeckStore::getReviewLogs(uint32_t cardId) const {
  std::vector<fsrs::CardState> result;
  auto* db = reinterpret_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT newStability, newDifficulty FROM review_logs WHERE cardId=? ORDER BY reviewedAt";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;

  sqlite3_bind_int64(stmt, 1, cardId);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    fsrs::CardState s{};
    s.stability = sqlite3_column_double(stmt, 0);
    s.difficulty = sqlite3_column_double(stmt, 1);
    result.push_back(s);
  }
  sqlite3_finalize(stmt);
  return result;
}

uint32_t DeckStore::countReviewLogs() const {
  auto* db = reinterpret_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM review_logs", -1, &stmt, nullptr) != SQLITE_OK) return 0;
  uint32_t count = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) count = static_cast<uint32_t>(sqlite3_column_int(stmt, 0));
  sqlite3_finalize(stmt);
  return count;
}

// ── Statistics ──

bool DeckStore::updateDailyStats(fsrs::Rating rating, uint32_t timeSpentSec, bool isNew) {
  auto* db = reinterpret_cast<sqlite3*>(db_);
  // Get today's date string
  time_t now = time(nullptr);
  struct tm tm;
  localtime_r(&now, &tm);
  char dateStr[11];
  snprintf(dateStr, sizeof(dateStr), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO daily_stats (date, reviews, newCards, againCount, hardCount, goodCount, easyCount, timeSpent) "
      "VALUES (?, 1, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(date) DO UPDATE SET reviews=daily_stats.reviews+1, "
      "newCards=daily_stats.newCards+?, timeSpent=daily_stats.timeSpent+?, "
      "againCount=daily_stats.againCount+?, hardCount=daily_stats.hardCount+?, "
      "goodCount=daily_stats.goodCount+?, easyCount=daily_stats.easyCount+?";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

  int isAgain = (rating == fsrs::Rating::Again) ? 1 : 0;
  int isHard = (rating == fsrs::Rating::Hard) ? 1 : 0;
  int isGood = (rating == fsrs::Rating::Good) ? 1 : 0;
  int isEasy = (rating == fsrs::Rating::Easy) ? 1 : 0;
  int newCard = isNew ? 1 : 0;

  sqlite3_bind_text(stmt, 1, dateStr, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 2, newCard);
  sqlite3_bind_int(stmt, 3, isAgain);
  sqlite3_bind_int(stmt, 4, isHard);
  sqlite3_bind_int(stmt, 5, isGood);
  sqlite3_bind_int(stmt, 6, isEasy);
  sqlite3_bind_int(stmt, 7, timeSpentSec);
  // ON CONFLICT update values
  sqlite3_bind_int(stmt, 8, newCard);
  sqlite3_bind_int(stmt, 9, timeSpentSec);
  sqlite3_bind_int(stmt, 10, isAgain);
  sqlite3_bind_int(stmt, 11, isHard);
  sqlite3_bind_int(stmt, 12, isGood);
  sqlite3_bind_int(stmt, 13, isEasy);

  bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  return ok;
}

// ── Media operations ──

bool DeckStore::insertMedia(const std::string& filename, const std::string& sdPath, uint32_t size) {
  auto* db = reinterpret_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO media (filename, sdPath, size) VALUES (?, ?, ?) "
      "ON CONFLICT(filename) DO UPDATE SET sdPath=excluded.sdPath, size=excluded.size";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

  sqlite3_bind_text(stmt, 1, filename.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, sdPath.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 3, size);

  bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  return ok;
}

// ── Maintenance ──

bool DeckStore::checkpoint() {
  auto* db = reinterpret_cast<sqlite3*>(db_);
  return sqlite3_exec(db, "PRAGMA wal_checkpoint(PASSIVE);", nullptr, nullptr, nullptr) == SQLITE_OK;
}

bool DeckStore::integrityCheck() {
  auto* db = reinterpret_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &stmt, nullptr) != SQLITE_OK) return false;
  bool ok = false;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char* result = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    ok = result && strcmp(result, "ok") == 0;
  }
  sqlite3_finalize(stmt);
  return ok;
}

uint64_t DeckStore::dbSize() const {
  auto* db = reinterpret_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "PRAGMA page_count", -1, &stmt, nullptr) != SQLITE_OK) return 0;
  uint64_t pages = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) pages = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
  sqlite3_finalize(stmt);

  if (sqlite3_prepare_v2(db, "PRAGMA page_size", -1, &stmt, nullptr) != SQLITE_OK) return 0;
  uint64_t pageSize = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) pageSize = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
  sqlite3_finalize(stmt);

  return pages * pageSize;
}

}  // namespace anki

#endif  // ANKIEINK
