# Changelog

## 0.3.0

Made ready for everyday use.

- **Start with Windows**: a tick box in Settings and in the tray menu.
- **Reliable tempo**: the BPM readout was rebuilt and is now covered by tests from 75 to 174 BPM, at loud and very quiet volumes. It shows `---` until it is sure.
- **Recovers by itself**: after a graphics driver update or reset, when a monitor is unplugged (it returns to a visible screen), and when Explorer restarts (the tray icon comes back).
- **Settings live in your user folder** (`%APPDATA%\MusicVisualizer`), so the app works from any folder. Keep a `settings.ini` next to the `.exe` for a portable copy.
- **Proper Windows citizen**: its own icon, version details in the file properties, and a tray tip that says when no audio device is found.
- **Privacy, checked on every build**: the program contains no networking code, and an automated check fails the build if that ever changes.
- Every release is now built and tested from source by GitHub Actions on two Windows versions.

## 0.2.0

- The visualizer became a desktop widget: drag to move, drag an edge to resize, double-click to lock. Locked, it sits behind your windows and clicks pass through it.
- Settings window with colour pickers, sliders and tick boxes; changes apply as you move the controls.
- Solid cells and outlines at 50% opacity by default.
- Draws only when the picture changes.

## 0.1.0

- First release: a neon segment strip above the taskbar that follows whatever your speakers play.
