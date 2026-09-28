# How the module talks to the RX-888 MkII

What the module sends, and where each fact comes from. The module was written
from these facts; no code was copied from the projects named here.

Sources, at the commits read:

- ringof/rx888-firmware `f97f213`, the firmware the module carries
  (release 0.1.0): `SDDC_FX3/USBHandler.c`, `SDDC_FX3/driver/Si5351.c`,
  `SDDC_FX3/radio/rx888r2.c`, `docs/LICENSE_ANALYSIS.md`
- ExtIO_sddc `331b35c`, where the firmware began: `SDDC_FX3/Interface.h`
- ka9q-radio `bb5ce03`: `src/rx888.c`, `src/ezusb.c`
- the LTC2208 and AD8370 datasheets (Analog Devices)

## USB

The FX3 enumerates as Cypress 04b4:00f3 until firmware runs, then as
04b4:00f1 (ExtIO `SDDC_FX3/USBdescriptor.c`). The firmware's serial number is
sixteen hex digits from the FX3's die; the product string of an MkII is
`RX888mk2`. The bus and port chain is the one name a board keeps across the
re-enumeration, which is why the module can be told `port:`.

## Loading the firmware

An FX3 image starts with `C`, `Y`, a control byte and the type `0xB0`.
Sections follow, each a length in 32-bit words, a RAM address and the words;
a section of length 0 ends the list and carries the entry point, and one
last word is the wrapping sum of all data words (ka9q `ezusb.c`). The
bootloader takes each section with vendor request `0xA0`, wValue the low and
wIndex the high half of the address, at most 4096 bytes a request, and starts
the firmware on a `0xA0` without data at the entry point. It may leave the bus
before acknowledging that, so an I/O error there is success. The firmware
then enumerates in its place; the module waits up to six seconds for it.
The new device node appears before udev has given it to the receiver's user,
so an open refused for permission in that time is tried again every 100 ms
until the six seconds are up.

## Requests to the firmware

Vendor requests, OUT unless noted (ringof `USBHandler.c`):

| Request | | |
|---|---|---|
| `0xAA` | start the stream | data ignored; refused unless the clock runs |
| `0xAB` | stop the stream | |
| `0xAC` | identify, IN | 4 bytes: board type, firmware major, minor, count |
| `0xAD` | GPIO | the whole 32-bit word, little-endian |
| `0xB1` | reset | back to the bootloader |
| `0xB2` | converter clock | 32-bit Hz, little-endian; 0 stops it |
| `0xB6` | set an argument | wValue the value, wIndex the argument, one byte of data |

Board type 4 is the RX-888 MkII (ExtIO `Interface.h`). The ringof firmware
supports the MkII only and reports 4 whatever board it runs on, so the module
asks a board that already runs firmware before it resets it, and leaves one
that reports another type, or does not know the request, as it is. The clock request is acknowledged once the Si5351 has
settled, which can take a second, so control requests wait up to five.

## GPIO

Bits of the word for the MkII (ringof `radio/rx888r2.c`):

| Bit | |
|---|---|
| 5 | converter shut down |
| 6 | dither |
| 7 | randomizer |
| 8 | bias tee, HF input |
| 9 | bias tee, VHF input |
| 11 | LED |
| 15 | VHF input |
| 16 | converter PGA pin low: the 2.25 Vpp range; clear, 1.5 Vpp |

The module sets bits 5 to 8, 11 and 16 and leaves the VHF input off.

## Attenuator and VGA

Argument 10 is the step attenuator, in half dB from 0 to 63 (31.5 dB).
Argument 11 is the AD8370's gain byte: a 7-bit code and, in bit 7, the
high-gain mode. Its voltage gain is code × 0.055744, times 7.079458 in
high-gain mode, from -25 dB (low-gain mode, code 1) to +34 dB (high-gain
mode, code 127); code 0 mutes it (AD8370 datasheet; ka9q `rx888.c`). The
module uses the high-gain mode, which has the lower noise, wherever one of its
codes lands within half a dB of the gain asked for, and the low-gain mode
otherwise.

## The converter clock

The firmware programs the Si5351's PLL A from its 27 MHz crystal: an output
divider of 900 MHz divided by the rate, rounded down to an even number, and a
PLL of that divider times the rate, set as a whole multiplier of the crystal
plus a fraction over 1048575, truncated (ringof `driver/Si5351.c`). The module
computes the same to report the rate the board runs at: 64.8 and 129.6 MHz
come out exact, 100 MHz about 3 parts in 10^8 low.

## Samples

Bulk IN endpoint `0x81`, bursts of 16 packets of 1024 bytes, so transfers are
multiples of 16 KiB; the module keeps 16 of 512 KiB in flight (ka9q uses the
same size), at most 24: Linux gives all programs' USB transfers 16 MiB
together unless `usbcore.usbfs_memory_mb` is raised. Samples are signed 16-bit little-endian, one converter sample
each. With the randomizer on, the converter XORs bits 1 to 15 of a sample
with its bit 0; the module undoes it by XORing samples whose bit 0 is set with
`0xFFFE` (LTC2208 datasheet). The clock is set before the stream starts;
stopping cancels the transfers, then stops the stream, the clock and the
converter.
