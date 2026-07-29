// Oodle Kraken decoding.
//
// The decoder is *not* shipped with RedFS. Every Cyberpunk 2077 install has
// bin/x64/oo2ext_7_win64.dll, and inside the game process it is already loaded,
// so we resolve it at runtime. Nothing about Oodle gets redistributed.

#include "internal.hpp"

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

std::once_flag g_once;
DecompressFn g_decompress = nullptr;

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

    g_decompress = reinterpret_cast<DecompressFn>(
        reinterpret_cast<void*>(::GetProcAddress(mod, "OodleLZ_Decompress")));
    if (!g_decompress) log("oodle: OodleLZ_Decompress not exported");
}

}  // namespace

bool load(const char* game_dir) {
    std::string dir = game_dir ? game_dir : "";
    std::call_once(g_once, [&] { try_load(dir); });
    return g_decompress != nullptr;
}

bool available() { return g_decompress != nullptr; }

int64_t decompress(const void* comp, int64_t comp_len, void* raw, int64_t raw_len) {
    if (!g_decompress) return -1;
    return g_decompress(comp, comp_len, raw, raw_len, kFuzzSafeYes, kCheckCrcNo, kVerbosityNone,
                        nullptr, 0, nullptr, nullptr, nullptr, 0, kUnthreaded);
}

}  // namespace redfs::oodle
