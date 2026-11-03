#include "th3_core.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {
unsigned checks = 0;

void require(bool condition, const char *message) {
  ++checks;
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

bool reasonIs(const th3::Controller &controller, const char *expected) {
  return std::strcmp(controller.reason(), expected) == 0;
}

void bootAndStartGuards() {
  th3::Controller controller;
  require(controller.state() == th3::State::Stopped && !controller.running(),
          "boot must leave the load stopped");
  require(reasonIs(controller, "boot"), "boot reason is available for logging");
  controller.update(1, true, false, true, false, false, false);
  require(!controller.running(), "network unavailable must block START");
  controller.update(2, true, false, true, false, true, true);
  require(!controller.running(), "configuration must block START");
  controller.update(3, true, true, true, false, true, false);
  require(!controller.running() && reasonIs(controller, "stop"),
          "STOP must win over simultaneous START");
  controller.update(4, true, false, true, true, true, false);
  require(!controller.running(), "RESET must suppress simultaneous START");
  controller.update(5, true, false, true, false, true, false);
  require(controller.running(), "a fresh eligible START must run the load");
  controller.update(6, true, true, true, false, true, false);
  require(!controller.running(), "STOP must also win while running");
}

void faultAndRecovery() {
  th3::Controller controller;
  controller.update(100, true, false, true, false, true, false);
  controller.update(101, true, true, false, true, true, false);
  require(controller.state() == th3::State::Fault && !controller.running(),
          "invalid input must force a safe FAULT even with STOP and RESET");
  require(reasonIs(controller, "input_invalid"), "input fault reason must persist");
  controller.update(102, false, false, false, true, true, false);
  require(controller.state() == th3::State::Fault,
          "RESET cannot clear a fault while input remains invalid");
  controller.update(103, true, false, true, false, true, false);
  require(controller.state() == th3::State::Fault,
          "valid data and START alone cannot clear a latched fault");
  controller.update(104, true, false, true, true, true, false);
  require(controller.state() == th3::State::Stopped && !controller.running(),
          "explicit recovery must stop without consuming simultaneous START");
  require(reasonIs(controller, "fault_reset"), "recovery must have its own reason");
  controller.update(105, true, false, true, false, true, false);
  require(controller.running(), "a fresh START after recovery may run");

  th3::Controller idle;
  idle.update(0, false, false, false, false, false, false);
  require(idle.state() == th3::State::Fault && !idle.running(),
          "invalid input must be detected while stopped too");
}

void timeoutAndRollover() {
  th3::Controller controller(5000);
  controller.update(100, true, false, true, false, true, false);
  controller.update(5099, false, false, true, false, true, false);
  require(controller.running(), "load may run before the timeout boundary");
  controller.update(5100, true, false, true, false, true, false);
  require(!controller.running() && reasonIs(controller, "timeout"),
          "timeout must stop at the boundary despite a concurrent START");
  controller.update(5200, false, false, true, false, true, false);
  require(!controller.running(), "timeout must not automatically restart");
  controller.update(5300, true, false, true, false, true, false);
  controller.update(10299, false, false, true, false, true, false);
  require(controller.running(), "a new run must receive a new timeout interval");
  controller.update(10300, false, false, true, false, true, false);
  require(!controller.running(), "a restarted run must still enforce its timeout");

  const uint32_t started = UINT32_MAX - 49u;
  th3::Controller wrapping(100);
  wrapping.update(started, true, false, true, false, true, false);
  wrapping.update(uint32_t(started + 99u), false, false, true, false, true, false);
  require(wrapping.running(), "rollover must not terminate a run early");
  wrapping.update(uint32_t(started + 100u), false, false, true, false, true, false);
  require(!wrapping.running() && reasonIs(wrapping, "timeout"),
          "elapsed timeout must work across uint32_t rollover");
}

void networkAndConfiguration() {
  th3::Controller controller;
  controller.update(10, true, false, true, false, true, false);
  controller.update(11, false, false, true, false, false, false);
  require(!controller.running() && reasonIs(controller, "network_unavailable"),
          "network loss must stop a running load");
  controller.update(12, false, false, true, false, true, false);
  require(!controller.running(), "network recovery must not automatically restart");
  controller.update(13, true, false, true, false, true, false);
  controller.update(14, false, false, true, false, true, true);
  require(!controller.running() && reasonIs(controller, "configuring"),
          "entering configuration must stop a running load");
  controller.update(15, true, false, true, false, true, true);
  require(!controller.running(), "START must remain blocked in configuration");
  controller.update(16, false, false, true, false, true, false);
  require(!controller.running(), "leaving configuration must not automatically run");
}
}  // namespace

int main() {
  bootAndStartGuards();
  faultAndRecovery();
  timeoutAndRollover();
  networkAndConfiguration();
  std::cout << "PASS: " << checks << " controller checks\n";
  return 0;
}
