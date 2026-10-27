#include <Arduino.h>
#include "sampling_schedule.h"
#include "th2_config.h"

namespace sampling {
const char *name() { return "delay"; }
void reset(uint32_t now) { (void)now; }
bool ready(uint32_t now) {
  (void)now;
  // Intentionally pauses button handling for the comparison experiment.
  delay(th2config::SAMPLE_PERIOD_MS);
  return true;
}
}
