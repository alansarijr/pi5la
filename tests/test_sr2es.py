"""Unit tests for tools/sr2es.py. Run: python3 -m unittest tests/test_sr2es.py"""

import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))

import sr2es  # noqa: E402


class Opts:
    """Stand-in for the parsed command line."""
    baud = 115200
    channel = 0
    base_gpio = 2
    gap_bytes = 10.0
    current_file = "/captures/cap.sr"


class TestParsing(unittest.TestCase):
    def test_annotation_lines_with_class(self):
        lines = [
            "sigrok-cli 0.7.2",                       # header, ignored
            "100-1000 uart-1: rx-data: 68",
            "1100-2000 uart-1: rx-data: 65",
            "not an annotation",
            "2100-3000 uart-1: rx-packet-error: frame error",
        ]
        events = sr2es.parse_annotations(lines)
        self.assertEqual(len(events), 3)
        self.assertEqual(events[0], {"start": 100, "end": 1000, "byte": 0x68,
                                     "error": False, "text": "68"})
        self.assertTrue(events[2]["error"])

    def test_annotation_lines_without_class(self):
        """sigrok 0.7 on the Pi prints no class name: '20018-20157 uart-1: 68'."""
        lines = [
            "20018-20157 uart-1: 68",
            "20191-20330 uart-1: 6C",                 # uppercase hex
            "20365-20504 uart-1: frame error",
        ]
        events = sr2es.parse_annotations(lines)
        self.assertEqual(len(events), 3)
        self.assertEqual(events[0]["byte"], 0x68)
        self.assertEqual(events[0]["start"], 20018)
        self.assertEqual(events[1]["byte"], 0x6C)
        self.assertTrue(events[2]["error"])
        self.assertIsNone(events[2]["byte"])

    def test_out_of_order_lines_are_sorted(self):
        events = sr2es.parse_annotations(["500-600 uart-1: rx-data: 02",
                                          "100-200 uart-1: rx-data: 01"])
        self.assertEqual([e["byte"] for e in events], [1, 2])

    def test_grouping_by_gap(self):
        events = [
            {"start": 0, "end": 100, "byte": 1, "error": False, "text": "01"},
            {"start": 150, "end": 250, "byte": 2, "error": False, "text": "02"},
            {"start": 5000, "end": 5100, "byte": 3, "error": False, "text": "03"},
        ]
        messages = sr2es.group_messages(events, gap_samples=1000)
        self.assertEqual([len(m) for m in messages], [2, 1])
        # A larger gap threshold keeps everything together.
        self.assertEqual(len(sr2es.group_messages(events, gap_samples=10000)), 1)


class TestDocuments(unittest.TestCase):
    def message(self, payload, start=1000, bit_samples=100):
        events = []
        pos = start
        for b in payload:
            events.append({"start": pos, "end": pos + 10 * bit_samples, "byte": b,
                           "error": False, "text": "%02x" % b})
            pos += 10 * bit_samples
        return events

    def test_fields_match_the_mapping(self):
        # 2 MSa/s, message starts 1000 samples (0.5 ms) into the capture.
        doc = sr2es.build_document(self.message(b"hi"), start_time=1000000000.0,
                                   rate_hz=2e6, opts=Opts(), extra_psf={})
        self.assertEqual(sorted(doc), ["duration", "event_type", "length_bytes",
                                       "payload_hex", "protocol", "psf", "timestamp"])
        self.assertEqual(doc["protocol"], "uart")
        self.assertEqual(doc["event_type"], "message")
        self.assertEqual(doc["payload_hex"], "6869")
        self.assertEqual(doc["length_bytes"], 2)
        # 2 bytes x 10 bits x 100 samples = 2000 samples = 1 ms = 1e6 ns
        self.assertEqual(doc["duration"], 1000000)
        self.assertEqual(doc["timestamp"], "2001-09-09T01:46:40.000500000Z")

    def test_psf_carries_protocol_details(self):
        doc = sr2es.build_document(self.message(b"ok\r\n"), 1e9, 2e6, Opts(),
                                   {"burst": "burst-0001.sr", "gap_before_s": 2.95})
        psf = doc["psf"]
        self.assertEqual(psf["baud"], 115200)
        self.assertEqual(psf["channel"], 0)
        self.assertEqual(psf["gpio"], 2)
        self.assertEqual(psf["byte_count"], 4)
        self.assertEqual(psf["text"], "ok\\r\\n")
        self.assertEqual(psf["source_file"], "cap.sr")
        self.assertEqual(psf["burst"], "burst-0001.sr")
        self.assertEqual(psf["gap_before_s"], 2.95)
        self.assertNotIn("errors", psf)

    def test_errors_change_the_event_type(self):
        message = self.message(b"x")
        message.append({"start": 3000, "end": 4000, "byte": None, "error": True,
                        "text": "frame error"})
        doc = sr2es.build_document(message, 1e9, 2e6, Opts(), {})
        self.assertEqual(doc["event_type"], "error")
        self.assertEqual(doc["psf"]["errors"], ["frame error"])
        self.assertEqual(doc["length_bytes"], 1)

    def test_long_payloads_are_truncated(self):
        doc = sr2es.build_document(self.message(b"A" * 40000), 1e9, 2e6, Opts(), {})
        self.assertEqual(len(doc["payload_hex"]), sr2es.MAX_PAYLOAD_HEX)
        self.assertTrue(doc["psf"]["payload_truncated"])
        self.assertLessEqual(len(doc["psf"]["text"]), sr2es.MAX_TEXT)
        self.assertEqual(doc["length_bytes"], 40000)     # the true length is kept

    def test_iso_nanos(self):
        self.assertEqual(sr2es.iso_nanos(0), "1970-01-01T00:00:00.000000000Z")
        self.assertEqual(sr2es.iso_nanos(1000000001), "1970-01-01T00:00:01.000000001Z")
        self.assertEqual(sr2es.epoch_nanos(1.5), 1500000000)
        # A double cannot hold nanoseconds at epoch scale, so the timestamp is
        # built from integers: this must be exact, not 36 ns out.
        self.assertEqual(sr2es.epoch_nanos(1789986306.5) + 500000,
                         1789986306500500000)

    def test_printable(self):
        self.assertEqual(sr2es.printable(b"ab\r\n\x00\xff"), "ab\\r\\n\\x00\\xff")


class TestBulkFile(unittest.TestCase):
    def test_ndjson_is_action_then_document(self):
        docs = [sr2es.build_document([{"start": 0, "end": 10, "byte": 0x41,
                                       "error": False, "text": "41"}],
                                     1e9, 2e6, Opts(), {})]
        with tempfile.TemporaryDirectory() as d:
            path = sr2es.write_ndjson(docs, os.path.join(d, "out.ndjson"))
            with open(path) as f:
                lines = f.read().splitlines()
        self.assertEqual(len(lines), 2)
        self.assertEqual(json.loads(lines[0]), {"create": {"_index": "wiresploit-poc1"}})
        self.assertEqual(json.loads(lines[1])["payload_hex"], "41")


class TestMetadata(unittest.TestCase):
    def test_sidecar_is_used(self):
        with tempfile.TemporaryDirectory() as d:
            sr = os.path.join(d, "cap.sr")
            with open(sr, "wb") as f:
                f.write(b"not really a zip")
            with open(sr + ".meta.json", "w") as f:
                json.dump({"samplerate_hz": 2000000, "start_time": 1789986306.5,
                           "start_sample": 0, "samples": 100}, f)
            items = sr2es.load_meta(sr)
        self.assertEqual(items, [(sr, 1789986306.5, 2000000.0, {})])

    def test_monitor_session(self):
        with tempfile.TemporaryDirectory() as d:
            for name in ("burst-0001.sr", "burst-0002.sr"):
                with open(os.path.join(d, name), "wb") as f:
                    f.write(b"x")
            with open(os.path.join(d, "index.json"), "w") as f:
                json.dump({"samplerate_hz": 2000000, "channels": 8, "bursts": [
                    {"file": "burst-0001.sr", "start_time": 100.0, "gap_before_s": 2.9},
                    {"file": "burst-0002.sr", "start_time": 103.0, "gap_before_s": 3.0},
                ]}, f)
            items = sr2es.load_meta(d)
        self.assertEqual(len(items), 2)
        self.assertEqual(items[1][1], 103.0)
        self.assertEqual(items[1][3]["burst"], "burst-0002.sr")

    def test_missing_index_is_an_error(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(sr2es.Error):
                sr2es.load_meta(d)


class TestAuth(unittest.TestCase):
    def test_api_key_and_basic(self):
        saved = {k: os.environ.pop(k, None) for k in ("ES_API_KEY", "ES_USER", "ES_PASSWORD")}
        try:
            self.assertIsNone(sr2es.auth_header())
            os.environ["ES_USER"], os.environ["ES_PASSWORD"] = "u", "p"
            self.assertEqual(sr2es.auth_header(), "Basic dTpw")
            os.environ["ES_API_KEY"] = "abc123"
            self.assertEqual(sr2es.auth_header(), "ApiKey abc123")   # API key wins
        finally:
            for k, v in saved.items():
                os.environ.pop(k, None)
                if v is not None:
                    os.environ[k] = v


if __name__ == "__main__":
    unittest.main()
