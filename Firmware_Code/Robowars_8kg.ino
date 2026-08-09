/*
 * ================================================================
 *  BATTLE BOT CONTROLLER v7.2
 *  ESP32 WROOM -- NO EXTERNAL LIBRARIES FOR IBUS
 * ================================================================
 *
 *  v7.2 CHANGES:
 *    - ESC arm is hardware arming sequence only (3s 1000us at boot)
 *    - "Armed" on dashboard = CH5 arm lock (drive + weapon gate)
 *    - CH7 (3-pos) is weapon lock/limit:
 *        Pos1 = LOCKED (weapon always 1000us, no spin)
 *        Pos2 = UNLOCKED, limit 3/6 (50%)
 *        Pos3 = UNLOCKED, limit 5/6 (83%)
 *    - CH8 override still goes to 100% (bypasses limit, not lock)
 *    - CH8 override ONLY works when CH7 is Pos2 or Pos3 (unlocked)
 *    - ESC Armed display now shows ESC hardware arm status only
 *    - Weapon Lock display shows CH7 lock state
 *    - Poll 100ms, PollLog 250ms
 *
 *  WEAPON LOGIC:
 *    CH5 OFF  -> all stopped (drive + weapon)
 *    CH5 ON + CH7 Pos1 -> drive works, weapon LOCKED at 1000us
 *    CH5 ON + CH7 Pos2 -> drive + weapon at knob * 50%
 *    CH5 ON + CH7 Pos3 -> drive + weapon at knob * 83%
 *    CH5 ON + CH7 Pos2/3 + CH8 ON -> weapon 100% override
 *
 *  EEPROM LAYOUT (v7.1 compatible):
 *    Byte 0:     Magic (0xBC)
 *    Byte 1-16:  4x float trims
 *    Byte 17-32: maxT, maxS, curveT, curveS, deadT, deadS
 *
 * ================================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <EEPROM.h>
#include <esp_task_wdt.h>

// ================================================================
//  WIFI CREDENTIALS -- EDIT THESE
// ================================================================

const char* WIFI_SSID     = "AndroidAP_6259";
const char* WIFI_PASSWORD = "11221122";

// ================================================================
//  DEBUG
// ================================================================

#define DEBUG_MODE false
#define RAMP_TRACE true

// ================================================================
//  WEB OTA SETTINGS
// ================================================================

const char* OTA_USERNAME  = "admin";
const char* OTA_PASSWORD  = "ota_secret_99";

// ================================================================
//  WIFI SETTINGS
// ================================================================

const unsigned long WIFI_CONNECT_TIMEOUT_MS = 10000;
const unsigned long WIFI_ACTIVE_TIMEOUT_MS  = 120000;

#define TELNET_PORT 23

// ================================================================
//  PINS
// ================================================================

#define IBUS_RX_PIN     16

#define LEFT_RPWM_PIN   25
#define LEFT_LPWM_PIN   26
#define RIGHT_RPWM_PIN  27
#define RIGHT_LPWM_PIN  13

#define ESC_PIN         33

// ================================================================
//  LEDC CHANNELS
// ================================================================

#define LEDC_CH_L_FWD   0
#define LEDC_CH_L_REV   1
#define LEDC_CH_R_FWD   2
#define LEDC_CH_R_REV   3
#define LEDC_CH_ESC     4

// ================================================================
//  IBUS
// ================================================================

#define IBUS_CH_STEERING    0
#define IBUS_CH_THROTTLE    1
#define IBUS_CH_ARM         4
#define IBUS_CH_RAMP        5
#define IBUS_CH_LIMIT       6
#define IBUS_CH_OVERRIDE    7
#define IBUS_CH_KNOB        9

#define IBUS_CHANNELS       14
#define IBUS_LENGTH         32
#define IBUS_COMMAND        0x40

// ================================================================
//  PWM
// ================================================================

#define DRIVE_PWM_FREQ       20000
#define DRIVE_PWM_RES        8
#define ESC_PWM_FREQ         50
#define ESC_PWM_RES          16

// ================================================================
//  TIMING
// ================================================================

#define FAILSAFE_TIMEOUT_MS  250
#define ESC_ARM_TIME_MS      3000

#define RAMP_SLOW_STEP       10
#define RAMP_MEDIUM_STEP     25
#define RAMP_INTERVAL_MS     10

#define DEBUG_INTERVAL_MS    500

// ================================================================
//  EEPROM v7.1
// ================================================================

#define EEPROM_SIZE          36
#define EEPROM_MAGIC_V71     0xBC
#define EEPROM_ADDR_MAGIC    0
#define EEPROM_ADDR_LF       1
#define EEPROM_ADDR_LR       5
#define EEPROM_ADDR_RF       9
#define EEPROM_ADDR_RR       13
#define EEPROM_ADDR_MAXT     17
#define EEPROM_ADDR_MAXS     19
#define EEPROM_ADDR_CURVT    21
#define EEPROM_ADDR_CURVS    25
#define EEPROM_ADDR_DEADT    29
#define EEPROM_ADDR_DEADS    31

// ================================================================
//  WATCHDOG
// ================================================================

#define WDT_TIMEOUT_SEC  2

// ================================================================
//  SERIAL LOG RING BUFFER
// ================================================================

#define LOG_BUF_SIZE 4096

char logRing[LOG_BUF_SIZE];
volatile int logHead = 0;
volatile int logCount = 0;

void logWrite(const char* s) {
    int len = strlen(s);
    for (int i = 0; i < len; i++) {
        logRing[logHead] = s[i];
        logHead = (logHead + 1) % LOG_BUF_SIZE;
        if (logCount < LOG_BUF_SIZE) logCount++;
    }
}

void logWriteLn(const char* s) {
    logWrite(s);
    logWrite("\n");
}

// ================================================================
//  OBJECTS
// ================================================================

WebServer webServer(80);
WiFiServer telnetServer(TELNET_PORT);
WiFiClient telnetClient;
bool telnetConnected    = false;
bool telnetFirstDraw    = true;

// ================================================================
//  IBUS STATE
// ================================================================

uint8_t ibusBuffer[IBUS_LENGTH];
uint8_t ibusIndex = 0;
uint16_t channels[IBUS_CHANNELS];
bool ibusFrameReady = false;
unsigned long lastIbusFrame = 0;

// ================================================================
//  GLOBALS
// ================================================================

bool wifiActive       = false;
bool wifiShutdown     = false;
bool otaInProgress    = false;
bool escArmed         = false;   // Hardware ESC arm (3s sequence)

bool armLockReleased  = false;   // CH5 software arm (drive + weapon gate)
bool armSeenOff       = false;

bool weaponLocked     = true;    // CH7 Pos1 = locked

int currentWeaponPWM  = 1000;
int targetWeaponPWM   = 1000;
int prevWeaponPWM     = 1000;

unsigned long bootTime            = 0;
unsigned long wifiStartTime       = 0;
unsigned long lastValidSignalTime = 0;
unsigned long lastDebugTime       = 0;
unsigned long lastRampTime        = 0;

unsigned long goodFrames = 0;
unsigned long badFrames  = 0;

int debugLeftSpd      = 0;
int debugRightSpd     = 0;

uint32_t debugLeftFwd  = 0;
uint32_t debugLeftRev  = 0;
uint32_t debugRightFwd = 0;
uint32_t debugRightRev = 0;

int debugThrotRaw = 0;
int debugSteerRaw = 0;
int debugThrotOut = 0;
int debugSteerOut = 0;

bool wasRamping        = false;
int  rampStartPWM      = 1000;
int  lastRampPrint     = 1000;

char statusLine[80] = "";

bool inFailsafe = false;
bool debugActive = false;
int weaponLimitPos = 1;

IPAddress assignedIP;

// TUNING VALUES (EEPROM)
float trimLF = 1.00f;
float trimLR = 1.00f;
float trimRF = 1.00f;
float trimRR = 1.00f;

int16_t maxThrottle = 255;
int16_t maxSteering = 150;

float curveThrottle = 2.0f;
float curveSteering = 2.0f;

int16_t deadThrottle = 15;
int16_t deadSteering = 15;

// Print buffers
char usbBuf[1400];
int  usbPos = 0;

char telBuf[1600];
int  telPos = 0;

// ================================================================
//  FUNCTION DECLARATIONS
// ================================================================

void setupMotorPWM();
void setupWiFiAndWeb();
void setupIBUS();
void setupWatchdog();
void setupEEPROM();
void handleWiFi();
void handleTelnet();
void updateDebugState();
void readIBUS();
bool isSignalValid();
void processArmLock();
void processDrive();
void processWeapon();
int  applyCurve(int input, int maxOutput, float exponent, int deadzone);
void setMotor(int speed, uint8_t fwdCh, uint8_t revCh, float tFwd, float tRev);
void setESC(int pulseUs);
void stopAll();
void stopDrive();
void printDashboard();
void setStatus(const char* msg);
void saveSettingsToEEPROM();
void loadSettingsFromEEPROM();
void setupWebRoutes();

void usbPrint(const char* s);
void usbLine(const char* s);
void telPrint(const char* s);
void telLine(const char* s);

// ================================================================
//  BUFFER HELPERS
// ================================================================

void setStatus(const char* msg) {
    strncpy(statusLine, msg, sizeof(statusLine) - 1);
    statusLine[sizeof(statusLine) - 1] = '\0';
    logWriteLn(msg);
}

void usbPrint(const char* s) {
    int len = strlen(s);
    if (usbPos + len < (int)sizeof(usbBuf) - 1) {
        memcpy(usbBuf + usbPos, s, len);
        usbPos += len;
    }
}

void usbLine(const char* s) {
    usbPrint(s);
    usbPrint("\r\n");
}

void telPrint(const char* s) {
    int len = strlen(s);
    if (telPos + len < (int)sizeof(telBuf) - 1) {
        memcpy(telBuf + telPos, s, len);
        telPos += len;
    }
}

void telLine(const char* s) {
    telPrint(s);
    telPrint("\r\n");
}

// ================================================================
//  CONFIGURABLE CURVE FUNCTION
// ================================================================

int applyCurve(int input, int maxOutput, float exponent, int deadzone) {
    if (input == 0) return 0;

    int sign = (input > 0) ? 1 : -1;
    int absIn = abs(input);

    if (absIn <= deadzone) return 0;

    float normalized = (float)(absIn - deadzone) / (float)(255 - deadzone);

    if (normalized > 1.0f) normalized = 1.0f;
    if (normalized < 0.0f) normalized = 0.0f;

    float curved = powf(normalized, exponent);

    int result = (int)(curved * (float)maxOutput + 0.5f);

    if (result < 1 && absIn > deadzone) result = 1;
    if (result > maxOutput) result = maxOutput;

    return result * sign;
}

// ================================================================
//  DEBUG STATE
// ================================================================

void updateDebugState() {
    if (DEBUG_MODE) {
        debugActive = true;
    } else if (wifiActive) {
        debugActive = true;
    } else {
        debugActive = false;
    }
}

// ================================================================
//  EEPROM
// ================================================================

void setupEEPROM() {
    EEPROM.begin(EEPROM_SIZE);
    loadSettingsFromEEPROM();
}

void loadSettingsFromEEPROM() {
    uint8_t magic = EEPROM.read(EEPROM_ADDR_MAGIC);

    if (magic == EEPROM_MAGIC_V71) {
        EEPROM.get(EEPROM_ADDR_LF, trimLF);
        EEPROM.get(EEPROM_ADDR_LR, trimLR);
        EEPROM.get(EEPROM_ADDR_RF, trimRF);
        EEPROM.get(EEPROM_ADDR_RR, trimRR);
        EEPROM.get(EEPROM_ADDR_MAXT, maxThrottle);
        EEPROM.get(EEPROM_ADDR_MAXS, maxSteering);
        EEPROM.get(EEPROM_ADDR_CURVT, curveThrottle);
        EEPROM.get(EEPROM_ADDR_CURVS, curveSteering);
        EEPROM.get(EEPROM_ADDR_DEADT, deadThrottle);
        EEPROM.get(EEPROM_ADDR_DEADS, deadSteering);

        if (trimLF < 0.0f || trimLF > 1.0f || isnan(trimLF)) trimLF = 1.0f;
        if (trimLR < 0.0f || trimLR > 1.0f || isnan(trimLR)) trimLR = 1.0f;
        if (trimRF < 0.0f || trimRF > 1.0f || isnan(trimRF)) trimRF = 1.0f;
        if (trimRR < 0.0f || trimRR > 1.0f || isnan(trimRR)) trimRR = 1.0f;

        if (maxThrottle < 10 || maxThrottle > 255) maxThrottle = 255;
        if (maxSteering < 10 || maxSteering > 255) maxSteering = 150;

        if (curveThrottle < 1.0f || curveThrottle > 3.0f || isnan(curveThrottle)) curveThrottle = 2.0f;
        if (curveSteering < 1.0f || curveSteering > 3.0f || isnan(curveSteering)) curveSteering = 2.0f;

        if (deadThrottle < 0 || deadThrottle > 50) deadThrottle = 15;
        if (deadSteering < 0 || deadSteering > 50) deadSteering = 15;

        Serial.println("[EEPROM] Loaded settings v7.1:");
    } else {
        trimLF = 1.0f; trimLR = 1.0f; trimRF = 1.0f; trimRR = 1.0f;
        maxThrottle = 255; maxSteering = 150;
        curveThrottle = 2.0f; curveSteering = 2.0f;
        deadThrottle = 15; deadSteering = 15;
        Serial.println("[EEPROM] No v7.1 data -- using defaults:");
    }

    char msg[120];
    sprintf(msg, "[SET] Trim: LF=%.2f LR=%.2f RF=%.2f RR=%.2f",
            trimLF, trimLR, trimRF, trimRR);
    Serial.println(msg);
    logWriteLn(msg);

    sprintf(msg, "[SET] MaxT=%d MaxS=%d CurveT=%.1f CurveS=%.1f DeadT=%d DeadS=%d",
            maxThrottle, maxSteering, curveThrottle, curveSteering,
            deadThrottle, deadSteering);
    Serial.println(msg);
    logWriteLn(msg);
}

void saveSettingsToEEPROM() {
    EEPROM.write(EEPROM_ADDR_MAGIC, EEPROM_MAGIC_V71);
    EEPROM.put(EEPROM_ADDR_LF, trimLF);
    EEPROM.put(EEPROM_ADDR_LR, trimLR);
    EEPROM.put(EEPROM_ADDR_RF, trimRF);
    EEPROM.put(EEPROM_ADDR_RR, trimRR);
    EEPROM.put(EEPROM_ADDR_MAXT, maxThrottle);
    EEPROM.put(EEPROM_ADDR_MAXS, maxSteering);
    EEPROM.put(EEPROM_ADDR_CURVT, curveThrottle);
    EEPROM.put(EEPROM_ADDR_CURVS, curveSteering);
    EEPROM.put(EEPROM_ADDR_DEADT, deadThrottle);
    EEPROM.put(EEPROM_ADDR_DEADS, deadSteering);
    EEPROM.commit();

    setStatus("SETTINGS SAVED to EEPROM");
}

// ================================================================
//  WEB PAGE -- MAIN DASHBOARD
// ================================================================

const char DASH_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset='UTF-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>BattleBot v7.2</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Courier New',monospace;background:#0a0a0a;color:#0f0;padding:10px}
h1{text-align:center;font-size:22px;margin:8px 0;color:#0f0}
.g{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin:8px 0}
@media(max-width:700px){.g{grid-template-columns:1fr}}
.box{background:#111;border:1px solid #0a0;border-radius:6px;padding:10px}
.box h2{font-size:14px;color:#0a0;margin-bottom:6px;border-bottom:1px solid #060;padding-bottom:4px}
table{width:100%;font-size:12px}
td{padding:2px 4px}
td:first-child{color:#0a0;white-space:nowrap}
td:last-child{text-align:right;color:#0f0;font-weight:bold}
.bar-bg{background:#030;height:16px;border-radius:3px;overflow:hidden;margin:2px 0}
.bar-fg{background:#0f0;height:100%;transition:width 0.3s}
.log{background:#000;border:1px solid #060;height:200px;overflow-y:auto;
     font-size:11px;padding:4px;color:#0c0;white-space:pre-wrap;word-wrap:break-word}
.sr{display:flex;align-items:center;margin:4px 0;gap:6px}
.sr label{width:70px;font-size:11px;color:#0a0;flex-shrink:0}
.sr input[type=range]{flex:1;accent-color:#0f0}
.sr span{width:45px;font-size:11px;text-align:right;color:#0f0;flex-shrink:0}
.btn{background:#0a0;color:#000;border:none;padding:8px 16px;font-size:14px;
     font-weight:bold;cursor:pointer;border-radius:4px;margin:4px}
.btn:hover{background:#0f0}
.btn-r{background:#a00;color:#fff}
.btn-r:hover{background:#f00}
.status{text-align:center;padding:6px;background:#020;border:1px solid #0a0;
        border-radius:4px;font-size:13px;margin:4px 0}
.armed{color:#f00;font-weight:bold}
.disarmed{color:#0f0}
.locked{color:#ff0;font-weight:bold}
.unlocked{color:#0f0}
.fs{color:#f00;font-size:18px;text-align:center;padding:10px;
    background:#200;border:2px solid #f00;border-radius:6px;margin:4px 0;display:none}
.full{grid-column:1/-1}
.ch-grid{display:grid;grid-template-columns:repeat(7,1fr);gap:2px;font-size:11px;text-align:center}
.ch-grid div{background:#060;padding:2px;border-radius:2px}
.ch-grid .lbl{background:none;color:#080;font-size:10px}
a{color:#0f0}
.top-bar{display:flex;justify-content:space-between;align-items:center;padding:4px 0;flex-wrap:wrap;gap:4px}
.top-bar span{font-size:12px;color:#080}
.sec{color:#080;font-size:11px;margin:6px 0 2px 0;border-top:1px solid #040;padding-top:4px}
</style>
</head>
<body>
<h1>== BATTLEBOT v7.2 ==</h1>
<div class='top-bar'>
  <span id='ip'></span>
  <span id='rssi'></span>
  <span id='up'></span>
  <a href='/update' class='btn' style='font-size:11px;padding:4px 8px'>OTA UPDATE</a>
</div>

<div class='fs' id='fs'>!! FAILSAFE -- NO SIGNAL !!</div>
<div class='status' id='st'>Connecting...</div>

<div class='g'>

  <div class='box'>
    <h2>CHANNELS</h2>
    <div class='ch-grid'>
      <div class='lbl'>CH1</div><div class='lbl'>CH2</div><div class='lbl'>CH3</div>
      <div class='lbl'>CH4</div><div class='lbl'>CH5</div><div class='lbl'>CH6</div><div class='lbl'>CH7</div>
      <div id='c0'>-</div><div id='c1'>-</div><div id='c2'>-</div>
      <div id='c3'>-</div><div id='c4'>-</div><div id='c5'>-</div><div id='c6'>-</div>
      <div class='lbl'>CH8</div><div class='lbl'>CH9</div><div class='lbl'>CH10</div>
      <div class='lbl'>CH11</div><div class='lbl'>CH12</div><div class='lbl'>CH13</div><div class='lbl'>CH14</div>
      <div id='c7'>-</div><div id='c8'>-</div><div id='c9'>-</div>
      <div id='c10'>-</div><div id='c11'>-</div><div id='c12'>-</div><div id='c13'>-</div>
    </div>
  </div>

  <div class='box'>
    <h2>DRIVE</h2>
    <table>
      <tr><td>Throttle Raw</td><td id='tr'>0</td></tr>
      <tr><td>Throttle Out</td><td id='to'>0</td></tr>
      <tr><td>Steer Raw</td><td id='sr2'>0</td></tr>
      <tr><td>Steer Out</td><td id='so'>0</td></tr>
      <tr><td>Left Motor</td><td id='lm'>0</td></tr>
      <tr><td>Right Motor</td><td id='rm'>0</td></tr>
    </table>
    <table style='margin-top:4px'>
      <tr><td>Pin LF(25)</td><td id='plf'>0</td></tr>
      <tr><td>Pin LR(26)</td><td id='plr'>0</td></tr>
      <tr><td>Pin RF(27)</td><td id='prf'>0</td></tr>
      <tr><td>Pin RR(13)</td><td id='prr'>0</td></tr>
    </table>
  </div>

  <div class='box'>
    <h2>WEAPON</h2>
    <table>
      <tr><td>CH5 Arm</td><td id='arm'>-</td></tr>
      <tr><td>ESC Init</td><td id='esc'>-</td></tr>
      <tr><td>Weapon Lock</td><td id='wlk'>-</td></tr>
      <tr><td>Limit</td><td id='lim'>-</td></tr>
      <tr><td>Target PWM</td><td id='wt'>1000</td></tr>
      <tr><td>Current PWM</td><td id='wc'>1000</td></tr>
      <tr><td>Ramping</td><td id='rmp'>NO</td></tr>
    </table>
    <div style='margin-top:6px'>
      <div class='bar-bg'><div class='bar-fg' id='wb' style='width:0%'></div></div>
      <div style='text-align:center;font-size:14px' id='wp'>0%</div>
    </div>
  </div>

  <div class='box'>
    <h2>TUNING (saved to EEPROM)</h2>

    <div class='sec'>-- MOTOR TRIM --</div>
    <div class='sr'>
      <label>Trim LF</label>
      <input type='range' min='0' max='100' value='100' id='tLF' oninput="sU('tLF','vLF',100)">
      <span id='vLF'>1.00</span>
    </div>
    <div class='sr'>
      <label>Trim LR</label>
      <input type='range' min='0' max='100' value='100' id='tLR' oninput="sU('tLR','vLR',100)">
      <span id='vLR'>1.00</span>
    </div>
    <div class='sr'>
      <label>Trim RF</label>
      <input type='range' min='0' max='100' value='100' id='tRF' oninput="sU('tRF','vRF',100)">
      <span id='vRF'>1.00</span>
    </div>
    <div class='sr'>
      <label>Trim RR</label>
      <input type='range' min='0' max='100' value='100' id='tRR' oninput="sU('tRR','vRR',100)">
      <span id='vRR'>1.00</span>
    </div>

    <div class='sec'>-- MAX SPEED --</div>
    <div class='sr'>
      <label>Max Throt</label>
      <input type='range' min='10' max='255' value='255' id='sMaxT' oninput="sI('sMaxT','vMaxT')">
      <span id='vMaxT'>255</span>
    </div>
    <div class='sr'>
      <label>Max Steer</label>
      <input type='range' min='10' max='255' value='150' id='sMaxS' oninput="sI('sMaxS','vMaxS')">
      <span id='vMaxS'>150</span>
    </div>

    <div class='sec'>-- CURVE (1.0=linear 2.0=quad 3.0=cubic) --</div>
    <div class='sr'>
      <label>Curve Thr</label>
      <input type='range' min='10' max='30' value='20' id='sCurT' oninput="sU('sCurT','vCurT',10)">
      <span id='vCurT'>2.0</span>
    </div>
    <div class='sr'>
      <label>Curve Str</label>
      <input type='range' min='10' max='30' value='20' id='sCurS' oninput="sU('sCurS','vCurS',10)">
      <span id='vCurS'>2.0</span>
    </div>

    <div class='sec'>-- DEADZONE (0-50) --</div>
    <div class='sr'>
      <label>Dead Thr</label>
      <input type='range' min='0' max='50' value='15' id='sDeadT' oninput="sI('sDeadT','vDeadT')">
      <span id='vDeadT'>15</span>
    </div>
    <div class='sr'>
      <label>Dead Str</label>
      <input type='range' min='0' max='50' value='15' id='sDeadS' oninput="sI('sDeadS','vDeadS')">
      <span id='vDeadS'>15</span>
    </div>

    <div style='text-align:center;margin-top:8px'>
      <button class='btn' onclick='saveAll()'>SAVE ALL</button>
      <button class='btn btn-r' onclick='resetAll()'>RESET ALL</button>
    </div>
    <div id='tMsg' style='text-align:center;font-size:12px;margin-top:4px'></div>
  </div>

  <div class='box full'>
    <h2>SERIAL MONITOR</h2>
    <div class='log' id='log'></div>
    <div style='text-align:right;margin-top:4px'>
      <button class='btn' onclick="document.getElementById('log').innerHTML=''" style='font-size:11px;padding:3px 8px'>CLEAR</button>
    </div>
  </div>

  <div class='box full'>
    <h2>SYSTEM</h2>
    <table>
      <tr><td>Signal Age</td><td id='sig'>-</td></tr>
      <tr><td>Good Frames</td><td id='gf'>0</td></tr>
      <tr><td>Bad Frames</td><td id='bf'>0</td></tr>
      <tr><td>Uptime</td><td id='upt'>0s</td></tr>
      <tr><td>WiFi RSSI</td><td id='rss'>-</td></tr>
      <tr><td>Free Heap</td><td id='heap'>-</td></tr>
    </table>
  </div>

</div>

<script>
var logPos=0;

function sU(sid,vid,div){
  var v=document.getElementById(sid).value;
  document.getElementById(vid).textContent=(v/div).toFixed(div==100?2:1);
}
function sI(sid,vid){
  document.getElementById(vid).textContent=document.getElementById(sid).value;
}

function poll(){
  fetch('/api/data').then(r=>r.json()).then(d=>{
    for(var i=0;i<14;i++){var e=document.getElementById('c'+i);if(e)e.textContent=d.ch[i];}
    document.getElementById('tr').textContent=d.tr;
    document.getElementById('to').textContent=d.to;
    document.getElementById('sr2').textContent=d.sr;
    document.getElementById('so').textContent=d.so;
    document.getElementById('lm').textContent=d.lm;
    document.getElementById('rm').textContent=d.rm;
    document.getElementById('plf').textContent=d.plf;
    document.getElementById('plr').textContent=d.plr;
    document.getElementById('prf').textContent=d.prf;
    document.getElementById('prr').textContent=d.prr;
    document.getElementById('arm').innerHTML=d.armed?'<span class="armed">ARMED</span>':'<span class="disarmed">OFF</span>';
    document.getElementById('esc').textContent=d.esc?'READY':'WAIT';
    document.getElementById('wlk').innerHTML=d.wlocked?'<span class="locked">LOCKED</span>':'<span class="unlocked">UNLOCKED</span>';
    document.getElementById('lim').textContent=d.limLbl;
    document.getElementById('wt').textContent=d.wt;
    document.getElementById('wc').textContent=d.wc;
    document.getElementById('wp').textContent=d.wpct+'%';
    document.getElementById('wb').style.width=d.wpct+'%';
    document.getElementById('rmp').textContent=d.ramping?'YES':'NO';
    document.getElementById('sig').textContent=d.sigAge+'ms';
    document.getElementById('gf').textContent=d.gf;
    document.getElementById('bf').textContent=d.bf;
    document.getElementById('upt').textContent=d.up+'s';
    document.getElementById('rss').textContent=d.rssi+'dBm';
    document.getElementById('heap').textContent=d.heap;
    document.getElementById('ip').textContent=d.ip;
    document.getElementById('st').textContent=d.status;
    var fs=document.getElementById('fs');
    if(d.failsafe){fs.style.display='block';}else{fs.style.display='none';}
  }).catch(e=>{
    document.getElementById('st').textContent='Connection lost...';
  });
}

function pollLog(){
  fetch('/api/log?pos='+logPos).then(r=>r.json()).then(d=>{
    if(d.text.length>0){
      var el=document.getElementById('log');
      el.textContent+=d.text;
      el.scrollTop=el.scrollHeight;
    }
    logPos=d.pos;
  }).catch(e=>{});
}

function loadSettings(){
  fetch('/api/settings').then(r=>r.json()).then(d=>{
    document.getElementById('tLF').value=Math.round(d.lf*100);
    document.getElementById('tLR').value=Math.round(d.lr*100);
    document.getElementById('tRF').value=Math.round(d.rf*100);
    document.getElementById('tRR').value=Math.round(d.rr*100);
    document.getElementById('vLF').textContent=d.lf.toFixed(2);
    document.getElementById('vLR').textContent=d.lr.toFixed(2);
    document.getElementById('vRF').textContent=d.rf.toFixed(2);
    document.getElementById('vRR').textContent=d.rr.toFixed(2);
    document.getElementById('sMaxT').value=d.maxT;
    document.getElementById('vMaxT').textContent=d.maxT;
    document.getElementById('sMaxS').value=d.maxS;
    document.getElementById('vMaxS').textContent=d.maxS;
    document.getElementById('sCurT').value=Math.round(d.curT*10);
    document.getElementById('vCurT').textContent=d.curT.toFixed(1);
    document.getElementById('sCurS').value=Math.round(d.curS*10);
    document.getElementById('vCurS').textContent=d.curS.toFixed(1);
    document.getElementById('sDeadT').value=d.deadT;
    document.getElementById('vDeadT').textContent=d.deadT;
    document.getElementById('sDeadS').value=d.deadS;
    document.getElementById('vDeadS').textContent=d.deadS;
  });
}

function saveAll(){
  var obj={
    lf:document.getElementById('tLF').value/100,
    lr:document.getElementById('tLR').value/100,
    rf:document.getElementById('tRF').value/100,
    rr:document.getElementById('tRR').value/100,
    maxT:parseInt(document.getElementById('sMaxT').value),
    maxS:parseInt(document.getElementById('sMaxS').value),
    curT:document.getElementById('sCurT').value/10,
    curS:document.getElementById('sCurS').value/10,
    deadT:parseInt(document.getElementById('sDeadT').value),
    deadS:parseInt(document.getElementById('sDeadS').value)
  };
  fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify(obj)
  }).then(r=>r.json()).then(d=>{
    document.getElementById('tMsg').textContent='Saved!';
    document.getElementById('tMsg').style.color='#0f0';
    setTimeout(()=>{document.getElementById('tMsg').textContent='';},3000);
  }).catch(e=>{
    document.getElementById('tMsg').textContent='Failed!';
    document.getElementById('tMsg').style.color='#f00';
  });
}

function resetAll(){
  document.getElementById('tLF').value=100; sU('tLF','vLF',100);
  document.getElementById('tLR').value=100; sU('tLR','vLR',100);
  document.getElementById('tRF').value=100; sU('tRF','vRF',100);
  document.getElementById('tRR').value=100; sU('tRR','vRR',100);
  document.getElementById('sMaxT').value=255; sI('sMaxT','vMaxT');
  document.getElementById('sMaxS').value=150; sI('sMaxS','vMaxS');
  document.getElementById('sCurT').value=20; sU('sCurT','vCurT',10);
  document.getElementById('sCurS').value=20; sU('sCurS','vCurS',10);
  document.getElementById('sDeadT').value=15; sI('sDeadT','vDeadT');
  document.getElementById('sDeadS').value=15; sI('sDeadS','vDeadS');
  saveAll();
}

loadSettings();
setInterval(poll,100);
setInterval(pollLog,250);
</script>
</body>
</html>
)rawliteral";

// ================================================================
//  WEB PAGE -- OTA UPDATE
// ================================================================

const char OTA_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset='UTF-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>BattleBot OTA</title>
<style>
body{font-family:monospace;background:#0a0a0a;color:#0f0;padding:20px;text-align:center}
h1{color:#0f0;font-size:24px}
.box{background:#111;border:2px solid #0a0;border-radius:10px;padding:20px;
     max-width:400px;margin:20px auto}
input[type=file]{color:#0f0;margin:10px 0}
input[type=submit]{background:#0a0;color:#000;border:none;padding:12px 30px;
     font-size:16px;font-weight:bold;cursor:pointer;border-radius:5px;margin:10px}
input[type=submit]:hover{background:#0f0}
.bar{background:#111;border:1px solid #0a0;height:30px;border-radius:5px;
     margin:10px 0;overflow:hidden;display:none}
.fill{background:#0f0;height:100%;width:0%;transition:width 0.3s}
.pct{color:#0f0;font-size:20px;margin:5px}
.warn{color:#f00;font-size:14px;margin:10px}
a{color:#0f0}
</style>
</head>
<body>
<h1>== FIRMWARE UPDATE ==</h1>
<div class='box'>
  <p>Select .bin file (Sketch -> Export Compiled Binary)</p>
  <form method='POST' action='/update' enctype='multipart/form-data' id='uf'>
    <input type='file' name='update' accept='.bin' id='file'><br>
    <input type='submit' value='UPLOAD FIRMWARE' id='btn'>
  </form>
  <div class='bar' id='bar'><div class='fill' id='fill'></div></div>
  <div class='pct' id='pct'></div>
  <div class='warn'>WARNING: ALL MOTORS STOP DURING UPLOAD</div>
  <br><a href='/'>Back to Dashboard</a>
</div>
<script>
var form=document.getElementById('uf');
form.addEventListener('submit',function(e){
  e.preventDefault();
  var file=document.getElementById('file').files[0];
  if(!file){alert('Select a .bin file');return;}
  var xhr=new XMLHttpRequest();
  var fd=new FormData();
  fd.append('update',file);
  document.getElementById('bar').style.display='block';
  document.getElementById('btn').disabled=true;
  document.getElementById('btn').value='UPLOADING...';
  xhr.upload.addEventListener('progress',function(e){
    if(e.lengthComputable){
      var p=Math.round((e.loaded/e.total)*100);
      document.getElementById('fill').style.width=p+'%';
      document.getElementById('pct').innerHTML=p+'%';
    }
  });
  xhr.onreadystatechange=function(){
    if(xhr.readyState==4){
      if(xhr.status==200){
        document.getElementById('pct').innerHTML='<span style="color:#0f0">SUCCESS -- Rebooting...</span>';
      }else{
        document.getElementById('pct').innerHTML='<span style="color:red">FAILED</span>';
        document.getElementById('btn').disabled=false;
        document.getElementById('btn').value='RETRY';
      }
    }
  };
  xhr.open('POST','/update',true);
  xhr.send(fd);
});
</script>
</body>
</html>
)rawliteral";

// ================================================================
//  SETUP
// ================================================================

void setup() {

    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("===========================================================");
    Serial.println("  BATTLE BOT v7.2 -- BOOT START");
    Serial.println("===========================================================");

    logWriteLn("=== BATTLE BOT v7.2 BOOT ===");

    bootTime = millis();
    lastValidSignalTime = millis();

    setupEEPROM();
    setupMotorPWM();
    setupIBUS();
    setupWiFiAndWeb();

    Serial.println();
    Serial.println("[ARM] CH5 = master arm (drive + weapon gate)");
    Serial.println("[WPN] CH7: Pos1=LOCKED  Pos2=50%  Pos3=83%");
    Serial.println("[WPN] CH8: Override 100% (only when CH7 unlocked)");
    Serial.println("[ESC] 3s hardware init at boot (1000us signal)");

    char msg[80];
    sprintf(msg, "[DRV] Throttle: exp=%.1f max=%d dead=%d",
            curveThrottle, maxThrottle, deadThrottle);
    Serial.println(msg);
    sprintf(msg, "[DRV] Steering: exp=%.1f max=%d dead=%d",
            curveSteering, maxSteering, deadSteering);
    Serial.println(msg);

    logWriteLn("ARM LOCK ACTIVE");

    Serial.println();
    Serial.println("===========================================================");
    Serial.println("  BOOT COMPLETE -- v7.2");
    Serial.println("===========================================================");
    Serial.println();

    setStatus("BOOT OK -- Waiting for signal...");

    setupWatchdog();
}

// ================================================================
//  MOTOR PWM SETUP
// ================================================================

void setupMotorPWM() {
    ledcSetup(LEDC_CH_L_FWD, DRIVE_PWM_FREQ, DRIVE_PWM_RES);
    ledcSetup(LEDC_CH_L_REV, DRIVE_PWM_FREQ, DRIVE_PWM_RES);
    ledcSetup(LEDC_CH_R_FWD, DRIVE_PWM_FREQ, DRIVE_PWM_RES);
    ledcSetup(LEDC_CH_R_REV, DRIVE_PWM_FREQ, DRIVE_PWM_RES);

    ledcAttachPin(LEFT_RPWM_PIN,  LEDC_CH_L_FWD);
    ledcAttachPin(LEFT_LPWM_PIN,  LEDC_CH_L_REV);
    ledcAttachPin(RIGHT_RPWM_PIN, LEDC_CH_R_FWD);
    ledcAttachPin(RIGHT_LPWM_PIN, LEDC_CH_R_REV);

    ledcSetup(LEDC_CH_ESC, ESC_PWM_FREQ, ESC_PWM_RES);
    ledcAttachPin(ESC_PIN, LEDC_CH_ESC);

    stopAll();
    Serial.println("[MOT] PWM ready -- L:G25/G26 R:G27/G13 ESC:G33");
    logWriteLn("Motors initialized");
}

// ================================================================
//  WIFI + WEB SETUP
// ================================================================

void setupWiFiAndWeb() {
    Serial.println("[WIFI] Starting STA mode...");
    logWriteLn("WiFi connecting...");

    #ifdef CONFIG_BT_ENABLED
        btStop();
    #endif

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(200);

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    delay(100);

    Serial.print("[WIFI] SSID: ");
    Serial.println(WIFI_SSID);

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long connectStart = millis();
    bool connected = false;

    while (millis() - connectStart < WIFI_CONNECT_TIMEOUT_MS) {
        if (WiFi.status() == WL_CONNECTED) {
            connected = true;
            break;
        }
        Serial.print(".");
        delay(250);
    }
    Serial.println();

    if (!connected) {
        Serial.println("[WIFI] FAILED -- no connection in 10s");
        Serial.println("[WIFI] Bot runs without WiFi");
        logWriteLn("WiFi FAILED -- running without");
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        delay(100);
        wifiActive = false;
        return;
    }

    assignedIP = WiFi.localIP();
    wifiActive = true;
    wifiStartTime = millis();

    Serial.print("[WIFI] CONNECTED -- IP: ");
    Serial.println(assignedIP);
    Serial.print("[WIFI] RSSI: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");

    char lmsg[60];
    sprintf(lmsg, "WiFi OK -- IP: %s (%ddBm)",
            assignedIP.toString().c_str(), WiFi.RSSI());
    logWriteLn(lmsg);

    telnetServer.begin();
    telnetServer.setNoDelay(true);
    Serial.print("[TEL] Telnet: ");
    Serial.print(assignedIP);
    Serial.println(":23");

    setupWebRoutes();
    webServer.begin();

    Serial.print("[WEB] Dashboard: http://");
    Serial.println(assignedIP);
    Serial.print("[OTA] Update:    http://");
    Serial.print(assignedIP);
    Serial.println("/update");

    logWriteLn("Web server started");
}

// ================================================================
//  WEB ROUTES
// ================================================================

void setupWebRoutes() {

    webServer.on("/", HTTP_GET, []() {
        webServer.send_P(200, "text/html", DASH_PAGE);
    });

    webServer.on("/update", HTTP_GET, []() {
        if (!webServer.authenticate(OTA_USERNAME, OTA_PASSWORD)) {
            return webServer.requestAuthentication();
        }
        webServer.send_P(200, "text/html", OTA_PAGE);
    });

    webServer.on("/update", HTTP_POST,
        []() {
            bool ok = !Update.hasError();
            webServer.send(200, "text/plain", ok ? "OK" : "FAIL");
            if (ok) {
                delay(500);
                ESP.restart();
            } else {
                otaInProgress = false;
                Serial2.begin(115200, SERIAL_8N1, IBUS_RX_PIN, -1);
                while (Serial2.available()) Serial2.read();
                ibusIndex = 0;
                setStatus("OTA FAILED -- IBUS restarted");
            }
        },
        []() {
            HTTPUpload& upload = webServer.upload();
            if (upload.status == UPLOAD_FILE_START) {
                stopAll();
                otaInProgress = true;
                Serial2.end();
                Serial.printf("[OTA] Start: %s\n", upload.filename.c_str());
                logWriteLn("OTA upload started...");
                if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                    Update.printError(Serial);
                }
            } else if (upload.status == UPLOAD_FILE_WRITE) {
                esp_task_wdt_reset();
                if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                    Update.printError(Serial);
                }
            } else if (upload.status == UPLOAD_FILE_END) {
                if (Update.end(true)) {
                    Serial.printf("[OTA] OK: %u bytes\n", upload.totalSize);
                    logWriteLn("OTA success -- rebooting");
                } else {
                    Update.printError(Serial);
                    logWriteLn("OTA FAILED");
                }
            }
        }
    );

    // JSON telemetry
    webServer.on("/api/data", HTTP_GET, []() {
        wifiStartTime = millis();

        int wpct = (currentWeaponPWM - 1000) / 10;
        const char* limLbl;
        if (weaponLimitPos == 1)      limLbl = "LOCKED";
        else if (weaponLimitPos == 2) limLbl = "50%";
        else                          limLbl = "83%";

        int rssi = 0;
        if (WiFi.status() == WL_CONNECTED) rssi = WiFi.RSSI();

        unsigned long uptime = (millis() - bootTime) / 1000;
        unsigned long sigAge = millis() - lastValidSignalTime;

        char json[750];
        snprintf(json, sizeof(json),
            "{\"ch\":[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u],"
            "\"tr\":%d,\"to\":%d,\"sr\":%d,\"so\":%d,"
            "\"lm\":%d,\"rm\":%d,"
            "\"plf\":%u,\"plr\":%u,\"prf\":%u,\"prr\":%u,"
            "\"armed\":%s,\"esc\":%s,\"wlocked\":%s,"
            "\"limLbl\":\"%s\",\"wt\":%d,\"wc\":%d,\"wpct\":%d,"
            "\"ramping\":%s,\"failsafe\":%s,"
            "\"sigAge\":%lu,\"gf\":%lu,\"bf\":%lu,"
            "\"up\":%lu,\"rssi\":%d,\"heap\":%u,"
            "\"ip\":\"%s\",\"status\":\"%s\"}",
            channels[0], channels[1], channels[2], channels[3],
            channels[4], channels[5], channels[6], channels[7],
            channels[8], channels[9], channels[10], channels[11],
            channels[12], channels[13],
            debugThrotRaw, debugThrotOut, debugSteerRaw, debugSteerOut,
            debugLeftSpd, debugRightSpd,
            debugLeftFwd, debugLeftRev, debugRightFwd, debugRightRev,
            armLockReleased ? "true" : "false",
            escArmed ? "true" : "false",
            weaponLocked ? "true" : "false",
            limLbl, targetWeaponPWM, currentWeaponPWM, wpct,
            (currentWeaponPWM != targetWeaponPWM) ? "true" : "false",
            inFailsafe ? "true" : "false",
            sigAge, goodFrames, badFrames,
            uptime, rssi, ESP.getFreeHeap(),
            assignedIP.toString().c_str(),
            statusLine
        );

        webServer.send(200, "application/json", json);
    });

    // Settings GET
    webServer.on("/api/settings", HTTP_GET, []() {
        wifiStartTime = millis();
        char json[200];
        snprintf(json, sizeof(json),
            "{\"lf\":%.2f,\"lr\":%.2f,\"rf\":%.2f,\"rr\":%.2f,"
            "\"maxT\":%d,\"maxS\":%d,"
            "\"curT\":%.1f,\"curS\":%.1f,"
            "\"deadT\":%d,\"deadS\":%d}",
            trimLF, trimLR, trimRF, trimRR,
            maxThrottle, maxSteering,
            curveThrottle, curveSteering,
            deadThrottle, deadSteering);
        webServer.send(200, "application/json", json);
    });

    // Settings POST
    webServer.on("/api/settings", HTTP_POST, []() {
        wifiStartTime = millis();

        String body = webServer.arg("plain");

        float lf = 1.0f, lr = 1.0f, rf = 1.0f, rr = 1.0f;
        int mt = 255, ms = 150;
        float ct = 2.0f, cs = 2.0f;
        int dt = 15, ds = 15;

        int idx;
        idx = body.indexOf("\"lf\":");
        if (idx >= 0) lf = body.substring(idx + 5).toFloat();
        idx = body.indexOf("\"lr\":");
        if (idx >= 0) lr = body.substring(idx + 5).toFloat();
        idx = body.indexOf("\"rf\":");
        if (idx >= 0) rf = body.substring(idx + 5).toFloat();
        idx = body.indexOf("\"rr\":");
        if (idx >= 0) rr = body.substring(idx + 5).toFloat();
        idx = body.indexOf("\"maxT\":");
        if (idx >= 0) mt = body.substring(idx + 7).toInt();
        idx = body.indexOf("\"maxS\":");
        if (idx >= 0) ms = body.substring(idx + 7).toInt();
        idx = body.indexOf("\"curT\":");
        if (idx >= 0) ct = body.substring(idx + 7).toFloat();
        idx = body.indexOf("\"curS\":");
        if (idx >= 0) cs = body.substring(idx + 7).toFloat();
        idx = body.indexOf("\"deadT\":");
        if (idx >= 0) dt = body.substring(idx + 8).toInt();
        idx = body.indexOf("\"deadS\":");
        if (idx >= 0) ds = body.substring(idx + 8).toInt();

        if (lf < 0.0f || lf > 1.0f || isnan(lf)) lf = 1.0f;
        if (lr < 0.0f || lr > 1.0f || isnan(lr)) lr = 1.0f;
        if (rf < 0.0f || rf > 1.0f || isnan(rf)) rf = 1.0f;
        if (rr < 0.0f || rr > 1.0f || isnan(rr)) rr = 1.0f;
        if (mt < 10 || mt > 255) mt = 255;
        if (ms < 10 || ms > 255) ms = 150;
        if (ct < 1.0f || ct > 3.0f || isnan(ct)) ct = 2.0f;
        if (cs < 1.0f || cs > 3.0f || isnan(cs)) cs = 2.0f;
        if (dt < 0 || dt > 50) dt = 15;
        if (ds < 0 || ds > 50) ds = 15;

        trimLF = lf; trimLR = lr; trimRF = rf; trimRR = rr;
        maxThrottle = (int16_t)mt;
        maxSteering = (int16_t)ms;
        curveThrottle = ct;
        curveSteering = cs;
        deadThrottle = (int16_t)dt;
        deadSteering = (int16_t)ds;

        saveSettingsToEEPROM();

        webServer.send(200, "application/json", "{\"ok\":true}");
    });

    // Log API
    webServer.on("/api/log", HTTP_GET, []() {
        wifiStartTime = millis();

        int reqPos = 0;
        if (webServer.hasArg("pos")) {
            reqPos = webServer.arg("pos").toInt();
        }

        String text = "";
        int newPos = logHead;

        if (reqPos != newPos) {
            int sendStart = reqPos;
            if (logCount >= LOG_BUF_SIZE && (newPos - reqPos > LOG_BUF_SIZE || reqPos > newPos)) {
                sendStart = newPos;
            }

            int pos = sendStart;
            int safeCount = 0;
            while (pos != newPos && safeCount < LOG_BUF_SIZE) {
                int ringIdx = pos % LOG_BUF_SIZE;
                if (ringIdx < 0) ringIdx += LOG_BUF_SIZE;
                char c = logRing[ringIdx];
                if (c == '"') text += "\\\"";
                else if (c == '\\') text += "\\\\";
                else if (c == '\n') text += "\\n";
                else if (c == '\r') { /* skip */ }
                else if (c >= 32 && c < 127) text += c;
                pos++;
                safeCount++;
                if (text.length() > 2048) break;
            }
        }

        char json[2200];
        snprintf(json, sizeof(json),
            "{\"text\":\"%s\",\"pos\":%d}",
            text.c_str(), newPos);

        webServer.send(200, "application/json", json);
    });
}

// ================================================================
//  IBUS SETUP
// ================================================================

void setupIBUS() {
    Serial.println("[IBUS] Starting...");
    Serial2.setRxBufferSize(512);
    Serial2.begin(115200, SERIAL_8N1, IBUS_RX_PIN, -1);
    while (Serial2.available()) Serial2.read();
    ibusIndex = 0;
    ibusFrameReady = false;
    Serial.println("[IBUS] Ready on GPIO16");
    logWriteLn("IBUS initialized");
}

void setupWatchdog() {
    esp_task_wdt_init(WDT_TIMEOUT_SEC, true);
    esp_task_wdt_add(NULL);
    Serial.print("[WDT] Active -- ");
    Serial.print(WDT_TIMEOUT_SEC);
    Serial.println("s");
}

// ================================================================
//  TELNET
// ================================================================

void handleTelnet() {
    if (!wifiActive) return;

    if (telnetServer.hasClient()) {
        if (telnetConnected && telnetClient.connected()) {
            WiFiClient reject = telnetServer.available();
            reject.println("Busy");
            reject.stop();
        } else {
            telnetClient = telnetServer.available();
            telnetConnected = true;
            telnetFirstDraw = true;
            wifiStartTime = millis();
            telnetClient.print("\033[2J\033[H");
            telnetClient.print("  BATTLE BOT v7.2 -- ");
            telnetClient.println(assignedIP);
        }
    }

    if (telnetConnected && !telnetClient.connected()) {
        telnetClient.stop();
        telnetConnected = false;
        telnetFirstDraw = true;
        wifiStartTime = millis();
        setStatus("Telnet disconnected");
    }

    if (telnetConnected) {
        while (telnetClient.available()) telnetClient.read();
    }
}

// ================================================================
//  IBUS PARSER
// ================================================================

void readIBUS() {
    while (Serial2.available()) {
        uint8_t b = Serial2.read();

        if (ibusIndex == 0) {
            if (b == IBUS_LENGTH) {
                ibusBuffer[0] = b;
                ibusIndex = 1;
            }
            continue;
        }

        if (ibusIndex == 1) {
            if (b == IBUS_COMMAND) {
                ibusBuffer[1] = b;
                ibusIndex = 2;
            } else {
                ibusIndex = 0;
                if (b == IBUS_LENGTH) {
                    ibusBuffer[0] = b;
                    ibusIndex = 1;
                }
            }
            continue;
        }

        ibusBuffer[ibusIndex] = b;
        ibusIndex++;

        if (ibusIndex > IBUS_LENGTH) {
            ibusIndex = 0;
            continue;
        }

        if (ibusIndex == IBUS_LENGTH) {
            uint16_t checksum = 0xFFFF;
            for (int i = 0; i < IBUS_LENGTH - 2; i++) {
                checksum -= ibusBuffer[i];
            }
            uint16_t received = ibusBuffer[30] | (ibusBuffer[31] << 8);

            if (checksum == received) {
                for (int i = 0; i < IBUS_CHANNELS; i++) {
                    int offset = 2 + (i * 2);
                    channels[i] = ibusBuffer[offset] | (ibusBuffer[offset + 1] << 8);
                }
                lastIbusFrame = millis();
                ibusFrameReady = true;
                goodFrames++;
            } else {
                badFrames++;
            }
            ibusIndex = 0;
        }
    }
}

// ================================================================
//  SIGNAL VALID
// ================================================================

bool isSignalValid() {
    if (!ibusFrameReady) return false;
    if (channels[IBUS_CH_THROTTLE] < 900 || channels[IBUS_CH_THROTTLE] > 2100) return false;
    if (channels[IBUS_CH_STEERING] < 900 || channels[IBUS_CH_STEERING] > 2100) return false;
    return true;
}

// ================================================================
//  MAIN LOOP
// ================================================================

void loop() {

    esp_task_wdt_reset();

    if (!wifiShutdown) {
        handleWiFi();
        handleTelnet();
    }

    updateDebugState();

    if (otaInProgress) {
        stopAll();
        esp_task_wdt_reset();
        if (wifiActive) webServer.handleClient();
        delay(1);
        return;
    }

    readIBUS();

    bool signalOK = isSignalValid();
    if (signalOK) {
        lastValidSignalTime = millis();
    }

    unsigned long signalAge = millis() - lastValidSignalTime;

    // FAILSAFE
    if (signalAge > FAILSAFE_TIMEOUT_MS) {
        stopAll();
        armLockReleased = false;
        armSeenOff = false;
        weaponLocked = true;

        if (!inFailsafe) {
            inFailsafe = true;
            setStatus("** FAILSAFE ** NO SIGNAL");
        }

        if (debugActive) printDashboard();
        return;
    }

    if (inFailsafe) {
        inFailsafe = false;
        setStatus("Signal recovered -- flip CH5 to arm");
    }

    // ESC HARDWARE ARM (3s 1000us at boot)
    if (!escArmed) {
        if (millis() - bootTime >= ESC_ARM_TIME_MS) {
            escArmed = true;
            setStatus("ESC init done -- system ready");
        } else {
            unsigned long rem = ESC_ARM_TIME_MS - (millis() - bootTime);
            sprintf(statusLine, "ESC init... %lu.%lus",
                    rem / 1000, (rem % 1000) / 100);
            processArmLock();
            stopDrive();
            setESC(1000);
            currentWeaponPWM = 1000;
            if (debugActive) printDashboard();
            return;
        }
    }

    processArmLock();
    processDrive();
    processWeapon();

    if (debugActive) printDashboard();
}

// ================================================================
//  WIFI HANDLER -- 2 MIN TIMEOUT
// ================================================================

void handleWiFi() {
    if (!wifiActive) return;

    webServer.handleClient();

    if (otaInProgress) return;

    if (telnetConnected && telnetClient.connected()) {
        wifiStartTime = millis();
        return;
    }

    if (millis() - wifiStartTime > WIFI_ACTIVE_TIMEOUT_MS) {
        Serial.println("[WIFI] 2-min timeout -- shutting down");
        logWriteLn("WiFi timeout -- shutting down");

        if (telnetConnected) {
            telnetClient.stop();
            telnetConnected = false;
        }
        telnetServer.stop();
        webServer.stop();
        WiFi.disconnect(true);
        delay(100);
        WiFi.mode(WIFI_OFF);
        delay(100);

        wifiActive = false;
        wifiShutdown = true;

        if (DEBUG_MODE) {
            setStatus("WiFi OFF -- USB debug stays (DEBUG_MODE)");
        } else {
            setStatus("WiFi OFF -- max performance");
        }
    }
}

// ================================================================
//  ARM LOCK (CH5 = master arm for drive + weapon)
// ================================================================

void processArmLock() {
    uint16_t armVal = channels[IBUS_CH_ARM];

    if (armVal < 1500) {
        armSeenOff = true;
        if (armLockReleased) {
            armLockReleased = false;
            currentWeaponPWM = 1000;
            targetWeaponPWM = 1000;
            setESC(1000);
            stopDrive();
            setStatus("DISARMED -- all stopped");
        }
    } else {
        if (armSeenOff && !armLockReleased) {
            armLockReleased = true;
            setStatus("*** ARMED *** DRIVE + WEAPON GATE OPEN");
        }
    }
}

// ================================================================
//  DRIVE
// ================================================================

void processDrive() {
    if (!armLockReleased) {
        stopDrive();
        debugThrotRaw = 0;
        debugSteerRaw = 0;
        debugThrotOut = 0;
        debugSteerOut = 0;
        return;
    }

    int throttleRaw = map(channels[IBUS_CH_THROTTLE], 1000, 2000, -255, 255);
    int steeringRaw = map(channels[IBUS_CH_STEERING], 1000, 2000, -255, 255);

    int throttle = applyCurve(throttleRaw, maxThrottle, curveThrottle, deadThrottle);
    int steering = applyCurve(steeringRaw, maxSteering, curveSteering, deadSteering);

    debugThrotRaw = throttleRaw;
    debugSteerRaw = steeringRaw;
    debugThrotOut = throttle;
    debugSteerOut = steering;

    int leftSpd  = constrain(throttle + steering, -255, 255);
    int rightSpd = constrain(throttle - steering, -255, 255);

    debugLeftSpd  = leftSpd;
    debugRightSpd = rightSpd;

    setMotor(leftSpd,  LEDC_CH_L_FWD, LEDC_CH_L_REV, trimLF, trimLR);
    setMotor(rightSpd, LEDC_CH_R_FWD, LEDC_CH_R_REV, trimRF, trimRR);
}

// ================================================================
//  WEAPON
//
//  CH7 (3-pos limit switch):
//    Pos1 (<1300) = LOCKED  -> weapon always 1000us
//    Pos2 (1300-1700) = UNLOCKED, limit 3/6 (50%)
//    Pos3 (>1700) = UNLOCKED, limit 5/6 (83%)
//
//  CH8 override: only works when CH7 is Pos2 or Pos3
//  CH5 must be armed for any weapon output
// ================================================================

void processWeapon() {

    // Read CH7 limit/lock switch
    uint16_t limRaw = channels[IBUS_CH_LIMIT];
    if (limRaw < 1300) {
        weaponLimitPos = 1;
        weaponLocked = true;
    } else if (limRaw < 1700) {
        weaponLimitPos = 2;
        weaponLocked = false;
    } else {
        weaponLimitPos = 3;
        weaponLocked = false;
    }

    // Determine target
    targetWeaponPWM = 1000;

    if (!armLockReleased) {
        // CH5 not armed -- everything off
        targetWeaponPWM = 1000;
    }
    else if (weaponLocked) {
        // CH7 Pos1 -- weapon locked, force 1000us
        targetWeaponPWM = 1000;
    }
    else if (channels[IBUS_CH_OVERRIDE] > 1500) {
        // CH8 override -- 100% (only reaches here if CH7 unlocked)
        targetWeaponPWM = 2000;
    }
    else {
        // Normal operation: knob * limit factor
        float limitFactor;
        if (weaponLimitPos == 2) limitFactor = 3.0f / 6.0f;
        else                     limitFactor = 5.0f / 6.0f;

        int knobRaw = constrain((int)channels[IBUS_CH_KNOB], 1000, 2000);
        float knobFrac = (float)(knobRaw - 1000) / 1000.0f;

        targetWeaponPWM = 1000 + (int)(knobFrac * limitFactor * 1000.0f);
        targetWeaponPWM = constrain(targetWeaponPWM, 1000, 2000);
    }

    // Ramp logic
    prevWeaponPWM = currentWeaponPWM;

    if (targetWeaponPWM <= 1000) {
        // Instant stop -- no ramp down delay for safety
        currentWeaponPWM = 1000;
    }
    else {
        if (millis() - lastRampTime >= RAMP_INTERVAL_MS) {
            lastRampTime = millis();

            uint16_t rampRaw = channels[IBUS_CH_RAMP];
            int rampStep;
            if (rampRaw < 1300)      rampStep = RAMP_SLOW_STEP;
            else if (rampRaw < 1700) rampStep = RAMP_MEDIUM_STEP;
            else                     rampStep = 0;

            if (rampStep == 0) {
                currentWeaponPWM = targetWeaponPWM;
            } else {
                if (currentWeaponPWM < targetWeaponPWM) {
                    currentWeaponPWM += rampStep;
                    if (currentWeaponPWM > targetWeaponPWM)
                        currentWeaponPWM = targetWeaponPWM;
                }
                else if (currentWeaponPWM > targetWeaponPWM) {
                    int downStep = rampStep * 3;
                    if (downStep < 30) downStep = 30;
                    currentWeaponPWM -= downStep;
                    if (currentWeaponPWM < targetWeaponPWM)
                        currentWeaponPWM = targetWeaponPWM;
                }
            }
        }
    }

    currentWeaponPWM = constrain(currentWeaponPWM, 1000, 2000);
    setESC(currentWeaponPWM);

    // Ramp trace
    if (RAMP_TRACE && debugActive) {
        bool isRamping = (currentWeaponPWM != targetWeaponPWM);

        if (isRamping && !wasRamping) {
            rampStartPWM = prevWeaponPWM;
            lastRampPrint = prevWeaponPWM;
            char msg[60];
            sprintf(msg, "RAMP: %d -> %d", prevWeaponPWM, targetWeaponPWM);
            setStatus(msg);
            wasRamping = true;
        }

        if (isRamping && wasRamping) {
            if (abs(currentWeaponPWM - lastRampPrint) >= 100) {
                lastRampPrint = currentWeaponPWM;
                char msg[40];
                sprintf(msg, "RAMPING: %dus (%d%%)",
                        currentWeaponPWM, (currentWeaponPWM - 1000) / 10);
                setStatus(msg);
            }
        }

        if (!isRamping && wasRamping) {
            char msg[60];
            sprintf(msg, "RAMP DONE: %d->%d (%d%%)",
                    rampStartPWM, currentWeaponPWM,
                    (currentWeaponPWM - 1000) / 10);
            setStatus(msg);
            wasRamping = false;
        }
    }
}

// ================================================================
//  MOTOR CONTROL
// ================================================================

void setMotor(int speed, uint8_t fwdCh, uint8_t revCh,
              float tFwd, float tRev) {
    uint32_t fwdVal = 0;
    uint32_t revVal = 0;

    if (speed > 0) {
        fwdVal = (uint32_t)((float)speed * tFwd);
    } else if (speed < 0) {
        revVal = (uint32_t)((float)(-speed) * tRev);
    }

    if (fwdVal > 255) fwdVal = 255;
    if (revVal > 255) revVal = 255;

    ledcWrite(fwdCh, fwdVal);
    ledcWrite(revCh, revVal);

    if (fwdCh == LEDC_CH_L_FWD) {
        debugLeftFwd = fwdVal;
        debugLeftRev = revVal;
    } else if (fwdCh == LEDC_CH_R_FWD) {
        debugRightFwd = fwdVal;
        debugRightRev = revVal;
    }
}

void setESC(int pulseUs) {
    pulseUs = constrain(pulseUs, 1000, 2000);
    uint32_t duty = ((uint32_t)pulseUs * 65536UL + 10000UL) / 20000UL;
    ledcWrite(LEDC_CH_ESC, duty);
}

void stopDrive() {
    ledcWrite(LEDC_CH_L_FWD, 0);
    ledcWrite(LEDC_CH_L_REV, 0);
    ledcWrite(LEDC_CH_R_FWD, 0);
    ledcWrite(LEDC_CH_R_REV, 0);
    debugLeftSpd  = 0;
    debugRightSpd = 0;
    debugLeftFwd  = 0;
    debugLeftRev  = 0;
    debugRightFwd = 0;
    debugRightRev = 0;
}

void stopAll() {
    stopDrive();
    setESC(1000);
    currentWeaponPWM = 1000;
    targetWeaponPWM  = 1000;
}

// ================================================================
//  DASHBOARD -- USB + TELNET
// ================================================================

void printDashboard() {
    if (millis() - lastDebugTime < DEBUG_INTERVAL_MS) return;
    lastDebugTime = millis();

    unsigned long uptime = (millis() - bootTime) / 1000;
    unsigned long sigAge = millis() - lastValidSignalTime;

    int barLen = (currentWeaponPWM - 1000) / 50;
    if (barLen < 0) barLen = 0;
    if (barLen > 20) barLen = 20;
    int pct = (currentWeaponPWM - 1000) / 10;

    char bar[22];
    int i;
    for (i = 0; i < barLen; i++) bar[i] = '#';
    for (; i < 20; i++) bar[i] = '-';
    bar[20] = '\0';

    const char* limLabel;
    if (weaponLimitPos == 1)      limLabel = "LOCKED";
    else if (weaponLimitPos == 2) limLabel = "50%   ";
    else                          limLabel = "83%   ";

    int rssi = 0;
    if (wifiActive && WiFi.status() == WL_CONNECTED) rssi = WiFi.RSSI();

    char line1[80], line2[80], line3[80], line4[80];
    char line5[80], line6[90], line7[80], line8[80];
    char line9[80], line10[90], line11[80];

    sprintf(line1, " CH 1-7 : %4u %4u %4u %4u %4u %4u %4u",
            channels[0], channels[1], channels[2], channels[3],
            channels[4], channels[5], channels[6]);

    sprintf(line2, " CH 8-14: %4u %4u %4u %4u %4u %4u %4u",
            channels[7], channels[8], channels[9], channels[10],
            channels[11], channels[12], channels[13]);

    sprintf(line3, " Steer:%4u Throt:%4u Arm:%4u Knob:%4u",
            channels[IBUS_CH_STEERING], channels[IBUS_CH_THROTTLE],
            channels[IBUS_CH_ARM], channels[IBUS_CH_KNOB]);

    sprintf(line4, " Ramp:%4u Lock:%4u(%s) Ovrd:%4u",
            channels[IBUS_CH_RAMP], channels[IBUS_CH_LIMIT],
            limLabel, channels[IBUS_CH_OVERRIDE]);

    sprintf(line5, " CURVE T:%+4d->%+4d(e%.1f) S:%+4d->%+4d(e%.1f)",
            debugThrotRaw, debugThrotOut, curveThrottle,
            debugSteerRaw, debugSteerOut, curveSteering);

    sprintf(line6, " DRV L:%+4d R:%+4d Arm:%-3s T:%.2f/%.2f/%.2f/%.2f",
            debugLeftSpd, debugRightSpd,
            armLockReleased ? "YES" : "NO",
            trimLF, trimLR, trimRF, trimRR);

    sprintf(line7, " PIN LF:%3u LR:%3u RF:%3u RR:%3u",
            debugLeftFwd, debugLeftRev, debugRightFwd, debugRightRev);

    sprintf(line8, " WPN Tgt:%4d Cur:%4d [%s] %3d%% Lock:%s",
            targetWeaponPWM, currentWeaponPWM, bar, pct,
            weaponLocked ? "YES" : "NO");

    sprintf(line9, "     ESC:%-3s Ramp:%-3s Lim:%s",
            escArmed ? "YES" : "NO",
            (currentWeaponPWM != targetWeaponPWM) ? "YES" : "NO",
            limLabel);

    sprintf(line10, " SYS Sig:%3lums OK:%6lu Bad:%4lu Up:%lus WiFi:%s Heap:%u",
            sigAge, goodFrames, badFrames, uptime,
            wifiActive ? "ON" : "OFF", ESP.getFreeHeap());

    sprintf(line11, " >> %s", statusLine);

    const char* sep  = "================================================================";
    const char* sep2 = "----------------------------------------------------------------";

    logWriteLn(line5);
    logWriteLn(line6);
    logWriteLn(line8);

    usbPos = 0;
    usbLine(sep);
    usbLine(line1);
    usbLine(line2);
    usbLine(sep2);
    usbLine(line3);
    usbLine(line4);
    usbLine(sep2);
    usbLine(line5);
    usbLine(line6);
    usbLine(line7);
    usbLine(sep2);
    usbLine(line8);
    usbLine(line9);
    usbLine(sep2);
    usbLine(line10);
    usbLine(sep);
    usbLine(line11);
    usbLine("");
    usbBuf[usbPos] = '\0';
    Serial.write((uint8_t*)usbBuf, usbPos);

    if (telnetConnected && telnetClient.connected()) {
        telPos = 0;

        if (telnetFirstDraw) {
            telPrint("\033[2J");
            telnetFirstDraw = false;
        }

        telPrint("\033[H");

        char title[80];
        sprintf(title, "  BATTLE BOT v7.2 -- %s | %ddBm",
                assignedIP.toString().c_str(), rssi);

        telPrint(title);  telPrint("\033[K\r\n");
        telPrint(sep);    telPrint("\033[K\r\n");
        telPrint(line1);  telPrint("\033[K\r\n");
        telPrint(line2);  telPrint("\033[K\r\n");
        telPrint(sep2);   telPrint("\033[K\r\n");
        telPrint(line3);  telPrint("\033[K\r\n");
        telPrint(line4);  telPrint("\033[K\r\n");
        telPrint(sep2);   telPrint("\033[K\r\n");
        telPrint(line5);  telPrint("\033[K\r\n");
        telPrint(line6);  telPrint("\033[K\r\n");
        telPrint(line7);  telPrint("\033[K\r\n");
        telPrint(sep2);   telPrint("\033[K\r\n");
        telPrint(line8);  telPrint("\033[K\r\n");
        telPrint(line9);  telPrint("\033[K\r\n");
        telPrint(sep2);   telPrint("\033[K\r\n");
        telPrint(line10); telPrint("\033[K\r\n");
        telPrint(sep);    telPrint("\033[K\r\n");
        telPrint(line11); telPrint("\033[K\r\n");
        telPrint("\033[K\r\n");

        telBuf[telPos] = '\0';
        telnetClient.write((uint8_t*)telBuf, telPos);
    }
}