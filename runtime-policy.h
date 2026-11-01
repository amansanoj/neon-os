#pragma once
#include <cstdint>

namespace Runtime {
// Elapsed-time comparisons remain correct across millis() rollover. Periodic
// callers refresh timestamps even when idle; no ancient signed deadlines.
inline bool elapsed(uint32_t now, uint32_t since, uint32_t period) {
  return static_cast<uint32_t>(now - since) >= period;
}

class DeferredWrite {
 public:
  void changed(uint32_t now) { dirty_ = true; changedAt_ = now; }
  bool due(uint32_t now) const {
    return dirty_ && elapsed(now, changedAt_, 5000) &&
           (!written_ || elapsed(now, writtenAt_, 60000));
  }
  void committed(uint32_t now) { dirty_ = false; written_ = true; writtenAt_ = now; }
  void retryLater(uint32_t now) { changedAt_ = now; }
 private:
  bool dirty_ = false, written_ = false;
  uint32_t changedAt_ = 0, writtenAt_ = 0;
};
}  // namespace Runtime
