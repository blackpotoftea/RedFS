// The C ABI surface: depot lifetime, blobs, the async worker, and thin
// forwarding to the internals.

#include "internal.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>

#include <windows.h>

namespace redfs {

// --- diagnostics -------------------------------------------------------------

namespace {
// The callback and its context are one atomic unit. Two separate globals could
// be read half-updated -- and the null check and the call would be separate
// loads of a plain global, so redfs_set_log racing with a worker-thread log()
// could produce an indirect call through null, or a callback paired with
// another logger's context.
struct LogSink {
    redfs_log_fn fn;
    void* user;
};
std::atomic<LogSink> g_log{LogSink{nullptr, nullptr}};
thread_local char t_last_error[512] = {};

void emit(const char* message) {
    const LogSink sink = g_log.load(std::memory_order_relaxed);
    if (sink.fn) sink.fn(message, sink.user);
}
}  // namespace

void log(const char* fmt, ...) {
    if (!g_log.load(std::memory_order_relaxed).fn) return;  // cheap early out
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    emit(buf);
}

redfs_status fail(redfs_status status, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(t_last_error, sizeof(t_last_error), fmt, ap);
    va_end(ap);
    emit(t_last_error);
    return status;
}

// --- blobs -------------------------------------------------------------------

redfs_status blob_alloc(uint64_t size, redfs_blob* out) {
    out->data = nullptr;
    out->size = 0;
    out->reserved = nullptr;
    // +1 so even a zero-length read yields a non-null, NUL-terminated pointer.
    // No early return for size == 0: it would hand back data == nullptr, which
    // is exactly the case a caller treating the blob as a C string would trip
    // over -- and the one this padding exists to make safe.
    void* p = std::malloc(static_cast<size_t>(size) + 1);
    if (!p) return fail(REDFS_E_OOM, "out of memory allocating %llu bytes",
                        static_cast<unsigned long long>(size));
    static_cast<uint8_t*>(p)[size] = 0;
    out->data = static_cast<uint8_t*>(p);
    out->size = size;
    out->reserved = p;
    return REDFS_OK;
}

// --- forward decls implemented in formats.cpp --------------------------------

redfs_status texture_desc_of(const redfs_depot*, uint64_t, redfs_texture_desc*);
redfs_status texture_read_raw(const redfs_depot*, uint64_t, redfs_texture_desc*, redfs_blob*);
redfs_status texture_read_dds(const redfs_depot*, uint64_t, redfs_blob*);
redfs_status audio_probe(const redfs_depot*, uint64_t, redfs_audio_format*);
redfs_status audio_info_of(const redfs_depot*, uint64_t, redfs_audio_info*);
redfs_status audio_info_parse(const void*, uint64_t, redfs_audio_info*);
redfs_status audio_walk_chunks(const void*, uint64_t, redfs_riff_chunk_fn, void*);
redfs_status mesh_desc_of(const redfs_depot*, uint64_t, redfs_mesh_desc*);

// --- depot construction ------------------------------------------------------

namespace {

std::string join(const std::string& a, const char* b) {
    std::string r = a;
    if (!r.empty() && r.back() != '\\' && r.back() != '/') r += '\\';
    r += b;
    return r;
}

bool dir_exists(const std::string& p) {
    const DWORD a = ::GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

void list_archives(const std::string& dir, std::vector<std::string>* out) {
    WIN32_FIND_DATAA fd{};
    HANDLE h = ::FindFirstFileA(join(dir, "*.archive").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::string> names;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        names.push_back(fd.cFileName);
    } while (::FindNextFileA(h, &fd));
    ::FindClose(h);
    // The game mounts in name order; match it so overrides resolve the same way.
    std::sort(names.begin(), names.end());
    for (const auto& n : names) out->push_back(join(dir, n.c_str()));
}

void list_subdirs(const std::string& dir, std::vector<std::string>* out) {
    WIN32_FIND_DATAA fd{};
    HANDLE h = ::FindFirstFileA(join(dir, "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (std::strcmp(fd.cFileName, ".") == 0 || std::strcmp(fd.cFileName, "..") == 0) continue;
        out->push_back(fd.cFileName);
    } while (::FindNextFileA(h, &fd));
    ::FindClose(h);
    std::sort(out->begin(), out->end());
}

// REDmod layout: mods/<name>/archives/*.archive, recursively.
//
// Ordering matches WolvenKit's ArchiveManager, which is the reference for what
// the game does: mod folders in name order, but the archives *within* one folder
// sorted then reversed -- so inside a single REDmod the alphabetically-first
// archive mounts last and therefore wins.
void list_redmod_archives(const std::string& mods_root, std::vector<std::string>* out) {
    std::vector<std::string> folders;
    list_subdirs(mods_root, &folders);
    for (const auto& folder : folders) {
        const std::string archives_dir = join(join(mods_root, folder.c_str()), "archives");
        if (!dir_exists(archives_dir)) continue;

        std::vector<std::string> found;
        list_archives(archives_dir, &found);  // already name-sorted
        std::reverse(found.begin(), found.end());
        out->insert(out->end(), found.begin(), found.end());
    }
}

// Walk up from the running executable looking for the install root.
std::string detect_game_dir() {
    char buf[MAX_PATH];
    const DWORD n = ::GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return "";
    std::string p(buf, n);
    for (int i = 0; i < 5; ++i) {
        const size_t slash = p.find_last_of("\\/");
        if (slash == std::string::npos) break;
        p.resize(slash);
        if (dir_exists(join(p, "archive\\pc"))) return p;
    }
    return "";
}

// Rebuild the hash -> (archive, entry) table. Later archives win, mirroring the
// game's own load order.
void reindex(redfs_depot* d) {
    size_t total = 0;
    for (auto* a : d->archives) total += a->entry_count();

    d->refs.clear();
    d->refs.reserve(total);
    for (uint32_t ai = 0; ai < d->archives.size(); ++ai) {
        const Archive* a = d->archives[ai];
        for (uint32_t e = 0; e < a->entry_count(); ++e)
            d->refs.push_back(redfs_depot::Ref{a->entry_hash(e), ai, e});
    }

    // stable_sort keeps mount order within a hash, so the last duplicate is the
    // highest-priority archive.
    std::stable_sort(d->refs.begin(), d->refs.end(),
                     [](const redfs_depot::Ref& x, const redfs_depot::Ref& y) {
                         return x.hash < y.hash;
                     });
    size_t w = 0;
    for (size_t r = 0; r < d->refs.size(); ++r) {
        if (w > 0 && d->refs[w - 1].hash == d->refs[r].hash)
            d->refs[w - 1] = d->refs[r];  // later mount overrides
        else
            d->refs[w++] = d->refs[r];
    }
    d->refs.resize(w);
    d->refs.shrink_to_fit();
}

// --- async worker ------------------------------------------------------------

struct Job {
    const redfs_depot* depot;
    uint64_t hash;
    uint32_t part;
    redfs_read_fn cb;
    void* user;
};

class Worker {
public:
    // The object is deliberately leaked -- never deleted -- because a static
    // destructor would run at DLL_PROCESS_DETACH under the loader lock. The
    // *thread*, however, must still be joined before this code is unmapped, or
    // FreeLibrary on a plugin that statically linked RedFS pulls the code out
    // from under a running thread. redfs_shutdown() does that join, and a host
    // that unloads plugins at runtime has to call it. Leaking the memory is
    // harmless; leaking the thread is not.
    static Worker& get() {
        static Worker* w = new Worker();
        return *w;
    }

    // Returns false when the job was refused because a shutdown is in progress;
    // the caller then reports it as cancelled itself. Never drops silently --
    // redfs.h promises every callback fires exactly once.
    bool post(const Job& job) {
        {
            std::lock_guard<std::mutex> lk(m_);
            if (state_ == State::kQuiescing) return false;
            queue_.push_back(job);
            ensure_thread();
        }
        cv_.notify_one();
        return true;
    }

    void drain() {
        std::unique_lock<std::mutex> lk(m_);
        done_.wait(lk, [&] { return queue_.empty() && !busy_; });
    }

    // True while this thread is executing a completion callback, so re-entrant
    // drain/shutdown can be refused instead of deadlocking on a flag only this
    // thread can clear.
    bool on_worker_thread() const {
        return std::this_thread::get_id() == worker_id_.load(std::memory_order_relaxed);
    }

    // Stops the thread and joins it. Safe to call more than once. Must NOT be
    // called from DllMain.
    //
    // Queued-but-unstarted work is CANCELLED rather than completed, so the wait
    // is bounded by the single read already in flight instead of by however much
    // the caller queued. Draining a deep queue at game close would look like a
    // hang; a caller who genuinely wants completion calls redfs_drain() first.
    //
    // There is deliberately no timeout. Abandoning the thread is not an option:
    // if the DLL is then unmapped, the thread wakes into freed code, which is
    // the exact crash this call exists to prevent. So the bound comes from doing
    // less work, never from giving up on the join.
    void stop() {
        // Re-entrancy from a completion callback would wait on !busy_ -- a flag
        // only this very thread can clear -- and then join itself. Refuse.
        if (on_worker_thread()) {
            log("redfs_shutdown called from a completion callback; ignored");
            return;
        }

        std::thread victim;
        std::deque<Job> cancelled;
        // Set before taking the lock: the worker is very likely mid-read and
        // holding no lock, and this is what makes it give up between segments.
        abort_.store(true, std::memory_order_relaxed);
        {
            std::unique_lock<std::mutex> lk(m_);
            state_ = State::kQuiescing;
            cancelled.swap(queue_);  // drop pending work
            cv_.notify_all();
            // Emptying the queue can satisfy a waiting drain()'s predicate, and
            // done_ is otherwise only signalled on job completion -- so without
            // this a drainer sleeps forever on a condition that is already true.
            done_.notify_all();
            // Only the in-flight job is waited on, and it is now aborting at the
            // next segment boundary. Bounded by one segment decode, not by the
            // queue and not by the size of whatever was being read.
            done_.wait(lk, [&] { return !busy_; });
            victim = std::move(thread_);
        }
        cv_.notify_all();
        if (victim.joinable()) victim.join();

        // Cancellations fire while still QUIESCING, on purpose.
        //
        // Re-opening first and then reporting would let a callback that re-posts
        // slip a job through and spawn a fresh thread *after* the join -- so
        // shutdown would return with a live worker, the one thing it exists to
        // prevent. Reporting first and re-opening after means such a re-post is
        // refused and reported as REDFS_E_CANCELLED instead: no lost callback,
        // and no thread outliving the call.
        for (const Job& job : cancelled)
            if (job.cb) job.cb(REDFS_E_CANCELLED, redfs_blob{}, job.user);

        // Quiesced, not disabled. A later post() starts a fresh thread. This
        // matters when RedFS.dll is shared between plugins: the first one to
        // unload must not permanently break async for the others.
        {
            std::lock_guard<std::mutex> lk(m_);
            state_ = State::kRunning;
        }
        abort_.store(false, std::memory_order_relaxed);
        done_.notify_all();
    }

    // Drop everything queued against `depot` and wait out an in-flight read on
    // it. Called by redfs_depot_close before the delete.
    //
    // A Job holds a raw depot pointer and the worker dereferences it, so deleting
    // a depot that queued work still names is a use-after-free -- and the C++
    // facade makes that the DEFAULT order, because ~Depot calls
    // redfs_depot_close. Documenting "drain first" would not have been enough.
    //
    // Like stop(), the wait is bounded by one segment decode rather than by the
    // queue: cancelled jobs are reported, not completed.
    void cancel_for(const redfs_depot* depot) {
        // A callback closing its own depot: the job it belongs to is this one, and
        // waiting for !busy_ would be waiting on ourselves. Nothing else can be
        // in flight, since there is a single worker.
        if (on_worker_thread()) return;

        std::deque<Job> cancelled;
        {
            std::unique_lock<std::mutex> lk(m_);
            for (auto it = queue_.begin(); it != queue_.end();) {
                if (it->depot == depot) {
                    cancelled.push_back(*it);
                    it = queue_.erase(it);
                } else {
                    ++it;
                }
            }
            if (busy_ && busy_depot_ == depot) {
                abort_.store(true, std::memory_order_relaxed);
                cv_.notify_all();
                done_.wait(lk, [&] { return !busy_ || busy_depot_ != depot; });
                // Cleared while still holding the lock. The worker needs this same
                // lock to dequeue its next job, so it cannot pick one up and see a
                // stale abort -- which would cancel a job nobody asked to cancel.
                abort_.store(false, std::memory_order_relaxed);
            }
            // A drain() may be waiting on a predicate the erase above just made
            // true, and done_ is otherwise only signalled on job completion.
            done_.notify_all();
        }

        for (const Job& job : cancelled)
            if (job.cb) job.cb(REDFS_E_CANCELLED, redfs_blob{}, job.user);
    }

private:
    void ensure_thread() {
        if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
    }

    void run() {
        worker_id_.store(std::this_thread::get_id(), std::memory_order_relaxed);
        for (;;) {
            Job job{};
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [&] { return state_ == State::kQuiescing || !queue_.empty(); });
                if (state_ == State::kQuiescing && queue_.empty()) {
                    worker_id_.store(std::thread::id{}, std::memory_order_relaxed);
                    return;
                }
                job = queue_.front();
                queue_.pop_front();
                busy_ = true;
                busy_depot_ = job.depot;  // so cancel_for can wait on the right job
            }

            redfs_blob blob{};
            uint64_t size = 0;
            redfs_status st = read_part(job.depot, job.hash, job.part, nullptr, 0, &size);
            if (st == REDFS_OK) st = blob_alloc(size, &blob);
            if (st == REDFS_OK) {
                uint64_t got = 0;
                st = read_part(job.depot, job.hash, job.part, blob.data, size, &got, &abort_);
            }
            if (st != REDFS_OK && blob.reserved) {
                std::free(blob.reserved);
                blob = redfs_blob{};
            }
            if (job.cb) job.cb(st, blob, job.user);

            {
                std::lock_guard<std::mutex> lk(m_);
                busy_ = false;
                busy_depot_ = nullptr;
            }
            done_.notify_all();
        }
    }

    // Two states, not a bool: "quiescing" has to be observable by post() for the
    // whole of stop(), including while the cancellation callbacks run. A plain
    // stop_ flag cleared before those callbacks lets one of them queue work that
    // spawns a thread after the join.
    enum class State { kRunning, kQuiescing };

    std::mutex m_;
    std::condition_variable cv_, done_;
    std::deque<Job> queue_;
    std::thread thread_;
    bool busy_ = false;
    const redfs_depot* busy_depot_ = nullptr;
    State state_ = State::kRunning;
    // Read without the lock by an in-flight read, hence atomic.
    std::atomic<bool> abort_{false};
    // Identifies the worker so re-entrant drain/shutdown can be refused.
    std::atomic<std::thread::id> worker_id_{};
};

}  // namespace

// Shared by redfs_stat and redfs_enumerate.
void fill_info(const Located& loc, redfs_file_info* out) {
    const Archive* a = loc.archive;
    const uint32_t first = a->entry_seg_start(loc.entry);
    // Clamp once, up front, rather than bounding the loop variable. The entry's
    // segment range is raw u32 out of the index and nothing in Archive::open
    // constrains it: an archive claiming `last = 0xFFFFFFFF` on every entry made
    // this loop run segment_count times PER ENTRY, so one redfs_enumerate over a
    // crafted index is entry_count x segment_count iterations -- on the order of
    // an hour for a 100 MB index, with no allocation and no fault for a watchdog
    // to notice.
    //
    // resolve_part rejects the same malformed range outright, but that is not an
    // option here: this function returns void and redfs_stat has no way to report
    // a per-entry rejection to a caller that only wanted the hash.
    uint32_t last = a->entry_seg_end(loc.entry);
    if (last > a->segment_count()) last = a->segment_count();
    if (last < first) last = first;

    out->hash = a->entry_hash(loc.entry);
    out->timestamp = a->entry_timestamp(loc.entry);
    out->archive_index = loc.archive_index;
    // Same clamp feeds this: an unvalidated `last` reported ~4.29e9 buffers, and
    // redfs.h documents buffer_count as the bound for addressing them -- our own
    // CLI loops on it.
    out->buffer_count = last > first ? last - first - 1 : 0;
    out->size = 0;
    out->compressed_size = 0;
    for (uint32_t i = first; i < last; ++i) {
        const Segment s = a->segment(i);
        out->size += s.size;
        out->compressed_size += s.zsize;
    }
    std::memcpy(out->sha1, a->entry_sha1(loc.entry), 20);
}

}  // namespace redfs

// =============================================================================
// C ABI
// =============================================================================

using namespace redfs;

// Exception barrier for the C ABI.
//
// Callers are C, Lua or another language's FFI; letting a C++ exception unwind
// into their frames is undefined behaviour, and terminate at best. The status
// enum already says allocation failure is RETURNED (REDFS_E_OOM), so the
// contract is clear -- it just was not enforced anywhere except blob_alloc.
//
// This is not hypothetical: several buffers are sized directly from a uint32
// segment size out of the archive, so a corrupt or crafted file can ask for
// gigabytes and throw std::bad_alloc straight out of a typed helper.
#define REDFS_GUARD(expr)                                                      \
    try {                                                                      \
        return (expr);                                                         \
    } catch (const std::bad_alloc&) {                                           \
        return fail(REDFS_E_OOM, "allocation failed");                         \
    } catch (const std::exception& e) {                                        \
        return fail(REDFS_E_IO, "internal error: %s", e.what());               \
    } catch (...) {                                                            \
        return fail(REDFS_E_IO, "internal error");                             \
    }

// Same, for entry points that return void and so cannot report.
#define REDFS_GUARD_VOID(expr)                                                 \
    try {                                                                      \
        expr;                                                                  \
    } catch (...) {                                                            \
    }

extern "C" {

uint32_t redfs_abi_version(void) { return REDFS_ABI_VERSION; }

const char* redfs_status_string(redfs_status status) {
    switch (status) {
        case REDFS_OK: return "ok";
        case REDFS_E_NOT_FOUND: return "not found";
        case REDFS_E_IO: return "i/o error";
        case REDFS_E_CORRUPT: return "corrupt data";
        case REDFS_E_OODLE: return "oodle unavailable or decode failed";
        case REDFS_E_INVALID_ARG: return "invalid argument";
        case REDFS_E_OOM: return "out of memory";
        case REDFS_E_UNSUPPORTED: return "unsupported";
        case REDFS_E_RANGE: return "out of range";
        case REDFS_E_CANCELLED: return "cancelled by shutdown";
    }
    return "unknown";
}

void redfs_set_log(redfs_log_fn fn, void* user) {
    // Single atomic store: the pair can never be observed half-updated.
    g_log.store(LogSink{fn, user}, std::memory_order_relaxed);
}

const char* redfs_last_error(void) { return t_last_error; }

int redfs_oodle_available(void) { return oodle::available() ? 1 : 0; }

// --- depot -------------------------------------------------------------------

redfs_status redfs_depot_open(const char* game_dir, uint32_t flags, redfs_depot** out_depot) {
    if (!out_depot) return REDFS_E_INVALID_ARG;
    *out_depot = nullptr;

    std::string root = game_dir ? game_dir : detect_game_dir();
    if (root.empty()) return fail(REDFS_E_NOT_FOUND, "could not locate the Cyberpunk 2077 install");
    if (!dir_exists(join(root, "archive\\pc")))
        return fail(REDFS_E_NOT_FOUND, "%s does not look like a Cyberpunk 2077 install", root.c_str());

    // Kraken decoding needs the game's own Oodle. Not fatal here: uncompressed
    // segments still work, and the error surfaces on the first compressed read.
    if (!oodle::load(root.c_str()))
        log("oo2ext_7_win64.dll not found -- compressed reads will fail");

    // Order is the override order: later mounts win, so this must match the
    // game's own sequence -- base, expansion, REDmod, then legacy mods on top.
    std::vector<std::string> paths;
    if (flags & REDFS_SCAN_CONTENT) list_archives(join(root, "archive\\pc\\content"), &paths);
    if (flags & REDFS_SCAN_EP1) list_archives(join(root, "archive\\pc\\ep1"), &paths);
    if (flags & REDFS_SCAN_REDMOD) list_redmod_archives(join(root, "mods"), &paths);
    if (flags & REDFS_SCAN_MODS) list_archives(join(root, "archive\\pc\\mod"), &paths);
    if (paths.empty()) return fail(REDFS_E_NOT_FOUND, "no .archive files under %s", root.c_str());

    auto* d = new redfs_depot();
    for (const auto& p : paths) {
        auto* a = new Archive();
        const redfs_status st = a->open(p);
        if (st != REDFS_OK) {
            log("skipping %s: %s", p.c_str(), redfs_last_error());
            delete a;
            continue;
        }
        d->archives.push_back(a);
    }
    if (d->archives.empty()) {
        delete d;
        return fail(REDFS_E_IO, "none of the %zu archives could be opened", paths.size());
    }

    reindex(d);
    *out_depot = d;
    return REDFS_OK;
}

redfs_status redfs_depot_open_empty(redfs_depot** out_depot) {
    if (!out_depot) return REDFS_E_INVALID_ARG;
    *out_depot = new redfs_depot();
    // Oodle is resolved lazily here: with no game directory to search, we can
    // only pick up an already-loaded copy. Uncompressed archives still work, and
    // a compressed read reports REDFS_E_OODLE rather than failing the mount.
    oodle::load(nullptr);
    return REDFS_OK;
}

redfs_status redfs_depot_mount(redfs_depot* depot, const char* archive_path) {
    if (!depot || !archive_path) return REDFS_E_INVALID_ARG;
    auto* a = new Archive();
    const redfs_status st = a->open(archive_path);
    if (st != REDFS_OK) {
        delete a;
        return st;
    }
    depot->archives.push_back(a);
    reindex(depot);
    // The archive set just changed, so any cached geometry may now be overridden.
    // Without this the cache serves pre-mount bounds and, worse, flushes new
    // entries under the stale fingerprint -- poisoning the file for later runs.
    cache_invalidate(depot);
    return REDFS_OK;
}

redfs_status redfs_depot_mount_dir(redfs_depot* depot, const char* dir, uint32_t* out_mounted) {
    if (!depot || !dir) return REDFS_E_INVALID_ARG;
    if (out_mounted) *out_mounted = 0;

    std::vector<std::string> paths;
    list_archives(dir, &paths);
    if (paths.empty()) return fail(REDFS_E_NOT_FOUND, "no .archive files in %s", dir);

    uint32_t added = 0;
    for (const auto& p : paths) {
        auto* a = new Archive();
        if (a->open(p) != REDFS_OK) {
            log("skipping %s: %s", p.c_str(), redfs_last_error());
            delete a;
            continue;
        }
        depot->archives.push_back(a);
        ++added;
    }
    if (added == 0) return fail(REDFS_E_IO, "none of the archives in %s could be opened", dir);

    reindex(depot);
    cache_invalidate(depot);  // same reasoning as redfs_depot_mount
    if (out_mounted) *out_mounted = added;
    return REDFS_OK;
}

void redfs_depot_close(redfs_depot* depot) {
    if (!depot) return;
    // Queued async reads hold a raw pointer to this depot, so they have to be
    // resolved before the memory goes away. Their callbacks fire with
    // REDFS_E_CANCELLED, which keeps the exactly-once promise in redfs.h.
    Worker::get().cancel_for(depot);
    delete depot;
}

uint32_t redfs_depot_archive_count(const redfs_depot* depot) {
    return depot ? static_cast<uint32_t>(depot->archives.size()) : 0;
}

const char* redfs_depot_archive_path(const redfs_depot* depot, uint32_t index) {
    if (!depot || index >= depot->archives.size()) return "";
    return depot->archives[index]->path().c_str();
}

uint64_t redfs_depot_file_count(const redfs_depot* depot) {
    return depot ? depot->refs.size() : 0;
}

uint64_t redfs_depot_index_bytes(const redfs_depot* depot) {
    if (!depot) return 0;
    uint64_t n = depot->refs.capacity() * sizeof(redfs_depot::Ref);
    for (const auto* a : depot->archives) n += a->index_bytes();
    return n;
}

// --- paths -------------------------------------------------------------------

uint64_t redfs_hash_n(const char* depot_path, size_t length) {
    if (!depot_path) return 0;
    const std::string s = sanitize_path(depot_path, length);
    return s.empty() ? 0 : fnv1a64(s.data(), s.size());
}

uint64_t redfs_hash(const char* depot_path) {
    return depot_path ? redfs_hash_n(depot_path, std::strlen(depot_path)) : 0;
}

size_t redfs_hash_string(const char* depot_path, char* out, size_t capacity) {
    if (!out || capacity == 0) return 0;
    const int n = std::snprintf(out, capacity, "%llu",
                                static_cast<unsigned long long>(redfs_hash(depot_path)));
    if (n < 0 || static_cast<size_t>(n) >= capacity) return 0;
    return static_cast<size_t>(n);
}

uint64_t redfs_hash_parse(const char* decimal) {
    if (!decimal) return 0;
    return std::strtoull(decimal, nullptr, 10);
}

// --- hash -> path ------------------------------------------------------------

// All of these allocate -- the list file is read whole, the decompressed size
// comes from the file itself, and every intern grows the dictionary -- so they
// need the same barrier as the rest of the ABI. They were the last unguarded
// allocating exports.
redfs_status redfs_path_load(const redfs_depot* depot, const char* list_file, uint32_t* out_kept) {
    // A null depot used to mean "keep every line", undocumented and reachable
    // only by accident. Filtering is the whole point of this call.
    if (!depot || !list_file) return REDFS_E_INVALID_ARG;
    REDFS_GUARD(paths_load(depot, list_file, out_kept));
}

void redfs_path_enable(void) { REDFS_GUARD_VOID(paths_enable()); }

void redfs_path_add(const char* depot_path) { REDFS_GUARD_VOID(paths_add(depot_path)); }

uint32_t redfs_path_count(void) {
    // REDFS_GUARD returns a redfs_status and does not fit a uint32_t entry
    // point; same shape as redfs_cache_entry_count.
    try {
        return paths_count();
    } catch (...) {
        return 0;
    }
}

const char* redfs_path_from_hash(uint64_t hash) {
    try {
        return path_from_hash(hash);
    } catch (...) {
        return nullptr;
    }
}

// --- lookup ------------------------------------------------------------------

int redfs_exists(const redfs_depot* depot, uint64_t hash) {
    Located loc{};
    return depot && depot->locate(hash, &loc) ? 1 : 0;
}

redfs_status redfs_stat(const redfs_depot* depot, uint64_t hash, redfs_file_info* out_info) {
    if (!depot || !out_info) return REDFS_E_INVALID_ARG;
    Located loc{};
    if (!depot->locate(hash, &loc)) return REDFS_E_NOT_FOUND;
    fill_info(loc, out_info);
    return REDFS_OK;
}

redfs_status redfs_enumerate(const redfs_depot* depot, redfs_enum_fn fn, void* user) {
    if (!depot || !fn) return REDFS_E_INVALID_ARG;
    for (const auto& ref : depot->refs) {
        Located loc{depot->archives[ref.archive], ref.entry, ref.archive};
        redfs_file_info info{};
        fill_info(loc, &info);
        if (!fn(&info, user)) break;
    }
    return REDFS_OK;
}

// --- reading -----------------------------------------------------------------

redfs_status redfs_part_size(const redfs_depot* depot, uint64_t hash, uint32_t part,
                             uint64_t* out_size) {
    if (!depot || !out_size) return REDFS_E_INVALID_ARG;
    return read_part(depot, hash, part, nullptr, 0, out_size);
}

redfs_status redfs_read_into(const redfs_depot* depot, uint64_t hash, uint32_t part, void* dst,
                             uint64_t capacity, uint64_t* out_written) {
    if (!depot || !dst) return REDFS_E_INVALID_ARG;
    return read_part(depot, hash, part, static_cast<uint8_t*>(dst), capacity, out_written);
}

redfs_status redfs_read(const redfs_depot* depot, uint64_t hash, uint32_t part,
                        redfs_blob* out_blob) {
    if (!depot || !out_blob) return REDFS_E_INVALID_ARG;
    *out_blob = redfs_blob{};

    uint64_t size = 0;
    redfs_status st = read_part(depot, hash, part, nullptr, 0, &size);
    if (st != REDFS_OK) return st;

    st = blob_alloc(size, out_blob);
    if (st != REDFS_OK) return st;

    uint64_t written = 0;
    st = read_part(depot, hash, part, out_blob->data, size, &written);
    if (st != REDFS_OK) {
        redfs_blob_free(out_blob);
        return st;
    }
    return REDFS_OK;
}

void redfs_blob_free(redfs_blob* blob) {
    if (!blob) return;
    if (blob->reserved) std::free(blob->reserved);
    *blob = redfs_blob{};
}

redfs_status redfs_read_async(const redfs_depot* depot, uint64_t hash, uint32_t part,
                              redfs_read_fn cb, void* user) {
    if (!depot || !cb) return REDFS_E_INVALID_ARG;
    // Refused while shutting down. Reported through the RETURN VALUE, never by
    // invoking the callback here: the callback is exactly the place that tends
    // to queue the next read, so calling it synchronously on refusal recurses
    // until the stack dies. The contract is therefore the conventional one --
    // REDFS_OK means the callback will fire exactly once, any error means it
    // will not fire at all and the caller handles it inline.
    if (!Worker::get().post(Job{depot, hash, part, cb, user})) return REDFS_E_CANCELLED;
    return REDFS_OK;
}

void redfs_drain(void) {
    // From a completion callback this would wait on a flag only the calling
    // thread can clear. Refuse rather than deadlock.
    if (Worker::get().on_worker_thread()) {
        log("redfs_drain called from a completion callback; ignored");
        return;
    }
    Worker::get().drain();
}

void redfs_shutdown(void) {
    // Order matters: stop the worker before touching the cache, so no in-flight
    // read is still using a depot the caller is about to close.
    Worker::get().stop();
    // cache_close flushes, which serializes every cached mesh into one buffer.
    // This is the call every host is required to make, so an allocation failure
    // here would take the game down on the way out.
    REDFS_GUARD_VOID(cache_close());
}

// --- CR2W --------------------------------------------------------------------

redfs_status redfs_cr2w_open(const void* data, uint64_t size, redfs_cr2w** out_cr2w) {
    if (!data || !out_cr2w) return REDFS_E_INVALID_ARG;
    auto* f = new redfs_cr2w();
    const redfs_status st = cr2w_parse(data, size, f);
    if (st != REDFS_OK) {
        delete f;
        *out_cr2w = nullptr;
        return st;
    }
    *out_cr2w = f;
    return REDFS_OK;
}

void redfs_cr2w_close(redfs_cr2w* cr2w) { delete cr2w; }

const char* redfs_cr2w_root_type(const redfs_cr2w* cr2w) {
    if (!cr2w || cr2w->chunks.empty()) return "";
    return cr2w->name(cr2w->chunks[0].class_name);
}

uint32_t redfs_cr2w_chunk_count(const redfs_cr2w* cr2w) {
    return cr2w ? static_cast<uint32_t>(cr2w->chunks.size()) : 0;
}

const char* redfs_cr2w_chunk_type(const redfs_cr2w* cr2w, uint32_t chunk) {
    if (!cr2w || chunk >= cr2w->chunks.size()) return "";
    return cr2w->name(cr2w->chunks[chunk].class_name);
}

int32_t redfs_cr2w_find_chunk(const redfs_cr2w* cr2w, const char* type_name) {
    if (!cr2w || !type_name) return -1;
    for (uint32_t i = 0; i < cr2w->chunks.size(); ++i)
        if (std::strcmp(cr2w->name(cr2w->chunks[i].class_name), type_name) == 0)
            return static_cast<int32_t>(i);
    return -1;
}

uint32_t redfs_cr2w_import_count(const redfs_cr2w* cr2w) {
    return cr2w ? static_cast<uint32_t>(cr2w->imports.size()) : 0;
}

const char* redfs_cr2w_import_path(const redfs_cr2w* cr2w, uint32_t index) {
    if (!cr2w || index >= cr2w->imports.size()) return "";
    return cr2w->str(cr2w->imports[index].str_offset);
}

const char* redfs_cr2w_import_type(const redfs_cr2w* cr2w, uint32_t index) {
    if (!cr2w || index >= cr2w->imports.size()) return "";
    return cr2w->name(cr2w->imports[index].class_name);
}

redfs_status redfs_cr2w_get(const redfs_cr2w* cr2w, uint32_t chunk, const char* prop_path,
                            redfs_value* out_value) {
    if (!cr2w || !out_value) return REDFS_E_INVALID_ARG;
    *out_value = redfs_value{};
    return cr2w_find(cr2w, chunk, prop_path, out_value);
}

redfs_status redfs_cr2w_walk(const redfs_cr2w* cr2w, uint32_t chunk, const char* prop_path,
                             redfs_prop_fn fn, void* user) {
    if (!cr2w) return REDFS_E_INVALID_ARG;
    return cr2w_walk(cr2w, chunk, prop_path, fn, user);
}

redfs_status redfs_cr2w_walk_array(const redfs_cr2w* cr2w, const redfs_value* array,
                                   redfs_elem_fn fn, void* user) {
    if (!cr2w) return REDFS_E_INVALID_ARG;
    return cr2w_walk_array(cr2w, array, fn, user);
}

redfs_status redfs_cr2w_get_in(const redfs_cr2w* cr2w, const redfs_value* parent,
                               const char* prop_path, redfs_value* out_value) {
    if (!cr2w || !out_value) return REDFS_E_INVALID_ARG;
    *out_value = redfs_value{};
    return cr2w_get_in(cr2w, parent, prop_path, out_value);
}

redfs_status redfs_cr2w_walk_in(const redfs_cr2w* cr2w, const redfs_value* parent,
                                const char* prop_path, redfs_prop_fn fn, void* user) {
    if (!cr2w) return REDFS_E_INVALID_ARG;
    return cr2w_walk_in(cr2w, parent, prop_path, fn, user);
}

// --- formats -----------------------------------------------------------------

redfs_status redfs_texture_desc_of(const redfs_depot* depot, uint64_t hash,
                                   redfs_texture_desc* out_desc) {
    if (!depot || !out_desc) return REDFS_E_INVALID_ARG;
    REDFS_GUARD(texture_desc_of(depot, hash, out_desc));
}

redfs_status redfs_texture_read_dds(const redfs_depot* depot, uint64_t hash, redfs_blob* out_blob) {
    if (!depot || !out_blob) return REDFS_E_INVALID_ARG;
    *out_blob = redfs_blob{};
    redfs_status st;
    try {
        st = texture_read_dds(depot, hash, out_blob);
    } catch (const std::bad_alloc&) {
        st = fail(REDFS_E_OOM, "allocation failed");
    } catch (...) {
        st = fail(REDFS_E_IO, "internal error");
    }
    if (st != REDFS_OK) redfs_blob_free(out_blob);
    return st;
}

redfs_status redfs_texture_read_raw(const redfs_depot* depot, uint64_t hash,
                                    redfs_texture_desc* out_desc, redfs_blob* out_blob) {
    if (!depot || !out_blob) return REDFS_E_INVALID_ARG;
    *out_blob = redfs_blob{};
    redfs_status st;
    try {
        st = texture_read_raw(depot, hash, out_desc, out_blob);
    } catch (const std::bad_alloc&) {
        st = fail(REDFS_E_OOM, "allocation failed");
    } catch (...) {
        st = fail(REDFS_E_IO, "internal error");
    }
    if (st != REDFS_OK) redfs_blob_free(out_blob);
    return st;
}

redfs_status redfs_audio_probe(const redfs_depot* depot, uint64_t hash,
                               redfs_audio_format* out_format) {
    if (!depot || !out_format) return REDFS_E_INVALID_ARG;
    REDFS_GUARD(audio_probe(depot, hash, out_format));
}

redfs_status redfs_audio_info_of(const redfs_depot* depot, uint64_t hash,
                                 redfs_audio_info* out_info) {
    if (!depot || !out_info) return REDFS_E_INVALID_ARG;
    REDFS_GUARD(audio_info_of(depot, hash, out_info));
}

redfs_status redfs_audio_info_parse(const void* data, uint64_t size, redfs_audio_info* out_info) {
    if (!data || !out_info) return REDFS_E_INVALID_ARG;
    return audio_info_parse(data, size, out_info);
}

redfs_status redfs_audio_walk_chunks(const void* data, uint64_t size, redfs_riff_chunk_fn fn,
                                     void* user) {
    if (!data) return REDFS_E_INVALID_ARG;
    return audio_walk_chunks(data, size, fn, user);
}

const char* redfs_audio_codec_name(redfs_audio_codec codec) {
    switch (codec) {
        case REDFS_CODEC_PCM: return "PCM";
        case REDFS_CODEC_ADPCM: return "ADPCM";
        case REDFS_CODEC_VORBIS: return "Wwise Vorbis";
        case REDFS_CODEC_XMA2: return "XMA2";
        case REDFS_CODEC_OPUS: return "Opus";
        case REDFS_CODEC_UNKNOWN: break;
    }
    return "unknown";
}

redfs_status redfs_mesh_desc_of(const redfs_depot* depot, uint64_t hash,
                                redfs_mesh_desc* out_desc) {
    if (!depot || !out_desc) return REDFS_E_INVALID_ARG;
    REDFS_GUARD(mesh_desc_of(depot, hash, out_desc));
}

// --- mesh chunks -------------------------------------------------------------

redfs_status redfs_mesh_open(const redfs_depot* depot, uint64_t hash, redfs_mesh** out_mesh) {
    if (!depot || !out_mesh) return REDFS_E_INVALID_ARG;
    *out_mesh = nullptr;

    std::shared_ptr<const Mesh> data;
    redfs_status st;
    // mesh_acquire sizes buffers from archive-supplied lengths, so a corrupt or
    // crafted file can throw bad_alloc here. That must not unwind into a C host.
    try {
        st = mesh_acquire(depot, hash, &data);
    } catch (const std::bad_alloc&) {
        return fail(REDFS_E_OOM, "allocation failed decoding mesh");
    } catch (...) {
        return fail(REDFS_E_IO, "internal error decoding mesh");
    }
    if (st != REDFS_OK) return st;

    // The handle owns a reference; the mesh itself is never touched here. It was
    // finalised before publication precisely so this function has nothing to
    // mutate -- writing through a pointer the cache shares would race with every
    // other holder, and would reallocate a vector that redfs_mesh_chunk_at hands
    // out interior pointers into.
    *out_mesh = new redfs_mesh{std::move(data)};
    return REDFS_OK;
}

void redfs_mesh_close(redfs_mesh* mesh) {
    // Always drops exactly this handle's reference. The data survives if the
    // cache -- or another open handle -- still refers to it, so closing is safe
    // no matter what the cache is doing.
    delete mesh;
}

uint32_t redfs_mesh_chunk_count(const redfs_mesh* mesh) {
    // public_chunks, not chunks: this must answer from the same vector
    // redfs_mesh_chunk_at hands out, or the two exports disagree about the same
    // number and redfs.hpp's chunks() span runs past the array.
    return mesh && mesh->data ? static_cast<uint32_t>(mesh->data->public_chunks.size()) : 0;
}

const redfs_mesh_chunk* redfs_mesh_chunk_at(const redfs_mesh* mesh, uint32_t index) {
    if (!mesh || !mesh->data || index >= mesh->data->public_chunks.size()) return nullptr;
    // Safe to hand out: public_chunks is immutable for as long as this handle
    // holds its reference.
    return &mesh->data->public_chunks[index];
}

uint32_t redfs_mesh_lod_count(const redfs_mesh* mesh) {
    return mesh && mesh->data ? mesh->data->lod_count : 0;
}

void redfs_mesh_bounds(const redfs_mesh* mesh, float out_min[3], float out_max[3]) {
    if (!mesh || !mesh->data) return;
    for (int i = 0; i < 3; ++i) {
        if (out_min) out_min[i] = mesh->data->bbox_min[i];
        if (out_max) out_max[i] = mesh->data->bbox_max[i];
    }
}

uint32_t redfs_mesh_appearance_count(const redfs_mesh* mesh) {
    return mesh && mesh->data ? static_cast<uint32_t>(mesh->data->appearances.size()) : 0;
}

const char* redfs_mesh_appearance_name(const redfs_mesh* mesh, uint32_t appearance) {
    if (!mesh || !mesh->data || appearance >= mesh->data->appearances.size()) return "";
    return mesh->data->appearances[appearance].name.c_str();
}

int32_t redfs_mesh_find_appearance(const redfs_mesh* mesh, const char* name) {
    if (!mesh || !mesh->data || !name) return -1;
    for (uint32_t i = 0; i < mesh->data->appearances.size(); ++i)
        if (mesh->data->appearances[i].name == name) return static_cast<int32_t>(i);
    return -1;
}

const char* redfs_mesh_chunk_material(const redfs_mesh* mesh, uint32_t appearance, uint32_t chunk) {
    if (!mesh || !mesh->data || appearance >= mesh->data->appearances.size()) return "";
    const auto& mats = mesh->data->appearances[appearance].chunk_materials;
    if (chunk >= mats.size()) return "";
    return mats[chunk].c_str();
}

// --- mesh cache --------------------------------------------------------------

// These allocate from data the caller does not control -- cache_open sizes
// vectors from the contents of a file on disk, and cache_flush builds the whole
// serialized image in memory -- so they are exactly the entry points that must
// not let an exception cross back into C.
redfs_status redfs_cache_open(const redfs_depot* depot, const char* cache_file) {
    REDFS_GUARD(cache_open(depot, cache_file));
}

redfs_status redfs_cache_flush(void) { REDFS_GUARD(cache_flush()); }

void redfs_cache_close(void) { REDFS_GUARD_VOID(cache_close()); }

uint32_t redfs_cache_entry_count(void) {
    // REDFS_GUARD returns a redfs_status and so does not fit here. Worth its own
    // barrier anyway: this is the natural call for a log sink to make while
    // annotating a message, and re-entering the cache from the sink throws
    // rather than deadlocks on the MSVC STL.
    try {
        return cache_entry_count();
    } catch (...) {
        return 0;
    }
}

redfs_status redfs_cache_warm(const redfs_depot* depot, const uint64_t* hashes, uint32_t count,
                              uint32_t* out_computed) {
    if (!depot || (!hashes && count)) return REDFS_E_INVALID_ARG;
    // Without an open cache this does every geometry decompress and then throws
    // all of it away -- mesh_acquire never inserts, so the count is unchanged on
    // both sides of every iteration and the caller is told 0 after paying full
    // price. Fail loudly instead of wasting seconds silently.
    if (!cache_is_open(depot))
        return fail(REDFS_E_INVALID_ARG, "redfs_cache_warm requires a cache opened on this depot");
    uint32_t computed = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t before = cache_entry_count();
        redfs_mesh* m = nullptr;
        if (redfs_mesh_open(depot, hashes[i], &m) != REDFS_OK) continue;
        if (cache_entry_count() > before) ++computed;
        redfs_mesh_close(m);
    }
    if (out_computed) *out_computed = computed;
    REDFS_GUARD(cache_flush());
}

}  // extern "C"
