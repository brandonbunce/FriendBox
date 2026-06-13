#include "mem.hpp"
#include "idf_compat.hpp"     // printf shim

#include <esp_heap_caps.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Playback decode scratch size: the canvas is 480x480 4bpp = (480*480)/2 bytes.
// Matches PIXEL_COUNT>>1 in io.cpp (frame_4bpp). Hardcoded here so this module
// stays free of the LovyanGFX/display include chain.
static const size_t kScratchBytes = (480u * 480u) / 2u;   // 115200

// Producer (fbox_dec) task stack — must match the 16 KB used in io.cpp.
static const size_t kProdStackBytes = 16384u;

static void  *s_scratch    = nullptr;
static void  *s_prod_stack = nullptr;     // StackType_t buffer for the producer
static StaticTask_t s_prod_tcb;           // TCB storage for the static task

// ---------------------------------------------------------------------------
// Eviction registry
// ---------------------------------------------------------------------------
struct Evictable {
    const char *name;
    MemEvictFn  fn;
    void       *ctx;
    int         priority;
};
static const int MAX_EVICTABLES = 8;
static Evictable s_evictables[MAX_EVICTABLES];
static int       s_evictable_count = 0;

void memInit()
{
    s_evictable_count = 0;
    // s_scratch is intentionally left intact across a (hypothetical) re-init —
    // the reservation is permanent for the app's lifetime.
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------
void memReport(const char *tag)
{
    size_t int_free  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t int_big   = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t ps_free   = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t ps_big    = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    printf("[mem:%s] INTERNAL free=%u largest=%u | PSRAM free=%u largest=%u\n",
           tag ? tag : "",
           (unsigned)int_free, (unsigned)int_big,
           (unsigned)ps_free,  (unsigned)ps_big);
}

// ---------------------------------------------------------------------------
// Reserved playback scratch pool
// ---------------------------------------------------------------------------
bool memReservePlaybackScratch()
{
    if (!s_scratch)
        s_scratch = heap_caps_malloc(kScratchBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_prod_stack)
        s_prod_stack = heap_caps_malloc(kProdStackBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    printf("[mem] reserved decode scratch %u B @ %p, producer stack %u B @ %p\n",
           (unsigned)kScratchBytes, s_scratch,
           (unsigned)kProdStackBytes, s_prod_stack);
    if (!s_scratch || !s_prod_stack)
        printf("[mem] WARNING: a playback reservation FAILED (will fall back at runtime)\n");
    return s_scratch != nullptr;
}

void  *memPlaybackScratch()     { return s_scratch; }
size_t memPlaybackScratchSize() { return kScratchBytes; }

void  *memProducerStack()       { return s_prod_stack; }
size_t memProducerStackWords()  { return kProdStackBytes / sizeof(StackType_t); }
void  *memProducerTCB()         { return &s_prod_tcb; }

// ---------------------------------------------------------------------------
// Eviction registry
// ---------------------------------------------------------------------------
void memRegisterEvictable(const char *name, MemEvictFn evict, void *ctx, int priority)
{
    if (!evict || s_evictable_count >= MAX_EVICTABLES) return;
    s_evictables[s_evictable_count++] = { name ? name : "?", evict, ctx, priority };
}

size_t memReclaim()
{
    size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    // Evict least-important first. Simple selection over a tiny array — no need
    // to keep the registry sorted.
    bool done[MAX_EVICTABLES] = { false };
    for (int n = 0; n < s_evictable_count; n++) {
        int best = -1;
        for (int i = 0; i < s_evictable_count; i++) {
            if (done[i]) continue;
            if (best < 0 || s_evictables[i].priority < s_evictables[best].priority)
                best = i;
        }
        if (best < 0) break;
        done[best] = true;
        printf("[mem] evict '%s'\n", s_evictables[best].name);
        s_evictables[best].fn(s_evictables[best].ctx);
    }

    size_t after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t freed = after > before ? after - before : 0;
    printf("[mem] reclaim freed %u B internal\n", (unsigned)freed);
    return freed;
}
