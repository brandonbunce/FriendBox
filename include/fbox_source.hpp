#ifndef FBOX_SOURCE_HPP
#define FBOX_SOURCE_HPP

#include <Arduino.h>
#include <SD.h>
#include <HTTPClient.h>
#include <WiFi.h>

/* Abstract sequential byte source for FBOX playback. Implementations wrap
 * SD files, PSRAM-resident buffers, or HTTP streams. The playback core only
 * reads forward — no seek() — so SD/HTTP/PSRAM all expose the same shape. */
class FboxSource
{
public:
    virtual ~FboxSource() = default;

    /* Read up to n bytes into dst. Returns:
     *   > 0  bytes read
     *   = 0  EOF
     *   < 0  hard error (underrun, disconnect, etc.) */
    virtual int read(uint8_t *dst, size_t n) = 0;

    /* Rewind to byte 0 for looping playback. Returns true on success.
     * HTTP impl re-opens the connection. */
    virtual bool reset() = 0;

    /* Total file size in bytes if known, 0 if not (chunked HTTP). */
    virtual uint32_t size() const = 0;
};

class FboxSourceSD : public FboxSource
{
public:
    explicit FboxSourceSD(const char *path);
    ~FboxSourceSD() override;

    int      read(uint8_t *dst, size_t n) override;
    bool     reset() override;
    uint32_t size() const override { return _size; }

    bool ok() const { return _f; }

private:
    File     _f;
    uint32_t _size = 0;
    String   _path;
};

class FboxSourcePSRAM : public FboxSource
{
public:
    /* Takes ownership semantics: the caller keeps base alive for the lifetime
     * of this source. The buffer should be in PSRAM but anywhere readable works. */
    FboxSourcePSRAM(const uint8_t *base, uint32_t len)
        : _base(base), _len(len), _cursor(0) {}

    int      read(uint8_t *dst, size_t n) override;
    bool     reset() override { _cursor = 0; return true; }
    uint32_t size() const override { return _len; }

private:
    const uint8_t *_base;
    uint32_t       _len;
    uint32_t       _cursor;
};

/* Wraps another FboxSource and folds every byte read into a CRC32 accumulator.
 * Used by playFboxAnimation to verify the header's CRC across all bytes from
 * offset 62 onward (the header's pre-CRC tail is folded in by the caller before
 * the wrapper is constructed). */
class FboxSourceCrc : public FboxSource
{
public:
    FboxSourceCrc(FboxSource *inner, uint32_t initial_crc)
        : _inner(inner), _crc(initial_crc) {}

    int      read(uint8_t *dst, size_t n) override;
    bool     reset() override { return _inner->reset(); }
    uint32_t size() const override { return _inner->size(); }

    uint32_t crc() const { return _crc; }

private:
    FboxSource *_inner;
    uint32_t    _crc;
};

class FboxSourceHTTP : public FboxSource
{
public:
    /* url is the full HTTPS or HTTP URL to the .fbox file.
     * read_timeout_ms applies to each chunk pull; underruns above that return -1. */
    explicit FboxSourceHTTP(const char *url, uint32_t read_timeout_ms = 2000);
    ~FboxSourceHTTP() override;

    int      read(uint8_t *dst, size_t n) override;
    bool     reset() override;
    uint32_t size() const override { return _size; }

    bool ok() const { return _stream != nullptr; }

private:
    bool _open();
    void _close();

    HTTPClient *_http   = nullptr;
    WiFiClient *_stream = nullptr;
    String      _url;
    uint32_t    _size           = 0;
    uint32_t    _read_timeout_ms;
};

#endif
