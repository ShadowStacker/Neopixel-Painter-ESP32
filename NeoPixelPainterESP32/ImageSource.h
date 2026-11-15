// Delivers lines of a converted image for playback, endlessly wrapping.
// Small images are preloaded into RAM (PSRAM if present). Larger ones are
// streamed from SD by a reader task on the other core into a ring buffer,
// so SD latency never stalls the LED timing.
#pragma once
#include <Arduino.h>
#include <FS.h>
#include <atomic>

class ImageSource {
 public:
  bool open(fs::FS &fs, const char *path, uint32_t nLines);
  void close();

  // Next line to show, waiting up to timeoutUs for the SD reader.
  // Returns nullptr on timeout (an underrun).
  const uint8_t *peek(uint32_t timeoutUs);
  void advance();      // Done with the line returned by peek()

  bool inRam() const { return ram != nullptr; }

 private:
  static void readerTrampoline(void *arg);
  void readerLoop();

  uint32_t nLines = 0;
  // RAM mode
  uint8_t *ram     = nullptr;
  uint32_t ramLine = 0;
  // Stream mode
  File                  file;
  uint8_t              *ring = nullptr;
  std::atomic<uint32_t> produced{0}, consumed{0};
  std::atomic<bool>     stopReq{false}, readerDone{true};
};
