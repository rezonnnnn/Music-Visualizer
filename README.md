# Music Visualizer

**A 105 KB cyberpunk music visualizer that lives on your Windows desktop.**

One `.exe`. No installer, no runtime, no browser engine. It listens to whatever your speakers are playing (Spotify, YouTube, games) and draws it as a block of neon segments. Put it anywhere on your desktop, size it, lock it, and it sits behind your windows like part of the wallpaper.

![The widget, locked](docs/widget.png)

## Get it

**[Download music-visualizer.exe](https://github.com/rezonnnnn/Music-Visualizer/releases/latest/download/music-visualizer.exe)** (105 KB), put it in any folder and double-click it.

1. It appears unlocked, floating on top with a frame around it. **Drag it** where you want it and **drag an edge** to resize.
2. **Double-click it** to lock. It drops behind your windows, clicks pass straight through it, and it remembers its spot.
3. To change anything later, use the **tray icon** (or just run the `.exe` again): Lock position, Settings, Quit.

![Unlocked, ready to place](docs/unlocked.png)

The file is not code-signed, so Windows SmartScreen may ask you to confirm ("More info", then "Run anyway"), and some antivirus tools may block it. If you'd rather not trust a download, build it yourself (below).

Windows 10 or 11, 64-bit.

## Make it yours

<img src="docs/settings.png" width="360" alt="Settings window">

Everything applies as you move the controls:

- **Colour** and **glitch colour**, with the standard Windows colour picker
- **Opacity** (50% by default), **columns** (8 to 96), **rows** (4 to 16) and **frame rate** (15 to 60)
- **Lock position** and **Always on top** (off by default: locked, it lives on the desktop behind your windows)
- **Readouts** (tempo, loudest frequency, level), **peak markers**, **brackets**, **beat glitch**, **scanlines**
- **Hide on fullscreen**: steps aside for fullscreen apps, games and presentations
- **Reset position**: back to bottom centre, if you lose it

Settings are kept in a plain `settings.ini` next to the `.exe`. You can edit it by hand; changes apply within a second of saving.

## Why it's small

| | Measured |
|---|---|
| Size on disk | **107,520 bytes** (one file) |
| CPU while music plays, 60 fps | **about 0.2%** of the whole CPU (0.16% and 0.26% in two runs) |
| CPU when nothing is playing | **0.007%** |
| GPU (Intel integrated) | about 2.4% while playing, 0% when silent |
| Memory | 35 MB private, almost all of it the graphics driver Windows loads |
| Source | one C++ file, about 870 lines, no dependencies |

Measured in 15 to 20 second samples on a Core Ultra 5 235U laptop (14 threads, Intel integrated graphics, Windows 11, 1920x1200 at 60 Hz) with a 120 BPM test track.

How it stays that small:

- **One pixel shader draws everything.** Cells, peak markers, brackets and the text are computed per pixel on the GPU in a single draw call. There are no textures and no font files: the text uses a 5x7 pixel font stored as 42 small bit patterns.
- **It only draws when the picture changes.** The look is made of whole cells, so many frames are identical to the last one and are skipped.
- **Windows does the transparency.** The widget is handed to the desktop compositor (DirectComposition), so the CPU never touches a pixel.
- **Audio comes straight from Windows.** It reads the speaker output directly (WASAPI loopback). No virtual audio cable, no "share your screen" prompt.
- **It sleeps when nothing is playing.** After 1.5 seconds of silence it fades out, stops drawing and checks for audio four times a second.
- **Nothing is allocated after start-up.** Every buffer has a fixed size.

## Build it

You need [Zig](https://ziglang.org/download/) (about 100 MB, unzip and go; tested with 0.17.1). No Visual Studio.

```
build.cmd
```

or directly:

```
zig c++ -target x86_64-windows-gnu -O2 -fno-exceptions -fno-rtti -s -Wl,--subsystem,windows src/main.cpp -o music-visualizer.exe -ld3d11 -ldxgi -ldcomp -lole32 -luser32 -lgdi32 -lshell32
```

## Status

Early, but it works. So you know what you're getting:

- **Tested** on one Windows 11 laptop: first-run placement, the drag and resize handles, double-click to lock, click-through when locked, saving position, the settings window (slider and checkbox), live reload of `settings.ini`, sleeping on silence, and the numbers above.
- **Not yet tested**: the colour picker, the tray menu itself, opening settings by running the `.exe` a second time, Reset position, switching audio devices, more than one monitor, and how it behaves with "Show desktop" (Win+D).
- **Tempo is approximate.** The BPM readout can read high on some music.
- **Antivirus**: small unsigned programs that record audio and draw on the desktop can trip corporate antivirus. One removed test builds during development. Building from source lets you see exactly what runs.
- **Next**: bring memory down (the 35 MB is the price of loading the graphics driver), steadier tempo detection, and a signed download.

## Licence

MIT. See [LICENSE](LICENSE).
