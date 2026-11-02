cmake_minimum_required(VERSION 3.21)
project(rerinku_device VERSION 0.5.0 LANGUAGES C)

include(CTest)
include(GNUInstallDirs)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON) # gnu11: the bundled C libraries rely on POSIX/GNU declarations
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Build products (the Rerinku Device SDK):
#   rerinku_device          static library for in-tree consumers
#                           (add_subdirectory + rerinku::device).
#   rerinku_device_shared   librerinku_device.so: exports only rerinku_*; the
#                           bundled dependencies stay internal, so they cannot
#                           clash with the firmware's own mbedTLS, cJSON, ...
#   rerinku_device_bundle   the installed librerinku_device.a: the SDK and every
#                           dependency in one archive, only rerinku_* global.
# Installed alongside: headers, a CMake package (find_package(rerinku_device)),
# a pkg-config file (rerinku_device.pc), examples and docs.
option(RERINKU_BUILD_SHARED "Build librerinku_device.so" ON)
option(RERINKU_BUILD_EXAMPLES "Build the example programs" ON)
option(RERINKU_BUILD_TESTS "Build the unit tests" ON)

# Every dependency is plain C, pinned, and built from source with the same
# toolchain as the SDK; nothing is taken from the host.
#   mbedTLS    DTLS-SRTP keying, HTTPS, MQTT over TLS
#   libjuice   ICE (STUN/TURN)
#   libsrtp    SRTP/SRTCP
#   coreMQTT   MQTT 3.1.1 client (no heap, no threads)
#   cJSON      JSON
include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(RERINKU_REQUESTED_BUILD_TESTING ${BUILD_TESTING})
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

# --- mbedTLS ---------------------------------------------------------------------
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
# A trimmed configuration: only what the SDK uses (see the file).
set(MBEDTLS_CONFIG_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/mbedtls_config.h" CACHE FILEPATH "" FORCE)
set(MBEDTLS_USER_CONFIG_FILE "" CACHE FILEPATH "" FORCE)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    mbedtls
    GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
    GIT_TAG        v3.6.2
    GIT_SHALLOW    ON
)

# --- libjuice (ICE) ----------------------------------------------------------------
set(NO_SERVER ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    libjuice
    GIT_REPOSITORY https://github.com/paullouisageneau/libjuice.git
    GIT_TAG        v1.6.2
    GIT_SHALLOW    ON
)

# --- libsrtp ------------------------------------------------------------------------
# Its built-in AES-CM + HMAC-SHA1 cover the one profile WebRTC requires
# (SRTP_AES128_CM_HMAC_SHA1_80); no external crypto backend needed.
set(LIBSRTP_TEST_APPS OFF CACHE BOOL "" FORCE)
set(ENABLE_OPENSSL OFF CACHE BOOL "" FORCE)
set(ENABLE_MBEDTLS OFF CACHE BOOL "" FORCE)
set(ENABLE_NSS OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_WARNINGS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    libsrtp
    GIT_REPOSITORY https://github.com/cisco/libsrtp.git
    GIT_TAG        v2.7.0
    GIT_SHALLOW    ON
)

# --- coreMQTT and cJSON: sources only -------------------------------------------------
# A handful of C files compiled straight into the SDK. SOURCE_SUBDIR points at a
# directory without a CMakeLists.txt so their own build scripts never run.
FetchContent_Declare(
    coremqtt
    GIT_REPOSITORY https://github.com/FreeRTOS/coreMQTT.git
    GIT_TAG        v2.3.1
    GIT_SHALLOW    ON
    SOURCE_SUBDIR  rerinku-sources-only
)
FetchContent_Declare(
    cjson
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    GIT_TAG        v1.7.18
    GIT_SHALLOW    ON
    SOURCE_SUBDIR  rerinku-sources-only
)
FetchContent_MakeAvailable(mbedtls libjuice libsrtp coremqtt cjson)

set(BUILD_TESTING ${RERINKU_REQUESTED_BUILD_TESTING} CACHE BOOL "" FORCE)

# --- rerinku ---------------------------------------------------------------------------
set(RERINKU_SOURCES
    src/bitstream.c
    src/common.c
    src/device.c
    src/dtls.c
    src/http.c
    src/log.c
    src/mqtt.c
    src/net.c
    src/peer.c
    src/rtp.c
    src/sdp.c
    src/whip.c
    ${coremqtt_SOURCE_DIR}/source/core_mqtt.c
    ${coremqtt_SOURCE_DIR}/source/core_mqtt_serializer.c
    ${coremqtt_SOURCE_DIR}/source/core_mqtt_state.c
    ${cjson_SOURCE_DIR}/cJSON.c
)

find_package(Threads REQUIRED)

# 0.x: every minor release may break the ABI, so the soname carries it
# (librerinku_device.so.0.5); from 1.0 on only the major does.
if(PROJECT_VERSION_MAJOR EQUAL 0)
    set(RERINKU_SOVERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
else()
    set(RERINKU_SOVERSION "${PROJECT_VERSION_MAJOR}")
endif()

# <rerinku/rerinku_version.h>: the version the headers describe.
set(RERINKU_GENERATED_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/include)
configure_file(cmake/rerinku_version.h.in
    ${RERINKU_GENERATED_INCLUDE}/rerinku/rerinku_version.h @ONLY)

function(rerinku_configure target)
    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<BUILD_INTERFACE:${RERINKU_GENERATED_INCLUDE}>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${coremqtt_SOURCE_DIR}/source/include
            ${coremqtt_SOURCE_DIR}/source/interface
            ${cjson_SOURCE_DIR})
    target_compile_definitions(${target} PRIVATE
        _GNU_SOURCE
        MQTT_DO_NOT_USE_CUSTOM_CONFIG
        RERINKU_BUILDING_LIBRARY)
    # Only what RERINKU_API marks is visible outside the library.
    set_target_properties(${target} PROPERTIES C_VISIBILITY_PRESET hidden)
    target_link_libraries(${target} PRIVATE
        LibJuice::LibJuiceStatic srtp2 mbedtls mbedx509 mbedcrypto Threads::Threads)
    # Let the linker drop what the SDK and its dependencies do not use.
    target_compile_options(${target} PRIVATE -ffunction-sections -fdata-sections
        -Wall -Wextra -Wno-unused-parameter)
endfunction()

add_library(rerinku_device STATIC ${RERINKU_SOURCES})
add_library(rerinku::device ALIAS rerinku_device)
rerinku_configure(rerinku_device)
# A static consumer links the bundled dependencies too.
target_link_libraries(rerinku_device PUBLIC
    $<BUILD_INTERFACE:LibJuice::LibJuiceStatic>
    $<BUILD_INTERFACE:srtp2>
    $<BUILD_INTERFACE:mbedtls> $<BUILD_INTERFACE:mbedx509> $<BUILD_INTERFACE:mbedcrypto>
    Threads::Threads)

if(RERINKU_BUILD_SHARED)
    add_library(rerinku_device_shared SHARED ${RERINKU_SOURCES})
    add_library(rerinku::device_shared ALIAS rerinku_device_shared)
    rerinku_configure(rerinku_device_shared)
    set_target_properties(rerinku_device_shared PROPERTIES
        OUTPUT_NAME rerinku_device
        VERSION ${PROJECT_VERSION}
        SOVERSION ${RERINKU_SOVERSION})
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # Self-contained .so: bundled static dependencies are not re-exported,
        # and the version script keeps only rerinku_* visible.
        target_link_options(rerinku_device_shared PRIVATE
            -Wl,--exclude-libs,ALL
            -Wl,--gc-sections
            -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/rerinku.map)
        set_property(TARGET rerinku_device_shared APPEND PROPERTY
            LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cmake/rerinku.map)
    endif()
endif()

# --- the static library firmware vendors link -------------------------------------------
# One archive holding the SDK and every bundled dependency, merged into a
# single relocatable object whose only global symbols are rerinku_*: the
# firmware links one file, and its own mbedTLS / cJSON / ... cannot clash with
# ours. Section-level dead-stripping (--gc-sections) still applies at the
# firmware's final link.
set(RERINKU_BUNDLE ${CMAKE_CURRENT_BINARY_DIR}/bundle/librerinku_device.a)
set(RERINKU_BUNDLE_LIBS
    $<TARGET_FILE:rerinku_device> $<TARGET_FILE:juice-static> $<TARGET_FILE:srtp2>
    $<TARGET_FILE:mbedtls> $<TARGET_FILE:mbedx509> $<TARGET_FILE:mbedcrypto>)
set(RERINKU_BUNDLE_DEPS rerinku_device juice-static srtp2 mbedtls mbedx509 mbedcrypto)
foreach(extra p256m everest)
    if(TARGET ${extra})
        list(APPEND RERINKU_BUNDLE_LIBS $<TARGET_FILE:${extra}>)
        list(APPEND RERINKU_BUNDLE_DEPS ${extra})
    endif()
endforeach()
add_custom_command(
    OUTPUT ${RERINKU_BUNDLE}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/bundle
    COMMAND ${CMAKE_C_COMPILER} -r -nostdlib -Wl,-d
            -o ${CMAKE_CURRENT_BINARY_DIR}/bundle/rerinku_device.o
            -Wl,--whole-archive ${RERINKU_BUNDLE_LIBS} -Wl,--no-whole-archive
    COMMAND ${CMAKE_OBJCOPY} --wildcard --keep-global-symbol=rerinku_*
            ${CMAKE_CURRENT_BINARY_DIR}/bundle/rerinku_device.o
    COMMAND ${CMAKE_COMMAND} -E rm -f ${RERINKU_BUNDLE}
    COMMAND ${CMAKE_AR} rcs ${RERINKU_BUNDLE} ${CMAKE_CURRENT_BINARY_DIR}/bundle/rerinku_device.o
    DEPENDS ${RERINKU_BUNDLE_DEPS}
    COMMENT "Bundling librerinku_device.a (SDK + dependencies, only rerinku_* exported)"
    VERBATIM)
add_custom_target(rerinku_device_bundle ALL DEPENDS ${RERINKU_BUNDLE})

if(RERINKU_BUILD_EXAMPLES)
    # The integration a firmware vendor writes: plain C against the public header.
    add_executable(rerinku_quickstart examples/quickstart/quickstart.c)
    target_link_libraries(rerinku_quickstart PRIVATE rerinku::device)
    add_executable(rerinku_device_example examples/device_example.c)
    target_link_libraries(rerinku_device_example PRIVATE rerinku::device)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_options(rerinku_quickstart PRIVATE -Wl,--gc-sections)
        target_link_options(rerinku_device_example PRIVATE -Wl,--gc-sections)
    endif()
endif()

if(RERINKU_REQUESTED_BUILD_TESTING AND RERINKU_BUILD_TESTS)
    function(rerinku_add_test name source)
        add_executable(${name} ${source})
        target_include_directories(${name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src ${cjson_SOURCE_DIR})
        target_link_libraries(${name} PRIVATE rerinku_device)
        target_compile_options(${name} PRIVATE -UNDEBUG)
        target_compile_definitions(${name} PRIVATE _GNU_SOURCE)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    rerinku_add_test(rerinku_device_api_test tests/device_api_test.c)
    rerinku_add_test(rerinku_bitstream_test tests/bitstream_test.c)
    rerinku_add_test(rerinku_rtp_test tests/rtp_test.c)
    rerinku_add_test(rerinku_sdp_test tests/sdp_test.c)
    rerinku_add_test(rerinku_token_test tests/token_test.c)

    # The vendor deliverable links on its own: the C API test against the
    # bundled archive and the C library alone.
    add_executable(rerinku_bundle_link_test tests/device_api_test.c)
    add_dependencies(rerinku_bundle_link_test rerinku_device_bundle)
    target_include_directories(rerinku_bundle_link_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include ${RERINKU_GENERATED_INCLUDE})
    target_compile_options(rerinku_bundle_link_test PRIVATE -UNDEBUG)
    target_link_libraries(rerinku_bundle_link_test PRIVATE ${RERINKU_BUNDLE} Threads::Threads)
    add_test(NAME rerinku_bundle_link_test COMMAND rerinku_bundle_link_test)
endif()

# --- install: the SDK as delivered (component "sdk") -----------------------------------------
# Only the SDK's own files: the dependencies are inside the libraries.
#   cmake --install <build> --component sdk --prefix <dir>
#
#   include/rerinku/                      rerinku_device.h, rerinku_version.h
#   lib/librerinku_device.a               SDK + dependencies, one archive
#   lib/librerinku_device.so*             same, shared (soname librerinku_device.so.0.5)
#   lib/cmake/rerinku_device/             find_package(rerinku_device) -> rerinku::device
#   lib/pkgconfig/rerinku_device.pc       pkg-config --cflags --libs rerinku_device
#   share/rerinku_device/examples/        quickstart/ and device_example.c
#   share/doc/rerinku_device/             README, CHANGELOG, LICENSE, NOTICE
include(CMakePackageConfigHelpers)
set(RERINKU_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/rerinku_device)
configure_package_config_file(cmake/rerinku_deviceConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/rerinku_deviceConfig.cmake
    INSTALL_DESTINATION ${RERINKU_CMAKE_DIR}
    PATH_VARS CMAKE_INSTALL_INCLUDEDIR CMAKE_INSTALL_LIBDIR)
# 0.x: a minor release may break the API, so only the same minor matches.
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/rerinku_deviceConfigVersion.cmake
    COMPATIBILITY SameMinorVersion)
# The .pc file finds its prefix relative to itself: an unpacked SDK tarball
# works wherever it lands.
file(RELATIVE_PATH RERINKU_PC_PREFIX "/${CMAKE_INSTALL_LIBDIR}/pkgconfig" "/")
string(REGEX REPLACE "/$" "" RERINKU_PC_PREFIX "${RERINKU_PC_PREFIX}")
configure_file(cmake/rerinku_device.pc.in ${CMAKE_CURRENT_BINARY_DIR}/rerinku_device.pc @ONLY)

install(FILES ${RERINKU_BUNDLE} DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT sdk)
if(RERINKU_BUILD_SHARED)
    install(TARGETS rerinku_device_shared
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT sdk NAMELINK_COMPONENT sdk)
endif()
install(FILES include/rerinku/rerinku_device.h ${RERINKU_GENERATED_INCLUDE}/rerinku/rerinku_version.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/rerinku COMPONENT sdk)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/rerinku_deviceConfig.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/rerinku_deviceConfigVersion.cmake
    DESTINATION ${RERINKU_CMAKE_DIR} COMPONENT sdk)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/rerinku_device.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig COMPONENT sdk)
install(DIRECTORY examples/quickstart
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rerinku_device/examples COMPONENT sdk)
install(FILES examples/device_example.c
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rerinku_device/examples COMPONENT sdk)
install(FILES README.md CHANGELOG.md LICENSE NOTICE
    DESTINATION ${CMAKE_INSTALL_DOCDIR} COMPONENT sdk)
