#pragma once

#include "Arduino.h"
#include <memory>
#include <cstring>
#include <cstdio>
#include <utility>
#include <algorithm>
#include <type_traits>
#include <optional>
#include <string_view>

/** Auxiliary functions for using Unique Pointers in ESP32 PSRAM **/

// PSRAM Deleter (ps_malloc → free)
struct PsramDeleter {
    void operator()(void* ptr) const {
        if (ptr) {
            free(ptr);
        }
    }
};

// Auxiliary function: Comparison of two strings case insensitive, only up to n characters
inline int strncasecmp_local(const char* s1, const char* s2, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        unsigned char c1 = static_cast<unsigned char>(s1[i]);
        unsigned char c2 = static_cast<unsigned char>(s2[i]);
        if (tolower(c1) != tolower(c2)) return tolower(c1) - tolower(c2);
        if (c1 == '\0') break;
    }
    return 0;
}

template <typename T>
class ps_ptr {
private:
    std::unique_ptr<T[], PsramDeleter> mem;
    size_t allocated_size = 0;
    static inline T dummy{}; // For invalid accesses

public:
    ps_ptr() = default;

    ~ps_ptr() {
        if (mem) {
            // log_w("Destructor called: Freeing %d bytes at %p", allocated_size * sizeof(T), mem.get());
        } else {
            // log_w("Destructor called: No memory to free.");
        }
    }

    ps_ptr(ps_ptr&& other) noexcept {
        mem = std::move(other.mem);
        allocated_size = other.allocated_size;
        other.allocated_size = 0;
    }

    ps_ptr& operator=(ps_ptr&& other) noexcept {
        if (this != &other) {
            mem = std::move(other.mem);
            allocated_size = other.allocated_size;
            other.allocated_size = 0;
        }
        return *this;
    }

    ps_ptr(const ps_ptr&) = delete;
    ps_ptr& operator=(const ps_ptr&) = delete;

    void alloc(std::size_t size, const char* name = nullptr) {
        size = (size + 15) & ~15; // Align to 16 bytes
        mem.reset(static_cast<T*>(ps_malloc(size)));
        allocated_size = size;
        if (!mem) {
            printf("OOM: failed to allocate %zu bytes for %s\n", size, name ? name : "unnamed");
        }
    }

    void alloc(const char* name = nullptr) {
        reset();
        void* raw_mem = ps_malloc(sizeof(T));
        if (raw_mem) {
            mem.reset(new (raw_mem) T());
            allocated_size = sizeof(T);
        } else {
            printf("OOM: failed to allocate %zu bytes for %s\n", sizeof(T), name ? name : "unnamed");
            allocated_size = 0;
        }
    }

    void clear() {
        if (mem && allocated_size > 0) {
            std::memset(mem.get(), 0, allocated_size);
        }
    }

    size_t size() const { return allocated_size; }

    bool valid() const { return mem != nullptr; }

    void reset() {
        mem.reset();
        allocated_size = 0;
    }

    T* get() const { return static_cast<T*>(mem.get()); }

    T& operator[](std::size_t index) {
        if (index >= allocated_size) {
            log_e("ps_ptr[]: Index %zu out of bounds (size = %zu)", index, allocated_size);
            return dummy;
        }
        return mem[index];
    }

    const T& operator[](std::size_t index) const {
        if (index >= allocated_size) {
            log_e("ps_ptr[] (const): Index %zu out of bounds (size = %zu)", index, allocated_size);
            return dummy;
        }
        return mem[index];
    }
};
