# Sensor Logger 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** ESP32 센서 허브의 `/data`, `/fft`, `/sys`를 1초 간격으로 폴링해 SQLite에 적재하는 `tools/logger.py` 작성.

**Architecture:** 단일 스크립트, 순수 함수 분리(DB 계층 / deadline 스케줄링 / HTTP 폴링 / CLI 루프)로 각각 unittest 가능하게 구성. HTTP는 `poll_once()` 한 함수로 묶고 테스트에서 `fetch_text`를 mock.

**Tech Stack:** Python 3 stdlib만 사용 — `urllib.request`, `sqlite3`, `json`, `argparse`, `time`, `unittest`. pip 설치 불필요.

## Global Constraints

- 런타임/테스트 의존성 모두 stdlib만 (스펙 명시)
- DB 기본 경로 `sensor_log.db`, 호스트 기본 `172.16.100.54`, 간격 기본 `1.0`초 (스펙 명시)
- WAL 모드 + busy_timeout 필수 (동시 조회 시나리오가 스펙 테스트 방법에 포함됨)
- 스케줄링은 `time.monotonic()` deadline 기반, 밀린 tick은 스킵 (스펙 명시)
- 요청 실패 시 경고 출력 후 사이클 스킵, 스크립트는 계속 실행
- DB 파일은 커밋 금지 — `.gitignore`에 `*.db` 추가
- 테스트 실행 명령: `python3 -m unittest discover -s tools -v`

## 파일 구조

- `tools/logger.py` — 전체 구현 (Task 1~4에 걸쳐 누적 작성)
- `tools/test_logger.py` — unittest (Task별 테스트 누적 추가)
- `.gitignore` — `*.db` 라인 추가 (Task 4)
- `CLAUDE.md` — 로거 사용법 한 줄 추가 (Task 4)

---

### Task 1: DB 계층 (스키마, init_db, build_row, insert_reading)

**Files:**
- Create: `tools/logger.py`
- Test: `tools/test_logger.py`

**Interfaces:**
- Produces:
  - `init_db(path: str) -> sqlite3.Connection` — WAL 설정, 스키마+인덱스 생성 후 연결 반환
  - `build_row(ts: float, data: dict, fft_text: str, sysinfo: dict) -> tuple` — INSERT 파라미터 14개 튜플
  - `insert_reading(conn, row: tuple) -> None` — INSERT 후 즉시 commit
  - 상수 `SCHEMA`, `INSERT_SQL`

- [ ] **Step 1: 실패하는 테스트 작성**

`tools/test_logger.py` 생성:

```python
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
```

- [ ] **Step 2: 실패 확인**

Run: `python3 -m unittest discover -s tools -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'logger'`

- [ ] **Step 3: 최소 구현**

`tools/logger.py` 생성:

```python
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
```

- [ ] **Step 4: 통과 확인**

Run: `python3 -m unittest discover -s tools -v`
Expected: PASS (3 tests)

- [ ] **Step 5: 커밋**

```bash
git add tools/logger.py tools/test_logger.py
git commit -m "feat: add sensor logger DB layer (schema, WAL, insert)"
```

---

### Task 2: Deadline 기반 스케줄링

**Files:**
- Modify: `tools/logger.py` (함수 추가)
- Test: `tools/test_logger.py` (클래스 추가)

**Interfaces:**
- Produces: `next_deadline(deadline: float, now: float, interval: float) -> float` — 다음 tick 시각(monotonic 기준). 항상 `now`보다 미래(또는 `now == deadline + interval`인 경계에선 그 다음 tick). 밀린 tick은 건너뜀.

- [ ] **Step 1: 실패하는 테스트 작성**

`tools/test_logger.py`에 클래스 추가:

```python
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
```

- [ ] **Step 2: 실패 확인**

Run: `python3 -m unittest discover -s tools -v`
Expected: FAIL — `AttributeError: module 'logger' has no attribute 'next_deadline'`

- [ ] **Step 3: 최소 구현**

`tools/logger.py`에 추가 (`insert_reading` 아래):

```python
def next_deadline(deadline, now, interval):
    """Return the next future tick, skipping any ticks missed while busy."""
    deadline += interval
    if deadline <= now:
        missed = int((now - deadline) // interval) + 1
        deadline += missed * interval
    return deadline
```

- [ ] **Step 4: 통과 확인**

Run: `python3 -m unittest discover -s tools -v`
Expected: PASS (7 tests)

- [ ] **Step 5: 커밋**

```bash
git add tools/logger.py tools/test_logger.py
git commit -m "feat: add drift-free deadline scheduling to logger"
```

---

### Task 3: HTTP 폴링 (fetch_text, poll_once)

**Files:**
- Modify: `tools/logger.py` (함수 추가, `import json`/`import urllib.request` 추가)
- Test: `tools/test_logger.py` (클래스 추가, `from unittest import mock` 추가)

**Interfaces:**
- Consumes: 없음 (독립)
- Produces:
  - `fetch_text(url: str, timeout: float = 3.0) -> str` — GET 후 UTF-8 본문 반환
  - `poll_once(base_url: str, timeout: float = 3.0) -> tuple[dict, str, dict]` — `(data, fft_text, sysinfo)`. `/data`·`/sys`는 파싱된 dict, `/fft`는 원문 문자열(단, JSON 유효성은 검증). 실패 시 예외 전파(호출자가 처리).

- [ ] **Step 1: 실패하는 테스트 작성**

`tools/test_logger.py` 상단 import에 `from unittest import mock` 추가 후 클래스 추가:

```python
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
```

- [ ] **Step 2: 실패 확인**

Run: `python3 -m unittest discover -s tools -v`
Expected: FAIL — `AttributeError: module 'logger' has no attribute 'poll_once'` (3건)

- [ ] **Step 3: 최소 구현**

`tools/logger.py` import 블록을 다음으로 교체:

```python
import json
import sqlite3
import urllib.request
```

`next_deadline` 아래 추가:

```python
def fetch_text(url, timeout=3.0):
    with urllib.request.urlopen(url, timeout=timeout) as resp:
        return resp.read().decode("utf-8")


def poll_once(base_url, timeout=3.0):
    data = json.loads(fetch_text(base_url + "/data", timeout))
    fft_text = fetch_text(base_url + "/fft", timeout)
    json.loads(fft_text)  # reject non-JSON payloads early
    sysinfo = json.loads(fetch_text(base_url + "/sys", timeout))
    return data, fft_text, sysinfo
```

- [ ] **Step 4: 통과 확인**

Run: `python3 -m unittest discover -s tools -v`
Expected: PASS (10 tests)

- [ ] **Step 5: 커밋**

```bash
git add tools/logger.py tools/test_logger.py
git commit -m "feat: add HTTP polling with FFT payload validation"
```

---

### Task 4: CLI 메인 루프 + .gitignore + 문서 + 실기기 검증

**Files:**
- Modify: `tools/logger.py` (main 추가, `import argparse`/`import sys`/`import time` 추가)
- Modify: `.gitignore` (`*.db` 라인)
- Modify: `CLAUDE.md` (사용법 한 줄)

**Interfaces:**
- Consumes: `init_db`, `build_row`, `insert_reading`, `next_deadline`, `poll_once` (Task 1~3 시그니처 그대로)
- Produces: `main(argv: list[str] | None = None) -> int` — CLI 진입점

- [ ] **Step 1: main 구현**

`tools/logger.py` import 블록을 다음으로 교체:

```python
import argparse
import json
import sqlite3
import sys
import time
import urllib.request
```

파일 끝에 추가:

```python
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
```

- [ ] **Step 2: 전체 테스트 여전히 통과 확인**

Run: `python3 -m unittest discover -s tools -v`
Expected: PASS (10 tests)

- [ ] **Step 3: .gitignore에 *.db 추가**

`.gitignore`의 `# macOS` 블록 위에 추가:

```
# Sensor logger output
*.db
*.db-wal
*.db-shm
```

- [ ] **Step 4: 실기기 통합 검증 (보드 172.16.100.54 살아있는 상태에서)**

```bash
cd /Users/kiie/project/xiao_esp32_sensor
python3 tools/logger.py --db /tmp/itest.db & LOGPID=$!
sleep 12
# 로거 실행 중 동시 조회 (WAL 검증)
sqlite3 /tmp/itest.db "SELECT count(*) FROM readings;"
sqlite3 /tmp/itest.db "SELECT json_array_length(fft_json) FROM readings LIMIT 1;"
kill -INT $LOGPID
```

Expected:
- `count(*)`가 10~12 (약 12초 경과, 드리프트 없음)
- `json_array_length` = 32
- 동시 조회 시 lock 에러 없음
- Ctrl+C(SIGINT) 후 "[logger] stopped" 출력, 정상 종료

보드 단절 복구(스펙 검증 항목 4)는 에러 전파·스킵 로직이 단위 테스트(`test_poll_once_propagates_error` + main의 `continue`)로 커버됨. 실기기로도 확인하려면 로거 실행 중 보드 USB 전원을 뽑았다 다시 꽂아 `[warn] ... poll failed` 경고만 찍히다가 재접속 후 자동 재개되는지 보면 됨 — 물리 개입 필요하므로 수동 선택 사항.

- [ ] **Step 5: CLAUDE.md 검증 문단에 사용법 추가**

CLAUDE.md의 "자동화 테스트 없음..." 문단 끝에 추가:

```
데이터 적재는 `python3 tools/logger.py --host <보드IP>` (1초 간격 SQLite 로깅, 상세는 `docs/superpowers/specs/2026-07-08-sensor-logger-design.md`). 로거 단위 테스트: `python3 -m unittest discover -s tools -v`.
```

- [ ] **Step 6: 커밋**

```bash
git add tools/logger.py .gitignore CLAUDE.md
git commit -m "feat: add logger CLI loop, ignore db files, document usage"
```
