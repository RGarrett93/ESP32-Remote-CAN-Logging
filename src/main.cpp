#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <sys/time.h>
#include "driver/twai.h"
#include <Update.h> // /ota 

// =========================
//   USER CONFIG — EDIT ME
// =========================
// Set your Wi‑Fi credentials
const char *WIFI_SSID = "YOUR_WIFI_SSID";
const char *WIFI_PASS = "YOUR_WIFI_PASSWORD";

// Set your CAN TX/RX pins for the ESP32 board you use
#ifndef CAN_TX
#define CAN_TX 27
#endif
#ifndef CAN_RX
#define CAN_RX 26
#endif
#ifndef CAN_SPEED_MODE
#define CAN_SPEED_MODE 23
#endif
// RS485 and CAN Boost power supply
#ifndef ME2107_EN
#define ME2107_EN 16 
#endif

#define LOG_TO_SERIAL 1

AsyncWebServer server(80);
AsyncWebSocket wsLog("/log");

// =========================
//        NTP timing
// =========================
static time_t   ntp_sec    = 0;   // last NTP‐synced whole seconds
static uint64_t ntp_micros = 0;   // micros() at that moment

static inline void recordNtpSync(){
  ntp_sec    = time(nullptr);
  ntp_micros = micros();
}

static inline double now_seconds(){
  uint64_t delta_us = micros() - ntp_micros;
  return double(ntp_sec) + double(delta_us) * 1e-6;
}

static void initNTP(){
  configTime(0, 0, "pool.ntp.org");
  Serial.print("Waiting for NTP");
  while (time(nullptr) < 24L*3600L) { delay(500); Serial.print("."); }
  Serial.println();
  recordNtpSync();
  Serial.printf("NTP synced at %s", ctime(&ntp_sec));
}

// =========================
//      WS ring buffer
// =========================
static constexpr size_t WSBUF_SZ      = 32768;
static constexpr size_t WSFLUSH_SLICE = 1024;

struct WsRing {
  char buf[WSBUF_SZ];
  size_t head = 0, tail = 0;
  SemaphoreHandle_t mtx = nullptr;
  inline size_t used() const { return (head + WSBUF_SZ - tail) % WSBUF_SZ; }
  inline size_t free() const { return WSBUF_SZ - 1 - used(); }
  inline void   pushByte(char c){ buf[head] = c; head = (head + 1) % WSBUF_SZ; if (head == tail) tail = (tail + 1) % WSBUF_SZ; }
  inline bool   popByte(char &out){ if (tail == head) return false; out = buf[tail]; tail = (tail + 1) % WSBUF_SZ; return true; }
};

static WsRing ringCan;

static void wsbuf_init(){ ringCan.mtx = xSemaphoreCreateMutex(); }

static void rb_drop_one_line(WsRing &rb){ char c; while (rb.tail != rb.head) { if (rb.popByte(c) && c=='\n') break; } }

static void rb_enqueue_line(WsRing &rb, const char *s){
  if (!s || !*s) return;
  if (xSemaphoreTake(rb.mtx, 0) != pdTRUE) return;
  size_t need = strlen(s) + 1; // +\n
  while (rb.free() < need) rb_drop_one_line(rb);
  while (*s) rb.pushByte(*s++);
  rb.pushByte('\n');
  xSemaphoreGive(rb.mtx);
}

static void ws_flush_ring(AsyncWebSocket &ws, WsRing &rb){
  if (ws.count() == 0) return;
  static uint32_t lastCleanup = 0; uint32_t now = millis();
  if (now - lastCleanup > 250) { ws.cleanupClients(); lastCleanup = now; }

  if (xSemaphoreTake(rb.mtx, 0) != pdTRUE) return;
  const size_t used = rb.used();
  const size_t limit = (used < WSFLUSH_SLICE ? used : (size_t)WSFLUSH_SLICE);
  if (limit == 0) { xSemaphoreGive(rb.mtx); return; }

  if (!ws.availableForWriteAll()) { xSemaphoreGive(rb.mtx); return; }

  // send up to the last newline inside the slice
  size_t idx = rb.tail; ssize_t last_nl = -1;
  for (size_t i = 0; i < limit; ++i) { if (rb.buf[idx] == '\n') last_nl = (ssize_t)i; idx = (idx + 1) % WSBUF_SZ; }
  if (last_nl < 0 && used < (WSBUF_SZ - 64)) { xSemaphoreGive(rb.mtx); return; }
  size_t to_send = (last_nl >= 0) ? ((size_t)last_nl + 1) : limit;

  static char out[WSFLUSH_SLICE + 1];
  for (size_t i = 0; i < to_send; ++i) rb.popByte(out[i]);
  xSemaphoreGive(rb.mtx);

  if (auto *mb = ws.makeBuffer(to_send)) { memcpy(mb->get(), out, to_send); ws.textAll(mb); }
  else { ws.textAll(out, to_send); }
}

static inline void streamCanLog(const char *message){ if (wsLog.count()) rb_enqueue_line(ringCan, message); }

// =========================
//        HTML (inline)
// =========================
static const char ROOT_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8" />
  <title>CAN Log Stream</title> 
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <style>
    :root { --chrome: #333; --bg: #fafafa; --mono: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, "Liberation Mono", "Courier New", monospace; }
    body { margin: 0; background: var(--bg); font-family: system-ui, -apple-system, Segoe UI, Roboto, Ubuntu, Cantarell, Noto Sans, Helvetica, Arial, "Apple Color Emoji", "Segoe UI Emoji"; }
    header { position: sticky; top: 0; z-index: 1; background: #fff; border-bottom: 1px solid #ddd; padding: 8px 12px; display: flex; gap: 8px; align-items: center; flex-wrap: wrap; }
    #status { font-size: 12px; color: #666; margin-left: auto; }
    #log { height: calc(100vh - 60px); overflow: auto; padding: 12px; font: 13px/1.4 var(--mono); background: #fff; white-space: pre; border-left: 6px solid #e5e5e5; }
    button { padding: 6px 10px; border-radius: 6px; border: 1px solid #ccc; background: #f5f5f5; cursor: pointer; }
    button:hover { background: #eee; }
    #scrollBtn { position: fixed; bottom: 16px; left: 50%; transform: translateX(-50%); background: rgba(0,0,0,.6); color: #fff; border: none; border-radius: 20px; cursor: pointer; padding: 6px 12px; display: none; }
    #scrollBtn:hover { background: rgba(0,0,0,.8); }
    .pill { padding: 2px 8px; border-radius: 999px; font-size: 12px; color: #fff; background: #999; }
    .up { background: #2e7d32; }
    .down { background: #c62828; }
    .warn { background: #ef6c00; }
  </style>
</head>
<body>
  <header>
    <button onclick="location.href='/'">Home</button>
    <button id="pauseBtn" onclick="togglePause()">Pause</button>
    <button onclick="clearLog()">Clear</button>
    <button onclick="downloadLog()">Download</button>
    <span id="status">Connecting…</span>
  </header>

  <div id="log" aria-label="CAN log output"></div>
  <button id="scrollBtn" onclick="scrollToBottom()">Follow ▼</button>

  <script>
    const endpoint = '/log';
    const logDiv = document.getElementById('log');
    const statusEl = document.getElementById('status');
    const scrollBtn = document.getElementById('scrollBtn');
    const pauseBtn = document.getElementById('pauseBtn');

    let ws, reconnects = 0, manualClose = false;
    let paused = false;
    let shouldAutoScroll = true;

    // client-side buffer of last N lines
    const lines = [];
    const MAX_LINES = 15000;  // keep last 15k lines in memory

    // track scroll state to show floating button
    logDiv.addEventListener('scroll', () => {
      const atBottom = logDiv.scrollTop + logDiv.clientHeight >= logDiv.scrollHeight - 2;
      shouldAutoScroll = atBottom && !paused;
      scrollBtn.style.display = shouldAutoScroll ? 'none' : 'block';
    });

    function setStatus(text, cls) {
      statusEl.textContent = text;
      statusEl.className = 'pill ' + (cls || '');
    }

    function connect() {
      const url = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + endpoint;
      ws = new WebSocket(url);
      setStatus('Connecting…', 'warn');

      ws.onopen = () => {
        setStatus('Connected', 'up');
        reconnects = 0;
      };

      ws.onmessage = (ev) => {
        if (paused) return; // drop while paused to protect ESP
        // Some servers batch multiple lines; normalize to \n splitting
        const chunk = String(ev.data);
        if (!chunk) return;
        const newLines = chunk.replace(/\r\n?/g, '\n').split('\n');
        if (newLines.length && newLines[newLines.length - 1] === '') newLines.pop();
        if (!newLines.length) return;

        for (let i = 0; i < newLines.length; i++) lines.push(newLines[i]);
        if (lines.length > MAX_LINES) {
          const drop = lines.length - MAX_LINES;
          lines.splice(0, drop);
          logDiv.textContent = lines.join('\n') + '\n';
        } else {
          const toAppend = newLines.join('\n') + '\n';
          logDiv.append(document.createTextNode(toAppend));
        }

        if (shouldAutoScroll) logDiv.scrollTop = logDiv.scrollHeight;
      };

      ws.onclose = () => {
        setStatus('Disconnected', 'down');
        if (!manualClose) scheduleReconnect();
      };

      ws.onerror = () => {
        setStatus('Connection error', 'down');
      };
    }

    function scheduleReconnect() {
      const delay = Math.min(30000, 500 * Math.pow(2, reconnects++));
      setStatus('Reconnecting in ' + delay + 'ms…', 'warn');
      setTimeout(() => { if (!manualClose) connect(); }, delay);
    }

    function togglePause(){
      paused = !paused;
      pauseBtn.textContent = paused ? 'Resume' : 'Pause';
      if (!paused && shouldAutoScroll) scrollToBottom();
    }

    function scrollToBottom(){
      logDiv.scrollTop = logDiv.scrollHeight;
      shouldAutoScroll = true;
      scrollBtn.style.display = 'none';
    }

    function clearLog(){
      lines.length = 0;
      logDiv.textContent = '';
    }

    function downloadLog(){
      const blob = new Blob([lines.join('\n') + '\n'], { type: 'text/plain' });
      const a = document.createElement('a');
      a.href = URL.createObjectURL(blob);
      a.download = 'can_log.log';
      a.click();
      URL.revokeObjectURL(a.href);
    }

    window.togglePause = togglePause;
    window.scrollToBottom = scrollToBottom;
    window.clearLog = clearLog;
    window.downloadLog = downloadLog;

    connect();
  </script>
</body>
</html>)HTML";

// =========================
//        CAN (TWAI)
// =========================
static bool twai_ok = false;

static void CAN_Init(){
  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)CAN_TX, (gpio_num_t)CAN_RX, TWAI_MODE_NORMAL);
  g.rx_queue_len = 64; g.tx_queue_len = 8; g.intr_flags = 0;
  twai_timing_config_t t = TWAI_TIMING_CONFIG_1MBITS();
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  esp_err_t err = twai_driver_install(&g, &t, &f);
  if (err != ESP_OK) { Serial.printf("TWAI install failed: %s\n", esp_err_to_name(err)); twai_ok = false; return; }
  err = twai_start();
  if (err != ESP_OK) { Serial.printf("TWAI start failed: %s\n", esp_err_to_name(err)); twai_driver_uninstall(); twai_ok = false; return; }

  uint32_t alerts = TWAI_ALERT_RX_DATA | TWAI_ALERT_RX_QUEUE_FULL | TWAI_ALERT_RX_FIFO_OVERRUN | TWAI_ALERT_ERR_PASS | TWAI_ALERT_BUS_ERROR | TWAI_ALERT_ARB_LOST | TWAI_ALERT_TX_FAILED | TWAI_ALERT_TX_SUCCESS;
  twai_reconfigure_alerts(alerts, NULL);

  twai_ok = true;
  Serial.println("TWAI CAN initialized (1Mbps)");
}

// =========================
//      CAN RX task
// =========================
static void canRxTask(void *arg){
  twai_message_t rx;
  for(;;){
    if (!twai_ok) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
    esp_err_t r = twai_receive(&rx, pdMS_TO_TICKS(100));
    if (r == ESP_OK){
      // Build EXACT candump-style line requested
      char logBuffer[128];
      double ts = now_seconds();
      int len = snprintf(logBuffer, sizeof(logBuffer), "(%012.6f) can0 %08lX#", ts, rx.identifier & 0x1FFFFFFF);
      for (uint8_t i = 0; i < rx.data_length_code; i++) len += snprintf(logBuffer + len, sizeof(logBuffer) - len, "%02X", rx.data[i]);
      streamCanLog(logBuffer);
      if (LOG_TO_SERIAL) Serial.println(logBuffer);
    }
  }
}

// =========================
//           Setup
// =========================
void setup(){
  Serial.begin(115200);
  delay(200);

  wsbuf_init();

  //  Hardware init
  pinMode(ME2107_EN, OUTPUT); digitalWrite(ME2107_EN, HIGH);
  pinMode(CAN_SPEED_MODE, OUTPUT); digitalWrite(CAN_SPEED_MODE, LOW);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Connecting to Wi‑Fi '%s'", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED){ delay(500); Serial.print('.'); }
  Serial.printf("\nWi‑Fi connected: %s\n", WiFi.localIP().toString().c_str());

  initNTP();

  // WebSocket handler (minimal logging)
  wsLog.onEvent([](AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len){
    switch(type){
      case WS_EVT_CONNECT:  Serial.printf("/log: client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str()); break;
      case WS_EVT_DISCONNECT: Serial.printf("/log: client #%u disconnect\n", client->id()); break;
      default: break;
    }
  });
  server.addHandler(&wsLog);

  // Root page (inline HTML, no SPIFFS)
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req){ req->send_P(200, "text/html", ROOT_HTML); });

  // === OTA Manual Update: form page (GET) ===
  server.on("/ota", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html",
      "<form method='POST' action='/ota' enctype='multipart/form-data'>"
      "<input type='file' name='update'><br><br>"
      "<input type='submit' value='OTA Update'>"
      "</form>");
  });

  // === OTA Upload endpoint (POST) ===
  server.on("/ota", HTTP_POST,
    // Request done: send result and reboot
    [](AsyncWebServerRequest *request) {
      String status = Update.hasError() ? "OTA Update Failed"
                                        : "OTA Update Success. Rebooting...";
      AsyncWebServerResponse *resp =
          request->beginResponse(200, "text/plain", status);
      resp->addHeader("Connection", "close");
      request->send(resp);

      // Give the TCP stack a moment to flush the response, then reboot
      // (This mirrors your delay+restart; async is fine here after send().)
      delay(1000);
      ESP.restart();
    },
    // Upload handler: called repeatedly with chunks
    [](AsyncWebServerRequest *request, String filename, size_t index,
       uint8_t *data, size_t len, bool final) {
      if (index == 0) {
        Serial.printf("OTA Update: %s\n", filename.c_str());
        // Start with max available size (same as your UPDATE_SIZE_UNKNOWN)
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
          Update.printError(Serial);
        }
      }

      // Write this chunk
      if (len) {
        if (Update.write(data, len) != len) {
          Update.printError(Serial);
        }
      }

      // Finalize
      if (final) {
        if (Update.end(true)) {
          Serial.printf("OTA Update Success: %u bytes\n", (unsigned)(index + len));
        } else {
          Update.printError(Serial);
        }
      }
    }
  );

  server.begin();
  Serial.println("HTTP/WebSocket server started");

  CAN_Init();
  xTaskCreatePinnedToCore(canRxTask, "canRx", 4096, nullptr, 8, nullptr, 0);
}

// =========================
//            Loop
// =========================
void loop(){
  static uint32_t lastFlush = 0, lastPing = 0;
  uint32_t now = millis();
  if (now - lastFlush >= 50){ lastFlush = now; ws_flush_ring(wsLog, ringCan); }
  if (now - lastPing  >= 15000){ lastPing = now; wsLog.pingAll(); wsLog.cleanupClients(); }
}
