/*
 * XIAO ESP32S3 Sense - Keychain Camera (FINAL, moon-themed UI)
 *   TAP button      -> wake, snap photo saved as YYYY-MM-DD_HH-MM-SS.jpg, sleep
 *   HOLD button 3s  -> WiFi mode, moon-themed photo gallery at 192.168.4.1
 *   In WiFi mode: TAP button to exit, OR it auto-exits after 2 min idle
 *
 * Gallery: tap photos to select, then Save (to Photos) or Delete.
 *
 * Button: D1 (GPIO2) + GND   |   Battery: red->BAT+  black->BAT-
 * Board: XIAO_ESP32S3 / ESP32S3 Dev Module
 * PSRAM: OPI PSRAM | Partition: Maximum APP | USB CDC On Boot: Enabled
 *
 * WiFi network: "Runas Cam"   password: "iloveryan123"   (page titled "Runa's Cam")
 */

#include "esp_camera.h"
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include <WiFi.h>
#include <WebServer.h>
#include "driver/rtc_io.h"
#include "time.h"
#include "sys/time.h"

// ---------- Camera pins ----------
#define PWDN_GPIO_NUM   -1
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM   10
#define SIOD_GPIO_NUM   40
#define SIOC_GPIO_NUM   39
#define Y9_GPIO_NUM     48
#define Y8_GPIO_NUM     11
#define Y7_GPIO_NUM     12
#define Y6_GPIO_NUM     14
#define Y5_GPIO_NUM     16
#define Y4_GPIO_NUM     18
#define Y3_GPIO_NUM     17
#define Y2_GPIO_NUM     15
#define VSYNC_GPIO_NUM  38
#define HREF_GPIO_NUM   47
#define PCLK_GPIO_NUM   13

// ---------- Config ----------
#define BUTTON_PIN       GPIO_NUM_2      // D1
#define SD_CS_PIN        21
#define WIFI_HOLD_MS     3000
#define WIFI_TIMEOUT_MS  120000UL        // 2 min idle -> auto-exit WiFi
#define WIFI_ARM_MS      2000
#define AP_SSID          "Runas Cam"
#define AP_PASS          "iloveryan123"

RTC_DATA_ATTR bool timeInitialized = false;

WebServer server(80);
unsigned long lastActivity = 0;

// ---------------- Clock ----------------
void setClockFromCompileTime() {
  char s_month[5];
  int day, year, hour, minute, sec;
  static const char month_names[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  sscanf(__DATE__, "%s %d %d", s_month, &day, &year);
  sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &sec);
  int month = (strstr(month_names, s_month) - month_names) / 3;
  struct tm t = {};
  t.tm_year = year - 1900; t.tm_mon = month; t.tm_mday = day;
  t.tm_hour = hour; t.tm_min = minute; t.tm_sec = sec; t.tm_isdst = 0;
  time_t tt = mktime(&t);
  struct timeval now = { .tv_sec = tt, .tv_usec = 0 };
  settimeofday(&now, NULL);
}

// ---------------- Camera ----------------
bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM; config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk  = XCLK_GPIO_NUM;  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn  = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode    = CAMERA_GRAB_LATEST;
  if (psramFound()) {
    config.frame_size = FRAMESIZE_QXGA; config.jpeg_quality = 10;
    config.fb_count = 2; config.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    Serial.println("PSRAM missing! Enable 'OPI PSRAM'.");
    config.frame_size = FRAMESIZE_SVGA; config.jpeg_quality = 12;
    config.fb_count = 1; config.fb_location = CAMERA_FB_IN_DRAM;
  }
  if (esp_camera_init(&config) != ESP_OK) { Serial.println("Camera init failed"); return false; }
  sensor_t *s = esp_camera_sensor_get();
  if (s->id.PID == OV3660_PID) { s->set_vflip(s, 1); s->set_brightness(s, 1); s->set_saturation(s, -2); }
  return true;
}

// ---------------- SD ----------------
bool initSD() {
  SPI.begin(7, 8, 9, SD_CS_PIN);
  for (int i = 0; i < 5; i++) {
    if (SD.begin(SD_CS_PIN, SPI, 1000000) && SD.cardType() != CARD_NONE) {
      Serial.printf("SD OK, %lluMB\n", SD.cardSize() / (1024 * 1024));
      return true;
    }
    Serial.printf("SD attempt %d failed\n", i + 1);
    delay(500);
  }
  return false;
}

void takePhoto() {
  for (int i = 0; i < 2; i++) { camera_fb_t *w = esp_camera_fb_get(); if (w) esp_camera_fb_return(w); delay(100); }
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) { Serial.println("Capture failed"); return; }
  time_t now = time(NULL); struct tm ti; localtime_r(&now, &ti);
  char path[48];
  strftime(path, sizeof(path), "/%Y-%m-%d_%H-%M-%S.jpg", &ti);
  if (SD.exists(path)) {
    char base[48]; strftime(base, sizeof(base), "/%Y-%m-%d_%H-%M-%S", &ti);
    for (int i = 1; i < 100; i++) { snprintf(path, sizeof(path), "%s_%d.jpg", base, i); if (!SD.exists(path)) break; }
  }
  File file = SD.open(path, FILE_WRITE);
  if (file) { file.write(fb->buf, fb->len); file.close(); Serial.printf("Saved %s (%u bytes)\n", path, fb->len); }
  else { Serial.println("File write failed"); }
  esp_camera_fb_return(fb);
}

// ---------------- Web Pages & CSS ----------------
const char CSS_DATA[] = R"LUNA(
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html,body{height:100%;height:100dvh;margin:0;padding:0;overflow:hidden;background-color:#080b1f}
body{color:#eef1ff;display:flex;flex-direction:column;
font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
background:linear-gradient(180deg,#080b1f 0%,#0f1436 55%,#171d47 100%)}
.sky,.sp1,.sp2{position:fixed;inset:-25%;z-index:0;pointer-events:none}
.sky{background:
radial-gradient(1.5px 1.5px at 8% 14%,#fff,transparent),radial-gradient(1.5px 1.5px at 20% 30%,#dfe6ff,transparent),
radial-gradient(1.5px 1.5px at 33% 10%,#fff,transparent),radial-gradient(1.5px 1.5px at 46% 24%,#cdd6ff,transparent),
radial-gradient(1.5px 1.5px at 58% 12%,#fff,transparent),radial-gradient(1.5px 1.5px at 71% 28%,#fff,transparent),
radial-gradient(1.5px 1.5px at 84% 9%,#dfe6ff,transparent),radial-gradient(1.5px 1.5px at 93% 33%,#cdd6ff,transparent),
radial-gradient(1.5px 1.5px at 12% 52%,#fff,transparent),radial-gradient(1.5px 1.5px at 29% 64%,#fff,transparent),
radial-gradient(1.5px 1.5px at 43% 55%,#dfe6ff,transparent),radial-gradient(1.5px 1.5px at 61% 69%,#cdd6ff,transparent),
radial-gradient(1.5px 1.5px at 77% 60%,#fff,transparent),radial-gradient(1.5px 1.5px at 89% 74%,#fff,transparent),
radial-gradient(1.5px 1.5px at 18% 86%,#fff,transparent),radial-gradient(1.5px 1.5px at 39% 93%,#cdd6ff,transparent),
radial-gradient(1.5px 1.5px at 63% 89%,#fff,transparent),radial-gradient(1.5px 1.5px at 85% 95%,#dfe6ff,transparent);
animation:driftA 35s linear infinite,tw 4s ease-in-out infinite}
.sp1{background:
radial-gradient(2px 2px at 25% 20%,#fff,transparent),radial-gradient(2px 2px at 68% 40%,#fff,transparent),
radial-gradient(2px 2px at 50% 78%,#fff,transparent);animation:driftB 25s linear infinite,spk 2.2s ease-in-out infinite}
.sp2{background:
radial-gradient(2.5px 2.5px at 82% 22%,#fff,transparent),radial-gradient(2.5px 2.5px at 15% 70%,#fff,transparent),
radial-gradient(3px 3px at 90% 62%,#fff,transparent);animation:driftC 40s linear infinite,spk 3.5s ease-in-out infinite;animation-delay:1s}
@keyframes driftA{0%{transform:translate3d(0,0,0)}50%{transform:translate3d(-35px,45px,0)}100%{transform:translate3d(0,0,0)}}
@keyframes driftB{0%{transform:translate3d(0,0,0)}50%{transform:translate3d(45px,-30px,0)}100%{transform:translate3d(0,0,0)}}
@keyframes driftC{0%{transform:translate3d(0,0,0)}50%{transform:translate3d(-25px,-50px,0)}100%{transform:translate3d(0,0,0)}}
@keyframes tw{0%,100%{opacity:.5}50%{opacity:1;filter:drop-shadow(0 0 8px #fff)}}
@keyframes spk{0%,100%{opacity:.2}50%{opacity:1;filter:drop-shadow(0 0 14px #fff)}}
header{flex:0 0 auto;position:relative;z-index:1;display:flex;flex-direction:column;align-items:center;gap:10px;padding:22px 18px 14px;text-align:center}
.moon{width:56px;height:56px;border-radius:50%;position:relative;
background:radial-gradient(circle at 36% 34%,#fffef7,#efe9d2 68%,#d8d1b4);
box-shadow:0 0 26px 6px rgba(240,238,205,.5),inset -6px -6px 12px rgba(0,0,0,.08)}
.moon i{position:absolute;border-radius:50%;background:rgba(0,0,0,.05)}
.moon i:nth-child(1){width:11px;height:11px;top:14px;left:30px}
.moon i:nth-child(2){width:8px;height:8px;top:33px;left:16px}
.moon i:nth-child(3){width:5px;height:5px;top:10px;left:14px}
h1{margin:0;font-family:"Noteworthy","Marker Felt","Chalkboard SE","Comic Sans MS",cursive;
font-size:30px;font-weight:700;letter-spacing:.5px;text-shadow:0 0 18px rgba(170,184,255,.4)}
#grid{flex:1 1 auto;overflow-y:auto;overscroll-behavior:contain;-webkit-overflow-scrolling:touch;
position:relative;z-index:1;display:grid;grid-template-columns:repeat(auto-fill,minmax(104px,1fr));
gap:10px;padding:6px 14px 16px;align-content:start}
.card{position:relative;aspect-ratio:1;border-radius:14px;overflow:hidden;cursor:pointer;background:#11162f;
border:1px solid rgba(255,255,255,.12);transition:transform .12s,box-shadow .15s}
.card img{width:100%;height:100%;object-fit:cover;display:block}
.card:active{transform:scale(.97)}
.card.sel{border-color:#aab8ff;box-shadow:0 0 0 2px #aab8ff,0 0 16px rgba(160,180,255,.55)}
.card.sel::after{content:"";position:absolute;inset:0;background:rgba(130,150,255,.20)}
.check{position:absolute;top:7px;left:7px;width:22px;height:22px;border-radius:50%;z-index:2;
background:rgba(10,14,35,.55);border:1.5px solid rgba(255,255,255,.7);display:flex;align-items:center;justify-content:center;font-size:13px;color:transparent}
.card.sel .check{background:#8aa0ff;border-color:#fff;color:#0a0e23}
.exp{position:absolute;bottom:6px;right:6px;width:24px;height:24px;border-radius:8px;z-index:2;
background:rgba(10,14,35,.55);display:flex;align-items:center;justify-content:center;font-size:13px;color:#dfe4ff;text-decoration:none}
.empty{grid-column:1/-1;text-align:center;color:#9aa3d4;padding:60px 12px;font-size:15px;line-height:1.6}
.bar{flex:0 0 auto;position:relative;z-index:2;display:flex;align-items:center;gap:8px;
padding:12px 14px calc(12px + env(safe-area-inset-bottom));
background:rgba(10,13,34,.82);backdrop-filter:blur(12px);-webkit-backdrop-filter:blur(12px);border-top:1px solid rgba(255,255,255,.1)}
.bar .cnt{flex:1;font-size:13.5px;color:#c3caf0;text-align:center}
button{font-family:inherit;font-size:14px;font-weight:600;border:none;border-radius:11px;padding:11px 14px;cursor:pointer;color:#fff}
.sa{background:rgba(255,255,255,.1);color:#dfe4ff}
.dl{background:linear-gradient(135deg,#6d7cff,#9aa8ff);color:#0a0e23}
.del{background:rgba(255,90,110,.16);color:#ff9fb0}
button:disabled{opacity:.4;cursor:default}
body.spg{overflow-y:auto;display:block}
.swrap{position:relative;z-index:1;display:flex;flex-direction:column;align-items:center;gap:24px;max-width:500px;margin:0 auto;padding:40px 20px;text-align:center}
.swrap img{width:100%;border-radius:12px;box-shadow:0 8px 24px rgba(0,0,0,0.6)}
.swrap p{font-size:16px;text-shadow:0 2px 4px rgba(0,0,0,0.8);margin:0;font-weight:600}
)LUNA";

const char PAGE_A[] = R"LUNA(<!DOCTYPE html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#080b1f"><title>Runa's Cam</title>
<link rel="stylesheet" href="/style.css">
</head><body>
<div class="sky"></div><div class="sp1"></div><div class="sp2"></div>
<header><div class="moon"><i></i><i></i><i></i></div><h1>Runa's Cam</h1></header>
<div id="grid"></div>
<div class="bar">
<button class="sa" onclick="selectAll()">Select all</button>
<span class="cnt" id="cnt">0 selected</span>
<button class="dl" id="dlb" onclick="saveSel()" disabled>Save</button>
<button class="del" id="delb" onclick="deleteSel()" disabled>Delete</button>
</div>
<script>
const FILES = )LUNA";

const char PAGE_B[] = R"LUNA(;
let sel=new Set();
const grid=document.getElementById('grid');
function build(){
  grid.innerHTML='';
  if(!FILES.length){grid.innerHTML='<p class="empty">No photos yet &#127769;<br>Tap the camera button to capture one.</p>';updateBar();return;}
  FILES.forEach(f=>{
    const c=document.createElement('div');c.className='card';c.dataset.n=f.n;
    c.onclick=()=>tog(f.n,c);
    const im=document.createElement('img');im.loading='lazy';im.src='/img?f='+encodeURIComponent(f.n);
    const ck=document.createElement('div');ck.className='check';ck.textContent='\u2713';
    const ex=document.createElement('a');ex.className='exp';ex.textContent='\u2922';
    ex.href='/view?f='+encodeURIComponent(f.n);ex.target='_blank';ex.onclick=(e)=>e.stopPropagation();
    c.appendChild(im);c.appendChild(ck);c.appendChild(ex);grid.appendChild(c);
  });
  updateBar();
}
function tog(n,c){ if(sel.has(n)){sel.delete(n);c.classList.remove('sel');}else{sel.add(n);c.classList.add('sel');} updateBar(); }
function selectAll(){
  const all=sel.size!==FILES.length; sel.clear();
  document.querySelectorAll('.card').forEach(c=>{ if(all){sel.add(c.dataset.n);c.classList.add('sel');}else{c.classList.remove('sel');} });
  updateBar();
}
function updateBar(){
  document.getElementById('cnt').textContent=sel.size+' selected';
  document.getElementById('dlb').disabled=sel.size===0;
  document.getElementById('delb').disabled=sel.size===0;
}
function saveSel(){
  if(!sel.size)return;
  localStorage.setItem('sf', JSON.stringify(Array.from(sel)));
  window.open('/save', '_blank');
}
async function deleteSel(){
  if(!sel.size)return;
  for(const n of Array.from(sel)){
    try{
      await fetch('/delete?f='+encodeURIComponent(n));
      const i=FILES.findIndex(x=>x.n===n);if(i>=0)FILES.splice(i,1);
      document.querySelectorAll('.card').forEach(c=>{ if(c.dataset.n===n)c.remove(); });
      sel.delete(n);
    }catch(e){}
  }
  if(!FILES.length)build();
  updateBar();
}
build();
</script></body></html>)LUNA";

const char PAGE_SAVE[] = R"LUNA(<!DOCTYPE html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#080b1f"><title>Save Photos</title>
<link rel="stylesheet" href="/style.css">
</head><body class="spg">
<div class="sky"></div><div class="sp1"></div><div class="sp2"></div>
<div class="swrap" id="w">
  <p>Long-press a photo, then tap "Save to Photos" &#127769;</p>
</div>
<script>
const f = JSON.parse(localStorage.getItem('sf')||'[]');
const w = document.getElementById('w');
f.forEach(n => {
  const im = document.createElement('img');
  im.src = '/img?f=' + encodeURIComponent(n);
  w.appendChild(im);
});
</script></body></html>)LUNA";

const char PAGE_VIEW[] = R"LUNA(<!DOCTYPE html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#080b1f"><title>View Photo</title>
<link rel="stylesheet" href="/style.css">
</head><body class="spg">
<div class="sky"></div><div class="sp1"></div><div class="sp2"></div>
<div class="swrap">
  <p>Long-press to save &#127769;</p>
  <script>
    const p=new URLSearchParams(window.location.search);
    const f=p.get('f');
    if(f){
      const i=document.createElement('img');
      i.src='/img?f='+encodeURIComponent(f);
      document.currentScript.parentNode.appendChild(i);
    }
  </script>
</div></body></html>)LUNA";

String jsonFileList() {
  String arr = "[";
  bool first = true;
  File root = SD.open("/");
  if (root) {
    File f = root.openNextFile();
    while (f) {
      String name = String(f.name());
      if (name.endsWith(".jpg") && !name.startsWith("/.")) {
        String clean = name;
        if (clean.startsWith("/")) clean = clean.substring(1);
        if (!first) arr += ",";
        arr += "{\"n\":\"" + clean + "\",\"s\":" + String(f.size() / 1024) + "}";
        first = false;
      }
      f = root.openNextFile();
    }
    root.close();
  }
  arr += "]";
  return arr;
}

void handleRoot() {
  lastActivity = millis();
  String page = String(PAGE_A) + jsonFileList() + String(PAGE_B);
  server.send(200, "text/html", page);
}

void handleImg() {
  lastActivity = millis();
  if (!server.hasArg("f")) { server.send(400, "text/plain", "missing f"); return; }
  File file = SD.open("/" + server.arg("f"));
  if (!file) { server.send(404, "text/plain", "not found"); return; }
  server.streamFile(file, "image/jpeg");
  file.close();
}

void handleDelete() {
  lastActivity = millis();
  if (!server.hasArg("f")) { server.send(400, "text/plain", "missing f"); return; }
  String p = "/" + server.arg("f");
  if (SD.remove(p)) server.send(200, "text/plain", "ok");
  else server.send(500, "text/plain", "fail");
}

void startTransferMode() {
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("WiFi mode. Join '" AP_SSID "' then open http://");
  Serial.println(WiFi.softAPIP());
  server.on("/", handleRoot);
  server.on("/style.css", []() {
    lastActivity = millis();
    server.send(200, "text/css", CSS_DATA);
  });
  server.on("/save", []() {
    lastActivity = millis();
    server.send(200, "text/html", PAGE_SAVE);
  });
  server.on("/view", []() {
    lastActivity = millis();
    server.send(200, "text/html", PAGE_VIEW);
  });
  server.on("/img", handleImg);
  server.on("/delete", handleDelete);
  server.begin();
  unsigned long modeStart = millis();
  lastActivity = modeStart;
  while (true) {
    server.handleClient();
    if (millis() - modeStart > WIFI_ARM_MS && digitalRead(BUTTON_PIN) == LOW) { Serial.println("Button exit."); break; }
    if (millis() - lastActivity > WIFI_TIMEOUT_MS) { Serial.println("Idle timeout."); break; }
    delay(2);
  }
  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

// ---------------- Sleep ----------------
void goToSleep() {
  Serial.println("Sleeping. Tap button to wake.");
  Serial.flush();
  while (digitalRead(BUTTON_PIN) == LOW) delay(10);
  delay(50);
  rtc_gpio_pullup_en(BUTTON_PIN);
  rtc_gpio_pulldown_dis(BUTTON_PIN);
  esp_sleep_enable_ext0_wakeup(BUTTON_PIN, 0);
  esp_deep_sleep_start();
}

// ---------------- Main ----------------
void setup() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  bool wokeFromButton = (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0);
  bool longHold = false;
  if (digitalRead(BUTTON_PIN) == LOW) {
    unsigned long start = millis();
    while (digitalRead(BUTTON_PIN) == LOW) {
      if (millis() - start > WIFI_HOLD_MS) { longHold = true; break; }
      delay(10);
    }
  }
  Serial.begin(115200);
  delay(300);
  if (!timeInitialized) { setClockFromCompileTime(); timeInitialized = true; }
  if (!initSD()) goToSleep();
  if (longHold) { startTransferMode(); }
  else if (wokeFromButton) { if (initCamera()) takePhoto(); }
  goToSleep();
}

void loop() {}