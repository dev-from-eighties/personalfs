#!/usr/bin/env sh
set -eu

cmake --fresh -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
