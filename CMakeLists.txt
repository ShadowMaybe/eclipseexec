cmake_minimum_required(VERSION 3.22.1)

project(eclipseexec
        VERSION 0.1.0
        LANGUAGES C
        DESCRIPTION "Eclipse Launcher")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)          # bionic's __system_property_get and friends

# Warnings are errors here on purpose: this library is small enough to hold in
# one head and every warning it produces is a real bug waiting for a device.
add_compile_options(-Wall -Wextra -Wshadow -Werror)

set(ECLIPSE_SOURCES
        src/eclipse_exec.c
        src/eclipse_jni.c
        src/eclipse_vulkan.c
        src/eclipse_affinity.c
        src/eclipse_namespace.c
        src/eclipse_elf.c
        src/eclipse_maps.c)

if(CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    list(APPEND ECLIPSE_SOURCES src/eclipse_arm64.c)
endif()

add_library(eclipseexec SHARED ${ECLIPSE_SOURCES})

# Consumers (an SDL fork, a launcher's own JNI code) include the public header
# through this target, so it has to be PUBLIC.
target_include_directories(eclipseexec
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        PRIVATE src)

target_link_libraries(eclipseexec PRIVATE log dl)

target_compile_definitions(eclipseexec PRIVATE _GNU_SOURCE)

target_link_options(eclipseexec PRIVATE
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/version.script"
        "-Wl,--no-undefined")

set_target_properties(eclipseexec PROPERTIES
        LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/version.script"
        OUTPUT_NAME eclipseexec)

# The Turnip path needs the android_dlopen_ext interposer, which only makes
# sense where the Turnip loader itself is built: arm64-v8a.
if(CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    target_compile_definitions(eclipseexec PRIVATE ECLIPSE_HAVE_TURNIP=1)

    add_library(eclipsehook SHARED src/eclipse_hook.c)
    target_include_directories(eclipsehook PRIVATE src)
    target_compile_definitions(eclipsehook PRIVATE ECLIPSE_HAVE_TURNIP=1)
    # The fallback path in this file calls dlopen(), and it logs.
    target_link_libraries(eclipsehook PRIVATE log dl)
    # -z global keeps android_dlopen_ext visible to the driver loaded after it,
    # which is the whole point of this library existing. The version script
    # pins the surface to the four entries check_exports.sh allows, so a stray
    # symbol cannot pass as part of the contract — and none of the linker's
    # synthetic _end/_edata/__bss_start can show up as an export either.
    target_link_options(eclipsehook PRIVATE
            "-Wl,-z,global"
            "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/version_hook.script"
            "-Wl,--no-undefined")
    set_target_properties(eclipsehook PROPERTIES
            LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/version_hook.script"
            OUTPUT_NAME eclipsehook)
endif()
