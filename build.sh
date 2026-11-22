#!/usr/bin/env bash
# Cross-compiles the Windows DLL with MinGW-w64 (Linux/WSL/MSYS2), and the
# command-line test tool for the host.
#   Debian/Ubuntu: sudo apt install g++-mingw-w64-x86-64
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p dist build
x86_64-w64-mingw32-g++ -O2 -std=c++17 -shared -static -static-libgcc -static-libstdc++ \
  -DUNICODE -D_UNICODE -s -o dist/CadThumbnails.dll \
  src/*.cpp win/dllmain.cpp win/CadThumbnails.def \
  -lole32 -luuid -lshlwapi -lwindowscodecs -lgdi32 -ladvapi32 -lshell32
echo "built dist/CadThumbnails.dll"
g++ -O2 -std=c++17 -o build/thumbtest src/*.cpp tools/thumbtest.cpp
echo "built build/thumbtest (usage: build/thumbtest <file> <out-prefix> [size])"
