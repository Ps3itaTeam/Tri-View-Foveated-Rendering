// Tests of src/mods/vr/d3d12/ShaderFileCache.hpp (the compiled shaders kept in files, fix110). No Windows needed:
//   g++ -std=c++20 -Wall -Wextra -I src/mods/vr/d3d12 tests/shader_file_cache_test.cpp -o /tmp/sfc_test && /tmp/sfc_test
#include <cstdio>
#include <string>
#include <vector>

#include "ShaderFileCache.hpp"

namespace sf = d3d12::shader_files;
namespace fs = std::filesystem;

static int g_failures = 0;

#define CHECK(x)                                               \
    do {                                                       \
        if (!(x)) {                                            \
            std::printf("FAIL line %d: %s\n", __LINE__, #x);   \
            ++g_failures;                                      \
        }                                                      \
    } while (0)

static std::vector<uint8_t> bytes(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);

    for (size_t i = 0; i < n; ++i) {
        v[i] = (uint8_t)(seed + i * 31 + (i >> 8));
    }

    return v;
}

static std::vector<char> whole(const fs::path& p) {
    std::ifstream in{p, std::ios::binary};
    return std::vector<char>{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

static void put(const fs::path& p, const std::vector<char>& v) {
    std::ofstream out{p, std::ios::binary | std::ios::trunc};
    out.write(v.data(), (std::streamsize)v.size());
}

int main() {
    const fs::path dir = fs::temp_directory_path() / "re4vr_shader_file_cache_test" / "nested";
    std::error_code ec{};
    fs::remove_all(dir.parent_path(), ec);

    // names: 32 hexadecimal digits and ".cso"; another key, another name; the same key, the same name
    const std::string key_a = std::string(100000, 'x') + std::string("\0main\0cs_5_0\0TSX=16", 19);
    const std::string key_b = std::string(100000, 'x') + std::string("\0main\0cs_5_0\0TSX=32", 19);
    const auto name_a = sf::file_name(key_a);
    CHECK(name_a.size() == 36 && name_a.substr(32) == ".cso");
    CHECK(name_a.find_first_not_of("0123456789abcdef") == 32);
    CHECK(name_a != sf::file_name(key_b));
    CHECK(name_a == sf::file_name(key_a));

    // a file that is not there
    std::vector<uint8_t> got{};
    CHECK(!sf::load(dir / name_a, got) && got.empty());

    // written (the folder is made) and read back, byte for byte; no temporary file left
    const auto code = bytes(211428, 7);
    CHECK(sf::save(dir / name_a, code.data(), code.size(), "t1"));
    CHECK(sf::load(dir / name_a, got) && got == code);
    int files = 0;

    for (const auto& e : fs::directory_iterator{dir}) {
        ++files;
        CHECK(e.path().extension() == ".cso");
    }

    CHECK(files == 1);

    // written again over itself (two threads compiled the same shader)
    CHECK(sf::save(dir / name_a, code.data(), code.size(), "t2"));
    CHECK(sf::load(dir / name_a, got) && got == code);

    // nothing to write
    CHECK(!sf::save(dir / "empty.cso", code.data(), 0, "t1"));
    CHECK(!sf::save(dir / "null.cso", nullptr, 10, "t1"));

    // damaged files are refused: cut short, one byte longer, one byte of the code changed, one byte of the header changed,
    // another kind of file, an empty file
    const auto good = whole(dir / name_a);
    CHECK(good.size() == code.size() + sf::HEADER_SIZE);

    auto cut = good;
    cut.resize(cut.size() - 100);
    put(dir / "cut.cso", cut);
    CHECK(!sf::load(dir / "cut.cso", got) && got.empty());

    auto longer = good;
    longer.push_back(0);
    put(dir / "longer.cso", longer);
    CHECK(!sf::load(dir / "longer.cso", got));

    auto flipped = good;
    flipped[sf::HEADER_SIZE + 1000] ^= 0x40;
    put(dir / "flipped.cso", flipped);
    CHECK(!sf::load(dir / "flipped.cso", got) && got.empty());

    auto header = good;
    header[17] ^= 1;
    put(dir / "header.cso", header);
    CHECK(!sf::load(dir / "header.cso", got));

    auto other = good;
    other[0] = 'D';
    put(dir / "other.cso", other);
    CHECK(!sf::load(dir / "other.cso", got));

    put(dir / "nothing.cso", {});
    CHECK(!sf::load(dir / "nothing.cso", got));
    put(dir / "tiny.cso", std::vector<char>(sf::HEADER_SIZE, 0));
    CHECK(!sf::load(dir / "tiny.cso", got));

    // a folder in the place of the file
    fs::create_directories(dir / "folder.cso", ec);
    CHECK(!sf::load(dir / "folder.cso", got));

    // pruning: old .cso and old .tmp go, new ones and other files stay; a used (touched) file counts as new
    const auto old = fs::file_time_type::clock::now() - std::chrono::hours{24 * 40};
    CHECK(sf::save(dir / "old.cso", code.data(), 1000, "t1"));
    CHECK(sf::save(dir / "used.cso", code.data(), 1000, "t1"));
    put(dir / "left.cso.t9.tmp", good);
    put(dir / "new.cso.t9.tmp", good);
    put(dir / "notes.txt", good);
    fs::last_write_time(dir / "old.cso", old, ec);
    fs::last_write_time(dir / "used.cso", old, ec);
    fs::last_write_time(dir / "left.cso.t9.tmp", old, ec);
    fs::last_write_time(dir / "notes.txt", old, ec);
    sf::touch(dir / "used.cso");
    const int removed = sf::prune(dir, std::chrono::hours{24 * 30});
    CHECK(removed == 2);
    CHECK(!fs::exists(dir / "old.cso") && !fs::exists(dir / "left.cso.t9.tmp"));
    CHECK(fs::exists(dir / "used.cso") && fs::exists(dir / "new.cso.t9.tmp") && fs::exists(dir / "notes.txt") && fs::exists(dir / name_a));
    CHECK(sf::load(dir / name_a, got) && got == code);

    // a folder that is not there
    CHECK(sf::prune(dir / "missing", std::chrono::hours{1}) == 0);

    fs::remove_all(dir.parent_path(), ec);

    if (g_failures == 0) {
        std::printf("ALL TESTS OK\n");
        return 0;
    }

    std::printf("%d FAILURES\n", g_failures);
    return 1;
}
