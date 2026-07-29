// Oodle Kraken decoding.
//
// The decoder is *not* shipped with RedFS. Every Cyberpunk 2077 install has
// bin/x64/oo2ext_7_win64.dll, and inside the game process it is already loaded,
// so we resolve it at runtime. Nothing about Oodle gets redistributed.

#include "internal.hpp"

#include <atomic>
#include <mutex>

#include <windows.h>

namespace redfs::oodle {
namespace {

// The subset of the signature we use; the trailing parameters are all defaulted
// in Oodle's own headers and passed as zero here.
using DecompressFn = int64_t(__stdcall*)(const void* comp, int64_t comp_len, void* raw,
                                         int64_t raw_len, int32_t fuzz_safe, int32_t check_crc,
                                         int32_t verbosity, void* dec_buf_base, int64_t dec_buf_size,
                                         void* cb, void* cb_user, void* scratch, int64_t scratch_size,
                                         int32_t thread_phase);

// Atomic because available() and decompress() read it without going through
// load(), so they do not inherit load()'s synchronisation. The pointer is
// written once and never cleared, so relaxed ordering is sufficient.
std::atomic<DecompressFn> g_decompress{nullptr};
std::mutex g_load_mutex;

constexpr int32_t kFuzzSafeYes = 1;
constexpr int32_t kCheckCrcNo = 0;
constexpr int32_t kVerbosityNone = 0;
constexpr int32_t kUnthreaded = 3;

void try_load(const std::string& game_dir) {
    // Inside the game the DLL is already resident -- reuse it, do not load a
    // second copy.
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
    g_decompress.store(fn, std::memory_order_relaxed);
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
    if (g_decompress.load(std::memory_order_relaxed)) return true;  // won the race
    try_load(game_dir ? game_dir : "");
    return g_decompress.load(std::memory_order_relaxed) != nullptr;
}

bool available() { return g_decompress.load(std::memory_order_relaxed) != nullptr; }

int64_t decompress(const void* comp, int64_t comp_len, void* raw, int64_t raw_len) {
    const DecompressFn fn = g_decompress.load(std::memory_order_relaxed);
    if (!fn) return -1;
    return fn(comp, comp_len, raw, raw_len, kFuzzSafeYes, kCheckCrcNo, kVerbosityNone, nullptr, 0,
              nullptr, nullptr, nullptr, 0, kUnthreaded);
}

}  // namespace redfs::oodle
