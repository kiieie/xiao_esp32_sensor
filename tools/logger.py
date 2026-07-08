#!/usr/bin/env python3
"""Poll the ESP32 sensor hub HTTP API and log readings to SQLite.

Endpoints polled each cycle: /data (sensor snapshot), /fft (32-band
spectrum, stored as raw JSON text), /sys (wifi mode / ip / ssid).
"""
import sqlite3

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
