// NEOPIXEL LIGHT PAINTER for ESP32 / ESP32-S2 / ESP32-S3 / ESP32-C3 / ESP32-C6
//
// Reads BMP images from an SD card and plays them back one line at a time on
// a NeoPixel strip, for long-exposure light-painting photography.
//
// A port of the Adafruit NeoPixel Painter by Phil Burgess / Paint Your Dragon
// (https://learn.adafruit.com/neopixel-painter), BSD license. It keeps the
// original's controls and adds:
//  * any *.bmp file name, natural (unrotated) orientation, scaling to fit
//  * converted images are cached on the card, so later boots start at once
//  * brightness and speed can be changed without resetting
//  * a "next image" button and LED status / progress display
//  * images play from RAM/PSRAM, or stream from SD with a read-ahead task
//    on the second core, so long images keep steady timing
//
// Controls:
//   Trigger tap  : play the current image once, then select the next one
//   Trigger hold : loop the image until released (finishes the current pass)
//   Next button  : skip to the next image (strip briefly shows which one)
//   Hold trigger during power-up: re-convert all images
//   Speed dial   : read at each trigger. Brightness dial: read at each trigger
//
// Settings and pins are in config.h.

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_NeoPixel.h>
#include <esp_timer.h>
#include <vector>
#include <algorithm>
#include "config.h"
#include "Converter.h"
#include "ImageSource.h"

struct ImageInfo {
  String   name;     // e.g. "/frame000.bmp"
  String   neoPath;  // cached conversion
  uint32_t lines;
  uint8_t  maxBrightness; // power-limited cap for this image
};

Adafruit_NeoPixel      strip(N_LEDS, PIN_LED, LED_TYPE);
std::vector<ImageInfo> images;
ImageSource            source;
size_t                 current = 0;
uint8_t                globalMaxBrightness = 255;
uint32_t               maxLps;
uint8_t                lut[256];

#ifdef ENCODER_PIN
volatile uint32_t encoderCount = 0;
void IRAM_ATTR encoderISR() { encoderCount++; }
#endif

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

static bool pressed(int pin) { return pin >= 0 && digitalRead(pin) == LOW; }

// Wait until esp_timer reaches t (us). Sleeps while there is plenty of time
// (so the idle task / watchdog get a turn on single-core chips), then spins
// for the last couple of milliseconds for precise line timing.
static int64_t waitUntil(int64_t t) {
  int64_t now;
  while ((now = esp_timer_get_time()) < t) {
    if (t - now > 2500) vTaskDelay(1);
  }
  return now;
}

static void waitRelease(int pin) {
  while (pressed(pin)) delay(5);
  delay(30); // debounce
}

// Returns 0..4095, or -1 if the pot isn't fitted.
static int readPot(int pin) {
  if (pin < 0) return -1;
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogRead(pin);
  return min<int>(sum / 8, 4095);
}

static void fillRange(int count, uint32_t color) {
  strip.clear();
  for (int i = 0; i < count && i < N_LEDS; i++) strip.setPixelColor(i, color);
  strip.show();
}

static void progressBar(float f) {
  fillRange((int)(f * N_LEDS + 0.5f), strip.Color(0, 6, 0));
}

[[noreturn]] static void fatal(const String &msg) {
  for (;;) {
    Serial.println("ERROR: " + msg);
    for (int i = 0; i < 4; i++) {
      fillRange(4, strip.Color(20, 0, 0));
      delay(250);
      fillRange(0, 0);
      delay(250);
    }
  }
}

// Highest brightness (0-255 scale) that keeps the brightest line of an image
// within CURRENT_MAX.
static uint8_t brightnessCap(uint32_t lineMax) {
  int32_t budget = CURRENT_MAX - N_LEDS * MA_IDLE_PER_LED;
  if (budget <= 0) return 1;
  if (lineMax == 0) return 255;
  // current(b) = lineMax / 255 * MA_PER_CHANNEL * b / 255
  uint64_t b = (uint64_t)budget * 255 * 255 / ((uint64_t)lineMax * MA_PER_CHANNEL);
  return (uint8_t)constrain(b, (uint64_t)1, (uint64_t)255);
}

static void buildLut(uint8_t brightness) {
  for (int v = 0; v < 256; v++)
    lut[v] = (uint8_t)lroundf(powf(v / 255.0f, GAMMA) * brightness);
}

static void renderLine(const uint8_t *line) {
  for (int i = 0; i < N_LEDS; i++, line += 3)
    strip.setPixelColor(i, lut[line[0]], lut[line[1]], lut[line[2]]);
}

static bool hasBmpExtension(const String &name) {
  String lower = name;
  lower.toLowerCase();
  return lower.endsWith(".bmp");
}

static String baseName(const String &path) {
  int slash = path.lastIndexOf('/');
  String b = path.substring(slash + 1);
  int dot = b.lastIndexOf('.');
  return dot > 0 ? b.substring(0, dot) : b;
}

// --------------------------------------------------------------------------
// Startup: find and convert images
// --------------------------------------------------------------------------

static void scanAndConvert(bool forceConvert) {
  std::vector<String> names;
  File dir = SD.open(IMAGE_DIR);
  if (!dir || !dir.isDirectory()) fatal("can't open image folder " IMAGE_DIR);
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    String path = f.path();
    String leaf = path.substring(path.lastIndexOf('/') + 1);
    // Skip folders and macOS "._" resource-fork files
    if (!f.isDirectory() && !leaf.startsWith(".") && hasBmpExtension(leaf))
      names.push_back(path);
    f.close();
    if (names.size() >= MAX_IMAGES) break;
  }
  dir.close();
  std::sort(names.begin(), names.end());
  if (names.empty()) fatal("no .bmp files found in " IMAGE_DIR);

  if (!SD.exists(CACHE_DIR)) SD.mkdir(CACHE_DIR);

  for (size_t i = 0; i < names.size(); i++) {
    ImageInfo img;
    img.name    = names[i];
    img.neoPath = String(CACHE_DIR) + "/" + baseName(names[i]) + ".neo";

    File src = SD.open(img.name, FILE_READ);
    uint32_t srcSize = src.size(), srcTime = (uint32_t)src.getLastWrite();
    src.close();

    Serial.printf("[%u/%u] %s: ", (unsigned)i + 1, (unsigned)names.size(),
                  img.name.c_str());
    NeoHeader hdr;
    if (!forceConvert && neoCacheValid(SD, img.neoPath.c_str(), srcSize, srcTime, hdr)) {
      Serial.print("cached");
    } else {
      static float base, span;
      base = (float)i / names.size();
      span = 1.0f / names.size();
      uint32_t t0 = millis();
      String err;
      if (!convertBmp(SD, img.name.c_str(), img.neoPath.c_str(),
                      [](float f) { progressBar(base + f * span); }, hdr, err)) {
        Serial.printf("SKIPPED (%s)\n", err.c_str());
        continue;
      }
      Serial.printf("converted in %lu ms", (unsigned long)(millis() - t0));
    }
    img.lines         = hdr.nLines;
    img.maxBrightness = brightnessCap(hdr.lineMax);
    Serial.printf(", %lu lines, brightness cap %u\n",
                  (unsigned long)img.lines, img.maxBrightness);
    images.push_back(img);
    globalMaxBrightness = min(globalMaxBrightness, img.maxBrightness);
  }
  fillRange(0, 0);
  if (images.empty()) fatal("no usable images (see messages above)");
}

// Get the current image ready to play (preload or start streaming).
static void prepareCurrent() {
  ImageInfo &img = images[current];
  if (!source.open(SD, img.neoPath.c_str(), img.lines))
    fatal("can't read " + img.neoPath);
  Serial.printf("Ready: %s (%lu lines, %s)\n", img.name.c_str(),
                (unsigned long)img.lines,
                source.inRam() ? "in RAM" : "streaming from SD");
}

// Briefly show which image is selected: a bar of (index+1) dim blue pixels.
static void showSelection() {
  fillRange((int)current + 1, strip.Color(0, 0, 12));
  delay(700);
  fillRange(0, 0);
}

// --------------------------------------------------------------------------
// Playback
// --------------------------------------------------------------------------

// Returns false if painting stopped part-way through an image.
static bool play() {
  ImageInfo &img = images[current];
  uint8_t cap = CONSISTENT_BRIGHTNESS ? globalMaxBrightness : img.maxBrightness;
  int     bp  = readPot(PIN_BRIGHTNESS_POT);
  uint8_t brightness = bp < 0 ? min<int>(DEFAULT_BRIGHTNESS, cap)
                              : map(bp, 0, 4095, 1, cap);
  buildLut(brightness);

#ifdef ENCODER_PIN
  encoderCount = 0;
  uint32_t period = 100000; // only used as the SD wait timeout
  Serial.printf("Painting %s, brightness %u, encoder mode\n",
                img.name.c_str(), brightness);
#else
  int      sp  = readPot(PIN_SPEED_POT);
  uint32_t lps = sp < 0 ? min<uint32_t>(DEFAULT_LPS, maxLps)
                        : map(sp, 0, 4095, MIN_LPS, maxLps);
  uint32_t period = 1000000UL / lps;
  Serial.printf("Painting %s, brightness %u, %lu lines/s (%.1f s per pass)\n",
                img.name.c_str(), brightness, (unsigned long)lps,
                (float)img.lines / lps);
#endif

  uint32_t lineInPass = 0, underruns = 0, late = 0;
  bool     complete   = true;
  int64_t  next = esp_timer_get_time();

  for (;;) {
    const uint8_t *line;
    while (!(line = source.peek(period))) underruns++; // SD fell behind
    renderLine(line);

#ifdef ENCODER_PIN
    uint32_t waitStart = millis();
    while (encoderCount < ENCODER_STEPS && millis() - waitStart < ENCODER_TIMEOUT_MS)
      vTaskDelay(1);
    if (encoderCount < ENCODER_STEPS) { complete = false; break; } // wheel stopped
    noInterrupts();
    encoderCount -= ENCODER_STEPS;
    interrupts();
#else
    int64_t now = waitUntil(next);
    if (now - next > (int64_t)period) { late++; next = now; } // don't rush to catch up
    next += period;
#endif

    strip.show();
    source.advance();

    if (++lineInPass >= img.lines) { // end of a pass
      lineInPass = 0;
      if (!pressed(PIN_TRIGGER)) break; // released: stop, else loop again
    }
  }

#ifndef ENCODER_PIN
  waitUntil(next); // let the last line show full width
#endif
  strip.clear();
  strip.show();
  if (underruns || late)
    Serial.printf("  (SD underruns: %lu, late lines: %lu)\n",
                  (unsigned long)underruns, (unsigned long)late);
  return complete;
}

// --------------------------------------------------------------------------

void setup() {
  if (PIN_TRIGGER >= 0) pinMode(PIN_TRIGGER, INPUT_PULLUP);
  if (PIN_NEXT >= 0)    pinMode(PIN_NEXT, INPUT_PULLUP);
  Serial.begin(115200);
  delay(50);
  bool forceConvert = pressed(PIN_TRIGGER); // poll startup trigger early

  strip.begin();
  strip.clear();
  strip.show();
  analogReadResolution(12);
#ifdef ENCODER_PIN
  pinMode(ENCODER_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENCODER_PIN), encoderISR, FALLING);
#endif

  delay(500); // give USB-serial a moment so the log is visible
  Serial.println("\nNeoPixel Painter (ESP32)");
  Serial.printf("Free heap %u, PSRAM %u\n", (unsigned)ESP.getFreeHeap(),
                (unsigned)(psramFound() ? ESP.getFreePsram() : 0));

  SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  if (!SD.begin(PIN_SD_CS, SPI, SD_SPI_HZ)) {
    fatal("SD card init failed. Is a FAT32 card inserted? "
          "Check wiring and the PIN_SD_* settings in config.h.");
  }
  Serial.printf("SD card: %llu MB\n", SD.cardSize() / (1024ULL * 1024ULL));

  if (forceConvert) Serial.println("Trigger held at startup: re-converting all images");
  scanAndConvert(forceConvert);
  Serial.printf("%u image(s)\n", (unsigned)images.size());

  // A NeoPixel refresh takes 30 us per pixel plus a 300 us latch.
  maxLps = min<uint32_t>(MAX_LPS, 1000000UL / (N_LEDS * 30UL + 300UL));
  Serial.printf("Max lines/sec: %lu\n", (unsigned long)maxLps);

  prepareCurrent();
  waitRelease(PIN_TRIGGER);
  Serial.println("Press the trigger to paint.");
}

void loop() {
  if (pressed(PIN_TRIGGER)) {
    delay(20); // debounce
    if (!pressed(PIN_TRIGGER)) return;
    bool complete = play();
    waitRelease(PIN_TRIGGER);
    current = (current + 1) % images.size(); // next image/frame for next time
    if (images.size() > 1 || !complete) prepareCurrent(); // else already at line 0
  } else if (pressed(PIN_NEXT)) {
    delay(20);
    if (!pressed(PIN_NEXT)) return;
    current = (current + 1) % images.size();
    prepareCurrent();
    showSelection();
    waitRelease(PIN_NEXT);
  }
  delay(2);
}
