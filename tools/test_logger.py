import json
import os
import sqlite3
import tempfile
import unittest
from unittest import mock

import logger


SAMPLE_DATA = {
    "temp": 26.8, "humi": 30.6, "lux": 64.2,
    "voc": 64, "nox": 1,
    "sraw_voc": 27941, "sraw_nox": 12687,
    "c2": 1, "c3": 0,
}
SAMPLE_FFT = json.dumps([{"freq": i * 125, "mag": 10.0} for i in range(32)])
SAMPLE_SYS = {"m": 0, "ip": "172.16.100.54", "ss": "robot-1"}


class TestDb(unittest.TestCase):
    def setUp(self):
        fd, self.path = tempfile.mkstemp(suffix=".db")
        os.close(fd)
        self.addCleanup(os.remove, self.path)
        self.conn = logger.init_db(self.path)
        self.addCleanup(self.conn.close)

    def test_wal_mode_enabled(self):
        mode = self.conn.execute("PRAGMA journal_mode").fetchone()[0]
        self.assertEqual(mode.lower(), "wal")

    def test_schema_created(self):
        names = {r[0] for r in self.conn.execute(
            "SELECT name FROM sqlite_master WHERE type IN ('table','index')")}
        self.assertIn("readings", names)
        self.assertIn("idx_readings_ts", names)

    def test_insert_and_read_back(self):
        row = logger.build_row(1751900000.0, SAMPLE_DATA, SAMPLE_FFT, SAMPLE_SYS)
        logger.insert_reading(self.conn, row)
        got = self.conn.execute(
            "SELECT ts, temp, humi, lux, voc, nox, sraw_voc, sraw_nox,"
            " has_bh, has_sgp, fft_json, wifi_mode, ip, ssid FROM readings"
        ).fetchone()
        self.assertEqual(got[0], 1751900000.0)
        self.assertEqual(got[1], 26.8)
        self.assertEqual(got[4], 64)
        self.assertEqual(got[8], 1)   # has_bh <- c2
        self.assertEqual(got[9], 0)   # has_sgp <- c3
        self.assertEqual(len(json.loads(got[10])), 32)
        self.assertEqual(got[12], "172.16.100.54")
        self.assertEqual(got[13], "robot-1")


class TestScheduling(unittest.TestCase):
    def test_normal_advance(self):
        # 사이클이 interval 안에 끝난 경우: 단순히 +interval
        self.assertEqual(logger.next_deadline(10.0, 10.4, 1.0), 11.0)

    def test_skip_missed_ticks(self):
        # 사이클이 2.5초 걸린 경우(예: 타임아웃): 11.0, 12.0 tick은 버리고 13.0
        self.assertEqual(logger.next_deadline(10.0, 12.5, 1.0), 13.0)

    def test_boundary_exactly_on_tick(self):
        # now가 정확히 다음 tick 위: 그 tick은 이미 지난 것으로 보고 다음으로
        self.assertEqual(logger.next_deadline(10.0, 11.0, 1.0), 12.0)

    def test_fractional_interval(self):
        self.assertAlmostEqual(logger.next_deadline(10.0, 10.1, 0.5), 10.5)


class TestPolling(unittest.TestCase):
    def test_poll_once_hits_three_endpoints(self):
        responses = {
            "http://h/data": json.dumps(SAMPLE_DATA),
            "http://h/fft": SAMPLE_FFT,
            "http://h/sys": json.dumps(SAMPLE_SYS),
        }
        with mock.patch.object(logger, "fetch_text",
                               side_effect=lambda url, timeout=3.0: responses[url]) as m:
            data, fft_text, sysinfo = logger.poll_once("http://h")
        self.assertEqual(data["temp"], 26.8)
        self.assertEqual(json.loads(fft_text)[0]["freq"], 0)
        self.assertEqual(sysinfo["ss"], "robot-1")
        self.assertEqual(m.call_count, 3)

    def test_poll_once_propagates_error(self):
        with mock.patch.object(logger, "fetch_text", side_effect=OSError("timed out")):
            with self.assertRaises(OSError):
                logger.poll_once("http://h")

    def test_poll_once_rejects_invalid_fft(self):
        responses = {
            "http://h/data": json.dumps(SAMPLE_DATA),
            "http://h/fft": "<html>not json</html>",
            "http://h/sys": json.dumps(SAMPLE_SYS),
        }
        with mock.patch.object(logger, "fetch_text",
                               side_effect=lambda url, timeout=3.0: responses[url]):
            with self.assertRaises(json.JSONDecodeError):
                logger.poll_once("http://h")


if __name__ == "__main__":
    unittest.main()
