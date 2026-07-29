// A minimal test framework. No dependency, because pulling in gtest for a
// library that has none would be the wrong trade.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace test {

struct Case {
    const char* group;
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

struct Registrar {
    Registrar(const char* group, const char* name, void (*fn)()) {
        registry().push_back(Case{group, name, fn});
    }
};

// Per-case state, reset by the runner.
inline int& failures() {
    static int n = 0;
    return n;
}
inline int& checks() {
    static int n = 0;
    return n;
}

/*
 * Copies a string argument so CHECK_STR can compare it safely.
 *
 * This has to be a function call, not a separate assignment statement, and that
 * distinction is the whole point. Given `CHECK_STR(f().c_str(), "x")`, the
 * temporary std::string returned by f() lives until the end of the
 * full-expression it appears in. Storing the raw pointer first
 * (`const char* p = f().c_str();`) ends that expression at the semicolon, so p
 * dangles before the comparison. Short strings live in the std::string's SSO
 * buffer on a dead stack frame, so the read comes back empty rather than
 * crashing -- a silent wrong answer that also hides from ASan.
 *
 * Passing through here keeps the copy inside the same full-expression.
 */
inline std::string own(const char* s) { return s ? std::string(s) : std::string(); }
inline std::string own(const std::string& s) { return s; }

inline void report(const char* file, int line, const char* expr, const std::string& detail) {
    ++failures();
    std::printf("    FAIL %s:%d\n         %s\n", file, line, expr);
    if (!detail.empty()) std::printf("         %s\n", detail.c_str());
}

}  // namespace test

#define TEST(group, name)                                                     \
    static void test_##group##_##name();                                      \
    static ::test::Registrar reg_##group##_##name(#group, #name,              \
                                                  test_##group##_##name);     \
    static void test_##group##_##name()

#define CHECK(expr)                                                           \
    do {                                                                      \
        ++::test::checks();                                                    \
        if (!(expr)) ::test::report(__FILE__, __LINE__, #expr, {});           \
    } while (0)

#define CHECK_EQ(a, b)                                                        \
    do {                                                                      \
        ++::test::checks();                                                    \
        const auto va_ = (a);                                                 \
        const auto vb_ = (b);                                                 \
        if (!(va_ == vb_)) {                                                  \
            char buf_[256];                                                   \
            std::snprintf(buf_, sizeof buf_, "got %lld, want %lld",           \
                          (long long)va_, (long long)vb_);                    \
            ::test::report(__FILE__, __LINE__, #a " == " #b, buf_);           \
        }                                                                     \
    } while (0)

/*
 * Copies both sides into std::string before comparing.
 *
 * Storing the raw const char* instead would dangle whenever an argument is
 * `something().c_str()` on a temporary std::string: the temporary dies at the
 * end of the initialising statement, before the comparison runs. With short
 * strings that is an SSO buffer on a dead stack frame, so it reads as empty
 * rather than crashing -- a silent wrong answer, which is the worst kind. The
 * copy happens inside the same full-expression, so temporaries are still alive.
 */
#define CHECK_STR(a, b)                                                       \
    do {                                                                      \
        ++::test::checks();                                                    \
        const std::string va_ = ::test::own(a);                               \
        const std::string vb_ = ::test::own(b);                               \
        if (va_ != vb_) {                                                     \
            char buf_[512];                                                   \
            std::snprintf(buf_, sizeof buf_, "got \"%s\", want \"%s\"",       \
                          va_.c_str(), vb_.c_str());                          \
            ::test::report(__FILE__, __LINE__, #a " == " #b, buf_);           \
        }                                                                     \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                 \
    do {                                                                      \
        ++::test::checks();                                                    \
        const double va_ = (double)(a);                                       \
        const double vb_ = (double)(b);                                       \
        if (!(va_ - vb_ < (eps) && vb_ - va_ < (eps))) {                      \
            char buf_[256];                                                   \
            std::snprintf(buf_, sizeof buf_, "got %g, want %g (+-%g)", va_,   \
                          vb_, (double)(eps));                                \
            ::test::report(__FILE__, __LINE__, #a " ~= " #b, buf_);           \
        }                                                                     \
    } while (0)

#define CHECK_OK(expr)                                                        \
    do {                                                                      \
        ++::test::checks();                                                    \
        const redfs_status st_ = (expr);                                      \
        if (st_ != REDFS_OK) {                                                \
            char buf_[512];                                                   \
            std::snprintf(buf_, sizeof buf_, "%s -- %s",                      \
                          redfs_status_string(st_), redfs_last_error());      \
            ::test::report(__FILE__, __LINE__, #expr, buf_);                  \
        }                                                                     \
    } while (0)

#define CHECK_ERR(expr, want)                                                 \
    do {                                                                      \
        ++::test::checks();                                                    \
        const redfs_status st_ = (expr);                                      \
        if (st_ != (want)) {                                                  \
            char buf_[256];                                                   \
            std::snprintf(buf_, sizeof buf_, "got %s, want %s",               \
                          redfs_status_string(st_),                           \
                          redfs_status_string(want));                         \
            ::test::report(__FILE__, __LINE__, #expr, buf_);                  \
        }                                                                     \
    } while (0)
