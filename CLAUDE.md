# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 프로젝트 개요

Seeed XIAO ESP32-S3 기반 단일 스케치(`xiao_esp32_sensor.ino`, 약 880줄) 프로젝트. 데이터센터/서버실 환경(온습도, VOC/NOx, 조도, 소음)을 모니터링하고 WiFi로 실시간 대시보드를 서빙한다. 빌드 시스템, package.json, 테스트 없음 — 모든 로직이 하나의 `.ino` 파일에 존재.

깊은 아키텍처 컨텍스트(EEPROM 메모리 맵, 태스크 구조, API 형식, 알려진 결함)는 `docs/sw.md` 참조. 스케치 구조, EEPROM 레이아웃, API 표면이 바뀔 때마다 그 파일을 최신 상태로 유지할 것.

## 빌드 / 플래시 / 모니터링

`arduino-cli` 사용 (`/opt/homebrew/bin/arduino-cli`에 설치됨). 보드 FQBN: `esp32:esp32:XIAO_ESP32S3`. 메인 스케치 파일명은 `xiao_esp32_sensor.ino`(폴더명과 일치 — arduino-cli 필수 조건, 아래 참고).

```bash
# 컴파일만
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3 .

# 포트 확인
arduino-cli board list

# 컴파일 + 업로드
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3 --upload -p <PORT> .

# 시리얼 모니터 (115200 baud)
arduino-cli monitor -p <PORT> -c baudrate=115200
```

**필수 조건**: arduino-cli는 스케치 폴더명과 메인 `.ino` 파일명이 반드시 일치해야 컴파일 가능(Arduino IDE 규칙과 동일). 리포 폴더명이 `xiao_esp32_sensor`라서 파일도 `xiao_esp32_sensor.ino`로 맞춰둠 — 파일명 다시 바꾸면 컴파일 깨짐.

필요 라이브러리 (없으면 `arduino-cli lib install "<name>"`):

| 라이브러리명 | 비고 |
|---|---|
| `ESP Async WebServer` (ESP32Async 포크, 3.11.2+) | ⚠️ 레지스트리 기본 검색 결과인 `ESPAsyncWebServer`(lacamera 포크, 3.1.0)는 esp32 core 3.3.10의 mbedtls API(`mbedtls_md5_*_ret` 제거됨)와 호환 안 돼 컴파일 실패함. 반드시 **`ESP Async WebServer`**(공백 포함, ESP32Async 조직)로 설치할 것. |
| `Async TCP` (ESP32Async 포크, 3.4.10+) | 위와 짝 맞춰야 함. 레지스트리 기본 `AsyncTCP`(dvarrel 포크, 1.1.x)는 `ESP Async WebServer` 3.x와 API 불일치로 컴파일 실패. |
| `Sensirion I2C SHT4x` | |
| `Sensirion I2C SGP41` | |
| `BH1750` | |
| `arduinoFFT` | |
| `ESP32Servo` | |
| `Sensirion Gas Index Algorithm` | `VOCGasIndexAlgorithm`/`NOxGasIndexAlgorithm` 제공 |

```bash
arduino-cli lib install "ESP Async WebServer" "Async TCP" "Sensirion I2C SHT4x" "Sensirion I2C SGP41" "BH1750" "arduinoFFT" "ESP32Servo" "Sensirion Gas Index Algorithm"
```

자동화 테스트 없음. 검증은 수동: 플래시 후 시리얼 모니터 확인 및/또는 HTTP 엔드포인트 호출(`curl http://<ip>/data`, `/fft`, `/sys`). 시리얼 콘솔은 런타임에 단일 문자 명령 지원: `d`/`t`는 전체 센서 진단 실행, `s`는 STA 모드 강제 전환 후 재시작. 데이터 적재는 `python3 tools/logger.py --host <보드IP>` (1초 간격 SQLite 로깅, 상세는 `docs/superpowers/specs/2026-07-08-sensor-logger-design.md`). 로거 단위 테스트: `python3 -m unittest discover -s tools -v`.

## 아키텍처

**단일 파일, 듀얼 코어, 듀얼 타이머 FreeRTOS 구조.** 모든 코드가 `xiao_esp32_sensor.ino`에 있으며 번호 매겨진 `[SECTION n]` 주석으로 위→아래 구성됨 — 코드 탐색 시 여기서 시작.

- **Core 0 (Arduino `loop()`)**: WiFi + `AsyncWebServer`(포트 80). 모든 HTTP 라우트와 시리얼 명령 리더 처리. FFT 버퍼에 직접 접근 안 함.
- **Core 1 (`processingTask`, `xTaskCreatePinnedToCore`로 고정)**: 두 volatile 플래그 `isBufferFull`(FFT 준비 완료), `timerSensorFlag`(100ms 센서 틱)를 폴링하며 모든 센서/FFT 연산과 서보 트리거 처리 수행.
- **하드웨어 타이머 2개**가 ISR 컨텍스트에서 volatile 플래그 구동: `timer0`은 8kHz로 `vReal[]` 채움(`analogRead`, 사운드 샘플링), `timer1`은 100ms마다 `timerSensorFlag` 설정. 두 ISR 모두 `setup()` 끝에서 `systemReady`가 설정되기 전까지 조기 반환 — 초기화 중 샘플링 안 함.
- **`fftMutex`** (`SemaphoreHandle_t`)가 유일한 코어 간 락 — `processFFTLogic()`이 쓰고 `/fft` 핸들러가 읽는, 미리 직렬화된 FFT JSON 문자열 `fftJsonData` 보호.
- **`gData` (`SensorData` 구조체)**는 단일 전역 센서 상태. Core 1의 `updateSensorData()`가 쓰고, `/data`·`/sys` HTTP 핸들러가 락 없이 직접 읽음 — 단일 word/float 읽기라 복합 불변식이 아니므로 허용 가능한 설계.
- **설정은 파일 상단 `namespace Config`**에 위치 (WiFi 인증정보, 핀, FFT/샘플링 상수, EEPROM 크기). 하드웨어 핀이나 샘플링 속도 변경 시 여기 수정.
- **설정 영속화는 구조체가 아니라 EEPROM 원시 바이트 오프셋** 방식 — 새 영속 필드 추가 전 `docs/sw.md`의 주소 맵 확인 필수 (충돌 시 다른 설정값이 조용히 깨짐).
- **대시보드 HTML/CSS/JS는 하나의 큰 C++ 문자열**(`buildHtmlPage()`, `p += "..."` 약 130줄)로 만들어져 인라인으로 서빙 — 별도 프론트엔드 파일이나 빌드 단계 없음. 클라이언트는 `/data`, `/fft`, `/sys`를 1초마다 `fetch`로 폴링.
- **WiFi는 두 모드** 지원, `wifiMode`(EEPROM 11번 바이트에 영속)로 제어: STA(클라이언트, 설정된/타겟 SSID 접속 시도, 타임아웃 시 복구용 SoftAP로 폴백)와 AP(고정 IP `192.168.1.1` 강제). 모드/인증정보 변경은 `/set_wifi`가 처리하며 EEPROM 기록 후 즉시 `ESP.restart()`.

### 수정 전 알아둘 결함

- `gData.hasSGP`는 `initSensors()`(부팅 시)에서 전혀 검사 안 됨(BH1750, SHT4x만 검사) — SGP41은 `updateSensorData()` 내 10초 재연결 재시도 루프에서만 감지됨. 즉 배선이 정상이어도 부팅 후 최초 10초간 SGP41은 항상 오프라인으로 표시됨.
- **WiFi 접속 안 될 때 진짜 원인 주의**: `Config::TARGET_SSID`/`TARGET_PASS`는 EEPROM에 저장된 `stSSID`/`stPW`가 비어있을 때만 쓰임(`setupNetwork()` 로직). 과거에 `/set_wifi`로 다른 SSID 저장한 적 있으면 소스에서 `Config` 기본값 바꿔 재플래시해도 무시되고 EEPROM 값 그대로 접속 시도함 — 실제로 겪은 문제. 현재 `Config::TARGET_SSID`는 `"robot-1"`(오픈 네트워크, 비밀번호 없음)으로 맞춰져 있고 EEPROM WiFi 필드는 비워둔 상태(둘이 일치해야 재플래시 없이도 항상 이 네트워크로 붙음).
  - **네트워크에 붙어 있고 IP를 아는 경우**: `curl "http://<현재IP>/set_wifi?m=0&s=<SSID>&p=<PW>"` 호출해 EEPROM 직접 덮어쓰면 됨(비밀번호 특수문자는 URL 인코딩 필수, 예: `!`→`%21`, `@`→`%40`, `#`→`%23`). 호출 즉시 재시작됨.
  - **다른 서브넷이라 HTTP로 못 붙는 경우**(자주 발생 — 개발 PC와 보드가 다른 네트워크): 시리얼 명령엔 EEPROM WiFi 필드를 지우는 기능이 없음. USB로 물리 연결된 상태에서 EEPROM 바이트 0~227을 `0xFF`로 채우는 임시 헬퍼 스케치(`EEPROM.begin(1024)` → write loop → `commit()`)를 먼저 업로드해 초기화한 다음, 본 펌웨어를 재업로드하는 방식으로 해결. EEPROM이 비면 `readEEPROMString`이 첫 바이트 `0xFF`를 감지해 빈 문자열 반환 → 소스의 `Config` 기본값이 다시 적용됨.

### 이미 고친 결함 (히스토리)

- ~~29번째 줄 근처 깨진 include(`arduinoFFT넵.h`)와 `#include <ESP32Servo.h>` 중복~~ → 제거 완료. 처음엔 "무해한 잔재"로 추정했으나 실제로는 컴파일 실패 원인이었음(`No such file or directory`) — 확인 없이 판단하지 말 것.
