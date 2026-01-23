# DATA CENTER ENVIRONMENT MONITORING SENSOR HUB (V7.4) - 한국어

[English Version](./README.md)

ESP32-S3와 고성능 환경 센서를 결합하여 데이터 센터 및 서버실의 환경을 정밀하게 모니터링하는 전문 등급 시스템입니다. 하드웨어 설계부터 소프트웨어 시각화까지 모든 과정이 데이터 센터의 안정적인 운영을 돕기 위해 최적화되었습니다.

---

## 🚀 주요 기능 및 업데이트 (Version 7.4)

### 1. Ultra-Premium Glassmorphism UI
*   **현대적인 심미성**: 최신 디자인 트렌드를 반영한 고품격 유리 질감 대시보드.
*   **동적 배경**: 실시간으로 움직이는 메쉬 그래디언트 배경을 통해 시각적으로 생동감 넘치는 시스템 상태 제공.
*   **히어로 메트릭 (Hero Metrics)**: VOC, NOx, 온도, 습도, 조도 등 핵심 지표를 한눈에 파악할 수 있는 전용 카드 배치.
*   **반응형 차트**: `Chart.js`를 활용하여 실시간 데이터 추이를 매끄럽게 시각화.

### 2. 정밀 데이터 안정화 시스템
*   **FFT DC Offset 제거**: 사운드 신호의 직류 편향을 계산하여 제거함으로써 FFT 베이스라인의 불필요한 흔들림 최소화.
*   **주파수 대역별 지수 이동 평균 (EMA)**: 각 FFT 빈(Bin)에 smoothing 필터를 적용하여 급격한 노우즈 변동 억제.
*   **노이즈 플로어 컷오프 (Noise Floor Cutoff)**: 일정 임계값 이하의 미세 소음을 차단하여 완벽하게 정적인 상태 유지.

### 3. 하드웨어 안정성 및 복구 로직
*   **SHT4x 고정밀 센서 연동**: 자동 I2C 주소 검색(0x44/0x45) 및 데이터 센터급 정밀 온도/습도 측정.
*   **SGP41 가스 보정**: SHT4x에서 측정된 실시간 온도/습도 데이터를 SGP41 엔진에 주입하여 정확한 VOC/NOx 지수 산출.
*   **내결합성 (Fault Tolerance)**: 센서가 없거나 통신 오류 발생 시 시스템 다운을 방지하고 기본값(Fallback)으로 자동 전환.

---

## 🛠 하드웨어 구성 (Hardware Configuration)

*   **MCU**: Seeed Studio XIAO ESP32-S3 (Dual-Core)
*   **Sensors**:
    *   **SHT4x**: 초정밀 온도 (±0.2°C) 및 습도 (±1.8% RH) 측정.
    *   **SGP41**: 휘발성 유기 화합물(VOC) 및 질소산화물(NOx) 감지.
    *   **BH1750**: 1~65535 lx 범위의 조도 측정.
    *   **MEMS Microphone**: 8kHz 샘플링을 통한 실시간 주파수(FFT) 분석.
*   **Actuators**: 시스템 제어를 위한 PWM 서보 모터.

---

## 💻 시스템 구조 (Technical Architecture)

*   **Dual-Core 태스크 분리**:
    *   **Core 0 (Networking)**: 웹 서버, JSON API 응답, WiFi 연결 유지.
    *   **Core 1 (Sensing/Computation)**: 하드웨어 타이머 기반 사운드 샘플링, FFT 연산, 센서 데이터 갱신.
*   **안정적인 데이터 흐름**: 뮤텍스(Mutex)를 사용한 FFT 데이터 보호 및 데이터 오염 방지.

---

## 🖼 시스템 프리뷰 (Visual Previews)

### 1. 소프트웨어: 실시간 대시보드
![Dashboard V7.4 Preview](images/dashboard_v7_4.png)

### 2. 하드웨어 설계
| 항목 | 이미지 |
| :--- | :--- |
| **회로도 (Circuit)** | ![Circuit Diagram](images/circuit%20diagram.png) |
| **PCB 레이아웃** | ![PCB Layout](images/pcb.png) |
| **3D 렌더링** | ![3D PCB Rendering](images/3DPCB.png) |

---

## 📡 API 레퍼런스

*   **JSON 데이터 리셋**: `GET /data` - 현재 모든 센서값 스냅샷 제공.
*   **FFT 데이터 확인**: `GET /fft` - 32개 밴드의 주파수 도메인 상세 데이터 제공.
*   **액추에이터 트리거**: `GET /trigger` - 서보 모터 동작 제어.

---

## ⚙️ 설치 및 설정

1.  **Arduino IDE**에서 `ESP32-S3` 보드 라이브러리 설치.
2.  `namespace Config` 섹션에서 WiFi SSID와 Password 입력.
3.  필요한 센서 라이브러리(`Sensirion`, `BH1750`, `arduinoFFT`) 설치 후 업로드.
