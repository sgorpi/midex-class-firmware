# midex-fw-upload — MIDEX RAM firmware uploader

Cross-platform libusb tool that streams an Intel-HEX (`.ihx`) image into the
MIDEX's EZ-USB on-chip RAM using the verified single-stage Anchor Download
sequence (`CPUCS=1` → `0xA0` writes → `CPUCS=0`). See
[../doc/hardware_register_map.md](../doc/hardware_register_map.md) and
[doc/firmware_upload_process.md](../../../doc/firmware_upload_process.md).

## Build

Requires **libusb-1.0** development files and CMake ≥ 3.13.

| Platform | Dependencies |
|----------|--------------|
| Debian/Ubuntu | `sudo apt install cmake libusb-1.0-0-dev` |
| Fedora | `sudo dnf install cmake libusbx-devel` |
| macOS (Homebrew) | `brew install cmake libusb` |
| Windows (MSYS2) | `pacman -S mingw-w64-x86_64-cmake mingw-w64-x86_64-libusb` |

```sh
cmake -S . -B build
cmake --build build
# -> build/midex-fw-upload
```

CMake checks for libusb-1.0 (pkg-config first, then a manual header/library
search) and fails configuration with an install hint if it is missing.

## Use

The device must be in a **loader PID** (`0x1000` r1 / `0x1010` r2 / `0x1100`
MIDEX3) so the EZ-USB ROM handles the `0xA0` request. A device running
operational firmware reverts to its loader PID on a **power-cycle** (RAM is
volatile — this is what makes the device unbrickable).

```sh
sudo ./build/midex-fw-upload firmware.ihx          # auto-detect loader PID
sudo ./build/midex-fw-upload -d 0a4e:1000 firmware.ihx
```

`NO_DEVICE` on the final `CPUCS=0` is **expected and treated as success**: the
8051 boots and the device re-enumerates before libusb finishes the round-trip.

### Linux caveat — the in-tree driver auto-uploads stock firmware

If `snd-usb-midex` is loaded, it will see the loader PID on power-up and upload
the **stock** firmware before this tool can grab the device. While testing
custom firmware, unbind/blacklist it:

```sh
sudo modprobe -r snd_usb_midex          # or blacklist it
```

### Optional: udev rule (auto-upload on plug)

To upload automatically when a loader-PID device appears, drop a rule like
`/etc/udev/rules.d/99-midex-class.rules`:

```udev
ACTION=="add", SUBSYSTEM=="usb", ATTR{idVendor}=="0a4e", \
  ATTR{idProduct}=="1000", RUN+="/usr/local/bin/midex-fw-upload /usr/local/lib/midex/midex-class-r1.ihx"
```

After our firmware boots it enumerates as a standard USB-MIDI device and the
inbox `snd-usb-audio` (Linux) / `usbaudio.sys` (Windows) / `AppleUSBAudio`
(macOS) driver binds with zero custom code.

### Zero-code Linux alternative: fxload

The same RAM download can be driven by the stock `fxload` utility (treats
`-ENODEV` differently, but works for the AN2131):

```sh
sudo fxload -t an21 -I firmware.ihx -D /dev/bus/usb/<bus>/<dev>
```

`midex-fw-upload` exists so the *same* codebase works on Windows and macOS
where `fxload` is unavailable.
