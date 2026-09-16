# pi5la: 8-channel PIO logic analyzer for Raspberry Pi 5

A command-line logic analyzer that captures GPIO 2–9 with the PIO block in the
Pi 5's RP1 southbridge. It writes sigrok `.sr` files that open in PulseView.

Target: 10 MSa/s on 8 channels, sustained, with **zero dropped samples**.

> **Status:** this code has been compiled and unit-tested on x86 Linux only.
> None of the milestones below has been checked on Pi 5 hardware yet. Every
> "Result" field is **TBD** until it has been measured on the Pi.

---

## ⚠️ Hardware safety: read before connecting anything

- **Pi 5 GPIO is 3.3 V and NOT 5 V tolerant.** Overvoltage on a GPIO pin damages
  **RP1**, the Pi 5 southbridge. RP1 is soldered to the board and cannot be
  replaced. Damaging it effectively destroys the Pi.
- **Before you connect to any external target**, you need a buffered front end:
  a **74LVC245** (or equivalent 5 V-tolerant-input buffer powered from 3.3 V)
  between the target and the Pi. Put **series resistors (e.g. 100–330 Ω)** on
  each channel, on the target side of the buffer. Share ground between the
  target and the Pi. The buffer protects RP1 from overvoltage, from contention,
  and from targets that are powered while the Pi is off.
- **The only safe bare-wire setup is loopback testing:** jumper wires between
  Pi 5 header pins (for example GPIO 20 → GPIO 2), with nothing external
  attached.
- GPIO 2 and GPIO 3 have on-board pull-ups to 3.3 V (I²C). An undriven channel
  0 or 1 reads high, not low.

Suggested front end per channel:

```
target ──[R 100–330 Ω]──► 74LVC245 An ──► Bn ──► Pi 5 GPIO (2..9)
                          VCC = 3.3 V (from Pi), DIR = A→B, /OE = GND
target GND ───────────────────────────────────── Pi 5 GND
```

---

## Build (M0)

The commands below are the intended M0 sequence. They have **not** been run on
a Pi yet. Once you have run them, replace this note with the exact commands
you used and their output.

### 1. Prerequisites

```bash
sudo apt install git build-essential cmake pkg-config libzip-dev \
                 device-tree-compiler libncurses-dev libfdt-dev libgnutls28-dev
```

### 2. Driver and permissions

```bash
uname -r              # this project targets kernel 6.18+ (DMA read-ahead)
ls -l /dev/pio0
```

- If `/dev/pio0` does not exist, the `rp1-pio` driver is not loaded. Update the
  Pi 5 firmware and kernel (`sudo apt full-upgrade`) and reboot.
- If it is `root root`, add the following to `/etc/udev/rules.d/99-com.rules`,
  following piolib's README:

  ```
  SUBSYSTEM=="*-pio", GROUP="gpio", MODE="0660"
  ```

  Then run `sudo udevadm control --reload-rules && sudo udevadm trigger` and
  make sure your user is in the `gpio` group. **Don't run pi5la under `sudo` to
  get around this.**

Measured on the development Pi 5 (Raspberry Pi OS Bookworm, kernel
`6.18.50+rpt-rpi-2712`):

```
crw-rw---- 1 root gpio 239, 0 Sep 15 03:18 /dev/pio0
```

It is group `gpio` with mode 0660 by default, and the user is in `gpio`, so no
root is needed. pi5la runs as a normal user.

### 3. piolib (raspberrypi/utils)

```bash
git clone https://github.com/raspberrypi/utils
cd utils
cmake -S . -B build
cmake --build build -j4
sudo cmake --install build        # libpio.a -> /usr/local/lib, headers -> /usr/local/include/piolib
cd ..
```

Stock example check (M0 acceptance): load a PIO program and run a state
machine.

```bash
./utils/build/piolib/examples/piopwm 4     # PWM on GPIO 4; expect no errors
```

### 4. pioasm (pico-sdk)

The spec says pioasm comes from raspberrypi/utils, but it lives in pico-sdk.
Build it on its own:

```bash
git clone --depth 1 https://github.com/raspberrypi/pico-sdk
cmake -S pico-sdk/tools/pioasm -B build-pioasm -DPIOASM_VERSION_STRING=2.2.0
cmake --build build-pioasm -j4
```

### 5. pi5la

```bash
cmake -S pi5la -B build-pi5la -DPIOASM_EXECUTABLE=$PWD/build-pioasm/pioasm
cmake --build build-pi5la -j4
(cd build-pi5la && ctest)
```

This builds `pi5la`, `ratesweep` and `bitprobe`. `.pio` files are assembled by
pioasm at build time.

To build and run only the unit tests on any Linux box, without piolib:
`cmake -S pi5la -B build-host -DPI5LA_TESTS_ONLY=ON`.

---

## Usage

```
pi5la --rate 10M --samples 1000000 --out capture.sr
      [--channels 8] [--base-gpio 2]
      [--trigger rising:0] [--mode dma|naive]
      [--generator 100k] [--raw words.bin] [--dma-buf 1048576]
pi5la --generator 1 --generator-only
```

| Option | Meaning |
|---|---|
| `--rate` | Sample rate with `k`/`M` suffix. Rate = 200 MHz / divider; the divider is an integer plus a 1/256 fraction. Fractional dividers add jitter, so pi5la prints a note when one is used. |
| `--samples` | Number of samples to capture. |
| `--out` | sigrok `.sr` output. |
| `--raw` | The packed 32-bit FIFO words, as received. Used for M4 analysis. |
| `--channels` | 1, 2, 4 or 8. The PIO program is `in pins, N`, which packs 32/N samples per word. |
| `--base-gpio` | First capture GPIO. Pins must be consecutive (default 2). |
| `--trigger` | `rising`, `falling`, `high` or `low`, then `:N` with N relative to base. |
| `--mode` | `dma` (default) or `naive` (the M2 baseline). |
| `--generator` | Square wave on GPIO 20 during capture, for loopback self-test. |
| `--generator-only` | Run only the generator until Ctrl-C (M1). |
| `--dma-buf` | Size of each DMA bounce buffer (4 buffers are used). |

Exit status: `0` OK, `1` error, `2` dropped samples detected. Stats and the
drop report go to stderr:

- **stall events**: in DMA mode, the number of DMA buffers after which the SM's
  sticky `RXSTALL` flag was set. `RXSTALL` means the FIFO filled and the SM
  paused, so samples are missing. In naive mode the flag is checked once at
  the end. Any stall event gives exit code 2.
- **estimated lost samples**: `rate × elapsed − samples received`, measured
  from the first buffer to the last. It's a wall-clock estimate that includes
  scheduling jitter, so it is reported but does **not** affect the exit code.

---

## Design notes

- **No word-by-word polling.** piolib calls are RPCs to RP1 firmware over PCIe.
  Bulk capture uses piolib's DMA API only (`pio_sm_config_xfer` /
  `pio_sm_xfer_data`). `--mode naive` exists only as the M2 baseline.
- **DMA read-ahead (kernel 6.18+).** `pio_sm_xfer_data(..., NULL)` queues a
  receive and returns; a call with a buffer waits for the oldest queued receive.
  pi5la keeps 4 receives queued, reads one, then queues the next. A final
  *guard* receive stays queued while RXSTALL is checked after the last data
  buffer, so the stall that follows the end of the capture isn't counted. On
  6.12 kernels `NULL` read-ahead returns `-EINVAL`, and this path does not work.
- **Packing.** `sm_config_set_in_shift(shift_right = true, autopush, 32)` and
  `PIO_FIFO_JOIN_RX` (TX is unused, so the RX FIFO is twice as deep).
- **Runtime PIO programs.** Channel counts other than 8 and all trigger types
  are built at runtime (`src/progbuild.c`). On every capture the builder is
  checked against the pioasm output of `pio/la8.pio` and `pio/la8_trig.pio`.
- **Generator.** `sqwave.pio` (the spec program) covers ~1.53 kHz–100 MHz.
  Lower frequencies, such as the 1 Hz LED check, use `sqwave_slow.pio`, a delay
  loop at divider 1.
- **Prior art.** cleverca22's `libsigrok` `pi5-rp1-pio` driver uses a single
  4096-byte DMA buffer (`buf_count = 1`), one channel and `shift_right = true`.
  It doesn't solve continuous rotation. raspberrypi/utils issue #123 (large
  transfers timing out on 6.12) was fixed in the kernel. Issue #124 (DMA reads
  can't be terminated early) is still open.

---

## Milestones

Each one needs its acceptance test to pass on the Pi before the next one.

### M0 — Toolchain bring-up
Follow [Build](#build-m0). Accept when a stock piolib example runs without
error.
**Result:** TBD

### M1 — Test signal generator
```bash
pi5la --generator 1 --generator-only          # LED + resistor on GPIO 20: 1 Hz blink
pi5la --generator 100k --generator-only       # check with a scope / frequency meter
```
Verify without using the capture path.
**Result:** PASS (2026-09-16). `--generator 1` reported
`actual 1.000000 Hz (sqwave_slow)`, and an LED on GPIO 20 blinked with a
1 s period. The 100 kHz `sqwave` path has not been checked with a scope yet.

### M2 — Naive capture baseline
```bash
pi5la --mode naive --rate 10M --samples 400000 --raw naive.bin
```
Read the `throughput` line. Expect about 250 kB/s. RXSTALL will be set, which
is expected for this mode.
**Result:** TBD kB/s

### M3 — DMA capture
```bash
pi5la --rate 10M --samples 16000000 --raw dma.bin --out dma.sr --generator 100k
```
Accept when there are no transfer errors (sample correctness is M4).
**Result:** TBD MB/s, stall events TBD

### Naive vs DMA

| Mode | Rate | Throughput | Stall events |
|---|---|---|---|
| naive (M2) | 10 MSa/s | TBD | TBD |
| dma (M3) | 10 MSa/s | TBD | TBD |

### M4 — Correctness and bit order
1. `bitprobe --map`: prompts you to move the GPIO 20 jumper through GPIO 2..9
   one at a time. For each channel it prints the word bits that toggle and
   their chronological order. At the end it prints a `map_8ch` table.
2. Paste that table into `src/unpack.c`. The table currently holds the
   *theoretical* layout (slot 0 in bits 0–7, channel k at bit k), which
   still needs to be checked on hardware. Update the matching check in
   `tests/test_unpack.c` too.
3. Acceptance: `bitprobe --verify 0 --generator 100k --rate 10M` should show
   channel 0 with every run exactly 50 samples and every other channel with no
   transitions.

Offline analysis of a `--raw` file: `bitprobe --raw-in dma.bin --channels 8`.
**Result:** map TBD, verify TBD

### M5 — Rate sweep
```bash
ratesweep --out ratesweep.csv            # jumper GPIO 20 -> GPIO 2
```
Columns: `channels,sample_rate,samples_captured,violations,violation_pct`.
Only integer dividers are swept (200 MHz / {100,40,20,10,8,5,4,2,1}). The
generator is locked to the capture clock with a 25-sample half period.
**Result:** highest clean rate: 1 ch TBD, 2 ch TBD, 4 ch TBD, 8 ch TBD

### M6 — sigrok output
Capture a real 115200-baud UART **through the buffer described above**, open
it in PulseView on the Pi, add the UART decoder, and compare the decoded bytes
with what was sent.
```bash
pi5la --rate 10M --samples 10000000 --out uart.sr
```
**Result:** TBD

### M7 — Hardware trigger
```bash
pi5la --rate 10M --samples 1000000 --trigger rising:0 --out trig.sr
```
On an idle line this should block (Ctrl-C to abort) and start only when the
condition occurs. There is no pre-trigger buffer: the first captured sample is
the first one after the trigger.
**Result:** TBD

### M8 — RLE and pre-trigger buffer
Not implemented. It is planned only after M0–M7 pass.

---

## Repository layout

```
CMakeLists.txt
src/        main.c capture.c generator.c srzip.c unpack.c
            progbuild.c (runtime PIO programs) analyze.c (run-length checks) util.c
pio/        la8.pio la8_trig.pio sqwave.pio sqwave_slow.pio
tools/      ratesweep.c (M5) bitprobe.c (M4)
tests/      host unit tests (ctest)
```
