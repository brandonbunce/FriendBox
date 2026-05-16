#include "fbox_source.hpp"
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <esp_rom_crc.h>

// ── FboxSourceSD ────────────────────────────────────────────────────────────

FboxSourceSD::FboxSourceSD(const char *path) : _path(path)
{
    _f = SD.open(path, FILE_READ);
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
