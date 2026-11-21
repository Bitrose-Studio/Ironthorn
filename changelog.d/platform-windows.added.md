- `ith/platform`: windows and the event loop on SDL 3.4. `ith::Platform` initializes the
  platform layer, creates windows, pumps the operating system's events once per frame into a
  list of engine events (quit and close requests, resize, display scale, focus, minimize,
  maximize, restore, displays, low memory, background and foreground) and reads and writes the
  clipboard. `ith::Window` reports its size in window coordinates and in pixels, its display
  scale and its native handles for the RHI. See `docs/development/platform.md`.
- Building the client modules on Linux needs the X11, Wayland and related development packages
  listed in `docs/development/building.md`.
