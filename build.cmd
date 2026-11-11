@echo off
rem Builds music-visualizer.exe into build\. Needs zig on PATH (https://ziglang.org/download/).
if not exist build mkdir build
zig c++ -target x86_64-windows-gnu -O2 -fno-exceptions -fno-rtti -s -Wl,--subsystem,windows src/main.cpp -o build/music-visualizer.exe -ld3d11 -ldxgi -ldcomp -lole32 -luser32 -lgdi32 -lshell32
