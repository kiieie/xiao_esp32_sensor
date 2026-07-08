# 소프트웨어 컨텍스트 (docs/sw.md)

`xiao_esp32_sensor.ino` 상세 아키텍처 문서. `CLAUDE.md`가 요약이고, 이 문서는 앞으로 작업할 때 필요한 세부 컨텍스트(메모리 맵, 태스크 구조, API 스펙, 필터 파라미터, 알려진 결함)를 담는다. **스케치 구조·EEPROM 레이아웃·API가 바뀔 때마다 이 파일도 함께 갱신할 것.**

## 파일 개요

- 파일: `xiao_esp32_sensor.ino`(원래 `ESP32-sensor.ino`였으나 arduino-cli 컴파일 요건상 폴더명과 맞춰 리네임함), 약 880줄, 단일 파일.
- 보드: Seeed XIAO ESP32-S3.
- 파일 헤더 주석은 "V5.0 (2026-01-21)"라 적혀 있지만 README/README_KR은 "V7.4"로 표기 — 헤더 주석이 갱신 안 된 것으로 보임(문서 드리프트, 신뢰하지 말 것). 실제 버전 판단은 git 로그 기준으로.

## 섹션 구조 (파일 내 `[SECTION n]` 주석 기준)

1. **Config** (namespace) — WiFi 자격증명, 핀 할당, FFT/샘플링 상수, EEPROM 크기.
2. **전역 상태** — 서버 객체, 센서 드라이버 객체, `SensorData gData`, volatile 플래그, 타이머 핸들.
3. **하드웨어 타이머 ISR** — `onTimer0`(8kHz 사운드 샘플링), `onTimer1`(100ms 센서 틱).
4. **유틸리티** — EMA 필터(`applyFilter`), I2C 존재 확인, EEPROM 문자열 read/write, SoftAP 헬퍼.
5. **웹 UI** — `buildHtmlPage()`: HTML/CSS/JS 전부 C++ 문자열 결합으로 생성, 별도 프론트엔드 파일 없음.
6. **핵심 로직** — I2C 버스 복구, 센서 초기화, 타이머 셋업, WiFi 셋업, 웹서버 라우트, FFT 처리, 센서 갱신, 개별 센서 진단 함수, I2C 스캐너.
7. **FreeRTOS 워커** — `processingTask` (Core 1 고정).
8. **표준 진입점** — `setup()`, `loop()`.

## 듀얼 코어 / 듀얼 타이머 구조

| 구성요소 | 코어 | 역할 |
|---|---|---|
| `loop()` + `AsyncWebServer` | Core 0 | WiFi 연결 유지, HTTP 라우트 응답, 시리얼 명령 리더 |
| `processingTask` | Core 1 (`xTaskCreatePinnedToCore(..., 1)`) | FFT 연산, 센서 폴링, 서보 트리거, 주기 로그 |
| `timer0` (하드웨어 타이머, 8kHz = 125us 간격) | ISR | `vReal[]`에 `analogRead(PIN_SOUND)` 샘플 채움, `FFT_SAMPLES`(4096개) 차면 `isBufferFull=true` |
| `timer1` (하드웨어 타이머, 100ms 간격) | ISR | `timerSensorFlag=true` 설정 |

두 ISR 모두 `systemReady`가 `setup()` 마지막에 `true`가 되기 전까지 즉시 리턴 — 초기화 도중 인터럽트로 미완성 상태 건드리지 않도록 하는 안전장치.

**동기화**: `fftMutex`(세마포어) 하나만 존재. `processFFTLogic()`(Core 1)이 쓰고 `/fft` 핸들러(Core 0)가 읽는 `fftJsonData` 문자열을 보호. `gData`(온습도/조도/VOC/NOx)는 락 없이 공유됨 — 개별 스칼라 필드 read/write라 실질적 위험은 낮지만, 복합 갱신을 추가할 경우 락 없는 레이스 주의.

## EEPROM 메모리 맵 (`Config::EEPROM_SIZE` = 1024 bytes)

| 주소 | 크기 | 내용 | 비고 |
|---|---|---|---|
| 0 | int(4B) | `servoStart` | `EEPROM.writeInt` |
| 4 | int(4B) | `servoEnd` | `EEPROM.writeInt` |
| 8 | int(4B) | `motionDelay` | **바이트 8~11 사용 — 11번 바이트가 `wifiMode`와 겹침** |
| 11 | 1B | `wifiMode` (0=STA, 1=AP) | ⚠️ `motionDelay`의 최상위 바이트와 물리적으로 같은 주소. `motionDelay`가 16,777,216ms를 넘지 않는 한(현실적으로 항상 그렇다) 상위 바이트는 0이라 실질 충돌은 드묾. 새 정수 필드 추가 시 이 겹침 반드시 고려. |
| 12 | 최대 64B (문자열+널) | `stSSID` | 다음 필드(44)까지 32바이트 여유 → SSID 실사용 가능 길이는 31자 |
| 44 | 최대 64B | `stPW` | 다음 필드(100)까지 56바이트 여유 |
| 100 | 최대 64B | `apSSID` | 다음 필드(164)까지 64바이트 여유 |
| 164 | ~860B | `apPW` | `EEPROM_SIZE`(1024) 끝까지 여유 큼 |

문자열은 `writeEEPROMString`/`readEEPROMString` 헬퍼로 null-terminated 저장, 읽기는 최대 64자 제한. 새 설정 추가 시 반드시 이 표에 주소를 추가하고 겹치지 않는지 확인할 것.

## HTTP API

모든 라우트는 `AsyncWebServer`(포트 80), 응답에 `Cache-Control: no-cache, no-store, must-revalidate` 헤더 포함(HTML/JSON 라우트).

### `GET /`
`buildHtmlPage()`가 생성한 전체 대시보드 HTML 반환.

### `GET /data`
```json
{"temp":26.8,"humi":30.6,"lux":64.2,"voc":64,"nox":1,"sraw_voc":27941,"sraw_nox":12687,"c2":1,"c3":1}
```
`c2`=BH1750 연결 여부, `c3`=SGP41 연결 여부. 락 없이 `gData` 직접 읽음.

### `GET /fft`
```json
[{"freq":0,"mag":253.3}, ... 32개 밴드 ...]
```
`fftMutex`로 보호된 `fftJsonData` 반환(뮤텍스 획득 실패 시 `"[]"` 반환, 타임아웃 50ms).

### `GET /sys`
```json
{"m":0,"ip":"192.168.1.42","ss":"ASUS"}
```
`m`=wifiMode, `ip`/`ss`는 `WiFi.getMode()` 기준 실시간 조회(STA면 `WiFi.localIP()`/`WiFi.SSID()`, 아니면 AP 저장값).

### `GET /set_wifi?m=&s=&p=`
모드/SSID/비밀번호를 해당 모드 필드에 저장(EEPROM commit) 후 **즉시 1초 뒤 `ESP.restart()`**. 부작용 있는 엔드포인트 — 자동화 스크립트에서 호출 시 주의.

### `GET /set_servo?s=&e=&d=`
서보 시작/끝 펄스폭(us)과 동작 지연(ms)을 EEPROM에 저장. 즉시 반영 안 되고 다음 `/trigger` 때 사용.

### `GET /trigger`
`servoTriggered=true` 설정 → `processingTask`가 `servoEnd`로 이동, `motionDelay`ms 대기, `servoStart`로 복귀.

## 센서 구성

| 센서 | I2C 주소 | 라이브러리 | 비고 |
|---|---|---|---|
| BH1750 (조도) | 0x23 | `BH1750` | |
| SHT4x (온습도) | 0x44 또는 0x45 | `SensirionI2cSht4x` | 부팅 시 두 주소 다 스캔, 앞서 잡히는 쪽 사용 |
| SGP41 (VOC/NOx) | 0x59 | `SensirionI2CSgp41` + `VOCGasIndexAlgorithm`/`NOxGasIndexAlgorithm` | **부팅 시 `initSensors()`가 이 센서를 전혀 프로브하지 않음** (아래 결함 참고) |

센서 재연결: `updateSensorData()` 안에서 10초마다 미연결 센서(`!hasSGP || !hasSHT`) 재스캔·재초기화 시도.

I2C 버스 잠김 감지: SHT4x 측정 연속 실패(`consecutiveErrors > 20`) 시 `recoverI2CBus()` 호출(SCL 핀을 10회 토글해 강제 언락).

## FFT / 사운드 처리 파이프라인 (`processFFTLogic`)

1. **샘플링**: `timer0` ISR, 8kHz, `Config::FFT_SAMPLES`=4096개 샘플로 `vReal[]` 채움(analogRead raw 0~4095).
2. **DC 제거 + 저주파 컷**: 1차 IIR 하이패스 필터, `hpfAlpha=0.99687`(8kHz 샘플링 기준 약 4Hz 컷오프), 이후 게인 0.5 적용.
3. **윈도잉 + FFT**: `FFT_WIN_TYP_HAMMING` 윈도우 → `FFT.compute(FFT_FORWARD)` → `complexToMagnitude()`.
4. **32밴드 압축**: `samplesPerBin = FFT_SAMPLES/2/32 = 64`. 밴드당 64개 샘플 평균(단, 각 밴드 첫 2개 인덱스는 DC 성분 제외를 위해 오프셋 +2 적용).
5. **밴드별 EMA 평활화**: `smoothAlpha=0.2` (낮을수록 부드러움), 밴드별 상태(`smoothedBins[32]`) 유지.
6. **노이즈 플로어 컷오프**: 평활화된 magnitude가 5.0 미만이면 0으로 클램프.
7. **직렬화**: `{"freq":<Hz>, "mag":<1자리 소수>}` 배열 JSON 문자열로 만들어 뮤텍스 하에 `fftJsonData`에 저장. 밴드 주파수 = `i * (SAMPLING_FREQ/2/32)` = `i * 125Hz`, 즉 0~3875Hz 범위 커버(Nyquist 4000Hz 근접).

## 알려진 결함 / 주의사항 (수정 전 반드시 확인)

1. **SGP41이 부팅 시 검사 안 됨** — `initSensors()`는 BH1750(0x23), SHT4x(0x44/0x45)만 스캔하고 `gData.hasSGP`를 boot 시점에 설정하는 코드가 없음(구조체 기본값 `false`). 실제로는 `updateSensorData()`의 10초 재연결 루프가 최초 감지를 담당 → 배선 정상이어도 부팅 후 최대 10초간 `/data`의 `c3`는 0으로 보임.
2. **EEPROM 주소 8과 11 물리적 겹침** — `motionDelay`(int, 4바이트, 주소 8~11)와 `wifiMode`(1바이트, 주소 11)가 같은 바이트 공유. 실용상 `motionDelay` 값이 16.7M을 넘지 않아 문제 안 되지만, 새 설정 추가 시 이 패턴 반복하지 말 것 — 대신 표를 갱신하고 여유 공간에 배치.
3. **버전 문서 드리프트** — `.ino` 헤더 주석("V5.0")과 README("V7.4")가 불일치. 버전 확인은 git 로그/커밋 메시지 기준으로.
4. **`c2`/`c3`(`/data` JSON) 필드명이 코드 가독성과 불일치** — `hasBH`→`c2`, `hasSGP`→`c3`로 매핑, 프론트엔드 JS(`buildHtmlPage()` 내 인라인 스크립트)에서만 이 규약을 알 수 있음. 새 센서 추가 시 이 네이밍 패턴(`c4`, `c5`...) 따라갈지 재검토 여지 있음.
5. **WiFi 자격증명은 EEPROM이 `Config` 기본값보다 항상 우선** — `setupNetwork()`는 `stSSID.length()>0`이면 무조건 EEPROM 값 사용, 소스의 `Config::TARGET_SSID`/`TARGET_PASS`는 EEPROM이 비어있을 때만 적용되는 최초 1회용 기본값임. 과거에 `/set_wifi` 한 번이라도 호출됐으면 이후 소스 수정 + 재플래시해도 무시됨. 네트워크 변경 요청 받으면 소스 수정만으로 안 끝난다고 가정하고 EEPROM도 함께 갱신해야 함(비밀번호 특수문자는 URL 인코딩 필수). 현재 EEPROM은 비워둔 상태(`Config` 기본값 `robot-1`/무비밀번호가 그대로 적용됨, 아래 히스토리 참고).

## 현재 배포 상태 (2026-07-08 기준)

- **WiFi**: `Config::TARGET_SSID="robot-1"`, `TARGET_PASS=""`(오픈 네트워크). EEPROM WiFi 필드는 전부 초기화(`0xFF`)해서 소스 기본값과 EEPROM이 일치 — 재부팅/재플래시 관계없이 항상 `robot-1`로 붙음.
- 실측 IP: `172.16.100.54` (STA 연결 성공, `WiFi.localIP()`로 확인).
- 트러블슈팅 중 겪은 일: 처음 `kepco`/`Admin123!@#`로 오인해 연결 시도 → EEPROM에 남은 예전 SSID(`DIRECT-ZvC145x Series le`) 때문에 소스 수정이 무시되는 문제까지 겹쳐 원인 파악이 오래 걸림. `curl /set_wifi`로 EEPROM 덮어써서 `kepco`로 전환 시도했으나 실제 사용자 의도는 오픈 네트워크 `robot-1`이었음 — 최종적으로 EEPROM 완전 초기화 + 소스 기본값을 `robot-1`로 맞추는 방식으로 정리.

## EEPROM 초기화 절차 (개발 PC와 보드가 다른 서브넷이라 `/set_wifi` HTTP 호출이 불가능할 때)

시리얼 명령엔 EEPROM WiFi 필드를 지우는 기능이 없음(`s`/`S`는 `wifiMode`만 0으로 바꿈, SSID/PW는 그대로). USB로 물리 연결된 상태에서만 가능한 절차:

1. 임시 헬퍼 스케치 작성 — `EEPROM.begin(1024)` 후 바이트 0~227을 `0xFF`로 채우고 `commit()`.
2. `arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3 --upload -p <PORT> <helper-sketch-dir>` 로 업로드, 시리얼로 "cleared" 로그 확인.
3. 본 펌웨어(`xiao_esp32_sensor.ino`) 재업로드.
4. `readEEPROMString`이 첫 바이트 `0xFF`(=255)를 감지해 빈 문자열 반환 → `setupNetwork()`가 `Config::TARGET_SSID`/`TARGET_PASS` 기본값 사용.

이 방식이 `esptool erase_flash` 전체 삭제보다 안전(다른 파티션 안 건드림).

### 이미 고친 결함 (히스토리 — 재발 방지용 기록)

- ~~29번째 줄 근처 깨진 include(`arduinoFFT넵.h`) + `#include <ESP32Servo.h>` 중복~~ → 제거 완료. 최초엔 "Arduino include 해석에 해 없는 잔재"로 추정했으나, 실제 `arduino-cli compile` 돌려보니 `fatal error: arduinoFFT넵.h: No such file or directory`로 컴파일 자체가 실패하는 진짜 버그였음. **추측으로 결함 등급 매기지 말고 항상 실제 컴파일로 검증할 것.**
- ~~`ESPAsyncWebServer`(lacamera 포크, registry 기본 검색 결과) 사용 시 esp32 core 3.3.10과 컴파일 안 됨~~ → `mbedtls_md5_starts_ret` 등 deprecated mbedtls API가 최신 IDF에서 제거되어 발생. `ESP Async WebServer`(ESP32Async 포크) + `Async TCP`(ESP32Async 포크) 조합으로 교체해 해결. CLAUDE.md 빌드 섹션에 정확한 라이브러리명 기록해둠 — 레지스트리 검색 시 이름이 비슷한 여러 포크가 나오니 헷갈리지 말 것.
- ~~폴더명(`xiao_esp32_sensor`)과 메인 `.ino` 파일명(`ESP32-sensor.ino`) 불일치로 `arduino-cli compile .` 실패~~ → 파일을 `xiao_esp32_sensor.ino`로 리네임해 해결(폴더명이 리포 이름과 결부돼 있어 폴더 쪽을 바꾸는 대신 파일명을 맞춤).

## WiFi 모드 전환 흐름

- `wifiMode` EEPROM 11번 바이트에 저장, 0=STA, 1=AP.
- STA 모드: `stSSID`/`stPW` 있으면 사용, 없으면 `Config::TARGET_SSID`/`TARGET_PASS` 기본값. `WIFI_CONNECT_TIMEOUT_MS`(10초) 내 연결 실패 시 `Config::HUB_AP_SSID`/`HUB_AP_PASS`로 복구용 SoftAP 자동 전환(고정 IP `192.168.1.1`).
- AP 모드: `apSSID`/`apPW` 있으면 사용, 없으면 `Config::HUB_AP_SSID`/`HUB_AP_PASS`. 고정 IP `192.168.1.1` 강제(`WiFi.softAPConfig`).
- `/set_wifi` 호출 시 즉시 EEPROM 기록 + `ESP.restart()`. 시리얼에서 `s`/`S` 입력 시에도 강제로 STA 모드 저장 후 재시작(빠른 복구용 백도어).

## 향후 작업 시 참고

- 새 센서/엔드포인트 추가 시: EEPROM 표(위) 갱신, `docs/sw.md` 최신화, `/data` JSON 필드 네이밍 규약(`c2`,`c3`...) 검토.
- 프론트엔드(대시보드) 변경 시: `buildHtmlPage()` 내 문자열이라 별도 빌드/린트 없음 — 브라우저에서 직접 확인 필요(하드웨어 없으면 `curl`로 HTML만 받아 로컬 파일로 저장 후 브라우저 오픈하는 방식으로 우회 가능).
- FFT 파라미터(샘플 수, 컷오프, 평활 계수) 변경 시 `Config` 네임스페이스와 `processFFTLogic()` 상수 둘 다 확인 — 일부 상수는 `Config`에 없고 함수 내부에 하드코딩됨(`hpfAlpha`, `gain`, `smoothAlpha`, 노이즈 플로어 `5.0f`).
