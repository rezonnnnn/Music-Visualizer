@echo off
rem Builds build\music-visualizer.exe. Needs zig on PATH (https://ziglang.org/download/, tested with 0.17.1).
if not exist build mkdir build
zig c++ -target x86_64-windows-gnu -O2 -fno-exceptions -fno-rtti -s -Wl,--subsystem,windows src/main.cpp assets/app.rc -o build/music-visualizer.exe -ld3d11 -ldxgi -ldcomp -lole32 -luser32 -lgdi32 -lshell32 -ladvapi32
