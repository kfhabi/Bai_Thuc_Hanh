#pragma once

#include <stdint.h>

namespace th3 {

enum class State { Stopped, Running, Fault };

inline const char *stateName(State state) {
  switch (state) {
    case State::Stopped: return "STOPPED";
    case State::Running: return "RUNNING";
    case State::Fault: return "FAULT";
  }
  return "UNKNOWN";
}

// Hardware-independent policy. Callers derive the physical output from running().
// A fault has priority over STOP; both paths keep the output safe.
class Controller {
 public:
  explicit Controller(uint32_t timeoutMs = 5000)
      : state_(State::Stopped), reason_("boot"), timeoutMs_(timeoutMs),
        runStartedMs_(0) {}

  State state() const { return state_; }
  const char *reason() const { return reason_; }
  bool running() const { return state_ == State::Running; }

  void update(uint32_t nowMs, bool start, bool stop, bool inputValid,
              bool resetFault, bool networkReady, bool configuring) {
    if (!inputValid) {
      setState(State::Fault, "input_invalid");
      return;
    }

    // Recovery always stops here: RESET and START together never restart a load.
    if (state_ == State::Fault) {
      if (resetFault) setState(State::Stopped, "fault_reset");
      return;
    }

    if (state_ == State::Running) {
      if (stop) setState(State::Stopped, "stop");
      else if (configuring) setState(State::Stopped, "configuring");
      else if (!networkReady) setState(State::Stopped, "network_unavailable");
      // Unsigned elapsed time remains correct across a uint32_t millis rollover.
      else if (uint32_t(nowMs - runStartedMs_) >= timeoutMs_)
        setState(State::Stopped, "timeout");
      return;
    }

    if (stop) {
      reason_ = "stop";
      return;
    }
    if (resetFault) return;
    if (configuring) {
      if (start) reason_ = "configuring";
      return;
    }
    if (!networkReady) {
      if (start) reason_ = "network_unavailable";
      return;
    }
    if (start) {
      runStartedMs_ = nowMs;
      setState(State::Running, "start");
    }
  }

 private:
  void setState(State next, const char *reason) {
    state_ = next;
    reason_ = reason;
  }

  State state_;
  const char *reason_;
  uint32_t timeoutMs_;
  uint32_t runStartedMs_;
};

}  // namespace th3
