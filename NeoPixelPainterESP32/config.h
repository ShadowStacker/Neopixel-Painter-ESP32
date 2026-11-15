// NeoPixel Painter for ESP32 -- user configuration.
// Everything you are likely to want to change lives in this file.
#pragma once
#include <sdkconfig.h> // defines CONFIG_IDF_TARGET_* used for the pin defaults below

// ---------------------------------------------------------------------------
// LED STRIP
// ---------------------------------------------------------------------------
#define N_LEDS            144                  // Number of pixels on the bar
#define LED_TYPE          (NEO_GRB + NEO_KHZ800) // Adafruit_NeoPixel type flags
#define LED0_AT_TOP       0   // 0 = first pixel (data-in end) at the BOTTOM of
                              //     the bar (usual: wires come out at handle)
                              // 1 = first pixel at the TOP

// Power limiting. Brightness is capped so the brightest line of an image
// never draws more than CURRENT_MAX from the supply. 144 pixels at full
// white draw ~8.6 A. The Adafruit UBEC is 3 A continuous / 5 A peak.
#define CURRENT_MAX       3500 // mA available for the LEDs
#define MA_PER_CHANNEL    20   // mA per R/G/B channel at full brightness
#define MA_IDLE_PER_LED   1    // quiescent mA per pixel

// 1 = every image uses the same brightness cap (the lowest of all images),
//     so multi-frame animations look consistent (the original's behaviour).
// 0 = each image gets its own cap (bright images limited, dim ones not).
#define CONSISTENT_BRIGHTNESS 1

#define GAMMA             2.6f // Gamma correction applied at playback
#define DEFAULT_BRIGHTNESS 255 // Used only when PIN_BRIGHTNESS_POT is -1
#define DEFAULT_LPS       100  // Lines/sec used only when PIN_SPEED_POT is -1

// ---------------------------------------------------------------------------
// IMAGES
// ---------------------------------------------------------------------------
// Every *.bmp (24- or 32-bit, uncompressed) in IMAGE_DIR is used, in
// alphabetical order (so frame000.bmp, frame001.bmp... still works).
// Converted copies are cached in CACHE_DIR and only rebuilt when the BMP
// changes. Hold the trigger button while powering up to force a rebuild.
#define IMAGE_DIR         "/"
#define CACHE_DIR         "/_painter_cache"
#define MAX_IMAGES        200

// 0 = images are drawn the natural way round: what you see on screen is
//     what appears in the photo. The bar is held vertically and swept
//     left-to-right (as seen by the camera); image height maps to the strip.
// 1 = images were pre-rotated 90 degrees counter-clockwise, as the original
//     Adafruit project required. Use this for images made for that project.
#define IMAGE_PREROTATED  0

// 1 = scale the image to fit the strip's length (nearest-neighbour).
// 0 = no scaling; images are centred on the strip and cropped if too tall.
#define SCALE_TO_FIT      1

// ---------------------------------------------------------------------------
// PLAYBACK
// ---------------------------------------------------------------------------
#define MIN_LPS           10   // Slowest speed (lines/sec) with dial at min
#define MAX_LPS           400  // Fastest; also limited by strip refresh time
                               // (144 px ~= 228 lines/sec)
#define STREAM_RING_LINES 128  // Read-ahead buffer when streaming from SD
#define RAM_RESERVE       (48 * 1024) // Internal RAM left free when deciding
                                      // whether an image can be preloaded

// Optional: advance one line per N pulses from a wheel/rotary encoder
// instead of using a timer (like the original's ENCODERSTEPS).
//#define ENCODER_PIN       25
//#define ENCODER_STEPS     8
#define ENCODER_TIMEOUT_MS 3000 // Stop if the wheel stops this long

// ---------------------------------------------------------------------------
// SD CARD
// ---------------------------------------------------------------------------
#define SD_SPI_HZ         20000000 // Drop to 4000000 with long/flaky wires

// ---------------------------------------------------------------------------
// PINS: defaults per chip. Change them to suit your board. Set a pot or
// button pin to -1 if you don't fit it.
//   PIN_TRIGGER        momentary button to GND: tap = play once, hold = loop
//   PIN_NEXT           momentary button to GND: skip to the next image
//                      (defaults to the on-board BOOT button)
//   PIN_SPEED_POT      10k pot (3V3 / wiper / GND): playback speed
//   PIN_BRIGHTNESS_POT 10k pot: brightness (read each time you trigger)
// Pots must be on ADC1-capable pins.
// ---------------------------------------------------------------------------
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  #define PIN_LED            5
  #define PIN_TRIGGER        6
  #define PIN_NEXT           0   // BOOT button
  #define PIN_SPEED_POT      1
  #define PIN_BRIGHTNESS_POT 2
  #define PIN_SD_SCK         12
  #define PIN_SD_MISO        13
  #define PIN_SD_MOSI        11
  #define PIN_SD_CS          10
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
  #define PIN_LED            5
  #define PIN_TRIGGER        6
  #define PIN_NEXT           0   // BOOT button
  #define PIN_SPEED_POT      1
  #define PIN_BRIGHTNESS_POT 2
  #define PIN_SD_SCK         36
  #define PIN_SD_MISO        37
  #define PIN_SD_MOSI        35
  #define PIN_SD_CS          34
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
  #define PIN_LED            10
  #define PIN_TRIGGER        3
  #define PIN_NEXT           9   // BOOT button
  #define PIN_SPEED_POT      0
  #define PIN_BRIGHTNESS_POT 1
  #define PIN_SD_SCK         4
  #define PIN_SD_MISO        5
  #define PIN_SD_MOSI        6
  #define PIN_SD_CS          7
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
  #define PIN_LED            10
  #define PIN_TRIGGER        11
  #define PIN_NEXT           9   // BOOT button
  #define PIN_SPEED_POT      0
  #define PIN_BRIGHTNESS_POT 1
  #define PIN_SD_SCK         21
  #define PIN_SD_MISO        20
  #define PIN_SD_MOSI        19
  #define PIN_SD_CS          18
#elif defined(CONFIG_IDF_TARGET_ESP32)
  #define PIN_LED            13
  #define PIN_TRIGGER        27
  #define PIN_NEXT           0   // BOOT button
  #define PIN_SPEED_POT      34  // input-only ADC1 pin
  #define PIN_BRIGHTNESS_POT 35  // input-only ADC1 pin
  #define PIN_SD_SCK         18
  #define PIN_SD_MISO        19
  #define PIN_SD_MOSI        23
  #define PIN_SD_CS          5
#else
  #error "No default pins for this chip: add a PIN_ block in config.h"
#endif
