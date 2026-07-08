import json
import os
import sqlite3
import tempfile
import unittest

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


if __name__ == "__main__":
    unittest.main()
