cmake_minimum_required(VERSION 3.20)
project(mqtt_broker VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Dependencies ──────────────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(MOSQUITTO REQUIRED libmosquitto)

# ── Include paths ─────────────────────────────────────────────────────────────
include_directories(include)

# ── Sources ───────────────────────────────────────────────────────────────────
file(GLOB_RECURSE BROKER_SOURCES "src/*.c" "src/*.cpp")

# ── Targets ───────────────────────────────────────────────────────────────────
add_executable(core_broker  src/core/main.cpp)
add_executable(edge_broker  src/edge/main.cpp)

foreach(TARGET core_broker edge_broker)
    target_include_directories(${TARGET} PRIVATE include ${MOSQUITTO_INCLUDE_DIRS})
    target_link_libraries(${TARGET} PRIVATE ${MOSQUITTO_LIBRARIES})
    target_compile_options(${TARGET} PRIVATE ${MOSQUITTO_CFLAGS_OTHER})
endforeach()
