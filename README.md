# CC Scout GUI

A C and GTK 4 frontend for the [CC Scout MIDI core](https://github.com/riccardo-enr/cc-scout).
The first window follows the desktop's light or dark preference and shows live
MIDI notes and control changes.

```text
ALSA sequencer -> CC Scout core -> GTK source list
```

## Build and run

On Ubuntu, install `build-essential cmake pkg-config libgtk-4-dev libadwaita-1-dev libasound2-dev`.
On Arch Linux, install `base-devel cmake pkgconf gtk4 libadwaita alsa-lib`.
The app needs GTK 4.6 or newer and libadwaita 1.4 or newer.

Keep the core checkout at `../cc-scout`, or pass
`-DCC_SCOUT_CORE_DIR=/path/to/cc-scout` when configuring.

```sh
cmake -S . -B build
cmake --build build
./build/cc-scout-gui
```

Connect the MPK mini IV before or after launching. CC Scout automatically
monitors its main MIDI and DAW ports as one device. Open Settings from the
header bar to monitor another ALSA client. The choice lasts for the current
session.
