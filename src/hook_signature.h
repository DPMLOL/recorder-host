#pragma once

#include <string>

// Whether game_capture can capture League here: Vanguard refuses an unsigned graphics hook, and a refused hook is
// retried by game_capture every second or two for as long as the source lives, which stutters the game.
struct HookSignature {
  // Every file game_capture would use for a 64-bit game has a valid, trusted Authenticode signature.
  bool trusted = false;
  // Why not, e.g. "graphics-hook64.dll is not signed"; empty when trusted.
  std::string reason;
};

// Checks the files game_capture's compatibility hook uses for a 64-bit game (League): inject-helper64.exe and
// graphics-hook64.dll, the ProgramData copy when win-capture installed one (that is the file it injects), else
// the plugin's own. Call after the modules are loaded: win-capture refreshes the copy as it loads.
HookSignature check_hook_signature();
