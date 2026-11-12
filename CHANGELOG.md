# Changelog

## 0.4.0

A new look: smooth and minimal.

- **Breath**: the neon segment grid is replaced by soft, round-ended bars, brightest at the top and fading towards the foot.
- **Smooth motion**: bars glide on springs (quick up, slow down) instead of jumping between cells.
- **Quieter defaults**: a soft white at full opacity, 32 columns. Any colour still works.
- **Simpler settings**: rows, scanlines, readouts, brackets, peak markers, beat glitch and the glitch colour are gone with the old look. Old `settings.ini` files keep working; the unused lines are ignored.
- **Only the bars**: no frame, no background, no text, locked or unlocked. Unlocked and silent, it shows a row of dots so you can find and place it.
- No download yet: releases are paused until they are signed.

## 0.3.0

Made ready for everyday use.

- **Start with Windows**: a tick box in Settings and in the tray menu.
- **Reliable tempo**: the BPM readout was rebuilt and is now covered by tests from 75 to 174 BPM, at loud and very quiet volumes. It shows `---` until it is sure.
- **Stays put on the desktop**: the locked widget now survives Win+D ("Show desktop") instead of vanishing.
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
