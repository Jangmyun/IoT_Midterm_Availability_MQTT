#include "connection_table_manager.h"
#include <cstring>
#include <ctime>
#include <cstdio>

// Util =====================================================

// Curren Time 을 문자열로
static void set_now(char* buf) {
    std::time_t t = std::time(nullptr);
    struct tm* utc = std::gmtime(&t);
    std::strftime(buf, TIMESTAMP_LEN, "%Y-%m-%dT%H:%M:%SZ", utc);
}

// Public

// Initialize ConnectionTable - memory allocation, set current time
void ConnectionTableManager::init(const char* active_core_id, const char* backup_core_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::memset(&table_, 0, sizeof(table_));
    set_now(table_.last_update);
    std::strncpy(table_.active_core_id, active_core_id, UUID_LEN - 1);
    if (backup_core_id && backup_core_id[0] != '\0') {
        std::strncpy(table_.backup_core_id, backup_core_id, UUID_LEN - 1);
    }
}
