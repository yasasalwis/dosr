#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// Pin map - Seeed XIAO ESP32-C5
// The D0..D10 macros come from the board variant. GPIO numbers in comments.
//
// All 11 exposed pads are spoken for. The pump gates and the float switch live
// on an MCP23017 rather than on the MCU, because the SPI display needs four
// pins and there is no other way to find them - see docs/BOM.md section 2.
// ---------------------------------------------------------------------------
#define PIN_ENC_SW   D0    // GPIO1  - encoder push, INPUT_PULLUP, also an LP wake pin
#define PIN_ENC_A    D1    // GPIO0  - encoder channel A
#define PIN_ENC_B    D2    // GPIO25 - encoder channel B
#define PIN_TFT_CS   D3    // GPIO7  - also the variant's native SPI SS
#define PIN_I2C_SDA  D4    // GPIO23
#define PIN_I2C_SCL  D5    // GPIO24
                           // D6 / GPIO11 is UART0 TX - left free for boot logs
#define PIN_TFT_DC   D7    // GPIO12 - data/command select
#define PIN_TFT_SCLK D8    // GPIO8  - native SPI SCK
#define PIN_DS18B20  D9    // GPIO9  - one-wire solution temperature probe
#define PIN_TFT_MOSI D10   // GPIO10 - native SPI MOSI

// SPI.begin() is called with miso = -1. The core skips spiAttachMISO() when
// the pin is negative, so GPIO9 is never claimed by the SPI peripheral and
// stays free for the one-wire temperature probe.
#define TFT_MISO_UNUSED (-1)

// The 1.8" ST7735 panel is 128x160 portrait.
static const int16_t TFT_W = 128;
static const int16_t TFT_H = 160;

// ---------------------------------------------------------------------------
// I2C addresses
// ---------------------------------------------------------------------------
#define ADDR_MCP23017 0x20   // A0/A1/A2 strapped to GND
#define ADDR_ADS1115  0x48

// Pump gates on port A, float switch on port B. The Adafruit library numbers
// GPA0..GPA7 as 0..7 and GPB0..GPB7 as 8..15.
static const uint8_t MCP_PUMP_PINS[4] = { 0, 1, 2, 3 };
static const uint8_t MCP_PIN_FLOAT    = 8;

#define ADS_CH_PH    0
#define ADS_CH_EC    1

// 100 kHz, not 400 kHz. Down long jumper wires the rise times at 400 kHz do
// not make it and the bus reports ESP_ERR_INVALID_STATE.
static const uint32_t I2C_CLOCK_HZ = 100000;

// The pH and EC boards output 0-5 V. The ADS1115 now runs at 3.3 V, so each
// analog output goes through a 2:1 divider (two 10k resistors). Readings are
// multiplied back by this to recover the true sensor voltage.
// Set to 1.0 if you feed the ADS1115 5 V and drive it directly.
static const float ADC_DIVIDER_RATIO = 2.0f;

// ---------------------------------------------------------------------------
// Safety limits. These are hard ceilings the UI cannot raise.
// ---------------------------------------------------------------------------
static const uint32_t PUMP_MAX_RUN_MS   = 90000;  // a single dose can never exceed 90 s
static const float    PUMP_MAX_DOSE_ML  = 500.0f;
static const float    PUMP_MIN_ML_PER_S = 0.05f;
static const uint8_t  DOSE_QUEUE_LEN    = 8;
static const uint8_t  DOSE_HISTORY_LEN  = 12;     // rolling per-hour rate limit window

// How long after a pump stops before sensor readings are trusted again.
// Overridable in settings; this is the floor.
static const uint16_t SETTLE_MIN_SEC    = 2;

// ---------------------------------------------------------------------------
// Misc
// ---------------------------------------------------------------------------
// Set to 0 if the encoder is not wired yet. Floating encoder pins generate a
// continuous interrupt storm that starves USB and trips the watchdog.
#define ENCODER_ENABLED 1

// If the ISR fires more often than this per second the encoder is treated as
// unwired or noisy and the interrupts are detached rather than left to
// lock up the board.
static const uint32_t ENC_ISR_STORM_PER_SEC = 4000;

#define FW_VERSION "1.1.0"
#define AP_FALLBACK_SSID "MiniFeeder-Setup"
#define AP_FALLBACK_PASS "feedme1234"
