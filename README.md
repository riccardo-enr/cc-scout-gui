# CC Scout GUI

A C and GTK 4 frontend for the [CC Scout MIDI core](https://github.com/riccardo-enr/cc-scout).
The first window follows the desktop's light or dark preference and shows live
MIDI notes and control changes when the ALSA sequencer signals input readiness.

```text
ALSA sequencer -> CC Scout core -> GTK source list
```

## Build and run

On Ubuntu, install `build-essential cmake pkg-config libgtk-4-dev libadwaita-1-dev libasound2-dev libglib2.0-dev-bin libyaml-dev`.
On Arch Linux, install `base-devel cmake pkgconf gtk4 libadwaita alsa-lib glib2-devel libyaml`.
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

The MPK mini IV pad grid follows the DAW preset's note mapping on MIDI
channel 10. Notes 36-43 (Bank A) and 44-51 (Bank B) share the eight
physical positions. Mirrored messages light one tile and remain separate
in the raw feed. Edit `presets/akai-mpk-mini-iv.yaml` and rebuild to match a custom
preset. The YAML supplies port names, MIDI channels, pad notes, and the names
used by Learn All Knobs.

The eight knob tiles show saved names and the last relative step: CW for
clockwise or CCW for counterclockwise. The bundled MPK mini IV DAW preset
starts with CC 24-31 in physical order. Learn All Knobs saves the slot
assignments if you use different CC numbers. Rerun it if you saved a
remapped preset before slot assignments were stored.

## MIDI Learn

Open Settings, click **Learn**, and turn the knob you want to name. Enter a
name and click **Save**. The name appears alongside the raw CC event and
survives a restart. Bindings are stored in `cc-scout-gui/knobs.ini` under your
XDG config directory, usually `~/.config`. The binding uses the ALSA client
and port names, MIDI channel, and CC number.

For the MPK mini IV, click **Learn All Knobs** and follow the prompts for
Division, Swing, Mode, Oct, Latch, Sync, Gate, and BPM. Turn only the requested
knob at each step. Click **Save All** after the last knob to write the entire
set together. Closing Settings before saving cancels the session.

For pads, click **Learn Pad** in Settings. The reference follows the MPK mini
IV layout, with pads 5-8 above 1-4. Tap one physical pad, check the highlighted
position, enter a name, and click **Save**. The same physical pad uses the
saved name in both banks, even after restarting. Mirrored messages capture
one press and still appear separately in the raw feed.
