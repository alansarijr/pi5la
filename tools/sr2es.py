#!/usr/bin/env python3
"""
Turn captures into Elasticsearch documents for the wiresploit-poc1 index.

    capture (.sr, or a monitor session directory)
        -> sigrok-cli UART decode, one annotation per byte
        -> bytes grouped into messages by idle gap
        -> one document per message, written as NDJSON
        -> optional _bulk upload

Document fields, matching the index mapping:

    timestamp     date_nanos, when the message's first bit was sampled
    event_type    "serial"
    protocol      "UART"
    duration      nanoseconds from the first bit to the last
    length_bytes  bytes in the message
    payload_hex   the message's bytes, lowercase hex
    psf           protocol-specific fields, shaped like the network documents
                  already in the index: psf.psd.uart (baud, channel, framing,
                  text, errors) and psf.psd.capture (file, burst, samples)

Absolute time comes from the sidecar pi5la writes (<capture>.sr.meta.json) or,
for a monitor session, from index.json.

Credentials are never read from the command line: set ES_API_KEY, or ES_USER
and ES_PASSWORD, in the environment.

Examples:

    sr2es.py capture.sr --baud 115200
    sr2es.py ~/captures/mon-test --channel 0 --es-url https://es.example:9200
"""

import argparse
import datetime
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.request

INDEX = "wiresploit-poc1"
PROTOCOL = "UART"            # uppercase, like the "TCP" documents in the index
EVENT_TYPE = "onboard"       # what the index's existing UART documents use
DEFAULT_UART_TYPE = "TTL-3V3"   # what the 74LVC245 buffer feeds the Pi
MAX_PAYLOAD_HEX = 64 * 1024      # 32 kB of bytes; longer messages are truncated
MAX_TEXT = 2000                  # psf.text stays well under the 8191 ignore_above
DEFAULT_GAP_BYTES = 10.0         # idle byte-times that end a message

# sigrok-cli --protocol-decoder-samplenum prints either
#   "1234-5678 uart-1: 6a"            (0.7: no class name)
#   "1234-5678 uart-1: rx-data: 6a"   (with the class)
# so the class is optional.
ANNOTATION_RE = re.compile(
    r"^(?P<start>\d+)-(?P<end>\d+)\s+(?P<decoder>[\w-]+):\s+"
    r"(?:(?P<cls>[a-z][\w-]*):\s+)?(?P<text>.*)$")
HEX_RE = re.compile(r"\b[0-9A-Fa-f]{2}\b")
ERROR_TEXT_RE = re.compile(r"error|break|parity|framing", re.I)


class Error(Exception):
    pass


# --------------------------------------------------------------------------
# capture metadata


def load_meta(path):
    """
    Where a capture starts in wall-clock time and how fast it was sampled.

    Returns a list of (sr_path, start_time, samplerate_hz, extra_psf); one
    entry for a .sr file, one per burst for a monitor session directory.
    """
    if os.path.isdir(path):
        index_path = os.path.join(path, "index.json")
        try:
            with open(index_path) as f:
                index = json.load(f)
        except OSError as e:
            raise Error("cannot read %s: %s" % (index_path, e))
        rate = float(index["samplerate_hz"])
        items = []
        for burst in index.get("bursts", []):
            items.append((
                os.path.join(path, burst["file"]),
                float(burst["start_time"]),
                rate,
                {"burst": burst["file"], "gap_before_s": burst.get("gap_before_s")},
            ))
        if not items:
            raise Error("%s has no bursts" % path)
        return items

    meta_path = path + ".meta.json"
    try:
        with open(meta_path) as f:
            meta = json.load(f)
        return [(path, float(meta["start_time"]), float(meta["samplerate_hz"]), {})]
    except OSError:
        pass
    except (KeyError, ValueError) as e:
        raise Error("%s is malformed: %s" % (meta_path, e))

    # No sidecar (an older capture): fall back to the file's modification time
    # and ask sigrok for the rate. The timestamps are then approximate.
    rate = samplerate_of(path)
    sys.stderr.write("warning: no %s, using the file's mtime; timestamps are approximate\n"
                     % os.path.basename(meta_path))
    return [(path, os.path.getmtime(path), rate, {"time_source": "mtime"})]


def samplerate_of(path, sigrok="sigrok-cli"):
    """Sample rate from the .sr metadata, via sigrok-cli -i FILE --show."""
    try:
        out = subprocess.run([sigrok, "-i", path, "--show"], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, timeout=60).stdout.decode(
                                 "utf-8", "replace")
    except (OSError, subprocess.SubprocessError) as e:
        raise Error("cannot run %s: %s" % (sigrok, e))
    m = re.search(r"samplerate:\s*([\d.]+)\s*(k|M|G)?Hz", out, re.I)
    if not m:
        raise Error("cannot read the sample rate from %s" % path)
    scale = {"k": 1e3, "M": 1e6, "G": 1e9}.get(m.group(2) or "", 1.0)
    return float(m.group(1)) * scale


# --------------------------------------------------------------------------
# decoding


_ANNOTATION_CACHE = {}
# Error-ish classes worth keeping, if this sigrok build has them. Names differ
# between versions, so they are matched against what the decoder reports.
ERROR_CLASSES = ("rx-packet-error", "rx-parity-err", "rx-break", "error", "warning")


def uart_classes(sigrok="sigrok-cli"):
    """
    Annotation classes this sigrok's UART decoder actually has.

    Class names vary between versions (0.7's uart has no rx-packet-error, for
    example), and naming a missing one makes sigrok-cli fail outright, so ask
    it rather than assume.
    """
    if sigrok in _ANNOTATION_CACHE:
        return _ANNOTATION_CACHE[sigrok]
    try:
        out = subprocess.run([sigrok, "-P", "uart", "--show"], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT,
                             timeout=60).stdout.decode("utf-8", "replace")
    except (OSError, subprocess.SubprocessError) as e:
        raise Error("cannot run %s: %s" % (sigrok, e))
    available = set(re.findall(r"^\s*-?\s*([a-z0-9][a-z0-9-]*):", out, re.M))
    wanted = ["rx-data"] + [c for c in ERROR_CLASSES if c in available]
    if "rx-data" not in available:
        wanted = ["rx-data"]          # trust the default name and let sigrok complain
    _ANNOTATION_CACHE[sigrok] = wanted
    return wanted


def decode_uart(path, channel, baud, base_gpio=2, sigrok="sigrok-cli"):
    """Run sigrok-cli and return its annotation lines."""
    argv = [
        sigrok, "-i", path,
        "-P", "uart:rx=GPIO%d:baudrate=%d:format=hex" % (base_gpio + channel, baud),
        "-A", "uart=" + ":".join(uart_classes(sigrok)),
        "--protocol-decoder-samplenum",
    ]
    try:
        done = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=600)
    except (OSError, subprocess.SubprocessError) as e:
        raise Error("cannot run %s: %s" % (sigrok, e))
    if done.returncode != 0:
        raise Error("sigrok-cli failed: %s" %
                    done.stderr.decode("utf-8", "replace").strip()[:400])
    return done.stdout.decode("utf-8", "replace").splitlines()


def parse_annotations(lines):
    """
    Annotation lines -> [{start, end, byte, error}], in sample order.

    Unparsable lines are skipped: sigrok prints headers and other classes too.
    """
    events = []
    for line in lines:
        m = ANNOTATION_RE.match(line.strip())
        if not m:
            continue
        text = m.group("text").strip()
        cls = m.group("cls") or ""
        hexes = HEX_RE.findall(text)
        # Without a class name, an error shows only in the text ("frame error").
        is_error = bool(ERROR_TEXT_RE.search(cls)) or (not hexes and
                                                       bool(ERROR_TEXT_RE.search(text)))
        if not hexes and not is_error:
            continue
        events.append({
            "start": int(m.group("start")),
            "end": int(m.group("end")),
            "byte": int(hexes[0], 16) if hexes else None,
            "error": is_error,
            "text": text,
        })
    events.sort(key=lambda e: e["start"])
    return events


def group_messages(events, gap_samples):
    """Split the byte stream wherever the line is idle for longer than gap_samples."""
    messages = []
    current = []
    for event in events:
        if current and event["start"] - current[-1]["end"] > gap_samples:
            messages.append(current)
            current = []
        current.append(event)
    if current:
        messages.append(current)
    return messages


# --------------------------------------------------------------------------
# documents


def epoch_nanos(epoch_seconds):
    """
    Seconds since the epoch -> integer nanoseconds.

    A double holds only ~240 ns of resolution at 2026 epoch values, which is
    coarser than one sample at 50 MSa/s, so everything downstream works in
    integers. The sidecar carries microseconds, so that is the real limit on
    the capture's start time; sample offsets stay exact.
    """
    return int(round(epoch_seconds * 1e6)) * 1000


def iso_nanos(nanos):
    """date_nanos wants an ISO-8601 string; keep all nine digits."""
    whole, rest = divmod(int(nanos), 1000000000)
    stamp = datetime.datetime.fromtimestamp(whole, datetime.timezone.utc)
    return "%s.%09dZ" % (stamp.strftime("%Y-%m-%dT%H:%M:%S"), rest)


def printable(payload):
    out = []
    for b in payload:
        if b == 0x0a:
            out.append("\\n")
        elif b == 0x0d:
            out.append("\\r")
        elif b == 0x09:
            out.append("\\t")
        elif 0x20 <= b < 0x7f:
            out.append(chr(b))
        else:
            out.append("\\x%02x" % b)
    return "".join(out)[:MAX_TEXT]


def build_document(message, start_time, rate_hz, opts, extra_psf):
    """One decoded message -> one document matching the index mapping."""
    first, last = message[0], message[-1]
    payload = bytes(e["byte"] for e in message if e["byte"] is not None)
    errors = [e["text"] for e in message if e["error"]]

    hex_payload = payload.hex()
    truncated = len(hex_payload) > MAX_PAYLOAD_HEX
    if truncated:
        hex_payload = hex_payload[:MAX_PAYLOAD_HEX]

    # Same shape as the UART documents already in this index: psd is flat, with
    # uart_type / port / baud_rate / framing_error. Everything specific to a
    # logic-analyzer capture goes under psd.capture, where it cannot clash.
    psd = {
        "uart_type": opts.uart_type,
        "port": opts.port or ("ch%d" % opts.channel),
        "baud_rate": opts.baud,
        "data_bits": 8,
        "parity": "none",
        "stop_bits": 1,
        "framing_error": bool(errors),
        "capture": {
            "channel": opts.channel,
            "gpio": opts.base_gpio + opts.channel,
            "byte_count": len(payload),
            "text": printable(payload),
            "file": os.path.basename(opts.current_file),
            "start_sample": first["start"],
            "end_sample": last["end"],
            "samplerate_hz": int(rate_hz),
        },
    }
    if errors:
        psd["capture"]["errors"] = errors[:20]
    if truncated:
        psd["capture"]["payload_truncated"] = True
    psd["capture"].update({k: v for k, v in extra_psf.items() if v is not None})

    psf = {"psd": psd}

    return {
        "timestamp": iso_nanos(epoch_nanos(start_time) +
                               int(round(first["start"] * 1e9 / rate_hz))),
        "event_type": EVENT_TYPE,
        "protocol": PROTOCOL,
        "duration": int(round((last["end"] - first["start"]) / rate_hz * 1e9)),
        "length_bytes": len(payload),
        "payload_hex": hex_payload,
        "psf": psf,
    }


def documents_for(path, opts):
    docs = []
    for sr_path, start_time, rate_hz, extra_psf in load_meta(path):
        opts.current_file = sr_path
        lines = decode_uart(sr_path, opts.channel, opts.baud, opts.base_gpio, opts.sigrok)
        events = parse_annotations(lines)
        if not events:
            sys.stderr.write("%s: nothing decoded (check --channel and --baud)\n" % sr_path)
            continue
        gap_samples = opts.gap_bytes * 10.0 / opts.baud * rate_hz   # 10 bits per byte
        for message in group_messages(events, gap_samples):
            docs.append(build_document(message, start_time, rate_hz, opts, extra_psf))
    return docs


def write_ndjson(docs, path, index=INDEX):
    """Bulk format: an action line, then the document, for each doc."""
    with open(path, "w") as f:
        for doc in docs:
            f.write(json.dumps({"create": {"_index": index}}) + "\n")
            f.write(json.dumps(doc) + "\n")
    return path


# --------------------------------------------------------------------------
# upload


def auth_header():
    """Credentials come from the environment, never the command line."""
    api_key = os.environ.get("ES_API_KEY")
    if api_key:
        return "ApiKey " + api_key
    user, password = os.environ.get("ES_USER"), os.environ.get("ES_PASSWORD")
    if user and password:
        import base64
        return "Basic " + base64.b64encode(("%s:%s" % (user, password)).encode()).decode()
    return None


def bulk_upload(ndjson_path, es_url, index=INDEX, timeout=120, insecure=False):
    url = es_url.rstrip("/") + "/_bulk"
    with open(ndjson_path, "rb") as f:
        body = f.read()
    request = urllib.request.Request(url, data=body, method="POST")
    request.add_header("Content-Type", "application/x-ndjson")
    auth = auth_header()
    if auth:
        request.add_header("Authorization", auth)
    context = None
    if insecure and url.startswith("https"):
        import ssl
        context = ssl._create_unverified_context()
    try:
        with urllib.request.urlopen(request, timeout=timeout, context=context) as response:
            result = json.loads(response.read().decode("utf-8", "replace"))
    except urllib.error.HTTPError as e:
        raise Error("%s returned %d: %s" % (url, e.code,
                                            e.read().decode("utf-8", "replace")[:400]))
    except (urllib.error.URLError, OSError, ValueError) as e:
        raise Error("cannot reach %s: %s" % (url, e))

    failed = [item for item in result.get("items", [])
              if item.get("create", {}).get("status", 200) >= 300]
    return result, failed


# --------------------------------------------------------------------------


def main():
    p = argparse.ArgumentParser(
        description="Decode a pi5la capture into Elasticsearch documents",
        epilog="Credentials come from ES_API_KEY, or ES_USER and ES_PASSWORD.")
    p.add_argument("capture", help=".sr file, or a monitor session directory")
    p.add_argument("--baud", type=int, default=115200, help="UART baud rate (default 115200)")
    p.add_argument("--channel", type=int, default=0, help="capture channel, 0-7 (default 0)")
    p.add_argument("--base-gpio", type=int, default=2, dest="base_gpio",
                   help="GPIO of channel 0 (default 2)")
    p.add_argument("--gap-bytes", type=float, default=DEFAULT_GAP_BYTES, dest="gap_bytes",
                   help="idle byte-times that end a message (default %g)" % DEFAULT_GAP_BYTES)
    p.add_argument("--uart-type", default=DEFAULT_UART_TYPE, dest="uart_type",
                   help="psd.uart_type, e.g. TTL-3V3, RS-232, RS-485 (default %s)"
                        % DEFAULT_UART_TYPE)
    p.add_argument("--port", help="psd.port label for the probed line (default chN)")
    p.add_argument("--out", help="NDJSON output path (default: next to the capture)")
    p.add_argument("--index", default=INDEX, help="index name (default %s)" % INDEX)
    p.add_argument("--es-url", dest="es_url",
                   help="Elasticsearch base URL; without it, only the NDJSON is written")
    p.add_argument("--insecure", action="store_true",
                   help="skip TLS certificate checks (self-signed clusters)")
    p.add_argument("--sigrok", default="sigrok-cli", help="sigrok-cli command")
    p.add_argument("--print", action="store_true", dest="print_docs",
                   help="also print the documents")
    opts = p.parse_args()
    opts.current_file = opts.capture

    if not 0 <= opts.channel <= 7:
        p.error("--channel must be 0-7")
    if opts.baud <= 0:
        p.error("--baud must be positive")

    try:
        docs = documents_for(opts.capture, opts)
    except Error as e:
        sys.stderr.write("sr2es: %s\n" % e)
        return 1

    if not docs:
        sys.stderr.write("sr2es: no messages decoded\n")
        return 1

    out_path = opts.out or (opts.capture.rstrip("/") + ".ndjson")
    write_ndjson(docs, out_path, opts.index)
    total_bytes = sum(d["length_bytes"] for d in docs)
    print("%d message(s), %d byte(s) -> %s" % (len(docs), total_bytes, out_path))
    if opts.print_docs:
        for doc in docs:
            print(json.dumps(doc, indent=2))

    if not opts.es_url:
        print("no --es-url: upload it yourself, e.g.")
        print("  curl -H 'Content-Type: application/x-ndjson' --data-binary @%s "
              "<url>/_bulk" % out_path)
        return 0

    if not auth_header():
        sys.stderr.write("sr2es: no credentials; set ES_API_KEY, or ES_USER and "
                         "ES_PASSWORD\n")
        return 1
    try:
        result, failed = bulk_upload(out_path, opts.es_url, opts.index,
                                     insecure=opts.insecure)
    except Error as e:
        sys.stderr.write("sr2es: %s\n" % e)
        sys.stderr.write("sr2es: %s is kept; re-send it when the cluster is reachable\n"
                         % out_path)
        return 1

    if failed:
        sys.stderr.write("sr2es: %d of %d documents were rejected; first error: %s\n"
                         % (len(failed), len(docs),
                            json.dumps(failed[0]["create"].get("error", {}))[:300]))
        return 1
    print("uploaded %d document(s) to %s/%s in %s ms" %
          (len(docs), opts.es_url.rstrip("/"), opts.index, result.get("took", "?")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
