// Oodle Kraken decoding.
//
// Oodle is proprietary: RedFS neither ships nor links it, binding it by name at
// runtime instead, so the only copy ever used is the one already in the user's
// own install. Nothing about Oodle gets redistributed.
//
// So a missing decoder has to be a recoverable runtime error rather than a link
// error -- outside the game there is no guarantee the DLL is there at all.

#include "internal.hpp"

#include <atomic>
#include <mutex>

#include <malloc.h>
#include <windows.h>

namespace redfs::oodle {
namespace {

// Oodle's header defaults everything after raw_len. GetProcAddress does not
// carry defaults, so the tail is spelled out. Most of it is passed with the
// values Oodle itself would have supplied -- the exception is the scratch pair,
// where taking the default is the whole bug; see "decoder scratch" below.
using DecompressFn = int64_t(__stdcall*)(const void* comp, int64_t comp_len, void* raw,
                                         int64_t raw_len, int32_t fuzz_safe, int32_t check_crc,
                                         int32_t verbosity, void* dec_buf_base, int64_t dec_buf_size,
                                         void* cb, void* cb_user, void* scratch, int64_t scratch_size,
                                         int32_t thread_phase);

// Sizes the scratch pair above.
using MemorySizeFn = int64_t(__stdcall*)(int32_t compressor, int64_t raw_len);

// Stored with release and read with acquire in decompress(), because
// g_scratch_size is a plain int64_t written immediately before it: a thread that
// saw the pointer but not the size would decode with no scratch, which is the
// one thing this file exists to prevent. Both are written once and never
// cleared, so available() and load() -- which only ask whether resolution
// happened -- can still read the pointer relaxed.
std::atomic<DecompressFn> g_decompress{nullptr};
int64_t g_scratch_size = 0;  // 0 means the size could not be queried; see try_load
std::mutex g_load_mutex;

// Oodle's defaults, matching WolvenKit's P/Invoke. fuzz_safe is the load-bearing
// one: segment bytes come from whatever mod is installed, and it is what obliges
// the decoder to stay inside raw_len instead of trusting the compressed stream.
constexpr int32_t kFuzzSafeYes = 1;
constexpr int32_t kCheckCrcNo = 0;
constexpr int32_t kVerbosityNone = 0;
constexpr int32_t kUnthreaded = 3;

// --- decoder scratch ---------------------------------------------------------
//
// Oodle takes its decoder scratch from the caller and reads a null one as
// "allocate it yourself" -- via the plugin allocator, which is process-wide
// state belonging to whoever installed it last.
//
// Standalone that is harmless: nobody installs one, so Oodle uses its own.
// In the game it is fatal. Cyberpunk installs an allocator whose entire purpose
// is to assert that it is never reached --
//
//   OodleMallocAligned called unexpectedly. Are we using an API we can pass
//   stack memory too instead?         (redCompression/src/wrapperKraken.cpp:85)
//
// -- because the engine only ever calls Oodle through APIs it can hand
// preallocated memory to. try_load reuses the resident DLL on purpose, so RedFS
// inherits that allocator along with it, and the first compressed read asserts
// and takes the game down. Measured against 2.31: one allocator call for every
// segment decoded, so it fires on read one, not on some rare path.
//
// The scratch is therefore ours to supply. OodleLZDecoder_MemorySizeNeeded
// (Invalid, -1) asks for the worst case over every compressor and every block
// size -- 462288 bytes on the Oodle shipped with 2.31, and flat once the block
// reaches 256 KiB. One size covers every call we make, so blocks can be pooled
// and reused rather than measured per segment.
constexpr int32_t kAnyCompressor = -1;  // OodleLZ_Compressor_Invalid: worst over all of them
constexpr int64_t kAnyBlockSize = -1;   // likewise, worst over all block sizes

// Blocks live on an intrusive free list -- not in a thread_local, and not in a
// container with static storage duration. RedFS is built to be statically linked
// into a plugin DLL that RED4ext calls FreeLibrary on, and a thread_local with a
// destructor registers a teardown callback into this module for every thread
// that ever decoded, to run long after the code is unmapped. Same reasoning as
// the leaked Worker in api.cpp; here free_scratch() does the releasing, so the
// list needs no destructor of its own.
//
// The link sits inside the block, so the list itself costs no allocation. Depth
// settles at peak concurrent decodes: one per synchronous caller, plus the
// async worker.
struct Block {
    Block* next;
};
constexpr size_t kBlockAlign = 64;  // clears a cache line, and gives `next` its own slot

std::mutex g_pool_mutex;
Block* g_pool = nullptr;

void* scratch_of(Block* b) { return reinterpret_cast<uint8_t*>(b) + kBlockAlign; }

// Null only under OOM: callers check that a size is known first.
Block* acquire() {
    {
        std::lock_guard<std::mutex> lock(g_pool_mutex);
        if (g_pool) {
            Block* b = g_pool;
            g_pool = b->next;
            return b;
        }
    }
    return static_cast<Block*>(
        ::_aligned_malloc(kBlockAlign + static_cast<size_t>(g_scratch_size), kBlockAlign));
}

void release(Block* b) {
    std::lock_guard<std::mutex> lock(g_pool_mutex);
    b->next = g_pool;
    g_pool = b;
}

void try_load(const std::string& game_dir) {
    // Inside the game the DLL is already resident -- reuse it rather than
    // pinning a second reference the host never asked for.
    HMODULE mod = ::GetModuleHandleW(L"oo2ext_7_win64.dll");

    if (!mod && !game_dir.empty()) {
        std::string dll = game_dir;
        if (!dll.empty() && dll.back() != '\\' && dll.back() != '/') dll += '\\';
        dll += "bin\\x64\\oo2ext_7_win64.dll";
        mod = ::LoadLibraryA(dll.c_str());
        if (!mod) log("oodle: LoadLibrary(%s) failed, error %lu", dll.c_str(), ::GetLastError());
    }
    if (!mod) mod = ::LoadLibraryW(L"oo2ext_7_win64.dll");  // last resort: search path
    if (!mod) return;

    auto* fn = reinterpret_cast<DecompressFn>(
        reinterpret_cast<void*>(::GetProcAddress(mod, "OodleLZ_Decompress")));
    if (!fn) {
        log("oodle: OodleLZ_Decompress not exported");
        return;
    }

    auto* size_fn = reinterpret_cast<MemorySizeFn>(
        reinterpret_cast<void*>(::GetProcAddress(mod, "OodleLZDecoder_MemorySizeNeeded")));
    if (size_fn) {
        const int64_t need = size_fn(kAnyCompressor, kAnyBlockSize);
        if (need > 0) g_scratch_size = need;
    }
    // Present on every Oodle the game has ever shipped, so this is the
    // out-of-game case. There is no safe constant to substitute: too small does
    // not fall back to allocating, it fails every decode outright. Letting Oodle
    // allocate is what RedFS always did and is correct wherever the game's
    // allocator is not installed -- which, if this branch is taken, is here.
    if (g_scratch_size <= 0)
        log("oodle: OodleLZDecoder_MemorySizeNeeded unavailable; falling back to Oodle's own "
            "allocator (unsafe in-process with the game)");

    g_decompress.store(fn, std::memory_order_release);
}

}  // namespace

bool load(const char* game_dir) {
    // Gated on "not yet RESOLVED", not "not yet attempted".
    //
    // std::call_once would run try_load exactly once ever, which lets a failed
    // first attempt poison every later one: redfs_depot_open_empty calls this
    // with no game directory, so outside the game it finds nothing, and a
    // subsequent redfs_depot_open with the real install path would never get to
    // try. Every compressed read then fails for the life of the process.
    // Retrying is cheap and idempotent, so gate on the result instead.
    if (g_decompress.load(std::memory_order_relaxed)) return true;

    std::lock_guard<std::mutex> lock(g_load_mutex);
    if (g_decompress.load(std::memory_order_relaxed)) return true;
    try_load(game_dir ? game_dir : "");
    return g_decompress.load(std::memory_order_relaxed) != nullptr;
}

bool available() { return g_decompress.load(std::memory_order_relaxed) != nullptr; }

int64_t decompress(const void* comp, int64_t comp_len, void* raw, int64_t raw_len) {
    const DecompressFn fn = g_decompress.load(std::memory_order_acquire);
    if (!fn) return -1;

    if (g_scratch_size <= 0)  // no size to pass; try_load has already said so
        return fn(comp, comp_len, raw, raw_len, kFuzzSafeYes, kCheckCrcNo, kVerbosityNone, nullptr,
                  0, nullptr, nullptr, nullptr, 0, kUnthreaded);

    Block* block = acquire();
    // Falling back to a null scratch under memory pressure would trade a failed
    // read for a killed process -- it is precisely the allocator call this
    // avoids. -1 reaches the caller as REDFS_E_OODLE.
    if (!block) return -1;

    const int64_t got =
        fn(comp, comp_len, raw, raw_len, kFuzzSafeYes, kCheckCrcNo, kVerbosityNone, nullptr, 0,
           nullptr, nullptr, scratch_of(block), g_scratch_size, kUnthreaded);
    release(block);
    return got;
}

void free_scratch() {
    Block* b;
    {
        std::lock_guard<std::mutex> lock(g_pool_mutex);
        b = g_pool;
        g_pool = nullptr;
    }
    while (b) {
        Block* next = b->next;
        ::_aligned_free(b);
        b = next;
    }
}

}  // namespace redfs::oodle
