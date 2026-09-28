#pragma once

#include <cstdint>

// libobs stamps frames and packets with os_gettime_ns() (QPC, time since boot).
// Converts such a timestamp to Unix epoch milliseconds, the clock DPM uses for game events.
double obs_ns_to_wall_ms(uint64_t obs_ns);
