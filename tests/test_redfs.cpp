// RedFS unit tests. No game install required -- every input is synthesized by
// tests/fixtures.cpp, so these run anywhere and can cover malformed data that no
// real install would ever produce.
//
// Run under ASan (cmake -DREDFS_SANITIZE=address) to turn every parser test into
// a memory-safety test as well.

#include "framework.hpp"
#include "fixtures.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <functional>
#include <string>
#include <chrono>
#include <thread>
#include <vector>

#include <windows.h>

#include "redfs.h"
// Included so the C++ facade's templates are actually instantiated somewhere --
// they were not, which is how a compile error in for_each survived.
#include "redfs.hpp"

using fixture::ArchiveBuilder;
using fixture::Cr2wBuilder;

namespace {

std::string temp_path(const char* name) {
    char buf[512];
    const char* tmp = std::getenv("TEMP");
    std::snprintf(buf, sizeof buf, "%s\\redfs_test_%s", tmp ? tmp : ".", name);
    return buf;
}

// Opens a depot over a single synthetic archive written to disk.
struct TempDepot {
    std::string path;
    redfs_depot* depot = nullptr;

    TempDepot(const char* name, const std::vector<uint8_t>& archive_bytes) {
        path = temp_path(name);
        ArchiveBuilder::write(path, archive_bytes);
        // Mount by hand: depot_open expects a full install layout.
        redfs_depot_open_empty(&depot);
        if (depot) redfs_depot_mount(depot, path.c_str());
    }
    ~TempDepot() {
        if (depot) redfs_depot_close(depot);
        std::remove(path.c_str());
    }
};

}  // namespace

// redfs_find's empty-dictionary contract cannot be tested here: the dictionary
// is a never-destroyed global, and under _DEBUG this binary runs the whole suite
// three times. See lifecycle_test.cpp's virgin-dictionary scenario.

// =============================================================================
// hashing
// =============================================================================

TEST(hash, fnv_vectors) {
    // Canonical FNV-1a 64 vectors -- an external oracle, not our expectations.
    CHECK_EQ(redfs_hash("a"), 0xaf63dc4c8601ec8cull);
    CHECK_EQ(redfs_hash("foobar"), 0x85944171f73967e8ull);
    CHECK_EQ(redfs_hash("hello"), 0xa430d84680aabd0bull);
    CHECK_EQ(redfs_hash("127.0.0.1"), 0xaabafe7104d914beull);
    CHECK_EQ(redfs_hash("feedfacedeadbeef"), 0xcac54572bb1a6fc8ull);
}

TEST(hash, normalisation) {
    const uint64_t want = redfs_hash("base\\icon\\foo.xbm");
    CHECK_EQ(redfs_hash("Base/Icon/Foo.XBM"), want);       // case + separators
    CHECK_EQ(redfs_hash("base//icon\\\\foo.xbm"), want);   // repeated separators
    CHECK_EQ(redfs_hash("  base\\icon\\foo.xbm  "), want); // surrounding space
    CHECK_EQ(redfs_hash("\"base/icon/foo.xbm\""), want);   // quotes
    CHECK_EQ(redfs_hash("/base/icon/foo.xbm"), want);      // leading separator
}

TEST(hash, edge_cases) {
    CHECK_EQ(redfs_hash(nullptr), 0ull);
    CHECK_EQ(redfs_hash(""), 0ull);
    CHECK_EQ(redfs_hash("///"), 0ull);  // normalises to empty
}

TEST(hash, decimal_round_trip) {
    char buf[REDFS_HASH_STRING_MAX];
    const size_t n = redfs_hash_string("base\\icon\\foo.xbm", buf, sizeof buf);
    CHECK(n > 0 && n < REDFS_HASH_STRING_MAX);
    CHECK_EQ(redfs_hash_parse(buf), redfs_hash("base\\icon\\foo.xbm"));

    // A hash with the high bit set must survive the text round trip; this is the
    // case a double would silently mangle.
    char small[4];
    CHECK_EQ(redfs_hash_string("base\\icon\\foo.xbm", small, sizeof small), 0u);  // no room
}

// =============================================================================
// archive container
// =============================================================================

TEST(archive, roundtrip_single_file) {
    const std::vector<uint8_t> payload = {'h', 'e', 'l', 'l', 'o', ' ', 'r', 'e', 'd'};
    ArchiveBuilder ab;
    const uint64_t key = redfs_hash("base\\test\\file.bin");
    ab.add(key, payload);

    TempDepot d("archive_single.archive", ab.build());
    CHECK(d.depot != nullptr);
    if (!d.depot) return;

    CHECK_EQ(redfs_depot_file_count(d.depot), 1ull);
    CHECK_EQ(redfs_exists(d.depot, key), 1);
    CHECK_EQ(redfs_exists(d.depot, key ^ 1), 0);

    redfs_file_info info{};
    CHECK_OK(redfs_stat(d.depot, key, &info));
    CHECK_EQ(info.size, payload.size());
    CHECK_EQ(info.buffer_count, 0u);

    redfs_blob blob{};
    CHECK_OK(redfs_read(d.depot, key, REDFS_PART_ALL, &blob));
    CHECK_EQ(blob.size, payload.size());
    CHECK(blob.data && std::memcmp(blob.data, payload.data(), payload.size()) == 0);
    redfs_blob_free(&blob);
    CHECK(blob.data == nullptr);  // free must clear the struct
}

TEST(archive, buffers_are_separate_parts) {
    ArchiveBuilder ab;
    const uint64_t key = redfs_hash("base\\test\\multi.bin");
    ab.add(key, {'M', 'A', 'I', 'N'}, {{'B', '0'}, {'B', 'U', 'F', '1', '!'}});

    TempDepot d("archive_multi.archive", ab.build());
    if (!d.depot) return;

    redfs_file_info info{};
    CHECK_OK(redfs_stat(d.depot, key, &info));
    CHECK_EQ(info.buffer_count, 2u);
    CHECK_EQ(info.size, 4u + 2u + 5u);

    redfs_blob main{}, b0{}, b1{}, all{};
    CHECK_OK(redfs_read(d.depot, key, REDFS_PART_MAIN, &main));
    CHECK_EQ(main.size, 4u);
    CHECK_OK(redfs_read(d.depot, key, 0, &b0));
    CHECK_EQ(b0.size, 2u);
    CHECK_OK(redfs_read(d.depot, key, 1, &b1));
    CHECK_EQ(b1.size, 5u);
    CHECK_OK(redfs_read(d.depot, key, REDFS_PART_ALL, &all));
    CHECK_EQ(all.size, 11u);

    // PART_ALL must be the concatenation, in order.
    CHECK(std::memcmp(all.data, "MAINB0BUF1!", 11) == 0);

    // Out-of-range buffer index is an error, not a crash.
    redfs_blob oops{};
    CHECK_ERR(redfs_read(d.depot, key, 7, &oops), REDFS_E_RANGE);

    redfs_blob_free(&main);
    redfs_blob_free(&b0);
    redfs_blob_free(&b1);
    redfs_blob_free(&all);
}

TEST(archive, missing_file_reports_not_found) {
    ArchiveBuilder ab;
    ab.add(redfs_hash("base\\a.bin"), {'a'});
    TempDepot d("archive_missing.archive", ab.build());
    if (!d.depot) return;

    redfs_file_info info{};
    CHECK_ERR(redfs_stat(d.depot, redfs_hash("base\\nope.bin"), &info), REDFS_E_NOT_FOUND);
    redfs_blob blob{};
    CHECK_ERR(redfs_read(d.depot, redfs_hash("base\\nope.bin"), REDFS_PART_ALL, &blob),
              REDFS_E_NOT_FOUND);
    CHECK(blob.data == nullptr);  // nothing allocated on the failure path
}

TEST(archive, read_into_reports_required_size) {
    ArchiveBuilder ab;
    const uint64_t key = redfs_hash("base\\sized.bin");
    ab.add(key, std::vector<uint8_t>(100, 0xAB));
    TempDepot d("archive_sized.archive", ab.build());
    if (!d.depot) return;

    uint8_t small[10];
    uint64_t written = 0;
    CHECK_ERR(redfs_read_into(d.depot, key, REDFS_PART_ALL, small, sizeof small, &written),
              REDFS_E_RANGE);
    CHECK_EQ(written, 100ull);  // required size still reported

    std::vector<uint8_t> big(100);
    CHECK_OK(redfs_read_into(d.depot, key, REDFS_PART_ALL, big.data(), big.size(), &written));
    CHECK_EQ(written, 100ull);
    CHECK_EQ(big[0], 0xABu);
    CHECK_EQ(big[99], 0xABu);
}

TEST(archive, mount_order_decides_overrides) {
    const uint64_t key = redfs_hash("base\\contested.bin");

    ArchiveBuilder first, second;
    first.add(key, {'O', 'L', 'D'});
    second.add(key, {'N', 'E', 'W'});

    const std::string p1 = temp_path("override_a.archive");
    const std::string p2 = temp_path("override_b.archive");
    ArchiveBuilder::write(p1, first.build());
    ArchiveBuilder::write(p2, second.build());

    redfs_depot* depot = nullptr;
    redfs_depot_open_empty(&depot);
    CHECK(depot != nullptr);
    if (depot) {
        CHECK_OK(redfs_depot_mount(depot, p1.c_str()));
        CHECK_OK(redfs_depot_mount(depot, p2.c_str()));
        CHECK_EQ(redfs_depot_file_count(depot), 1ull);  // deduplicated

        redfs_blob blob{};
        CHECK_OK(redfs_read(depot, key, REDFS_PART_ALL, &blob));
        CHECK(blob.size == 3 && std::memcmp(blob.data, "NEW", 3) == 0);  // later wins
        redfs_blob_free(&blob);
        redfs_depot_close(depot);
    }
    std::remove(p1.c_str());
    std::remove(p2.c_str());
}

TEST(archive, rejects_garbage) {
    const std::string p = temp_path("garbage.archive");
    ArchiveBuilder::write(p, std::vector<uint8_t>(512, 0x7F));  // no RDAR magic

    redfs_depot* depot = nullptr;
    redfs_depot_open_empty(&depot);
    if (depot) {
        CHECK(redfs_depot_mount(depot, p.c_str()) != REDFS_OK);
        redfs_depot_close(depot);
    }
    std::remove(p.c_str());
}

TEST(archive, rejects_truncated_index) {
    ArchiveBuilder ab;
    ab.add(redfs_hash("base\\a.bin"), {'a'});
    std::vector<uint8_t> bytes = ab.build();
    // Claim an index far larger than the file.
    const uint32_t huge = 0x7FFFFFFF;
    std::memcpy(bytes.data() + 16, &huge, 4);

    const std::string p = temp_path("truncated.archive");
    ArchiveBuilder::write(p, bytes);
    redfs_depot* depot = nullptr;
    redfs_depot_open_empty(&depot);
    if (depot) {
        CHECK(redfs_depot_mount(depot, p.c_str()) != REDFS_OK);
        redfs_depot_close(depot);
    }
    std::remove(p.c_str());
}

// =============================================================================
// layering -- the real deployment shape: base archives, mod archives that
// override the base and each other, and mods that add entirely new files
// =============================================================================

namespace {

// Mounts a stack of archives in order, cleaning up the files afterwards.
struct LayeredDepot {
    std::vector<std::string> paths;
    redfs_depot* depot = nullptr;

    explicit LayeredDepot(const char* tag) : tag_(tag) {
        redfs_depot_open_empty(&depot);
    }
    ~LayeredDepot() {
        if (depot) redfs_depot_close(depot);
        for (const auto& p : paths) std::remove(p.c_str());
    }

    // Adds one archive on top of the stack. `files` is (depot path, contents).
    bool push(const std::vector<std::pair<std::string, std::string>>& files) {
        ArchiveBuilder ab;
        for (const auto& [path, body] : files)
            ab.add(redfs_hash(path.c_str()),
                   std::vector<uint8_t>(body.begin(), body.end()));

        char name[128];
        std::snprintf(name, sizeof name, "%s_%02zu.archive", tag_, paths.size());
        const std::string p = temp_path(name);
        if (!ArchiveBuilder::write(p, ab.build())) return false;
        paths.push_back(p);
        return depot && redfs_depot_mount(depot, p.c_str()) == REDFS_OK;
    }

    // Contents of a file, or "" if absent.
    std::string read(const std::string& path) const {
        redfs_blob blob{};
        if (redfs_read(depot, redfs_hash(path.c_str()), REDFS_PART_ALL, &blob) != REDFS_OK)
            return {};
        std::string s(reinterpret_cast<const char*>(blob.data), (size_t)blob.size);
        redfs_blob_free(const_cast<redfs_blob*>(&blob));
        return s;
    }

private:
    const char* tag_;
};

}  // namespace

TEST(layering, mods_override_base_and_each_other) {
    LayeredDepot d("layer");
    if (!d.depot) return;

    // Base game: three files.
    CHECK(d.push({{"base\\a.bin", "base-a"},
                  {"base\\b.bin", "base-b"},
                  {"base\\c.bin", "base-c"}}));

    // Mod 1: overrides a, adds a brand-new file.
    CHECK(d.push({{"base\\a.bin", "mod1-a"}, {"base\\new1.bin", "mod1-new"}}));

    // Mod 2: overrides a again (wins, mounted later) and b; adds another new file.
    CHECK(d.push({{"base\\a.bin", "mod2-a"},
                  {"base\\b.bin", "mod2-b"},
                  {"base\\new2.bin", "mod2-new"}}));

    // Last mount wins for every contested path.
    CHECK_STR(d.read("base\\a.bin").c_str(), "mod2-a");   // base -> mod1 -> mod2
    CHECK_STR(d.read("base\\b.bin").c_str(), "mod2-b");   // base -> mod2
    CHECK_STR(d.read("base\\c.bin").c_str(), "base-c");   // untouched
    // Files a mod introduces are readable like any other.
    CHECK_STR(d.read("base\\new1.bin").c_str(), "mod1-new");
    CHECK_STR(d.read("base\\new2.bin").c_str(), "mod2-new");

    // Five distinct paths across three archives: overrides collapse, new files add.
    CHECK_EQ(redfs_depot_file_count(d.depot), 5ull);
    CHECK_EQ(redfs_depot_archive_count(d.depot), 3u);

    // stat must report the winning archive, not the first one that had the path.
    redfs_file_info info{};
    CHECK_OK(redfs_stat(d.depot, redfs_hash("base\\a.bin"), &info));
    CHECK_EQ(info.archive_index, 2u);
}

TEST(layering, many_archives_deep_override_chain) {
    // The realistic shape: a lot of base archives, a lot of mods, a path that
    // every single mod overrides. Also checks that mounting scales -- each
    // archive costs a file handle, a mapping and an index view.
    constexpr int kBase = 12;
    constexpr int kMods = 60;

    LayeredDepot d("deep");
    if (!d.depot) return;

    for (int i = 0; i < kBase; ++i) {
        std::vector<std::pair<std::string, std::string>> files;
        for (int f = 0; f < 20; ++f)
            files.emplace_back("base\\pack" + std::to_string(i) + "\\f" + std::to_string(f),
                               "base" + std::to_string(i));
        files.emplace_back("base\\contested.bin", "base" + std::to_string(i));
        CHECK(d.push(files));
    }

    for (int m = 0; m < kMods; ++m) {
        CHECK(d.push({{"base\\contested.bin", "mod" + std::to_string(m)},
                      {"mod\\added" + std::to_string(m) + ".bin", "new" + std::to_string(m)}}));
    }

    CHECK_EQ(redfs_depot_archive_count(d.depot), (uint32_t)(kBase + kMods));

    // The last mod mounted owns the contested path, through 71 layers of override.
    CHECK_STR(d.read("base\\contested.bin").c_str(),
              ("mod" + std::to_string(kMods - 1)).c_str());

    // Every mod's added file survives, and every base file is still reachable.
    for (int m = 0; m < kMods; ++m)
        CHECK_STR(d.read("mod\\added" + std::to_string(m) + ".bin").c_str(),
                  ("new" + std::to_string(m)).c_str());
    CHECK_STR(d.read("base\\pack0\\f0").c_str(), "base0");
    CHECK_STR(d.read("base\\pack11\\f19").c_str(), "base11");

    // kBase*20 base files + 1 contested + kMods added.
    CHECK_EQ(redfs_depot_file_count(d.depot), (uint64_t)(kBase * 20 + 1 + kMods));
}

namespace {

// Builds a fake install tree so redfs_depot_open's own scanning can be tested:
//   <root>/archive/pc/content     base game
//   <root>/archive/pc/ep1         expansion
//   <root>/archive/pc/mod         legacy .archive mods
//   <root>/mods/<name>/archives   REDmod
struct FakeInstall {
    std::string root;

    explicit FakeInstall(const char* tag) {
        root = temp_path(tag);
        mkdir_p(root);
        for (const char* sub : {"archive", "archive\\pc", "archive\\pc\\content",
                                "archive\\pc\\ep1", "archive\\pc\\mod", "mods"})
            mkdir_p(root + "\\" + sub);
    }
    ~FakeInstall() { remove_tree(root); }

    void add(const char* relative_dir, const char* filename,
             const std::vector<std::pair<std::string, std::string>>& files) {
        const std::string dir = root + "\\" + relative_dir;
        mkdir_p(dir);
        ArchiveBuilder ab;
        for (const auto& [path, body] : files)
            ab.add(redfs_hash(path.c_str()),
                   std::vector<uint8_t>(body.begin(), body.end()));
        ArchiveBuilder::write(dir + "\\" + filename, ab.build());
    }

    // CreateDirectory makes exactly one level, so nested paths like
    // mods\Foo\archives need every ancestor created first.
    static void mkdir_p(const std::string& p) {
        for (size_t i = 0; i <= p.size(); ++i) {
            if (i == p.size() || p[i] == '\\' || p[i] == '/') {
                if (i == 0) continue;
                const std::string part = p.substr(0, i);
                // Skip the drive letter ("C:").
                if (part.size() == 2 && part[1] == ':') continue;
                ::CreateDirectoryA(part.c_str(), nullptr);
            }
        }
    }

    static void remove_tree(const std::string& dir) {
        WIN32_FIND_DATAA fd{};
        HANDLE h = ::FindFirstFileA((dir + "\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (std::strcmp(fd.cFileName, ".") == 0 || std::strcmp(fd.cFileName, "..") == 0)
                    continue;
                const std::string child = dir + "\\" + fd.cFileName;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    remove_tree(child);
                else
                    ::DeleteFileA(child.c_str());
            } while (::FindNextFileA(h, &fd));
            ::FindClose(h);
        }
        ::RemoveDirectoryA(dir.c_str());
    }
};

std::string read_from(redfs_depot* d, const char* path) {
    redfs_blob blob{};
    if (redfs_read(d, redfs_hash(path), REDFS_PART_ALL, &blob) != REDFS_OK) return {};
    std::string s(reinterpret_cast<const char*>(blob.data), (size_t)blob.size);
    redfs_blob_free(&blob);
    return s;
}

}  // namespace

TEST(layering, install_scan_order_matches_the_game) {
    // The deployment shape a mod manager produces, and the order the game
    // resolves it in: content -> ep1 -> REDmod -> legacy mods, later winning.
    FakeInstall fi("install");

    fi.add("archive\\pc\\content", "basegame_1.archive",
           {{"base\\shared.bin", "content"}, {"base\\only_base.bin", "base"}});
    fi.add("archive\\pc\\ep1", "ep1_1.archive",
           {{"base\\shared.bin", "ep1"}, {"base\\only_ep1.bin", "ep1"}});
    fi.add("mods\\SomeRedMod\\archives", "redmod.archive",
           {{"base\\shared.bin", "redmod"}, {"base\\only_redmod.bin", "redmod"}});
    fi.add("archive\\pc\\mod", "zz_legacy.archive",
           {{"base\\shared.bin", "legacy"}, {"base\\only_legacy.bin", "legacy"}});

    redfs_depot* d = nullptr;
    CHECK_OK(redfs_depot_open(fi.root.c_str(), REDFS_SCAN_ALL, &d));
    if (!d) return;

    CHECK_EQ(redfs_depot_archive_count(d), 4u);

    // Legacy mods sit on top of everything, REDmod above the base game.
    CHECK_STR(read_from(d, "base\\shared.bin").c_str(), "legacy");
    // Everything each layer contributes uniquely is still reachable.
    CHECK_STR(read_from(d, "base\\only_base.bin").c_str(), "base");
    CHECK_STR(read_from(d, "base\\only_ep1.bin").c_str(), "ep1");
    CHECK_STR(read_from(d, "base\\only_redmod.bin").c_str(), "redmod");
    CHECK_STR(read_from(d, "base\\only_legacy.bin").c_str(), "legacy");

    redfs_depot_close(d);
}

TEST(layering, scan_flags_select_layers) {
    FakeInstall fi("flags");
    fi.add("archive\\pc\\content", "basegame_1.archive", {{"base\\shared.bin", "content"}});
    fi.add("mods\\ARedMod\\archives", "a.archive", {{"base\\shared.bin", "redmod"}});
    fi.add("archive\\pc\\mod", "zz.archive", {{"base\\shared.bin", "legacy"}});

    // Vanilla only: mods are not consulted at all.
    redfs_depot* d = nullptr;
    CHECK_OK(redfs_depot_open(fi.root.c_str(), REDFS_SCAN_CONTENT, &d));
    if (d) {
        CHECK_EQ(redfs_depot_archive_count(d), 1u);
        CHECK_STR(read_from(d, "base\\shared.bin").c_str(), "content");
        redfs_depot_close(d);
    }

    // Base + REDmod, but no legacy: REDmod wins.
    d = nullptr;
    CHECK_OK(redfs_depot_open(fi.root.c_str(), REDFS_SCAN_CONTENT | REDFS_SCAN_REDMOD, &d));
    if (d) {
        CHECK_EQ(redfs_depot_archive_count(d), 2u);
        CHECK_STR(read_from(d, "base\\shared.bin").c_str(), "redmod");
        redfs_depot_close(d);
    }
}

TEST(layering, redmod_folders_mount_in_name_order) {
    FakeInstall fi("redmod");
    fi.add("archive\\pc\\content", "basegame_1.archive", {{"base\\shared.bin", "content"}});
    fi.add("mods\\AAA_first\\archives", "m.archive", {{"base\\shared.bin", "aaa"}});
    fi.add("mods\\ZZZ_last\\archives", "m.archive", {{"base\\shared.bin", "zzz"}});

    redfs_depot* d = nullptr;
    CHECK_OK(redfs_depot_open(fi.root.c_str(), REDFS_SCAN_ALL, &d));
    if (!d) return;
    CHECK_EQ(redfs_depot_archive_count(d), 3u);
    // Folders mount in name order, so the last-named REDmod wins.
    CHECK_STR(read_from(d, "base\\shared.bin").c_str(), "zzz");
    redfs_depot_close(d);
}

TEST(layering, redmod_archives_are_found_in_subfolders) {
    // mods/<name>/archives is searched RECURSIVELY. RedFS listed only the top
    // level, so a REDmod that organises its archives into subfolders had them
    // silently ignored -- the mod simply did not load, with no diagnostic.
    //
    // The reference is WolvenKit's ArchiveManager:
    //   GetFiles(<mod>/archives, "*.archive", SearchOption.AllDirectories)
    // Only REDmod recurses; archive/pc/mod is TopDirectoryOnly there and here.
    FakeInstall fi("redmodsub");
    fi.add("archive\\pc\\content", "basegame_1.archive",
           {{"base\\shared.bin", "content"}, {"base\\nested_only.bin", "content"}});
    // Nothing at the top level of this mod's archives folder at all.
    fi.add("mods\\NestedMod\\archives\\dlc\\deep", "inner.archive",
           {{"base\\shared.bin", "nested"}, {"base\\nested_only.bin", "nested"}});

    redfs_depot* d = nullptr;
    CHECK_OK(redfs_depot_open(fi.root.c_str(), REDFS_SCAN_ALL, &d));
    if (!d) return;

    // Two archives, not one: the nested archive must have been discovered.
    CHECK_EQ(redfs_depot_archive_count(d), 2u);
    CHECK_STR(read_from(d, "base\\shared.bin").c_str(), "nested");
    CHECK_STR(read_from(d, "base\\nested_only.bin").c_str(), "nested");
    redfs_depot_close(d);
}

TEST(layering, redmod_orders_nested_archives_by_full_path) {
    // The sort is over FULL PATHS, then reversed, so a subdirectory interleaves
    // with the top level by path order rather than being appended after it.
    //
    // Picking names that actually distinguish that from a basename sort takes
    // care. The two orders agree far more often than not -- the discriminating
    // shape is a top-level name that sorts BEFORE the subdirectory's own name
    // while its basename sorts AFTER the nested file's:
    //
    //   full paths:  archives\m_top.archive  <  archives\sub\a_deep.archive
    //                ('m' < 's')                 -> reversed: m_top mounts LAST
    //   basenames:   a_deep.archive          <  m_top.archive
    //                                            -> reversed: a_deep mounts LAST
    //
    // So full-path ordering makes "top" win and basename ordering makes "deep"
    // win. A first attempt at this test used z_top, where both orders give the
    // same winner -- it passed against a deliberately wrong sort.
    FakeInstall fi("redmodorder");
    fi.add("archive\\pc\\content", "basegame_1.archive", {{"base\\shared.bin", "content"}});
    fi.add("mods\\OrderMod\\archives", "m_top.archive", {{"base\\shared.bin", "top"}});
    fi.add("mods\\OrderMod\\archives\\sub", "a_deep.archive", {{"base\\shared.bin", "deep"}});

    redfs_depot* d = nullptr;
    CHECK_OK(redfs_depot_open(fi.root.c_str(), REDFS_SCAN_ALL, &d));
    if (!d) return;
    CHECK_EQ(redfs_depot_archive_count(d), 3u);
    CHECK_STR(read_from(d, "base\\shared.bin").c_str(), "top");
    redfs_depot_close(d);
}

TEST(layering, remount_after_adding_a_mod) {
    // Installing a mod mid-session: mount it on top and the winner changes,
    // without disturbing anything else.
    LayeredDepot d("remount");
    if (!d.depot) return;

    CHECK(d.push({{"base\\x.bin", "original"}, {"base\\y.bin", "keep"}}));
    CHECK_STR(d.read("base\\x.bin").c_str(), "original");

    CHECK(d.push({{"base\\x.bin", "replaced"}}));
    CHECK_STR(d.read("base\\x.bin").c_str(), "replaced");
    CHECK_STR(d.read("base\\y.bin").c_str(), "keep");
    CHECK_EQ(redfs_depot_file_count(d.depot), 2ull);
}

// =============================================================================
// CR2W
// =============================================================================

namespace {
// Parses a CR2W blob and hands the handle to a body, cleaning up after.
template <typename Fn>
void with_cr2w(const std::vector<uint8_t>& bytes, Fn&& fn) {
    redfs_cr2w* f = nullptr;
    const redfs_status st = redfs_cr2w_open(bytes.data(), bytes.size(), &f);
    if (st != REDFS_OK) {
        ++test::checks();
        test::report(__FILE__, __LINE__, "redfs_cr2w_open", redfs_last_error());
        return;
    }
    fn(f);
    redfs_cr2w_close(f);
}
}  // namespace

TEST(cr2w, scalar_values) {
    Cr2wBuilder b;
    b.begin_chunk("TestClass");
    b.prop_bool("flagTrue", true);
    b.prop_bool("flagFalse", false);
    b.prop_u8("small", 200);
    b.prop_u16("medium", 40000);
    b.prop_u32("large", 3000000000u);
    b.prop_u64("huge", 0xDEADBEEFCAFEBABEull);
    b.prop_i32("negative", -12345);
    b.prop_f32("ratio", 0.5f);
    b.prop_cname("label", "SomeName");
    b.end_chunk();

    with_cr2w(b.build(), [](redfs_cr2w* f) {
        CHECK_STR(redfs_cr2w_root_type(f), "TestClass");
        CHECK_EQ(redfs_cr2w_chunk_count(f), 1u);

        redfs_value v{};
        CHECK_OK(redfs_cr2w_get(f, 0, "flagTrue", &v));
        CHECK_EQ((int)v.kind, (int)REDFS_KIND_BOOL);
        CHECK_EQ(v.as.u, 1ull);

        CHECK_OK(redfs_cr2w_get(f, 0, "flagFalse", &v));
        CHECK_EQ(v.as.u, 0ull);

        CHECK_OK(redfs_cr2w_get(f, 0, "small", &v));
        CHECK_EQ((int)v.kind, (int)REDFS_KIND_UINT);
        CHECK_EQ(v.as.u, 200ull);

        CHECK_OK(redfs_cr2w_get(f, 0, "medium", &v));
        CHECK_EQ(v.as.u, 40000ull);

        CHECK_OK(redfs_cr2w_get(f, 0, "large", &v));
        CHECK_EQ(v.as.u, 3000000000ull);

        CHECK_OK(redfs_cr2w_get(f, 0, "huge", &v));
        CHECK_EQ(v.as.u, 0xDEADBEEFCAFEBABEull);

        CHECK_OK(redfs_cr2w_get(f, 0, "negative", &v));
        CHECK_EQ((int)v.kind, (int)REDFS_KIND_INT);
        CHECK_EQ(v.as.i, -12345ll);

        CHECK_OK(redfs_cr2w_get(f, 0, "ratio", &v));
        CHECK_EQ((int)v.kind, (int)REDFS_KIND_FLOAT);
        CHECK_NEAR(v.as.f, 0.5, 1e-9);

        CHECK_OK(redfs_cr2w_get(f, 0, "label", &v));
        CHECK_EQ((int)v.kind, (int)REDFS_KIND_NAME);
        CHECK_STR(v.as.s, "SomeName");
    });
}

TEST(cr2w, enums_decode_as_names) {
    // The behaviour the texture format mapping depends on: an enum is a
    // name-table index, so it resolves to its symbolic string, not an ordinal.
    Cr2wBuilder b;
    b.begin_chunk("TestClass");
    b.prop_enum("compression", "ETextureCompression", "TCM_QualityColor");
    b.end_chunk();

    with_cr2w(b.build(), [](redfs_cr2w* f) {
        redfs_value v{};
        CHECK_OK(redfs_cr2w_get(f, 0, "compression", &v));
        CHECK_EQ((int)v.kind, (int)REDFS_KIND_NAME);
        CHECK_STR(v.as.s, "TCM_QualityColor");
        CHECK_STR(v.type, "ETextureCompression");
    });
}

TEST(cr2w, nested_structs_via_dotted_path) {
    Cr2wBuilder b;
    b.begin_chunk("Outer");
    b.begin_struct("header", "HeaderType");
    b.begin_struct("sizeInfo", "SizeInfo");
    b.prop_in_u16("width", 1024);
    b.prop_in_u16("height", 512);
    b.end_struct();
    b.prop_in_u32("version", 7);
    b.end_struct();
    b.end_chunk();

    with_cr2w(b.build(), [](redfs_cr2w* f) {
        redfs_value v{};
        CHECK_OK(redfs_cr2w_get(f, 0, "header.sizeInfo.width", &v));
        CHECK_EQ(v.as.u, 1024ull);
        CHECK_OK(redfs_cr2w_get(f, 0, "header.sizeInfo.height", &v));
        CHECK_EQ(v.as.u, 512ull);
        CHECK_OK(redfs_cr2w_get(f, 0, "header.version", &v));
        CHECK_EQ(v.as.u, 7ull);

        // Intermediate node is a struct.
        CHECK_OK(redfs_cr2w_get(f, 0, "header", &v));
        CHECK_EQ((int)v.kind, (int)REDFS_KIND_STRUCT);

        // Paths that do not exist, at each level.
        CHECK_ERR(redfs_cr2w_get(f, 0, "header.sizeInfo.depth", &v), REDFS_E_NOT_FOUND);
        CHECK_ERR(redfs_cr2w_get(f, 0, "nope.width", &v), REDFS_E_NOT_FOUND);
        // Descending through a scalar is not a struct traversal.
        CHECK_ERR(redfs_cr2w_get(f, 0, "header.version.nope", &v), REDFS_E_NOT_FOUND);
    });
}

TEST(cr2w, handles_and_buffers) {
    Cr2wBuilder b;
    b.begin_chunk("Root");
    b.prop_handle("blob", "handle:SomeBlob", 1);
    b.prop_handle("nothing", "handle:SomeBlob", -1);
    b.prop_deferred_buffer("textureData", 0);
    b.prop_data_buffer("renderBuffer", 3);
    b.end_chunk();
    b.begin_chunk("SomeBlob");
    b.prop_u32("marker", 42);
    b.end_chunk();

    with_cr2w(b.build(), [](redfs_cr2w* f) {
        CHECK_EQ(redfs_cr2w_chunk_count(f), 2u);
        CHECK_STR(redfs_cr2w_chunk_type(f, 1), "SomeBlob");
        CHECK_EQ(redfs_cr2w_find_chunk(f, "SomeBlob"), 1);
        CHECK_EQ(redfs_cr2w_find_chunk(f, "Nonexistent"), -1);

        redfs_value v{};
        CHECK_OK(redfs_cr2w_get(f, 0, "blob", &v));
        CHECK_EQ((int)v.kind, (int)REDFS_KIND_HANDLE);
        CHECK_EQ(v.as.chunk, 1);

        CHECK_OK(redfs_cr2w_get(f, 0, "nothing", &v));
        CHECK_EQ(v.as.chunk, -1);  // null handle

        // Both buffer spellings must resolve to a buffer index.
        CHECK_OK(redfs_cr2w_get(f, 0, "textureData", &v));
        CHECK_EQ((int)v.kind, (int)REDFS_KIND_BUFFER);
        CHECK_EQ(v.as.buffer, 0u);

        CHECK_OK(redfs_cr2w_get(f, 0, "renderBuffer", &v));
        CHECK_EQ((int)v.kind, (int)REDFS_KIND_BUFFER);
        CHECK_EQ(v.as.buffer, 3u);

        // Follow the handle and read through it.
        CHECK_OK(redfs_cr2w_get(f, 1, "marker", &v));
        CHECK_EQ(v.as.u, 42ull);
    });
}

TEST(cr2w, deferred_buffer_lowercase_spelling) {
    // Regression: texture resources spell this type with a leading lowercase 's'
    // while everything else capitalises it. Matching case-sensitively made the
    // value fall through to the 2-byte enum branch and decode as a name.
    for (const char* spelling :
         {"serializationDeferredDataBuffer", "SerializationDeferredDataBuffer"}) {
        Cr2wBuilder b;
        b.begin_chunk("Root");
        b.prop_deferred_buffer("textureData", 2, spelling);
        b.end_chunk();

        with_cr2w(b.build(), [](redfs_cr2w* f) {
            redfs_value v{};
            CHECK_OK(redfs_cr2w_get(f, 0, "textureData", &v));
            CHECK_EQ((int)v.kind, (int)REDFS_KIND_BUFFER);
            CHECK_EQ(v.as.buffer, 2u);
        });
    }
}

TEST(cr2w, imports_are_readable) {
    Cr2wBuilder b;
    b.import("base\\materials\\thing.mt", "IMaterial");
    b.import("base\\worlds\\place.mlsetup", "Multilayer_Setup");
    b.begin_chunk("Root");
    b.prop_rref("material", "rRef:IMaterial", 1);
    b.end_chunk();

    with_cr2w(b.build(), [](redfs_cr2w* f) {
        CHECK_EQ(redfs_cr2w_import_count(f), 2u);
        CHECK_STR(redfs_cr2w_import_path(f, 0), "base\\materials\\thing.mt");
        CHECK_STR(redfs_cr2w_import_path(f, 1), "base\\worlds\\place.mlsetup");
        CHECK_STR(redfs_cr2w_import_type(f, 0), "IMaterial");
        CHECK_STR(redfs_cr2w_import_path(f, 99), "");  // out of range is empty, not a crash

        // rRef resolves through the import table.
        redfs_value v{};
        CHECK_OK(redfs_cr2w_get(f, 0, "material", &v));
        CHECK_EQ((int)v.kind, (int)REDFS_KIND_STRING);
        CHECK_STR(v.as.s, "base\\materials\\thing.mt");
    });
}

namespace {
struct PropCount {
    int n = 0;
    std::vector<std::string> names;
};
int count_props(const char* name, const redfs_value*, void* user) {
    auto* c = static_cast<PropCount*>(user);
    ++c->n;
    c->names.emplace_back(name);
    return 1;
}
int stop_after_first(const char*, const redfs_value*, void*) { return 0; }
}  // namespace

TEST(cr2w, walk_enumerates_and_can_stop) {
    Cr2wBuilder b;
    b.begin_chunk("Root");
    b.prop_u32("a", 1);
    b.prop_u32("b", 2);
    b.prop_u32("c", 3);
    b.end_chunk();

    with_cr2w(b.build(), [](redfs_cr2w* f) {
        PropCount c;
        CHECK_OK(redfs_cr2w_walk(f, 0, nullptr, count_props, &c));
        CHECK_EQ(c.n, 3);
        CHECK_STR(c.names[0].c_str(), "a");
        CHECK_STR(c.names[2].c_str(), "c");

        // Returning 0 must stop the walk.
        PropCount stopped;
        CHECK_OK(redfs_cr2w_walk(f, 0, nullptr, stop_after_first, &stopped));
    });
}

namespace {
struct ElemCollect {
    std::vector<uint64_t> values;
    std::vector<std::string> names;
};
int collect_elems(uint32_t, const redfs_value* v, void* user) {
    auto* c = static_cast<ElemCollect*>(user);
    if (v->kind == REDFS_KIND_UINT) c->values.push_back(v->as.u);
    // NAME and STRING both land in `names`: an enum element and a CString/NodeRef
    // element are both "a piece of text" as far as a collecting test cares.
    if (v->kind == REDFS_KIND_NAME || v->kind == REDFS_KIND_STRING)
        c->names.emplace_back(v->as.s ? v->as.s : "");
    return 1;
}
}  // namespace

TEST(cr2w, arrays_of_fixed_width_elements) {
    const uint32_t nums[4] = {10, 20, 30, 40};
    Cr2wBuilder b;
    b.begin_chunk("Root");
    b.prop_array("numbers", "Uint32", 4, nums, sizeof nums);
    b.prop_array_cname("materials", {"mat_a", "mat_b", "mat_c"});
    b.end_chunk();

    with_cr2w(b.build(), [](redfs_cr2w* f) {
        redfs_value arr{};
        CHECK_OK(redfs_cr2w_get(f, 0, "numbers", &arr));
        CHECK_EQ((int)arr.kind, (int)REDFS_KIND_ARRAY);
        CHECK_EQ(arr.as.u, 4ull);

        ElemCollect c;
        CHECK_OK(redfs_cr2w_walk_array(f, &arr, collect_elems, &c));
        CHECK_EQ(c.values.size(), 4u);
        if (c.values.size() == 4) {
            CHECK_EQ(c.values[0], 10ull);
            CHECK_EQ(c.values[3], 40ull);
        }

        CHECK_OK(redfs_cr2w_get(f, 0, "materials", &arr));
        CHECK_EQ(arr.as.u, 3ull);
        ElemCollect m;
        CHECK_OK(redfs_cr2w_walk_array(f, &arr, collect_elems, &m));
        CHECK_EQ(m.names.size(), 3u);
        if (m.names.size() == 3) CHECK_STR(m.names[1].c_str(), "mat_b");
    });
}

// --- regressions from the adversarial review of cr2w.cpp ---------------------

TEST(cr2w, struct_array_size_field_cannot_hang) {
    // struct_end advanced by `8 + (sz - 4)`, evaluated in 32-bit. sz == 0xFFFFFFFC
    // gives 8 + 0xFFFFFFF8 == 0x100000000 -> 0, so the pointer never moved and the
    // after-the-fact bounds check could not fire: an unbounded spin, reachable
    // from redfs_mesh_open on a mod-supplied .mesh.
    //
    // If this regresses, the test does not fail -- it hangs. That is the point:
    // the fuzzer's 0x7FFFFFFF and 0xFFFFFFFF advance by ~2 GB and 3, so neither
    // could ever reach the one fatal value.
    fixture::Buf arr;
    arr.u32(1);   // one element
    arr.u8(0);    // struct leading zero
    arr.u16(1);   // name index, non-zero so the walk continues
    arr.u16(1);   // type index
    arr.u32(0xFFFFFFFCu);  // the size field that used to wrap the advance to zero
    arr.u32(0);   // trailing bytes so the 8-byte header read is in bounds

    Cr2wBuilder b;
    b.begin_chunk("Root");
    b.prop("items", "array:SomeStruct", arr.bytes.data(), arr.size());
    b.end_chunk();

    with_cr2w(b.build(), [](redfs_cr2w* f) {
        redfs_value v{};
        CHECK_OK(redfs_cr2w_get(f, 0, "items", &v));
        CHECK_EQ((int)v.kind, (int)REDFS_KIND_ARRAY);
        // Must terminate and report corruption rather than spin.
        const redfs_status st = redfs_cr2w_walk_array(
            f, &v, [](uint32_t, const redfs_value*, void*) -> int { return 1; }, nullptr);
        CHECK_ERR(st, REDFS_E_CORRUPT);
    });
}

TEST(cr2w, null_deferred_buffer_is_not_part_main) {
    // Index 0 means null. Subtracting first produced 0xFFFFFFFF == REDFS_PART_MAIN,
    // so a null buffer silently resolved to segment 0 and handed back the CR2W
    // document as payload -- a DDS whose pixels were the document.
    Cr2wBuilder b;
    b.begin_chunk("Root");
    const uint16_t null_index = 0;
    b.prop("textureData", "serializationDeferredDataBuffer", &null_index, 2);
    const uint16_t real_index = 3;  // 1-based on the wire -> buffer 2
    b.prop("otherData", "serializationDeferredDataBuffer", &real_index, 2);
    b.end_chunk();

    with_cr2w(b.build(), [](redfs_cr2w* f) {
        redfs_value v{};
        CHECK_OK(redfs_cr2w_get(f, 0, "textureData", &v));
        CHECK(v.kind != REDFS_KIND_BUFFER);          // no buffer attached
        CHECK(v.as.buffer != REDFS_PART_MAIN);       // and above all, not the sentinel

        CHECK_OK(redfs_cr2w_get(f, 0, "otherData", &v));
        CHECK_EQ((int)v.kind, (int)REDFS_KIND_BUFFER);
        CHECK_EQ(v.as.buffer, 2u);                   // 3 on the wire is buffer 2
    });
}

TEST(cr2w, enum_and_string_arrays_are_sized_correctly) {
    // fixed_width knows neither enums (2-byte name index under a per-enum type
    // name) nor CString (variable). Both used to fall into the TLV struct walker,
    // which either failed a well-formed file or sheared elements at the wrong
    // boundaries. Both shapes occur in unmodified game data.
    Cr2wBuilder b;
    b.begin_chunk("Root");

    // array:<enum> -- three 2-byte name indices, one deliberately >= 256 so its
    // low byte is zero and it looks exactly like a struct's leading zero.
    const uint16_t e0 = b.name("TCM_None");
    const uint16_t e1 = b.name("TCM_DXTAlpha");
    const uint16_t e2 = b.name("TCM_QualityColor");
    fixture::Buf enums;
    enums.u16(e0);
    enums.u16(e1);
    enums.u16(e2);
    b.prop_array("modes", "ETextureCompression", 3, enums.bytes.data(), enums.size());

    b.end_chunk();

    with_cr2w(b.build(), [](redfs_cr2w* f) {
        redfs_value arr{};
        CHECK_OK(redfs_cr2w_get(f, 0, "modes", &arr));
        CHECK_EQ((int)arr.kind, (int)REDFS_KIND_ARRAY);
        CHECK_EQ(arr.as.u, 3ull);

        ElemCollect c;
        CHECK_OK(redfs_cr2w_walk_array(f, &arr, collect_elems, &c));
        CHECK_EQ(c.names.size(), 3u);
        if (c.names.size() == 3) {
            CHECK_STR(c.names[0].c_str(), "TCM_None");
            CHECK_STR(c.names[1].c_str(), "TCM_DXTAlpha");
            CHECK_STR(c.names[2].c_str(), "TCM_QualityColor");
        }
    });
}

TEST(cr2w, noderef_is_a_length_prefixed_string_not_eight_bytes) {
    // NodeRef was listed in fixed_width as 8 bytes and decoded with
    // Uint64/TweakDBID. It is neither: in CR2W it is a VLQ length-prefixed string
    // encoded exactly like CString. Red4Reader::ReadNodeRef calls
    // ReadLengthPrefixedString, and the only two overrides of it -- RedPackageReader
    // and PersistencySystem2Parser -- are not on the CR2W path.
    //
    // Two consequences, both fixed here: a scalar decoded to the first 8 bytes of
    // its prefix-plus-characters as an integer, and an array:NodeRef strode 8 bytes
    // through variable-length strings instead of walking them.
    Cr2wBuilder b;
    b.begin_chunk("Root");

    // VLQ prefix 0x80 | len, negative-signed => UTF-8.
    fixture::Buf one;
    one.u8(0x80 | 5);
    one.raw("node1", 5);
    b.prop("target", "NodeRef", one.bytes.data(), one.size());

    // Three refs of DIFFERENT lengths -- a stride of 8 cannot walk these.
    fixture::Buf many;
    for (const char* s : {"a", "bbbb", "ccccccccc"}) {
        const size_t n = std::strlen(s);
        many.u8(static_cast<uint8_t>(0x80 | n));
        many.raw(s, n);
    }
    b.prop_array("refs", "NodeRef", 3, many.bytes.data(), many.size());

    b.end_chunk();

    with_cr2w(b.build(), [](redfs_cr2w* f) {
        redfs_value v{};
        CHECK_OK(redfs_cr2w_get(f, 0, "target", &v));
        // A string, not an integer.
        CHECK_EQ((int)v.kind, (int)REDFS_KIND_STRING);
        if (v.kind == REDFS_KIND_STRING) CHECK_STR(v.as.s, "node1");

        redfs_value arr{};
        CHECK_OK(redfs_cr2w_get(f, 0, "refs", &arr));
        CHECK_EQ((int)arr.kind, (int)REDFS_KIND_ARRAY);

        ElemCollect c;
        CHECK_OK(redfs_cr2w_walk_array(f, &arr, collect_elems, &c));
        CHECK_EQ(c.names.size(), 3u);
        if (c.names.size() == 3) {
            CHECK_STR(c.names[0].c_str(), "a");
            CHECK_STR(c.names[1].c_str(), "bbbb");
            CHECK_STR(c.names[2].c_str(), "ccccccccc");
        }
    });
}

TEST(cr2w, fixed_size_array_spelling_is_handled) {
    // cr2w_decode accepted a type starting with '[' as an array, but element_type
    // had no '[' case and returned the whole name as the element type -- so every
    // element was sized and decoded as if it were the array itself. [N]T is a real
    // RED4 spelling used by CMaterialTemplate.Parameters among others.
    const uint32_t values[3] = {11, 22, 33};
    Cr2wBuilder b;
    b.begin_chunk("Root");
    fixture::Buf v;
    v.u32(3);
    v.raw(values, sizeof values);
    b.prop("samples", "[3]Uint32", v.bytes.data(), v.size());
    b.end_chunk();

    with_cr2w(b.build(), [](redfs_cr2w* f) {
        redfs_value arr{};
        CHECK_OK(redfs_cr2w_get(f, 0, "samples", &arr));
        CHECK_EQ((int)arr.kind, (int)REDFS_KIND_ARRAY);
        CHECK_EQ(arr.as.u, 3ull);

        ElemCollect c;
        CHECK_OK(redfs_cr2w_walk_array(f, &arr, collect_elems, &c));
        CHECK_EQ(c.values.size(), 3u);
        if (c.values.size() == 3) {
            CHECK_EQ(c.values[0], 11ull);
            CHECK_EQ(c.values[2], 33ull);
        }
    });
}

TEST(cr2w, repeated_string_reads_do_not_grow_the_handle) {
    // Every CString decode used to allocate and retain a fresh std::string with no
    // dedup, so a per-frame query grew the handle until it was closed. Decoding
    // the same property must reuse the cached result -- and the pointer must stay
    // stable, since callers hold it.
    Cr2wBuilder b;
    b.begin_chunk("Root");
    fixture::Buf s;
    s.u8(0x83);  // VLQ: negative-signed prefix, 3 chars, UTF-8
    s.raw("abc", 3);
    b.prop("label", "CString", s.bytes.data(), s.size());
    b.end_chunk();

    with_cr2w(b.build(), [](redfs_cr2w* f) {
        redfs_value first{};
        CHECK_OK(redfs_cr2w_get(f, 0, "label", &first));
        CHECK_EQ((int)first.kind, (int)REDFS_KIND_STRING);
        const char* first_ptr = first.as.s;

        for (int i = 0; i < 500; ++i) {
            redfs_value again{};
            CHECK_OK(redfs_cr2w_get(f, 0, "label", &again));
            if (again.as.s != first_ptr) {
                ::test::report(__FILE__, __LINE__, "cached string pointer is stable",
                               "a repeated decode allocated a new string");
                break;
            }
        }
        CHECK_STR(first_ptr, "abc");
    });
}

TEST(cr2w, rejects_malformed) {
    redfs_cr2w* f = nullptr;

    // Too small to hold a header.
    const uint8_t tiny[8] = {};
    CHECK_ERR(redfs_cr2w_open(tiny, sizeof tiny, &f), REDFS_E_CORRUPT);

    // Right size, wrong magic.
    std::vector<uint8_t> bad(0x200, 0);
    CHECK_ERR(redfs_cr2w_open(bad.data(), bad.size(), &f), REDFS_E_CORRUPT);

    // Correct magic, unsupported version.
    Cr2wBuilder b;
    b.begin_chunk("Root");
    b.prop_u32("x", 1);
    b.end_chunk();
    auto bytes = b.build(42);  // below the 163 floor
    CHECK_ERR(redfs_cr2w_open(bytes.data(), bytes.size(), &f), REDFS_E_UNSUPPORTED);

    // Valid header, string table claiming to run past the end.
    auto truncated = b.build();
    const uint32_t huge = 0x7000000;
    std::memcpy(truncated.data() + 0x28 + 4, &huge, 4);  // table[0].item_count
    CHECK_ERR(redfs_cr2w_open(truncated.data(), truncated.size(), &f), REDFS_E_CORRUPT);
}

TEST(cr2w, chunk_index_out_of_range) {
    Cr2wBuilder b;
    b.begin_chunk("Root");
    b.prop_u32("x", 1);
    b.end_chunk();

    with_cr2w(b.build(), [](redfs_cr2w* f) {
        redfs_value v{};
        CHECK_ERR(redfs_cr2w_get(f, 99, "x", &v), REDFS_E_RANGE);
        CHECK_STR(redfs_cr2w_chunk_type(f, 99), "");
    });
}

// =============================================================================
// typed helpers
// =============================================================================

TEST(texture, descriptor_and_format_mapping) {
    ArchiveBuilder ab;
    const uint64_t key = redfs_hash("base\\test\\tex.xbm");
    // 64x64 BC7_UNORM_SRGB, 7 mips: 16+4+1+1+1+1+1 blocks... computed below.
    const auto cr2w = fixture::make_texture_cr2w(64, 64, 7, "TCM_QualityColor", "TRF_TrueColor",
                                                 /*gamma=*/true);
    // BC7: 16 bytes per 4x4 block. 64x64 -> 16x16 blocks.
    uint32_t bytes = 0;
    for (uint32_t m = 0; m < 7; ++m) {
        const uint32_t w = (64u >> m) ? (64u >> m) : 1u;
        const uint32_t h = (64u >> m) ? (64u >> m) : 1u;
        bytes += ((w + 3) / 4) * ((h + 3) / 4) * 16;
    }
    ab.add(key, cr2w, {std::vector<uint8_t>(bytes, 0x11)});

    TempDepot d("tex.archive", ab.build());
    if (!d.depot) return;

    redfs_texture_desc t{};
    CHECK_OK(redfs_texture_desc_of(d.depot, key, &t));
    CHECK_EQ(t.width, 64u);
    CHECK_EQ(t.height, 64u);
    CHECK_EQ(t.mip_count, 7u);
    CHECK_EQ(t.dxgi_format, 99u);  // BC7_UNORM_SRGB
    CHECK_EQ(t.data_size, (uint64_t)bytes);

    // A DDS must carry the magic, the 148-byte header, and the whole payload.
    redfs_blob dds{};
    CHECK_OK(redfs_texture_read_dds(d.depot, key, &dds));
    CHECK_EQ(dds.size, 148ull + bytes);
    CHECK(dds.data && std::memcmp(dds.data, "DDS ", 4) == 0);
    uint32_t dxgi = 0;
    if (dds.data) std::memcpy(&dxgi, dds.data + 128, 4);  // DXT10 header
    CHECK_EQ(dxgi, 99u);
    redfs_blob_free(&dds);
}

TEST(texture, rejects_non_texture) {
    ArchiveBuilder ab;
    const uint64_t key = redfs_hash("base\\test\\notatexture.mesh");
    ab.add(key, fixture::make_mesh_cr2w(2, 4, 1.0f, 0.0f),
           {fixture::make_mesh_geometry(2, 4)});

    TempDepot d("nottex.archive", ab.build());
    if (!d.depot) return;

    // Regression: this used to describe the mesh's first embedded texture blob
    // and read `setup` off CMesh, silently reporting the fallback format.
    redfs_texture_desc t{};
    CHECK_ERR(redfs_texture_desc_of(d.depot, key, &t), REDFS_E_UNSUPPORTED);
}

namespace {

// Bytes one full mip chain of a BC-compressed surface occupies, computed
// INDEPENDENTLY of formats.cpp. The point of these checks is that two different
// derivations of the same number agree; sharing the implementation would make
// them agree trivially.
uint64_t bc_chain_bytes(uint32_t w, uint32_t h, uint32_t mips, uint32_t block_bytes) {
    uint64_t total = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        const uint32_t mw = (w >> m) ? (w >> m) : 1u;
        const uint32_t mh = (h >> m) ? (h >> m) : 1u;
        total += static_cast<uint64_t>((mw + 3) / 4) * ((mh + 3) / 4) * block_bytes;
    }
    return total;
}

// The DDS we emit, read back the way a loader reads it.
struct DdsHeader {
    uint32_t width, height, mips, format, array_size, misc_flags;
    uint64_t payload;  // bytes after the 148-byte header
};

DdsHeader parse_dds(const redfs_blob& b) {
    const uint8_t* p = static_cast<const uint8_t*>(b.data);
    auto u32 = [p](size_t off) {
        uint32_t v;
        std::memcpy(&v, p + off, 4);
        return v;
    };
    DdsHeader d{};
    d.height = u32(12);      // DDS_HEADER.dwHeight
    d.width = u32(16);       // dwWidth
    d.mips = u32(28);        // dwMipMapCount
    d.format = u32(128);     // DXT10.dxgiFormat
    d.misc_flags = u32(136); // DXT10.miscFlag
    d.array_size = u32(140); // DXT10.arraySize
    d.payload = b.size - 148;
    return d;
}

}  // namespace

TEST(texture, emitted_dds_describes_the_payload_it_carries) {
    // A semantic check rather than a memory-safety one, and the class of check
    // this suite was missing: the header we WRITE must describe the bytes we
    // ATTACH. Every existing texture test asserted on redfs_texture_desc, which
    // is the input to the encoder -- so an encoder bug was invisible.
    //
    // 2D first, then a cubemap. The cubemap case is the one that matters: on
    // disk arraySize counts CUBES and loaders multiply by 6 for
    // MISC_TEXTURECUBE, so writing the face count there declared 36 faces for a
    // 6-face payload and every emitted cubemap failed to load. Nothing caught it
    // because the fixture could not build a cubemap at all.
    struct Case {
        const char* name;
        const char* type;
        uint32_t slices;
        uint32_t expect_cube;
    } cases[] = {
        {"base\\test\\rt2d.xbm", "TEXTYPE_2D", 1, 0},
        {"base\\test\\rtcube.xbm", "TEXTYPE_CUBE", 6, 1},
    };

    for (const Case& c : cases) {
        const uint32_t w = 64, h = 64, mips = 7;
        // BC7 -- 16 bytes per 4x4 block.
        const uint64_t per_surface = bc_chain_bytes(w, h, mips, 16);

        ArchiveBuilder ab;
        const uint64_t key = redfs_hash(c.name);
        ab.add(key,
               fixture::make_texture_cr2w(w, h, mips, "TCM_QualityColor", "TRF_TrueColor", true,
                                          c.type, c.slices),
               {std::vector<uint8_t>(static_cast<size_t>(per_surface * c.slices), 0x22)});

        TempDepot d("rt.archive", ab.build());
        if (!d.depot) return;

        redfs_texture_desc t{};
        CHECK_OK(redfs_texture_desc_of(d.depot, key, &t));
        CHECK_EQ(t.is_cubemap, c.expect_cube);
        CHECK_EQ(t.slice_count, c.slices);

        redfs_blob dds{};
        CHECK_OK(redfs_texture_read_dds(d.depot, key, &dds));
        const DdsHeader hdr = parse_dds(dds);

        CHECK_EQ(hdr.width, w);
        CHECK_EQ(hdr.height, h);
        CHECK_EQ(hdr.mips, mips);
        CHECK_EQ(hdr.format, t.dxgi_format);
        CHECK_EQ(hdr.misc_flags, c.expect_cube ? 0x4u : 0u);

        // The invariant a loader actually applies: arraySize counts cubes, so
        // multiply it back out and it must equal the surfaces present.
        const uint64_t declared_faces =
            static_cast<uint64_t>(hdr.array_size) * (hdr.misc_flags & 0x4 ? 6u : 1u);
        CHECK_EQ(declared_faces, static_cast<uint64_t>(c.slices));

        // And the header must account for exactly the bytes stapled to it.
        CHECK_EQ(declared_faces * per_surface, hdr.payload);

        redfs_blob_free(&dds);
    }
}

TEST(texture, absurd_mip_count_is_rejected_not_spun_on) {
    // mipCount went from the file straight into a loop bound. 0xFFFFFFFB spun
    // ~3 s per call to mip_chain_bytes and describe_texture makes five of them,
    // measured at ~16 s total -- on the calling thread, which for a synchronous
    // entry point is the game's. REDFS_GUARD converts exceptions; a spin throws
    // nothing, so nothing caught it.
    ArchiveBuilder ab;
    const uint64_t key = redfs_hash("base\\test\\hugemips.xbm");
    // The format has to actually resolve, or describe_texture bails at the
    // DXGI_UNKNOWN gate and never reaches the loop -- which is how the first
    // version of this test passed against the unfixed code.
    ab.add(key,
           fixture::make_texture_cr2w(4, 4, 0xFFFFFFFBu, "TCM_QualityColor", "TRF_TrueColor", true),
           {std::vector<uint8_t>(64, 0)});

    TempDepot d("hugemips.archive", ab.build());
    if (!d.depot) return;

    const auto t0 = std::chrono::steady_clock::now();
    redfs_texture_desc t{};
    CHECK_ERR(redfs_texture_desc_of(d.depot, key, &t), REDFS_E_CORRUPT);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    // Deliberately generous -- the point is 16 000 vs. instant, not a benchmark.
    CHECK(ms < 1000);
}

TEST(mesh, chunks_bounds_and_appearances) {
    const uint32_t chunks = 4, verts = 8;
    const float scale = 10.0f, offset = 0.0f;

    ArchiveBuilder ab;
    const uint64_t key = redfs_hash("base\\test\\thing.mesh");
    ab.add(key, fixture::make_mesh_cr2w(chunks, verts, scale, offset),
           {fixture::make_mesh_geometry(chunks, verts)});

    TempDepot d("mesh.archive", ab.build());
    if (!d.depot) return;

    redfs_mesh* m = nullptr;
    CHECK_OK(redfs_mesh_open(d.depot, key, &m));
    if (!m) return;

    CHECK_EQ(redfs_mesh_chunk_count(m), chunks);
    CHECK_EQ(redfs_mesh_appearance_count(m), 1u);
    CHECK_STR(redfs_mesh_appearance_name(m, 0), "default");
    CHECK_EQ(redfs_mesh_find_appearance(m, "default"), 0);
    CHECK_EQ(redfs_mesh_find_appearance(m, "nope"), -1);
    CHECK_STR(redfs_mesh_chunk_material(m, 0, 2), "mat_2");
    CHECK_STR(redfs_mesh_chunk_material(m, 0, 99), "");  // out of range is empty

    // The fixture places each chunk in its own z band, ascending, so bounds must
    // come out ordered and disjoint. This is the real assertion: it exercises
    // stride, offset and dequantization together.
    float prev_max = -1e30f;
    for (uint32_t i = 0; i < chunks; ++i) {
        const redfs_mesh_chunk* c = redfs_mesh_chunk_at(m, i);
        CHECK(c != nullptr);
        if (!c) continue;
        CHECK_EQ(c->index, i);
        CHECK_EQ(c->vertex_count, verts);
        CHECK_EQ(c->lod, 1u);
        CHECK(c->bbox_min[2] <= c->bbox_max[2]);
        CHECK(c->bbox_min[2] >= prev_max);  // ascending, non-overlapping bands
        prev_max = c->bbox_max[2];
        // x is driven to the quantization extremes, so it must span the scale.
        CHECK_NEAR(c->bbox_min[0], -scale + offset, 0.01);
        CHECK_NEAR(c->bbox_max[0], scale + offset, 0.01);
    }

    CHECK(redfs_mesh_chunk_at(m, 999) == nullptr);
    redfs_mesh_close(m);
}

TEST(mesh, rejects_non_mesh) {
    ArchiveBuilder ab;
    const uint64_t key = redfs_hash("base\\test\\tex.xbm");
    ab.add(key, fixture::make_texture_cr2w(8, 8, 1, "TCM_None", "TRF_TrueColor", false),
           {std::vector<uint8_t>(256, 0)});

    TempDepot d("notmesh.archive", ab.build());
    if (!d.depot) return;

    redfs_mesh* m = nullptr;
    CHECK_ERR(redfs_mesh_open(d.depot, key, &m), REDFS_E_UNSUPPORTED);
    CHECK(m == nullptr);
}

// =============================================================================
// audio (.wem)
// =============================================================================

namespace {

// A RIFF/WAVE file shaped like Wwise's: fmt, an out-of-spec chunk, then data.
std::vector<uint8_t> make_wem(uint16_t format_tag, uint16_t channels, uint32_t rate,
                              uint16_t bits, uint32_t payload_bytes) {
    fixture::Buf fmt;
    fmt.u16(format_tag);
    fmt.u16(channels);
    fmt.u32(rate);
    fmt.u32(rate * channels * (bits ? bits / 8 : 1));  // avg bytes/sec
    fmt.u16(static_cast<uint16_t>(channels * (bits ? bits / 8 : 1)));  // block align
    fmt.u16(bits);
    fmt.u16(0);  // cbSize

    fixture::Buf body;
    body.u32(0x45564157);  // 'WAVE'

    body.u32(0x20746D66);  // 'fmt '
    body.u32(static_cast<uint32_t>(fmt.size()));
    body.raw(fmt.bytes.data(), fmt.size());

    // A Wwise-private chunk: the walker must step over it to reach 'data'.
    body.u32(0x62726F76);  // 'vorb'
    body.u32(8);
    body.u64(0);

    body.u32(0x61746164);  // 'data'
    body.u32(payload_bytes);
    for (uint32_t i = 0; i < payload_bytes; ++i) body.u8(static_cast<uint8_t>(i));

    fixture::Buf out;
    out.u32(0x46464952);  // 'RIFF'
    out.u32(static_cast<uint32_t>(body.size()));
    out.raw(body.bytes.data(), body.size());
    return out.bytes;
}

struct ChunkList {
    std::vector<std::string> ids;
};
int collect_chunk(const char fourcc[4], uint64_t, uint64_t, void* user) {
    static_cast<ChunkList*>(user)->ids.emplace_back(fourcc, 4);
    return 1;
}

}  // namespace

TEST(audio, wem_header_is_parsed) {
    ArchiveBuilder ab;
    const uint64_t key = redfs_hash("base\\sound\\voice.wem");
    ab.add(key, make_wem(0xFFFF, 1, 48000, 0, 512));  // Wwise Vorbis, mono
    TempDepot d("wem.archive", ab.build());
    if (!d.depot) return;

    redfs_audio_format container = REDFS_AUDIO_UNKNOWN;
    CHECK_OK(redfs_audio_probe(d.depot, key, &container));
    CHECK_EQ((int)container, (int)REDFS_AUDIO_WEM);

    redfs_audio_info info{};
    CHECK_OK(redfs_audio_info_of(d.depot, key, &info));
    CHECK_EQ((int)info.codec, (int)REDFS_CODEC_VORBIS);
    CHECK_EQ(info.format_tag, 0xFFFFu);
    CHECK_EQ(info.channels, 1u);
    CHECK_EQ(info.sample_rate, 48000u);
    CHECK_EQ(info.data_size, 512ull);
    CHECK(info.data_offset > 0);
    // Vorbis duration is not derivable from the header; reporting a guess would
    // be worse than reporting nothing.
    CHECK_EQ((uint64_t)info.duration_seconds, 0ull);

    CHECK_STR(redfs_audio_codec_name(REDFS_CODEC_VORBIS), "Wwise Vorbis");
}

TEST(audio, pcm_duration_is_derived) {
    // 16-bit stereo PCM at 44100: the sample count follows from the block size,
    // so duration is honest here where it is not for compressed codecs.
    const uint32_t bytes = 44100 * 2 * 2;  // exactly one second
    ArchiveBuilder ab;
    const uint64_t key = redfs_hash("base\\sound\\pcm.wem");
    ab.add(key, make_wem(0x0001, 2, 44100, 16, bytes));
    TempDepot d("wempcm.archive", ab.build());
    if (!d.depot) return;

    redfs_audio_info info{};
    CHECK_OK(redfs_audio_info_of(d.depot, key, &info));
    CHECK_EQ((int)info.codec, (int)REDFS_CODEC_PCM);
    CHECK_EQ(info.channels, 2u);
    CHECK_EQ(info.bits_per_sample, 16u);
    CHECK_EQ(info.total_samples, 44100ull);
    CHECK_NEAR(info.duration_seconds, 1.0, 1e-6);
}

TEST(audio, riff_chunks_are_walkable) {
    // Wwise keeps codec state in non-standard chunks, so a decoder front-end has
    // to be able to find them.
    const auto wem = make_wem(0xFFFF, 2, 48000, 0, 64);
    ChunkList chunks;
    CHECK_OK(redfs_audio_walk_chunks(wem.data(), wem.size(), collect_chunk, &chunks));
    CHECK_EQ(chunks.ids.size(), 3u);
    if (chunks.ids.size() == 3) {
        CHECK_STR(chunks.ids[0].c_str(), "fmt ");
        CHECK_STR(chunks.ids[1].c_str(), "vorb");
        CHECK_STR(chunks.ids[2].c_str(), "data");
    }
}

TEST(audio, rejects_malformed_wem) {
    redfs_audio_info info{};
    const uint8_t tiny[4] = {'R', 'I', 'F', 'F'};
    CHECK_ERR(redfs_audio_info_parse(tiny, sizeof tiny, &info), REDFS_E_CORRUPT);

    // Right size, wrong magic.
    std::vector<uint8_t> junk(64, 0x5A);
    CHECK_ERR(redfs_audio_info_parse(junk.data(), junk.size(), &info), REDFS_E_CORRUPT);

    // RIFF, but not a WAVE form.
    auto wem = make_wem(0x0001, 1, 8000, 8, 16);
    wem[8] = 'X';
    CHECK_ERR(redfs_audio_info_parse(wem.data(), wem.size(), &info), REDFS_E_UNSUPPORTED);

    // A chunk header that claims more bytes than the file holds must stop the
    // walk rather than read past the buffer.
    auto truncated = make_wem(0x0001, 1, 8000, 8, 16);
    const uint32_t huge = 0x7FFFFFFF;
    std::memcpy(truncated.data() + 16, &huge, 4);  // fmt chunk size
    redfs_audio_info_parse(truncated.data(), truncated.size(), &info);  // must not crash
}

// =============================================================================
// resource handle
// =============================================================================

TEST(resource, owns_its_bytes_and_its_document) {
    Cr2wBuilder cb;
    cb.begin_chunk("CMesh");
    cb.prop_u32("renderChunkCount", 7);
    cb.end_chunk();

    ArchiveBuilder ab;
    const uint64_t key = redfs_hash("base\\res\\thing.mesh");
    ab.add(key, cb.build(), {{'B', 'U', 'F', '0'}, {'B', 'U', 'F', '1'}});

    TempDepot d("resource.archive", ab.build());
    if (!d.depot) return;

    redfs_resource* r = nullptr;
    CHECK_OK(redfs_open(d.depot, key, &r));
    CHECK(r != nullptr);
    if (!r) return;

    // The type is the question redfs_read could not answer without the caller
    // assembling a blob and a document by hand in the right order.
    CHECK_STR(redfs_resource_type(r), "CMesh");
    CHECK(redfs_resource_cr2w(r) != nullptr);
    CHECK_EQ(redfs_resource_buffer_count(r), 2u);
    CHECK_EQ(redfs_resource_hash(r), key);

    // The document is usable through the handle, and its values point into bytes
    // the handle owns -- which is the pairing this type exists to enforce.
    redfs_value v{};
    CHECK_OK(redfs_cr2w_get(redfs_resource_cr2w(r), 0, "renderChunkCount", &v));
    CHECK_EQ(v.as.u, 7ull);

    // The main segment is the DOCUMENT, not buffer 0. That is the whole point:
    // redfs_read(..., 0, ...) on this file would have returned "BUF0".
    CHECK(redfs_resource_size(r) > 4);
    CHECK(std::memcmp(redfs_resource_data(r), "CR2W", 4) == 0);

    redfs_close(r);
}

TEST(resource, buffer_index_is_bounds_checked_not_reinterpreted) {
    ArchiveBuilder ab;
    const uint64_t key = redfs_hash("base\\res\\two_buffers.mesh");
    Cr2wBuilder cb;
    cb.begin_chunk("CMesh");
    cb.prop_u32("x", 1);
    cb.end_chunk();
    ab.add(key, cb.build(), {{'A', 'A', 'A'}, {'B', 'B', 'B', 'B'}});

    TempDepot d("resbuf.archive", ab.build());
    if (!d.depot) return;

    redfs_resource* r = nullptr;
    CHECK_OK(redfs_open(d.depot, key, &r));
    if (!r) return;

    redfs_blob b{};
    CHECK_OK(redfs_resource_buffer(r, 0, &b));
    CHECK_EQ(b.size, 3ull);
    CHECK(b.data && std::memcmp(b.data, "AAA", 3) == 0);
    redfs_blob_free(&b);

    CHECK_OK(redfs_resource_buffer(r, 1, &b));
    CHECK_EQ(b.size, 4ull);
    redfs_blob_free(&b);

    // Past the end is REDFS_E_RANGE against the count on the handle, not
    // arithmetic that could land on another file's segment.
    CHECK_ERR(redfs_resource_buffer(r, 2, &b), REDFS_E_RANGE);
    CHECK(b.data == nullptr);
    CHECK_ERR(redfs_resource_buffer(r, 0xFFFFFFFFu, &b), REDFS_E_RANGE);
    CHECK_ERR(redfs_resource_buffer(r, REDFS_PART_MAIN, &b), REDFS_E_RANGE);
    CHECK_ERR(redfs_resource_buffer(r, REDFS_PART_ALL, &b), REDFS_E_RANGE);

    redfs_close(r);
}

TEST(resource, a_file_with_no_document_still_opens) {
    // .wem, .bnk and friends have no CR2W. Refusing them would make redfs_open
    // useless for the cheapest question it should answer -- what is this? --
    // so it opens, reports an empty type, and still hands over the bytes.
    ArchiveBuilder ab;
    const uint64_t key = redfs_hash("base\\res\\sound.wem");
    ab.add(key, {'R', 'I', 'F', 'F', 1, 2, 3, 4});

    TempDepot d("resraw.archive", ab.build());
    if (!d.depot) return;

    redfs_resource* r = nullptr;
    CHECK_OK(redfs_open(d.depot, key, &r));
    if (!r) return;

    CHECK_STR(redfs_resource_type(r), "");
    CHECK(redfs_resource_cr2w(r) == nullptr);
    CHECK_EQ(redfs_resource_buffer_count(r), 0u);
    CHECK_EQ(redfs_resource_size(r), 8ull);
    CHECK(std::memcmp(redfs_resource_data(r), "RIFF", 4) == 0);

    redfs_close(r);
}

TEST(resource, missing_and_invalid_arguments) {
    ArchiveBuilder ab;
    ab.add(redfs_hash("base\\res\\present.bin"), {'x'});
    TempDepot d("resmiss.archive", ab.build());
    if (!d.depot) return;

    redfs_resource* r = reinterpret_cast<redfs_resource*>(0x1);
    CHECK_ERR(redfs_open(d.depot, redfs_hash("base\\res\\absent.bin"), &r), REDFS_E_NOT_FOUND);
    CHECK(r == nullptr);  // cleared even on the failure path

    CHECK_ERR(redfs_open(nullptr, 1, &r), REDFS_E_INVALID_ARG);
    CHECK_ERR(redfs_open(d.depot, 1, nullptr), REDFS_E_INVALID_ARG);
    CHECK_ERR(redfs_open_path(d.depot, nullptr, &r), REDFS_E_INVALID_ARG);

    // Every accessor tolerates a null handle rather than crashing on it.
    CHECK_STR(redfs_resource_type(nullptr), "");
    CHECK(redfs_resource_cr2w(nullptr) == nullptr);
    CHECK(redfs_resource_data(nullptr) == nullptr);
    CHECK_EQ(redfs_resource_size(nullptr), 0ull);
    CHECK_EQ(redfs_resource_buffer_count(nullptr), 0u);
    redfs_close(nullptr);

    // open_path agrees with open on the same file.
    redfs_resource* a = nullptr;
    redfs_resource* b = nullptr;
    CHECK_OK(redfs_open_path(d.depot, "base\\res\\present.bin", &a));
    CHECK_OK(redfs_open(d.depot, redfs_hash("base\\res\\present.bin"), &b));
    if (a && b) CHECK_EQ(redfs_resource_hash(a), redfs_resource_hash(b));
    redfs_close(a);
    redfs_close(b);
}

TEST(resource, cpp_facade) {
    Cr2wBuilder cb;
    cb.begin_chunk("CBitmapTexture");
    cb.prop_u32("w", 64);
    cb.end_chunk();

    ArchiveBuilder ab;
    const uint64_t key = redfs_hash("base\\res\\tex.xbm");
    ab.add(key, cb.build(), {{'P', 'I', 'X'}});

    const std::string p = temp_path("resfacade.archive");
    ArchiveBuilder::write(p, ab.build());

    redfs_depot* h = nullptr;
    redfs_depot_open_empty(&h);
    if (h) {
        CHECK_OK(redfs_depot_mount(h, p.c_str()));
        redfs::Depot d{h};

        auto res = d.resource("base\\res\\tex.xbm");
        CHECK(res.has_value());
        if (res) {
            CHECK_STR(std::string(res->type()), "CBitmapTexture");
            CHECK_EQ(res->buffer_count(), 1u);
            CHECK_EQ(res->key(), key);
            CHECK(res->cr2w() != nullptr);
            CHECK(res->bytes().size() > 4);

            auto buf = res->buffer(0);
            CHECK(buf.has_value());
            if (buf) CHECK_EQ(buf->size(), 3ull);
            CHECK(!res->buffer(1).has_value());
        }
        // A file that is not there is nullopt, not a half-built handle.
        CHECK(!d.resource("base\\res\\nope.xbm").has_value());
    }
    std::remove(p.c_str());
}

// =============================================================================
// path dictionary
// =============================================================================

TEST(paths, reverse_lookup) {
    ArchiveBuilder ab;
    const char* p1 = "base\\test\\alpha.mesh";
    const char* p2 = "base\\test\\beta.xbm";
    ab.add(redfs_hash(p1), {'a'});
    ab.add(redfs_hash(p2), {'b'});

    TempDepot d("paths.archive", ab.build());
    if (!d.depot) return;

    // A plain-text list; one of these lines names a file that is not present.
    const std::string list = temp_path("paths.txt");
    {
        FILE* f = std::fopen(list.c_str(), "wb");
        std::fprintf(f, "%s\n%s\nbase\\test\\absent.mesh\n", p1, p2);
        std::fclose(f);
    }

    uint32_t kept = 0;
    CHECK_OK(redfs_path_load(d.depot, list.c_str(), &kept));
    CHECK_EQ(kept, 2u);  // the absent path is filtered out

    CHECK_STR(redfs_path_from_hash(redfs_hash(p1)), p1);
    CHECK_STR(redfs_path_from_hash(redfs_hash(p2)), p2);
    CHECK(redfs_path_from_hash(redfs_hash("base\\test\\absent.mesh")) == nullptr);
    CHECK(redfs_path_from_hash(0xFFFFFFFFFFFFFFFFull) == nullptr);

    std::remove(list.c_str());
}

// =============================================================================
// find
// =============================================================================
//
// The dictionary is process-global and every case in this file adds to it, so
// each test below anchors its pattern on a prefix of its own. That is also what
// keeps them order-independent.

namespace {

struct Hits {
    std::vector<std::string> paths;
    std::vector<uint64_t> keys;
    uint32_t stop_after = 0;  // 0 = never stop
};

int collect(uint64_t hash, const char* path, void* user) {
    auto* h = static_cast<Hits*>(user);
    h->paths.push_back(path);
    h->keys.push_back(hash);
    return h->stop_after && h->paths.size() >= h->stop_after ? 0 : 1;
}

// Sorted, because the dictionary walks in hash order and nothing about the
// result set should depend on that.
std::vector<std::string> find_sorted(const redfs_depot* depot, const char* pattern) {
    Hits h;
    redfs_find(depot, pattern, collect, &h, nullptr);
    std::sort(h.paths.begin(), h.paths.end());
    return h.paths;
}

}  // namespace

TEST(find, literal_star_in_an_entry) {
    // Entries can hold a literal '*' -- sanitize_path strips neither wildcard --
    // and matching one against a '*' in the pattern is what breaks if
    // glob_match's branch order is reversed. See src/paths.cpp.
    redfs_path_enable();
    redfs_path_add("base\\globstar\\*weird.mesh");
    redfs_path_add("base\\globstar\\a*b\\thing.mesh");

    CHECK_EQ(find_sorted(nullptr, "base\\globstar\\*").size(), 2u);
    CHECK_EQ(find_sorted(nullptr, "base\\globstar\\*weird.mesh").size(), 1u);
    CHECK_EQ(find_sorted(nullptr, "base\\globstar\\*.mesh").size(), 2u);
    CHECK_EQ(find_sorted(nullptr, "base\\globstar\\a*b\\*").size(), 1u);
    // The minimal reported form, anchored so other cases cannot contribute.
    CHECK_EQ(find_sorted(nullptr, "base\\globstar\\*d.mesh").size(), 1u);
}

TEST(find, wildcards) {
    redfs_path_enable();
    redfs_path_add("base\\globtest\\a\\one.mesh");
    redfs_path_add("base\\globtest\\a\\two.mesh");
    redfs_path_add("base\\globtest\\b\\deep\\three.mesh");
    redfs_path_add("base\\globtest\\a\\one.xbm");
    // Lengths 2 and 4 under a\, so a '?' count actually discriminates -- with
    // only 3-character names present, "???" and "*" returned the same 2 and the
    // assertion below held for an implementation where '?' behaved as '*'.
    redfs_path_add("base\\globtest\\a\\ab.mesh");
    redfs_path_add("base\\globtest\\a\\abcd.mesh");

    // '*' crosses separators -- the documented departure from shell globbing,
    // and the reason "every mesh anywhere" is expressible at all.
    const auto meshes = find_sorted(nullptr, "base\\globtest\\*.mesh");
    CHECK_EQ(meshes.size(), 5u);
    if (meshes.size() == 5) {
        CHECK_STR(meshes[0], "base\\globtest\\a\\ab.mesh");
        CHECK_STR(meshes[1], "base\\globtest\\a\\abcd.mesh");
        CHECK_STR(meshes[2], "base\\globtest\\a\\one.mesh");
        CHECK_STR(meshes[3], "base\\globtest\\a\\two.mesh");
        // The one under a deeper directory: this is what proves '*' spans them.
        CHECK_STR(meshes[4], "base\\globtest\\b\\deep\\three.mesh");
    }

    // A prefix narrows it back down: one, two, ab, abcd.
    CHECK_EQ(find_sorted(nullptr, "base\\globtest\\a\\*.mesh").size(), 4u);

    // '?' is exactly one character, so the count discriminates by name length.
    CHECK_EQ(find_sorted(nullptr, "base\\globtest\\a\\??.mesh").size(), 1u);    // ab
    CHECK_EQ(find_sorted(nullptr, "base\\globtest\\a\\???.mesh").size(), 2u);   // one, two
    CHECK_EQ(find_sorted(nullptr, "base\\globtest\\a\\????.mesh").size(), 1u);  // abcd

    // '?' matches a separator too -- the header says so, so pin it.
    CHECK_EQ(find_sorted(nullptr, "base\\globtest\\b?deep\\three.mesh").size(), 1u);

    // A trailing separator means everything beneath, not an exact directory match.
    CHECK_EQ(find_sorted(nullptr, "base\\globtest\\a\\").size(), 5u);
    CHECK_EQ(find_sorted(nullptr, "base/globtest/").size(), 6u);

    // ...and it has to survive whatever follows the separator: sanitize_path
    // trims quotes, spaces, CR and LF, and tab is trimmed by paths_find alone.
    // Get that order wrong and each of these degrades to an exact match on a
    // directory name -- a silent 0 with REDFS_OK.
    CHECK_EQ(find_sorted(nullptr, "base\\globtest\\a\\ ").size(), 5u);      // space
    CHECK_EQ(find_sorted(nullptr, "base\\globtest\\a\\\t").size(), 5u);     // tab
    CHECK_EQ(find_sorted(nullptr, "base\\globtest\\a\\\r\n").size(), 5u);   // CRLF, e.g. from a file
    CHECK_EQ(find_sorted(nullptr, "\"base\\globtest\\a\\\"").size(), 5u);   // quoted
    CHECK_EQ(find_sorted(nullptr, "  base/globtest/a/  ").size(), 5u);      // both ends

    // Separators only means everything.
    const size_t all = find_sorted(nullptr, "*").size();
    CHECK(all > 0);
    CHECK_EQ(find_sorted(nullptr, "\\").size(), all);
    CHECK_EQ(find_sorted(nullptr, "/").size(), all);
    // But genuinely empty is still an error.
    CHECK_ERR(redfs_find(nullptr, "   ", collect, nullptr, nullptr), REDFS_E_INVALID_ARG);

    // A pattern with no wildcard is an exact match, not a substring.
    CHECK_EQ(find_sorted(nullptr, "base\\globtest\\a\\one.mesh").size(), 1u);
    CHECK_EQ(find_sorted(nullptr, "globtest").size(), 0u);

    // Anchoring is real at both ends: no implicit leading or trailing '*'.
    CHECK_EQ(find_sorted(nullptr, "globtest\\*.mesh").size(), 0u);
    CHECK_EQ(find_sorted(nullptr, "base\\globtest\\*.me").size(), 0u);
}

TEST(find, pattern_is_normalised_like_a_path) {
    redfs_path_enable();
    redfs_path_add("base\\globcase\\Mixed\\Thing.MESH");

    // The entry was canonicalised on the way in, so a pattern typed in the
    // original casing with forward slashes has to find it -- otherwise a path
    // pasted out of WolvenKit would silently match nothing.
    CHECK_EQ(find_sorted(nullptr, "Base/GlobCase/*.MESH").size(), 1u);
    CHECK_EQ(find_sorted(nullptr, "base\\globcase\\*.mesh").size(), 1u);
    // Leading separators and stray quotes are trimmed, exactly as redfs_hash does.
    CHECK_EQ(find_sorted(nullptr, "\"/base/globcase/*.mesh\"").size(), 1u);

    CHECK_ERR(redfs_find(nullptr, "", collect, nullptr, nullptr), REDFS_E_INVALID_ARG);
    CHECK_ERR(redfs_find(nullptr, nullptr, collect, nullptr, nullptr), REDFS_E_INVALID_ARG);
    CHECK_ERR(redfs_find(nullptr, "*", nullptr, nullptr, nullptr), REDFS_E_INVALID_ARG);
}

TEST(find, depot_filters_out_what_is_not_mounted) {
    ArchiveBuilder ab;
    const char* present = "base\\globdepot\\here.mesh";
    ab.add(redfs_hash(present), {'m'});

    TempDepot d("globdepot.archive", ab.build());
    if (!d.depot) return;

    // Both land in the dictionary; only one is in the depot. paths_add is not
    // filtered at insert -- redfs.h says a hit means "this is what the file is
    // called", not "this file is readable" -- so redfs_find has to filter.
    redfs_path_enable();
    redfs_path_add(present);
    redfs_path_add("base\\globdepot\\absent.mesh");

    const auto filtered = find_sorted(d.depot, "base\\globdepot\\*.mesh");
    CHECK_EQ(filtered.size(), 1u);
    if (filtered.size() == 1) CHECK_STR(filtered[0], present);

    // A null depot means no filter, and reports both.
    CHECK_EQ(find_sorted(nullptr, "base\\globdepot\\*.mesh").size(), 2u);

    // Every hash handed back must be the one that opens the file.
    Hits h;
    CHECK_OK(redfs_find(d.depot, "base\\globdepot\\*.mesh", collect, &h, nullptr));
    CHECK_EQ(h.keys.size(), 1u);
    if (h.keys.size() == 1) {
        CHECK_EQ(h.keys[0], redfs_hash(present));
        CHECK_EQ(redfs_exists(d.depot, h.keys[0]), 1);
    }
}

TEST(find, callback_can_stop_early) {
    redfs_path_enable();
    for (int i = 0; i < 8; ++i) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "base\\globstop\\f%d.mesh", i);
        redfs_path_add(buf);
    }

    Hits h;
    h.stop_after = 3;
    uint32_t matched = 0;
    CHECK_OK(redfs_find(nullptr, "base\\globstop\\*.mesh", collect, &h, &matched));
    CHECK_EQ(h.paths.size(), 3u);
    // out_matched is the TOTAL, not the number delivered: the scan finishes
    // before the first callback, so stopping early cannot change it.
    CHECK_EQ(matched, 8u);

    h = Hits{};
    CHECK_OK(redfs_find(nullptr, "base\\globstop\\*.mesh", collect, &h, &matched));
    CHECK_EQ(matched, 8u);
    CHECK_EQ(h.paths.size(), 8u);
}

TEST(find, a_loaded_dictionary_that_matches_nothing_is_a_success) {
    // The other half of find/empty_dictionary_is_not_an_empty_result: once
    // anything IS loaded, a pattern hitting nothing is a plain REDFS_OK with a
    // count of zero. Only "you never loaded a list" is an error.
    redfs_path_enable();
    redfs_path_add("base\\globnone\\x.mesh");

    uint32_t matched = 123;
    CHECK_OK(redfs_find(nullptr, "base\\globnone\\nothing*.xbm", collect, nullptr, &matched));
    CHECK_EQ(matched, 0u);
}

TEST(find, callback_may_read_and_learn_more_paths) {
    // The documented use is "find files, then read them". A read parses a CR2W,
    // which calls paths_learn_imports, which takes the dictionary mutex -- so
    // dispatching matches while still holding it deadlocks the first caller who
    // uses the API for its purpose. This test hangs rather than fails if the
    // callback is ever moved back inside the lock.
    Cr2wBuilder cb;
    cb.import("base\\globread\\learned.mt", "IMaterial");
    cb.begin_chunk("CMesh");
    cb.prop_u32("x", 1);
    cb.end_chunk();

    ArchiveBuilder ab;
    const char* doc = "base\\globread\\doc.mesh";
    ab.add(redfs_hash(doc), cb.build());

    TempDepot d("globread.archive", ab.build());
    if (!d.depot) return;

    redfs_path_enable();
    redfs_path_add(doc);

    struct Ctx {
        redfs_depot* depot;
        uint32_t read_ok;
    } ctx{d.depot, 0};

    CHECK_OK(redfs_find(
        d.depot, "base\\globread\\*.mesh",
        [](uint64_t hash, const char*, void* user) -> int {
            auto* c = static_cast<Ctx*>(user);
            redfs_blob blob{};
            if (redfs_read(c->depot, hash, REDFS_PART_MAIN, &blob) == REDFS_OK) {
                redfs_cr2w* f = nullptr;
                // Parsing is what learns the imports and re-enters the lock.
                if (redfs_cr2w_open(blob.data, blob.size, &f) == REDFS_OK) {
                    ++c->read_ok;
                    redfs_cr2w_close(f);
                }
                redfs_blob_free(&blob);
            }
            return 1;
        },
        &ctx, nullptr));

    CHECK_EQ(ctx.read_ok, 1u);
    // And the path the read learned is now findable itself.
    CHECK_EQ(find_sorted(nullptr, "base\\globread\\learned.mt").size(), 1u);
}

TEST(find, pathological_inputs_terminate) {
    // Backtracking globs are the classic blow-up shape. Two facts about it:
    //
    //   - A regression to the RECURSIVE form is a HANG, not a stack overflow.
    //     Measured against a naive recursive matcher on these inputs: the
    //     positive case completes in 8,021 calls at depth 4,221 -- well inside
    //     MSVC's 1 MiB -- while the negative case exceeded 200,000,000 calls
    //     without finishing. So ASan catches nothing here; the failure mode is
    //     the suite never returning.
    //   - Pattern length alone is not the cost driver. Measured: 2-character and
    //     200,000-character all-star patterns both cost 103 iterations against a
    //     real 94-char path, because the matcher only ever tracks the most recent
    //     star. The bound is O(min(|pattern|,|entry|) x |entry|), which is why
    //     add_locked bounds the ENTRY.
    //
    // The expensive shape is "*" + a long literal run + a character the entry
    // cannot supply, and it needs the run in the ENTRY. 900 characters keeps it
    // under add_locked's 1024 limit while still costing ~800k advances.
    redfs_path_enable();
    const std::string run(900, 'a');
    redfs_path_add(("base\\globevil\\" + run + ".mesh").c_str());

    std::string evil = "base\\globevil\\";
    for (int i = 0; i < 200; ++i) evil += "*a";
    CHECK_EQ(find_sorted(nullptr, (evil + "*.mesh").c_str()).size(), 1u);
    // Ending in a character the entry cannot supply: the match must fail after
    // exhausting the backtracks rather than running away.
    CHECK_EQ(find_sorted(nullptr, (evil + "*z.mesh").c_str()).size(), 0u);
    // The quadratic shape itself, both ways.
    CHECK_EQ(find_sorted(nullptr, ("base\\globevil\\*" + run + ".mesh").c_str()).size(), 1u);
    CHECK_EQ(find_sorted(nullptr, ("base\\globevil\\*" + run + "z.mesh").c_str()).size(), 0u);
}

TEST(find, an_absurdly_long_path_is_refused_not_interned) {
    // The dictionary is process-global, so one 100k-character entry would tax
    // every later redfs_find for the life of the process.
    //
    // Each half uses its OWN prefix: under _DEBUG the runner executes every case
    // three times in one process, so a case that asserts against a dictionary
    // its own earlier run populated passes on pass one and fails on pass two.
    redfs_path_enable();

    const std::string huge = "base\\globlong\\huge\\" + std::string(100000, 'a') + ".mesh";
    redfs_path_add(huge.c_str());
    CHECK_EQ(find_sorted(nullptr, "base\\globlong\\huge\\*").size(), 0u);

    // Just inside the limit still lands.
    const std::string ok = "base\\globlong\\ok\\" + std::string(900, 'b') + ".mesh";
    redfs_path_add(ok.c_str());
    CHECK_EQ(find_sorted(nullptr, "base\\globlong\\ok\\*").size(), 1u);
}

TEST(find, cpp_facade_both_overloads) {
    // Instantiates the templated overload with a NAMED callable, which is the
    // shape that trips MSVC C2528 if the trampoline forgets remove_reference_t
    // -- an inline lambda compiles either way, so only this catches it.
    ArchiveBuilder ab;
    const char* one = "base\\globfacade\\one.mesh";
    const char* two = "base\\globfacade\\two.mesh";
    ab.add(redfs_hash(one), {'1'});
    ab.add(redfs_hash(two), {'2'});

    const std::string p = temp_path("globfacade.archive");
    ArchiveBuilder::write(p, ab.build());

    redfs_depot* h = nullptr;
    redfs_depot_open_empty(&h);
    if (h) {
        CHECK_OK(redfs_depot_mount(h, p.c_str()));
        redfs::Depot d{h};  // takes ownership; ~Depot closes it

        redfs_path_enable();
        redfs_path_add(one);
        redfs_path_add(two);

        const auto all = d.find("base\\globfacade\\*.mesh");
        CHECK(all.has_value());
        if (all) CHECK_EQ(all->size(), 2u);

        int seen = 0;
        auto count = [&seen](uint64_t, std::string_view) {
            ++seen;
            return true;
        };
        const auto n = d.find("base\\globfacade\\*.mesh", count);
        CHECK(n.has_value());
        if (n) CHECK_EQ(*n, 2u);
        CHECK_EQ(seen, 2);

        // Returning false stops DELIVERY; the count is the total either way.
        auto stop_at_once = [](uint64_t, std::string_view) { return false; };
        const auto stopped = d.find("base\\globfacade\\*.mesh", stop_at_once);
        CHECK(stopped.has_value());
        if (stopped) CHECK_EQ(*stopped, 2u);

        // A std::string works without .c_str() now, and a failure is nullopt
        // rather than an empty vector that looks like "nothing matched".
        const std::string built = "base\\globfacade\\*.mesh";
        CHECK(d.find(built).has_value());
        CHECK(!d.find("").has_value());
    }

    // A moved-from Depot must not silently widen the query: a null handle is a
    // real mode in the C API ("search unfiltered"), so passing it through would
    // return MORE than the caller asked for rather than failing.
    redfs::Depot empty;
    CHECK(!empty.find("base\\globfacade\\*.mesh").has_value());
    auto never = [](uint64_t, std::string_view) { return true; };
    CHECK(!empty.find("base\\globfacade\\*.mesh", never).has_value());

    // That mode is still reachable, just not by accident. Both entries are in
    // the dictionary whether or not any depot holds them.
    const auto unfiltered = redfs::Depot::find_unfiltered("base\\globfacade\\*.mesh");
    CHECK(unfiltered.has_value());
    if (unfiltered) CHECK_EQ(unfiltered->size(), 2u);

    int seen_unfiltered = 0;
    auto tally = [&seen_unfiltered](uint64_t, std::string_view) {
        ++seen_unfiltered;
        return true;
    };
    CHECK(redfs::Depot::find_unfiltered("base\\globfacade\\*.mesh", tally).has_value());
    CHECK_EQ(seen_unfiltered, 2);

    // A CONST callable: `&fn` is a `const L*`, which will not convert to the
    // void* the C ABI takes without the const_cast in find_in. An inline lambda
    // compiles without it, so only a const named one catches its removal.
    const auto const_counter = [](uint64_t, std::string_view) { return true; };
    CHECK(redfs::Depot::find_unfiltered("base\\globfacade\\*.mesh", const_counter).has_value());
    const std::function<bool(uint64_t, std::string_view)> const_fn = const_counter;
    CHECK(redfs::Depot::find_unfiltered("base\\globfacade\\*.mesh", const_fn).has_value());
    std::remove(p.c_str());
}

// --- KARK path lists ---------------------------------------------------------
//
// These bound the header BEFORE Oodle is consulted, so they run on a machine
// with no oo2ext_7_win64.dll -- which is what makes them worth having, since
// nothing else in the suite covers read_list's compressed branch at all.

namespace {

// A KARK header claiming `declared` decompressed bytes over `payload_len` bytes
// of (garbage) compressed data. Never decodes; every case here is rejected, or
// expected to reach Oodle and fail there, before the payload is looked at.
std::string write_kark(const char* name, uint32_t declared, size_t payload_len) {
    const std::string path = temp_path(name);
    FILE* f = std::fopen(path.c_str(), "wb");
    const uint8_t header[8] = {'K',
                               'A',
                               'R',
                               'K',
                               static_cast<uint8_t>(declared),
                               static_cast<uint8_t>(declared >> 8),
                               static_cast<uint8_t>(declared >> 16),
                               static_cast<uint8_t>(declared >> 24)};
    std::fwrite(header, 1, sizeof header, f);
    const std::vector<uint8_t> payload(payload_len, 0xAB);
    std::fwrite(payload.data(), 1, payload.size(), f);
    std::fclose(f);
    return path;
}

}  // namespace

TEST(paths, oversized_kark_is_unsupported_not_corrupt) {
    ArchiveBuilder ab;
    ab.add(redfs_hash("base\\kark\\x.bin"), {'x'});
    TempDepot d("kark.archive", ab.build());
    if (!d.depot) return;

    // A 1 KB file claiming 2 GiB. The old absolute cap called this REDFS_E_CORRUPT
    // -- "corrupt data" for a header that is merely bigger than a constant, which
    // sends the caller to inspect the file instead of the limit.
    const std::string huge = write_kark("kark_huge.kark", 0x80000000u, 1024);
    CHECK_ERR(redfs_path_load(d.depot, huge.c_str(), nullptr), REDFS_E_UNSUPPORTED);
    std::remove(huge.c_str());

    // The ratio bound is what actually stops the cheap attack: nine bytes of
    // input may not commit a gigabyte of zero-filled allocation.
    const std::string bomb = write_kark("kark_bomb.kark", 1u << 30, 1);
    CHECK_ERR(redfs_path_load(d.depot, bomb.c_str(), nullptr), REDFS_E_UNSUPPORTED);
    std::remove(bomb.c_str());

    // Zero stays REDFS_E_CORRUPT: a KARK header naming no output is malformed,
    // not merely large.
    const std::string zero = write_kark("kark_zero.kark", 0, 64);
    CHECK_ERR(redfs_path_load(d.depot, zero.c_str(), nullptr), REDFS_E_CORRUPT);
    std::remove(zero.c_str());
}

TEST(paths, real_world_kark_ratios_are_accepted) {
    ArchiveBuilder ab;
    ab.add(redfs_hash("base\\kark\\y.bin"), {'y'});
    TempDepot d("kark_ok.archive", ab.build());
    if (!d.depot) return;

    // Every ratio WolvenKit actually ships, scaled down. The payload is garbage,
    // so passing the bound means reaching Oodle -- REDFS_E_OODLE either because
    // it is absent or because the junk does not decode. What must NOT come back
    // is REDFS_E_UNSUPPORTED, which would mean the header was refused unread.
    //
    // 87x is red.kark's real shape (417 MiB from 86 MB is 4.9x, and 178 MiB,
    // 204 MiB and 129 MiB lists run 26x, 54x and 40x) with margin to spare;
    // 200x is past anything real and still inside the bound.
    for (const uint32_t ratio : {5u, 27u, 54u, 87u, 200u}) {
        const size_t payload = 4096;
        const std::string p = write_kark("kark_ratio.kark",
                                         static_cast<uint32_t>(payload * ratio), payload);
        const redfs_status st = redfs_path_load(d.depot, p.c_str(), nullptr);
        CHECK(st == REDFS_E_OODLE);
        if (st != REDFS_E_OODLE)
            std::printf("         ratio %ux got %s\n", ratio, redfs_status_string(st));
        std::remove(p.c_str());
    }

    // And the far side of the bound is still refused.
    const std::string over = write_kark("kark_over.kark", 4096 * 257, 4096);
    CHECK_ERR(redfs_path_load(d.depot, over.c_str(), nullptr), REDFS_E_UNSUPPORTED);
    std::remove(over.c_str());

    // THE CASE THAT ACTUALLY DISCRIMINATES. Everything above declares under a
    // megabyte, three orders of magnitude below the 256 MB cap this fix
    // replaced -- so the old code would have accepted all of it and the test
    // could not catch a revert. red.kark's real shape is 417 MiB at 4.9x: a
    // ratio nothing would blink at, and a size the old cap rejected outright.
    // 300 MB at 4.9x reproduces that in a 61 MB file.
    const size_t payload = 62914560;  // 60 MiB
    const std::string big = write_kark("kark_redshape.kark", 300u * 1024 * 1024, payload);
    const redfs_status st = redfs_path_load(d.depot, big.c_str(), nullptr);
    CHECK(st == REDFS_E_OODLE);  // past the size bound, dies on the garbage payload
    if (st != REDFS_E_OODLE) std::printf("         got %s\n", redfs_status_string(st));
    std::remove(big.c_str());

    // Above the 512 MiB ceiling, refused whatever the ratio -- and the ceiling
    // is what bounds how much gets committed and zero-filled before Oodle is
    // consulted, so it is the number that matters.
    const std::string huge = write_kark("kark_ceiling.kark", 600u * 1024 * 1024, payload);
    CHECK_ERR(redfs_path_load(d.depot, huge.c_str(), nullptr), REDFS_E_UNSUPPORTED);
    std::remove(huge.c_str());
}

TEST(paths, the_length_bound_covers_every_intern_route) {
    // The bound started on paths_add alone -- the one route fed by host code --
    // while paths_load and paths_learn_imports, both fed by file content a mod
    // ships, interned without any check. It belongs in add_locked, where all
    // three meet.
    redfs_path_enable();
    const std::string run(4000, 'q');

    // Route 1: paths_add.
    redfs_path_add(("base\\lenbound\\add\\" + run + ".mesh").c_str());
    CHECK_EQ(find_sorted(nullptr, "base\\lenbound\\add\\*").size(), 0u);

    // Route 2: import learning, i.e. bytes out of an archive.
    Cr2wBuilder b;
    b.import(("base\\lenbound\\import\\" + run + ".mt").c_str(), "IMaterial");
    b.begin_chunk("Root");
    b.prop_u32("x", 1);
    b.end_chunk();
    with_cr2w(b.build(), [](redfs_cr2w*) {});
    CHECK_EQ(find_sorted(nullptr, "base\\lenbound\\import\\*").size(), 0u);

    // Route 3: a path list on disk.
    ArchiveBuilder ab;
    const std::string listed = "base\\lenbound\\list\\" + run + ".mesh";
    ab.add(redfs_hash(listed.c_str()), {'z'});
    TempDepot d("lenbound.archive", ab.build());
    if (!d.depot) return;

    const std::string list = temp_path("lenbound.txt");
    {
        FILE* f = std::fopen(list.c_str(), "wb");
        std::fprintf(f, "%s\n", listed.c_str());
        std::fclose(f);
    }
    uint32_t kept = 0;
    CHECK_OK(redfs_path_load(d.depot, list.c_str(), &kept));
    // It resolves in the depot -- so `kept` counts it -- but it must not be
    // interned, because the cost of holding it is paid by every later search.
    CHECK(redfs_path_from_hash(redfs_hash(listed.c_str())) == nullptr);
    std::remove(list.c_str());
}

TEST(paths, learned_from_imports) {
    // Reading a file with imports must teach the dictionary those paths.
    redfs_path_enable();

    Cr2wBuilder b;
    b.import("base\\learned\\from_import.mt", "IMaterial");
    b.begin_chunk("Root");
    b.prop_u32("x", 1);
    b.end_chunk();

    with_cr2w(b.build(), [](redfs_cr2w*) {});
    CHECK_STR(redfs_path_from_hash(redfs_hash("base\\learned\\from_import.mt")),
              "base\\learned\\from_import.mt");
}

TEST(paths, non_canonical_imports_resolve_under_the_canonical_hash) {
    // Import strings are archive content and are not guaranteed canonical. They
    // were hashed raw while paths_add and redfs_hash both sanitize, so a mixed
    // case or forward-slash import landed under a key redfs_hash could never
    // produce: the entry was permanently dead and the real path stayed
    // unresolvable.
    redfs_path_enable();

    Cr2wBuilder b;
    b.import("Base/Learned/MixedCase.mt", "IMaterial");
    b.begin_chunk("Root");
    b.prop_u32("x", 1);
    b.end_chunk();

    with_cr2w(b.build(), [](redfs_cr2w*) {});
    CHECK_STR(redfs_path_from_hash(redfs_hash("base\\learned\\mixedcase.mt")),
              "base\\learned\\mixedcase.mt");
}

TEST(paths, returned_pointer_survives_later_additions) {
    // redfs.h promises the returned string "stays valid for the lifetime of the
    // process". It used to be an interior pointer into a std::vector<char> that
    // any later insert could reallocate -- and the documented usage pattern
    // (resolve a hash to a path, then open that mesh, which parses a CR2W and
    // learns its imports) is exactly the sequence that invalidated it.
    //
    // Two successive lookups with nothing in between always looked fine, which
    // is why this survived. The failure needs an intervening add.
    redfs_path_enable();
    redfs_path_add("base\\stable\\first.mesh");

    const char* p = redfs_path_from_hash(redfs_hash("base\\stable\\first.mesh"));
    CHECK(p != nullptr);
    if (!p) return;

    // Comfortably more than enough to force repeated reallocation of any single
    // growing buffer.
    for (int i = 0; i < 20000; ++i) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "base\\stable\\f%d.mesh", i);
        redfs_path_add(buf);
    }

    // Against the old code this reads freed memory; under ASan it is a hard
    // failure rather than a lucky pass.
    CHECK_STR(p, "base\\stable\\first.mesh");
}

// =============================================================================
// API contracts
// =============================================================================

TEST(api, null_arguments_are_rejected) {
    // Every entry point must reject nulls rather than dereference them.
    redfs_depot* d = nullptr;
    CHECK_ERR(redfs_depot_open(nullptr, REDFS_SCAN_ALL, nullptr), REDFS_E_INVALID_ARG);
    CHECK_ERR(redfs_depot_mount(nullptr, "x"), REDFS_E_INVALID_ARG);
    CHECK_ERR(redfs_depot_mount_dir(nullptr, "x", nullptr), REDFS_E_INVALID_ARG);

    redfs_file_info info{};
    CHECK_ERR(redfs_stat(nullptr, 0, &info), REDFS_E_INVALID_ARG);
    CHECK_EQ(redfs_exists(nullptr, 0), 0);

    redfs_blob blob{};
    CHECK_ERR(redfs_read(nullptr, 0, REDFS_PART_ALL, &blob), REDFS_E_INVALID_ARG);
    CHECK_ERR(redfs_read(d, 0, REDFS_PART_ALL, nullptr), REDFS_E_INVALID_ARG);

    redfs_cr2w* f = nullptr;
    CHECK_ERR(redfs_cr2w_open(nullptr, 0, &f), REDFS_E_INVALID_ARG);

    redfs_texture_desc t{};
    CHECK_ERR(redfs_texture_desc_of(nullptr, 0, &t), REDFS_E_INVALID_ARG);
    redfs_mesh* m = nullptr;
    CHECK_ERR(redfs_mesh_open(nullptr, 0, &m), REDFS_E_INVALID_ARG);

    // These must tolerate null without crashing.
    redfs_depot_close(nullptr);
    redfs_cr2w_close(nullptr);
    redfs_mesh_close(nullptr);
    redfs_blob_free(nullptr);
    CHECK_EQ(redfs_depot_archive_count(nullptr), 0u);
    CHECK_STR(redfs_depot_archive_path(nullptr, 0), "");
    CHECK_EQ(redfs_cr2w_chunk_count(nullptr), 0u);
    CHECK_EQ(redfs_mesh_chunk_count(nullptr), 0u);
    CHECK(redfs_mesh_chunk_at(nullptr, 0) == nullptr);
}

TEST(api, double_free_is_safe) {
    ArchiveBuilder ab;
    const uint64_t key = redfs_hash("base\\dbl.bin");
    ab.add(key, {'x', 'y', 'z'});
    TempDepot d("dbl.archive", ab.build());
    if (!d.depot) return;

    redfs_blob blob{};
    CHECK_OK(redfs_read(d.depot, key, REDFS_PART_ALL, &blob));
    redfs_blob_free(&blob);
    redfs_blob_free(&blob);  // must be a no-op, not a double free
    CHECK(blob.data == nullptr);
    CHECK_EQ(blob.size, 0ull);
}

TEST(api, blob_is_nul_terminated) {
    // Text payloads should be usable as C strings without copying.
    ArchiveBuilder ab;
    const uint64_t key = redfs_hash("base\\text.json");
    const std::vector<uint8_t> json = {'{', '"', 'a', '"', ':', '1', '}'};
    ab.add(key, json);
    TempDepot d("text.archive", ab.build());
    if (!d.depot) return;

    redfs_blob blob{};
    CHECK_OK(redfs_read(d.depot, key, REDFS_PART_ALL, &blob));
    CHECK_EQ(blob.size, json.size());
    CHECK(blob.data && blob.data[blob.size] == 0);
    redfs_blob_free(&blob);
}

TEST(api, async_read_and_shutdown) {
    // The lifecycle a plugin actually follows: queue async work, then shut down
    // before the DLL can be unloaded. If shutdown did not join the worker, a
    // FreeLibrary on a statically-linked plugin would unmap running code.
    ArchiveBuilder ab;
    const uint64_t key = redfs_hash("base\\async.bin");
    const std::vector<uint8_t> payload(4096, 0x5A);
    ab.add(key, payload);
    TempDepot d("async.archive", ab.build());
    if (!d.depot) return;

    struct Ctx {
        redfs_status st = REDFS_E_IO;
        uint64_t size = 0;
        int calls = 0;
    } ctx;

    for (int i = 0; i < 8; ++i) {
        CHECK_OK(redfs_read_async(d.depot, key, REDFS_PART_ALL,
                                  [](redfs_status st, redfs_blob b, void* u) {
                                      auto* c = static_cast<Ctx*>(u);
                                      c->st = st;
                                      c->size = b.size;
                                      ++c->calls;
                                      redfs_blob_free(&b);
                                  },
                                  &ctx));
    }
    redfs_drain();
    CHECK_EQ(ctx.calls, 8);
    CHECK_EQ((int)ctx.st, (int)REDFS_OK);
    CHECK_EQ(ctx.size, payload.size());

    // Joins the worker. Must be safe to call twice, and must not wedge.
    redfs_shutdown();
    redfs_shutdown();

    // Every callback must have been accounted for -- none left hanging.
    CHECK_EQ(ctx.calls, 8);

    // Synchronous reads keep working afterwards -- shutdown stops the worker,
    // it does not invalidate a depot the caller still holds.
    redfs_blob blob{};
    CHECK_OK(redfs_read(d.depot, key, REDFS_PART_ALL, &blob));
    CHECK_EQ(blob.size, payload.size());
    redfs_blob_free(&blob);

    // Async work posted after shutdown is dropped rather than queued forever.
    redfs_drain();
}

TEST(api, out_of_range_part_is_rejected_not_wrapped) {
    // `part` is a caller-supplied uint32 and reaches `start + 1 + part` with no
    // validation. In 32-bit that wrapped: for a file whose segments begin at S,
    // part = 0xFFFFFFFF - S selected segment S - 1, so the caller got ANOTHER
    // FILE'S bytes back with REDFS_OK where redfs.h promises REDFS_E_RANGE.
    //
    // An earlier review round refuted this as unreachable, reasoning from the
    // cap on a `part` the library derives from CR2W. That bound is real but says
    // nothing about the four public entry points that take one from the caller.
    // A host passing a signed -3 through an FFI lands here without trying.
    // The wrap only reaches a VALID segment when the target index is at or below
    // start - 2: anything above that either exceeds `end` and is caught anyway,
    // or collides with REDFS_PART_MAIN / REDFS_PART_ALL, which are consumed
    // before this branch. So the first file needs enough segments of its own to
    // push the second file's start up. Getting this wrong makes the test pass
    // against the broken code -- it did, once.
    ArchiveBuilder ab;
    const uint64_t first_key = redfs_hash("base\\wrap\\first.bin");
    const uint64_t later_key = redfs_hash("base\\wrap\\later.bin");
    ab.add(first_key, std::vector<uint8_t>(64, 0xA1),
           {std::vector<uint8_t>(16, 0xA2), std::vector<uint8_t>(16, 0xA3)});  // segments 0,1,2
    ab.add(later_key, std::vector<uint8_t>(64, 0xB2),
           {std::vector<uint8_t>(32, 0xC3), std::vector<uint8_t>(32, 0xD4)});  // segments 3,4,5

    TempDepot d("wrap.archive", ab.build());
    if (!d.depot) return;

    redfs_file_info info{};
    CHECK_OK(redfs_stat(d.depot, later_key, &info));
    CHECK_EQ(info.buffer_count, 2u);

    // In range: fine.
    uint64_t sz = 0;
    CHECK_OK(redfs_part_size(d.depot, later_key, 0, &sz));
    const uint64_t own_buffer_size = sz;

    // Just past the end: the documented error.
    CHECK_ERR(redfs_part_size(d.depot, later_key, 2, &sz), REDFS_E_RANGE);

    // start = 3, so part = target - start - 1 wraps onto `target`. These select
    // segments 0, 1 and 2 -- all belonging to the OTHER file -- and every one of
    // them used to return REDFS_OK with that file's bytes.
    for (uint32_t target = 0; target <= 1; ++target) {
        const uint32_t part = target - 3u - 1u;  // wraps by construction
        CHECK_ERR(redfs_part_size(d.depot, later_key, part, &sz), REDFS_E_RANGE);
        redfs_blob b{};
        CHECK_ERR(redfs_read(d.depot, later_key, part, &b), REDFS_E_RANGE);
        // Nothing may have been handed back, least of all the other file's data.
        CHECK(b.data == nullptr);
        redfs_blob_free(&b);
    }

    // Sanity: the legitimate read still works and is unaffected by the above.
    CHECK_OK(redfs_part_size(d.depot, later_key, 0, &sz));
    CHECK_EQ(sz, own_buffer_size);
}

TEST(api, a_lying_segment_range_does_not_blow_up_stat_or_enumerate) {
    // fill_info took an entry's segment range straight from the index and bounded
    // the LOOP VARIABLE by segment_count rather than rejecting. So an entry
    // claiming last = 0xFFFFFFFF made one redfs_stat walk every segment in the
    // archive, and redfs_enumerate did that once per entry -- entry_count x
    // segment_count iterations, with no allocation and no fault to notice.
    // buffer_count came from the same unvalidated value and reported ~4.29e9,
    // which is the bound redfs.h documents for addressing buffers.
    //
    // Nothing could test this until ArchiveBuilder could write a range that lies.
    ArchiveBuilder ab;
    const uint64_t key = redfs_hash("base\\lying.bin");
    ab.add(key, std::vector<uint8_t>(128, 0x7E), {std::vector<uint8_t>(64, 0x7F)});
    ab.segment_range(0, 0, 0xFFFFFFFFu);

    TempDepot d("lying.archive", ab.build());
    if (!d.depot) return;

    const auto t0 = std::chrono::steady_clock::now();

    redfs_file_info info{};
    CHECK_OK(redfs_stat(d.depot, key, &info));
    // Clamped to what the archive actually holds, not what the entry claimed.
    CHECK(info.buffer_count < 16u);

    uint32_t seen = 0;
    redfs_enumerate(
        d.depot,
        [](const redfs_file_info*, void* u) {
            ++*static_cast<uint32_t*>(u);
            return 1;
        },
        &seen);
    CHECK_EQ(seen, 1u);

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    CHECK(ms < 1000);

    // And the read path rejects the same entry outright rather than clamping --
    // a file whose segment range is nonsense has no readable content.
    redfs_blob b{};
    CHECK_ERR(redfs_read(d.depot, key, REDFS_PART_MAIN, &b), REDFS_E_CORRUPT);
    redfs_blob_free(&b);
}

TEST(mesh, declared_array_counts_are_not_trusted) {
    // redfs_mesh_desc_of reported the count an array DECLARES; redfs_mesh_chunk_*
    // reported what could be walked. Two exports, one file, different answers --
    // and a caller looping to the first and indexing with the second walks off
    // the end of nothing, spinning four billion times against a null return.
    //
    // The fixture could only ever state the truth, so the distinction did not
    // exist as far as the suite was concerned.
    const uint32_t chunks = 3, verts = 6;
    fixture::MeshOverrides ov;
    ov.declared_chunk_count = 0xFFFFFFFFu;
    ov.declared_appearance_count = 0xFFFFFFFFu;
    ov.lod_count = 2;
    ov.declared_lod_count = 0xFFFFFFFFu;

    ArchiveBuilder ab;
    const uint64_t key = redfs_hash("base\\liars.mesh");
    ab.add(key, fixture::make_mesh_cr2w(chunks, verts, 1.0f, 0.0f, ov),
           {fixture::make_mesh_geometry(chunks, verts)});

    TempDepot d("liars.archive", ab.build());
    if (!d.depot) return;

    redfs_mesh_desc desc{};
    CHECK_OK(redfs_mesh_desc_of(d.depot, key, &desc));
    redfs_mesh* m = nullptr;
    CHECK_OK(redfs_mesh_open(d.depot, key, &m));

    // Neither export may repeat the file's claim.
    CHECK(desc.submesh_count < 0xFFFFFFFFu);
    CHECK(desc.appearance_count < 0xFFFFFFFFu);
    CHECK(redfs_mesh_lod_count(m) < 0xFFFFFFFFu);

    // And they must still agree with each other.
    CHECK_EQ(desc.submesh_count, redfs_mesh_chunk_count(m));
    CHECK_EQ(desc.appearance_count, redfs_mesh_appearance_count(m));
    CHECK_EQ(redfs_mesh_chunk_count(m), chunks);

    redfs_mesh_close(m);
}

TEST(mesh, an_impossible_position_stride_yields_no_bounds) {
    // compute_bounds refuses a stride under 6 -- it cannot hold three int16s --
    // and refuses a span that runs off the geometry buffer. Both guards were
    // unreachable while the fixture always wrote the stock stride of 8.
    //
    // The failure this prevents is a confident box swept from misaligned bytes,
    // which then gets cached and served forever.
    for (uint32_t stride : {4u, 0x01000000u}) {
        fixture::MeshOverrides ov;
        ov.position_stride = stride;

        ArchiveBuilder ab;
        const uint64_t key = redfs_hash("base\\stride.mesh");
        ab.add(key, fixture::make_mesh_cr2w(2, 6, 1.0f, 0.0f, ov),
               {fixture::make_mesh_geometry(2, 6, 8)});  // real buffer stays stock

        TempDepot d("stride.archive", ab.build());
        if (!d.depot) return;

        redfs_mesh* m = nullptr;
        CHECK_OK(redfs_mesh_open(d.depot, key, &m));
        for (uint32_t i = 0; i < redfs_mesh_chunk_count(m); ++i) {
            const redfs_mesh_chunk* c = redfs_mesh_chunk_at(m, i);
            CHECK(c != nullptr);
            // No box rather than a wrong one, and it says so.
            if (c) CHECK_EQ(c->bounds_valid, 0u);
        }
        redfs_mesh_close(m);
    }
}

TEST(mesh, entry_points_agree_about_the_same_facts) {
    // Two exports describing one file must not disagree. redfs_mesh_desc_of
    // reported DECLARED array counts straight from the CR2W while
    // redfs_mesh_chunk_count reported what actually walked -- a caller looping to
    // one and indexing with the other was the documented pattern.
    //
    // Review found that; no test could, because nothing cross-checked two APIs
    // against each other. This does.
    const uint32_t chunks = 4, verts = 8;
    ArchiveBuilder ab;
    const uint64_t key = redfs_hash("base\\agree.mesh");
    ab.add(key, fixture::make_mesh_cr2w(chunks, verts, 4.0f, 1.0f),
           {fixture::make_mesh_geometry(chunks, verts)});

    TempDepot d("agree.archive", ab.build());
    if (!d.depot) return;

    redfs_mesh_desc desc{};
    CHECK_OK(redfs_mesh_desc_of(d.depot, key, &desc));

    redfs_mesh* m = nullptr;
    CHECK_OK(redfs_mesh_open(d.depot, key, &m));

    CHECK_EQ(desc.submesh_count, redfs_mesh_chunk_count(m));
    CHECK_EQ(desc.appearance_count, redfs_mesh_appearance_count(m));

    // Every index the count promises must actually resolve, and one past it
    // must not -- the bound and the accessor have to come from the same vector.
    for (uint32_t i = 0; i < redfs_mesh_chunk_count(m); ++i)
        CHECK(redfs_mesh_chunk_at(m, i) != nullptr);
    CHECK(redfs_mesh_chunk_at(m, redfs_mesh_chunk_count(m)) == nullptr);

    // Chunk boxes marked valid must sit inside the whole-mesh box, which comes
    // from a different source entirely (CMesh vs. dequantized vertices).
    float lo[3], hi[3];
    redfs_mesh_bounds(m, lo, hi);
    const float eps = 1e-3f;
    for (uint32_t i = 0; i < redfs_mesh_chunk_count(m); ++i) {
        const redfs_mesh_chunk* c = redfs_mesh_chunk_at(m, i);
        if (!c || !c->bounds_valid) continue;
        for (int a = 0; a < 3; ++a) {
            CHECK(c->bbox_min[a] >= lo[a] - eps);
            CHECK(c->bbox_max[a] <= hi[a] + eps);
        }
    }
    redfs_mesh_close(m);
}

TEST(api, cache_round_trip_preserves_the_public_view) {
    // The cache is a second implementation of "what is this mesh", and the only
    // thing that made it agree with the first was that both were written by the
    // same person on the same day. Serialize, reload, and compare the PUBLIC
    // view field by field -- which is what a caller sees, and what the stale-
    // geometry and lod-clamp defects both corrupted.
    const uint32_t chunks = 3, verts = 6;
    ArchiveBuilder ab;
    const uint64_t key = redfs_hash("base\\roundtrip.mesh");
    ab.add(key, fixture::make_mesh_cr2w(chunks, verts, 2.5f, -1.0f),
           {fixture::make_mesh_geometry(chunks, verts)});

    const std::string p = temp_path("roundtrip.archive");
    const std::string cache_file = temp_path("roundtrip.cache");
    ArchiveBuilder::write(p, ab.build());
    std::remove(cache_file.c_str());

    struct Snap {
        uint32_t lods = 0;
        std::vector<redfs_mesh_chunk> chunks;
        std::vector<std::string> materials;
    };
    auto capture = [&](Snap* s) {
        redfs_depot* d = nullptr;
        redfs_depot_open_empty(&d);
        if (!d) return false;
        CHECK_OK(redfs_depot_mount(d, p.c_str()));
        CHECK_OK(redfs_cache_open(d, cache_file.c_str()));
        redfs_mesh* m = nullptr;
        CHECK_OK(redfs_mesh_open(d, key, &m));
        s->lods = redfs_mesh_lod_count(m);
        for (uint32_t i = 0; i < redfs_mesh_chunk_count(m); ++i) {
            s->chunks.push_back(*redfs_mesh_chunk_at(m, i));
            s->materials.push_back(redfs_mesh_chunk_material(m, 0, i));
        }
        redfs_mesh_close(m);
        CHECK_OK(redfs_cache_flush());
        redfs_cache_close();
        redfs_depot_close(d);
        return true;
    };

    Snap fresh, cached;
    if (capture(&fresh) && capture(&cached)) {
        // Second pass came off disk. Nothing a caller can observe may differ.
        CHECK_EQ(cached.lods, fresh.lods);
        CHECK_EQ(cached.chunks.size(), fresh.chunks.size());
        CHECK_EQ(cached.materials.size(), fresh.materials.size());
        for (size_t i = 0; i < fresh.chunks.size() && i < cached.chunks.size(); ++i) {
            const redfs_mesh_chunk& a = fresh.chunks[i];
            const redfs_mesh_chunk& b = cached.chunks[i];
            CHECK_EQ(b.index, a.index);
            CHECK_EQ(b.lod, a.lod);
            CHECK_EQ(b.vertex_count, a.vertex_count);
            CHECK_EQ(b.index_count, a.index_count);
            CHECK_EQ(b.bounds_valid, a.bounds_valid);
            for (int ax = 0; ax < 3; ++ax) {
                CHECK_EQ(b.bbox_min[ax], a.bbox_min[ax]);
                CHECK_EQ(b.bbox_max[ax], a.bbox_max[ax]);
            }
        }
        for (size_t i = 0; i < fresh.materials.size() && i < cached.materials.size(); ++i)
            CHECK_STR(cached.materials[i].c_str(), fresh.materials[i].c_str());
    }
    std::remove(p.c_str());
    std::remove(cache_file.c_str());
}

namespace {
struct CloseFromCb {
    redfs_depot* depot;
    std::atomic<int> calls{0};
    std::atomic<bool> closed{false};
};
}  // namespace

TEST(api, closing_a_depot_from_its_own_callback_still_purges_the_queue) {
    // cancel_for bailed out entirely when called on the worker thread, reasoning
    // about the IN-FLIGHT job -- which is indeed this one -- and thereby skipping
    // the queue purge, which is the whole point of the call. Every other job
    // queued against the depot stayed in the queue holding a raw pointer to it,
    // and redfs_depot_close then deleted it.
    //
    // Only the wait needs skipping on the worker thread. The purge does not.
    ArchiveBuilder ab;
    const uint64_t key = redfs_hash("base\\cbclose.bin");
    ab.add(key, std::vector<uint8_t>(48 * 1024, 0x2D));
    const std::string p = temp_path("cbclose.archive");
    ArchiveBuilder::write(p, ab.build());

    redfs_depot* d = nullptr;
    redfs_depot_open_empty(&d);
    if (d) {
        CHECK_OK(redfs_depot_mount(d, p.c_str()));

        CloseFromCb ctx;
        ctx.depot = d;
        for (int i = 0; i < 32; ++i) {
            redfs_read_async(d, key, REDFS_PART_ALL,
                             [](redfs_status, redfs_blob b, void* u) {
                                 auto* c = static_cast<CloseFromCb*>(u);
                                 c->calls.fetch_add(1);
                                 redfs_blob_free(&b);
                                 // The first callback closes the depot the
                                 // remaining queued jobs still point at.
                                 if (!c->closed.exchange(true)) redfs_depot_close(c->depot);
                             },
                             &ctx);
        }

        // Every callback must still resolve exactly once, and nothing may touch
        // the depot after it is gone.
        for (int spin = 0; spin < 400 && ctx.calls.load() < 32; ++spin)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK_EQ(ctx.calls.load(), 32);
    }
    std::remove(p.c_str());
}

TEST(api, cpp_facade_accepts_named_callables) {
    // redfs.hpp's for_each and Cr2w::walk took Fn&& and then did
    // static_cast<Fn*>. For an lvalue callable Fn deduces to L&, and `L&*` is not
    // a type -- MSVC rejects it with C2528. So passing a named lambda did not
    // compile while an inline one did.
    //
    // It went unnoticed because nothing in the tree instantiated either template:
    // the C++ facade had no test coverage at all. This test exists mostly to be
    // COMPILED -- if the deduction regresses, the build breaks here.
    ArchiveBuilder ab;
    const uint64_t key = redfs_hash("base\\facade.bin");
    ab.add(key, std::vector<uint8_t>(32, 0x11));
    const std::string p = temp_path("facade.archive");
    ArchiveBuilder::write(p, ab.build());

    redfs_depot* h = nullptr;
    redfs_depot_open_empty(&h);
    if (h) {
        CHECK_OK(redfs_depot_mount(h, p.c_str()));
        redfs::Depot d{h};  // takes ownership; ~Depot closes it

        int seen = 0;
        // The whole point: a NAMED callable, not a temporary.
        auto count_files = [&seen](const redfs::FileInfo&) {
            ++seen;
            return true;
        };
        d.for_each(count_files);
        CHECK_EQ(seen, 1);

        // And the ABI handshake the facade now performs on open.
        CHECK(redfs::abi_ok());
    }
    std::remove(p.c_str());
}

TEST(api, closing_a_depot_resolves_its_queued_reads) {
    // A queued Job holds a RAW depot pointer and the worker dereferences it, so
    // redfs_depot_close deleting the depot out from under the queue was a
    // use-after-free: ~redfs_depot unmaps every archive index while the worker is
    // reading one.
    //
    // Documenting "drain first" would not have been enough -- redfs::Depot's
    // destructor calls redfs_depot_close, so the unsafe order is what the C++
    // facade does by default.
    //
    // Under ASan against the old code this is a hard failure. Without ASan it is
    // a nondeterministic one, which is why it needs to exist rather than being
    // argued about.
    ArchiveBuilder ab;
    const uint64_t key = redfs_hash("base\\closerace.bin");
    ab.add(key, std::vector<uint8_t>(64 * 1024, 0x3C));
    const std::string p = temp_path("closerace.archive");
    ArchiveBuilder::write(p, ab.build());

    redfs_depot* d = nullptr;
    redfs_depot_open_empty(&d);
    if (d) {
        CHECK_OK(redfs_depot_mount(d, p.c_str()));

        std::atomic<int> calls{0};
        // Deep enough that the worker cannot possibly have finished them all
        // before the close below.
        for (int i = 0; i < 64; ++i) {
            redfs_read_async(d, key, REDFS_PART_ALL,
                             [](redfs_status, redfs_blob b, void* u) {
                                 static_cast<std::atomic<int>*>(u)->fetch_add(1);
                                 redfs_blob_free(&b);
                             },
                             &calls);
        }

        // No drain, no shutdown. Straight to close, exactly as ~Depot does.
        redfs_depot_close(d);

        // Every callback must still have fired exactly once -- cancelled counts,
        // dropped does not. redfs.h promises exactly-once for anything that
        // returned REDFS_OK.
        CHECK_EQ(calls.load(), 64);
    }
    std::remove(p.c_str());
}

TEST(api, shutdown_cancels_queued_work) {
    // Shutdown must be bounded by the read already in flight, not by however
    // much the caller queued -- draining a deep queue at game close would look
    // like a hang. Queued-but-unstarted jobs are reported as cancelled so no
    // caller is left waiting on a callback that never arrives.
    ArchiveBuilder ab;
    const uint64_t key = redfs_hash("base\\bulk.bin");
    ab.add(key, std::vector<uint8_t>(64 * 1024, 0x33));
    TempDepot d("bulk.archive", ab.build());
    if (!d.depot) return;

    struct Ctx {
        int completed = 0;
        int cancelled = 0;
    } ctx;

    auto cb = [](redfs_status st, redfs_blob b, void* u) {
        auto* c = static_cast<Ctx*>(u);
        if (st == REDFS_E_CANCELLED)
            ++c->cancelled;
        else if (st == REDFS_OK)
            ++c->completed;
        redfs_blob_free(&b);
    };

    constexpr int kJobs = 200;
    for (int i = 0; i < kJobs; ++i)
        redfs_read_async(d.depot, key, REDFS_PART_ALL, cb, &ctx);

    redfs_shutdown();

    // The contract: every job resolves exactly once, completed or cancelled, and
    // no callback is silently dropped. Which side of the split a given job lands
    // on is timing -- even the in-flight one may abort at a segment boundary
    // rather than finish, which is the point of the abort token.
    CHECK_EQ(ctx.completed + ctx.cancelled, kJobs);
    CHECK(ctx.cancelled > 0);  // the queue was dropped, not drained

    // Shutdown quiesces rather than disables: a later post starts a fresh
    // worker. That matters when RedFS.dll is shared -- one plugin unloading must
    // not permanently break async for the others.
    Ctx after;
    redfs_read_async(d.depot, key, REDFS_PART_ALL, cb, &after);
    redfs_drain();
    CHECK_EQ(after.completed, 1);

    redfs_shutdown();  // leave the suite quiesced for the next test
}

TEST(api, oodle_load_is_retried_not_poisoned) {
    // Regression: load() used std::call_once, so the FIRST attempt decided the
    // outcome forever. redfs_depot_open_empty calls it with no game directory,
    // which outside the game resolves nothing -- and a later redfs_depot_open
    // with the real install path could then never retry, leaving every
    // compressed read failing with REDFS_E_OODLE for the process lifetime.
    //
    // The whole suite already runs open_empty first (TempDepot does), so by this
    // point a poisoned gate would have latched. Assert it did not: either Oodle
    // is genuinely absent on this machine, or it resolved -- what must not happen
    // is resolving being made impossible by an early argument-less call.
    redfs_depot* d = nullptr;
    CHECK_OK(redfs_depot_open_empty(&d));
    CHECK(d != nullptr);
    if (d) redfs_depot_close(d);

    // Idempotent and side-effect free: querying must not change the answer.
    const int first = redfs_oodle_available();
    const int second = redfs_oodle_available();
    CHECK_EQ(first, second);
}

// --- regressions from the adversarial review of api.cpp / archive.cpp --------

TEST(api, mesh_handle_survives_cache_close) {
    // A cached mesh used to be owned solely by the cache: redfs_shutdown ->
    // cache_close -> entries.clear() destroyed it while callers still held the
    // pointer, and redfs_mesh_close was a silent no-op on it. Every accessor was
    // then a use-after-free. The handle now holds its own reference.
    ArchiveBuilder ab;
    const uint64_t key = redfs_hash("base\\cachelife.mesh");
    ab.add(key, fixture::make_mesh_cr2w(3, 6, 4.0f, 0.0f),
           {fixture::make_mesh_geometry(3, 6)});
    TempDepot d("cachelife.archive", ab.build());
    if (!d.depot) return;

    const std::string cache_file = temp_path("lifetime.cache");
    std::remove(cache_file.c_str());
    CHECK_OK(redfs_cache_open(d.depot, cache_file.c_str()));

    redfs_mesh* m = nullptr;
    CHECK_OK(redfs_mesh_open(d.depot, key, &m));
    if (!m) return;
    const uint32_t chunks_before = redfs_mesh_chunk_count(m);
    const redfs_mesh_chunk* c = redfs_mesh_chunk_at(m, 0);
    CHECK(c != nullptr);
    const float z_before = c ? c->bbox_min[2] : 0.f;

    // Pull the cache out from under the live handle.
    redfs_cache_close();

    // Everything must still read correctly -- under ASan this is where a
    // use-after-free would fire.
    CHECK_EQ(redfs_mesh_chunk_count(m), chunks_before);
    const redfs_mesh_chunk* after = redfs_mesh_chunk_at(m, 0);
    CHECK(after != nullptr);
    if (after) CHECK_NEAR(after->bbox_min[2], z_before, 1e-6);
    CHECK_STR(redfs_mesh_appearance_name(m, 0), "default");

    redfs_mesh_close(m);
    std::remove(cache_file.c_str());
}

TEST(api, concurrent_mesh_open_is_safe) {
    // The suite had NO threading tests, which is why a data race on the cached
    // mesh's public_chunks vector survived every other check. redfs.h lists
    // redfs_mesh_* as concurrency-safe and INTEGRATION.md describes two threads
    // racing on the same mesh as supported, so this is the documented path.
    ArchiveBuilder ab;
    const uint64_t key = redfs_hash("base\\race.mesh");
    ab.add(key, fixture::make_mesh_cr2w(6, 8, 3.0f, 0.0f),
           {fixture::make_mesh_geometry(6, 8)});
    TempDepot d("race.archive", ab.build());
    if (!d.depot) return;

    const std::string cache_file = temp_path("race.cache");
    std::remove(cache_file.c_str());
    CHECK_OK(redfs_cache_open(d.depot, cache_file.c_str()));

    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 200; ++i) {
                redfs_mesh* m = nullptr;
                if (redfs_mesh_open(d.depot, key, &m) != REDFS_OK || !m) {
                    ++failures;
                    continue;
                }
                if (redfs_mesh_chunk_count(m) != 6) ++failures;
                const redfs_mesh_chunk* c = redfs_mesh_chunk_at(m, 5);
                if (!c || c->index != 5) ++failures;
                redfs_mesh_close(m);
            }
        });
    }
    for (auto& th : threads) th.join();
    CHECK_EQ(failures.load(), 0);

    redfs_cache_close();
    std::remove(cache_file.c_str());
}

TEST(api, mount_invalidates_the_mesh_cache) {
    // The fingerprint was computed only in cache_open, so mounting afterwards
    // kept serving pre-mount geometry -- and flushed new entries under the stale
    // fingerprint, poisoning the file for later runs. redfs.h promises "a game
    // patch or a new mod cannot serve stale geometry".
    const uint64_t key = redfs_hash("base\\swap.mesh");

    ArchiveBuilder base;
    base.add(key, fixture::make_mesh_cr2w(2, 6, 1.0f, 0.0f),
             {fixture::make_mesh_geometry(2, 6)});
    // The override has a different chunk count, so a stale answer is unmistakable.
    ArchiveBuilder patch;
    patch.add(key, fixture::make_mesh_cr2w(5, 6, 1.0f, 0.0f),
              {fixture::make_mesh_geometry(5, 6)});

    const std::string p1 = temp_path("swap_base.archive");
    const std::string p2 = temp_path("swap_patch.archive");
    ArchiveBuilder::write(p1, base.build());
    ArchiveBuilder::write(p2, patch.build());

    redfs_depot* d = nullptr;
    redfs_depot_open_empty(&d);
    if (d) {
        CHECK_OK(redfs_depot_mount(d, p1.c_str()));

        const std::string cache_file = temp_path("swap.cache");
        std::remove(cache_file.c_str());
        CHECK_OK(redfs_cache_open(d, cache_file.c_str()));

        redfs_mesh* m = nullptr;
        CHECK_OK(redfs_mesh_open(d, key, &m));
        CHECK_EQ(redfs_mesh_chunk_count(m), 2u);
        redfs_mesh_close(m);

        // Mount an override AFTER the cache was opened.
        CHECK_OK(redfs_depot_mount(d, p2.c_str()));

        m = nullptr;
        CHECK_OK(redfs_mesh_open(d, key, &m));
        CHECK_EQ(redfs_mesh_chunk_count(m), 5u);  // must reflect the override
        redfs_mesh_close(m);

        redfs_cache_close();
        redfs_depot_close(d);
        std::remove(cache_file.c_str());
    }
    std::remove(p1.c_str());
    std::remove(p2.c_str());
}

TEST(api, cache_notices_an_archive_replaced_in_place) {
    // The fingerprint mixed path + entry count + index size, and none of those
    // move when an archive's FILES change but its SHAPE does not. Re-cook a mesh,
    // repack with the same file and segment counts, and every input was
    // byte-identical -- so the cache validated and kept serving the old geometry,
    // across restarts, with nothing in the log. redfs.h says a game patch or a
    // new mod cannot serve stale geometry; this is the case that could.
    //
    // The sibling test above only covers ADDING an archive, where a new path
    // enters the mix. That is why this survived the whole suite.
    const uint64_t key = redfs_hash("base\\recook.mesh");

    // Identical structure -- same chunk count, same vertex count, hence the same
    // number of segments at the same sizes. Only the quantization scale differs,
    // which is what moves the bounds.
    ArchiveBuilder v1;
    v1.add(key, fixture::make_mesh_cr2w(3, 6, 1.0f, 0.0f), {fixture::make_mesh_geometry(3, 6)});
    ArchiveBuilder v2;
    v2.add(key, fixture::make_mesh_cr2w(3, 6, 4.0f, 0.0f), {fixture::make_mesh_geometry(3, 6)});

    const std::vector<uint8_t> bytes1 = v1.build();
    const std::vector<uint8_t> bytes2 = v2.build();
    // The premise: same length, different content. If these ever diverge in size
    // the test has stopped exercising the bug it was written for.
    CHECK_EQ(bytes1.size(), bytes2.size());
    CHECK(bytes1 != bytes2);

    const std::string p = temp_path("recook.archive");
    const std::string cache_file = temp_path("recook.cache");
    std::remove(cache_file.c_str());

    float first_max = 0.f;
    ArchiveBuilder::write(p, bytes1);
    {
        redfs_depot* d = nullptr;
        redfs_depot_open_empty(&d);
        if (d) {
            CHECK_OK(redfs_depot_mount(d, p.c_str()));
            CHECK_OK(redfs_cache_open(d, cache_file.c_str()));
            redfs_mesh* m = nullptr;
            CHECK_OK(redfs_mesh_open(d, key, &m));
            const redfs_mesh_chunk* c = redfs_mesh_chunk_at(m, 0);
            CHECK(c != nullptr);
            if (c) first_max = c->bbox_max[0];
            redfs_mesh_close(m);
            CHECK_OK(redfs_cache_flush());  // persist under v1's fingerprint
            redfs_cache_close();
            redfs_depot_close(d);
        }
    }
    CHECK(first_max != 0.f);

    // Replace the archive in place, exactly as a mod update or a re-cook does.
    ArchiveBuilder::write(p, bytes2);
    {
        redfs_depot* d = nullptr;
        redfs_depot_open_empty(&d);
        if (d) {
            CHECK_OK(redfs_depot_mount(d, p.c_str()));
            CHECK_OK(redfs_cache_open(d, cache_file.c_str()));
            redfs_mesh* m = nullptr;
            CHECK_OK(redfs_mesh_open(d, key, &m));
            const redfs_mesh_chunk* c = redfs_mesh_chunk_at(m, 0);
            CHECK(c != nullptr);
            // Before the index CRC joined the fingerprint, this handed back v1's
            // box straight out of the cache file.
            if (c) CHECK(c->bbox_max[0] != first_max);
            redfs_mesh_close(m);
            redfs_cache_close();
            redfs_depot_close(d);
        }
    }
    std::remove(p.c_str());
    std::remove(cache_file.c_str());
}

namespace {
struct ReQueue {
    redfs_depot* depot;
    uint64_t key;
    std::atomic<int> cancelled{0};
    std::atomic<int> completed{0};
    std::atomic<int> refused{0};
};

void requeue_cb(redfs_status st, redfs_blob b, void* user) {
    auto* ctx = static_cast<ReQueue*>(user);
    if (st == REDFS_E_CANCELLED)
        ++ctx->cancelled;
    else if (st == REDFS_OK)
        ++ctx->completed;
    redfs_blob_free(&b);

    // The dangerous pattern: chain the next read from inside a callback. During
    // shutdown this used to spawn a thread AFTER the join, so shutdown returned
    // with a live worker. It must be refused via the RETURN VALUE -- and not by
    // invoking this same callback again, which recurses until the stack dies.
    // That is exactly what this test caught on the first attempt at the fix.
    if (redfs_read_async(ctx->depot, ctx->key, REDFS_PART_ALL, requeue_cb, ctx) != REDFS_OK)
        ++ctx->refused;
}
}  // namespace

TEST(api, callback_requeue_during_shutdown_is_refused_not_dropped) {
    ArchiveBuilder ab;
    const uint64_t key = redfs_hash("base\\chain.bin");
    ab.add(key, std::vector<uint8_t>(32 * 1024, 0x77));
    TempDepot d("chain.archive", ab.build());
    if (!d.depot) return;

    ReQueue ctx{d.depot, key};
    for (int i = 0; i < 64; ++i)
        redfs_read_async(d.depot, key, REDFS_PART_ALL, requeue_cb, &ctx);

    // Must return with the worker joined, even though callbacks re-post.
    redfs_shutdown();

    // Every job resolved one way or another; nothing was silently dropped.
    const int total = ctx.completed + ctx.cancelled;
    CHECK(total >= 64);
    // And a re-post during shutdown was refused rather than swallowed.
    CHECK(ctx.refused > 0);

    // The worker must be gone: a fresh post now starts a new one cleanly.
    redfs_drain();
}

TEST(api, drain_and_shutdown_from_a_callback_do_not_deadlock) {
    // busy_ is cleared only after the callback returns, so both drain() and
    // shutdown() would wait on a flag only the calling thread can clear -- and
    // shutdown would additionally join itself. Both must refuse instead.
    ArchiveBuilder ab;
    const uint64_t key = redfs_hash("base\\reentrant.bin");
    ab.add(key, std::vector<uint8_t>(1024, 0x22));
    TempDepot d("reentrant.archive", ab.build());
    if (!d.depot) return;

    std::atomic<int> calls{0};
    struct Ctx {
        std::atomic<int>* calls;
    } ctx{&calls};

    redfs_read_async(d.depot, key, REDFS_PART_ALL,
                     [](redfs_status, redfs_blob b, void* user) {
                         redfs_blob_free(&b);
                         // If these deadlock, the test hangs rather than fails.
                         redfs_drain();
                         redfs_shutdown();
                         ++*static_cast<Ctx*>(user)->calls;
                     },
                     &ctx);

    redfs_drain();
    CHECK_EQ(calls.load(), 1);
    redfs_shutdown();
}

TEST(api, status_strings_exist) {
    const redfs_status all[] = {REDFS_OK,          REDFS_E_NOT_FOUND, REDFS_E_IO,
                                REDFS_E_CORRUPT,   REDFS_E_OODLE,     REDFS_E_INVALID_ARG,
                                REDFS_E_OOM,       REDFS_E_UNSUPPORTED, REDFS_E_RANGE,
                                REDFS_E_CANCELLED};
    for (redfs_status s : all) {
        const char* str = redfs_status_string(s);
        CHECK(str != nullptr && str[0] != '\0');
    }
    CHECK_EQ(redfs_abi_version(), (uint32_t)REDFS_ABI_VERSION);
}
