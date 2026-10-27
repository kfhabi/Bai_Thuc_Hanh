#include "sampling_schedule.h"
#include "th2_config.h"
#include "th2_core.h"

namespace sampling {
namespace { uint32_t previousMs = 0; }
const char *name() { return "millis"; }
void reset(uint32_t now) { previousMs = now; }
bool ready(uint32_t now) {
  if (!th2::sampleDue(now, previousMs, th2config::SAMPLE_PERIOD_MS)) return false;
  // Read one sample; do not create catch-up readings after a slow task.
  previousMs = now;
  return true;
}
}
