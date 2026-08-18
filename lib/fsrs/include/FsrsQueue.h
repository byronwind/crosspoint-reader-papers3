#pragma once

#include <cstdint>
#include <vector>
#include "FsrsTypes.h"

namespace fsrs {

/**
 * FsrsQueue — Daily review queue generator.
 *
 * Pure logic: the caller (DeckStore/ReviewActivity) supplies the raw due and
 * new card id lists; this class applies the daily limits from FsrsConfig and
 * produces the ordered session (reviews first, then new cards). Kept free of
 * SQLite so the host build's unit tests can link it standalone.
 *
 * Usage:
 *   FsrsQueue queue(config);
 *   auto session = queue.buildSession(today, dueIds, newIds);
 *   // session.reviewIds + session.newIds contain the card IDs to review
 */
class FsrsQueue {
 public:
  explicit FsrsQueue(const FsrsConfig& config);

  /// A review session containing ordered card IDs
  struct Session {
    std::vector<uint32_t> reviewIds;  ///< Due review cards (capped by dailyReviewLimit)
    std::vector<uint32_t> newIds;     ///< New cards (capped by dailyNewLimit)
    uint32_t timestamp = 0;           ///< Session creation time
    uint16_t reviewsDue = 0;          ///< Total reviews that were due
    uint16_t newAvailable = 0;        ///< Total new cards available
  };

  /// Build a review session for the given day from raw card id lists.
  /// A limit of 0 disables that card type (matching Anki semantics).
  Session buildSession(uint32_t todayTimestamp, const std::vector<uint32_t>& dueIds,
                       const std::vector<uint32_t>& newIds) const;

  /// True when the most recently built session still has cards to review.
  bool hasRemaining() const;

  /// Count of due reviews (clamped to uint16 range).
  uint16_t countDueReviews(const std::vector<uint32_t>& dueIds) const;

 private:
  FsrsConfig config_;
  mutable Session lastSession_;
};

}  // namespace fsrs
