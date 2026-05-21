#include "fbox_source.hpp"
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <esp_rom_crc.h>
#include <esp_timer.h>

// ── FboxSourceSD ────────────────────────────────────────────────────────────

// FATFS path conversion: SD_MMC mounts the FATFS volume at drive "0:".
// Callers pass paths in VFS form ("/sketches/foo.fbox"). Strip any "/sd"
// mountpoint prefix and prepend "0:" for FATFS direct calls.
static String _fatfs_path(const char *path)
{
    String out = "0:";
    if (strncmp(path, "/sd/", 4) == 0)      out += path + 3;   // keep leading '/'
    else if (strcmp(path, "/sd") == 0)      out += "/";
    else if (path[0] != '/')                { out += "/"; out += path; }
    else                                    out += path;
    return out;
}

FboxSourceSD::FboxSourceSD(const char *path)
{
    String fp = _fatfs_path(path);
    FRESULT fr = f_open(&_fil, fp.c_str(), FA_READ);
    if (fr != FR_OK) {
        Serial.printf("[FBOX-SD] f_open(%s) failed: %d\n", fp.c_str(), fr);
        return;
    }
    _ok   = true;
    _size = (uint32_t)f_size(&_fil);
}

FboxSourceSD::~FboxSourceSD()
{
    if (_ok) f_close(&_fil);
}

int FboxSourceSD::read(uint8_t *dst, size_t n)
{
    if (!_ok) return -1;
    UINT bytes_read = 0;
    FRESULT fr = f_read(&_fil, dst, (UINT)n, &bytes_read);
    if (fr != FR_OK) return -1;
    return (int)bytes_read;
}

bool FboxSourceSD::reset()
{
    if (!_ok) return false;
    return f_lseek(&_fil, 0) == FR_OK;
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
    // Pin loader to core 1 alongside the SPI consumer (main task). Core 0
    // hosts the decoder + Wi-Fi background tasks (Wi-Fi is core-0-pinned in
    // ESP-IDF). With Wi-Fi paused during playback (esp_wifi_stop) and SDIO's
    // interrupt-driven I/O (the loader yields naturally during DMA), running
    // on core 1 avoids decoder-loader contention on core 0 and Wi-Fi-task
    // preemption of the loader — which was the cause of the ~120 ms ring-
    // drain stalls observed at 24 fps target.
    // 8 KB stack: SD/FATFS calls + xStreamBufferSend internals + 4 KB chunk
    // local fit comfortably; FreeRTOS stack-canary check is the canary on
    // overruns.
    BaseType_t ok = xTaskCreatePinnedToCore(loaderTrampoline, "fbox_ld",
                                            8192, this, 2, &_loader_task, 1);
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
    // 4 KB inner chunks: empirically the sweet spot. Bigger chunks (tried
    // 16 KB) reduce per-call FATFS overhead but widen the window between
    // produces — every "ring drains" stall grows from ~0.85 ms (4 KB SDIO
    // wall time) to ~3.4 ms (16 KB), and the decoder's sub-ms-per-4-KB
    // demand outpaces those bigger fills. Net result: dithered fps dropped
    // 22.6 → 20.5 going from 4 KB to 16 KB. Keep at 4 KB.
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
        // No explicit yield needed on SDIO: the ESP-IDF SDMMC host is
        // interrupt-driven, so SD_MMC.read() naturally blocks the loader task
        // during DMA — IDLE0 / decoder get CPU. The old vTaskDelay(1) here
        // was a workaround for SD-over-SPI's polling-mode behaviour and
        // imposed ~30 ms of forced delay per frame on dithered content,
        // which prevented the ring from staying ahead of decoder demand.
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
