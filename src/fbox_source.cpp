#include "fbox_source.hpp"
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <esp_rom_crc.h>
#include <esp_timer.h>

// ── FboxSourceSD ────────────────────────────────────────────────────────────

FboxSourceSD::FboxSourceSD(const char *path) : _path(path)
{
    _f = SD_MMC.open(path, FILE_READ);
    if (_f) _size = _f.size();
}

FboxSourceSD::~FboxSourceSD()
{
    if (_f) _f.close();
}

int FboxSourceSD::read(uint8_t *dst, size_t n)
{
    if (!_f) return -1;
    int r = (int)_f.readBytes((char *)dst, n);
    if (r <= 0) return _f.available() ? -1 : 0;
    return r;
}

bool FboxSourceSD::reset()
{
    if (!_f) return false;
    return _f.seek(0);
}

// ── FboxSourcePSRAM ─────────────────────────────────────────────────────────

int FboxSourcePSRAM::read(uint8_t *dst, size_t n)
{
    if (!_base) return -1;
    if (_cursor >= _len) return 0;
    uint32_t remaining = _len - _cursor;
    size_t   take      = (n < remaining) ? n : remaining;
    memcpy(dst, _base + _cursor, take);
    _cursor += take;
    return (int)take;
}

// ── FboxSourceRingBuffered ──────────────────────────────────────────────────

FboxSourceRingBuffered::FboxSourceRingBuffered(FboxSource *inner, uint32_t ring_bytes)
    : _inner(inner), _storage(nullptr), _stream(nullptr), _loader_task(nullptr),
      _stop(false), _eof(false), _loader_done(false), _ring_bytes(ring_bytes),
      _stall_count(0), _stall_time_us(0)
{
    _storage = (uint8_t *)ps_malloc(ring_bytes + 1);
    if (!_storage) {
        Serial.printf("[FBOX-RING] ps_malloc %lu B failed\n", (unsigned long)(ring_bytes + 1));
        return;
    }
    _stream = xStreamBufferCreateStatic(ring_bytes, 1, _storage, &_stream_static);
    if (!_stream) {
        free(_storage); _storage = nullptr;
        Serial.println("[FBOX-RING] xStreamBufferCreateStatic failed");
        return;
    }
    // Pin loader to core 0 alongside the decoder. Earlier attempt on core 1
    // (with the SPI consumer) saw the loader's SPI-DMA ISRs preempt the main
    // task between frames — visible as a ~66 ms/frame gap that wasn't in any
    // measured stage. Core 0 has the decoder, but once the ring is in place
    // the decoder is PSRAM-bound (~39 ms decode) and spends most of each
    // cycle blocked on sem_free, so there's ample core-0 headroom for the
    // loader's SD I/O.
    // 8 KB stack: SD/FATFS calls + xStreamBufferSend internals + 4 KB chunk
    // local easily exceed 4 KB and trip the FreeRTOS stack-canary panic.
    BaseType_t ok = xTaskCreatePinnedToCore(loaderTrampoline, "fbox_ld",
                                            8192, this, 2, &_loader_task, 0);
    if (ok != pdPASS) {
        vStreamBufferDelete(_stream); _stream = nullptr;
        free(_storage); _storage = nullptr;
        Serial.println("[FBOX-RING] loader task spawn failed");
        return;
    }
}

FboxSourceRingBuffered::~FboxSourceRingBuffered()
{
    _stop = true;
    if (_loader_task) {
        // Wake the loader if it's blocked inside xStreamBufferSend by draining
        // the ring, then wait on _loader_done — which the loader sets just
        // before vTaskDelete(NULL). Polling eTaskGetState after a task has
        // self-deleted is undefined; the explicit flag is safe.
        if (_stream) xStreamBufferReset(_stream);
        for (int i = 0; i < 400 && !_loader_done; i++) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (!_loader_done) {
            Serial.println("[FBOX-RING] loader did not exit within 2s; leaking task");
        }
    }
    if (_stream)  { vStreamBufferDelete(_stream); _stream = nullptr; }
    if (_storage) { free(_storage); _storage = nullptr; }
}

void FboxSourceRingBuffered::loaderTrampoline(void *arg)
{
    static_cast<FboxSourceRingBuffered *>(arg)->loaderLoop();
}

void FboxSourceRingBuffered::loaderLoop()
{
    // 4 KB inner chunk size matches the FboxRleReader's own buf size on the
    // playback side, keeping per-call SD overhead amortised at both ends.
    static const size_t kChunk = 4096;
    uint8_t chunk[kChunk];

    while (!_stop) {
        int r = _inner->read(chunk, kChunk);
        if (r <= 0) {
            // EOF (r==0) or hard error (r<0). Both end the stream cleanly;
            // playback core will surface a READ_UNDERRUN if r<0 happened mid-stream.
            break;
        }
        size_t sent = 0;
        while (sent < (size_t)r && !_stop) {
            // 100 ms timeout so we periodically re-check _stop while the
            // playback core is paused / consuming slowly.
            size_t s = xStreamBufferSend(_stream, chunk + sent, (size_t)r - sent,
                                         pdMS_TO_TICKS(100));
            sent += s;
        }
        // Yield one tick per chunk. The Arduino-ESP32 SD library uses polling-
        // mode SPI for parts of a read, so File::readBytes is CPU-active for
        // most of its duration rather than blocking. Without this yield, the
        // loader never drops below its priority and IDLE0 starves → task WDT.
        vTaskDelay(1);
    }
    _eof = true;
    _loader_done = true;   // signal destructor before we vanish
    _loader_task = nullptr;
    vTaskDelete(NULL);
}

int FboxSourceRingBuffered::read(uint8_t *dst, size_t n)
{
    if (!_stream) return -1;
    uint64_t t0 = esp_timer_get_time();
    size_t got = xStreamBufferReceive(_stream, dst, n, pdMS_TO_TICKS(2000));
    uint64_t dt = esp_timer_get_time() - t0;
    // Any wait > 1 ms counts as a stall (ring drained at least briefly).
    if (dt > 1000) { _stall_count++; _stall_time_us += dt; }
    if (got == 0) {
        // 0 from xStreamBufferReceive on timeout. If loader signalled EOF and
        // the buffer is empty, this is a clean end-of-stream.
        if (_eof && xStreamBufferIsEmpty(_stream) == pdTRUE) return 0;
        return -1;  // genuine underrun (timeout while loader still working)
    }
    return (int)got;
}

bool FboxSourceRingBuffered::reset()
{
    // Looping playback would need stop-loader → inner->reset() → restart-loader.
    // Not used today (playback core hits EOF and returns), so leave unimplemented.
    return false;
}

// ── FboxSourceCrc ───────────────────────────────────────────────────────────

int FboxSourceCrc::read(uint8_t *dst, size_t n)
{
    int r = _inner->read(dst, n);
    if (r > 0) _crc = esp_rom_crc32_le(_crc, dst, (uint32_t)r);
    return r;
}

// ── FboxSourceHTTP ──────────────────────────────────────────────────────────

FboxSourceHTTP::FboxSourceHTTP(const char *url, uint32_t read_timeout_ms)
    : _url(url), _read_timeout_ms(read_timeout_ms)
{
    _open();
}

FboxSourceHTTP::~FboxSourceHTTP() { _close(); }

bool FboxSourceHTTP::_open()
{
    _close();
    _http = new HTTPClient();
    if (!_http) return false;
    _http->setTimeout(_read_timeout_ms);
    if (!_http->begin(_url)) { _close(); return false; }
    int code = _http->GET();
    if (code != 200) {
        Serial.printf("[FBOX-HTTP] %s GET=%d\n", _url.c_str(), code);
        _close();
        return false;
    }
    _size   = (uint32_t)_http->getSize();   // -1 if unknown → wraps to large; treat as 0
    if ((int32_t)_size < 0) _size = 0;
    _stream = _http->getStreamPtr();
    return _stream != nullptr;
}

void FboxSourceHTTP::_close()
{
    _stream = nullptr;
    if (_http) {
        _http->end();
        delete _http;
        _http = nullptr;
    }
}

int FboxSourceHTTP::read(uint8_t *dst, size_t n)
{
    if (!_stream) return -1;
    size_t   got = 0;
    uint32_t t0  = millis();
    while (got < n) {
        int avail = _stream->available();
        if (avail > 0) {
            int chunk = _stream->readBytes((char *)(dst + got),
                                           (int)((n - got) < (size_t)avail ? (n - got) : (size_t)avail));
            if (chunk <= 0) break;
            got += (size_t)chunk;
            t0  = millis();
            continue;
        }
        if (!_http->connected() && got == 0) return 0;
        if (millis() - t0 > _read_timeout_ms) {
            Serial.printf("[FBOX-HTTP] read timeout after %lu ms (got %u/%u)\n",
                          (unsigned long)_read_timeout_ms, (unsigned)got, (unsigned)n);
            return got > 0 ? (int)got : -1;
        }
        delay(1);
    }
    return (int)got;
}

bool FboxSourceHTTP::reset()
{
    return _open();
}
