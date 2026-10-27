#pragma once
#include <stdint.h>

// PlatformIO selects exactly one of the two implementations in src/.
namespace sampling {
const char *name();
void reset(uint32_t now);
bool ready(uint32_t now);
}
