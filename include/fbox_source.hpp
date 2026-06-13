#ifndef FBOX_SOURCE_HPP
#define FBOX_SOURCE_HPP

#include <stdint.h>
#include <stddef.h>
#include <string>

#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/stream_buffer.h>
#include <ff.h>

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

    bool ok() const { return _ok; }

private:
    // Uses FATFS FIL directly instead of Arduino's fs::File / VFS path.
    // Skipping the VFS + POSIX wrappers saves ~150–250 µs of per-call overhead
    // on each File::readBytes — measurable in the playback profile as a drop
    // in `refill_us/call`. SD_MMC.begin() already mounts the FATFS volume
    // (at drive "0:"), so we just open the path against that same volume.
    FIL      _fil;
    bool     _ok   = false;
    uint32_t _size = 0;
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

/* Async PSRAM ring buffer wrapper. A FreeRTOS loader task on core 1 pulls
 * from `inner` and writes into a PSRAM-backed stream buffer; reads on the
 * playback path block on `xStreamBufferReceive` so the producer never touches
 * SD/HTTP directly. The loader runs in parallel with both the decoder (core 0)
 * and the consumer's SPI burst (core 1), so for content where the inner source
 * can keep up with the decoder's demand, playback runs at PSRAM speed with
 * zero stalls. When the ring drains (decoder demand exceeds inner throughput)
 * read() blocks momentarily — playback pauses until the ring refills.
 *
 * Profiling: stallCount() and stallTimeUs() report receive-side waits > 1 ms,
 * which indicate the ring drained at least once. */
class FboxSourceRingBuffered : public FboxSource
{
public:
    /* ring_bytes: usable capacity of the PSRAM ring (allocator adds +1 internally).
     * Recommended: 1–4 MB. Smaller buffers stall more on jittery sources.
     * Defaults to 2 MB which absorbs ≈ 17 dithered 480×480 frames. */
    explicit FboxSourceRingBuffered(FboxSource *inner, uint32_t ring_bytes = 2u * 1024u * 1024u);
    ~FboxSourceRingBuffered() override;

    int      read(uint8_t *dst, size_t n) override;
    bool     reset() override;
    uint32_t size() const override { return _inner ? _inner->size() : 0; }

    bool     ok() const { return _stream != nullptr; }
    uint32_t stallCount() const { return _stall_count; }
    uint64_t stallTimeUs() const { return _stall_time_us; }

    /* Seamless looping: when enabled, the loader rewinds the inner source on
     * EOF and keeps filling the ring instead of stopping. The byte stream
     * becomes effectively infinite ([file][file][file]…) so a reader can play
     * the file back-to-back with no refill stall at the loop boundary. The
     * reader is responsible for re-parsing each repeated header. Safe to set
     * before the first read; the loader picks it up at the next inner EOF. */
    void     setLoop(bool on) { _loop = on; }

private:
    static void  loaderTrampoline(void *arg);
    void         loaderLoop();

    FboxSource          *_inner;
    uint8_t             *_storage;       // PSRAM, ring_bytes + 1
    StreamBufferHandle_t _stream;
    StaticStreamBuffer_t _stream_static; // FreeRTOS control block (internal RAM)
    TaskHandle_t         _loader_task;
    volatile bool        _stop;
    volatile bool        _eof;
    volatile bool        _loader_done;   // set by loader just before vTaskDelete
    volatile bool        _loop;          // rewind inner on EOF instead of stopping
    uint32_t             _ring_bytes;
    uint32_t             _stall_count;
    uint64_t             _stall_time_us;
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

    bool ok() const { return _client != nullptr; }

private:
    bool _open();
    void _close();

    esp_http_client_handle_t _client = nullptr;
    bool        _headers_fetched = false;
    std::string _url;
    uint32_t    _size           = 0;
    uint32_t    _read_timeout_ms;
};

#endif
