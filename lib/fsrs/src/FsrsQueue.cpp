#include "FsrsQueue.h"

#include <algorithm>
#include <cstddef>

namespace fsrs {

FsrsQueue::FsrsQueue(const FsrsConfig& config) : config_(config) {}

FsrsQueue::Session FsrsQueue::buildSession(uint32_t todayTimestamp, const std::vector<uint32_t>& dueIds,
                                           const std::vector<uint32_t>& newIds) const {
  Session session;
  session.timestamp = todayTimestamp;
  session.reviewsDue = static_cast<uint16_t>(std::min<size_t>(dueIds.size(), 65535));
  session.newAvailable = static_cast<uint16_t>(std::min<size_t>(newIds.size(), 65535));

  // A limit of 0 disables that card type (Anki semantics); otherwise cap the
  // session to the configured daily limits.
  const size_t reviewCount =
      config_.dailyReviewLimit == 0 ? 0 : std::min(dueIds.size(), static_cast<size_t>(config_.dailyReviewLimit));
  session.reviewIds.assign(dueIds.begin(), dueIds.begin() + reviewCount);

  const size_t newCount =
      config_.dailyNewLimit == 0 ? 0 : std::min(newIds.size(), static_cast<size_t>(config_.dailyNewLimit));
  session.newIds.assign(newIds.begin(), newIds.begin() + newCount);

  lastSession_ = session;
  return session;
}

bool FsrsQueue::hasRemaining() const {
  return !lastSession_.reviewIds.empty() || !lastSession_.newIds.empty();
}

uint16_t FsrsQueue::countDueReviews(const std::vector<uint32_t>& dueIds) const {
  return static_cast<uint16_t>(std::min<size_t>(dueIds.size(), 65535));
}

}  // namespace fsrs
