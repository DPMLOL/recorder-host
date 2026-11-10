#pragma once

#include <cstdint>
#include <string>

// Writes a 24-bit BGR image as a JPEG with the Windows Imaging Component (part of Windows: no new dependency).
// The file is written to `<path>.tmp`, then renamed over `path`, so a reader never sees half a file.
// The calling thread must have initialised COM.
bool write_jpeg(const std::string &path, const uint8_t *bgr, uint32_t width, uint32_t height, uint32_t stride,
                float quality, std::string &error);
