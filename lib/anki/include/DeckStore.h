#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "FsrsTypes.h"

namespace anki {

/// Deck metadata
struct DeckInfo {
  uint32_t id = 0;
  std::string name;
  std::string description;
  uint32_t cardCount = 0;
  uint32_t newCount = 0;
  uint32_t createdAt = 0;
  uint32_t updatedAt = 0;
};

/// Card content (front/back HTML + metadata)
struct CardInfo {
  uint32_t id = 0;
  uint32_t deckId = 0;
  uint32_t noteId = 0;
  std::string frontHtml;
  std::string backHtml;
  std::string tags;
  std::string mediaRefs;  // JSON array of filenames
  uint32_t createdAt = 0;
};

/// Card with full scheduling state
struct CardWithState {
  CardInfo card;
  fsrs::CardState fsrsState;
};

/**
 * DeckStore — SQLite DAO for AnkiEInk database.
 *
 * Wraps all database operations (CRUD for decks, cards, card_states,
 * review_logs, media, daily_stats). All methods are thread-safe
 * when called from a single task; concurrent access requires external
 * synchronization or SQLite's built-in mutex.
 */
class DeckStore {
 public:
  /// Process-wide singleton (initialized once from main.cpp under ANKIEINK)
  static DeckStore& getInstance();

  /// Initialize the database (create schema if needed)
  bool init(const char* dbPath);

  /// Close the database connection
  void close();

  /// Check if database is open
  bool isOpen() const;

  // ── Deck operations ──

  /// Create or update a deck, returns deck ID
  uint32_t upsertDeck(const DeckInfo& deck);

  /// Get all decks
  std::vector<DeckInfo> listDecks() const;

  /// Get a single deck by ID
  DeckInfo getDeck(uint32_t deckId) const;

  /// Delete a deck and all its cards
  bool deleteDeck(uint32_t deckId);

  // ── Card operations ──

  /// Insert a card, returns card ID
  uint32_t insertCard(const CardInfo& card);

  /// Batch insert cards (for APKG import). When outIds is given, receives
  /// the assigned local card ids in the same order as `cards`.
  bool insertCards(const std::vector<CardInfo>& cards, std::vector<uint32_t>* outIds = nullptr);

  /// Get card content by ID
  CardInfo getCard(uint32_t cardId) const;

  /// Get all cards for a deck
  std::vector<CardInfo> listCards(uint32_t deckId) const;

  // ── FSRS state operations ──

  /// Get card's FSRS state
  fsrs::CardState getCardState(uint32_t cardId) const;

  /// Update card's FSRS state after a review
  bool updateCardState(uint32_t cardId, const fsrs::CardState& state);

  /// Batch-update FSRS states in a single transaction (APKG import). The two
  /// vectors are parallel (index i pairs cardIds[i] with states[i]). One
  /// commit/fsync instead of one per card, which is dramatically cheaper on
  /// SD-card storage and keeps any single import batch short.
  bool updateCardStates(const std::vector<uint32_t>& cardIds, const std::vector<fsrs::CardState>& states);

  /// Get due review cards (where due <= now and state != New)
  std::vector<uint32_t> getDueCards(uint32_t nowTimestamp, uint16_t limit = 200) const;

  /// Get new cards for a deck (state == New)
  std::vector<uint32_t> getNewCards(uint32_t deckId, uint16_t limit = 20) const;

  // ── Review log operations ──

  /// Record a review log entry
  bool logReview(uint32_t cardId, fsrs::Rating rating, float elapsedDays,
                 const fsrs::CardState& prevState, const fsrs::CardState& newState);

  /// Get all review logs (for FSRS optimization)
  std::vector<fsrs::CardState> getReviewLogs(uint32_t cardId) const;

  /// Count total review log entries
  uint32_t countReviewLogs() const;

  // ── Statistics ──

  /// Update daily stats for today
  bool updateDailyStats(fsrs::Rating rating, uint32_t timeSpentSec, bool isNew);

  /// Get statistics for a date range
  // TODO: implement with date range query

  // ── Media operations ──

  /// Record a media file (idempotent by filename)
  bool insertMedia(const std::string& filename, const std::string& sdPath, uint32_t size);

  // ── Maintenance ──

  /// Run WAL checkpoint
  bool checkpoint();

  /// Run integrity check
  bool integrityCheck();

  /// Get database file size in bytes
  uint64_t dbSize() const;

 private:
  void* db_ = nullptr;  // sqlite3* handle (opaque to avoid header dependency)
};

}  // namespace anki

// Helper macro to access the deck store singleton
#define ANKI_STORE anki::DeckStore::getInstance()
