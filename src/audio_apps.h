#pragma once

#include <obs.hpp>

// Applications with an audio session on any active output device: what the mixer offers to add
// as a track. Sessions of pid 0 (Windows' own sounds) cannot be captured per process and are left out.
// Fills `apps` with [{executable, pid, active}], one entry per executable.
void list_audio_apps(obs_data_array_t *apps);
