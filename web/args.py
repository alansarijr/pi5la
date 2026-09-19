"""
Input validation and argv building for the pi5la web UI.

Everything here is pure: no Flask, no subprocess, no filesystem. The web app
turns form fields into argument lists through these functions, so nothing a
browser sends is ever handed to a shell. Unit tests live in
tests/test_web_args.py.

The rules mirror pi5la's own parsers: parse_si / parse_duration in src/util.c
and src/idle.c, and the option checks in src/main.c.
"""

import datetime
import re

MAX_GPIO = 28              # CAPTURE_GPIO_COUNT
VALID_CHANNELS = (1, 2, 4, 8)
PIO_CLOCK_HZ = 200e6
MIN_RATE_HZ = PIO_CLOCK_HZ / 65535.999   # largest divider
MAX_DMA_BUF = 8 << 20      # 4 x 8 MiB is the largest ring the 64 MB CMA pool allows
RELIABLE_BYTES_PER_S = 50e6  # see REPORT.md: 50 MB/s packed ran clean 15/15

CAPTURE_NAME_RE = re.compile(r"^[A-Za-z0-9._-]+\.sr$")
TRIGGERS = ("rising", "falling", "high", "low")
PROTOCOLS = ("uart", "i2c", "spi")


class InvalidInput(ValueError):
    """Raised with a message meant to be shown in the page."""


def _fail(what, value):
    raise InvalidInput("invalid %s: %r" % (what, value))


def parse_si(text, what="rate"):
    """Number with an optional k/M suffix, as src/util.c parse_si does."""
    if not isinstance(text, str):
        _fail(what, text)
    s = text.strip()
    mult = 1.0
    if s.endswith("k"):
        mult, s = 1e3, s[:-1]
    elif s.endswith("M"):
        mult, s = 1e6, s[:-1]
    if not re.fullmatch(r"\d+(\.\d+)?", s):
        _fail(what, text)
    return float(s) * mult


def parse_duration(text, what="duration"):
    """Seconds from '500ms', '10s', '2m', or a bare number of seconds."""
    if not isinstance(text, str):
        _fail(what, text)
    s = text.strip()
    mult = 1.0
    if s.endswith("ms"):
        mult, s = 1e-3, s[:-2]
    elif s.endswith("m"):
        mult, s = 60.0, s[:-1]
    elif s.endswith("s"):
        s = s[:-1]
    if not re.fullmatch(r"\d+(\.\d+)?", s):
        _fail(what, text)
    value = float(s) * mult
    if value <= 0:
        _fail(what, text)
    return value


def _int(value, what, lo, hi):
    try:
        n = int(str(value).strip())
    except (TypeError, ValueError):
        _fail(what, value)
    if n < lo or n > hi:
        raise InvalidInput("%s must be between %d and %d, got %d" % (what, lo, hi, n))
    return n


def new_capture_name(now=None):
    """Server-side name, so no client string ever becomes a path."""
    now = now or datetime.datetime.now()
    return now.strftime("cap-%Y%m%d-%H%M%S.sr")


def safe_capture_name(name):
    """A plain *.sr basename, with no path separators or traversal."""
    if not isinstance(name, str) or not CAPTURE_NAME_RE.fullmatch(name) or name.startswith("."):
        _fail("capture name", name)
    return name


def throughput_warning(rate_hz, channels):
    """Warn above the packed data rate that has been proven reliable."""
    packed = rate_hz * channels / 8.0
    if packed <= RELIABLE_BYTES_PER_S:
        return None
    return (
        "%.0f MB/s of packed data is above the 50 MB/s that ran clean 15/15 on this Pi; "
        "samples may be dropped (see REPORT.md)." % (packed / 1e6)
    )


def capture_command(form, pi5la="pi5la", out_path="capture.sr"):
    """
    Build the pi5la argv for a capture.

    form keys: rate (required), channels, base_gpio, duration (required),
    idle_stop, trigger, trigger_channel, dma_buf. Returns (argv, warnings).
    """
    rate_hz = parse_si(form.get("rate", ""), "rate")
    if rate_hz < MIN_RATE_HZ or rate_hz > PIO_CLOCK_HZ:
        raise InvalidInput(
            "rate must be between %.0f Hz and 200 MHz, got %.0f Hz" % (MIN_RATE_HZ, rate_hz))

    channels = _int(form.get("channels", 8), "channels", 1, 8)
    if channels not in VALID_CHANNELS:
        raise InvalidInput("channels must be 1, 2, 4 or 8, got %d" % channels)

    base = _int(form.get("base_gpio", 2), "base GPIO", 0, MAX_GPIO - 1)
    if base + channels > MAX_GPIO:
        raise InvalidInput(
            "GPIO %d..%d is past the last PIO GPIO (%d)" % (base, base + channels - 1, MAX_GPIO - 1))

    duration_s = parse_duration(form.get("duration", ""), "duration")

    argv = [
        pi5la,
        "--rate", form["rate"].strip(),
        "--channels", str(channels),
        "--base-gpio", str(base),
        "--duration", form["duration"].strip(),
        "--out", out_path,
    ]

    idle = form.get("idle_stop", "")
    if idle and idle.strip():
        parse_duration(idle, "idle stop")   # validate only; pi5la parses it again
        argv += ["--idle-stop", idle.strip()]

    trigger = form.get("trigger", "")
    if trigger and trigger.strip() and trigger != "none":
        if trigger not in TRIGGERS:
            _fail("trigger", trigger)
        ch = _int(form.get("trigger_channel", 0), "trigger channel", 0, 7)
        if ch >= channels:
            raise InvalidInput("trigger channel %d is not below channels %d" % (ch, channels))
        argv += ["--trigger", "%s:%d" % (trigger, ch)]

    dma_buf = form.get("dma_buf", "")
    if dma_buf and str(dma_buf).strip():
        n = _int(dma_buf, "DMA buffer", 4, MAX_DMA_BUF)
        if n % 4:
            raise InvalidInput("DMA buffer must be a multiple of 4, got %d" % n)
        argv += ["--dma-buf", str(n)]

    warnings = []
    warning = throughput_warning(rate_hz, channels)
    if warning:
        warnings.append(warning)
    if duration_s * rate_hz * channels / 8 > 2e9:
        warnings.append("this capture needs over 2 GB of RAM on the Pi; shorten the duration.")
    return argv, warnings


def live_commands(form, pi5la="pi5la", sigrok="sigrok-cli"):
    """
    Build the two argv lists for live decoding: pi5la --stream piped into
    sigrok-cli. Same pipeline as tools/uart-listen.sh, without the shell.

    In the binary input, channels are named 0..7, not GPIO2..GPIO9.
    """
    rate_hz = parse_si(form.get("rate", ""), "rate")
    if rate_hz < MIN_RATE_HZ or rate_hz > PIO_CLOCK_HZ:
        raise InvalidInput("rate must be between %.0f Hz and 200 MHz" % MIN_RATE_HZ)
    channels = _int(form.get("channels", 8), "channels", 1, 8)
    if channels not in VALID_CHANNELS:
        raise InvalidInput("channels must be 1, 2, 4 or 8, got %d" % channels)
    base = _int(form.get("base_gpio", 2), "base GPIO", 0, MAX_GPIO - 1)
    if base + channels > MAX_GPIO:
        raise InvalidInput("GPIO %d..%d is past the last PIO GPIO" % (base, base + channels - 1))

    capture = [
        pi5la,
        "--rate", form["rate"].strip(),
        "--channels", str(channels),
        "--base-gpio", str(base),
        "--stream",
    ]
    decoder = _decoder_spec(form, channels, name=lambda ch: str(ch))
    decode = [
        sigrok,
        "-I", "binary:numchannels=%d:samplerate=%d" % (channels, round(rate_hz)),
        "-i", "-",
        "-P", decoder,
    ] + _annotation_args(form.get("protocol", "uart"))
    warnings = []
    warning = throughput_warning(rate_hz, channels)
    if warning:
        warnings.append(warning)
    warnings.append(
        "sigrok's decoders run in Python: above a few MSa/s the pipe falls behind and the "
        "DMA ring overflows.")
    return capture, decode, warnings


def decode_command(path, form, channels=8, base_gpio=2, sigrok="sigrok-cli"):
    """argv to decode a saved .sr file. Channels there are named GPIO<n>."""
    decoder = _decoder_spec(form, channels, name=lambda ch: "GPIO%d" % (base_gpio + ch))
    return [sigrok, "-i", path, "-P", decoder] + _annotation_args(form.get("protocol", "uart"))


def _channel(form, key, channels, required=True, default=None):
    value = form.get(key, "")
    if value is None or str(value).strip() == "":
        if required:
            raise InvalidInput("%s channel is required" % key)
        return default
    ch = _int(value, "%s channel" % key, 0, 7)
    if ch >= channels:
        raise InvalidInput("%s channel %d is not below channels %d" % (key, ch, channels))
    return ch


def _decoder_spec(form, channels, name):
    """sigrok -P argument for the chosen protocol, e.g. uart:rx=GPIO2:baudrate=115200."""
    protocol = form.get("protocol", "uart")
    if protocol not in PROTOCOLS:
        _fail("protocol", protocol)

    if protocol == "uart":
        rx = _channel(form, "rx", channels)
        baud = _int(form.get("baudrate", 115200), "baud rate", 50, 20000000)
        return "uart:rx=%s:baudrate=%d" % (name(rx), baud)

    if protocol == "i2c":
        scl = _channel(form, "scl", channels)
        sda = _channel(form, "sda", channels)
        if scl == sda:
            raise InvalidInput("SCL and SDA must be different channels")
        return "i2c:scl=%s:sda=%s" % (name(scl), name(sda))

    clk = _channel(form, "clk", channels)
    spec = "spi:clk=%s" % name(clk)
    used = [clk]
    for key in ("mosi", "miso", "cs"):
        ch = _channel(form, key, channels, required=False)
        if ch is None:
            continue
        if ch in used:
            raise InvalidInput("SPI channels must all be different")
        used.append(ch)
        spec += ":%s=%s" % (key, name(ch))
    if len(used) == 1:
        raise InvalidInput("SPI needs MOSI or MISO as well as CLK")
    return spec


def _annotation_args(protocol):
    if protocol == "uart":
        return ["-B", "uart=rx"]
    if protocol == "i2c":
        return ["-A", "i2c"]
    return ["-A", "spi"]
