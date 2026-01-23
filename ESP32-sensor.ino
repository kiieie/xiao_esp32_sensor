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
#include <arduinoFFT.h>
#include <ESP32Servo.h>
#include <EEPROM.h>
#include <VOCGasIndexAlgorithm.h>
#include <NOxGasIndexAlgorithm.h>

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
    constexpr int EEPROM_SIZE = 1024; // SSID, PW, Mode, AP_SSID, AP_PW 등 저장공간 확보
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
VOCGasIndexAlgorithm voc_algorithm;
NOxGasIndexAlgorithm nox_algorithm;

// 메모리 패닉 방지용 정적 할당
static double vReal[Config::FFT_SAMPLES];
static double vImag[Config::FFT_SAMPLES];
ArduinoFFT<double> FFT = ArduinoFFT<double>(vReal, vImag, Config::FFT_SAMPLES, Config::SAMPLING_FREQ);

// 센서 측정값 전역 저장소
struct SensorData {
    float temp = 25.0, humi = 50.0, lux = 0.0; // SHT가 없을 때를 위한 기본값
    uint16_t voc = 0, nox = 0;
    uint16_t srawVoc = 0, srawNox = 0;
    bool hasBH = false, hasSGP = false, hasSHT = false;
} gData;

// 시스템 상태 및 제어 플래그
volatile bool isBufferFull = false;
volatile bool timerSensorFlag = false;
volatile bool servoTriggered = false;
int servoStart, servoEnd, motionDelay;
String stSSID, stPW, fftJsonData = "[]";
String apSSID, apPW;
int wifiMode = 0; // 0: Client, 1: AP
SemaphoreHandle_t fftMutex; // Mutex for thread-safe FFT data access

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
 * @brief 지수 평활 필터 (Exponential Moving Average) 헬퍼
 * @param current 현재 측정값
 * @param prev 이전 필터링된 값
 * @param alpha 가중치 (0.0~1.0, 낮을수록 더 부드러움)
 * @return 필터링된 결과값
 */
float applyFilter(float current, float prev, float alpha) {
    if (prev == 0.0f) return current; // 초기값 설정
    return (current * alpha) + (prev * (1.0f - alpha));
}

/**
 * @brief I2C 장치 연결 확인
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
    p += "<title>AI Sensor Hub Elite</title>";
    p += "<link rel='preconnect' href='https://fonts.googleapis.com'><link rel='preconnect' href='https://fonts.gstatic.com' crossorigin>";
    p += "<link href='https://fonts.googleapis.com/css2?family=Outfit:wght@300;400;600&display=swap' rel='stylesheet'>";
    
    // --- CSS: Ultra-Premium Glassmorphism Theme ---
    p += "<style>";
    p += ":root{--primary:#00ffaa; --secondary:#7000ff; --accent:#ff0077; --bg:#050510; --card-bg:rgba(20,20,35,0.4); --text:#ffffff;}";
    p += "*{margin:0; padding:0; box-sizing:border-box;}";
    p += "body{font-family:'Outfit',sans-serif; background:var(--bg); color:var(--text); min-height:100vh; overflow-x:hidden; position:relative;}";
    p += ".mesh{position:fixed; top:0; left:0; width:100%; height:100%; z-index:-1; background:radial-gradient(at 10% 10%, #1a0f30 0%, transparent 50%), radial-gradient(at 90% 10%, #0a2a20 0%, transparent 50%), radial-gradient(at 50% 90%, #200a1a 0%, transparent 50%); filter:blur(100px); animation:meshShift 20s infinite alternate;}";
    p += "@keyframes meshShift{0%{transform:scale(1);} 100%{transform:scale(1.2) rotate(5deg);}}";
    p += ".container{max-width:1400px; margin:0 auto; padding:30px; position:relative; z-index:1;}";
    p += "header{display:flex; justify-content:space-between; align-items:center; margin-bottom:40px;}";
    p += "h2{font-weight:600; font-size:28px; letter-spacing:-0.5px; background:linear-gradient(to right, #fff, #888); -webkit-background-clip:text; -webkit-text-fill-color:transparent;}";
    p += ".badge{padding:6px 15px; border-radius:50px; font-size:12px; font-weight:600; background:rgba(255,255,255,0.1); border:1px solid rgba(255,255,255,0.2); backdrop-filter:blur(5px);}";
    p += ".hero-grid{display:grid; grid-template-columns:repeat(auto-fit, minmax(200px, 1fr)); gap:15px; margin-bottom:30px;}";
    p += ".hero-card{background:var(--card-bg); backdrop-filter:blur(20px); border:1px solid rgba(255,255,255,0.1); border-radius:24px; padding:20px; transition:0.4s cubic-bezier(0.2,0.8,0.2,1); position:relative; overflow:hidden;}";
    p += ".hero-card:hover{transform:translateY(-5px); border-color:rgba(255,255,255,0.3);}";
    p += ".label{font-size:13px; opacity:0.6; margin-bottom:8px; display:flex; justify-content:space-between; align-items:center;}";
    p += ".value{font-size:28px; font-weight:600;} .unit{font-size:14px; opacity:0.5; margin-left:3px;}";
    p += ".status-dot{width:8px; height:8px; border-radius:50%; background:#444;} .status-active{background:var(--primary); box-shadow:0 0 10px var(--primary);}";
    p += ".main-grid{display:grid; grid-template-columns:2fr 1fr; gap:20px; margin-bottom:30px;}";
    p += ".chart-card{background:var(--card-bg); backdrop-filter:blur(20px); border:1px solid rgba(255,255,255,0.08); border-radius:24px; padding:25px;}";
    p += "@media (max-width:1000px){.main-grid{grid-template-columns:1fr;}}";
    p += ".sub-grid{display:grid; grid-template-columns:repeat(auto-fit, minmax(260px, 1fr)); gap:20px;}";
    p += "canvas{width:100% !important; height:200px !important; margin-top:10px;}";
    p += ".panel{background:rgba(255,255,255,0.03); border-radius:20px; padding:20px; margin-top:20px; border:1px solid rgba(255,255,255,0.05);}";
    p += "h3{font-size:16px; margin-bottom:15px; opacity:0.8; font-weight:400;}";
    p += "input, select{background:rgba(0,0,0,0.4); border:1px solid rgba(255,255,255,0.1); color:white; padding:12px; border-radius:12px; width:100%; margin-bottom:15px; font-family:inherit;}";
    p += ".btn{background:linear-gradient(135deg, var(--secondary), #4e00b3); color:white; border:none; padding:12px; border-radius:12px; cursor:pointer; font-weight:600; width:100%; transition:0.3s;}";
    p += ".btn:hover{filter:brightness(1.2); transform:scale(1.02);} .btn-prime{background:linear-gradient(135deg, var(--primary), #00cc88); color:#000;}";
    p += ".sys-list{list-style:none; padding:0;} .sys-item{display:flex; justify-content:space-between; padding:8px 0; border-bottom:1px solid rgba(255,255,255,0.05); font-size:13px;}";
    p += ".sys-item span{opacity:0.6;} .sys-item b{color:var(--primary);}";
    p += "</style></head><body><div class='mesh'></div>";

    p += "<div class='container'><header><h2>DATA CENTER ENVIRONMENT MONITORING SENSOR HUB <small style='font-weight:300; opacity:0.4;'>V7.4</small></h2>";
    p += "<div class='badge' id='top_info'>System Online</div></header>";

    p += "<div class='hero-grid'>";
    p += "<div class='hero-card'><div class='label'>VOC <div id='dot_voc' class='status-dot'></div></div><div class='value' id='h_voc'>--</div></div>";
    p += "<div class='hero-card'><div class='label'>NOx <div id='dot_nox' class='status-dot'></div></div><div class='value' id='h_nox'>--</div></div>";
    p += "<div class='hero-card'><div class='label'>TEMP <div id='dot_temp' class='status-dot'></div></div><div class='value' id='h_temp'>--<span class='unit'>&deg;C</span></div></div>";
    p += "<div class='hero-card'><div class='label'>HUMI <div id='dot_humi' class='status-dot'></div></div><div class='value' id='h_humi'>--<span class='unit'>%</span></div></div>";
    p += "<div class='hero-card'><div class='label'>LIGHT <div id='dot_lux' class='status-dot'></div></div><div class='value' id='h_lux'>--<span class='unit'>lx</span></div></div>";
    p += "</div>";

    p += "<div class='main-grid'>";
    p += "<div class='chart-card' style='display:flex; flex-direction:column; height:100%;'><b>Acoustic Spectrogram (FFT)</b><canvas id='fC' style='flex-grow:1; width:100%; min-height:300px; margin-top:10px;'></canvas></div>";
    p += "<div class='chart-card'><h3>System Configuration</h3><div class='sys-list'>";
    p += "<li class='sys-item'><span>Mode</span><b id='cur_mode'>--</b></li>";
    p += "<li class='sys-item'><span>IP</span><b id='cur_ip'>--</b></li>";
    p += "<li class='sys-item'><span>SSID</span><b id='cur_ssid'>--</b></li></div>";
    p += "<div class='panel'><h3>Network Mode</h3><select id='wm' onchange='togglePass()'><option value='0'>Client (STA)</option><option value='1'>Hotspot (AP)</option></select>";
    p += "<input type='text' id='ws' value='"+stSSID+"' placeholder='SSID'><input type='password' id='wp' value='"+stPW+"' placeholder='Password'>";
    p += "<button onclick='saveWiFi()' class='btn btn-prime'>Apply Network State</button></div></div></div>";

    p += "<div class='sub-grid'>";
    p += "<div class='chart-card'><b id='t_t'>Temperature</b><canvas id='tC'></canvas></div>";
    p += "<div class='chart-card'><b id='t_h'>Humidity</b><canvas id='hC'></canvas></div>";
    p += "<div class='chart-card'><b id='t_v'>VOC Index</b><canvas id='vC'></canvas></div>";
    p += "<div class='chart-card'><b id='t_n'>NOx Index</b><canvas id='nC'></canvas></div>";
    p += "<div class='chart-card'><b id='t_l'>Light Level</b><canvas id='lC'></canvas></div>";
    p += "</div>";

    p += "<div class='main-grid' style='margin-top:20px;'><div class='chart-card'><h3>Servo Controller</h3><div style='display:flex; gap:10px;'>";
    p += "<div style='flex:1;'><label style='font-size:10px; opacity:0.5;'>Start</label><input type='number' id='ss' value='"+String(servoStart)+"'></div>";
    p += "<div style='flex:1;'><label style='font-size:10px; opacity:0.5;'>End</label><input type='number' id='se' value='"+String(servoEnd)+"'></div>";
    p += "<div style='flex:1;'><label style='font-size:10px; opacity:0.5;'>Delay</label><input type='number' id='sd' value='"+String(motionDelay)+"'></div></div>";
    p += "<div style='display:flex; gap:10px;'><button onclick='saveServo()' class='btn'>Update</button><button onclick='triggerServo()' class='btn btn-prime'>Trigger</button></div></div>";
    p += "<div class='chart-card'><h3>Diagnostics</h3><div id='st_bh' class='badge' style='display:block; margin-bottom:8px;'>BH1750: --</div>";
    p += "<div id='st_sgp' class='badge' style='display:block;'>SGP41: --</div></div></div></div>";

    p += "<script>var fC=document.getElementById('fC'), fCtx=fC.getContext('2d');";
    p += "var tC=document.getElementById('tC'), hC=document.getElementById('hC'), vC=document.getElementById('vC'), nC=document.getElementById('nC'), lC=document.getElementById('lC');";
    p += "var tCtx=tC.getContext('2d'), hCtx=hC.getContext('2d'), vCtx=vC.getContext('2d'), nCtx=nC.getContext('2d'), lCtx=lC.getContext('2d');";
    p += "var sD={t:[], h:[], v:[], n:[], l:[]}; var maxLen=60;";
    
    p += "function drawFFT(d){ if(!d || d.length<32) return; fC.width=fC.offsetWidth; fC.height=fC.offsetHeight; fCtx.clearRect(0,0,fC.width,fC.height);";
    p += "var barW=(fC.width-50)/32; var grad=fCtx.createLinearGradient(0,fC.height,0,0); grad.addColorStop(0,'rgba(112,0,255,0.4)'); grad.addColorStop(1,'#00ffaa');";
    p += "for(var i=0;i<32;i++){ var h=(d[i].mag/400)*fC.height; if(h<2) h=2; fCtx.fillStyle=grad; fCtx.roundRect(40+i*barW+2, fC.height-h-12, barW-4, h, 4); fCtx.fill();";
    p += "if(i%8==0){ fCtx.fillStyle='rgba(255,255,255,0.4)'; fCtx.font='10px Outfit'; fCtx.fillText(d[i].freq+'Hz', 40+i*barW, fC.height-2); } } }";

    p += "function drawChart(ctx,can,data,color,max,id,name,unit,val,shId){";
    p += "can.width=can.offsetWidth; can.height=can.offsetHeight; var W=can.width, H=can.height, pL=40, pR=10, pT=20, pB=25; var dW=W-pL-pR, dH=H-pT-pB; ctx.clearRect(0,0,W,H);";
    p += "if(id) document.getElementById(id).innerHTML=name + '<span style=\"color:'+color+'; margin-left:8px; opacity:0.8;\">'+val+unit+'</span>';";
    p += "ctx.beginPath(); ctx.strokeStyle='rgba(255,255,255,0.1)'; ctx.lineWidth=1; ctx.font='10px Outfit'; ctx.textAlign='right'; ctx.fillStyle='rgba(255,255,255,0.4)';";
    p += "for(var i=0;i<=4;i++){ var y=pT+dH-(dH*i/4); ctx.moveTo(pL,y); ctx.lineTo(W-pR,y); ctx.fillText(Math.round(max*i/4),pL-10,y+4); } ctx.stroke();";
    p += "if(data.length<2) return; ctx.beginPath(); for(var i=0;i<data.length;i++){ var x=pL+(i/(maxLen-1))*dW; var dv=data[i]>max?max:data[i]; var y=pT+dH-(dv/max)*dH; i==0?ctx.moveTo(x,y):ctx.lineTo(x,y); }";
    p += "ctx.save(); var gr=ctx.createLinearGradient(0,pT,0,H); gr.addColorStop(0,color); gr.addColorStop(1,'transparent'); ctx.strokeStyle=color; ctx.lineWidth=3; ctx.lineCap='round'; ctx.lineJoin='round'; ctx.stroke();";
    p += "ctx.lineTo(pL+((data.length-1)/(maxLen-1))*dW, pT+dH); ctx.lineTo(pL, pT+dH); ctx.fillStyle=gr; ctx.globalAlpha=0.15; ctx.fill(); ctx.restore();";
    p += "if(shId){ var vE=document.getElementById(shId); vE.innerText=val; if(val>0) document.getElementById('dot_'+shId.split('_')[1]).classList.add('status-active'); } }";

    p += "async function updateData(){ try{";
    p += "var [d, f, s]=await Promise.all([fetch('/data').then(r=>r.json()), fetch('/fft').then(r=>r.json()), fetch('/sys').then(r=>r.json())]);";
    p += "document.getElementById('st_bh').innerText='BH1750: '+(d.c2?'ONLINE':'OFFLINE'); document.getElementById('st_bh').style.color=d.c2?'#0f8':'#f44';";
    p += "document.getElementById('st_sgp').innerText='SGP41: '+(d.c3?'ONLINE':'OFFLINE'); document.getElementById('st_sgp').style.color=d.c3?'#0f8':'#f44';";
    p += "document.getElementById('cur_mode').innerText=s.m==1?'Access Point':'Client (STA)';";
    p += "document.getElementById('cur_ip').innerText=s.ip; document.getElementById('cur_ssid').innerText=s.ss;";
    p += "sD.t.push(d.temp); sD.h.push(d.humi); sD.v.push(d.voc); sD.n.push(d.nox); sD.l.push(d.lux); if(sD.t.length>maxLen){['t','h','v','n','l'].forEach(k=>sD[k].shift());}";
    p += "drawFFT(f);";
    p += "drawChart(tCtx,tC,sD.t,'#ff4466',60,'t_t','Temperature','&deg;C',d.temp.toFixed(1),'h_temp');";
    p += "drawChart(hCtx,hC,sD.h,'#44aaff',100,'t_h','Humidity','%',d.humi.toFixed(1),'h_humi');";
    p += "drawChart(vCtx,vC,sD.v,'#00ffaa',500,'t_v','VOC Index','',d.voc,'h_voc');";
    p += "drawChart(nCtx,nC,sD.n,'#ffaa00',500,'t_n','NOx Index','',d.nox,'h_nox');";
    p += "drawChart(lCtx,lC,sD.l,'#ffff00',1000,'t_l','Light Level','lx',d.lux.toFixed(0),'h_lux');";
    p += "} catch(e){ console.error(e); } }";
    p += "setInterval(updateData,1000); updateData();";
    p += "function saveWiFi(){ location.href='/set_wifi?m='+document.getElementById('wm').value+'&s='+document.getElementById('ws').value+'&p='+document.getElementById('wp').value; }";
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

    gData.hasSHT = checkI2CConnection(0x44) || checkI2CConnection(0x45);
    if (gData.hasSHT) {
        uint8_t addr = checkI2CConnection(0x44) ? 0x44 : 0x45;
        sht4x.begin(Wire, addr);
        sht4x.softReset(); // 센서 내부 상태 초기화
        delay(100);
        Serial.printf(" - SHT4x OK (at 0x%02X)\n", addr);
    }

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
 * @brief Wi-Fi 연결 및 AP 모드 설정
 */
void setupNetwork() {
    Serial.println("\n--- WiFi Configuration ---");
    
    // 1. 모드에 따른 초기화
    WiFi.disconnect(true);
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(500); 

    if (wifiMode == 1) { // AP MODE
        Serial.println("Starting in AP Mode...");
        WiFi.mode(WIFI_AP);
        
        // AP IP 설정: 192.168.1.1
        IPAddress local_IP(192, 168, 1, 1);
        IPAddress gateway(192, 168, 1, 1);
        IPAddress subnet(255, 255, 255, 0);
        WiFi.softAPConfig(local_IP, gateway, subnet);
        
        String ssid = (apSSID.length() > 0) ? apSSID : Config::HUB_AP_SSID;
        String pass = (apSSID.length() > 0) ? apPW : Config::HUB_AP_PASS;
        
        if (WiFi.softAP(ssid.c_str(), pass.c_str())) {
            Serial.printf("AP Started: %s\n", ssid.c_str());
            Serial.print("IP Address: "); Serial.println(WiFi.softAPIP());
        } else {
            Serial.println("AP Failed. Falling back to recovery...");
            WiFi.softAP(Config::HUB_AP_SSID, Config::HUB_AP_PASS);
        }
    } else { // STA MODE
        String ssidToTry = (stSSID.length() > 0) ? stSSID : Config::TARGET_SSID;
        String passToTry = (stSSID.length() > 0) ? stPW : Config::TARGET_PASS;

        Serial.printf("Connecting to Network: %s\n", ssidToTry.c_str());
        WiFi.mode(WIFI_STA); 
        WiFi.setAutoReconnect(true);
        WiFi.begin(ssidToTry.c_str(), passToTry.c_str());
        
        unsigned long startTime = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - startTime < Config::WIFI_CONNECT_TIMEOUT_MS) {
            delay(500);
            Serial.print(".");
        }

        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("\n[!] Connection Failed. Switching to Recovery AP...");
            WiFi.mode(WIFI_AP);
            WiFi.softAP(Config::HUB_AP_SSID, Config::HUB_AP_PASS);
            Serial.print("Recovery IP: "); Serial.println(WiFi.softAPIP());
        } else {
            Serial.println("\n[OK] Connected Successfully!");
            Serial.print("Local IP: "); Serial.println(WiFi.localIP());
        }
    }
}

/**
 * @brief 웹 서버 핸들러 및 API 설정
 */
void setupWebServer() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *req){
        AsyncWebServerResponse *response = req->beginResponse(200, "text/html", buildHtmlPage());
        response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
        req->send(response);
    });

    server.on("/data", HTTP_GET, [](AsyncWebServerRequest *req){
        String payload;
        payload.reserve(1024);
        payload = "{\"temp\":" + String(gData.temp, 1) + 
                  ",\"humi\":" + String(gData.humi, 1) + 
                  ",\"lux\":" + String(gData.lux, 1) + 
                  ",\"voc\":" + String(gData.voc) + 
                  ",\"nox\":" + String(gData.nox) + 
                  ",\"sraw_voc\":" + String(gData.srawVoc) + 
                  ",\"sraw_nox\":" + String(gData.srawNox) + 
                  ",\"c2\":" + String(gData.hasBH) + 
                  ",\"c3\":" + String(gData.hasSGP) + 
                  "}";
        
        AsyncWebServerResponse *response = req->beginResponse(200, "application/json", payload);
        response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
        req->send(response);
    });

    server.on("/fft", HTTP_GET, [](AsyncWebServerRequest *req){
        String safeFFT = "[]";
        if (xSemaphoreTake(fftMutex, 50 / portTICK_PERIOD_MS) == pdTRUE) {
            safeFFT = fftJsonData;
            xSemaphoreGive(fftMutex);
        }
        AsyncWebServerResponse *response = req->beginResponse(200, "application/json", safeFFT);
        response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
        req->send(response);
    });

    server.on("/sys", HTTP_GET, [](AsyncWebServerRequest *req){
        String ip = (WiFi.getMode() & WIFI_MODE_STA) ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
        String curSSID = (WiFi.getMode() & WIFI_MODE_STA) ? WiFi.SSID() : apSSID;
        String payload = "{\"m\":" + String(wifiMode) + ",\"ip\":\"" + ip + "\",\"ss\":\"" + curSSID + "\"}";
        req->send(200, "application/json", payload);
    });

    server.on("/set_wifi", HTTP_GET, [](AsyncWebServerRequest *req){
        if (req->hasParam("m")) {
            wifiMode = req->getParam("m")->value().toInt();
            EEPROM.write(11, (uint8_t)wifiMode);
        }
        if (req->hasParam("s")) {
            String s = req->getParam("s")->value();
            if (wifiMode == 1) { apSSID = s; writeEEPROMString(100, s); }
            else { stSSID = s; writeEEPROMString(12, s); }
        }
        if (req->hasParam("p")) {
            String p = req->getParam("p")->value();
            if (wifiMode == 1) { apPW = p; writeEEPROMString(164, p); }
            else { stPW = p; writeEEPROMString(44, p); }
        }
        EEPROM.commit();
        req->send(200, "text/html", "Settings saved. Restarting...");
        delay(1000); ESP.restart();
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
 * @brief FFT 연산 및 JSON 직렬화 (안정화 적용)
 */
void processFFTLogic() {
    // 1. DC Offset 제거 (평균값 차감)
    double sum = 0;
    for (int i = 0; i < Config::FFT_SAMPLES; i++) sum += vReal[i];
    double mean = sum / Config::FFT_SAMPLES;
    for (int i = 0; i < Config::FFT_SAMPLES; i++) vReal[i] -= mean;

    // 2. FFT 수행
    FFT.windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);
    FFT.compute(FFT_FORWARD);
    FFT.complexToMagnitude();

    // 3. 32개 밴드로 압축 및 노이즈 필터링
    static float smoothedBins[32] = {0};
    float alpha = 0.2f; // FFT 평활화 강도 (낮을수록 더 안정적)

    String json = "[";
    int samplesPerBin = Config::FFT_SAMPLES / 2 / 32;
    for (int i = 0; i < 32; i++) {
        double magSum = 0;
        for (int j = 0; j < samplesPerBin; j++) {
            magSum += vReal[i * samplesPerBin + j + 2]; // DC성분 제외하고 합산
        }
        float currentMag = (float)(magSum / samplesPerBin);
        
        // 지수 평활화 적용 (Fluctuation 억제)
        smoothedBins[i] = applyFilter(currentMag, smoothedBins[i], alpha);
        
        // 미세 노이즈 컷오프 (Floor 정돈)
        float finalMag = smoothedBins[i];
        if (finalMag < 5.0f) finalMag = 0.0f; 

        json += "{\"freq\":" + String((int)(i * (Config::SAMPLING_FREQ / 2 / 32))) + ",\"mag\":" + String(finalMag, 1) + "}";
        if (i < 31) json += ",";
    }
    json += "]";
    
    if (xSemaphoreTake(fftMutex, 10 / portTICK_PERIOD_MS) == pdTRUE) {
        fftJsonData = json;
        xSemaphoreGive(fftMutex);
    }
    isBufferFull = false;
}

void updateSensorData() {
    static uint32_t shtTick = 0;
    static uint32_t sgpTick = 0;
    static int consecutiveErrors = 0;

    // 1. SHT4x (온습도)
    if (gData.hasSHT && (millis() - shtTick >= 2000)) {
        shtTick = millis();
        uint16_t error = sht4x.measureHighPrecision(gData.temp, gData.humi);
        if (!error) {
            consecutiveErrors = 0;
        } else {
            consecutiveErrors++;
        }
    }
    
    // 2. SGP41 (공기질)
    if (gData.hasSGP && (millis() - sgpTick >= 1000)) {
        sgpTick = millis();
        float compT = gData.hasSHT ? gData.temp : 25.0;
        float compH = gData.hasSHT ? gData.humi : 50.0;
        
        sgp41.measureRawSignals(compH, compT, gData.srawVoc, gData.srawNox);
        
        if (gData.srawVoc > 0) gData.voc = voc_algorithm.process(gData.srawVoc);
        if (gData.srawNox > 0) gData.nox = nox_algorithm.process(gData.srawNox);
    }
    
    // 3. BH1750 (조도)
    if (gData.hasBH) {
        float rawL = lightMeter.readLightLevel();
        if (rawL >= 0) gData.lux = rawL;
    }

    // 버스 잠김 의심 시 자동 복구 (에러 지속 시)
    if (consecutiveErrors > 20) {
        Serial.println("[System] Bus freeze detected. Recovering...");
        recoverI2CBus();
        consecutiveErrors = 0;
    }

    // 4. (추가) 센서 재연결 시도 (10초마다)
    static uint32_t retryTick = 0;
    if ((!gData.hasSGP || !gData.hasSHT) && (millis() - retryTick >= 10000)) {
        retryTick = millis();
        if (!gData.hasSGP) {
            gData.hasSGP = checkI2CConnection(0x59);
            if(gData.hasSGP) { sgp41.begin(Wire); Serial.println("[sys] SGP41 Reconnected!"); }
        }
        if (!gData.hasSHT) {
            uint8_t addr = checkI2CConnection(0x44) ? 0x44 : (checkI2CConnection(0x45) ? 0x45 : 0);
            if (addr > 0) {
                gData.hasSHT = true;
                sht4x.begin(Wire, addr);
                sht4x.softReset(); 
                Serial.printf("[sys] SHT4x Reconnected (at 0x%02X)!\n", addr); 
            }
        }
    }

    timerSensorFlag = false;
}

/**
 * @brief 개별 센서 정밀 진단 함수들
 */
void testSHT4x() {
    Serial.print("\n[Test] SHT4x (Temp/Humi): ");
    if (!gData.hasSHT) { Serial.println("NOT FOUND (Skipping test to prevent crash)"); return; }
    
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
    
    uint16_t srawVoc = 0, srawNox = 0;
    float currentT = gData.hasSHT ? gData.temp : 25.0;
    float currentRH = gData.hasSHT ? gData.humi : 50.0;

    // Measure Raw Signals with compensation
    uint16_t err = sgp41.measureRawSignals(currentRH, currentT, srawVoc, srawNox);
    // currentT = (gData.temp == 0 && gData.humi == 0) ? 25.0 : gData.temp;
    // currentRH = (gData.temp == 0 && gData.humi == 0) ? 50.0 : gData.humi;

    if (err) {
        Serial.printf("FAILED (Error code: %u)\n", err);
    } else {
        Serial.println("OK!");
        Serial.printf("    > Environment: T=%.1fC, RH=%.1f%%\n", currentT, currentRH);
        Serial.printf("    > SRAW_VOC: %u (Raw Ticks)\n", srawVoc);
        Serial.printf("    > SRAW_NOX: %u (Raw Ticks)\n", srawNox);
        Serial.printf("    > Current Index (Periodic): VOC Algorithm=%u, NOx Algorithm=%u\n", gData.voc, gData.nox);
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
            if (millis() < 60000) {
                Serial.printf("[Info] Sensor Updated %s| Temp: %.1fC, Humi: %.1f%%, Lux: %.1f, VOC Index: %u (Warmup), NOx Index: %u (Warmup)\n", 
                              WiFi.localIP().toString().c_str(), gData.temp, gData.humi, gData.lux, gData.voc, gData.nox);
            } else {
                Serial.printf("[Info] Sensor Updated %s| Temp: %.1fC, Humi: %.1f%%, Lux: %.1f, VOC Index: %u, NOx Index: %u\n", 
                              WiFi.localIP().toString().c_str(), gData.temp, gData.humi, gData.lux, gData.voc, gData.nox);
            }
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

    // Mutex 초기화
    fftMutex = xSemaphoreCreateMutex();

    wifiMode = EEPROM.read(11);
    if (wifiMode > 1) wifiMode = 0; // Default to STA

    stSSID = readEEPROMString(12);
    stPW = readEEPROMString(44);
    apSSID = readEEPROMString(100);
    apPW = readEEPROMString(164);

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