cmake_minimum_required(VERSION 3.20)
project(mqtt_broker VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Platform: macOS Homebrew prefix ──────────────────────────────────────────
# Ubuntu/Raspberry Pi OS 에서는 시스템 기본 경로를 사용하므로 아무 영향 없음
if(APPLE)
    execute_process(
        COMMAND brew --prefix
        OUTPUT_VARIABLE HOMEBREW_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(HOMEBREW_PREFIX)
        set(ENV{PKG_CONFIG_PATH} "${HOMEBREW_PREFIX}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    endif()
endif()

# ── Dependencies ──────────────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)

# IMPORTED_TARGET: PkgConfig::<NAME> 타겟 자동 생성 (include/link/cflags 일괄 포함)
pkg_check_modules(MOSQUITTO REQUIRED IMPORTED_TARGET libmosquitto)
# mosquitto 2.x 가 cJSON 을 헤더에서 전이 노출하므로 명시적으로 연결
pkg_check_modules(CJSON     REQUIRED IMPORTED_TARGET libcjson)

include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

# ── Common library (shared by core & edge) ────────────────────────────────────
add_library(broker_common STATIC
    src/common/mqtt_json.cpp
    src/common/connection_table_manager.cpp
)
target_include_directories(broker_common PUBLIC include)
target_link_libraries(broker_common PUBLIC nlohmann_json::nlohmann_json)

# ── Targets ───────────────────────────────────────────────────────────────────
add_executable(core_broker src/core/main.cpp)
add_executable(edge_broker src/edge/main.cpp)

foreach(TARGET core_broker edge_broker)
    target_link_libraries(${TARGET} PRIVATE
        broker_common
        PkgConfig::MOSQUITTO
        PkgConfig::CJSON
    )
endforeach()

# ── Tests ─────────────────────────────────────────────────────────────────────
enable_testing()
add_executable(test_json test/test_json.cpp)
target_link_libraries(test_json PRIVATE broker_common)
add_test(NAME test_json COMMAND test_json)

add_executable(test_core_logic test/test_core_logic.cpp)
target_link_libraries(test_core_logic PRIVATE broker_common)
add_test(NAME test_core_logic COMMAND test_core_logic)

add_executable(test_edge_logic test/test_edge_logic.cpp)
target_link_libraries(test_edge_logic PRIVATE broker_common)
add_test(NAME test_edge_logic COMMAND test_edge_logic)

add_executable(test_publisher_logic test/test_publisher_logic.cpp)
target_link_libraries(test_publisher_logic PRIVATE broker_common)
add_test(NAME test_publisher_logic COMMAND test_publisher_logic)

# ── Publisher simulator ───────────────────────────────────────────────────────
add_executable(pub_sim src/publisher/main.cpp)
target_link_libraries(pub_sim PRIVATE
    broker_common
    PkgConfig::MOSQUITTO
    PkgConfig::CJSON
)
