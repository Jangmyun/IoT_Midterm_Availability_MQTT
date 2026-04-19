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

inline uint64_t uuid_mix64(uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

inline uint64_t uuid_hash_seed(const std::string& seed, uint64_t salt) {
    uint64_t hash = 1469598103934665603ULL ^ salt;
    for (unsigned char ch : seed) {
        hash ^= static_cast<uint64_t>(ch);
        hash *= 1099511628211ULL;
        hash = uuid_mix64(hash);
    }
    return hash;
}

inline void uuid_format(uint64_t hi, uint64_t lo, char out[UUID_LEN], uint16_t version) {
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | (static_cast<uint64_t>(version & 0xF) << 12);
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    std::snprintf(out, UUID_LEN,
        "%08x-%04x-%04x-%04x-%04x%08x",
        static_cast<uint32_t>(hi >> 32),
        static_cast<uint32_t>((hi >> 16) & 0xFFFF),
        static_cast<uint32_t>(hi         & 0xFFFF),
        static_cast<uint32_t>(lo >> 48),
        static_cast<uint32_t>((lo >> 32) & 0xFFFF),
        static_cast<uint32_t>(lo         & 0xFFFFFFFF));
}

// char 배열 버전 — struct 필드(char id[UUID_LEN]) 직접 채울 때
inline void uuid_generate(char out[UUID_LEN]) {
    thread_local std::mt19937_64 eng(std::random_device{}());
    std::uniform_int_distribution<uint64_t> dist;

    uint64_t hi = dist(eng);
    uint64_t lo = dist(eng);

    uuid_format(hi, lo, out, 4);
}

// endpoint 같은 고정 seed에 대해 동일한 UUID를 생성한다.
// PRD의 "배포 후 IP/Port 고정" 전제를 edge/node 식별 안정화에 활용한다.
inline void uuid_generate_deterministic(const std::string& seed, char out[UUID_LEN]) {
    uint64_t hi = uuid_hash_seed(seed, 0x6a09e667f3bcc909ULL);
    uint64_t lo = uuid_hash_seed(seed, 0xbb67ae8584caa73bULL);
    uuid_format(hi, lo, out, 5);
}

// std::string 버전 — MqttMessage 생성 등 임시값으로 쓸 때
inline std::string uuid_generate() {
    char buf[UUID_LEN];
    uuid_generate(buf);
    return std::string(buf);
}

inline std::string uuid_generate_deterministic(const std::string& seed) {
    char buf[UUID_LEN];
    uuid_generate_deterministic(seed, buf);
    return std::string(buf);
}
