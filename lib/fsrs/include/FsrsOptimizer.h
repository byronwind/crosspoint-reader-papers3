#pragma once

#include <cstdint>
#include <functional>
#include <vector>
#include "FsrsTypes.h"

namespace fsrs {

/**
 * FsrsOptimizer — FSRS parameter optimizer using gradient descent.
 *
 * Runs on Core 1 as a background FreeRTOS task. Requires >= 1000 review
 * log entries to produce meaningful results. Iterates up to 100 rounds
 * or until convergence (delta < threshold).
 *
 * Usage:
 *   FsrsOptimizer optimizer;
 *   optimizer.startAsync(reviewLogs, currentConfig, [](const FsrsConfig& optimized) {
 *     // Save optimized parameters to NVS
 *   });
 */
class FsrsOptimizer {
 public:
  /// Review log entry for optimization
  struct ReviewLogEntry {
    uint32_t cardId;
    Rating rating;
    float elapsedDays;
    float previousStability;
    float previousDifficulty;
  };

  /// Progress callback: called periodically during optimization
  using ProgressCallback = std::function<void(float progress, float loss)>;
  /// Completion callback: called when optimization finishes
  using CompletionCallback = std::function<void(const FsrsConfig& optimizedConfig)>;

  /// Start async optimization on Core 1 background task
  /// @param logs       Review log entries (must have >= 1000 entries)
  /// @param config     Current FSRS configuration (starting point)
  /// @param onComplete Called with optimized parameters
  /// @param onProgress Optional progress callback
  void startAsync(const std::vector<ReviewLogEntry>& logs, const FsrsConfig& config,
                  CompletionCallback onComplete, ProgressCallback onProgress = nullptr);

  /// Check if optimization is currently running
  bool isRunning() const;

  /// Cancel a running optimization
  void cancel();

 private:
  bool running_ = false;

  /// Core optimization loop (runs on background task)
  void optimize(const std::vector<ReviewLogEntry>& logs, FsrsConfig& config, uint16_t maxIterations = 100,
                float convergenceThreshold = 0.001f);

  /// Compute loss (log-loss / cross-entropy) for given parameters
  float computeLoss(const std::vector<ReviewLogEntry>& logs, const FsrsConfig& config) const;

  /// FreeRTOS task trampoline
  static void taskTrampoline(void* param);
};

}  // namespace fsrs
