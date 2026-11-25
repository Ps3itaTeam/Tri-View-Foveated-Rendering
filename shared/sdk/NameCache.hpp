// NameCache.hpp -- [CPU 04.11.2026] A per-thread front for the type database's lookups by name.
//
// RETypeDefinition::get_method / get_field keep every result in a global map, behind a shared lock and keyed by a hash
// of the name's text. Every managed call by name (the mods' call_safe / try_call, sdk::call_object_func, the Lua scripts'
// obj:call) pays for hashing the name, taking the lock (from many threads at once: the main thread, the GUI draws on
// the workers, the camera hooks) and two map lookups, each time. A name passed again from the same place usually is the
// same text at the same address (a string literal): this table finds it by the owner and the name's address and length,
// compares the text, and only calls the full lookup when the slot holds something else. The result is exactly what the
// full lookup returns (it keeps its results for good, including "not found"); a name that does not fit in a slot always
// takes the full lookup.
//
// One table per thread (no lock); portable (no engine types), tested in tests/quadview_math_test.cpp.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace sdk::namecache {
template <typename V, size_t SLOTS = 1024, size_t TEXT = 48>
class Table {
public:
    static_assert(SLOTS != 0 && (SLOTS & (SLOTS - 1)) == 0, "SLOTS must be a power of two");

    // the value for (owner, name): from the slot, or from `lookup()` (then kept in the slot). keep_empty false: a V{}
    // result is not kept (for a full lookup that does not keep "not found" itself and might find it later)
    template <typename F>
    V get(const void* owner, std::string_view name, F&& lookup, bool keep_empty = true) {
        if (owner == nullptr || name.size() > TEXT) {
            return lookup();
        }

        auto& s = m_slots[index(owner, name)];

        if (s.owner == owner && s.ptr == name.data() && s.len == (uint32_t)name.size()
            && (name.empty() || std::memcmp(s.text, name.data(), name.size()) == 0)) {
            ++m_hits;
            return s.value;
        }

        V v = lookup();
        ++m_misses;

        if (!keep_empty && v == V{}) {
            return v;
        }

        s.owner = owner;
        s.ptr = name.data();
        s.len = (uint32_t)name.size();

        if (!name.empty()) {
            std::memcpy(s.text, name.data(), name.size());
        }

        s.value = v;
        return v;
    }

    uint64_t hits() const { return m_hits; }
    uint64_t misses() const { return m_misses; }

private:
    struct Slot {
        const void* owner{nullptr};
        const char* ptr{nullptr};
        uint32_t len{0};
        V value{};
        char text[TEXT]{};
    };

    static size_t index(const void* owner, std::string_view name) {
        uint64_t h = (uint64_t)(uintptr_t)owner * 0x9E3779B97F4A7C15ull;
        h ^= (uint64_t)(uintptr_t)name.data() * 0xC2B2AE3D27D4EB4Full;
        h ^= (uint64_t)name.size() * 0x165667B19E3779F9ull;
        h ^= h >> 31;
        h *= 0xD6E8FEB86659FD93ull;
        h ^= h >> 32;
        return (size_t)(h & (SLOTS - 1));
    }

    std::array<Slot, SLOTS> m_slots{};
    uint64_t m_hits{0};
    uint64_t m_misses{0};
};
} // namespace sdk::namecache
