#!/usr/bin/env sh
set -eu

cmake --fresh -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug
