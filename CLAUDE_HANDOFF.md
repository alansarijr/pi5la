# Handoff: pi5la (Raspberry Pi 5 PIO logic analyzer)

Paste this into a new Claude chat, or attach it, to continue the project.

---

## How I want you to work

- **Ask me when anything is unclear. Don't make design decisions on your own.**
  Use multiple-choice questions for real choices, and give a recommendation.
- **You can't reach the Pi.** You write and compile code on my Windows PC. I copy it to
  the Pi, run the commands you give me, and paste the output back.
- Give short, exact commands. Tell me which terminal and which physical pin, and what
  output to expect.
- **Before any loopback test, have me confirm the wiring** with `pinctrl`. Wrong
  jumper pins caused most of our past confusion.
- **Hardware safety:** any external device must go through a 74LVC245 buffer. Never let
  me connect pins 1, 2 or 4 (3.3 V / 5 V) to a GPIO.

## Environment

- **PC:** Windows 11. Repo at `D:\CSE\wiresploit\pi5la` (git, local only, no remote).
  - The spec is at `D:\CSE\wiresploit\SPECS.md`.
  - Commit messages end with a `Co-Authored-By: Claude` line.
- **Compile-checking on the PC:** WSL Ubuntu 22.04 has a test tree in `~/pi5la-check`,
  with pioasm, a local piolib install, and libzip unpacked locally (no sudo). A rebuild
  script copies the repo in, runs cmake with
  `-DCMAKE_PREFIX_PATH=~/pi5la-check/prefix -DPIOASM_EXECUTABLE=~/pi5la-check/build-pioasm/pioasm`
  and `PKG_CONFIG_PATH` pointing at `~/pi5la-check/zroot`, then builds and runs `ctest`.
- **C: drive is nearly full** (~0.5 GB free). WSL sometimes fails with out-of-memory or
  "not enough space". If so, say so; don't retry in a loop.
- **Pi 5:** 8 GB RAM, Raspberry Pi OS Bookworm, kernel `6.18.50+rpt-rpi-2712`.
  - User `ansari`, IP `192.168.111.145`.
  - `/dev/pio0` is `root:gpio 0660`, and the user is in `gpio`, so no sudo is needed.
  - RP1 firmware `23fca1cd…` reports **op count 33**, so there is no `PIO_SM_GET_FLAGS`.
  - `dtparam=uart0=on` is set, giving `/dev/ttyAMA0` on GPIO 14.
  - Build dirs on the Pi: `~/utils/build`, `~/build-pioasm`, `~/pi5la`, `~/build-pi5la`.
- **Copying code to the Pi:** read-only `.git` objects make `scp` over an existing copy
  fail. Have me run `mv ~/pi5la ~/pi5la-oldN` on the Pi first, then
  `scp -r D:\CSE\wiresploit\pi5la ansari@192.168.111.145:~/`, then rebuild.
  If only source files changed, copying single files works too.

## Current state

The latest commit is `89f529e`, "Add --stream mode and uart-listen live UART sniffer".
See `PROJECT_SUMMARY.md` and `README.md` for details and all measurements.

**Verified on the Pi:**
- **M1:** generator.
- **M2:** naive capture, 396 kB/s.
- **M3:** cyclic DMA, 0 drops at 10 MSa/s, 10 MB/s.
- **M4:** bit map on all 8 channels.
- **M5:** sustained ceilings confirmed with long runs: 50 / 100 / 200 / 200 MSa/s at 8 / 4 / 2 / 1 channels
  (1/2/4-ch confirmed 2026-09-17). `ratesweep` CSV now has `stalls` and `status` (`ok`/`violations`/`nosignal`).
- **M7:** trigger.
- **M6:** UART loopback decoded byte-exact with sigrok-cli.

**Not yet verified:**
1. **`pi5la --stream` and `tools/uart-listen.sh`**, the live UART listener. They compile
   and pass argument-handling checks in WSL, but **have never run on the Pi**. The Pi's
   copy may be older than `89f529e`, so the first step is to copy and rebuild.
   Test: jumper pin 8 → pin 3, run `uart-listen.sh` in one terminal, then
   `printf 'hello\r\n' > /dev/ttyAMA0` in another.
2. **PulseView visual check** of `uart.sr` (M6).
3. **M0:** confirm `~/utils/build/piolib/examples/piopwm 4` ran without errors.
4. **ESP32:** I want to sniff its UART (115200 baud, powered from my PC). **It needs a
   74LVC245 buffer and a shared GND**, which I don't have wired yet.
5. ~~Open question: ratesweep no-signal rows~~ Done: `status=nosignal`, empty `violation_pct`
   (`status=ok` seen on the Pi 2026-09-17; the `nosignal` path not yet seen on hardware).

## Key technical decisions (and why)

- **Capture uses the kernel's cyclic DMA** (`PIO_IOC_SM_CONFIG_XFER_V2` with the
  `DMA_CYCLE` flag, reads via `PIO_IOC_SM_XFER_DATA32`), not piolib's `pio_sm_xfer_data`
  read-ahead. Read-ahead lost samples at every buffer hand-over (stalls = buffer count).
  - Ring: 4 periods; `--dma-buf` sets the period size (default 1 MiB).
  - `-EOVERFLOW` from a read means dropped samples.
- **Stall detection reads FDEBUG** at `0xf0000008` via `READ_HW` / `WRITE_HW` (RXSTALL is
  bit `sm`, write 1 to clear), on pi5la's own `/dev/pio0` fd. piolib's
  `pio_sm_get_flags` panics on this firmware.
- **PIO capture:** `in pins, N`, `shift_right = true`, autopush threshold 32, FIFO
  joined to RX, 32/N samples per word.
  - **Verified map:** slot 0 in the low bits, channel k at bit `s*N + k`.
  - Trigger programs are built at runtime (`progbuild.c`) and checked against the
    pioasm output of `la8.pio` / `la8_trig.pio`.
- **Generator:** `sqwave.pio` above ~1.53 kHz, `sqwave_slow.pio` (a delay loop) below.
- **Live mode:** `--stream` writes one byte per sample to stdout. `uart-listen.sh` pipes
  it into `sigrok-cli -I binary:numchannels=8:samplerate=<Hz> -i -`. In that input,
  channels are named `0`–`7`, not `GPIO2`.
- **Exit codes:** `0` OK, `1` error, `2` dropped samples (RXSTALL or ring overflow). The
  wall-clock loss estimate is informational only.
- M8 (RLE, pre-trigger) was skipped by agreement.

## Useful facts

- **Pins:**
  - Channels 0–7 = GPIO 2–9 = physical pins 3, 5, 7, 29, 31, 26, 24, 21.
  - Generator = GPIO 20 = pin 38.
  - UART TX = GPIO 14 = pin 8.
  - GND = pins 6, 9, 39, and others.
- **Idle levels:** GPIO 2–8 high (pull-ups), GPIO 9 low. An undriven GPIO 20 on a jumper
  idles high.
- **CMA pool is 64 MB** (`CmaTotal 65536 kB`): the largest DMA ring is 4 × 8 MiB.
  66.7 MB/s packed is unreliable at every `--dma-buf` (≤3/5 clean); 100 MB/s fails outright.
  50 MB/s is reliable: 8ch@50M, 4ch@100M, 2ch@200M each 5/5 clean (2026-09-17).
- **RP1 PIO clock is 200 MHz:** rate = 200 MHz / divider (int + frac/256). Integer
  dividers avoid jitter.
- **A capture of all `0x7f7f7f7f` words** means no signal reached any channel: check the
  jumper.
