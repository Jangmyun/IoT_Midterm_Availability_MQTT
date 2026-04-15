#pragma once

// RFC 4122 v4 UUID generator
//
// Core approach adapted from sole (r-lyeh, zlib/libpng license):
//   https://github.com/r-lyeh-archived/sole
//
// Entropy source: std::random_device
//   Linux        → getrandom(2) syscall
//   macOS        → arc4random_buf
//   Raspberry Pi → getrandom(2) (Linux ≥ 3.17)
//
// Thread safety: thread_local engine — no mutex needed

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include "connection_table.h"   // UUID_LEN

// char 배열 버전 — struct 필드(char id[UUID_LEN]) 직접 채울 때
inline void uuid_generate(char out[UUID_LEN]) {
    thread_local std::mt19937_64 eng(std::random_device{}());
    std::uniform_int_distribution<uint64_t> dist;

    uint64_t hi = dist(eng);
    uint64_t lo = dist(eng);

    // version 4: b6[7:4] = 0100  →  hi bits [15:12]
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    // variant 10xx: b8[7:6] = 10  →  lo bits [63:62]
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    // "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx"  (36 chars + '\0' = UUID_LEN 37)
    std::snprintf(out, UUID_LEN,
        "%08x-%04x-%04x-%04x-%04x%08x",
        static_cast<uint32_t>(hi >> 32),
        static_cast<uint32_t>((hi >> 16) & 0xFFFF),
        static_cast<uint32_t>(hi         & 0xFFFF),   // version 4 포함
        static_cast<uint32_t>(lo >> 48),               // variant 포함
        static_cast<uint32_t>((lo >> 32) & 0xFFFF),
        static_cast<uint32_t>(lo         & 0xFFFFFFFF));
}

// std::string 버전 — MqttMessage 생성 등 임시값으로 쓸 때
inline std::string uuid_generate() {
    char buf[UUID_LEN];
    uuid_generate(buf);
    return std::string(buf);
}
