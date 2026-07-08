# Sensor Logger — Design

## 목적

ESP32 센서 허브(`xiao_esp32_sensor.ino`)의 HTTP API를 1초 간격으로 폴링해 SQLite DB에 적재하는 Python 스크립트. 향후 데이터 분석/그래프용 로그 축적이 목적.

## 범위

- 대상 엔드포인트: `/data`, `/fft`, `/sys` (전부)
- 저장소: SQLite (`sensor_log.db`, 기본 스크립트와 같은 디렉터리)
- 위치: `tools/logger.py`
- 의존성: Python stdlib만 사용(`urllib.request`, `json`, `sqlite3`, `argparse`, `time`) — 별도 pip 설치 불필요

## 스키마

```sql
CREATE TABLE IF NOT EXISTS readings (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ts REAL NOT NULL,          -- unix epoch (time.time())
    temp REAL, humi REAL, lux REAL,
    voc INTEGER, nox INTEGER,
    sraw_voc INTEGER, sraw_nox INTEGER,
    has_bh INTEGER, has_sgp INTEGER,
    fft_json TEXT,             -- /fft 응답 원문 그대로 (32밴드 JSON 배열)
    wifi_mode INTEGER, ip TEXT, ssid TEXT
);
```

FFT는 32밴드를 그대로 JSON 문자열로 저장(정규화 안 함) — 조회 빈도 낮고 스키마 단순화 우선.

## 동작

- CLI 인자: `--host`(기본 `172.16.100.54`), `--db`(기본 `sensor_log.db`), `--interval`(기본 `1.0`초)
- 매 interval마다 `/data`, `/fft`, `/sys` 순차 GET(각 타임아웃 3초) → 한 행으로 합쳐 INSERT
- 개별 요청 실패 시: 경고 출력하고 해당 사이클 스킵(스크립트 안 죽음) — 네트워크 일시 단절 대비
- `Ctrl+C`로 정상 종료(진행 중 커밋 보장)
- 콘솔에 매 사이클 한 줄 상태 출력(타임스탬프, temp/humi/voc/nox 요약)

## 테스트 방법

보드 실행 중 상태에서 짧게(예: 10~20초) 돌려 `sqlite3 sensor_log.db "select count(*) from readings;"`로 행 쌓이는지, FFT JSON 파싱 되는지 확인.
