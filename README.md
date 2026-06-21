![DreamSourceLab Logo](DSView/icons/dsl_logo.svg)

# DSView + Remote Control API (fork)

This is a fork of [DreamSourceLab/DSView](https://github.com/DreamSourceLab/DSView)
that adds a small **TCP remote-control API** so captures can be driven
programmatically (start / stop / poll status / set threshold / set simple
triggers / export) without touching the GUI.

Motivation: the stock DSView is GUI-only (no CLI, no scripting — see upstream
issues [#382](https://github.com/DreamSourceLab/DSView/issues/382) and
[#104](https://github.com/DreamSourceLab/DSView/issues/104)), and upstream
`libsigrok`/`sigrok-cli` does not support newer DSLogic hardware revisions such
as the **DSLogic U2 Basic** (USB PID `0x0035`). DSView itself does support that
hardware, so adding a thin control server to DSView is the most reliable way to
automate it.

> This is a modified version of GPLv3 software. See [Changes from upstream](#changes-from-upstream)
> and [Copyright and license](#copyright-and-license).

---

## Remote Control API

When DSView runs, it listens on a TCP socket bound to **`127.0.0.1:8321`**.
It is a line-oriented text protocol: connect, send one command, read the
response, connection closes. Localhost only.

### Commands

| Command | Response | Description |
|---------|----------|-------------|
| `ping` | `PONG` | Health check |
| `start` | `OK` / `ERROR: ...` | Start a capture (commits the current simple trigger first) |
| `stop` | `OK` | Stop a running capture |
| `status` | `RUNNING` / `STOPPED` / `IDLE` | Capture state |
| `threshold <V>` | `OK: threshold set to X.XXV` | Set logic voltage threshold (0.0–5.0 V) |
| `trigger` | `chN <type>` per channel | Show the current simple trigger on every logic channel |
| `trigger <ch> <type>` | `OK: chN trigger = <type> ...` | Set a simple trigger on one channel |
| `export <path>` | `OK: exported to <path>` / `ERROR: ...` | Export the last capture to a file |

### Trigger types

`none`, `rising`, `falling`, `high`, `low`, `edge` (edge = any transition).

- A trigger is applied on the **next** `start`.
- Triggers on multiple channels are **ANDed** — the capture fires only when all
  set conditions hold in the same sample.
- Trigger position is fixed at the start of the buffer (post-trigger capture).

### Export formats

Format is chosen by file extension: `.csv`, `.vcd`, `.sr` (sigrok session).

### Notes / limitations

- Sample rate and channel enable/disable must be set in the DSView GUI.
- `threshold` and `trigger` cannot be changed while a capture is running.
- `export` blocks until the file is written and fails if a capture is still
  running — check `status` first.
- The `trigger` command mirrors the per-channel trigger flags in the GUI, so
  changes are visible there too.
- To avoid a modal "trigger set on multiple channels" dialog blocking headless
  use, that warning is disabled at runtime when the server starts.
- Port `8321` is hardcoded.

---

## Usage examples

### Shell (netcat)

```bash
echo "threshold 1.4"       | nc localhost 8321
echo "trigger 2 rising"    | nc localhost 8321
echo "trigger"             | nc localhost 8321   # show all triggers
echo "start"               | nc localhost 8321
while [ "$(echo status | nc localhost 8321)" = "RUNNING" ]; do sleep 0.5; done
echo "export /tmp/cap.csv" | nc localhost 8321
```

### Python

```python
import socket, time

def dsview(cmd):
    s = socket.socket()
    s.connect(('127.0.0.1', 8321))
    s.send(cmd.encode())
    r = s.recv(4096).decode().strip()
    s.close()
    return r

dsview('threshold 1.4')
dsview('trigger 0 none')      # clear
dsview('trigger 2 rising')    # arm on rising edge of ch2
print(dsview('trigger'))      # show all
dsview('start')
while dsview('status') == 'RUNNING':
    time.sleep(0.5)
print(dsview('export /tmp/capture.csv'))
```

---

## Build (macOS / Apple Silicon, Homebrew)

```bash
brew install qt@5 fftw boost libusb libzip glib pkg-config cmake python

mkdir build && cd build
cmake .. \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DLIBUSB_1_INCLUDE_DIR="$(brew --prefix libusb)/include" \
  -DCMAKE_PREFIX_PATH="$(brew --prefix qt@5);$(brew --prefix libusb);$(brew --prefix fftw);$(brew --prefix boost);$(brew --prefix libzip);$(brew --prefix python@3.14)"
make -j$(sysctl -n hw.ncpu)
```

The binary is written to `build.dir/DSView`.

For Linux/Windows build instructions, see the upstream
[INSTALL](INSTALL) notes — the Remote API code is platform-independent
(POSIX sockets; on Windows it would need a Winsock shim).

## Install over an existing DSView.app (macOS)

The freshly built binary links against Homebrew's Qt5, so it needs
`QT_PLUGIN_PATH` set. The clean way is a wrapper script inside the app bundle:

```bash
APP=/Applications/DSView.app/Contents/MacOS

# Back up the original once
cp "$APP/DSView" "$APP/DSView.bak"

# Install our binary as DSView.bin
cp build.dir/DSView "$APP/DSView.bin"
codesign --remove-signature "$APP/DSView.bin"

# Wrapper that sets the Qt plugin path, then launches the real binary
cat > "$APP/DSView" <<'WRAPPER'
#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
export QT_PLUGIN_PATH="/opt/homebrew/opt/qt@5/plugins"
exec "$DIR/DSView.bin" "$@"
WRAPPER
chmod +x "$APP/DSView"
```

Now `open /Applications/DSView.app` (or double-click) works normally and the
API server starts automatically.

Restore the stock app with:

```bash
rm "$APP/DSView" "$APP/DSView.bin"
cp "$APP/DSView.bak" "$APP/DSView"
```

---

## Changes from upstream

Forked from [DreamSourceLab/DSView](https://github.com/DreamSourceLab/DSView)
at upstream commit `2e9e2c8`. All changes are additive; no upstream behaviour
was removed.

| File | Change |
|------|--------|
| `DSView/pv/remoteserver.h` / `.cpp` | **New.** TCP server thread + command handler (start/stop/status/threshold/trigger/export/ping) |
| `DSView/pv/appcontrol.h` / `.cpp` | Start the `RemoteServer` on app open, stop it on close |
| `DSView/pv/storesession.h` | Add `setExportFile()` so an export target can be set without the GUI file dialog |
| `CMakeLists.txt` | Build the new source and run MOC on the new header |

The simple-trigger commit is replicated synchronously in the `start` handler
(`ds_trigger_reset()` + per-channel `LogicSignal::commit_trig()`) because the
GUI's normal `try_commit_trigger()` runs via a cross-thread queued Qt signal and
would otherwise race the FPGA arm when `start` comes from the server thread.

### Hardware note (DSLogic U2 Basic, PID 0x0035)

DSView already drives this device via its bundled `libsigrok4DSL` and FPGA
bitstream; no firmware changes were needed here. (Upstream mainline
`libsigrok`/`sigrok-cli` does **not** support PID `0x0035` — it speaks an older
FX2 control protocol — which is why this DSView-based approach is used.)

---

# DSView (upstream)

DSView is a GUI program for supporting various instruments from [DreamSourceLab](http://www.dreamsourcelab.com), including logic analyzers, oscilloscopes, etc. DSView is based on the [sigrok project](https://sigrok.org).

The sigrok project aims at creating a portable, cross-platform, Free/Libre/Open-Source signal analysis software suite that supports various device types (such as logic analyzers, oscilloscopes, multimeters, and more).

# Useful links

- [dreamsourcelab.com](https://www.dreamsourcelab.com)
- [kickstarter.com](https://www.kickstarter.com/projects/dreamsourcelab/dslogic-multifunction-instruments-for-everyone)
- [sigrok.org](https://sigrok.org)

# Copyright and license

DSView software is licensed under the terms of the GNU General Public License
(GPL), version 3 or later.

While some individual source code files are licensed under the GPLv2+, and
some files are licensed under the GPLv3+, this doesn't change the fact that
the program as a whole is licensed under the terms of the GPLv3+ (e.g. also
due to the fact that it links against GPLv3+ libraries).

Please see the individual source files for the full list of copyright holders.
