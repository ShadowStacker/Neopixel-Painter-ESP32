#include "config.h"
#include "Converter.h"
#include <esp_heap_caps.h>
#include <math.h>

#define NEO_VERSION 1

static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t cfgHash() {
  return (uint32_t)NEO_VERSION | ((uint32_t)N_LEDS << 4) |
         ((uint32_t)IMAGE_PREROTATED << 20) | ((uint32_t)SCALE_TO_FIT << 21) |
         ((uint32_t)LED0_AT_TOP << 22);
}

uint8_t gammaFull(uint8_t v) {
  static uint8_t lut[256];
  static bool built = false;
  if (!built) {
    for (int i = 0; i < 256; i++)
      lut[i] = (uint8_t)lroundf(powf(i / 255.0f, GAMMA) * 255.0f);
    built = true;
  }
  return lut[v];
}

// For each LED, the index along the image axis that maps onto the strip
// (image row for natural images, column for pre-rotated), or -1 = black.
static void buildLedMap(int32_t *map, int32_t srcLen) {
  for (int led = 0; led < N_LEDS; led++) {
    int32_t r = LED0_AT_TOP ? led : (N_LEDS - 1 - led); // position from top
#if SCALE_TO_FIT
    map[led] = (int32_t)(((int64_t)(2 * r + 1) * srcLen) / (2 * N_LEDS));
#else
    int32_t s = r - (N_LEDS - srcLen) / 2;              // centre / crop
    map[led] = (s >= 0 && s < srcLen) ? s : -1;
#endif
  }
}

bool neoCacheValid(fs::FS &fs, const char *neoPath, uint32_t srcSize,
                   uint32_t srcTime, NeoHeader &hdr) {
  File f = fs.open(neoPath, FILE_READ);
  if (!f) return false;
  bool ok = f.read((uint8_t *)&hdr, sizeof hdr) == sizeof hdr &&
            memcmp(hdr.magic, "NEOP", 4) == 0 && hdr.version == NEO_VERSION &&
            hdr.nLeds == N_LEDS && hdr.cfgHash == cfgHash() &&
            hdr.srcSize == srcSize && hdr.srcTime == srcTime &&
            hdr.nLines > 0 &&
            f.size() == NEO_DATA_OFFSET + (size_t)hdr.nLines * NEO_LINE_BYTES;
  f.close();
  return ok;
}

bool convertBmp(fs::FS &fs, const char *srcPath, const char *dstPath,
                ProgressFn progress, NeoHeader &hdr, String &err) {
  File in = fs.open(srcPath, FILE_READ);
  if (!in) { err = "can't open"; return false; }

  uint8_t h[54];
  if (in.read(h, sizeof h) != sizeof h || h[0] != 'B' || h[1] != 'M') {
    err = "not a BMP file"; return false;
  }
  uint32_t dataOff = rd32(h + 10), dibSize = rd32(h + 14), comp = rd32(h + 30);
  int32_t  width   = (int32_t)rd32(h + 18), height = (int32_t)rd32(h + 22);
  uint16_t planes  = rd16(h + 26), bpp = rd16(h + 28);
  if (dibSize < 40 || planes != 1 ||
      !((bpp == 24 && comp == 0) || (bpp == 32 && (comp == 0 || comp == 3)))) {
    err = "unsupported BMP (save as uncompressed 24-bit)"; return false;
  }
  bool bottomUp = height > 0;
  if (height < 0) height = -height;
  if (width <= 0 || height <= 0) { err = "empty image"; return false; }

  const uint32_t bytesPP = bpp / 8;
  const uint32_t rowSize = (width * bytesPP + 3) & ~3u;
  auto rowOffset = [&](int32_t y) -> uint32_t { // y = 0 is the top row
    int32_t fileRow = bottomUp ? (height - 1 - y) : y;
    return dataOff + (uint32_t)fileRow * rowSize;
  };
  if (in.size() < rowOffset(bottomUp ? 0 : height - 1) + rowSize) {
    err = "file truncated"; return false;
  }

  memset(&hdr, 0, sizeof hdr);
  hdr.version = NEO_VERSION;
  hdr.nLeds   = N_LEDS;
  hdr.nLines  = IMAGE_PREROTATED ? height : width;
  hdr.srcSize = in.size();
  hdr.srcTime = (uint32_t)in.getLastWrite();
  hdr.cfgHash = cfgHash();

  fs.remove(dstPath);
  File out = fs.open(dstPath, FILE_WRITE);
  if (!out) { err = "can't create cache file"; return false; }
  out.write((const uint8_t *)&hdr, sizeof hdr); // magic still blank

  static int32_t ledMap[N_LEDS];
  buildLedMap(ledMap, IMAGE_PREROTATED ? width : height);

  uint32_t lineMax = 0;
  bool     ok      = true;
  auto emit = [&](const uint8_t *line) {
    uint32_t sum = 0;
    for (int i = 0; i < NEO_LINE_BYTES; i++) sum += gammaFull(line[i]);
    if (sum > lineMax) lineMax = sum;
    if (out.write(line, NEO_LINE_BYTES) != NEO_LINE_BYTES) ok = false;
  };

#if IMAGE_PREROTATED
  // Each BMP row is one painted line, bottom row first; columns -> pixels.
  uint8_t *row  = (uint8_t *)malloc(rowSize);
  uint8_t *line = (uint8_t *)malloc(NEO_LINE_BYTES);
  if (!row || !line) { err = "out of memory"; ok = false; }
  for (int32_t t = 0; ok && t < height; t++) {
    in.seek(rowOffset(height - 1 - t));
    if (in.read(row, rowSize) != rowSize) { err = "read error"; ok = false; break; }
    for (int led = 0; led < N_LEDS; led++) {
      uint8_t *d = line + led * 3;
      int32_t  x = ledMap[led];
      if (x < 0) { d[0] = d[1] = d[2] = 0; continue; }
      const uint8_t *p = row + x * bytesPP;   // BMP stores B,G,R
      d[0] = p[2]; d[1] = p[1]; d[2] = p[0];
    }
    emit(line);
    if (progress && (t & 15) == 0) progress((float)t / height);
  }
  free(row);
  free(line);
#else
  // Each BMP column is one painted line, left column first. BMP is stored
  // in rows, so transpose a chunk of columns at a time: for each pixel,
  // read that row's slice of the chunk, then write out the chunk's lines.
  size_t   largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  uint32_t chunk   = (largest / 2) / (NEO_LINE_BYTES + bytesPP);
  if (chunk < 8) chunk = 8;
  if (chunk > 512) chunk = 512;
  if (chunk > (uint32_t)width) chunk = width;
  uint8_t *lines = (uint8_t *)malloc(chunk * NEO_LINE_BYTES);
  uint8_t *seg   = (uint8_t *)malloc(chunk * bytesPP);
  if (!lines || !seg) { err = "out of memory"; ok = false; }

  for (int32_t x0 = 0; ok && x0 < width; x0 += chunk) {
    uint32_t n = min<uint32_t>(chunk, width - x0);
    memset(lines, 0, n * NEO_LINE_BYTES);
    int32_t lastY = -1;
    for (int led = 0; led < N_LEDS && ok; led++) {
      int32_t y = ledMap[led];
      if (y < 0) continue;
      if (y != lastY) { // consecutive pixels often share a row when upscaling
        in.seek(rowOffset(y) + x0 * bytesPP);
        if (in.read(seg, n * bytesPP) != n * bytesPP) {
          err = "read error"; ok = false; break;
        }
        lastY = y;
      }
      for (uint32_t c = 0; c < n; c++) {
        const uint8_t *p = seg + c * bytesPP;
        uint8_t       *d = lines + c * NEO_LINE_BYTES + led * 3;
        d[0] = p[2]; d[1] = p[1]; d[2] = p[0];
      }
    }
    for (uint32_t c = 0; ok && c < n; c++) emit(lines + c * NEO_LINE_BYTES);
    if (progress) progress((float)(x0 + n) / width);
  }
  free(lines);
  free(seg);
#endif

  in.close();
  if (ok) {
    memcpy(hdr.magic, "NEOP", 4);
    hdr.lineMax = lineMax;
    ok = out.seek(0) && out.write((const uint8_t *)&hdr, sizeof hdr) == sizeof hdr;
    if (!ok) err = "write error (card full?)";
  } else if (err.length() == 0) {
    err = "write error (card full?)";
  }
  out.close();
  if (!ok) fs.remove(dstPath);
  return ok;
}
