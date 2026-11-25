// ShaderFileCache.hpp -- compiled shader code kept in files between sessions (standard library only: tested by
// tests/shader_file_cache_test.cpp without Windows).
//
// [SHADER FILES fix110 20.11.2026] RE4 log of 20.11.2026 20:18: the eye pass's shader took 29.5 s to compile, its 16 x 16
// text 21.3 s more and the one with the tiles' bits 20.1 s more: 71 s at every start of the game for this one shader,
// with the picture stopped (the compiled code was kept only for the session, FoveaComposite.cpp's compile_cached). The
// code depends only on the source text, the entry point, the target, the flags and the macros: it is now written to a
// file named after a hash of all of those, and read back in the next sessions. A build that changes a shader's text
// gets other names; files not used for 30 days are removed.
//
// A file: 8 bytes "RE4VRSC1", the code's size (8 bytes), the code's FNV-1a hash (8 bytes), the code. A file that is
// short, long, of another kind or with another hash is not used (the shader is compiled again and the file rewritten).
#pragma once

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace d3d12::shader_files {

inline uint64_t fnv64(const void* data, size_t size, uint64_t hash = 14695981039346656037ull) {
    const auto* p = (const uint8_t*)data;

    for (size_t i = 0; i < size; ++i) {
        hash ^= p[i];
        hash *= 1099511628211ull;
    }

    return hash;
}

// the file's name for a key (the whole of what the code depends on): two hashes of it, 32 hexadecimal digits
inline std::string file_name(std::string_view key) {
    const uint64_t a = fnv64(key.data(), key.size());
    const uint64_t b = fnv64(key.data(), key.size(), 0x9E3779B97F4A7C15ull ^ (uint64_t)key.size());
    static const char* const HEX = "0123456789abcdef";
    std::string name(32, '0');

    for (int i = 0; i < 16; ++i) {
        name[(size_t)i] = HEX[(a >> (60 - 4 * i)) & 15];
        name[(size_t)(16 + i)] = HEX[(b >> (60 - 4 * i)) & 15];
    }

    return name + ".cso";
}

inline constexpr char MAGIC[8] = {'R', 'E', '4', 'V', 'R', 'S', 'C', '1'};
inline constexpr size_t HEADER_SIZE = 24;
inline constexpr size_t MAX_CODE_SIZE = 64u * 1024u * 1024u;

// the code of a file, or false (no file, or not one of ours, or damaged)
inline bool load(const std::filesystem::path& file, std::vector<uint8_t>& code) {
    code.clear();

    try {
        std::error_code ec{};
        const auto size = std::filesystem::file_size(file, ec);

        if (ec || size < HEADER_SIZE + 1 || size > HEADER_SIZE + MAX_CODE_SIZE) {
            return false;
        }

        std::ifstream in{file, std::ios::binary};

        if (!in) {
            return false;
        }

        char header[HEADER_SIZE]{};
        in.read(header, (std::streamsize)HEADER_SIZE);

        if (!in || std::memcmp(header, MAGIC, sizeof(MAGIC)) != 0) {
            return false;
        }

        uint64_t code_size = 0;
        uint64_t code_hash = 0;
        std::memcpy(&code_size, header + 8, 8);
        std::memcpy(&code_hash, header + 16, 8);

        if (code_size != size - HEADER_SIZE) {
            return false;
        }

        code.resize((size_t)code_size);
        in.read((char*)code.data(), (std::streamsize)code.size());

        if (!in || in.gcount() != (std::streamsize)code.size() || fnv64(code.data(), code.size()) != code_hash) {
            code.clear();
            return false;
        }

        return true;
    } catch (...) {
        code.clear();
        return false;
    }
}

// writes the code beside its final name and renames it, so that a file with the final name is always whole
inline bool save(const std::filesystem::path& file, const void* code, size_t size, std::string_view unique) {
    if (code == nullptr || size == 0 || size > MAX_CODE_SIZE) {
        return false;
    }

    try {
        std::error_code ec{};
        std::filesystem::create_directories(file.parent_path(), ec);
        auto temp = file;
        temp += ".";
        temp += unique;
        temp += ".tmp";

        {
            std::ofstream out{temp, std::ios::binary | std::ios::trunc};

            if (!out) {
                return false;
            }

            char header[HEADER_SIZE]{};
            const uint64_t code_size = size;
            const uint64_t code_hash = fnv64(code, size);
            std::memcpy(header, MAGIC, sizeof(MAGIC));
            std::memcpy(header + 8, &code_size, 8);
            std::memcpy(header + 16, &code_hash, 8);
            out.write(header, (std::streamsize)HEADER_SIZE);
            out.write((const char*)code, (std::streamsize)size);
            out.flush();

            if (!out) {
                out.close();
                std::filesystem::remove(temp, ec);
                return false;
            }
        }

        std::filesystem::rename(temp, file, ec);

        if (ec) {
            // (the final name is in use or already there: the copy is not needed)
            std::error_code ignored{};
            std::filesystem::remove(temp, ignored);
            return std::filesystem::exists(file, ignored);
        }

        return true;
    } catch (...) {
        return false;
    }
}

// a file that was used counts as new again
inline void touch(const std::filesystem::path& file) {
    std::error_code ec{};
    std::filesystem::last_write_time(file, std::filesystem::file_time_type::clock::now(), ec);
}

// removes the folder's .cso files not written (or used) for max_age, and every .tmp left over; the number removed
inline int prune(const std::filesystem::path& dir, std::chrono::hours max_age) {
    int removed = 0;

    try {
        std::error_code ec{};
        const auto now = std::filesystem::file_time_type::clock::now();

        for (std::filesystem::directory_iterator it{dir, ec}, end{}; !ec && it != end; it.increment(ec)) {
            std::error_code e2{};

            if (!it->is_regular_file(e2) || e2) {
                continue;
            }

            const auto ext = it->path().extension().string();
            const bool temp = ext == ".tmp";

            if (!temp && ext != ".cso") {
                continue;
            }

            const auto written = it->last_write_time(e2);

            if (e2) {
                continue;
            }

            // (a .tmp of this very moment may belong to another thread: only those older than an hour)
            if (now - written > (temp ? std::chrono::hours{1} : max_age)) {
                if (std::filesystem::remove(it->path(), e2) && !e2) {
                    ++removed;
                }
            }
        }
    } catch (...) {
    }

    return removed;
}

} // namespace d3d12::shader_files
