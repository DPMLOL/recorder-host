#include "wall_clock.h"

#include <util/platform.h>
#include <windows.h>

namespace {

constexpr int64_t kFiletimeToUnixMs = 11644473600000LL;

double now_wall_ms() {
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  ULARGE_INTEGER t;
  t.LowPart = ft.dwLowDateTime;
  t.HighPart = ft.dwHighDateTime;
  return static_cast<double>(t.QuadPart) / 10000.0 - static_cast<double>(kFiletimeToUnixMs);
}

}  // namespace

double obs_ns_to_wall_ms(uint64_t obs_ns) {
  // Sample both clocks back to back; the pair is re-taken on every call so wall clock adjustments are followed.
  const uint64_t obs_now = os_gettime_ns();
  const double wall_now = now_wall_ms();
  return wall_now - static_cast<double>(static_cast<int64_t>(obs_now - obs_ns)) / 1e6;
}
