// BMP -> raw NeoPixel line file (.neo) conversion.
#pragma once
#include <Arduino.h>
#include <FS.h>
#include "config.h"

// .neo file layout: 32-byte header, then nLines lines of nLeds*3 bytes
// (R,G,B per pixel, in strip order, NOT gamma corrected).
struct NeoHeader {
  char     magic[4];  // "NEOP" (written last, so a half-written file is invalid)
  uint16_t version;
  uint16_t nLeds;
  uint32_t nLines;
  uint32_t lineMax;   // Largest per-line sum of gamma-corrected values (for power)
  uint32_t srcSize;   // Source BMP size, for cache validation
  uint32_t srcTime;   // Source BMP modification time, for cache validation
  uint32_t cfgHash;   // Settings that affect conversion
  uint32_t reserved;
};
static_assert(sizeof(NeoHeader) == 32, "NeoHeader must be 32 bytes");

#define NEO_DATA_OFFSET  sizeof(NeoHeader)
#define NEO_LINE_BYTES   (N_LEDS * 3)

typedef void (*ProgressFn)(float fraction);

// Returns true and fills hdr if neoPath is a complete, current conversion of
// the BMP whose size/time are given.
bool neoCacheValid(fs::FS &fs, const char *neoPath, uint32_t srcSize,
                   uint32_t srcTime, NeoHeader &hdr);

// Converts srcPath to dstPath. On failure returns false and sets err.
bool convertBmp(fs::FS &fs, const char *srcPath, const char *dstPath,
                ProgressFn progress, NeoHeader &hdr, String &err);

// Gamma-corrected value at full brightness (used for power estimation).
uint8_t gammaFull(uint8_t v);
