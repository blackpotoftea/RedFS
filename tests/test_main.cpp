// Test runner, with leak detection.
//
// The leak check measures STEADY STATE, not whole-process allocation, and that
// distinction matters. RedFS deliberately leaks three singletons -- the async
// worker, the mesh cache and the path dictionary -- because destroying them from
// a static destructor would run under the loader lock at DLL_PROCESS_DETACH and
// deadlock a mod DLL (see docs/done/api-design.md). A whole-process check
// reports those every run, and a report full of expected noise is a report
// nobody reads.
//
// So the suite runs twice: the first pass populates every lazy singleton and
// cache, then a heap checkpoint is taken, then the second pass runs. Anything
// allocated during the second pass and not released is a genuine leak, because
// by then nothing is being populated for the first time. That is the signal
// worth failing on.
//
// Under ASan (-DREDFS_SANITIZE=address) the same tests additionally become a
// memory-safety sweep over every parser.

#include "framework.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#include "redfs.h"

#if defined(_DEBUG)
#  define _CRTDBG_MAP_ALLOC
#  include <crtdbg.h>
#  include <stdlib.h>
#endif

namespace {

void on_log(const char* msg, void*) {
    if (std::getenv("REDFS_VERBOSE")) std::fprintf(stderr, "    [redfs] %s\n", msg);
}

bool matches(const char* filter, const test::Case& c) {
    if (!filter || !*filter) return true;
    return std::strstr(c.group, filter) != nullptr || std::strstr(c.name, filter) != nullptr;
}

// Returns the number of cases that failed.
int run_all(const char* filter, bool report) {
    int failed_cases = 0;
    const char* last_group = nullptr;

    for (const auto& c : test::registry()) {
        if (!matches(filter, c)) continue;
        if (report && (!last_group || std::strcmp(last_group, c.group) != 0)) {
            std::printf("%s\n", c.group);
            last_group = c.group;
        }

        const int before_failures = test::failures();
        const int before_checks = test::checks();

        c.fn();

        const int case_failures = test::failures() - before_failures;
        const int case_checks = test::checks() - before_checks;
        if (case_failures) ++failed_cases;
        if (!report) continue;

        if (case_failures)
            std::printf("  %-34s FAILED (%d of %d checks)\n", c.name, case_failures, case_checks);
        else
            std::printf("  %-34s ok (%d checks)\n", c.name, case_checks);
    }
    return failed_cases;
}

}  // namespace

int main(int argc, char** argv) {
    const char* filter = argc >= 2 ? argv[1] : nullptr;
    redfs_set_log(on_log, nullptr);

    std::printf("RedFS tests");
#if defined(_DEBUG)
    std::printf("  [CRT leak check]");
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(REDFS_ASAN)
    std::printf("  [ASan]");
#endif
    std::printf("\n\n");

#if defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDOUT);

    // Two warm-up passes, then measure. One is not enough: some global state is
    // enabled by a test rather than at startup -- the path dictionary starts off
    // and only begins learning from CR2W imports once a later test switches it
    // on -- so pass 2 populates things pass 1 never reached. A second warm-up
    // settles that, and anything still growing on the measured pass is real.
    run_all(filter, /*report=*/false);
    run_all(filter, /*report=*/false);
    test::failures() = 0;
    test::checks() = 0;

    _CrtMemState before;
    _CrtMemCheckpoint(&before);
#endif

    const int failed_cases = run_all(filter, /*report=*/true);
    std::printf("\n%d checks, %d failures\n", test::checks(), test::failures());

#if defined(_DEBUG)
    _CrtMemState after, diff;
    _CrtMemCheckpoint(&after);
    if (_CrtMemDifference(&diff, &before, &after)) {
        std::printf("\nLEAK: the second pass allocated memory it did not release.\n");
        std::printf("Singletons were already populated by the warm-up, so this is real.\n\n");
        _CrtMemDumpStatistics(&diff);
        _CrtDumpMemoryLeaks();
        return 2;
    }
    std::printf("no leaks (steady state clean across two full passes)\n");
#endif

    return failed_cases ? 1 : 0;
}
