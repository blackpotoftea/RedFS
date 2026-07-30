// A minimal RED4ext plugin that proves RedFS works from inside the game.
//
// Everything else in this repository runs in a standalone test process. Three
// things can only be exercised in the game, and this exists for those three:
//
//   1. Install auto-detection -- redfs_depot_open(nullptr, ...) walks up from the
//      running executable. Outside the game that path is never taken.
//   2. Oodle resolution against the RESIDENT oo2ext_7_win64.dll. The game has it
//      loaded already; a standalone tool has to find it on disk.
//   3. Unload ordering. RED4ext calls Main(Unload) from its plugin manager and
//      then FreeLibrary's this DLL. redfs_shutdown() has to have joined the
//      worker by then or the thread is executing unmapped code.
//
// It deliberately does no RTTI, registers no types and hooks nothing. RedFS does
// not touch live game state, and a test of RedFS should not either.

#include <RED4ext/RED4ext.hpp>

#include "redfs.h"

namespace {

redfs_depot* g_depot = nullptr;
RED4ext::v1::PluginHandle g_handle = nullptr;
const RED4ext::v1::Sdk* g_sdk = nullptr;

// RedFS diagnostics can arrive on its worker thread and concurrently, and the
// message dies with the call -- so format it out immediately rather than storing
// the pointer. RED4ext's logger is safe to call from any thread.
void on_redfs_log(const char* message, void*) {
    if (g_sdk && g_sdk->logger) g_sdk->logger->InfoF(g_handle, "[redfs] %s", message);
}

// Reads one real file end to end. Any of these steps failing in the game but
// passing in a test harness is exactly the kind of thing this plugin is for.
void smoke_test() {
    auto* log = g_sdk->logger;

    log->InfoF(g_handle, "archives mounted: %u, files: %llu, index: %.1f MB",
               redfs_depot_archive_count(g_depot),
               static_cast<unsigned long long>(redfs_depot_file_count(g_depot)),
               redfs_depot_index_bytes(g_depot) / 1048576.0);

    if (!redfs_oodle_available()) {
        // Inside the game this should be impossible: the DLL is already resident.
        // If it ever fires here, the resolution order in oodle.cpp is wrong for
        // the real process, which is precisely what no offline test can tell us.
        log->Error(g_handle, "[redfs] Oodle NOT available in-process -- "
                             "compressed reads will fail");
        return;
    }

    // A stock texture that exists on every install. Read it as a DDS, which
    // exercises the whole path: index lookup, Kraken decode, CR2W parse, header
    // synthesis.
    const char* path = "base\\gameplay\\gui\\common\\icons\\mappin_quest.xbm";
    const uint64_t hash = redfs_hash(path);

    if (!redfs_exists(g_depot, hash)) {
        log->WarnF(g_handle, "[redfs] %s not in this install; skipping the read", path);
        return;
    }

    redfs_texture_desc desc{};
    if (redfs_texture_desc_of(g_depot, hash, &desc) != REDFS_OK) {
        log->ErrorF(g_handle, "[redfs] desc failed: %s", redfs_last_error());
        return;
    }

    redfs_blob dds{};
    if (redfs_texture_read_dds(g_depot, hash, &dds) != REDFS_OK) {
        log->ErrorF(g_handle, "[redfs] dds failed: %s", redfs_last_error());
        return;
    }
    log->InfoF(g_handle, "[redfs] read %s: %ux%u, %u mips, dxgi %u, %llu bytes of DDS", path,
               desc.width, desc.height, desc.mip_count, desc.dxgi_format,
               static_cast<unsigned long long>(dds.size));
    redfs_blob_free(&dds);
}

}  // namespace

RED4EXT_C_EXPORT bool RED4EXT_CALL Main(RED4ext::v1::PluginHandle aHandle,
                                        RED4ext::v1::EMainReason aReason,
                                        const RED4ext::v1::Sdk* aSdk) {
    switch (aReason) {
    case RED4ext::v1::EMainReason::Load: {
        g_handle = aHandle;
        g_sdk = aSdk;

        // Before anything else: a struct-layout mismatch is silent. redfs_mesh_chunk
        // has only ever grown by appending, so field reads still land and only the
        // stride is wrong.
        if (redfs_abi_version() != REDFS_ABI_VERSION) {
            aSdk->logger->ErrorF(aHandle, "[redfs] ABI %u, built against %u -- refusing to load",
                                 redfs_abi_version(), REDFS_ABI_VERSION);
            return false;
        }

        redfs_set_log(on_redfs_log, nullptr);

        // nullptr: auto-detect the install from the running process. This is the
        // branch that only exists for the in-game case.
        if (redfs_depot_open(nullptr, REDFS_SCAN_ALL, &g_depot) != REDFS_OK) {
            aSdk->logger->ErrorF(aHandle, "[redfs] depot open failed: %s", redfs_last_error());
            return false;
        }

        smoke_test();
        break;
    }

    case RED4ext::v1::EMainReason::Unload: {
        // Order matters and is the whole point of this scenario. redfs_shutdown
        // joins the worker; without it, the FreeLibrary that RED4ext performs
        // after this returns unmaps code the worker is still running.
        redfs_shutdown();
        redfs_depot_close(g_depot);
        g_depot = nullptr;
        // The sink points at this module's code, so it must not outlive it.
        redfs_set_log(nullptr, nullptr);
        g_sdk = nullptr;
        break;
    }
    }
    return true;
}

RED4EXT_C_EXPORT void RED4EXT_CALL Query(RED4ext::v1::PluginInfo* aInfo) {
    aInfo->name = L"RedFS.SmokeTest";
    aInfo->author = L"RedFS";
    aInfo->version = RED4EXT_V1_SEMVER(1, 0, 0);
    // LATEST rather than pinning a build: RedFS reads files and touches no game
    // internals, so there is nothing here for a game patch to invalidate.
    aInfo->runtime = RED4EXT_V1_RUNTIME_VERSION_LATEST;
    aInfo->sdk = RED4EXT_V1_SDK_VERSION_CURRENT;
}

RED4EXT_C_EXPORT uint32_t RED4EXT_CALL Supports() {
    return RED4EXT_API_VERSION_1;
}
