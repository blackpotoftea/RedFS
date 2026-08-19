// Lifecycle and teardown tests.
//
// The unit tests cover behaviour inside one process. These cover how RedFS dies:
// abrupt exit, shutdown while work is in flight, and a real LoadLibrary /
// FreeLibrary cycle -- which is exactly what RED4ext does to a plugin.
//
// Several scenarios are about process teardown, so they cannot run in-process:
// the harness re-invokes itself as a child and inspects how the child died. A
// hang is a failure just as much as a crash, so every child is waited on with a
// timeout.
//
//   lifecycle_test              run every scenario
//   lifecycle_test <scenario>   run one (used for the child processes)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>

#include <windows.h>

#include "fixtures.hpp"
#include "redfs.h"

namespace {

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

std::string temp_path(const char* name) {
    char buf[512];
    const char* tmp = std::getenv("TEMP");
    std::snprintf(buf, sizeof buf, "%s\\redfs_life_%s", tmp ? tmp : ".", name);
    return buf;
}

// An archive holding one file split into many sizeable segments. Many segments
// is the point: the abort token is checked between them, so this is what proves
// a long read gives up promptly instead of running to completion.
std::string make_big_archive(const char* name, uint32_t buffers, uint32_t bytes_each,
                             uint64_t* out_key) {
    fixture::ArchiveBuilder ab;
    const uint64_t key = redfs_hash("base\\big\\payload.bin");
    std::vector<std::vector<uint8_t>> bufs;
    for (uint32_t i = 0; i < buffers; ++i)
        bufs.emplace_back(bytes_each, static_cast<uint8_t>(i));
    ab.add(key, std::vector<uint8_t>(1024, 0xAA), std::move(bufs));

    const std::string path = temp_path(name);
    fixture::ArchiveBuilder::write(path, ab.build());
    *out_key = key;
    return path;
}

struct Counters {
    volatile long completed = 0;
    volatile long cancelled = 0;
};

void CALLBACK_count(redfs_status st, redfs_blob b, void* user) {
    auto* c = static_cast<Counters*>(user);
    if (st == REDFS_E_CANCELLED)
        ::InterlockedIncrement(&c->cancelled);
    else if (st == REDFS_OK)
        ::InterlockedIncrement(&c->completed);
    redfs_blob_free(&b);
}

// --- child scenarios ---------------------------------------------------------

// Queue a lot of large reads and then just return from main, never calling
// redfs_shutdown. This is the "modder forgot" case. It must not hang and must
// not crash: at process exit Windows terminates the worker before running
// DLL_PROCESS_DETACH, and because RedFS never destroys its singletons nothing
// tries to take a mutex the dead thread was holding.
int scenario_exit_without_shutdown() {
    uint64_t key = 0;
    const std::string path = make_big_archive("exit.archive", 24, 2 * 1024 * 1024, &key);

    redfs_depot* depot = nullptr;
    if (redfs_depot_open_empty(&depot) != REDFS_OK) return 90;
    if (redfs_depot_mount(depot, path.c_str()) != REDFS_OK) return 91;

    static Counters counters;
    for (int i = 0; i < 500; ++i)
        redfs_read_async(depot, key, REDFS_PART_ALL, CALLBACK_count, &counters);

    // Deliberately no drain, no shutdown, no close. Walk out mid-flight.
    std::printf("child: exiting with work in flight\n");
    std::fflush(stdout);
    return 0;
}

// redfs_find must refuse a dictionary that has never been populated, rather than
// reporting an empty success -- "you never loaded a path list" and "nothing
// matched" are different answers.
//
// This lives here rather than in redfs_test because the dictionary is a
// process-global singleton that is deliberately never destroyed, so it only
// reaches the empty state once per process. redfs_test cannot host that: under
// _DEBUG its runner executes the whole suite three times (two warm-up passes to
// populate the leaked singletons, then the measured pass), so any assertion
// about virgin global state is true on pass one and false forever after. A
// freshly spawned child is the only place the precondition holds.
int scenario_virgin_dictionary() {
    fixture::ArchiveBuilder ab;
    ab.add(redfs_hash("base\\virgin\\x.mesh"), std::vector<uint8_t>(16, 0x5A));
    const std::string path = temp_path("virgin.archive");
    fixture::ArchiveBuilder::write(path, ab.build());

    redfs_depot* depot = nullptr;
    if (redfs_depot_open_empty(&depot) != REDFS_OK) return 1;
    if (redfs_depot_mount(depot, path.c_str()) != REDFS_OK) return 1;

    int rc = 0;
    if (redfs_path_count() != 0) {
        std::printf("child: dictionary was not empty at startup (%u entries)\n",
                    redfs_path_count());
        rc = 1;
    }

    // The file IS mounted and IS findable by hash -- only its NAME is unknown.
    // That is the case a caller misreads as "my pattern is wrong".
    if (!redfs_exists(depot, redfs_hash("base\\virgin\\x.mesh"))) {
        std::printf("child: fixture did not mount\n");
        rc = 1;
    }

    uint32_t matched = 123;
    auto sink = [](uint64_t, const char*, void*) -> int { return 1; };
    const redfs_status st = redfs_find(depot, "*.mesh", sink, nullptr, &matched);
    // NO_DICTIONARY, specifically -- not NOT_FOUND, which is what redfs_read
    // returns for "no such file" and would read here as "nothing matched".
    if (st != REDFS_E_NO_DICTIONARY) {
        std::printf("child: empty dictionary gave %s, wanted no path dictionary loaded\n",
                    redfs_status_string(st));
        rc = 1;
    }
    if (matched != 0) {
        std::printf("child: out_matched was %u, wanted 0 on the failure path\n", matched);
        rc = 1;
    }

    // And once something IS loaded, a pattern that matches nothing is a success.
    redfs_path_enable();
    redfs_path_add("base\\virgin\\x.mesh");
    matched = 123;
    if (redfs_find(depot, "base\\virgin\\nothing*.xbm", sink, nullptr, &matched) != REDFS_OK ||
        matched != 0) {
        std::printf("child: a loaded dictionary matching nothing should be OK with 0\n");
        rc = 1;
    }

    redfs_depot_close(depot);
    std::remove(path.c_str());
    if (!rc) std::printf("child: empty dictionary refused, loaded-but-no-match accepted\n");
    std::fflush(stdout);
    return rc;
}

// --- path cache round trip ---------------------------------------------------
//
// The claim the whole design rests on: the run that RESTORES the dictionary
// knows everything the run that built it knew. It cannot be tested in
// redfs_test -- the dictionary is a never-destroyed global, so by the time a
// second "boot" ran in-process the entries would still be there from the first
// and a restore that dropped every one of them would still pass.
//
// Two children over one file. The first teaches and writes; the second starts
// from nothing and must come back whole.

const char* pc_cache_file() {
    static const std::string s = temp_path("pathcache.bin");
    return s.c_str();
}
const char* pc_archive_file() {
    static const std::string s = temp_path("pathcache.archive");
    return s.c_str();
}
const char* pc_count_file() {
    static const std::string s = temp_path("pathcache.count");
    return s.c_str();
}

// Paths reachable only through sources redfs_path_load would have filtered out:
// nothing mounted holds either, so a restore that filtered would drop both.
const char* kPcImported = "base\\pathcache\\life_import.mt";
const char* kPcAdded = "base\\pathcache\\life_added.mesh";

std::vector<uint8_t> pc_cr2w_with_import(const char* import_path) {
    fixture::Cr2wBuilder b;
    b.import(import_path, "IMaterial");
    b.begin_chunk("Root");
    b.prop_u32("x", 1);
    b.end_chunk();
    return b.build();
}

int scenario_path_cache_teach() {
    fixture::ArchiveBuilder ab;
    ab.add(redfs_hash("base\\pathcache\\life.bin"), std::vector<uint8_t>(32, 0x7E));
    fixture::ArchiveBuilder::write(pc_archive_file(), ab.build());
    std::remove(pc_cache_file());

    redfs_depot* depot = nullptr;
    if (redfs_depot_open_empty(&depot) != REDFS_OK) return 1;
    if (redfs_depot_mount(depot, pc_archive_file()) != REDFS_OK) return 1;

    int rc = 0;
    if (redfs_path_count() != 0) {
        std::printf("child: dictionary was not empty at startup (%u)\n", redfs_path_count());
        rc = 1;
    }
    if (redfs_path_cache_open(pc_cache_file()) != REDFS_OK) {
        std::printf("child: path cache would not open\n");
        return 1;
    }

    // Learned, not loaded: opening the cache is the ONLY thing that switched the
    // dictionary on here -- no redfs_path_enable, no redfs_path_load -- so an
    // import that fails to land means open forgot to enable it.
    const std::vector<uint8_t> doc = pc_cr2w_with_import(kPcImported);
    redfs_cr2w* cr2w = nullptr;
    if (redfs_cr2w_open(doc.data(), doc.size(), &cr2w) != REDFS_OK) {
        std::printf("child: fixture cr2w would not parse\n");
        rc = 1;
    }
    redfs_cr2w_close(cr2w);
    if (!redfs_path_from_hash(redfs_hash(kPcImported))) {
        std::printf("child: opening a path cache did not enable import learning\n");
        rc = 1;
    }
    redfs_path_add(kPcAdded);

    uint32_t pending = 99;
    if (redfs_path_cache_pending(depot, nullptr, 0, &pending) != REDFS_OK || pending != 1) {
        std::printf("child: expected 1 unharvested archive, got %u\n", pending);
        rc = 1;
    }
    if (redfs_path_cache_mark(depot, 0) != REDFS_OK) {
        std::printf("child: mark failed\n");
        rc = 1;
    }

    const uint32_t total = redfs_path_count();
    redfs_path_cache_close();  // flushes
    redfs_depot_close(depot);

    FILE* f = std::fopen(pc_count_file(), "wb");
    if (!f) return 1;
    std::fprintf(f, "%u", total);
    std::fclose(f);

    if (!rc) std::printf("child: taught and wrote %u paths\n", total);
    std::fflush(stdout);
    return rc;
}

int scenario_path_cache_restore() {
    redfs_depot* depot = nullptr;
    if (redfs_depot_open_empty(&depot) != REDFS_OK) return 1;
    if (redfs_depot_mount(depot, pc_archive_file()) != REDFS_OK) return 1;

    int rc = 0;
    if (redfs_path_count() != 0) {
        std::printf("child: dictionary was not empty at startup (%u)\n", redfs_path_count());
        rc = 1;
    }

    uint32_t expected = 0;
    FILE* f = std::fopen(pc_count_file(), "rb");
    if (!f || std::fscanf(f, "%u", &expected) != 1) {
        std::printf("child: no count from the teaching run\n");
        if (f) std::fclose(f);
        return 1;
    }
    std::fclose(f);

    if (redfs_path_cache_open(pc_cache_file()) != REDFS_OK) {
        std::printf("child: path cache would not open\n");
        return 1;
    }

    // THE assertion. Not ">= 1", not "the one I looked up": the count. A restore
    // that ran its entries through the depot filter comes back smaller here
    // while every individual lookup a test happened to try still worked.
    if (redfs_path_count() != expected) {
        std::printf("child: restored %u paths, the run that wrote them knew %u\n",
                    redfs_path_count(), expected);
        rc = 1;
    }
    // Both of these are names NO mounted archive holds. That is exactly what the
    // filter would have removed.
    if (!redfs_path_from_hash(redfs_hash(kPcImported))) {
        std::printf("child: the import-learned path did not survive the round trip\n");
        rc = 1;
    }
    if (!redfs_path_from_hash(redfs_hash(kPcAdded))) {
        std::printf("child: the added path did not survive the round trip\n");
        rc = 1;
    }
    // And they are still only NAMES -- restoring must not imply presence.
    if (redfs_exists(depot, redfs_hash(kPcAdded))) {
        std::printf("child: a restored name reported as present in the depot\n");
        rc = 1;
    }

    uint32_t pending = 99;
    if (redfs_path_cache_pending(depot, nullptr, 0, &pending) != REDFS_OK || pending != 0) {
        std::printf("child: archive was harvested last run but reads as %u pending\n", pending);
        rc = 1;
    }

    // Learning still works on the restored dictionary. A restore that filled the
    // dictionary without enabling it looks perfect until the first new mod.
    const char* fresh = "base\\pathcache\\life_after_restore.mt";
    const std::vector<uint8_t> doc = pc_cr2w_with_import(fresh);
    redfs_cr2w* cr2w = nullptr;
    if (redfs_cr2w_open(doc.data(), doc.size(), &cr2w) != REDFS_OK) rc = 1;
    redfs_cr2w_close(cr2w);
    if (!redfs_path_from_hash(redfs_hash(fresh))) {
        std::printf("child: a restored dictionary stopped learning new imports\n");
        rc = 1;
    }

    redfs_path_cache_close();
    redfs_depot_close(depot);
    std::remove(pc_cache_file());
    std::remove(pc_archive_file());
    std::remove(pc_count_file());

    if (!rc) std::printf("child: %u paths restored intact, nothing left to harvest\n", expected);
    std::fflush(stdout);
    return rc;
}

// The same, but through ExitProcess -- a harder abort than returning from main,
// because no C++ cleanup runs at all.
int scenario_abrupt_exit_process() {
    uint64_t key = 0;
    const std::string path = make_big_archive("abrupt.archive", 24, 2 * 1024 * 1024, &key);

    redfs_depot* depot = nullptr;
    if (redfs_depot_open_empty(&depot) != REDFS_OK) return 90;
    if (redfs_depot_mount(depot, path.c_str()) != REDFS_OK) return 91;

    static Counters counters;
    for (int i = 0; i < 500; ++i)
        redfs_read_async(depot, key, REDFS_PART_ALL, CALLBACK_count, &counters);

    std::printf("child: ExitProcess with work in flight\n");
    std::fflush(stdout);
    ::ExitProcess(0);
}

// Shutdown while a deep queue of large reads is running. Prints the latency so
// the parent can assert it is bounded rather than proportional to the queue.
int scenario_shutdown_latency() {
    uint64_t key = 0;
    // ~96 MB of payload per read, across 48 segments.
    const std::string path = make_big_archive("latency.archive", 48, 2 * 1024 * 1024, &key);

    redfs_depot* depot = nullptr;
    if (redfs_depot_open_empty(&depot) != REDFS_OK) return 90;
    if (redfs_depot_mount(depot, path.c_str()) != REDFS_OK) return 91;

    static Counters counters;
    constexpr int kJobs = 400;
    for (int i = 0; i < kJobs; ++i)
        redfs_read_async(depot, key, REDFS_PART_ALL, CALLBACK_count, &counters);

    // Let the worker actually get into a read before pulling the rug.
    ::Sleep(20);

    const auto t0 = Clock::now();
    redfs_shutdown();
    const double took = ms_since(t0);

    const long done = counters.completed + counters.cancelled;
    std::printf("child: shutdown took %.1f ms; %ld/%d resolved (%ld done, %ld cancelled)\n", took,
                done, kJobs, counters.completed, counters.cancelled);
    std::fflush(stdout);

    redfs_depot_close(depot);
    std::remove(path.c_str());

    if (done != kJobs) return 80;      // a callback went missing
    if (took > 2000.0) return 81;      // not bounded -- it drained instead of cancelling
    return 0;
}

// The real plugin lifecycle: load the shared library, use it, shut it down,
// unload it. Repeated, because the failure mode is a thread surviving the
// unload and faulting later.
constexpr int kSkipped = 3;

int scenario_dll_load_unload(const char* dll_path) {
    // Configurations that build only the static library have nothing to load.
    // That is a skip, not a failure -- reporting it as failure would train
    // people to ignore this test.
    if (::GetFileAttributesA(dll_path) == INVALID_FILE_ATTRIBUTES) {
        std::printf("child: %s not built in this configuration\n", dll_path);
        return kSkipped;
    }

    for (int round = 0; round < 3; ++round) {
        HMODULE mod = ::LoadLibraryA(dll_path);
        if (!mod) {
            std::printf("child: LoadLibrary(%s) failed, error %lu\n", dll_path, ::GetLastError());
            return 92;
        }

        auto fn_open_empty = reinterpret_cast<redfs_status (*)(redfs_depot**)>(
            reinterpret_cast<void*>(::GetProcAddress(mod, "redfs_depot_open_empty")));
        auto fn_mount = reinterpret_cast<redfs_status (*)(redfs_depot*, const char*)>(
            reinterpret_cast<void*>(::GetProcAddress(mod, "redfs_depot_mount")));
        auto fn_hash = reinterpret_cast<uint64_t (*)(const char*)>(
            reinterpret_cast<void*>(::GetProcAddress(mod, "redfs_hash")));
        auto fn_async = reinterpret_cast<redfs_status (*)(const redfs_depot*, uint64_t, uint32_t,
                                                          redfs_read_fn, void*)>(
            reinterpret_cast<void*>(::GetProcAddress(mod, "redfs_read_async")));
        auto fn_shutdown = reinterpret_cast<void (*)()>(
            reinterpret_cast<void*>(::GetProcAddress(mod, "redfs_shutdown")));
        auto fn_close = reinterpret_cast<void (*)(redfs_depot*)>(
            reinterpret_cast<void*>(::GetProcAddress(mod, "redfs_depot_close")));
        auto fn_blob_free = reinterpret_cast<void (*)(redfs_blob*)>(
            reinterpret_cast<void*>(::GetProcAddress(mod, "redfs_blob_free")));

        if (!fn_open_empty || !fn_mount || !fn_hash || !fn_async || !fn_shutdown || !fn_close ||
            !fn_blob_free) {
            std::printf("child: missing exports in %s\n", dll_path);
            return 93;
        }

        // The archive is built by the statically-linked fixture code, but hashed
        // by the DLL so the key matches what the DLL's depot will look up.
        uint64_t key = 0;
        char name[64];
        std::snprintf(name, sizeof name, "dll%d.archive", round);
        const std::string path = make_big_archive(name, 16, 1024 * 1024, &key);

        redfs_depot* depot = nullptr;
        if (fn_open_empty(&depot) != REDFS_OK) return 94;
        if (fn_mount(depot, path.c_str()) != REDFS_OK) return 95;

        // The blob is allocated inside the DLL, so it must be released by the
        // DLL's own redfs_blob_free -- not this module's free(). They happen to
        // share a CRT here, but relying on that is how cross-module heap bugs
        // start. The free function travels through the user pointer because a
        // capturing lambda cannot decay to the C callback type.
        // Passed straight through `user`: a capturing lambda cannot decay to the C
        // callback type, and a function pointer already fits in void*.
        //
        // It must NOT be cached in a static. This function runs once per round and
        // each round loads a fresh image, so a function-local static would hold
        // round 0's address forever and later rounds would call into memory that
        // FreeLibrary has unmapped. That reproduced as an intermittent
        // access violation -- intermittent because the reloaded DLL usually lands
        // at the same base, so the stale pointer happens to remain valid.
        using FreeFn = void (*)(redfs_blob*);
        for (int i = 0; i < 200; ++i)
            fn_async(depot, fn_hash("base\\big\\payload.bin"), REDFS_PART_ALL,
                     [](redfs_status, redfs_blob b, void* user) {
                         reinterpret_cast<FreeFn>(user)(&b);
                     },
                     reinterpret_cast<void*>(fn_blob_free));

        // This is the call under test. Without it, FreeLibrary below unmaps code
        // the worker is still executing.
        fn_shutdown();
        fn_close(depot);

        if (!::FreeLibrary(mod)) {
            std::printf("child: FreeLibrary failed, error %lu\n", ::GetLastError());
            return 96;
        }
        // If the worker had survived, it would now be running in unmapped memory.
        // Give it a window to fault before the next round.
        ::Sleep(30);
        std::remove(path.c_str());
        std::printf("child: load/use/shutdown/unload round %d ok\n", round);
        std::fflush(stdout);
    }
    return 0;
}

// --- parent ------------------------------------------------------------------

struct Result {
    bool ok;
    DWORD exit_code;
    double ms;
    bool timed_out;
    std::string output;
};

Result run_child(const std::string& self, const char* scenario, DWORD timeout_ms) {
    Result r{false, 0, 0, false, {}};

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read_end = nullptr, write_end = nullptr;
    if (!::CreatePipe(&read_end, &write_end, &sa, 0)) return r;
    ::SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    std::string cmd = "\"" + self + "\" " + scenario;
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_end;
    si.hStdError = write_end;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};

    const auto t0 = Clock::now();
    if (!::CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si,
                          &pi)) {
        ::CloseHandle(read_end);
        ::CloseHandle(write_end);
        return r;
    }
    ::CloseHandle(write_end);

    // Drain the pipe while waiting, so a chatty child cannot fill it and block.
    std::string out;
    char buf[512];
    DWORD got = 0;
    while (::ReadFile(read_end, buf, sizeof buf - 1, &got, nullptr) && got) {
        buf[got] = 0;
        out += buf;
    }
    ::CloseHandle(read_end);

    const DWORD wait = ::WaitForSingleObject(pi.hProcess, timeout_ms);
    r.ms = ms_since(t0);
    r.output = out;

    if (wait == WAIT_TIMEOUT) {
        r.timed_out = true;
        ::TerminateProcess(pi.hProcess, 0xDEAD);
        ::WaitForSingleObject(pi.hProcess, 2000);
    } else {
        ::GetExitCodeProcess(pi.hProcess, &r.exit_code);
        r.ok = (r.exit_code == 0);
    }
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    return r;
}

int failures = 0;

void check(const char* name, const Result& r, DWORD budget_ms) {
    std::printf("%-28s ", name);
    if (r.timed_out) {
        std::printf("FAIL  hung (>%lu ms)\n", budget_ms);
        ++failures;
    } else if (r.exit_code == 3) {
        std::printf("skip  %.0f ms\n", r.ms);
    } else if (!r.ok) {
        // 0xC0000005 and friends surface here as the exit code.
        std::printf("FAIL  exit 0x%08lX after %.0f ms\n", r.exit_code, r.ms);
        ++failures;
    } else {
        std::printf("ok    %.0f ms\n", r.ms);
    }
    for (const auto& line : std::vector<std::string>{r.output})
        if (!line.empty()) {
            std::string indented = "    " + line;
            for (size_t i = 0; i < indented.size(); ++i)
                if (indented[i] == '\n' && i + 1 < indented.size())
                    indented.insert(i + 1, "    "), i += 4;
            std::printf("%s", indented.c_str());
            if (indented.back() != '\n') std::printf("\n");
        }
}

}  // namespace

int main(int argc, char** argv) {
    char self_path[MAX_PATH];
    ::GetModuleFileNameA(nullptr, self_path, MAX_PATH);

    if (argc >= 2) {
        const char* s = argv[1];
        if (std::strcmp(s, "exit-without-shutdown") == 0) return scenario_exit_without_shutdown();
        if (std::strcmp(s, "virgin-dictionary") == 0) return scenario_virgin_dictionary();
        if (std::strcmp(s, "path-cache-teach") == 0) return scenario_path_cache_teach();
        if (std::strcmp(s, "path-cache-restore") == 0) return scenario_path_cache_restore();
        if (std::strcmp(s, "abrupt-exit") == 0) return scenario_abrupt_exit_process();
        if (std::strcmp(s, "shutdown-latency") == 0) return scenario_shutdown_latency();
        if (std::strcmp(s, "dll-unload") == 0) {
            // RedFS.dll sits next to this executable.
            std::string dll = self_path;
            const size_t slash = dll.find_last_of("\\/");
            dll = dll.substr(0, slash + 1) + "RedFS.dll";
            return scenario_dll_load_unload(dll.c_str());
        }
        std::printf("unknown scenario: %s\n", s);
        return 2;
    }

    std::printf("RedFS lifecycle tests\n\n");

    // Each budget is generous next to the expected cost; the point is to catch a
    // hang, not to benchmark.
    // Not about teardown, but it needs the same thing the teardown scenarios do:
    // a process whose global singletons have never been touched.
    check("virgin path dictionary", run_child(self_path, "virgin-dictionary", 15000), 15000);
    // Ordered, and the pair IS the test: the second child restores what the
    // first wrote, from a dictionary that has never held anything.
    check("path cache: teach and write", run_child(self_path, "path-cache-teach", 15000), 15000);
    check("path cache: restore into a virgin dictionary",
          run_child(self_path, "path-cache-restore", 15000), 15000);
    check("exit without shutdown", run_child(self_path, "exit-without-shutdown", 15000), 15000);
    check("abrupt ExitProcess", run_child(self_path, "abrupt-exit", 15000), 15000);
    check("shutdown under load", run_child(self_path, "shutdown-latency", 20000), 20000);
    check("dll load/unload cycle", run_child(self_path, "dll-unload", 30000), 30000);

    std::printf("\n%s (%d failures)\n", failures ? "LIFECYCLE TESTS FAILED" : "LIFECYCLE TESTS PASSED",
                failures);
    return failures ? 1 : 0;
}
