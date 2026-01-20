/**
 * @file Esp32S3_Final_Master_V4.6.ino
 * @author Gemini Thought Partner
 * @version V4.6 (Visual UI Optimization & Labelled Graphs)
 * @date 2025-12-22
 * * [프로그램 설명]
 * - FFT UI: 32개 밴드 막대 폭 고정, X축 주파수(Hz) 눈금 및 라벨링 추가.
 * - 센서 UI: 온도(Red), 습도(Blue), 조도(Yellow), 가스(Green) 범례(Legend) 및 그래프 복원.
 * - WiFi: 5초간 접속 시도 후 실패 시 AP 전환. 저장 시 리셋.
 * - 서보: 저장과 트리거 버튼 분리. 트리거 시 페이지 새로고침 없음.
 */

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <SensirionI2cSht4x.h>
#include <SensirionI2CSgp41.h>
#include <BH1750.h>
#include <arduinoFFT.h>
#include <ESP32Servo.h>
#include <EEPROM.h>

#define SERVO_PIN 2     // D1
#define SOUND_PIN 1     // A0
#define I2C_SDA 5
#define I2C_SCL 6
#define EEPROM_SIZE 512
#define SAMPLES 8192    
#define SAMPLING_FREQ 8000
#define SAMPLING_PERIOD_US 125 

AsyncWebServer server(80);
AsyncEventSource events("/events");
SensirionI2cSht4x sht4x;
SensirionI2CSgp41 sgp41;
BH1750 lightMeter;
Servo myservo;

// 메모리 패닉 방지용 정적 할당
static double vReal[SAMPLES];
static double vImag[SAMPLES];
ArduinoFFT<double> FFT = ArduinoFFT<double>(vReal, vImag, SAMPLES, SAMPLING_FREQ);

float temp=0, humi=0, lux=0;
uint16_t voc=0, nox=0;
bool hasSHT=false, hasSGP=false, hasBH=false;
volatile bool isBufferFull=false, servoTriggered=false;
int servoStart, servoEnd, motionDelay;
String stSSID, stPW, fftJsonData = "[]";

// --- 헬퍼 함수 ---
bool checkI2C(uint8_t a) { Wire.beginTransmission(a); return (Wire.endTransmission()==0); }
void writeStr(int a, String d) { for(int i=0; i<d.length(); i++) EEPROM.write(a+i, d[i]); EEPROM.write(a+d.length(), '\0'); }
String readStr(int a) { String r=""; char k; int i=0; while((k=EEPROM.read(a+i))!='\0' && i<64){ r+=k; i++; } return (r.length()==0 || (uint8_t)r[0]==255)?"":r; }

// --- [V4.6] UI 최적화 대시보드 ---
String buildHtmlPage() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:sans-serif; background:#000; color:#eee; padding:15px;} .card{background:#111; padding:15px; border-radius:10px; margin-bottom:15px; border:1px solid #333;}";
  html += "canvas{background:#050505; width:100%; height:200px; border:1px solid #222;} .legend{font-size:12px; margin-bottom:5px;} .leg-item{display:inline-block; margin-right:10px;}";
  html += ".btn{padding:12px; border:none; border-radius:5px; cursor:pointer; color:#fff; display:block; width:100%; text-align:center; text-decoration:none; margin-top:10px;}";
  html += "input{background:#222; color:#fff; border:1px solid #444; padding:5px; margin:5px; width:120px;}</style></head><body>";
  
  html += "<h2>ESP32-S3 Master V4.6</h2>";
  html += "<div class='card'><b>Status:</b> SHT(<span id='c1'>-</span>) | BH(<span id='c2'>-</span>) | SGP(<span id='c3'>-</span>)</div>";

  // FFT 차트
  html += "<div class='card'><b>FFT Spectrum (8kHz Sample / 1s Analysis)</b><canvas id='fC'></canvas></div>";

  // 센서 트렌드 차트 및 범례
  html += "<div class='card'><b>Sensor Trends</b><div class='legend'>";
  html += "<span class='leg-item' style='color:#f44;'>■ Temp</span> <span class='leg-item' style='color:#44f;'>■ Humi</span> ";
  html += "<span class='leg-item' style='color:#ff0;'>■ Lux</span> <span class='leg-item' style='color:#0f0;'>■ VOC</span></div>";
  html += "<canvas id='sC'></canvas></div>";

  // 설정 영역
  html += "<div class='card'><h3>Configuration</h3>";
  html += "<b>WiFi:</b> <input type='text' id='ws' value='"+stSSID+"' placeholder='SSID'> / <input type='password' id='wp' value='"+stPW+"' placeholder='PW'>";
  html += "<button onclick='saveWiFi()' class='btn' style='background:#007bff;'>Save WiFi & Restart</button><hr>";
  html += "<b>Servo:</b> <input type='number' id='ss' value='"+String(servoStart)+"'> to <input type='number' id='se' value='"+String(servoEnd)+"'> (Delay: <input type='number' id='sd' value='"+String(motionDelay)+"'>ms)";
  html += "<button onclick='saveServo()' class='btn' style='background:#6c757d;'>Save Servo Values</button>";
  html += "<button onclick='triggerServo()' class='btn' style='background:#28a745;'>Manual Trigger (No Reload)</button></div>";

  html += "<script>";
  // 그래프 드로잉 변수
  html += "var fC=document.getElementById('fC'), sC=document.getElementById('sC'); var fCtx=fC.getContext('2d'), sCtx=sC.getContext('2d');";
  html += "var sD = {t:[], h:[], l:[], v:[]};";

  // [V4.6] FFT 드로잉 (X축 주파수 및 고정 폭 막대)
  html += "function drawFFT(d){ fCtx.clearRect(0,0,fC.width,fC.height); fCtx.fillStyle='#28a745'; fCtx.font='10px Arial';";
  html += "var pad=30; var bw=(fC.width-pad-10)/32; for(var i=0;i<32;i++){ var bh=(d[i].mag/5000)*(fC.height-40); fCtx.fillRect(pad+i*bw+1, fC.height-20-bh, bw-2, bh); ";
  html += "if(i%8==0){ fCtx.fillStyle='#888'; fCtx.fillText(d[i].freq+'Hz', pad+i*bw, fC.height-5); fCtx.fillStyle='#28a745'; } } fCtx.strokeStyle='#444'; fCtx.strokeRect(pad,0,fC.width-pad,fC.height-20); }";

  // [V4.6] 센서 트렌드 드로잉 (멀티 라인)
  html += "function drawSens(t,h,l,v){ sCtx.clearRect(0,0,sC.width,sC.height); sD.t.push(t); sD.h.push(h); sD.l.push(l); sD.v.push(v); if(sD.t.length>50) {sD.t.shift(); sD.h.shift(); sD.l.shift(); sD.v.shift();} ";
  html += "function line(data, color, max){ sCtx.beginPath(); sCtx.strokeStyle=color; for(var i=0;i<data.length;i++){ var x=(sC.width/50)*i; var y=sC.height-(data[i]/max)*sC.height; if(i==0) sCtx.moveTo(x,y); else sCtx.lineTo(x,y); } sCtx.stroke(); }";
  html += "line(sD.t, '#f44', 50); line(sD.h, '#44f', 100); line(sD.l, '#ff0', 1000); line(sD.v, '#0f0', 65535); }";

  // 비동기 통신
  html += "function saveWiFi(){ location.href='/set_wifi?ssid='+document.getElementById('ws').value+'&pass='+document.getElementById('wp').value; }";
  html += "function saveServo(){ fetch('/set_servo?s='+document.getElementById('ss').value+'&e='+document.getElementById('se').value+'&d='+document.getElementById('sd').value).then(()=>alert('Saved')); }";
  html += "function triggerServo(){ fetch('/trigger'); }";

  html += "var src=new EventSource('/events'); src.addEventListener('data', function(e){ var d=JSON.parse(e.data);";
  html += "document.getElementById('c1').innerText=d.c1?'OK':'Err'; document.getElementById('c2').innerText=d.c2?'OK':'Err'; document.getElementById('c3').innerText=d.c3?'OK':'Err';";
  html += "drawFFT(d.fft); drawSens(d.temp, d.humi, d.lux, d.voc); }, false);</script></body></html>";
  return html;
}

// --- 태스크 및 루프 ---
void samplingTask(void *p) { while(1) { if(!isBufferFull) { unsigned long nt=micros(); for(int i=0; i<SAMPLES; i++) { vReal[i]=analogRead(SOUND_PIN); vImag[i]=0; while(micros()<nt); nt+=SAMPLING_PERIOD_US; } isBufferFull=true; } vTaskDelay(1); } }
void sensorTask(void *p) { while(1) { if(hasSHT) sht4x.measureHighPrecision(temp, humi); if(hasSGP) sgp41.measureRawSignals(humi, temp, voc, nox); if(hasBH) lux=lightMeter.readLightLevel(); vTaskDelay(100); } }

void setup() {
  Serial.begin(115200); Wire.begin(I2C_SDA, I2C_SCL); EEPROM.begin(EEPROM_SIZE);
  servoStart=EEPROM.readInt(0); servoEnd=EEPROM.readInt(4); motionDelay=EEPROM.readInt(8);
  stSSID=readStr(12); stPW=readStr(44);
  if(servoStart<500){ servoStart=1500; servoEnd=1800; motionDelay=200; }

  myservo.attach(SERVO_PIN, 500, 2500); myservo.writeMicroseconds(servoStart);
  hasBH=checkI2C(0x23); if(hasBH) lightMeter.begin();
  hasSHT=checkI2C(0x44); if(hasSHT) sht4x.begin(Wire, 0x44);
  hasSGP=checkI2C(0x59); if(hasSGP) sgp41.begin(Wire);

  if(stSSID.length()>0) WiFi.begin(stSSID.c_str(), stPW.c_str());
  unsigned long stT=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-stT<5000) { delay(500); Serial.print("."); }
  if(WiFi.status()!=WL_CONNECTED) WiFi.softAP("ESP32_MASTER", "12345678");

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req){ req->send(200, "text/html", buildHtmlPage()); });
  server.on("/set_wifi", HTTP_GET, [](AsyncWebServerRequest *req){
    if(req->hasParam("ssid")) writeStr(12, req->getParam("ssid")->value());
    if(req->hasParam("pass")) writeStr(44, req->getParam("pass")->value());
    EEPROM.commit(); req->send(200, "text/html", "Restarting..."); delay(2000); ESP.restart();
  });
  server.on("/set_servo", HTTP_GET, [](AsyncWebServerRequest *req){
    if(req->hasParam("s")) servoStart = req->getParam("s")->value().toInt();
    if(req->hasParam("e")) servoEnd = req->getParam("e")->value().toInt();
    if(req->hasParam("d")) motionDelay = req->getParam("d")->value().toInt();
    EEPROM.writeInt(0, servoStart); EEPROM.writeInt(4, servoEnd); EEPROM.writeInt(8, motionDelay); EEPROM.commit();
    req->send(200, "text/plain", "OK");
  });
  server.on("/trigger", HTTP_GET, [](AsyncWebServerRequest *req){ servoTriggered=true; req->send(200, "text/plain", "OK"); });
  server.on("/api/all", HTTP_GET, [](AsyncWebServerRequest *req){
    req->send(200, "application/json", "{\"lux\":"+String(lux,1)+",\"fft\":"+fftJsonData+"}");
  });

  server.addHandler(&events); server.begin();
  xTaskCreatePinnedToCore(samplingTask, "Samp", 8192, NULL, 24, NULL, 1);
  xTaskCreatePinnedToCore(sensorTask, "Sens", 4096, NULL, 1, NULL, 1);
}

void loop() {
  if(isBufferFull) {
    FFT.windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD); FFT.compute(FFT_FORWARD); FFT.complexToMagnitude();
    String binD="["; for(int i=0; i<32; i++){ double a=0; for(int j=0; j<128; j++) a+=vReal[i*128+j+2]; binD+="{\"freq\":"+String(i*125)+",\"mag\":"+String(a/128.0,1)+"}"; if(i<31)binD+=","; } binD+="]";
    fftJsonData = binD; isBufferFull=false;
    events.send(String("{\"temp\":"+String(temp,1)+",\"humi\":"+String(humi,1)+",\"lux\":"+String(lux,1)+",\"voc\":"+String(voc)+",\"c1\":"+String(hasSHT)+",\"c2\":"+String(hasBH)+",\"c3\":"+String(hasSGP)+",\"fft\":"+fftJsonData+"}").c_str(), "data", millis());
  }
  if(servoTriggered) { myservo.writeMicroseconds(servoEnd); delay(motionDelay); myservo.writeMicroseconds(servoStart); servoTriggered=false; }
  yield();
}