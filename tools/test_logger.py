import io
import itertools
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


class TestMainLoop(unittest.TestCase):
    """End-to-end coverage for main()'s poll/sleep/insert loop, with mocked
    time and network but a real temp SQLite DB (real init_db/build_row/
    insert_reading)."""

    def setUp(self):
        fd, self.path = tempfile.mkstemp(suffix=".db")
        os.close(fd)
        self.addCleanup(os.remove, self.path)

    def _run_main(self, poll_side_effect, extra_patches=()):
        monotonic_counter = itertools.count(0.0, 1.0)
        patches = [
            mock.patch.object(logger, "poll_once", side_effect=poll_side_effect),
            mock.patch.object(logger.time, "sleep"),
            mock.patch.object(logger.time, "monotonic",
                               side_effect=lambda: next(monotonic_counter)),
            mock.patch.object(logger.time, "time", return_value=1751900000.0),
        ] + list(extra_patches)
        stderr = io.StringIO()
        with mock.patch("sys.stderr", stderr):
            for p in patches:
                p.start()
            try:
                rc = logger.main(["--db", self.path, "--interval", "1.0"])
            finally:
                for p in patches:
                    p.stop()
        return rc, stderr.getvalue()

    def test_runs_iterations_and_survives_poll_failure(self):
        sample2 = dict(SAMPLE_DATA, temp=27.1)
        poll_effects = [
            (SAMPLE_DATA, SAMPLE_FFT, SAMPLE_SYS),   # iteration 1: success
            OSError("boom"),                          # iteration 2: poll fails
            (sample2, SAMPLE_FFT, SAMPLE_SYS),        # iteration 3: success
            KeyboardInterrupt(),                      # iteration 4: stop the loop
        ]
        rc, err = self._run_main(poll_effects)

        self.assertEqual(rc, 0)
        self.assertIn("[warn]", err)

        conn = sqlite3.connect(self.path)
        try:
            rows = conn.execute(
                "SELECT temp, humi, voc, nox FROM readings ORDER BY id"
            ).fetchall()
        finally:
            conn.close()

        # Only the 2 successful cycles should have inserted a row; the
        # failed poll must not leave a row behind.
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[0], (SAMPLE_DATA["temp"], SAMPLE_DATA["humi"],
                                    SAMPLE_DATA["voc"], SAMPLE_DATA["nox"]))
        self.assertEqual(rows[1], (sample2["temp"], sample2["humi"],
                                    sample2["voc"], sample2["nox"]))

    def test_survives_insert_failure(self):
        poll_effects = [
            (SAMPLE_DATA, SAMPLE_FFT, SAMPLE_SYS),   # iteration 1: success
            (SAMPLE_DATA, SAMPLE_FFT, SAMPLE_SYS),   # iteration 2: success
            KeyboardInterrupt(),                      # iteration 3: stop the loop
        ]
        insert_effects = [sqlite3.OperationalError("database is locked"), None]
        extra = [mock.patch.object(logger, "insert_reading",
                                    side_effect=insert_effects)]
        rc, err = self._run_main(poll_effects, extra_patches=extra)

        self.assertEqual(rc, 0)
        self.assertIn("[warn]", err)


class TestArgValidation(unittest.TestCase):
    def test_non_positive_interval_rejected(self):
        fd, path = tempfile.mkstemp(suffix=".db")
        os.close(fd)
        self.addCleanup(os.remove, path)
        with mock.patch("sys.stderr", io.StringIO()):
            self.assertRaises(
                SystemExit, logger.main,
                ["--interval", "0", "--db", path])


if __name__ == "__main__":
    unittest.main()
