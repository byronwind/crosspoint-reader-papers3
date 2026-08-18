#pragma once

namespace anki {

/// SQL to create the local AnkiEInk database schema (SD card storage).
/// esp32_arduino_sqlite3_lib is compiled with SQLITE_OMIT_WAL, so crash safety
/// comes from the rollback journal (synchronous=NORMAL) — fine for
/// low-frequency review writes on the SD card.
static constexpr const char* CREATE_SCHEMA_SQL = R"SQL(
-- Deck metadata
CREATE TABLE IF NOT EXISTS decks (
  id          INTEGER PRIMARY KEY,
  name        TEXT NOT NULL,
  description TEXT DEFAULT '',
  cardCount   INTEGER DEFAULT 0,
  newCount    INTEGER DEFAULT 0,
  createdAt   INTEGER DEFAULT 0,
  updatedAt   INTEGER DEFAULT 0
);

-- Individual cards (content)
CREATE TABLE IF NOT EXISTS cards (
  id          INTEGER PRIMARY KEY,
  deckId      INTEGER NOT NULL REFERENCES decks(id) ON DELETE CASCADE,
  noteId      INTEGER NOT NULL,
  frontHtml   TEXT NOT NULL,
  backHtml    TEXT NOT NULL,
  tags        TEXT DEFAULT '',
  mediaRefs   TEXT DEFAULT '',  -- JSON array of media filenames
  createdAt   INTEGER DEFAULT 0
);

-- FSRS scheduling state (separate from card content)
CREATE TABLE IF NOT EXISTS card_states (
  cardId        INTEGER PRIMARY KEY REFERENCES cards(id) ON DELETE CASCADE,
  state         INTEGER DEFAULT 0,   -- 0=New, 1=Learning, 2=Review, 3=Relearning
  stability     REAL DEFAULT 0.0,
  difficulty    REAL DEFAULT 0.0,
  due           INTEGER DEFAULT 0,   -- Unix timestamp
  lastReview    INTEGER DEFAULT 0,
  elapsedDays   REAL DEFAULT 0.0,
  scheduledDays REAL DEFAULT 0.0,
  reps          INTEGER DEFAULT 0,
  lapses        INTEGER DEFAULT 0
);

-- Review log for FSRS optimization
CREATE TABLE IF NOT EXISTS review_logs (
  id              INTEGER PRIMARY KEY AUTOINCREMENT,
  cardId          INTEGER NOT NULL REFERENCES cards(id) ON DELETE CASCADE,
  rating          INTEGER NOT NULL,  -- 1=Again, 2=Hard, 3=Good, 4=Easy
  elapsedDays     REAL NOT NULL,
  previousStability     REAL DEFAULT 0.0,
  previousDifficulty    REAL DEFAULT 0.0,
  newStability          REAL DEFAULT 0.0,
  newDifficulty         REAL DEFAULT 0.0,
  reviewedAt      INTEGER NOT NULL   -- Unix timestamp
);

-- Media file index
CREATE TABLE IF NOT EXISTS media (
  id          INTEGER PRIMARY KEY AUTOINCREMENT,
  filename    TEXT NOT NULL UNIQUE,
  sdPath      TEXT NOT NULL,         -- Path on SD card (bucketed)
  size        INTEGER DEFAULT 0,
  referenced  INTEGER DEFAULT 1      -- 1=referenced by cards, 0=orphan
);

-- Daily review statistics
CREATE TABLE IF NOT EXISTS daily_stats (
  date        TEXT NOT NULL,          -- YYYY-MM-DD
  reviews     INTEGER DEFAULT 0,
  newCards    INTEGER DEFAULT 0,
  againCount  INTEGER DEFAULT 0,
  hardCount   INTEGER DEFAULT 0,
  goodCount   INTEGER DEFAULT 0,
  easyCount   INTEGER DEFAULT 0,
  timeSpent   INTEGER DEFAULT 0,     -- seconds
  PRIMARY KEY (date)
);

-- Indexes for efficient querying
CREATE INDEX IF NOT EXISTS idx_cards_deck ON cards(deckId);
CREATE INDEX IF NOT EXISTS idx_card_states_due ON card_states(due);
CREATE INDEX IF NOT EXISTS idx_card_states_state ON card_states(state);
CREATE INDEX IF NOT EXISTS idx_review_logs_card ON review_logs(cardId);
CREATE INDEX IF NOT EXISTS idx_review_logs_date ON review_logs(reviewedAt);
CREATE INDEX IF NOT EXISTS idx_media_filename ON media(filename);

-- PRAGMA settings (WAL is compiled out of the SQLite build; journal is used)
PRAGMA synchronous=NORMAL;
PRAGMA foreign_keys=ON;
)SQL";

}  // namespace anki
