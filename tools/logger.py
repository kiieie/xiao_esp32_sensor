#!/usr/bin/env python3
"""Poll the ESP32 sensor hub HTTP API and log readings to SQLite.

Endpoints polled each cycle: /data (sensor snapshot), /fft (32-band
spectrum, stored as raw JSON text), /sys (wifi mode / ip / ssid).
"""
import argparse
import json
import sqlite3
import sys
import time
import urllib.request

SCHEMA = """
CREATE TABLE IF NOT EXISTS readings (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ts REAL NOT NULL,
    temp REAL, humi REAL, lux REAL,
    voc INTEGER, nox INTEGER,
    sraw_voc INTEGER, sraw_nox INTEGER,
    has_bh INTEGER, has_sgp INTEGER,
    fft_json TEXT,
    wifi_mode INTEGER, ip TEXT, ssid TEXT
);
CREATE INDEX IF NOT EXISTS idx_readings_ts ON readings(ts);
"""

INSERT_SQL = """INSERT INTO readings
    (ts, temp, humi, lux, voc, nox, sraw_voc, sraw_nox,
     has_bh, has_sgp, fft_json, wifi_mode, ip, ssid)
    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"""


def init_db(path):
    conn = sqlite3.connect(path)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA busy_timeout=3000")
    conn.executescript(SCHEMA)
    conn.commit()
    return conn


def build_row(ts, data, fft_text, sysinfo):
    return (
        ts,
        data.get("temp"), data.get("humi"), data.get("lux"),
        data.get("voc"), data.get("nox"),
        data.get("sraw_voc"), data.get("sraw_nox"),
        data.get("c2"), data.get("c3"),
        fft_text,
        sysinfo.get("m"), sysinfo.get("ip"), sysinfo.get("ss"),
    )


def insert_reading(conn, row):
    conn.execute(INSERT_SQL, row)
    conn.commit()


def next_deadline(deadline, now, interval):
    """Return the next future tick, skipping any ticks missed while busy."""
    deadline += interval
    if deadline <= now:
        missed = int((now - deadline) // interval) + 1
        deadline += missed * interval
    return deadline


def fetch_text(url, timeout=3.0):
    with urllib.request.urlopen(url, timeout=timeout) as resp:
        return resp.read().decode("utf-8")


def poll_once(base_url, timeout=3.0):
    data = json.loads(fetch_text(base_url + "/data", timeout))
    fft_text = fetch_text(base_url + "/fft", timeout)
    json.loads(fft_text)  # reject non-JSON payloads early
    sysinfo = json.loads(fetch_text(base_url + "/sys", timeout))
    return data, fft_text, sysinfo


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="172.16.100.54", help="sensor hub IP")
    ap.add_argument("--db", default="sensor_log.db", help="SQLite file path")
    ap.add_argument("--interval", type=float, default=1.0, help="poll period seconds")
    args = ap.parse_args(argv)

    base_url = "http://" + args.host
    conn = init_db(args.db)
    print("[logger] polling %s every %.1fs -> %s" % (base_url, args.interval, args.db))

    deadline = time.monotonic()
    try:
        while True:
            now = time.monotonic()
            if now < deadline:
                time.sleep(deadline - now)
            deadline = next_deadline(deadline, time.monotonic(), args.interval)
            ts = time.time()
            try:
                data, fft_text, sysinfo = poll_once(base_url)
            except Exception as e:
                print("[warn] %s poll failed: %s"
                      % (time.strftime("%H:%M:%S"), e), file=sys.stderr)
                continue
            insert_reading(conn, build_row(ts, data, fft_text, sysinfo))
            print("[%s] temp=%s humi=%s voc=%s nox=%s"
                  % (time.strftime("%H:%M:%S"), data.get("temp"),
                     data.get("humi"), data.get("voc"), data.get("nox")))
    except KeyboardInterrupt:
        print("\n[logger] stopped")
    finally:
        conn.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
