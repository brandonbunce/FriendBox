#include "fbox_source.hpp"
#include "auth.hpp"
#include "idf_compat.hpp"

#include <cstring>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_rom_crc.h>
#include <esp_timer.h>

// ── FboxSourceSD ────────────────────────────────────────────────────────────

// FATFS path conversion: esp_vfs_fat_sdmmc_mount registers the volume at
// drive "0:". Callers pass paths in VFS form ("/sd/sketches/foo.fbox").
// Strip the "/sd" mountpoint prefix and prepend "0:" for FATFS direct calls.
static std::string _fatfs_path(const char *path)
{
    std::string out = "0:";
    if (std::strncmp(path, "/sd/", 4) == 0)      out += (path + 3);    // keep leading '/'
    else if (std::strcmp(path, "/sd") == 0)      out += "/";
    else if (path[0] != '/')                     { out += "/"; out += path; }
    else                                          out += path;
    return out;
}

FboxSourceSD::FboxSourceSD(const char *path)
{
    std::string fp = _fatfs_path(path);
    FRESULT fr = f_open(&_fil, fp.c_str(), FA_READ);
    if (fr != FR_OK) {
        printf("[FBOX-SD] f_open(%s) failed: %d\n", fp.c_str(), fr);
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
      _stop(false), _eof(false), _loader_done(false), _loop(false),
      _ring_bytes(ring_bytes), _stall_count(0), _stall_time_us(0)
{
    _storage = (uint8_t *)heap_caps_malloc(ring_bytes + 1, MALLOC_CAP_SPIRAM);
    if (!_storage) {
        printf("[FBOX-RING] ps_malloc %lu B failed\n", (unsigned long)(ring_bytes + 1));
        return;
    }
    _stream = xStreamBufferCreateStatic(ring_bytes, 1, _storage, &_stream_static);
    if (!_stream) {
        free(_storage); _storage = nullptr;
        puts("[FBOX-RING] xStreamBufferCreateStatic failed");
        return;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(loaderTrampoline, "fbox_ld",
                                            8192, this, 2, &_loader_task, 1);
    if (ok != pdPASS) {
        vStreamBufferDelete(_stream); _stream = nullptr;
        free(_storage); _storage = nullptr;
        puts("[FBOX-RING] loader task spawn failed");
        return;
    }
}

FboxSourceRingBuffered::~FboxSourceRingBuffered()
{
    _stop = true;
    if (_loader_task) {
        if (_stream) xStreamBufferReset(_stream);
        for (int i = 0; i < 400 && !_loader_done; i++) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (!_loader_done) {
            puts("[FBOX-RING] loader did not exit within 2s; leaking task");
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
            // Inner EOF. In loop mode, rewind and keep filling so the ring
            // never drains at the file boundary — the reader sees one
            // continuous [file][file]… stream. If the rewind fails, fall
            // through to the normal EOF path.
            if (_loop && !_stop && _inner->reset()) continue;
            break;
        }
        size_t sent = 0;
        while (sent < (size_t)r && !_stop) {
            size_t s = xStreamBufferSend(_stream, chunk + sent, (size_t)r - sent,
                                         pdMS_TO_TICKS(100));
            sent += s;
        }
    }
    _eof = true;
    _loader_done = true;
    _loader_task = nullptr;
    vTaskDelete(NULL);
}

int FboxSourceRingBuffered::read(uint8_t *dst, size_t n)
{
    if (!_stream) return -1;
    uint64_t t0 = esp_timer_get_time();
    size_t got = xStreamBufferReceive(_stream, dst, n, pdMS_TO_TICKS(2000));
    uint64_t dt = esp_timer_get_time() - t0;
    if (dt > 1000) { _stall_count++; _stall_time_us += dt; }
    if (got == 0) {
        if (_eof && xStreamBufferIsEmpty(_stream) == pdTRUE) return 0;
        return -1;
    }
    return (int)got;
}

bool FboxSourceRingBuffered::reset()
{
    if (!_stream || !_inner) return false;

    // Stop the current loader and wait for it to exit. Mirrors the dtor's
    // sequence: flag stop, unblock any in-flight xStreamBufferSend with a
    // reset, then poll _loader_done.
    _stop = true;
    if (_loader_task) {
        xStreamBufferReset(_stream);
        for (int i = 0; i < 400 && !_loader_done; i++) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (!_loader_done) {
            puts("[FBOX-RING] reset: loader did not exit within 2s");
            return false;
        }
    }

    // Discard whatever the loader left in the ring, rewind the inner source.
    xStreamBufferReset(_stream);
    if (!_inner->reset()) return false;

    // Fresh flags + new loader task. Stall counters persist across resets so
    // looped-playback stats stay cumulative.
    _stop        = false;
    _eof         = false;
    _loader_done = false;
    BaseType_t ok = xTaskCreatePinnedToCore(loaderTrampoline, "fbox_ld",
                                            8192, this, 2, &_loader_task, 1);
    if (ok != pdPASS) {
        puts("[FBOX-RING] reset: loader task spawn failed");
        return false;
    }
    return true;
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
    esp_http_client_config_t cfg = {};
    cfg.url        = _url.c_str();
    cfg.timeout_ms = (int)_read_timeout_ms;
    if (_url.rfind("https://", 0) == 0) {
        cfg.transport_type    = HTTP_TRANSPORT_OVER_SSL;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    _client = esp_http_client_init(&cfg);
    if (!_client) return false;
    authApplyHeader(_client);
    esp_err_t err = esp_http_client_open(_client, 0);
    if (err != ESP_OK) {
        printf("[FBOX-HTTP] open failed: %s\n", esp_err_to_name(err));
        _close();
        return false;
    }
    int64_t content_len = esp_http_client_fetch_headers(_client);
    int status = esp_http_client_get_status_code(_client);
    if (status / 100 != 2) {
        printf("[FBOX-HTTP] %s status=%d\n", _url.c_str(), status);
        if (status == 401) authNotify401();
        _close();
        return false;
    }
    _size = content_len > 0 ? (uint32_t)content_len : 0;
    _headers_fetched = true;
    return true;
}

void FboxSourceHTTP::_close()
{
    if (_client) {
        esp_http_client_close(_client);
        esp_http_client_cleanup(_client);
        _client = nullptr;
    }
    _headers_fetched = false;
}

int FboxSourceHTTP::read(uint8_t *dst, size_t n)
{
    if (!_client) return -1;
    size_t got = 0;
    uint32_t t0 = millis();
    while (got < n) {
        int r = esp_http_client_read(_client, (char *)(dst + got), (int)(n - got));
        if (r > 0) {
            got += (size_t)r;
            t0   = millis();
            continue;
        }
        if (r == 0) {
            // Connection closed cleanly. Return whatever we got — 0 means EOF.
            return (int)got;
        }
        // r < 0 — transient or hard error.
        if (millis() - t0 > _read_timeout_ms) {
            printf("[FBOX-HTTP] read timeout after %lu ms (got %u/%u)\n",
                   (unsigned long)_read_timeout_ms, (unsigned)got, (unsigned)n);
            return got > 0 ? (int)got : -1;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return (int)got;
}

bool FboxSourceHTTP::reset()
{
    return _open();
}
