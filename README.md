# ESP32-S3 Premium Sensor Hub (V5.1)

ESP32-S3와 다양한 환경 센서를 활용한 프리미엄 데이터 수집 및 시각화 시스템입니다.
A premium data acquisition and visualization system using ESP32-S3 and various environmental sensors.

---

## 주요 기능 (Key Features)

### 1. 실시간 데이터 시각화 (Real-time Visualization)
- **Glassmorphism Dashboard**: 현대적인 다크 테마 웹 UI 제공 (Modern dark-themed UI).
- **Split Graphs**: 온도, 습도, 조도, VOC 데이터를 4개의 개별 차트로 분리하여 가독성 극대화 (4 separate charts for Temp, Humi, Lux, and VOC).
- **FFT Spectrum**: 8kHz 샘플링을 통한 실시간 사운드 주파수 분석 (Real-time sound frequency analysis with 8kHz sampling).

### 2. 고성능 아키텍처 (High-Performance Architecture)
- **Dual-Core Optimization**: 네트워킹(Core 0)과 연산/센싱(Core 1)의 역할 분리 (Separation of tasks between networking and computation).
- **Precision Timers**: 하드웨어 타이머를 이용한 정밀 사운드 샘플링 및 센서 데이터 수집 (Precise sampling and sensing using hardware timers).

---

## API 및 데이터 확인 (API & Data Access)

브라우저 외에 `curl`을 통해 JSON 데이터를 직접 가져올 수 있습니다.
You can retrieve JSON data directly via `curl` in addition to the web dashboard.

### JSON 데이터 요청 (Get Snapshot)
```bash
curl -s http://<ESP32_IP>/data | jq .
```
- **Response**: `{"temp": 25.4, "humi": 45.2, "lux": 150.0, "voc": 120, "fft": [...]}`

### 서보 모터 제어 (Actuator Control)
```bash
curl -s http://<ESP32_IP>/trigger
```

---

## 주요 함수 코드 요약 (Core Function Details)

| 함수명 (Function) | 설명 (Korean Description) | Description (English) |
| :--- | :--- | :--- |
| `buildHtmlPage()` | 대시보드 HTML/JS 생성 및 폴링 로직 구현 | Generates UI & implements JSON polling. |
| `processFFTLogic()` | FFT 연산 및 32밴드 데이터 압축 | Handles FFT computation & data optimization. |
| `updateSensorData()` | SHT4x, SGP41, BH1750 데이터 갱신 | Updates all environmental sensor values. |
| `setupWebServer()` | JSON API 및 제어 엔드포인트 설정 | Configures API and control routes. |
| `processingTask()` | Core 1에서 실행되는 핵심 워커 태스크 | Main worker task running on Core 1. |

---

## 하드웨어 구성 (Hardware Configuration)

- **Controller**: Seeed Studio XIAO ESP32-S3
- **Sensors**: 
  - SHT4x (Temp/Humidity)
  - SGP41 (Air Quality/VOC)
  - BH1750 (Light/Lux)
  - Analog Sound Sensor (GPIO 1)
- **Actuator**: Servo Motor (GPIO 2)

---

## 사용자 설정 (Configuration)

`ESP32-sensor.ino` 상단의 `namespace Config` 영역에서 다음 항목을 수정할 수 있습니다:
You can modify these settings in the `namespace Config` section of `ESP32-sensor.ino`:

- `TARGET_SSID / TARGET_PASS`: WiFi 자격 증명 (WiFi Credentials).
- `SAMPLING_FREQ`: 사운드 샘플링 주파수 (Sound sampling frequency).
- `PIN_I2C_SDA / SCL`: I2C 핀 번호 (I2C pin assignments).

---

## 대시보드 미리보기 (Dashboard Preview)

상단 상태 표시줄(Status Bar)에는 각 센서의 **현재값과 단위**가 표시되며, 하단에는 **4개의 독립적인 환경 수치 그래프**가 실시간으로 그려집니다.
The status bar shows **live values with units**, and the bottom section features **4 independent trend charts** updating in real-time.
