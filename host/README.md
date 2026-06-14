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

## Installing on Linux — choosing the firmware path

The MIDEX has **no RAM firmware at power-up**, so whatever claims the loader-PID
device first decides what it becomes. Two paths compete:

- **class** (this project): our uploader streams the class-compliant firmware →
  the device re-enumerates as a standard USB-MIDI device and the inbox
  `snd-usb-audio` driver binds, no custom code.
- **stock** (parent project): the in-tree `snd_usb_midex` driver sees the loader
  PID and uploads Steinberg's **proprietary** firmware, which it then drives over
  its vendor protocol.

If both are active they race for the device, so pick one. The helper script
[`midex-fwctl.sh`](midex-fwctl.sh) installs the pieces and flips between paths:

```sh
cmake -S . -B build && cmake --build build     # build the uploader first
make -C ../firmware class                        # build the firmware .ihx

sudo ./midex-fwctl.sh install     # copy uploader, firmware, udev rule, systemd unit
sudo ./midex-fwctl.sh class       # use the class-compliant firmware
sudo ./midex-fwctl.sh stock       # switch back to the original Steinberg firmware
sudo ./midex-fwctl.sh status      # show what's installed and which path is active
```

After `class`/`stock`, **power-cycle the MIDEX** so it re-enters its loader PID
and the selected path claims it. `class` blacklists `snd_usb_midex` and arms the
upload unit; `stock` removes the blacklist and masks the unit so a stray plug
event cannot re-flash the device.

### Why a systemd unit and not just `RUN+=`

The upload takes a couple of seconds and the device **re-enumerates** mid-way.
`udev` reaps `RUN+=` child processes after a short timeout and explicitly forbids
long-running processes, so a direct `RUN+=` upload can be killed mid-transfer.
The installed rule ([`install/99-midex-class.rules`](install/99-midex-class.rules))
therefore only **tags** the device and hands off to a oneshot
([`install/midex-upload@.service`](install/midex-upload@.service)) that owns the
upload in its own cgroup.

On a system **without systemd**, a direct `RUN+=` rule is the pragmatic fallback
(accepting the reap-timeout caveat); replace the tag/hand-off lines with:

```udev
ACTION=="add", SUBSYSTEM=="usb", ATTR{idVendor}=="0a4e", \
  ATTR{idProduct}=="1000", \
  RUN+="/usr/local/bin/midex-fw-upload /usr/local/lib/midex/midex-class-r1.ihx"
# (repeat for idProduct 1010 = r2, 1100 = MIDEX3)
```

### Manual / one-shot upload

You can always skip the rule and upload by hand (see **Use** above). To stop the
stock driver grabbing the device for a single test:

```sh
sudo modprobe -r snd_usb_midex
sudo ./build/midex-fw-upload ../firmware/midex-class-r1.ihx
```

### Zero-code Linux alternative: fxload

The same RAM download can be driven by the stock `fxload` utility (treats
`-ENODEV` differently, but works for the AN2131):

```sh
sudo fxload -t an21 -I firmware.ihx -D /dev/bus/usb/<bus>/<dev>
```

`midex-fw-upload` exists so the *same* codebase works on Windows and macOS
where `fxload` is unavailable.

## Testing the class-compliant firmware

After uploading `midex-class-r1.ihx`, the device enumerates as a standard
USB-MIDI device (PID `0x10C1`) and `snd-usb-audio` binds — no custom protocol.
[`class_loopback.py`](class_loopback.py) is a quick smoke test over ALSA
`amidi`: it discovers the MIDEX ports and round-trips a SysEx on each.

```sh
# patch a MIDI cable OUT->IN on each port under test, then:
./class_loopback.py            # test every discovered MIDEX port
./class_loopback.py -n 2       # require/test the first 2 ports
```

Needs `alsa-utils` (`amidi`); no root required if your user can reach the sound
devices. This is distinct from [`loopback.py`](loopback.py), which tests the
*bus-probe* (`0x10C0`) over EP0 vendor commands, not the class firmware.
