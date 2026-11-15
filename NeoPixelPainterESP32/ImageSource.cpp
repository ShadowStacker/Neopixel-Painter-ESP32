#include "config.h"
#include "Converter.h"
#include "ImageSource.h"
#include <esp_heap_caps.h>
#include <esp_timer.h>

static uint8_t *allocImage(size_t bytes) {
  if (psramFound()) {
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return (uint8_t *)p;
  }
  size_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (bytes + RAM_RESERVE > freeInternal) return nullptr;
  return (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

bool ImageSource::open(fs::FS &fs, const char *path, uint32_t lines) {
  close();
  nLines = lines;
  file   = fs.open(path, FILE_READ);
  if (!file) return false;

  size_t bytes = (size_t)lines * NEO_LINE_BYTES;
  ram = allocImage(bytes);
  if (ram) { // Preload whole image
    file.seek(NEO_DATA_OFFSET);
    bool ok = file.read(ram, bytes) == bytes;
    file.close();
    ramLine = 0;
    if (!ok) { free(ram); ram = nullptr; }
    return ok;
  }

  // Stream mode
  ring = (uint8_t *)malloc(STREAM_RING_LINES * NEO_LINE_BYTES);
  if (!ring) { file.close(); return false; }
  produced = 0;
  consumed = 0;
  stopReq  = false;
  readerDone = false;
  // Run the reader on the core that is NOT running loop() / the LEDs.
  BaseType_t core = (portNUM_PROCESSORS > 1) ? (1 - xPortGetCoreID()) : 0;
  if (xTaskCreatePinnedToCore(readerTrampoline, "neoReader", 4096, this,
                              configMAX_PRIORITIES - 2, nullptr, core) != pdPASS) {
    readerDone = true;
    close();
    return false;
  }
  // Prefill the ring before playback can start (up to 2 s).
  uint32_t want = min<uint32_t>(STREAM_RING_LINES, lines);
  uint32_t t0   = millis();
  while (produced.load() < want && millis() - t0 < 2000) delay(1);
  return produced.load() > 0;
}

void ImageSource::close() {
  if (!readerDone.load()) {
    stopReq = true;
    while (!readerDone.load()) delay(1);
  }
  if (file) file.close();
  free(ring); ring = nullptr;
  free(ram);  ram  = nullptr;
}

void ImageSource::readerTrampoline(void *arg) {
  static_cast<ImageSource *>(arg)->readerLoop();
  vTaskDelete(nullptr);
}

void ImageSource::readerLoop() {
  uint32_t fileLine = 0;
  while (!stopReq.load()) {
    uint32_t p = produced.load(std::memory_order_acquire);
    if (p - consumed.load(std::memory_order_acquire) >= STREAM_RING_LINES) {
      vTaskDelay(1); // ring full
      continue;
    }
    if (fileLine == 0) file.seek(NEO_DATA_OFFSET); // wrap to first line
    uint8_t *slot = ring + (p % STREAM_RING_LINES) * NEO_LINE_BYTES;
    if (file.read(slot, NEO_LINE_BYTES) != NEO_LINE_BYTES) {
      memset(slot, 0, NEO_LINE_BYTES); // shouldn't happen; keep line count right
      file.seek(NEO_DATA_OFFSET + (size_t)(fileLine + 1) * NEO_LINE_BYTES);
    }
    if (++fileLine >= nLines) fileLine = 0;
    produced.store(p + 1, std::memory_order_release);
  }
  readerDone = true;
}

const uint8_t *ImageSource::peek(uint32_t timeoutUs) {
  if (ram) return ram + (size_t)ramLine * NEO_LINE_BYTES;
  if (!ring) return nullptr;
  int64_t deadline = esp_timer_get_time() + timeoutUs;
  uint32_t c = consumed.load(std::memory_order_relaxed);
  while (produced.load(std::memory_order_acquire) == c) {
    if (esp_timer_get_time() > deadline) return nullptr;
    vTaskDelay(1); // underrun: let the reader task run
  }
  return ring + (c % STREAM_RING_LINES) * NEO_LINE_BYTES;
}

void ImageSource::advance() {
  if (ram) {
    if (++ramLine >= nLines) ramLine = 0;
  } else {
    consumed.fetch_add(1, std::memory_order_release);
  }
}
