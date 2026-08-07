#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// Pin map - Seeed XIAO ESP32-C5
// The D0..D10 macros come from the board variant. GPIO numbers in comments.
// ---------------------------------------------------------------------------
#define PIN_ENC_SW   D0    // GPIO1  - encoder push, INPUT_PULLUP, also an LP wake pin
#define PIN_ENC_A    D1    // GPIO0  - encoder channel A
#define PIN_ENC_B    D2    // GPIO25 - encoder channel B
#define PIN_PUMP_1   D3    // GPIO7  - MOSFET gate
#define PIN_I2C_SDA  D4    // GPIO23
#define PIN_I2C_SCL  D5    // GPIO24
                           // D6 / GPIO11 is UART0 TX - left free for boot logs
#define PIN_PUMP_2   D7    // GPIO12
#define PIN_PUMP_3   D8    // GPIO8
#define PIN_PUMP_4   D9    // GPIO9
#define PIN_FLOAT    D10   // GPIO10 - reservoir float switch (optional)

static const uint8_t PUMP_PINS[4] = { PIN_PUMP_1, PIN_PUMP_2, PIN_PUMP_3, PIN_PUMP_4 };

// ---------------------------------------------------------------------------
// I2C addresses
// ---------------------------------------------------------------------------
#define ADDR_OLED    0x3C
#define ADDR_ADS1115 0x48

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
