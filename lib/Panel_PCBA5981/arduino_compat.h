// Containment shim for the 1.5 kLOC Panel_PCBA5981 driver. Provides just the
// Arduino convenience symbols this file actually uses — millis/delay/GPIO
// and a Serial.printf object — backed by ESP-IDF primitives.
//
// Everywhere else in the firmware we use IDF APIs directly; this header
// exists so we don't have to touch the panel driver while migrating off
// Arduino. Do NOT include this from src/ — convert those call sites instead.

#pragma once

#include <stdint.h>
#include <stdio.h>

#include <driver/gpio.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ---- Pin level / direction constants -----------------------------------
#ifndef HIGH
#define HIGH 1
#endif
#ifndef LOW
#define LOW 0
#endif

// Arduino mode constants used in this driver only. Distinct values so
// pinMode() can dispatch.
#ifndef INPUT
#define INPUT 0x00
#endif
#ifndef OUTPUT
#define OUTPUT 0x01
#endif
#ifndef INPUT_PULLUP
#define INPUT_PULLUP 0x02
#endif

// ---- Timing -----------------------------------------------------------
static inline uint32_t millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

static inline void delay(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1));
}

static inline void delayMicroseconds(uint32_t us)
{
    esp_rom_delay_us(us);
}

// ---- GPIO -------------------------------------------------------------
static inline void pinMode(int pin, int mode)
{
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << (uint32_t)pin;
    switch (mode) {
        case OUTPUT:
            io.mode = GPIO_MODE_OUTPUT;
            io.pull_up_en = GPIO_PULLUP_DISABLE;
            io.pull_down_en = GPIO_PULLDOWN_DISABLE;
            break;
        case INPUT_PULLUP:
            io.mode = GPIO_MODE_INPUT;
            io.pull_up_en = GPIO_PULLUP_ENABLE;
            io.pull_down_en = GPIO_PULLDOWN_DISABLE;
            break;
        default:  // INPUT
            io.mode = GPIO_MODE_INPUT;
            io.pull_up_en = GPIO_PULLUP_DISABLE;
            io.pull_down_en = GPIO_PULLDOWN_DISABLE;
            break;
    }
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);
}

static inline void digitalWrite(int pin, int level)
{
    gpio_set_level((gpio_num_t)pin, level ? 1 : 0);
}

static inline int digitalRead(int pin)
{
    return gpio_get_level((gpio_num_t)pin);
}

// ---- Serial.printf / println shim -------------------------------------
// Tiny stateless object so `Serial.printf(...)` keeps compiling. UART0
// is wired to stdout by IDF, so plain printf reaches the same monitor.
struct ArduinoCompat_Serial {
    template <typename... Args>
    int printf(const char *fmt, Args... args) const
    {
        return ::printf(fmt, args...);
    }
    void print(const char *s)   const { fputs(s, stdout); }
    void println(const char *s) const { fputs(s, stdout); fputc('\n', stdout); }
    void println()              const { fputc('\n', stdout); }
};

static const ArduinoCompat_Serial Serial;
