"""Unit tests for web/args.py. Run: python3 -m unittest tests/test_web_args.py"""

import datetime
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "web"))

import args  # noqa: E402


class TestParsing(unittest.TestCase):
    def test_si(self):
        self.assertEqual(args.parse_si("10M"), 10e6)
        self.assertEqual(args.parse_si("115.2k"), 115200)
        self.assertEqual(args.parse_si(" 4000 "), 4000)
        for bad in ("", "10G", "-5", "5kk", "M", None, "1e6"):
            with self.assertRaises(args.InvalidInput):
                args.parse_si(bad)

    def test_duration(self):
        self.assertEqual(args.parse_duration("10s"), 10)
        self.assertEqual(args.parse_duration("10"), 10)
        self.assertAlmostEqual(args.parse_duration("500ms"), 0.5)
        self.assertEqual(args.parse_duration("1.5m"), 90)
        for bad in ("0s", "", "5h", "-1s", "s", "1 s"):
            with self.assertRaises(args.InvalidInput):
                args.parse_duration(bad)

    def test_capture_names(self):
        stamp = datetime.datetime(2026, 9, 19, 14, 5, 6)
        self.assertEqual(args.new_capture_name(stamp), "cap-20260919-140506.sr")
        self.assertEqual(args.safe_capture_name("cap-1.sr"), "cap-1.sr")
        for bad in ("../etc/passwd.sr", "a/b.sr", "cap.txt", ".sr", "", "cap.sr\n", None):
            with self.assertRaises(args.InvalidInput):
                args.safe_capture_name(bad)


class TestCaptureCommand(unittest.TestCase):
    def base_form(self, **kw):
        form = {"rate": "2M", "channels": "8", "duration": "60s"}
        form.update(kw)
        return form

    def test_minimal(self):
        argv, warnings = args.capture_command(self.base_form(), pi5la="/bin/pi5la",
                                              out_path="/c/x.sr")
        self.assertEqual(argv, ["/bin/pi5la", "--rate", "2M", "--channels", "8",
                                "--base-gpio", "2", "--duration", "60s", "--out", "/c/x.sr"])
        self.assertEqual(warnings, [])

    def test_all_options(self):
        argv, _ = args.capture_command(self.base_form(
            idle_stop="1s", trigger="falling", trigger_channel="3", dma_buf="131072",
            base_gpio="4", channels="4"))
        self.assertIn("--idle-stop", argv)
        self.assertEqual(argv[argv.index("--idle-stop") + 1], "1s")
        self.assertEqual(argv[argv.index("--trigger") + 1], "falling:3")
        self.assertEqual(argv[argv.index("--dma-buf") + 1], "131072")
        self.assertEqual(argv[argv.index("--base-gpio") + 1], "4")

    def test_rejects_bad_input(self):
        cases = [
            {"channels": "3"},                       # not 1/2/4/8
            {"channels": "0"},
            {"rate": "400M"},                        # above the PIO clock
            {"rate": "1"},                           # below the slowest divider
            {"rate": "2M; rm -rf /"},                # shell metacharacters
            {"duration": ""},
            {"duration": "0s"},
            {"base_gpio": "27"},                     # 27 + 8 channels is past GPIO 27
            {"base_gpio": "-1"},
            {"trigger": "sideways"},
            {"trigger": "rising", "trigger_channel": "9"},
            {"channels": "4", "trigger": "rising", "trigger_channel": "5"},
            {"dma_buf": "3"},                        # not a multiple of 4
            {"dma_buf": "134217728"},                # past the CMA pool
        ]
        for extra in cases:
            with self.assertRaises(args.InvalidInput, msg=extra):
                args.capture_command(self.base_form(**extra))

    def test_throughput_warning(self):
        self.assertIsNone(args.throughput_warning(50e6, 8))
        self.assertIsNotNone(args.throughput_warning(66.6e6, 8))
        _, warnings = args.capture_command(self.base_form(rate="100M", duration="1s"))
        self.assertTrue(any("50 MB/s" in w for w in warnings))

    def test_memory_warning(self):
        _, warnings = args.capture_command(self.base_form(rate="50M", duration="60s"))
        self.assertTrue(any("RAM" in w for w in warnings))


class TestDecoders(unittest.TestCase):
    def test_uart_saved_file(self):
        argv = args.decode_command("/c/cap.sr", {"protocol": "uart", "rx": "0",
                                                 "baudrate": "115200"})
        self.assertEqual(argv, ["sigrok-cli", "-i", "/c/cap.sr", "-P",
                                "uart:rx=GPIO2:baudrate=115200", "-B", "uart=rx"])

    def test_i2c_and_spi(self):
        i2c = args.decode_command("/c/a.sr", {"protocol": "i2c", "scl": "0", "sda": "1"})
        self.assertIn("i2c:scl=GPIO2:sda=GPIO3", i2c)
        spi = args.decode_command("/c/a.sr", {"protocol": "spi", "clk": "0", "mosi": "1",
                                              "cs": "3"})
        self.assertIn("spi:clk=GPIO2:mosi=GPIO3:cs=GPIO5", spi)

    def test_live_uses_numeric_channel_names(self):
        capture, decode, warnings = args.live_commands(
            {"rate": "2M", "channels": "8", "protocol": "uart", "rx": "0",
             "baudrate": "115200"}, pi5la="/bin/pi5la")
        self.assertEqual(capture, ["/bin/pi5la", "--rate", "2M", "--channels", "8",
                                   "--base-gpio", "2", "--stream"])
        self.assertIn("binary:numchannels=8:samplerate=2000000", decode)
        self.assertIn("uart:rx=0:baudrate=115200", decode)
        self.assertTrue(any("decoders run in Python" in w for w in warnings))

    def test_rejects_bad_decoders(self):
        cases = [
            {"protocol": "morse", "rx": "0"},
            {"protocol": "uart"},                                  # no rx
            {"protocol": "uart", "rx": "8"},                       # out of range
            {"protocol": "uart", "rx": "0", "baudrate": "0"},
            {"protocol": "i2c", "scl": "0", "sda": "0"},           # same channel
            {"protocol": "i2c", "scl": "0"},                       # no sda
            {"protocol": "spi", "clk": "0"},                       # no data channel
            {"protocol": "spi", "clk": "0", "mosi": "0"},          # duplicate channel
        ]
        for form in cases:
            with self.assertRaises(args.InvalidInput, msg=form):
                args.decode_command("/c/a.sr", form)


if __name__ == "__main__":
    unittest.main()
