/**
 * @file ESP32-sensor.ino
 * @author Antigravity (Advanced Agentic AI)
 * @version V5.0 (Dual Timer & Core Optimization / Premium Glassmorphism UI)
 * @date 2026-01-21
 * 
 * [프로그램 혁신 사항]
 * 1. 하드웨어 타이머 최적화: 
 *    - Timer 0 (8kHz): 정밀 사운드 샘플링 (Jitter 최소화)
 *    - Timer 1 (100ms): 정기적인 센서 수집 주기 보장
 * 2. 듀얼 코어 역할 분리:
 *    - Core 0: 네트워킹 및 웹 서버 (Wi-Fi, AsyncWebServer, SSE)
 *    - Core 1: 데이터 연산 및 핵심 로직 (FFT 변환, 센서 수합, 제어)
 * 3. 프리미엄 웹 인터페이스:
 *    - 글래스모피즘(Glassmorphism) 다크 테마 적용
 *    - 고해상도 고정 폭 FFT 스펙트럼 및 부드러운 센서 트렌드 라인 차트
 * 4. 모듈화 및 클린 코드:
 *    - 모든 기능을 의미 있는 함수 단위로 구조화 (한글 상세 주석 포함)
 *    - 최상단 사용자 설정 영역(Config) 분리로 편의성 극대화
 */

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <SensirionI2cSht4x.h>
#include <SensirionI2CSgp41.h>
#include <BH1750.h>
#include <arduinoFFT.h>
#include <ESP32Servo.h>
#include <EEPROM.h>

// ==========================================
// [SECTION 1] 사용자 설정 (Global Configuration)
// ==========================================
namespace Config {
    // --- WiFi 설정 ---
    // 실제 접속할 공유기(AP) 정보 (사용자 컴퓨터와 동일한 네트워크)
    const char* TARGET_SSID = "DIRECT-ZvC145x Series le";
    const char* TARGET_PASS = "11111111";
    
    // 접속 실패 시 생성할 자체 네트워크 이름 (DHCP 서버 역할)
    const char* HUB_AP_SSID = "S3_SENSOR_HUB_RECOVERY";
    const char* HUB_AP_PASS = "12345678";
    
    constexpr int WIFI_CONNECT_TIMEOUT_MS = 10000; // WiFi 접속 시도 제한 시간

    // --- 하드웨어 핀 할당 ---
    constexpr int PIN_SERVO = 2;    // 서보 모터 제어 핀 (D1)
    constexpr int PIN_SOUND = 1;    // 사운드 센서 아날로그 입력 (A0)
    constexpr int PIN_I2C_SDA = 5;  // I2C 데이터 핀
    constexpr int PIN_I2C_SCL = 6;  // I2C 클럭 핀

    // --- 분석 및 샘플링 설정 ---
    constexpr uint16_t FFT_SAMPLES = 4096;     // 8192에서 4096으로 조정 (S3 메모리 안정성 고려)
    constexpr double SAMPLING_FREQ = 8000.0;   // 샘플링 주파수 (8kHz)
    constexpr uint32_t TIMER0_INTERVAL_US = 125; // 8kHz 주기를 위한 타이머 간격 (1,000,000 / 8,000)
    constexpr uint32_t TIMER1_INTERVAL_MS = 100; // 센서 수집 주기 (100ms)

    // --- 기타 설정 ---
    constexpr int EEPROM_SIZE = 512;
}

// ==========================================
// [SECTION 2] 전역 변수 및 객체 (Global States)
// ==========================================
// 센서 및 장치 객체
AsyncWebServer server(80);
// AsyncEventSource 제거 (JSON Polling 방식으로 전환)
SensirionI2cSht4x sht4x;
SensirionI2CSgp41 sgp41;
BH1750 lightMeter;
Servo myservo;

// 메모리 패닉 방지용 정적 할당
static double vReal[Config::FFT_SAMPLES];
static double vImag[Config::FFT_SAMPLES];
ArduinoFFT<double> FFT = ArduinoFFT<double>(vReal, vImag, Config::FFT_SAMPLES, Config::SAMPLING_FREQ);

// 센서 측정값 전역 저장소
struct SensorData {
    float temp = 0.0, humi = 0.0, lux = 0.0;
    uint16_t voc = 0, nox = 0;
    bool hasSHT = false, hasBH = false, hasSGP = false;
} gData;

// 시스템 상태 및 제어 플래그
volatile bool isBufferFull = false;
volatile bool timerSensorFlag = false;
volatile bool servoTriggered = false;
int servoStart, servoEnd, motionDelay;
String stSSID, stPW, fftJsonData = "[]";

// 타이머 객체
hw_timer_t *timer0 = NULL; // 사운드 샘플링용
hw_timer_t *timer1 = NULL; // 센서 수집용

// --- 시스템 상태 제어 ---
volatile bool systemReady = false; // 모든 초기화 완료 후 TRUE 전환

// ==========================================
// [SECTION 3] 하드웨어 타이머 ISR (Interrupt Services)
// ==========================================

/**
 * @brief 사운드 샘플링 타이머 ISR (8kHz)
 */
void IRAM_ATTR onTimer0() {
    if (!systemReady) return; // 초기화 중 간섭 방지
    
    static int sampleIndex = 0;
    if (!isBufferFull) {
        vReal[sampleIndex] = (double)analogRead(Config::PIN_SOUND);
        vImag[sampleIndex] = 0;
        sampleIndex++;
        if (sampleIndex >= Config::FFT_SAMPLES) {
            sampleIndex = 0;
            isBufferFull = true;
        }
    }
}

/**
 * @brief 센서 수합 주기 타이머 ISR (100ms)
 */
void IRAM_ATTR onTimer1() {
    if (!systemReady) return;
    timerSensorFlag = true;
}

// ==========================================
// [SECTION 4] 유틸리티 및 헬퍼 함수 (Helpers)
// ==========================================

/**
 * @brief I2C 장치 연결 확인
 * @param address 7비트 I2C 주소
 * @return 연결 성공 여부
 */
bool checkI2CConnection(uint8_t address) {
    Wire.beginTransmission(address);
    return (Wire.endTransmission() == 0);
}

/**
 * @brief EEPROM 문자열 쓰기
 */
void writeEEPROMString(int startAddr, String data) {
    for (int i = 0; i < data.length(); i++) {
        EEPROM.write(startAddr + i, data[i]);
    }
    EEPROM.write(startAddr + data.length(), '\0');
}

/**
 * @brief EEPROM 문자열 읽기
 */
String readEEPROMString(int startAddr) {
    String result = "";
    char ch;
    int i = 0;
    while ((ch = EEPROM.read(startAddr + i)) != '\0' && i < 64) {
        result += ch;
        i++;
    }
    return (result.length() == 0 || (uint8_t)result[0] == 255) ? "" : result;
}

// ==========================================
// [SECTION 5] 시각적 프리미엄 웹 UI (Premium Dashboard)
// ==========================================

/**
 * @brief 현대적인 글래스모피즘 아키텍처의 HTML 페이지 생성
 */
String buildHtmlPage() {
    String p = "<!DOCTYPE html><html lang='ko'><head><meta charset='UTF-8'>";
    p += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    p += "<title>ESP32 Premium Sensor Hub</title>";
    
    // --- CSS: Premium Glassmorphism Theme ---
    p += "<style>";
    p += ":root{--bg:#08080c; --glass:rgba(255,255,255,0.05); --primary:#00ff88; --accent:#7000ff; --text:#e0e0e0;}";
    p += "body{margin:0; font-family:'Inter',system-ui,-apple-system,sans-serif; background:var(--bg); color:var(--text); overflow-x:hidden;}";
    p += ".container{max-width:1200px; margin:0 auto; padding:20px;}";
    p += "h2{font-weight:300; letter-spacing:1px; color:var(--primary); text-shadow:0 0 10px rgba(0,255,136,0.3);}";
    p += ".grid{display:grid; grid-template-columns:repeat(auto-fit, minmax(300px, 1fr)); gap:20px; margin-bottom:20px;}";
    p += ".card{background:var(--glass); backdrop-filter:blur(10px); border:1px solid rgba(255,255,255,0.1); border-radius:16px; padding:20px; transition:0.3s;}";
    p += ".card:hover{border-color:var(--primary); box-shadow:0 0 20px rgba(0,255,136,0.1);}";
    p += "canvas{width:100% !important; height:200px !important; border-radius:8px;}";
    p += ".status-bar{display:flex; gap:10px; font-size:12px; margin-bottom:20px; opacity:0.7;}";
    p += ".status-item{padding:4px 10px; border-radius:20px; border:1px solid currentColor;}";
    p += ".btn{background:var(--accent); color:#white; border:none; padding:12px 24px; border-radius:8px; cursor:pointer; font-weight:bold; transition:0.2s; width:100%; border:1px solid transparent;}";
    p += ".btn:hover{filter:brightness(1.2); border-color:var(--primary);}";
    p += ".config-group{margin-top:15px;} label{display:block; font-size:12px; opacity:0.6; margin-bottom:5px;}";
    p += "input{background:rgba(0,0,0,0.3); border:1px solid #444; color:white; padding:10px; border-radius:6px; width:100%; box-sizing:border-box; margin-bottom:10px;}";
    p += "</style></head><body>";

    // --- HTML Structure ---
    p += "<div class='container'><h2>ESP32-S3 AI SENSOR HUB <small style='font-size:12px; opacity:0.5;'>V5.1</small></h2>";
    p += "<div class='status-bar'>";
    p += "<div id='st1' class='status-item'>SHT: --</div><div id='st2' class='status-item'>BH: --</div><div id='st3' class='status-item'>SGP: --</div></div>";

    p += "<div class='grid'>";
    p += "<div class='card' style='grid-column: span 2;'><b>Real-time FFT Spectrum</b><canvas id='fC'></canvas></div>";
    p += "</div>";

    p += "<div class='grid' style='grid-template-columns: repeat(auto-fit, minmax(250px, 1fr));'>";
    p += "<div class='card'><b>Temperature (&deg;C)</b><canvas id='tC'></canvas></div>";
    p += "<div class='card'><b>Humidity (%)</b><canvas id='hC'></canvas></div>";
    p += "<div class='card'><b>Light (Lux)</b><canvas id='lC'></canvas></div>";
    p += "<div class='card'><b>Air Quality (VOC)</b><canvas id='vC'></canvas></div>";
    p += "</div>";

    p += "<div class='grid'>";
    p += "<div class='card'><h3>System Setup</h3>";
    p += "<div class='config-group'><label>WiFi SSID / Password</label>";
    p += "<input type='text' id='ws' value='"+stSSID+"' placeholder='SSID'><input type='password' id='wp' value='"+stPW+"' placeholder='Password'>";
    p += "<button onclick='saveWiFi()' class='btn'>Connect & Restart</button></div></div>";
    
    p += "<div class='card'><h3>Actuator Control</h3>";
    p += "<div class='config-group'><label>Servo Range (Start/End) & Delay</label><div style='display:flex; gap:5px;'>";
    p += "<input type='number' id='ss' value='"+String(servoStart)+"'><input type='number' id='se' value='"+String(servoEnd)+"'><input type='number' id='sd' value='"+String(motionDelay)+"'></div>";
    p += "<button onclick='saveServo()' class='btn' style='background:#444; margin-bottom:10px;'>Update Values</button>";
    p += "<button onclick='triggerServo()' class='btn' style='background:var(--primary); color:#000;'>Manual Trigger</button></div></div>";
    p += "</div></div>";

    // --- JavaScript Logic ---
    p += "<script>var fC=document.getElementById('fC'), fCtx=fC.getContext('2d');";
    p += "var tC=document.getElementById('tC'), hC=document.getElementById('hC'), lC=document.getElementById('lC'), vC=document.getElementById('vC');";
    p += "var tCtx=tC.getContext('2d'), hCtx=hC.getContext('2d'), lCtx=lC.getContext('2d'), vCtx=vC.getContext('2d');";
    p += "var sD = {t:[], h:[], l:[], v:[]}; var maxLen=60;";
    
    // [Draw] FFT Visualization
    p += "function drawFFT(d){ fC.width=fC.offsetWidth; fC.height=fC.offsetHeight; fCtx.clearRect(0,0,fC.width,fC.height);";
    p += "var barW=(fC.width-40)/32; var grad=fCtx.createLinearGradient(0,fC.height,0,0); grad.addColorStop(0,'#003322'); grad.addColorStop(1,'#00ff88');";
    p += "for(var i=0;i<32;i++){ var h=(d[i].mag/500)*(fC.height-20); fCtx.fillStyle=grad; fCtx.fillRect(20+i*barW+2, fC.height-h, barW-4, h); ";
    p += "if(i%8==0){ fCtx.fillStyle='#666'; fCtx.font='10px Arial'; fCtx.fillText(d[i].freq+'Hz', 20+i*barW, fC.height-5); } } }";

    // [Draw] Single Line Chart Helper
    p += "function drawLine(ctx, can, data, color, max){ can.width=can.offsetWidth; can.height=can.offsetHeight; ctx.clearRect(0,0,can.width,can.height);";
    p += "ctx.beginPath(); ctx.strokeStyle=color; ctx.lineWidth=2; for(var i=0;i<data.length;i++){ var x=(i/(maxLen-1))*can.width, y=can.height-(data[i]/max)*can.height*0.8-10;";
    p += "if(i==0) ctx.moveTo(x,y); else ctx.lineTo(x,y); } ctx.stroke(); }";

    // Polling Logic / Get Current Data
    p += "async function updateData(){ try{ var res=await fetch('/data'); var d=await res.json();";
    p += "document.getElementById('st1').innerHTML='SHT: '+(d.c1? (d.temp.toFixed(1)+'&deg;C / '+d.humi.toFixed(1)+'%') : 'Err'); document.getElementById('st1').style.color=d.c1?'#0f8':'#f44';";
    p += "document.getElementById('st2').innerHTML='BH: '+(d.c2? (d.lux.toFixed(1)+' lx') : 'Err'); document.getElementById('st2').style.color=d.c2?'#0f8':'#f44';";
    p += "document.getElementById('st3').innerHTML='SGP: '+(d.c3? ('VOC: '+d.voc) : 'Err'); document.getElementById('st3').style.color=d.c3?'#0f8':'#f44';";
    p += "sD.t.push(d.temp); sD.h.push(d.humi); sD.l.push(d.lux); sD.v.push(d.voc); if(sD.t.length>maxLen){['t','h','l','v'].forEach(k=>sD[k].shift());}";
    p += "drawFFT(d.fft); drawLine(tCtx,tC,sD.t,'#ff4444',50); drawLine(hCtx,hC,sD.h,'#4444ff',100); drawLine(lCtx,lC,sD.l,'#ffaa00',1000); drawLine(vCtx,vC,sD.v,'#00ff88',65535);";
    p += "} catch(e){ console.error(e); } }";
    p += "setInterval(updateData, 1000); updateData();";
    
    p += "function saveWiFi(){ location.href='/set_wifi?ssid='+document.getElementById('ws').value+'&pass='+document.getElementById('wp').value; }";
    p += "function saveServo(){ fetch('/set_servo?s='+document.getElementById('ss').value+'&e='+document.getElementById('se').value+'&d='+document.getElementById('sd').value).then(()=>alert('Updated')); }";
    p += "function triggerServo(){ fetch('/trigger'); }</script></body></html>";
    return p;
}

// ==========================================
// [SECTION 6] 핵심 관리 함수 (Core Functional Logic)
// ==========================================

/**
 * @brief I2C 버스 복구 핸들러
 * 버스가 잠겼을 때 강제로 SCL 클럭을 인가하여 세션을 종료합니다.
 */
void recoverI2CBus() {
    Serial.println("[I2C] Attempting Bus Recovery...");
    pinMode(Config::PIN_I2C_SCL, OUTPUT);
    for (int i = 0; i < 10; i++) {
        digitalWrite(Config::PIN_I2C_SCL, LOW); delayMicroseconds(5);
        digitalWrite(Config::PIN_I2C_SCL, HIGH); delayMicroseconds(5);
    }
    Wire.begin(Config::PIN_I2C_SDA, Config::PIN_I2C_SCL);
}

void initSensors() {
    Serial.println("Initializing I2C Bus (Pins 5, 6)...");
    
    // XIAO ESP32-S3 하드웨어 I2C 특징에 맞춰 타임아웃/속도 설정
    recoverI2CBus(); 
    Wire.setClock(100000); 
    Wire.setTimeOut(100); // 하드웨어 타임아웃 연장 (중요)
    delay(500);

    gData.hasBH = checkI2CConnection(0x23);
    if (gData.hasBH) {
        lightMeter.begin();
        Serial.println(" - BH1750 OK");
    }

    gData.hasSHT = checkI2CConnection(0x44);
    if (gData.hasSHT) {
        sht4x.begin(Wire, 0x44);
        sht4x.softReset(); // 센서 내부 상태 초기화
        delay(100);
        Serial.println(" - SHT4x OK");
    }

    gData.hasSGP = checkI2CConnection(0x59);
    if (gData.hasSGP) {
        sgp41.begin(Wire);
        Serial.println(" - SGP41 OK");
    }
}

/**
 * @brief 하드웨어 타이머 설정 (사운드 및 센서)
 */
void setupTimers() {
    // Timer 0: 사운드 샘플링 (8kHz)
    // Core 3.x API: 1MHz frequency (1us tick) 설정
    timer0 = timerBegin(1000000); 
    timerAttachInterrupt(timer0, &onTimer0);
    // 8kHz 주기를 위해 TIMER0_INTERVAL_US(125us) 마다 알람 발생, 자동 재로드 활성화
    timerAlarm(timer0, Config::TIMER0_INTERVAL_US, true, 0);

    // Timer 1: 센서 데이터 수집 (100ms)
    timer1 = timerBegin(1000000);
    timerAttachInterrupt(timer1, &onTimer1);
    // 100ms 주기를 위해 알람 발생
    timerAlarm(timer1, Config::TIMER1_INTERVAL_MS * 1000, true, 0);
}

/**
 * @brief Wi-Fi 연결 및 AP 모드 폴백
 */
void setupNetwork() {
    Serial.println("\n--- WiFi Connection ---");
    
    // 1. 시도할 정보 결정 (EEPROM 정보가 없으면 Config의 기본 TARGET 정보 사용)
    String ssidToTry = (stSSID.length() > 0) ? stSSID : Config::TARGET_SSID;
    String passToTry = (stSSID.length() > 0) ? stPW : Config::TARGET_PASS;

    // 네트워크 리셋
    WiFi.disconnect(true);
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(500); 

    // 2. Station 모드로 접속 시도
    Serial.printf("Connecting to Network: %s\n", ssidToTry.c_str());
    WiFi.mode(WIFI_STA); 
    WiFi.setAutoReconnect(true); // 연결 끊김 시 자동 재접속 활성화
    delay(200);
    WiFi.begin(ssidToTry.c_str(), passToTry.c_str());
    
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < Config::WIFI_CONNECT_TIMEOUT_MS) {
        delay(500);
        Serial.print(".");
    }

    // 3. 접속 실패 시 자체 Recovery AP 모드 전환
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\n[!] Connection Failed. Starting Recovery AP Mode...");
        WiFi.mode(WIFI_OFF);
        delay(300);
        
        WiFi.mode(WIFI_AP); // 안전하게 순수 AP 모드로 전환
        delay(200);
        
        bool apResult = WiFi.softAP(Config::HUB_AP_SSID, Config::HUB_AP_PASS);
        delay(500); 
        
        if (apResult) {
            Serial.printf("Recovery AP Started: %s\n", Config::HUB_AP_SSID);
            Serial.print("Access Dashboard IP: "); Serial.println(WiFi.softAPIP());
        } else {
            Serial.println("Error: Failed to start AP.");
        }
    } else {
        Serial.println("\n[OK] WiFi Connected Successfully!");
        Serial.print("Dashboard IP: "); Serial.println(WiFi.localIP());
    }
}

/**
 * @brief 웹 서버 핸들러 및 API 설정
 */
void setupWebServer() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *req){
        req->send(200, "text/html", buildHtmlPage());
    });

    server.on("/data", HTTP_GET, [](AsyncWebServerRequest *req){
        String payload;
        payload.reserve(1024);
        payload = "{\"temp\":" + String(gData.temp, 1) + 
                  ",\"humi\":" + String(gData.humi, 1) + 
                  ",\"lux\":" + String(gData.lux,1) + 
                  ",\"voc\":" + String(gData.voc) + 
                  ",\"c1\":" + String(gData.hasSHT) + 
                  ",\"c2\":" + String(gData.hasBH) + 
                  ",\"c3\":" + String(gData.hasSGP) + 
                  ",\"fft\":" + fftJsonData + "}";
        req->send(200, "application/json", payload);
    });

    server.on("/set_wifi", HTTP_GET, [](AsyncWebServerRequest *req){
        if (req->hasParam("ssid")) writeEEPROMString(12, req->getParam("ssid")->value());
        if (req->hasParam("pass")) writeEEPROMString(44, req->getParam("pass")->value());
        EEPROM.commit();
        req->send(200, "text/html", "Restarting in 2s...");
        delay(2000); ESP.restart();
    });

    server.on("/set_servo", HTTP_GET, [](AsyncWebServerRequest *req){
        if (req->hasParam("s")) servoStart = req->getParam("s")->value().toInt();
        if (req->hasParam("e")) servoEnd = req->getParam("e")->value().toInt();
        if (req->hasParam("d")) motionDelay = req->getParam("d")->value().toInt();
        EEPROM.writeInt(0, servoStart); EEPROM.writeInt(4, servoEnd); EEPROM.writeInt(8, motionDelay);
        EEPROM.commit();
        req->send(200, "text/plain", "OK");
    });

    server.on("/trigger", HTTP_GET, [](AsyncWebServerRequest *req){
        servoTriggered = true;
        req->send(200, "text/plain", "OK");
    });

    server.begin();
}

/**
 * @brief FFT 연산 및 JSON 직렬화
 */
void processFFTLogic() {
    FFT.windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);
    FFT.compute(FFT_FORWARD);
    FFT.complexToMagnitude();

    // 32개 밴드로 압축하여 전송량 최적화
    String json = "[";
    int samplesPerBin = Config::FFT_SAMPLES / 2 / 32;
    for (int i = 0; i < 32; i++) {
        double magSum = 0;
        for (int j = 0; j < samplesPerBin; j++) {
            magSum += vReal[i * samplesPerBin + j + 2]; // DC 성분 제외
        }
        json += "{\"freq\":" + String((int)(i * (Config::SAMPLING_FREQ / 2 / 32))) + ",\"mag\":" + String(magSum / samplesPerBin, 1) + "}";
        if (i < 31) json += ",";
    }
    json += "]";
    fftJsonData = json;
    isBufferFull = false;
}

/**
 * @brief 센서 데이터 읽기 및 전역 변수 업데이트
 * Sensirion 에러(270) 최소화를 위해 업데이트 주기를 분산하고 안전 장치 마련
 */
void updateSensorData() {
    static uint32_t shtTick = 0;
    static uint32_t sgpTick = 0;
    static int consecutiveErrors = 0;

    // 1. SHT4x (온습도) - 안정성을 위해 2초 마다 측정
    if (gData.hasSHT && (millis() - shtTick >= 2000)) {
        shtTick = millis();
        uint16_t error = sht4x.measureHighPrecision(gData.temp, gData.humi);
        if (error) {
            consecutiveErrors++;
            // 에러 출력 빈도 감소 (시리얼 모니터 가독성)
            if (consecutiveErrors % 5 == 0) Serial.printf("[SHT] I2C Status: %u\n", error);
        } else {
            consecutiveErrors = 0;
        }
    }
    
    // 2. SGP41 (공기질) - 리소스 보호를 위해 1초 주기로 측정
    if (gData.hasSGP && (millis() - sgpTick >= 1000)) {
        sgpTick = millis();
        sgp41.measureRawSignals(gData.humi, gData.temp, gData.voc, gData.nox);
    }
    
    // 3. BH1750 (조도) - 100ms 마다 업데이트
    if (gData.hasBH) {
        float l = lightMeter.readLightLevel();
        if (l >= 0) gData.lux = l;
    }

    // 버스 잠김 의심 시 자동 복구 (에러 지속 시)
    if (consecutiveErrors > 20) {
        Serial.println("[System] Bus freeze detected. Recovering...");
        recoverI2CBus();
        consecutiveErrors = 0;
    }

    timerSensorFlag = false;
}

/**
 * @brief 개별 센서 정밀 진단 함수들
 */
void testSHT4x() {
    Serial.print("\n[Test] SHT4x (Temp/Humi): ");
    if (!gData.hasSHT) { Serial.println("NOT FOUND"); return; }
    
    float t, h;
    uint16_t err = sht4x.measureHighPrecision(t, h);
    if (err) {
        Serial.printf("FAILED (Error code: %u)\n", err);
    } else {
        Serial.printf("OK! -> Temp: %.2fC, Humi: %.2f%%\n", t, h);
    }
}

void testBH1750() {
    Serial.print("[Test] BH1750 (Light): ");
    if (!gData.hasBH) { Serial.println("NOT FOUND"); return; }
    
    float lux = lightMeter.readLightLevel();
    if (lux < 0) {
        Serial.println("FAILED");
    } else {
        Serial.printf("OK! -> Light: %.2f Lux\n", lux);
    }
}

void testSGP41() {
    Serial.print("[Test] SGP41 (Air Quality): ");
    if (!gData.hasSGP) { Serial.println("NOT FOUND"); return; }
    
    uint16_t voc, nox;
    uint16_t err = sgp41.measureRawSignals(50.0f, 25.0f, voc, nox); // 임시 보정값 사용
    if (err) {
        Serial.printf("FAILED (Error code: %u)\n", err);
    } else {
        Serial.printf("OK! -> VOC Raw: %u, NOX Raw: %u\n", voc, nox);
    }
}

void testSoundSensor() {
    Serial.print("[Test] Sound Sensor (A0): ");
    int raw = analogRead(Config::PIN_SOUND);
    Serial.printf("Current Raw Value: %d (0~4095)\n", raw);
}

/**
 * @brief I2C 버스 스캐너
 * 현재 버스에 연결된 모든 장치의 주소를 출력합니다.
 */
void scanI2CBus() {
    Serial.println("\n[Scan] Scanning I2C Bus (SDA:5, SCL:6)...");
    byte count = 0;
    for (byte address = 1; address < 127; address++) {
        Wire.beginTransmission(address);
        if (Wire.endTransmission() == 0) {
            Serial.printf(" - Found device at 0x%02X", address);
            if (address == 0x23) Serial.print(" (BH1750)");
            else if (address == 0x44) Serial.print(" (SHT4x)");
            else if (address == 0x59) Serial.print(" (SGP41)");
            Serial.println();
            count++;
        }
    }
    if (count == 0) {
        Serial.println(" [!] No I2C devices found. Check wiring (SDA->GPIO 5, SCL->GPIO 6) and Power (3.3V).");
    } else {
        Serial.printf(" [OK] %d device(s) found.\n", count);
    }
}

void runFullSensorDiagnostic() {
    Serial.println("\n========= SYSTEM HARDWARE DIAGNOSTIC =========");
    scanI2CBus(); // I2C 버스 상태 전수 조사
    
    testSHT4x();
    testBH1750();
    testSGP41();
    testSoundSensor();
    Serial.println("==============================================");
}

// ==========================================
// [SECTION 7] FreeRTOS 워커 태스크 (Core Allocation)
// ==========================================

/**
 * @brief Core 1 전용 연산 태스크
 * 샘플링 플래그 체크 및 FFT 연산을 수행합니다.
 */
void processingTask(void *pvParameters) {
    uint32_t lastPulse = 0;
    Serial.println("[Task] Processing Task Started on Core 1.");
    
    while (1) {
        if (isBufferFull) {
            processFFTLogic();
        }
        
        if (timerSensorFlag) {
            updateSensorData();
        }

        // 브라우저 및 curl용 데이터 준비 (JSON 응용을 위해 보관)
        if (millis() - lastPulse >= 1000) {
            lastPulse = millis();
            
            // 시리얼 모니터 가독성을 위해 상세 로그 출력
            Serial.printf("[Info] Sensor Updated | Temp: %.1fC, Humi: %.1f%%, Lux: %.1f, VOC: %u\n", 
                          gData.temp, gData.humi, gData.lux, gData.voc);
        }

        if (servoTriggered) {
            myservo.writeMicroseconds(servoEnd);
            vTaskDelay(pdMS_TO_TICKS(motionDelay));
            myservo.writeMicroseconds(servoStart);
            servoTriggered = false;
        }

        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
}

// ==========================================
// [SECTION 8] 표준 진입점 (Setup & Loop)
// ==========================================

void setup() {
    Serial.begin(115200);
    delay(1000); // S3 USB 직렬 포트 안정화를 위한 시작 지연
    Serial.println("\n\n=== ESP32-S3 SENSOR HUB SYSTEM START ===");

    EEPROM.begin(Config::EEPROM_SIZE);

    // EEPROM 설정 로드
    servoStart = EEPROM.readInt(0);
    servoEnd = EEPROM.readInt(4);
    motionDelay = EEPROM.readInt(8);
    stSSID = readEEPROMString(12);
    stPW = readEEPROMString(44);

    // 기본값 설정 (EEPROM이 비었을 경우)
    if (servoStart < 500 || servoStart > 2500) { 
        servoStart = 1500; servoEnd = 1800; motionDelay = 200; 
    }

    // 하드웨어 초기화
    myservo.attach(Config::PIN_SERVO, 500, 2500);
    myservo.writeMicroseconds(servoStart);
    initSensors();
    
    // 개별 센서 정밀 진단 수행 (시리얼 출력)
    runFullSensorDiagnostic();
    
    // 네트워크 설정 (가장 민감한 작업을 타이머 시작 전에 처리)
    setupNetwork();
    setupWebServer();

    // 분석 태스크 생성 (Core 1)
    xTaskCreatePinnedToCore(processingTask, "ProcTask", 8192, NULL, 5, NULL, 1);
    
    // 전역 시스템 준비 완료 선언 (ISR이 이때부터 활성화됨)
    systemReady = true;

    // 하드웨어 타이머 활성화
    setupTimers();
    
    Serial.println("System Initialization Complete. ISR and Processing Task started.");
}

void loop() {
    // 시리얼 입력을 통한 수동 진단 트리거
    if (Serial.available()) {
        char c = Serial.read();
        if (c == 'd' || c == 't' || c == 'D' || c == 'T') {
            runFullSensorDiagnostic();
        }
    }
    
    vTaskDelay(100 / portTICK_PERIOD_MS);
}