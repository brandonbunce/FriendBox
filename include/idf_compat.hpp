// Tiny IDF-flavored stand-ins for the Arduino timing primitives the
// rest of the firmware still calls. Only the panel driver (under lib/)
// uses the broader Arduino compat shim; everything in src/ converted
// to IDF APIs except these two timing helpers, which are too pervasive
// to inline at every call site.

#pragma once

#include <stdint.h>
#include <stdio.h>

#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

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

// Pragmatic Serial.* → stdout shim. Lets the ~130 print sites in src/
// keep their existing syntax (Serial.printf, Serial.println, Serial.print)
// while the underlying transport is the IDF UART-stdout, not Arduino's
// HardwareSerial. The panel driver under lib/ has its own equivalent in
// arduino_compat.h and does not include this header.
struct IdfSerial
{
    void begin(unsigned long) const {}

    template <typename... Args>
    int printf(const char *fmt, Args... args) const
    {
        return ::printf(fmt, args...);
    }

    void print(const char *s)   const { fputs(s, stdout); }
    void print(int v)           const { ::printf("%d", v); }
    void print(unsigned v)      const { ::printf("%u", v); }
    void print(long v)          const { ::printf("%ld", v); }
    void print(unsigned long v) const { ::printf("%lu", v); }
    void print(float v)         const { ::printf("%g", (double)v); }
    void print(double v)        const { ::printf("%g", v); }
    void print(char c)          const { fputc(c, stdout); }

    void println(const char *s) const { fputs(s, stdout); fputc('\n', stdout); }
    void println(int v)           const { ::printf("%d\n", v); }
    void println(unsigned v)      const { ::printf("%u\n", v); }
    void println(long v)          const { ::printf("%ld\n", v); }
    void println(unsigned long v) const { ::printf("%lu\n", v); }
    void println(float v)         const { ::printf("%g\n", (double)v); }
    void println(double v)        const { ::printf("%g\n", v); }
    void println()                const { fputc('\n', stdout); }
};

static const IdfSerial Serial;
