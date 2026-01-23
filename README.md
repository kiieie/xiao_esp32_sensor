# DATA CENTER ENVIRONMENT MONITORING SENSOR HUB (V7.4)

[한국어 버전 (Korean Version)](./README_KR.md)

A professional-grade monitoring system designed for the rigorous demands of data center and server room environments. Combining the ESP32-S3 with precision environmental sensors, this hub provides high-fidelity data acquisition and real-time visualization.

---

## 🚀 Key Features & Innovations (Version 7.4)

### 1. Ultra-Premium Glassmorphism UI
*   **Elegant Aesthetics**: A high-fidelity dashboard with glass textures, leveraging modern design trends.
*   **Dynamic Backdrop**: An animated mesh gradient provides a living visual status of the environment.
*   **Hero Metrics**: Dedicated visual cards for critical indicators: VOC, NOx, Temperature, Humidity, and Light (Lux).
*   **Fluid Visuals**: Real-time trend charts powered by `Chart.js` for seamless data storytelling.

### 2. Advanced Data Stabilization Engine
*   **FFT DC Offset Removal**: Dynamically calculates and subtracts the DC bias from raw audio signals to ensure a zeroed FFT baseline.
*   **Per-Bin Smoothing (EMA)**: Applies independent Exponential Moving Averages to all frequency bins, suppressing transient flickering and noise.
*   **Noise Floor Cutoff**: A software-defined threshold ensures a perfectly clean, silent state in calm environments.

### 3. Hardware Robustness & Logic
*   **Precision SHT4x Integration**: Automatic I2C multi-address scanning (0x44/0x45) coupled with data-center grade T/H accuracy.
*   **SGP41 Environmental Compensation**: SHT4x data is fed directly into the SGP41 algorithm in real-time for highly accurate gas indexing.
*   **Fault Tolerance**: Built-in protection against sensor disconnection, preventing "Guru Meditation Errors" and automatically switching to fallback data modes.

---

## 🛠 Hardware Configuration

*   **Controller**: Seeed Studio XIAO ESP32-S3 (Dual-Core Performance)
*   **Sensors**:
    *   **SHT4x**: High-precision Temperature (±0.2°C) and Humidity (±1.8% RH).
    *   **SGP41**: Volatile Organic Compounds (VOC) and Nitrogen Oxides (NOx) sensing.
    *   **BH1750**: Digital Light Sensor (1 to 65535 lx range).
    *   **MEMS Microphone**: Focused on real-time noise and frequency analysis (8kHz sampling).
*   **Actuators**: Integrated PWM Servo Motor for external system control.

---

## 💻 Technical Architecture

*   **Dual-Core Task Separation**:
    *   **Core 0 (Communication)**: Manages Wi-Fi, the asynchronous web server, and the JSON API.
    *   **Core 1 (Processing)**: Dedicated to hardware-timer sampling, FFT computation, and sensor polling.
*   **Data Integrity**: Mutex-protected FFT buffers ensure thread safety between ISR sampling and web-requested data serialization.

---

## 🖼 System Previews

### 1. Software: Real-time Dashboard
![Dashboard V7.4 Preview](images/dashboard_v7_4.png)

### 2. Hardware: Design & Schematics
| Asset | Preview |
| :--- | :--- |
| **Circuit Diagram** | ![Circuit Diagram](images/circuit%20diagram.png) |
| **PCB Layout** | ![PCB Layout](images/pcb.png) |
| **3D Rendering** | ![3D PCB Rendering](images/3DPCB.png) |

---

## 📡 API Reference

*   **Telemetry Snapshot**: `GET /data` - Returns all environment metrics in JSON format.
*   **Spectral Analysis**: `GET /fft` - Detailed 32-band frequency domain data.
*   **Actuator Control**: `GET /trigger` - Manual override for the servo motor.

---

## ⚙️ Setup & Configuration

1.  Configure the **ESP32-S3** board in your Arduino IDE environment.
2.  Input your WiFi credentials in the `namespace Config` block within the `.ino` file.
3.  Install dependencies: `Sensirion SHT4x`, `Sensirion SGP41`, `BH1750`, and `arduinoFFT`.
4.  Flash as usual and access the server IP via your browser.
