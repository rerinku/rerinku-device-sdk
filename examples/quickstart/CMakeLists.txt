# Build the quick start against an installed (or unpacked) Rerinku Device SDK:
#
#   cmake -S . -B build -DCMAKE_PREFIX_PATH=<sdk dir>
#   cmake --build build
#
# Cross-compiling: add -DCMAKE_TOOLCHAIN_FILE=<your toolchain file> and point
# CMAKE_PREFIX_PATH at an SDK built with that toolchain.
cmake_minimum_required(VERSION 3.16)
project(rerinku_quickstart LANGUAGES C)

find_package(rerinku_device 0.5 CONFIG REQUIRED)

add_executable(quickstart quickstart.c)
target_compile_features(quickstart PRIVATE c_std_11)
# rerinku::device is the static archive (every dependency inside);
# rerinku::device_shared links librerinku_device.so instead.
target_link_libraries(quickstart PRIVATE rerinku::device)
