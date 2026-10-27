#pragma once

#include <math.h>
#include <stdint.h>

namespace th2 {
// Only valid sensor readings should be passed to add(). Missing readings
// neither advance the window nor become artificial zero measurements.
class MovingAverage5 {
 public:
  void clear() {
    count_ = 0;
    index_ = 0;
    sum_ = 0;
    for (float &value : values_) value = 0;
  }
  void add(float value) {
    if (!isfinite(value)) return;
    if (count_ == 5) sum_ -= values_[index_];
    else ++count_;
    values_[index_] = value;
    sum_ += value;
    index_ = (index_ + 1) % 5;
  }
  bool ready() const { return count_ == 5; }
  float value() const { return ready() ? sum_ / 5.0f : NAN; }
  uint8_t count() const { return count_; }

 private:
  float values_[5] = {};
  float sum_ = 0;
  uint8_t count_ = 0;
  uint8_t index_ = 0;
};

class DebouncedButton {
 public:
  explicit DebouncedButton(uint32_t debounceMs) : debounceMs_(debounceMs) {}
  // Returns true exactly when a stable press is recognized.
  bool update(bool pressed, uint32_t now) {
    if (pressed != raw_) {
      raw_ = pressed;
      changedAt_ = now;
      ++rawEdges_;
    }
    if (raw_ != stable_ && uint32_t(now - changedAt_) >= debounceMs_) {
      stable_ = raw_;
      if (stable_) return true;
    }
    return false;
  }
  bool pressed() const { return stable_; }
  uint32_t rawEdges() const { return rawEdges_; }

 private:
  uint32_t debounceMs_;
  uint32_t changedAt_ = 0;
  uint32_t rawEdges_ = 0;
  bool raw_ = false;
  bool stable_ = false;
};

inline bool sampleDue(uint32_t now, uint32_t previous, uint32_t period) {
  return uint32_t(now - previous) >= period;
}
}  // namespace th2
