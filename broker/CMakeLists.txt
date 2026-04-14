cmake_minimum_required(VERSION 3.20)
project(mqtt_broker VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Dependencies ──────────────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(MOSQUITTO REQUIRED libmosquitto)

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
        ${MOSQUITTO_LIBRARIES}
    )
    target_include_directories(${TARGET} PRIVATE ${MOSQUITTO_INCLUDE_DIRS})
    target_link_directories(${TARGET} PRIVATE ${MOSQUITTO_LIBRARY_DIRS})
    target_compile_options(${TARGET} PRIVATE ${MOSQUITTO_CFLAGS_OTHER})
endforeach()
