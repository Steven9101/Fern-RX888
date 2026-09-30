# Fern-RX888

Fern-RX888 lets [FernSDR](https://github.com/Steven9101/WebSDR) receive with an
RX-888 MkII. It is an input module: a separate program that FernSDR starts
for a band and talks to over pipes, as FernSDR's `docs/MODULES.md` describes.
FernSDR sends the band's settings; the module loads the RX-888's firmware,
sets its attenuator, gain and converter clock, and writes the converter's
16-bit samples to FernSDR, one band from 0 Hz to half the sample rate. It
reports statistics once a second, clipping among them, and takes gain,
attenuator, bias tee and dither changes while it runs.

Keeping the USB code in a module means FernSDR itself links only the C and
C++ runtime. The module is one statically linked executable; libusb and the
RX-888's firmware are built into it.

## Hardware

The RX-888 MkII, receiving HF by direct sampling: its LTC2208 converter
samples the antenna input at up to 130 MHz, so a band covers 0 Hz to half the
sample rate.

- `sample_rate = 64800000` covers 0 to 32 MHz and keeps the board cooler.
- `sample_rate = 129600000` covers 0 to 64 MHz.

Both are exact on the board's clock; other rates from 10 to 130 MHz work and
come within a few parts in 10^8. The RX-888 needs a USB 3 port: at 129.6 MHz
it sends 259 MB/s.

VHF through the board's R828D tuner is not supported yet. The RX-888 mk1, the
RX-888 r3 and other SDDC boards wire their controls differently and are
refused.

## Using it with FernSDR

Install the package for your machine from the admin panel, which offers the
releases of this repository, or from a shell:

```sh
fernsdr --install-module rx888-0.1.0-linux-x86_64.fernmod fernsdr.conf
```

Then give a band `source = module`, a real signal and a centre of 0:

```ini
[band:hf]
name               = HF
source             = module
module             = rx888
sample_rate        = 64800000
signal             = real
center             = 0
module.gain        = auto
module.attenuation = 0
```

`fern-rx888 --list-devices` prints the RX-888s that are plugged in, by USB
port and, once their firmware runs, by serial number. With only one plugged
in, `module.device` can be left out; with several, give each band
`module.device = port:<bus-port>`, the one name known before the firmware
runs.

FernSDR's `install.sh` adds a udev rule that gives the receiver's user the
RX-888, as Cypress 04b4:00f3 before its firmware is loaded and 04b4:00f1
after. Without FernSDR's installer:

```
SUBSYSTEM=="usb", ATTRS{idVendor}=="04b4", ATTRS{idProduct}=="00f3", MODE:="0660", GROUP:="fernsdr"
SUBSYSTEM=="usb", ATTRS{idVendor}=="04b4", ATTRS{idProduct}=="00f1", MODE:="0660", GROUP:="fernsdr"
```

in `/etc/udev/rules.d/61-fernsdr-usb.rules`, then unplug the RX-888 and plug
it in again.

## Settings

`fern-rx888 --describe` lists them with their help texts; FernSDR's admin
panel shows the same.

| Setting | Default | Live | |
|---|---|---|---|
| `device` | empty | no | `serial:<serial>`, `port:<bus-port>` or `index:<n>` |
| `gain` | `auto` | yes | the VGA: `auto`, or -25 to 34 dB |
| `attenuation` | 0 | yes | the step attenuator, 0 to 31.5 dB in 0.5 dB steps |
| `bias_tee` | no | yes | DC on the HF input for an active antenna |
| `dither` | no | yes | the converter's dither |
| `randomizer` | no | no | scramble the converter's output bits on the board |
| `adc_range` | `1.5` | no | the converter's input range in Vpp; `2.25` for more headroom |
| `firmware` | empty | no | the path of another FX3 image to load |
| `transfers` | 16 | no | 512 KiB USB transfers in flight, 4 to 24 |

`gain = auto` chooses the highest VGA gain that keeps the converter out of
clipping with 6 dB to spare and lowers it at once when something clips. When
even the lowest gain clips, raise `attenuation`.

## The firmware

The RX-888's USB controller, a Cypress FX3, runs firmware that the host loads
into its RAM whenever the board is plugged in. The module carries release
0.1.0 of [ringof/rx888-firmware](https://github.com/ringof/rx888-firmware),
unmodified, and loads it on every start, also into a board that already runs
firmware, so that it always knows what it is talking to. `fern-rx888
--notices` prints the firmware's licences: its own code is under the MIT
licence, and the Cypress FX3 SDK it is built with under the Cypress Software
License Agreement; `firmware/NOTICE.md` says more. It also prints the
licences of the module itself and of the libusb its static builds carry.

The module loads firmware only into the FX3 a band selects. A board that
already runs firmware is asked what it is first and left alone unless it
answers as an RX-888 MkII. A board still waiting for firmware cannot say:
Cypress's bootloader id is shared by every FX3 board without firmware, and
the firmware the module loads reports an MkII on any board. With another FX3
device plugged in (an RX-888 mk1, an HF103, a logic analyser), point
`module.device` at the RX-888's port so that the module never picks the
other one.

## Building

```sh
make                 # build/fern-rx888 with the system libusb
make test            # unit, protocol, command line and package tests
make test-asan       # the same under AddressSanitizer and UBSan
make package         # dist/rx888-0.1.0-linux-<arch>.fernmod, statically linked
make package ARCH=aarch64
```

The native build needs the libusb-1.0 development files; `make static` and
`make package` build libusb from `third_party/` and need none.

The tests drive the module against a fake RX-888 that loads firmware,
re-enumerates and streams as the real one does (`tests/fake_fx3.cpp`). No
RX-888 was attached while this version was written: its behaviour with real
hardware follows the firmware's sources and the parts' datasheets
(`docs/PROTOCOL.md`), not a test on a board.

## Licence

GPL-2.0-or-later, see `LICENSE`. libusb is under LGPL-2.1-or-later
(`third_party/README.md`); the firmware under the terms above.
