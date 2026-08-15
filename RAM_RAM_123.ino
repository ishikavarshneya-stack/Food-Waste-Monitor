#include <HX711.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <RTClib.h>
#include <Adafruit_VL53L0X.h>
#include <Preferences.h>
#include <math.h>

// =====================================================
// WI-FI ACCESS POINT
// =====================================================

const char* AP_NAME = "FoodWasteScale";
const char* AP_PASSWORD = "foodscale123";

// =====================================================
// INTERNET WI-FI + GOOGLE SHEETS
// =====================================================

// Replace these four values before uploading.
const char* INTERNET_SSID =
  "GalaxyS25A";

const char* INTERNET_PASSWORD =
  "12345678";

const char* GOOGLE_SCRIPT_URL =
  "https://script.google.com/macros/s/AKfycbw0yJq4By2kSAlXwCSTgDQXNeCQWIIjHfvqKAcHBQNhdrbC00v7oGvtjysivZvqMJ4e/exec";

const char* GOOGLE_TOKEN =
  "foodscale-secret-2026";

const char* DEVICE_NAME =
  "FoodWasteScale01";

const unsigned long WIFI_RETRY_INTERVAL =
  30000UL;

const unsigned long GOOGLE_RETRY_INTERVAL =
  10UL * 60UL * 1000UL;

unsigned long lastWiFiRetryTime = 0;
unsigned long lastGoogleRetryTime = 0;

String lastCloudMessage =
  "No upload attempted yet";

WebServer server(80);

// =====================================================
// RTC + PERSISTENT STATISTICS
// =====================================================

#define I2C_SDA 8
#define I2C_SCL 9

RTC_DS3231 rtc;
Adafruit_VL53L0X tof;
Preferences preferences;

bool rtcAvailable = false;
bool tofAvailable = false;

// Presence detection
const int PERSON_DISTANCE_MM = 900;
const unsigned long PERSON_DEBOUNCE_MS = 60;
const unsigned long PERSON_LEAVE_TIMEOUT_MS = 2200;

bool personPresent = false;
int lastDistanceMm = -1;

unsigned long approachStartTime = 0;
unsigned long lastPersonSeenTime = 0;
bool approachTimerRunning = false;

// =====================================================
// HX711 OBJECTS
// =====================================================

HX711 lc1;
HX711 lc2;
HX711 lc3;
HX711 lc4;

// =====================================================
// ESP32-S3 PIN CONNECTIONS
// =====================================================

#define DT1   5
#define SCK1  4

#define DT2   7
#define SCK2  6

#define DT3   16
#define SCK3  15

#define DT4   18
#define SCK4  17

// =====================================================
// PASSIVE BUZZER
// =====================================================

#define BUZZER_PIN 21

// =====================================================
// PLATFORM CALIBRATION
// =====================================================

const long OFFSET1 = -408892;
const long OFFSET2 = -459848;
const long OFFSET3 = -67721;
const long OFFSET4 = 40680;

const float PLATFORM_FACTOR = 446.60;

const float EVENT_GAIN = 1.0158;
const float EVENT_OFFSET = 0.0;

// =====================================================
// MEAL CONVERSION
// =====================================================

const float GRAMS_PER_MEAL = 350.0;

// =====================================================
// FILTER SETTINGS
// =====================================================

const int NUM_SAMPLES = 5;
const int TRIM_COUNT = 1;

// =====================================================
// STABILITY SETTINGS
// =====================================================

const float STABLE_THRESHOLD = 3.5;
const unsigned long STABLE_TIME = 280;

// =====================================================
// EVENT SETTINGS
// =====================================================

const float EVENT_START_THRESHOLD = 10.0;
const float MINIMUM_EVENT_WEIGHT = 10.0;

const float MOTION_THRESHOLD = 4.5;
const unsigned long EVENT_SETTLE_TIME = 900;

const float EVENT_CANCEL_RANGE = 5.0;
const float REMOVAL_THRESHOLD = 10.0;

const float EMPTY_TOLERANCE = 40.0;

// =====================================================
// FOOD-WASTE FEEDBACK RANGES
// =====================================================
// 0-30 g    : Good
// 31-70 g   : Acceptable
// 71-100 g  : Noticeable
// 101-130 g : Mid
// 131-170 g : High
// 171-200 g : Heavy
// 201-230 g : Too much
// 231-260 g : Very high
// 261-289 g : Severe
// 290 g+    : Extreme

const float WASTE_GOOD_MAX = 30.0;
const float WASTE_ACCEPTABLE_MAX = 70.0;
const float WASTE_NOTICEABLE_MAX = 100.0;
const float WASTE_MID_MAX = 130.0;
const float WASTE_HIGH_MAX = 170.0;
const float WASTE_HEAVY_MAX = 200.0;
const float WASTE_TOO_MUCH_MAX = 230.0;
const float WASTE_VERY_HIGH_MAX = 260.0;
const float WASTE_SEVERE_MAX = 289.999;

// "Low-waste" means Good + Acceptable.
const float LOW_WASTE_MAX = WASTE_ACCEPTABLE_MAX;

// =====================================================
// AUTOMATIC ZERO TRACKING
// =====================================================

const float ZERO_TRACK_RANGE = 5.0;
const unsigned long ZERO_TRACK_DELAY = 10000;
const float ZERO_TRACK_ALPHA = 0.02;
const unsigned long ZERO_TRACK_INTERVAL = 500;

// =====================================================
// SYSTEM STATES
// =====================================================

enum SystemState
{
  FINDING_BASELINE,
  IDLE,
  EVENT_IN_PROGRESS
};

SystemState systemState = FINDING_BASELINE;

// =====================================================
// SCALE VARIABLES
// =====================================================

float startupWeight = 0.0;
float baselineWeight = 0.0;
float currentWeight = 0.0;

float stabilityReference = 0.0;
float lastMotionWeight = 0.0;

float accumulatedFoodWeight = 0.0;
float lastFoodAdded = 0.0;
float lastMealsWasted = 0.0;

unsigned long eventNumber = 0;

unsigned long stabilityStartTime = 0;
unsigned long lastEventMovementTime = 0;

bool stabilityTimerRunning = false;
bool currentReadingStable = false;

// =====================================================
// AUTOMATIC ZERO VARIABLES
// =====================================================

unsigned long zeroTrackStartTime = 0;
unsigned long lastZeroTrackUpdate = 0;
bool zeroTrackTimerRunning = false;


// =====================================================
// DAILY + WEEKLY STATISTICS
// =====================================================

// Current calendar day's totals.
float todayWaste = 0.0;
uint32_t todayEvents = 0;

// A "low waste plate" is one event at or below 70 g.
// This matches the Good + Acceptable ranges and success buzzer.
uint32_t lowWasteEventsToday = 0;

// History index 0 = yesterday.
// History index 1 = two days ago, and so on.
float dailyWasteHistory[7] =
{
  0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0
};

uint32_t dailyEventHistory[7] =
{
  0, 0, 0, 0, 0, 0, 0
};

// Number of days since Unix epoch for the active date.
uint32_t currentDaySerial = 0;

unsigned long lastRTCCheckTime = 0;

float getWeeklyWaste();
uint32_t getWeeklyEvents();
float getWeeklyDailyAverage();

// =====================================================
// GOOGLE SHEETS PENDING-UPLOAD QUEUE
// =====================================================

// Keeps completed days in flash until Google confirms
// that each row has been received.
const int MAX_PENDING_UPLOADS = 14;

String pendingDate[MAX_PENDING_UPLOADS];
float pendingWaste[MAX_PENDING_UPLOADS];
uint32_t pendingEvents[MAX_PENDING_UPLOADS];
float pendingWeeklyWaste[MAX_PENDING_UPLOADS];
uint32_t pendingWeeklyEvents[MAX_PENDING_UPLOADS];
float pendingDailyAverage[MAX_PENDING_UPLOADS];

uint8_t pendingUploadCount = 0;

// =====================================================
// COMMENT / COMPARISON VARIABLES
// =====================================================

String currentComment =
  "Waiting for the next food disposal...";

String currentComparison =
  "Waiting for the next plate...";

String currentBand = "neutral";

// One rotating-comment index per waste band.
uint8_t commentIndexByBand[10] =
{
  0, 0, 0, 0, 0,
  0, 0, 0, 0, 0
};

// =====================================================
// WEB PAGE
// =====================================================

const char MAIN_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>Food Waste Scale — Living Impact</title>
<meta name="ui-version" content="FINAL-HANDMADE-2026-08-10">
<style>
:root{
  --ink:#f6f2e9;
  --ink-dark:#182319;
  --muted:#aab7a8;
  --forest:#102c22;
  --forest2:#183b2c;
  --lime:#c9ef74;
  --cream:#f2eadc;
  --glass:rgba(255,255,255,.085);
  --line:rgba(255,255,255,.13);
  --ease:cubic-bezier(.16,1,.3,1);
}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html,body{width:100%;height:100%;margin:0;overflow:hidden}
body{
  font-family:Inter,-apple-system,BlinkMacSystemFont,"Segoe UI",Arial,sans-serif;
  background:var(--forest);
  color:var(--ink);
}
button{font:inherit}
.app{
  position:relative;width:100%;height:100%;
  background:
    radial-gradient(900px 700px at 92% -5%,rgba(201,239,116,.12),transparent 62%),
    radial-gradient(650px 600px at -8% 105%,rgba(91,160,107,.22),transparent 66%),
    linear-gradient(145deg,#0c261c 0%,#143528 52%,#0e2b20 100%);
  isolation:isolate;
}
.app:before,.app:after{
  content:"";position:absolute;border-radius:50%;filter:blur(1px);pointer-events:none;z-index:-1;
}
.app:before{width:42vw;height:42vw;right:-18vw;top:16vh;border:1px solid rgba(255,255,255,.07)}
.app:after{width:28vw;height:28vw;left:-9vw;top:12vh;border:1px solid rgba(201,239,116,.08)}
.topbar{
  position:absolute;z-index:40;top:0;left:0;width:100%;
  display:flex;align-items:center;justify-content:space-between;
  padding:28px 34px;
  font-size:14px;font-weight:750;letter-spacing:.11em;text-transform:uppercase;
}
.brand{display:flex;align-items:center;gap:10px}
.seed{width:12px;height:18px;border-radius:70% 25% 70% 25%;background:var(--lime);transform:rotate(34deg);box-shadow:0 0 24px rgba(201,239,116,.25)}
.date{opacity:.7;font-weight:650}
.status-dot{display:flex;align-items:center;gap:8px;opacity:.8}
.status-dot i{display:block;width:7px;height:7px;border-radius:50%;background:var(--lime);box-shadow:0 0 0 5px rgba(201,239,116,.08)}

.screen{
  position:absolute;inset:0;
  opacity:0;visibility:hidden;pointer-events:none;
  transform:translateY(22px) scale(.992);
  transition:opacity .18s ease,transform .28s var(--ease),visibility .18s;
}
.screen.active{opacity:1;visibility:visible;pointer-events:auto;transform:none}

/* IDLE */
#idleScreen{display:flex;align-items:center;padding:84px 7vw 52px}
.idle-wrap{width:min(1250px,100%);margin:auto;display:grid;grid-template-columns:minmax(0,1.25fr) minmax(280px,.75fr);gap:5vw;align-items:center}
.eyebrow{font-size:14px;font-weight:800;letter-spacing:.16em;text-transform:uppercase;color:var(--lime);margin-bottom:24px}
.hero{
  margin:0;max-width:960px;
  font-size:clamp(61.5px,8.8vw,133.6px);
  line-height:.91;font-weight:900;letter-spacing:-.03em;
  text-wrap:balance;
}
.hero .accent{color:var(--lime)}
.idle-sub{max-width:690px;margin-top:24px;font-size:clamp(16px,1.48vw,22.3px);line-height:1.45;color:#d7dfd3}
.walk{margin-top:40px;display:flex;align-items:center;gap:14px;font-size:15px;font-weight:700;opacity:.72}
.walk .arrow{font-size:25.4px;animation:floatArrow 1.9s var(--ease) infinite}
.plate-totem{display:flex;justify-content:center;align-items:center;position:relative;min-height:480px}
.big-plate{
  --fraction:62%;
  width:min(31vw,390px);aspect-ratio:1;border-radius:50%;position:relative;
  background:
    radial-gradient(circle at 50% 50%,rgba(255,255,255,.045) 0 56%,transparent 57%),
    conic-gradient(from -90deg,var(--lime) 0 var(--fraction),rgba(255,255,255,.09) var(--fraction) 100%);
  box-shadow:inset 0 0 0 20px rgba(255,255,255,.06),0 40px 100px rgba(0,0,0,.18);
  animation:plateFloat 7s ease-in-out infinite;
}
.big-plate:before{
  content:"";position:absolute;inset:12%;border-radius:50%;
  border:1px solid rgba(255,255,255,.16);background:rgba(9,33,24,.72);
}
.big-plate:after{
  content:"";position:absolute;inset:26%;border-radius:50%;
  background:
    radial-gradient(circle at 32% 30%, #f7ead0 0 10%, #edd6a1 11% 24%, #d2a56a 25% 45%, #a87347 46% 67%, #8a5b37 68% 100%);
  opacity:.97;filter:saturate(.96)
}
.totem-label{
  position:absolute;bottom:17px;padding:12px 18px;border:1px solid var(--line);
  background:rgba(9,31,23,.6);backdrop-filter:blur(15px);border-radius:99px;
  font-size:14px;font-weight:800;letter-spacing:.05em;
}

/* MEASURING */
#measureScreen{padding:96px 34px 30px}
.measure-grid{height:100%;display:grid;grid-template-columns:minmax(0,1.45fr) minmax(310px,.55fr);gap:24px;max-width:1450px;margin:auto}
.weight-stage{
  position:relative;border:1px solid rgba(201,239,116,.18);border-radius:38px;overflow:hidden;
  background:
    radial-gradient(circle at 50% 28%, rgba(201,239,116,.12), transparent 38%),
    linear-gradient(165deg, rgba(12,34,25,.92), rgba(18,47,35,.9) 52%, rgba(15,40,30,.92));
  backdrop-filter:blur(18px);
  box-shadow: inset 0 1px 0 rgba(255,255,255,.05), 0 30px 60px rgba(0,0,0,.18);
  display:flex;align-items:center;justify-content:center;min-height:0;
}
.weight-stage:before{
  content:"";position:absolute;width:64%;aspect-ratio:1;border-radius:50%;
  border:1px solid rgba(201,239,116,.13);animation:breathe 3.8s ease-in-out infinite
}
.weight-stage:after{
  content:"";position:absolute;width:44%;aspect-ratio:1;border-radius:50%;
  border:1px solid rgba(255,255,255,.07);animation:breathe 3.8s ease-in-out infinite reverse
}
.weight-center{position:relative;text-align:center;z-index:2}
.weight-kicker{font-size:14px;font-weight:800;letter-spacing:.17em;text-transform:uppercase;color:#d6ec90;margin-bottom:14px}
.weight-number{font-size:clamp(118.7px,16.96vw,275.6px);font-weight:900;line-height:.76;letter-spacing:-.08em;font-variant-numeric:tabular-nums;transition:transform .25s var(--ease);color:var(--lime);text-shadow:0 8px 30px rgba(201,239,116,.08)}
.weight-unit{margin-top:20px;font-size:23.3px;font-weight:800;letter-spacing:.02em;color:#d8dfcf;opacity:.9}
.weight-note{margin-top:16px;font-size:18px;font-weight:800;color:#eff7df}
.weight-subnote{display:none}
.rail{display:flex;flex-direction:column;justify-content:center;gap:14px;min-height:0}
.metric{
  min-height:180px;border:1px solid rgba(255,255,255,.1);border-radius:30px;padding:28px 28px;
  background:
    linear-gradient(160deg, rgba(14,36,27,.9), rgba(22,53,39,.88));
  backdrop-filter:blur(16px);display:flex;flex-direction:column;justify-content:space-between;
  box-shadow: inset 0 1px 0 rgba(255,255,255,.04), 0 18px 30px rgba(0,0,0,.12);
  transition:transform .5s var(--ease),background .4s ease,border-color .4s ease;
}
.metric:hover{transform:translateY(-4px);background:linear-gradient(160deg, rgba(16,40,29,.96), rgba(26,60,43,.94))}
.metric:nth-child(1){border-color:rgba(201,239,116,.2)}
.metric:nth-child(2){border-color:rgba(126,198,151,.2)}
.metric:nth-child(3){border-color:rgba(255,226,165,.18)}
.metric:nth-child(4){border-color:rgba(201,239,116,.14)}
.metric .m-label{font-size:13px;font-weight:800;letter-spacing:.12em;text-transform:uppercase;color:#a6baaa}
.metric .m-value{font-size:clamp(29.7px,3.39vw,52px);font-weight:850;letter-spacing:-.04em;margin-top:6px;color:#f4f2e8}
.metric .m-foot{font-size:14px;color:#9db09e;margin-top:7px}
.compare-bar{height:8px;background:rgba(255,255,255,.06);border-radius:99px;overflow:hidden;margin-top:13px}
.compare-fill{height:100%;width:65%;background:linear-gradient(90deg,#94d95f,#c9ef74);border-radius:inherit;transition:width .8s var(--ease)}
.plates-mini{display:flex;gap:7px;margin-top:12px;align-items:center;flex-wrap:wrap}
.mini-plate{width:23px;height:23px;border-radius:50%;border:2px solid rgba(255,255,255,.28);position:relative}
.mini-plate.full:after{content:"";position:absolute;inset:4px;border-radius:50%;background:var(--lime)}
.mini-plate.partial:after{content:"";position:absolute;inset:4px;border-radius:50%;background:conic-gradient(var(--lime) 0 var(--mini-frac,50%),transparent var(--mini-frac,50%) 100%)}

/* RESULT */
#resultScreen{display:flex;align-items:center;justify-content:center;padding:85px 5vw 38px}
.result-layout{width:min(1280px,100%);display:grid;grid-template-columns:minmax(330px,.8fr) minmax(0,1.2fr);gap:5vw;align-items:center}
.impact-plate-wrap{position:relative;display:flex;align-items:center;justify-content:center;min-height:520px}
.impact-plate{
  --waste:45%;
  width:min(42vw,560px);
  aspect-ratio:1.08;
  position:relative;
  padding:18px;
  border-radius:34px;
  overflow:hidden;
  background:linear-gradient(160deg,#d7d0c4 0%,#eee7db 45%,#d8d0c3 100%);
  box-shadow:
    0 48px 90px rgba(0,0,0,.3),
    inset 0 0 0 10px rgba(255,255,255,.35),
    inset 0 -10px 18px rgba(0,0,0,.05);
  transform:scale(.82) rotate(-4deg);
  opacity:0;
}
.active .impact-plate{animation:plateArrive .95s var(--ease) .1s forwards}

.tray-surface{
  position:absolute;
  inset:18px;
  border-radius:28px;
  background:
    linear-gradient(160deg, rgba(255,255,255,.46), rgba(217,209,196,.26)),
    linear-gradient(180deg,#f4eee3,#d7cfc2);
  box-shadow:
    inset 0 0 0 8px rgba(255,255,255,.42),
    inset 0 -8px 14px rgba(0,0,0,.05);
  overflow:hidden;
}
.tray-fill{
  position:absolute;
  inset:16px auto 16px 16px;
  width:var(--waste);
  max-width:calc(100% - 32px);
  border-radius:22px;
  overflow:hidden;
  background:linear-gradient(180deg,rgba(245,235,221,.96),rgba(220,209,192,.88));
  box-shadow:
    inset 0 0 22px rgba(120,82,45,.12),
    inset 0 0 0 1px rgba(255,255,255,.08);
  transition:width .95s var(--ease), opacity .4s ease, transform .95s var(--ease);
}
.tray-fill:before,
.tray-fill:after{
  content:"";
  position:absolute;
  inset:0;
  pointer-events:none;
}
.tray-fill:after{
  background:
    linear-gradient(90deg, rgba(255,255,255,.06), transparent 25% 75%, rgba(0,0,0,.03));
  mix-blend-mode:soft-light;
}

.tray-lines{
  position:absolute;
  inset:18px;
  pointer-events:none;
}
.tray-lines .vline,
.tray-lines .hline{
  position:absolute;
  background:rgba(255,255,255,.5);
  box-shadow:0 0 0 1px rgba(0,0,0,.03);
}
.tray-lines .vline{
  width:12px;
  top:18px;
  bottom:18px;
  left:67.5%;
  border-radius:28px;
  transform:translateX(-50%);
  background:linear-gradient(90deg, rgba(255,255,255,.55), rgba(216,209,199,.78), rgba(255,255,255,.42));
}
.tray-lines .hline{
  height:12px;
  right:18px;
  left:67.5%;
  top:50%;
  border-radius:28px;
  transform:translateY(-50%);
  background:linear-gradient(180deg, rgba(255,255,255,.55), rgba(216,209,199,.78), rgba(255,255,255,.42));
}

/* FOOD THEMES — based on your handmade illustration, lightly tuned to match the UI */

/* base food layer */
.tray-fill{
  transition:
    width .95s var(--ease),
    opacity .35s ease,
    transform .82s var(--ease);
  transform-origin:left center;
  background-repeat:no-repeat;
  background-size:cover;
  background-position:center;
  overflow:hidden;
}
.tray-fill:before,
.tray-fill:after{
  content:"";
  position:absolute;
  inset:0;
  pointer-events:none;
}

/* subtle polish so the handmade texture sits nicely inside the tray */
.tray-fill:before{
  background:
    linear-gradient(180deg, rgba(255,255,255,.08), rgba(255,255,255,0) 22%, rgba(0,0,0,.04) 100%);
  mix-blend-mode:soft-light;
  opacity:.95;
}
.tray-fill:after{
  background:
    radial-gradient(120% 90% at 12% 18%, rgba(255,255,255,.08), transparent 40%),
    radial-gradient(90% 70% at 86% 82%, rgba(0,0,0,.05), transparent 45%);
  opacity:.8;
}

/* theme mappings from the handmade board */
.impact-plate[data-theme="ramen"] .tray-fill{
  background-image:url("data:image/jpeg;base64,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");
  background-position:center;
}

.impact-plate[data-theme="rice"] .tray-fill{
  background-image:url("data:image/jpeg;base64,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");
  background-position:center;
}

.impact-plate[data-theme="thali"] .tray-fill{
  background-image:url("data:image/jpeg;base64,/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAkGBwgHBgkIBwgKCgkLDRYPDQwMDRsUFRAWIB0iIiAdHx8kKDQsJCYxJx8fLT0tMTU3Ojo6Iys/RD84QzQ5Ojf/2wBDAQoKCg0MDRoPDxo3JR8lNzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzf/wAARCAF3AfQDASIAAhEBAxEB/8QAHAABAAIDAQEBAAAAAAAAAAAAAAQFAgMGAQcI/8QARhAAAQMCBAMEBggFAgUDBQAAAQACAwQRBRIhMQZBURMiYXEUMkJSgZEHIzNyobHB0RU1YrLhkvAWQ1OC8SQmNiU0g5Oi/8QAGgEAAgMBAQAAAAAAAAAAAAAAAAQBAgMFBv/EADERAAICAQQBAgQFBAIDAAAAAAABAgMRBBIhMUETUSIyYXEFFDM0gSNCkaFSsVPB0f/aAAwDAQACEQMRAD8A7jisWxb/APG39VwHHWKSUWHspYHZZam4Lhu1g3+ey73im/8AGHX1AjblXMYngtBik0MtbCZHRaN7xAI6Ec14y2cIa6UrOkzuOuyzRqFbw2jmvo6w97RUYg8EMc3sor8+p/ILtdljFGyGNscTGsY0Wa1osAFvp4H1EgZHvzPIeaV1Fz1NrkkbaWiOmpUPY9paczucSS2Ngu93QfusZZM7u6MrBo1vQKVWTRxwikpjdo1kf7xUFZ2Yitq/k2hmT3MLCaVkET5ZXhkbGlznHYALNUPHLpBw5P2V7F7A/wC7f/wiiv1LIxb7K32OuqU/YtMOxGkxOn7eilD2A2OliD4jkpS+ZcD18lLjcUDbujq3CJzfE7H/AH1X2KnwkkXqH2/pb+6b1OglXZth0K6LXK+ndLtdlUsO1ZnDM4zHYLpo6Gmj9WJt+p1UOHBIIsQdWZi4l2ZrCNGlTXoV/exqOohzkqNOqLqHQxuHeY0+YUWXDKZ47rSw9WlUloZLplVqF5RQoq/j8y4NgMr4pNZ3iFjhoRff8Aud+juukljq6OR7nNjyyMzG9r6EKHoZql2vwYvXQWoVK8nZIiJEePWgucAOfVbJZAfq49I2/wD9HqVqGiKylhYIaywiIqkhERABERABERABFshhkmfkiaXO/JbXhlIS1jg+a1i4bM8vFaRreMvoq5pPHkw7JsYBnuD/ANNu/wAei8fM4jK2zGe63n5nmtRNzdFDnjiIbfcIiKhYIiIAIiIAIizDLDM/QHYcypSDJ5HG6U2YL9TyHmtuaKH1AJX9SO6PhzWt0hcLDRvuhYKykl0Vw32ZPe6Rxc9xcfFYoiq3kkIiKCQsmMc85WNLj0AU+jwxz7Onu1vujdW0UMcItGwNHgE7Vo5T5lwjCd6XCKWPC6l4uQ1nmVvbg7rd6bXwara+qj1FdBTyZJHHNa+gvZNPS0wWZGPq2SfBAfg7wO5K0+Yso02H1EQvkzDq03V5T1MdQzNGbjmOi2qHpKpLMQ9aafJyh0OunmtjWta3PKDb2W83f4WdaHitkDhdxdoLbrCvjdRMbJUnR+x536LnqqW5pLOBvO5Je5i97pHZnH/HksUBuARsUWTznktjAREUAFjLG2WN8b25mPaWuHUFZIpTw8g0nwzjargKF8xNLWviiJ9R7MxaPA3XWUdNFRUkVNALRxNDWjwW5FvbqrboqM3nAtTpKaZOVccNhERYDWfoW/FLgcXeByjbf5XVQrfikg4u/KNQxod8v2sqhN6/9zP7i+l/Rj9gNwtjpnluQHIz3W6X8+q1olFJro2wERLXNhv+aOyT0AkgAXJ5BbcS4cfiuD1NLJL2L5mWYbXyncXVvh9EKdofILyO/BS5pWQwulkcAxguSei6mm0u3E5did8/UTgumfI+FuCcbh4jp31lMaeGklbK+Vxu19js0je//lfX/Pdc/Q49JWYmyCOFoiebC/rDS91fhdK1ybWRSGj/ACvw+/J6iIsjQKFXYpTUUjGTOOZx2aL28T4KTPK2GJ0j3AAAndcDVzvqqh8zzdzzp4dAtK4qTGtLp/Wk89In/SpSvqeFO1iBcKedkjgBfu2Iv5C4VJ9F2BPloKzEZQ6PtSI4SRo4N1J8rkD4L6TTtHo0bXgG8YBBG+i2NaGtDWgADQACwCiUlKt1vycx6fGoV2ejmJonwyFkjbO/PyWCucdmp6ejL525nXswDe65uimlnkcXWygchzXIu0UoRc10jt07pw3EtERIkhERABERABERABTqHD31PfkOSEczuVlh1E17fSKkhsLdRf2v8LyvxA1A7KHuwDT73+E1CuMI77P4RjKbk9sP8mVXWxsZ6PQjJGPWeN3KuRFjZbKb5LwgooIiLMuEREAEREAF6ASbAEk8gvWMdI8MY0lx0ACmShuHt7NpDqkjvO9weHitIV5WX0VlPDwuzS5jKYWkAdN7vJnn1K0OJcbuNyvPM3KKJyz0CjjsIiKhYIi9DS5wa0XJ2ClLIHrGOe4NYCXHYBXVDQNgs+QB0h+Q8lnQUbaaO7rGQ7np4L519JXFtdDiMmD4dM+njiAE8kZs97iL2vyAB5brr6PR85l2czWaxVRz4PpjphnyNBc8bgbDzK2NvbW118j+inEa7/iB1H2sslLNE98rXuJDSLWdrzvp8V9dTs63B4bMNPerobksGp0JdIXOlksfZBsAqGuikhqH9pc5jcO6ro1pqadlSwskHkeY8QlL6PUjx2OVWbJFDRVBp6hr72adHeSuMPxKnxAyCAuuw63FviFSYpRyUlJM97mkXDWEHclc/DxPScNVOerjll7ZthHFa9r76q2homq3kYvVTplc30fRHRsc8Pc0Fw2NtlScSUNVWzU4gbmjFw43tlPUqBw/x9hmN4hHQshqaeolJEfaAFriOVwdD8F1qZw630JafUrKnW84KCrw91Kxro7ujAAPUFQ11TmhzSHC4O4XNVbGRVD2Rm7Qfl4LkaylRe9eRymxz4ZqRESJuEREAEREAEW6OmnlZmjjJadiiv6c34K74+5Y8UuzYu5tvVjaqhW3FAAxiQi1zGy/nZVKZ1/7mf3MtL+jH7BERJm4VnhFJmPbvGg9W/5qBBEZ5mRD2jZdLEwRxtY3QAWCd0dO57n0he+eFhFdiNaYZA1jbuAuL7DxVHjOJTTU8dO8nU3efe6BdFiFAKqz2HLINL9QqxuFyue4zNaGtG+9/JNb7a7tzWUTRKqOG1yjHAaJ1J9a9t6mQdxh9hvUroB3W94+ZUWJkdBSl8p71rucdT5LKlEkw7ae4vqxnJo8fFMepKUvi7MLZuyTkyUo9dVNpYsxF3HRo6qRsoOJ0j6ljDGRmYToTuFFrkoPZ2Uhjcs9FNUzyVJJlcTfTLyCiYThT6msu8WhjNz/AFdArWLC6h5+sysb53KuKanjpowyMeZ6lL6P1ott9MdlqFXFxh5NtraIiJwQK7FcLbiTos8rmNYTo0DVbP4bTso/R4mBoGoPO/VTURL4o7X0X9SWFHPCOWe10b3McLEGxWKssZgyvbMB63dd5qtXCur9ObiOwlujkxlkbFE+R2oY0uIHQarnsI4wosQqBBNG+mkebML3Atcel+RXRPa17HMeLtcCCPAr5HjmFT4PXSQygiO5MT+Tm8iPFPaCim9SjPvwc38R1F+ncZw68n11FHw5z30FK6W/aGFhdfrYKQudKO14OnF7op+4UzD6L0gmWU5YGes48/Be0FF6Qe0kOWFpu53Ve4hWCa0MHdgZoLaZv8LeuuMY+pP+PqZym5PbExr601DhHGMsLdGtHPxUNEWM5ylLLNIxUVhBERULBERABERABesa57g1gLnE2ACMaXuDWtLnHYDmrmCOPC4DLPYzOFgB+QW9NLny+Eu2Z2WbeF2YNZHhVPnkAdVP2HRVL3Oe4ucbuJuSVnUTPqJTJIbk/ILWi61S+GPSCuDXL7CIiwNAiIgArXCKQ6VEg5dwfqq6niM0zY28z+C6VrQxgaNAAntHUpPe/AvfPC2o01ExZ3WauAvb8vmVxHFH0eS4viPp9HWxxSygekNlaSCfeFvyXcU7MxfK4avdceDeS3rqVylF7kIXVQtjtl0UPCnC9Hw3SuZBeWpkAE1Q4WLrcgOQ8FfItfbM7Xsgbv5gcvNTKWXlkwrjCO2K4NiIiguVfEUD5sLcI2lzg4OsBqbL5XxTgWKVjmYhS0cs1OxmR3ZtJLTc8t19nO1kDQBYaeS2hbsRF/8AV07pfl5Pjn0c4BiE3ElNWz0s0NNSEvc+RhaC62jRfdfYxsvSbnUk+ZRVsm5vJhptOqI7UDsqT+FTvkeXOaBckG9yVdrRVVUdMy8h1OwG5St1cJrM+kOVzlF/Cc45pa4tcLEGxXi31UgqHGdseS5yuF+fL8FoXGnFJ8DsXlchERULBbqWITTtY42bu4+A3Wlb9YYTf1pQPMN/ytK1zl+CsnxhCpndNKXRuLIxoxrTawCLQil2zbzkFBJFnxL/ADuf7rP7VWK04mN8amsNmM166KrW2v8A3M/uZaX9GP2CIiUNzOLEIsPkzyRue5ws0A2VjgmJVFfLMJYmtjZsW669F7htNDUUjmzxMkaX+0FZQwxU8Yjhjaxg2DRou7pVGNC47FrbK8NbeTYlhdEWoqRKuIz1EMZ+zF3u+Gy2ySXlZC3QkZneAW2w1NtbKA574zXVGUFzBZoPQC6z24l92WWXwb4pXS1UjcwDItLDcnmVprcQ9HeY2NzPG9zoFx0VbURVXpTZCZSbu/qXSuj9KniqgLwysDz4WGyNTCyuGYsas0yqknLlFjS1RkawTBkcj9WsB5eSkrn6bPJWNmkuLHOTbYcl0FwBc7blZ0WOcXnwLWQUWYykiJxabEA2KwpJxUQMkta41HQqIagtw6WV51kccgPQrzBHXgkb0dp8VPqp2KPuidnwtlko9XW09G3NPK1o6c/kouO4gaClDo7do42bfl4rm8Ow+oxSpL5C/Je75XfkE3CKayzWmhSjvm8I6eaWKuwx0sJJYW5mki2yo10nYshpDFGLMawgD4Lmxy8lydeluTRahrlLoLCWGKduWaNkjb3s9oIus0SCbTyjdpNcjbyU7DqA1B7SS4hHP3lnh+HmYdrPdsQ1sdC7/CYhX52+j05DYhoSOfgE1CpQj6ln8L3MZTcnsgMRrRIPR6cBsLdDb2v8KuRFhbY7JZZrCCgsIIiLMsEREAEREAF6AXEBouTyCAFxAAuToAugw7D20rBLLYy9Ts1b0UStlhdGdlqrRroaRlBCaioIz21/pHh4qprKl1VO6Rx02aOgUnFa70mTs4z9Uw/6j1VetNRZHHpw6RSqDzvl2EREobhERABERAFngkd5JJCNhlHmrg+KgYM3LSk+84qeu1po7akc+15myox3FjQMbFBbt3i4v7I6qTg1W+tw+OaS2e5DiOZBXI4xKZsUqHv5PLR5DRdTw5GYsJhzbvJd8ynpxSihu6mFdEX5ZNqHva3LGLvcbN6DxK9p4WwMytuSdXOO7j1W3miX2rOWI5C1TTxQi8j2t8ytGJVfo0YDPtHbeHioNHQSVJ7epccp113csp2vdsgss0jBY3SfBYNxGmMMkpkAZH6xOllGjx3D5HFvbZfFwIC143hz6ihyUbGghwJaNMwXH17Th0faV4NOz3pNAm6Ybo/E+RmiiicG5SwfRI5GSsD43tc07Fpus1xHC/EmBwRvZLi0DJJXjKx1xb42su3UThtYnJw3tQlnAXNVsjpamRzjfvEadF0qgQ4XFHNnc4vF7hpGgSepqlbiMTSmcYNtmqCiIwxzHD6x3fA6dFULoq6shoYO1ndYcgNyegXLx1kdRM8Ma5t7kA9FhqdM1BSiuuxmjfLMmuDciKRQ0j6yXI02YNXO6Lnwg5S2o0k1FZZ7RwtdmmmH1Mep/qPILTLI6WR0jt3G/kp2KyRsyUkFuzj1IHXxVctbcQ+Bfz9ykMy+Ji6IixNCw4tlEGJ1kzgSI4WuIHOzbr55gXGj6qtbT4lHGxszrMkj0yk7A+HivofFsQmxSshJ0kiay/S7LL4NPTyU08kErcskTixwPIhehhRVdddGXeTz2r1NunhVKD4wfaUUHApZJ8FoZZiTI6BpcTz0U5cCcdsnH2O/XLfFS9+S8wY3pCNNHlT1VYHJpLHpuCrGeQQwvkPsi67Gnl/RTErF8bPJ546dmeR1hy6lRIMVjkkDDG5t3Wad7qnrpxJSS1EkrnT2s1oGjVnws+KV72yazRi7b9OqiLtsW6HRutOlU5vwdLusHxtcHAgd7fxWaJgU+xxk2CVYrjBHGTGTdsh9UDxXWUtMynpWU7dWtbl15rfZPFXlPcsM3u1E7UlLwaDTNIY1xPZt2b+V1slydm7tCAy2tytFRWwwkgkud7rRcqsnmqq52VkbgwHQcvilLLo19cspGDk+TXX1PpEoDdImeqOvirHCYTDTOfJ3c5zeQsq6nNNDM0zEvIOuX1R+6ta9z3QiKAZnyaeAHVLULLdsuzWzhKC6K/tfTcSZcXjB0HgFdDICGi22gUWkpo6OIucRmt3nnosqQmV7qhwsHaMHRvX4pqlSgvi7ZnNp9dI2VjstLKejSuaC6HFHZaGTq6w+ZVbSYZNUd5942crjU/BLauEpzUYo0pkoxbZBa1z3BrQSTsBzVtT0MVIwVFa4XGoZ/vcr2aWnw1uSma1853ceXmquaaSd+eV5ceV+SxxXR3zL/SL5lb1wiTXV8lSS1t2RD2Rz81CREtOcpvLNYxUVhBERULBEVZxHUVlNhMsmHMc6pzNa3K3MRc6my0qg5yUV5KWTUIOT5wWfkio+EpcTloJTi/a9oJiGdqLOtYX/ABV4purdc3HOcFabPVgppYyFkxjpHBrGlzjsApNJQTVWoGSP3z+iuoaemw+IvuG+8925/wB9FrRpZT+KXCIsuUeFyzRh+HMpgJpyDINfBqiYniRmvDASI/ad7y1YhiL6pxYzuRX25nzUFXu1EYx9OropXU298wiIkhkIiIAIiIAIiIAvcHP/AKMeDipUs8UQvI9rR4lQMHkApJL+w4kqqnldPKZXnUnTwC6n5j06Y4FFVvm/YSUFBJiD55KhzonOzZAz9ei6aMMDG9nbJbS3RcxHG6V7Y2DM52gXSwxiKFrAb5Ra61o1Nl2d3RbUt8Js2IdtET8uqZFStbTGqrXzSi8UZytB52VloBZeC2wFrKLJPM6sbDCBlaM0jj06LJJV8+WWbciUbnZcBxRwVjHEWPS1E2JU7KFthTtdmcWNtqA3a9+fNfQETEJuPKMLaY2rEuji8C+jnCsNnjqamaWuljOZrXgNjv8AdG/xK7TzRFEpuTyya6oVLEFgIsJZBEzO7a4CzVcmhy3EdPW1OIAMhkfE1oEeUaeK1RYZLQta+e2eQbD2V13wVdjMfaRxAEZs9gCd7rPUybpcUOV6qW1V9IqYInzStjjF3O/BXzwzDcPdk3A395yzoKJlJH1kI7zlWY3U9rUCFh7se/mlFWtNU5y+ZmTl608LornXJJJuTqT1XiIuY3kcCIiALTiX+dz/AHWf2rlcW4dw7Fp2z1UbhKLXfG6xeByP+7rquJf53UfcZ/aqtdDV2zr1U3B4eRSmqFmnjGayjGONkUTIo2hrGNDWtGwA2CyWTmOaBmFr7XWKQefI0sY4JFBP2FUx59U6O8iuiNnDqCuVVxhVYHNEMh77fVJ5hP6O5L4JC99efiRtxSlEuGzwxMAcW90Abkaqh4YpJ/Tu3LXNjYCCSLXJ5LrPFF1Yy2xcSkL5QrlD3CIvHeqfJUbwjAjvr6djC8yaXIFtyVvikbLG17DdrhcFcnO8RMc462urbh3EGVVN2Js2WPcdR1CX09lluZNcIZs07jDei3IHNYyMzsc3bMCFmiYxwLHKyMMcjmO3abK9wqUvo2l59W4uei11tFDPLnBcXe01gvdbYqS7GskAEQ1Ebef3jzSNFM67G/AxZYpwRgQ6ukBvamaf/wBh/ZbqysgoYc8zg0DYcz5BbnuZExz3uayNjbuJNg0DquEq+JeHsYxiKEYnO0OcI2EwWYD1zHkepT1VXOX/ACYKypTStlhHQUuP+l18MLKUljnakm5Hj4KXieJ/8qlf954/IKoDWQsMUAyxg783eZXi5+p1ibcax101uSaXAJvr+aIi5uTUIiIAIiIAreIMVGD4ZJVZBI+4YxpNgXHr4LgGcTYxJiEMjqx+sjfq2gBpBNrWXdcWYHW4zhAbRRh0kUgeA5waHDYgE81F4R4LiwqrjxHHp4ZJYu9FSRHPZ3IuI0PkF3NCqIUbp4y/c4Ou/M26lQrztOqp6KoqDdjCG+87QK2pMJiis6U9o7xGg+CjT4092kMYb4u1Kgy1tTL68z7dAbBJRnp6nlfEzquN0++C6rMShphlZaSQaZWnQeapKmplqX5pHXtsOQWhFjdqZ28dI0rpjD7hERLGoREQARERgAi2RQSy/Zxud5BbfRHN+1mhj8C65/BXVcn0irnFeSMik9nStHeqHvP9DP3WJfTNHche49Xv/ZT6eO2Rvz0iRhMzI5JGyOAaW3N9tFzfFPEeH4a0mliLnOvkGb1z1A5BbsRnJk7Jmg5gc1RYjwRi2OVQq6Z8UcWRrLVBLfiLA6Lt6OiDqXq/wY6yNlNLtrWZPoqD9IGNtaW0xpacnTMyEF1vN113/wBHFfjeJ4ZUVeMzGWJ7wKZzmBpcAO8dANL2+RUDAfozoaORs2L1Hpr26iFrS2MHx5n8F3kbGxsaxjWtY0Wa1osAOgTU3XFbYI4umq1Dlvtl/BkSALnYbr4m/jPEDxf/ABaOV5hEvZtpy45eyva1vLW/VfapG543sG7mkfML8/4JgtViXEEWGRscJWy5ZdPs2tPeJ8rKaEsPJXXymnBR9z9Aix1HNYtY1r3OA1cbk9VkNtNuS9S7XJ0VkxeQ1pLnBo63svWkFoINxbQ9VhPCyeMskFwVsAAAA2CjkkIiKQIWLutREc3OAClx37Nt97C60TQGedhf9nH3gOrv8KTss4xe9yLN8JHjnBoJJsBuVz1ZVOqJ84NmtPdCucQJFFMR7pC53mk9bY1iKNqIJ5Z0dTXNhomSn13tGVviudc4ucXONyTclZyyulLcx0a0NA6LWldRe7X9EbVVqARES5qEREAWvEbS/Hp2tF3ERgDr3UfHDhkILg2SqcNLjRqssUYymxKqrZh7LGsHXu/uubmkdLI6R5u5x1XV1uKrpS8t/wChHTZnXFeEjx73SPL3uzOO5KxRFy28jqC9BIIINiOa8RQSW9FigIDKnQ8n9fNWbXBwBBBB5hcqoWOY1NgmET1NPJlktkjB2zHQH4b/AAXS02pnKSg1nInfCMIOfWDp4scw2XF5MJjq2Oro25nRC+nUX2uOisV8L+j4TT8ZUEgkdmD3SSPcdxlN7nxuvuY0aurbBQeEc7S3u+Lk1jk53GsLMdJLKx+YNObKBsL6qpwKV8eKQdnu45XDwK7d7WvYWvALSLEFV9HgtLR1JnizZrWAJuG+SrVtrg4o69WrSqlCf8Fki85LCaQRQukOzQT8lTGRJI1Gsp21QpTK0TEXDFJ/JcLh3aVeLQuJJe6TO4ruC6wV5xURjUU+k1HOTlfpOlq4uE520jHOEkjWTuaLlse5+FwAvlHD2FSYricTGNd2LHB0sltGtB69TyX27EMRGV0UBBJ0c/kqhrGMFmNa0XvYAD8krP8AEI1RcILL9xGf4b61qsk+F4PURFxDs4QREQARFv7JkTQ6e+Y6iP8AU9FaMWyG8GDInPGbRrBu92gCyzxxj6sF7vfdt8AsJJHSEZztsOQWCtuUflIw32ZSSPkN5HF3mdliiKjbZYIiKACIiACIiACWubC5PQLNjLjM85WdevkFkZcndhGQe97R+Kuortlc+x6IQwXneGf0jV3yWXbsj+wiaD77+8f2UdFO/Hy8Btz2bJJ5ZfXkcR0votaIqOTfbJSS6CIigk8DWl4cWgkc7Lqm6tBG1lyy6HDZe1pGG9yBlPwXR0NjbcWLalNpMlIiiYjVejQd37R2jf3T8pKEW2LJNvCJJe0GxcATsCdStcdLTxTyzxQRMlmt2kjWAOfbqeaoKfNJUsc9xNjmc4nYDUlSKbiSCWpdHKwxsJ7ryb/NU09krotpcG0tNLws4LqWRkTC+R7WMGpc4gAfFZNIc0OaQQdiDcFfJfpT4gmqsR/g0LrUlPldIB/zXkX18ACLKX9EuPPFRLgtRIXRuaZaa59Uj1m+VtfgU26Xs3HKWti7/SwfUERFiPBeNGUWuT4lery55IA8ke2Nhc8gAakrGCTtYhJYgO1APRQHxzV01n3bTNda3vWVk0ZRYaALOEnJv2LNJIxmjE0To3bOFlS1WGyQNc9rg5jRck6FXqhYs/LRuHvEBZ6iuEoOT8FqpNPCKFERcUfCIiACIiALviyZzsQEWzWtB8yVSKz4lJONzg8mMt/pVYnNe29TPPuL6VL0YhERJjAREQAVJxdhNRjGHMhpHMEkcmfK82DtLbq7Ra1Wyqmpx7RndVG2DhLplBwpw7/Bo3y1DmvqpBYluzG9B1811EFbPAAGvLmj2XaqMtkMMkzssTC4+A0Wkr7bbN+eWUroqprUIrhFnFi7DpLGW+LdQpTMQpX7Sgfe0VDKwxSlhc1xboS06LBXWssjwwdEHyjpfSICPtWf6lHr5qeWklhM7Gl7C297qi+SK/5+XsQtOk85N2GCnw0F7bzzuGrrZQB0HNZ1NbNUXDnZW+63QKMiwt1VlvzM2cU5bnywiIlywREQAWTGue4NaLlYr25tYbHdSvqQzeXMpz9XaST3zqG+XXzWgkkkkkk7leIplLJCjgIiKpYIiIAIim0mHS1Azu+rZyJ3PwV4Vym8RRWUlFZZCRXzMMpmixaXnq4r1+G0rhozKerSmvyVmDL8xEoFmMrQC6zncm8h5qfNhD296F4d/SdCqY1IbVugc3LY5b+KzWmtTfw9G0GrPlJL3F5u43KxTz0RLvJOAiIoAIiIAIiIAKywWfLKYTs/UearVlG90cjXtPeabhaVWOuakUnHdHB1KpsYbJJVxsY0m7e6B1VpTTNnhbI03uti7NkFbDGeBKEnCWSujwwCilic76yVti4cvALkqvD6mjeWzROHRwFwV36xLQRqt6mqo7Y9G9OrnW35yfA+MKOejxyUVDCztmNkYD7pH+FP+jegqKzieB8Jc1lO10kkg9nSwHxJX1fHuGcLx/sziNOXSRghkjHlrgOl+YW7A8DocBpPRcOiLIycz3ON3vPUnmmJXKUMHCejlLVO19ZyZ9niMfqSxyj+oWKxMuJ7CCPzv/lWNvFeWC5/pe0mdbf9Cv7LEpfXlZGD7u4Uymi7CIMzF/Vx3K2/FFaFai8lXLJ45uZtrkeSh4hUmCNscf2j9G+CkVE7IIzJIbAfiqekL6zEBI8XA7xHQDYLO6zDUI9svCGeX0i3klipaftKiVkUbAMz5HZQPMlVuLVLJooRE9j2OGcOY4EEcrFfLvpLxmqruIJ6CTOylo3ZGRX0cbXLz1v+SlfRy+rcyqa5zjRsADAdg8m5t8Fvq6WtM5ZEdPrVPVeng7REReeO6F64Buhvm5+CzH1YzH1yNB0HVa1ZrBGchERVLFnxL/PKj7sf9qrFZ8S/z2o+7H/aqxOa/wDcz+4vpf0Y/YIvVtjp3PZ2jiGR++7n5dUqoNvCNm0uzSnzW0vY3SJmvvu1J+HJaySdzfzUNJAnkyEZtdzmNHi6/wCC8dlFwCT42ssURlBg9usjLIRYvdbpfRYIhNroMBERQSEREAEREAEREAEREAe30A5BeIiACIiACIiACIp2F0onlL3juMO3Uq9cHOSiis5bVk34bh4OWadvi1p/Mq2tZOVli9wYwucbNAuT0XbrqjXHCEJScnlmSLS2oYacTOJYw63dotoIcAQbg7FXTTKg7FfP4pIvTTLVStiiDy+SRxsGjUkr6DuFx2JcFnE6Opp5qswlzw6JzG3Ghv3gtq3HDUvIxTd6VdjXzY4OHxzjSeWofDg7hHA0kCdze+8dQDsFd8F12KV9LNLiJzwggRSubZzjz8wrXCfo1wyjYX10z66e3dzDLG0/d5/Eqdk7L6vKG5dMoFgEjrpU11+nCPfkR0VWpst9W2X8BERcY7IREQAREQARFIho6ibWOJxHUiwVoxlJ8LJDkl2zZh1X6NJld9k7fwPVXzTcAjZU8eDVDvWcxv4q1o6R1NDkdIXgHS4tZdXSRtitslwJXODeYs2IhFt0TZkYTSNiidI82DRdQaHETUTmN7Q0G5aQs8YBNE63UE+ShYNGX1Jlt3WN/EpSy2aujFG0YR9NyZdoi0yyTM9SEO/7wE03gxNy0VNTFTMLpDryA3KhyzYi+7WQZAdLjVa4sLklf2lVJvuAbk/FLyuk+K4migl8zIsss+ITgBt7bNGzfNT5IxhmGTyM70gaST4qdBDHAzLG0NA/FR8Rqo4YnMIa97hYMOvzUV1xp/qTeWWc9zUYrg+fy4BR4y909a1/aafWsdZx8+qu6OkgoaZlPSxiOJgsGhbgANAAB0CJHU6qdz+nsNuuv1HZGKTYWbQGtzuGvsg8/HyXjGi2d3q9OvgvHOLnXcl+uWHbBJJJJuSvERVZYIiIAsuJP57U/dj/ALVXAE6Ky4ha5+P1DWC7i2MAde6F6xseGx5ngPqHDRvRdHV1b9TNt4WexWie2mKXZpjp2U0Ymqxdx9SHr5qNPPJO/NIfIDYeSxlkfNIXyG7jzWCUnZxtjwjaMecy7CIixNAiIgAiIgAiIgAiIgAiIgAiIgAiIgAiIgAiIgAiIgAuhwyMR0cVuYuVzy6HDXh9HH4Cye0OPUYvqM7USlrnj7WJzL2zCy2Ium1lYYoV+NujhwqeaVwZFAztHHoAvkp+kLG2Vva0krGUrPVpnsBaR/Vzv8V9Q4yoJ8S4YxGkpWl874rsY02LiCDb8F8e4b4XxDH64QRRPggafrqh7CAwfHc+CYorhzY+zma+2/dGuHR9rwHEBi+D0eIZOzNRGHFl72Ox/JWCj0VLFQUcFJTtywwMDGA9AFFlqnTVsUMBuGuu8jmlrLFF8eejpVxk4rJZLn8VAFa/LzsT5q9kkbGwvebAC5XNTyGaZ8h3cbpPXSWxR8jGnXxZMERFyxwIiIAL0WvqLjzsvEQBY0tfTQMFqQdoOY/yt5xs+zB83KnRMLV2xWE8GTog3llo7G5yO7FGPO6xOM1JGjYx8FWoh6q5/wBwejX7E/8Ai9UfWyH/ALU/itT0j/0qAir+Yt/5E+lD2LimxOKVuWqs1x020KnwNiaz6jKGf07LmFsimkhdeN5b+XyTFesa+dZM5UJ/Kzp1UcU47Dw9hL66ZnaOzBkUQNs7zsPLqtIxWpHuf6Vy30iCqxXBIy1he6nmD8sbdbEEHRO06qqdiixPU12wqlKK5RzmA8Z4y/iemnqqySWKombHLT3+ryuNu6OVtF9nkeyK4e4C3XRfFuEOG6mWsir62N0MMLs7GuFnSOG2nQFfQXEuN3Ek9SVfW6uEJ7YcsX/C9PbKtysLWrxUC7abV22cjT4Kqc4vcXOJJO5K8ss443yuyxtL3HkFyLLZ3Pk7EYxguDBSIafNGZpyWwjmN3HoFvFGylHaVh8mA6nwUapqHVDgSA1jdGsGwCPT9PmffsRuc+I9GM0nauvYNaNGtHILWiLFvL5NEsIIUW0ARAOeLv3a0/mUJZBvBjksBme1txex3RY3JJJJuTcop+EMM6bEmRwYpV1shBLWsaBbY5Vzc0r55XSSG7nH5eCvOLJj6WyAbBoc79FQLofiVmbpRXv/ALFdHDFakwiIuaNhERABERABERABEXoaTe2w3KAPEREAEREAEREAEWUcb5TaNpcegUj+H1RAPZHXxCuqpvwQ5JdsiotlTDJSxPlqGmONjcz3nZo6lcBjHG8xldHhTGMiGnbSNu53kNgExTo7bniKFtRrKtPHM3/9O7RcvwbxBU4q6amrQHyxNziVotcXtYhdQsr6JUTcJdmmnvhfWrIdBERYmwU3DKv0eQskNo3fgVEjbmdYmzQLuPQI92ZxNrDp0WlcnBqSKySlwzqAQQCLWO1l6ufpK+Wns09+P3Ty8lZsxOncwkvykC9j+i69WphYu8MSlVKJurKuCki7Sd+UDYcyfBYUFdBXxGSAmwdYgixuuMxGulr6kyP9X2GjkP3XR8PRso8PBmexj3uLi0nUDkm5xjCPL5GLdKq6lJ/MW00XasLC9zQd8ptda2RU9IwuaGsAGrio1RikTARF33cjyVXUVUtQ4do7QbNGwXPt1FceVyzGFU5d9G7EK51UcjLiIbDr4lQ0RcyycpyzIbjFRWEERFQsEREAEREAEREAEREAEREAEW+CkqJvsoXEdbWHzU6HBXkXmlDfBoutoaeyfyopK2Ee2VS9a1zjZrST0Gqv2YRSt1dnd5uspcbIKduVmRg+SZjoJf3vBhLUr+1HPsw+rl1ETh4vNlJiwaV1u0ka0dBqVcdvDf7Rn+oKJU4rBDdrD2jujdvmt/y2ngsyf+zP1rZcJCHCaZgGZpkI5uKyqaymomFrA3PyY39VU1OJ1MwIDuzaeTd/moXnusp6qEFipGkaZS5sZnNK+aR0kpu526wRFz223ljKWFwF6ATYAXJ5BSqXD56mxy5GH2nD9FKkkpcOuyBva1FtXu9lbRoeN0uEZuxZxHlkTsPRmiSoAzn1Iz+ZUZznPcXOJJOpJXssj5Hl8jszjuViqTkuo9F4p9vsIiLPJYs+JHE45UA8mst/pVYrLiP+e1X3Y/7Qq1Oa/wDcz+5hpf0Y/YIiJM3CIiACIiACIplNQOeztZz2UI1JO5V4Vym8IrKSj2aKenfUPs2waPWedmhZ1L47CKnv2TTq47vPVKqdrwIoG5IG7Dm49So91eUlFbYlUnJ7mEReOIa0ucQGgXJOwCyxl8GjeD1ZMa57srGlx6ALln8cYZHXiIwVElMHWdLHYE+QO6+jYVLR1NHFVYeWPglbdj28x+6eh+H29z4QmtdVNtVvLRWRYZUvPeaGDq5aMepZKOmj7AEsdftJOY6DwC6ZYTRMmjdHI0Oa4WIPNO06eut5xkmOokpqT6PnsM0kEgkhe5rwdCCu8w6d1TRQzPFnPaCQq2LhqlZUdo6R74xqIzt8TzV01oa0NaLAaAJuySa4NdXfXbjauTgfpfr6qnwukooWubBUvcZpANLNtZt/M3+C+a4XhGI4tOyLD6SWZznWzBpDR5u2C/Q72NkaWyNa5p3a4AgoxjWNysAa33WiwVoXbY4wcK/Q+tbvlLg4vBeFXcO4eAcssslnTSMGl+g8Apa6ogEWKqcRw4MBmpxpu5n7Lk6uiUm7Ezr6eUa4qGMJFWvQCSABcnYdV4trfq2Z/bd6vgOq50VzyNt4PJCGDsmnxcepWtEUN5BLAS1wQURQSa44IozdjAD81st4IivKyUuW8g232FkxjpHBjGlzjyC8AubD/wAKQypFOzLTeufWlI18h0CIJP5uism10YyximOV1nS8wNm/uVoOpum5JO51KIlJProlLHYREVCQiIgAiIgAi9AJIAFydvFT6bCpZBmnPZM8d1pXVOx4iispxj2V62RQTTfZROd4gaK2azDKQ3c4SOHXvH5I/Go2jLDC4jlfQJhaaEf1JGLtk/liaqfBnkZqh+X+lmp+a3kYbRa2a54/7iq6oxConJBflafZbooil31V/pxz9WHpTlzNlvLjR2hht0Lj+iiSYnVyf8zKOjRZQ0WM9TbLtl40wXg2yVE0gtJK93m5ajrvqiLJyb7ZoopeBYeCIs44pJXZY2lx8EJN8A2kYLOON8rssbHOd0AUtlJBFrW1DWn/AKbDcre/E4oRko4AGjm7QfJbxpSWbHj/ALMnY3xBZNcGEzv1lc2NvPmf2W+2HUHPtpfmf2VZPUTTuvLI4+F7BalZXwr+SP8ALI9OUvmZPq8UmnBYz6uPoDqfioCIl52SseZM1jFRWEgiIAXGwFyeSpgsEW/6pndex0jhuQ6wHgiv6bK7kSuI/wCe1X3Y/wC1VyseI/59V/dj/tVcmdf+5n9zLS/ox+wRESZuERbIoZZjaNjneI2+alRbeEQ2l2a1lGx0kgZG0ucdgFXcW4lHw5h7JHujmrJiWwwA6C27nEch05q2wDH46nA6SqZRtjnljzPA0ANyPO2n4ptaOUYepY8IW/NwlY64cyLKGjgoIu2rCHScm8r9B1VfWVklW+79GD1WdFrqKiSokMkrrnkOQ8lqVLLljZXwv+zSFfO6XYRESxsFGx3B63FOHa1lESJi0FjOcljct+IurOgpTUy96/Zt1cf0Uiox6lppuwY0uy90lo7rV0dDp25eo10L3qVsXXHyfAnxvjkcyRrmPabFjhYtPQhfduBP4e3huliwupFRFEC2STKWntDq64O2/wArKZXYHhOLtbJX0FNUucL9oWWcfiNVKoKKlwymjpaGCOCFt8sbBz6/5XZstU44OHpdHKixvKaJSIiXOiETdQa3EWUxLGDPJ05BVnOMFmRMYuTwicipYMYMZJrLZPeA2VlRVsFZGX0zw4A2OliFFdisjuiWnXOHLRIROarqzEvR6js2sDrDU35onZGCzIiMXJ4RGxSi7I9rELMPrAcioD3Fzi48/wDdlPq8TlloJPR4z29wA0DNcEqCykq4aZstU3Vx293pdIX0Jx9WvpjleVH4+zBERc81CIiAC9aMxte3j0XrGOe4NaLkr15aO6zUc3e9/hSl5IbD3AjKzRo67nzWCIhvIJYCIigkIiIAIi2QxPlcWsYSRqTyHmVKTbwiG0llmtbI4i4BzyI2e87n5Beksj0YA9w9o7fALW5znnM4knqVbCj2Ry+iUyrZTgiliGb/AKj9T8lolnlmN5JHO8ytaKZWyax4BQS58hERZ5LBERABERABetaXGzd14iANwbDHq89o73W7DzPP4L19TI5uRto2e6zRaEV/UeMLgrtXkIiKhYIiIAIizDDa7zlb47/AKUsgzFrXPcGsFyeSwZW0he6KnqoJJho/LICR4Bc7xxDjFVFDDhTJDSFp7ZkTu+830zdRbkFxtPw1jMj2mOgmZc6OfZtvFdSjRVzr3SmkzkanX2127I1to+rD4fEIouFwVNPh1PDVzdtOxgD333Py16X8EXOlBKTSZ1Iybim4l3xH/Pqv7sf9qrlY8R/z6r+7H/aoMTA93edlbzd/vmmdcs6qa+plpn/Rj9jyON8j8kbS53QLY+FsX2srcw9hneI8yspKm0Zipx2cZ3953mVHS72R65Zqtz+hkSL91tvPVeulkcLF7rdAbBYIqbmi2D5vxi6fEeJzRwgudHlijbbmRc/n+C+hUNM2jooKZnqxRhg+AUePCKNmLSYoIyaqRtiSdBpbQdSFO3Tmp1KsrhXHpL/YjpdI6rJ2T7k/9BERIj4WTWl7g1upJsAsVZ4NT53mdw0GjVrTW7J7UUnLbHJY00DaanDByFyepXB1RJqZi5mUl7rt6arssYqHxRNjYbZ9z4Lnp6WOexN2vvq4a3C68NVXTP030TonsbnLyWPCtaXxvpJDfILsPhzCvw0A3/2FXYRhVNRtbNE4yPc31z0UypBlIhBsHauPQf5W9kk3lCl8oSsbh0bmuDhdpBHUL1eMaGgACwGgHRau3BqDC1pJAu48gq59zE2nZctISZX33Lje66oqBU4ZFPIXteWF24A3S2qplYlt8G1Fig+TksSeS9rOQF1acIF3bVAHq5W3817imDhsr6iSaOCjhjzSSuOoA1JIXOcL8d0bcYbh5oxDRzyBsVQXXkvsC8ba+G109RU40KKXJvqtfRCpVt8s+lKgxZrW1rsulwCfNX+xsdFUYvSyPmEsTC8EWNhqElq4t18GFDSnyQ8PcW1sJHM28wuhewSMLXAEEWIVbhlA+NwnmFnW7reniVaKNJW41/F5C+SlLKObrKZ1NMWG5bu09VoXQYlTienOX1295q59Iamr05/QYqnuQXo1NhqV4rnC6ERN9KqALgXaCNh1WdNLtlhFp2KCyyvmYaWPsnfayC7/AAb0UZbJ5TNM+RxvmN/gtaixpvjomCaXIREWZYIiIAIgWxrWsAfLsRdrPe8/BWjHLIbwesi7naSEhh0HV3l+68fM9zMgs1nuN2/yvJJHSOzOPgANgOgWClyS4iQlnlhERULBERABERABEW2GF0t3CzWN9Z7tgpSb6IbS7MGMc+9rAD1nHYLE2vpstkj7jIy/ZtOgPXqtamSS4QLIREVSQiIgAi9a0vNmAnyCz7LL9o9rPC9z8grKLIyjWsmMc8EgDKNydAss0bfVZmPV/wC37rF73PN3m/6IwkGWxdrbFvePUjReEkm5NyvEUNhgIiKCQiIjJGCw4j/n1WP6Y/7VXqw4iH/1+s+7H/aq9Oa/9zP7mGl/RiEREmMBERABERABERAAXJAG52XTUsQhgZGPZCoKGMvrIm+Nz8F0i6WhjhOQrqJc4IVdS+lscGnvt0afFU3otQX5OxfmvbbT5ro4tnHq4rNb2aaNjy+DOF0ocI0UcJhpmRuNyBqVutY3Fr816qDjfHTgGAy1UNjUyERQX5OPtfAAlMwh1FGFliinORfHTzWuCPsw4nV7zmcepXwyh4x4ipZmOjxOom79+ylOcPvytbmvusLnuhjdK0MkLAXtHsm2o+a0sp2YbF9Nqo352rozRNl412docL2PVZjRy/0l9r/wdXdjm9aPtLe5mF18o4UwmoxfHqSmpmkhsjZJXW0YxpuSflYL75PDHPC+KaNskcjS17HC4cDyKh4TguHYNE+PDKSOna83fluS7zJW8LdsceRC/Ru25TzwWB1JPUoU80WI+ERFABc9iUPY1bwNnd4K0rq9lOMre9L7vTzVVGJa6pY17iS87nYDwSGrlGeIR7GKU4/E+iVhNCZXCeVvcGrAfaP7KdjFR2NKYwe9J3R5c1OaAwBoFgBYDoucxWoE9W7Ke6zuj9VpalpqMLtlIZtsyyGiIuQPBERABEU/DaQSH0ibSBmpv7S0rrdkkkVnNRWWa2QtggFRO0Oc77OM8/E+Civc57i5xu46krdWVBqZ3P8AZ2aOgWhTZKPyx6X+yIJ4y+wiIsi4REQAREQAREQAXuYluW5y3vbldeIgAiyjY+R2VjS4+C2ZIo/tHlzvdj1+ZVlBtZIckjVbWyzML7XLco/qNl66cjSICNvRu58ytR115qXtRHLM8rBvID90XTMweqy/i43WCKNxODN0j3CxcbdBoFgiKG2TgIiKACIiACIiACIiALHiMk49VgnZsYH+lVysOIj/AO4K0f0xf2qvTn4h+5n9xfS/ox+wRESYwEREAEREAEREAT8GF6wnowq8KpcEF6h56N/VXS6+jX9ISv8AnMIvVP3is14Ba/mvU2YhQMbweixuhfR4hFnjdq1wNnMd1aeRU9aJ6yCDR7xmHsjUqN6jznBDhvW3GTk8D+jvDMJxOOtfUzVTonZoo5GgNa7kTbchdkBYdVqpp2VMYkZe1+Ystys7HPnJSumNS2xWAomIVbqWJrmNBc421UtR66mFVDkvYg3BWdm7Y9vZrHGeTRh9eal5jkaGuAuCOYU1r2uc5rSCW6HwUQU3ocV6aLPMdM1/xK2UMLoYfrD9Y45nHxWdTmsRnyWntbbibHyETRsG7rk+QW3ksOzb2hk3cRa55Be5TnzZtLbLZZKGSjYhP6PRyyhwblG5UjRePY17C17Q5p3BG6lrKwSsJps4WbEHucSy2ut3akq0wGtrKrEYWykmEX0DAGjRaOKcdwXhcsjNDHUVknebAywsPecTsPzWOBcYx45TyinopKWWKwdchzNehFtVZwrprc1HgYnr6LJejFfEX+K4jlvT07tdnOHLwCpkRcG652yyzautQWAiIsTQIi9a1z3BrBdxNgEJZ4Ak4fRmrlsfs2+uf0U/GZxFEyli0BF3AchyCmU8TMPo+8fVGZx6lc9PK6eZ8r93G/kuhNfl6dv9zFYv1bM+Ea0RFzxoIiIAIiIAIiIAIiIALcImsAdOS2+zB6x/ZYRyGO5aBm5OPLyWLnFxJcSSdyeaunFc9leWbJJi5uRgDI/dbz8+q1IihybfJKSQREVSQiIgAiIgAiIgAiIgAiIgAiIgC34npJI8VlqibxyhgFuRAsqhdVxR9jJf+m3zXKro/ikNuof1FdG80oIiLnDQREQAREQAREAJ2F1KAnYO61YBfdpCvVzVJJ2VTE++gdquluuroXmvAneviyERRsQqfRqcuHrnRvmmpSUYuTMUm3hEXEq8xEwwHv8AtO93/Kp9Xu95zj8So89YxktiS5xPePRXuE0VrVEo+4P1XOlC26a3Lhj7iqIZfZPooRT0zI+YGvmt6IulGKikkc9vLyeEgAk7LQytp3yZGSgu5KJjUsjGMjbcNd6x/RUsglc21P8AaWu34JWzUtWqEUMVUKcctnWXG3NekgL59FWVMUpkjnkDibk5jr5rqsBxV1e18cwAlYL3Gzgn5VtLJe7Rzqju7Rboi1VEzIIi+Q6dOZ8As28LLFDboPihWmm7QszS6Ocb26eC3KE8oGcBi/0dyYtxJUYhU4nalnfnLGsPaNHug7W8VOpsJosGa+kw9jhGHXc57ruc7qSux5Lmq1/aVUrha2awt4JbX2ydajknR6auFjmlyaURFxzpBEXoBJsASfBCA8V5hNAYgJ5x3z6o6BMMw0R2mqB3+TeTf8rLFa/sR2MJ+sI1PuhdKimNMfVs/wAClljseyBGxqsD3ejRnut9cjmeiqkRJXWuye5jEIbI4QRczxJxYzC53UlHEJqho77nOs1h6ablVuD8YYjW4nT0r6aB7ZnhpEYII8d+SZh+H3ShvxwJz/EtPGz085fR3CIiRY+EREYAJzssp2Pp6OarkjcIYWOe9x6AXXyrFeJMSr5HEVT4YXasiidYN6A+Kd02hsv56Qjq9fXpks8tn1NFlhlNVVmGUlTkB7aFkm4vqAVtdR1Ld4X/AAF0vOicXhoahbGaTTNCL1zS02c0g+IXizaa7NMoIiKACIiACIiACIiACIpUFDNKMz7RR+/IbBXjCUukRKSj2yKishT4dEPrakyO6MXj6jD2C0dK53i42WvoY+aSRn6uekyuRS3VUJ9WjiA8SSVrfO12jaeFg8GkqjhFf3f9llKXsaEWTiHG5AHkEVMFs/Q6Pi6TLkjv6xH4LmirXied8uMzRO9WFjA34i5VUnfxKe7Uy+gvpFimIRESAyEQamw1WZa1nrm7vdH6lSlkMmIaXGzQSegWYjaB9ZIG+De8V4ZHEWHdHQLBTwiOWbc8LfViLj1e79AvHyvcLaNb0aLBa0UubaDah5Lo6GYTUzHA3cBZ3gVzisMIqOzmMTvVkPyK30lmyzD6ZldHMePBdqrxyKR8IljAyxhxOqtBuo2IxOmw+ojYLvcwgBdaUFNbWK1vbNM4HV56ly+h0zDHTxsPstA/BcvgmCzSTtnqmGONhuGuGrj+y6sXATFrXCQ5rrYzkox8HqIq3HK6Sgoy+IAvecrb8vFZJZeBKMXOSiu2SqymbVxZCbEG4IF7LXR0DKa7r5nndxH5Kh4exOd1cIJ5HSMlv6x1BXVBUnTFT3Ps1thOl+m3wc3iPDj3zukonMDXG5Y42sfBTcDwg4eHySvDpXCxtsArYjXchR54oQ1z53PLQLm7tFeVstoPUWThsb4PJ62ON2RgMkvuM1WMNPJJKKiqPeHqR8mqDHibYn2jgY2LoBr5q3ikZLGHxm7SlYTja+XnBSUXBdGaImyZMzTVyiGnfIeQ081zXnurPGajM9sDToNXearFydZZunheByiOI5CIttNTyVMojjGvM9EpGLk8I2bSWWYRxvleGRtLnHkFfYdhraa0klnS/g1b6KijpGd3V53cdyvK+tZSR3Ni93qt6/4XVp00aVvs7ErLXY9sTzEK1lLHprIfVb+q5x73SPL3m7nG5PVeyyvlkdJI4uc7clYJLUah2y+gzVUq19QvRoQV4iWNTh63gWomrJJIq6Ls5HlxL2HMLn8Vf4Bw7S4KC9hM1S8WdK4bDoByCuUTlmuvshsb4Eqvw/T12epGPIXoBJAAuTsBzWUUT5nhkbSXHor2hoWUzczrOkO7unks6aJWv6DFlqh9yFS4UXDNUEt/pG6soqaGIAMjaAPC5W5F1K6IQXCFJWSl2R6+mjq6KoppW5mTROY4DmCLL4bw1wtW47iXozYpIqeJ1qiZ7SAwX1Hi49F96TX4JuuzYmkJX6WN0oyfgwhiZDEyKJoaxjQ1oHIDQL2RzWNLnGwGpKysoeKskkpHCMEm4uBzCwsliLaGoRWUiBJib5Zg1rG9mTaxFyVOmwyCXUAxu6tUHDaGSSYSStLWM11HrFXaVog7It2rs3tai8QOfqaCaC7rZ2dW/soi6vRV9dhrJbvhs1/Tk5Y3aPzD/BaF/iRSIsnscxxY8FrhuCsUg1jhjIRFvpaWSqdZmjR6zzsFMYuTwgclFZZpAJNhueSkijLG56p4hadhu4/Bb31EFECyjAfLs6V2vyVe5znuLpHFzjzK1cYV8PlmacpdcIkeksiJFLEGf1v1d+y0SSPkN3vc49SbrFFnKyT4LqKQRF6xj5HBsbS5x5AXVUmy2TxFYQ4TUPsZMsY8dSpIpsOpPt5BI/oTf8AmI6Wb5lwvqZO6K65KbdF0AxKgYMrdAOWQotfytf8A5EU9aX/BlfxH/Pqz7sX9qr1a8UNIxqUk3Do2W020VUq6/wDcz+4aX9GP2CyYwvvbYbk7BZMYC3PIbM5f1eS57i7iU4RFHDTsY6okF2sOrWN949SsqaZWyUY9sm6+FMHOT4R0JcGi0YPi62v+Fr5r5BLW4ji1c2880tRK7IwNcRvyAGwX1mihfT0UEMj87442tc7qQLFMavSfl4xbllvwLaLWrVOSjHCXk3IiJA6AREQAWEs0cDO0mlZG0e052WyzXz/j6PEJcRa58Mhoo2AROAu2/MnxTWkoV9m1vAprNQ9PVvUcn0mn4ywEQt9JxWlZINHDNe/yCn0PEGD4jI2OhxKlmld6rGyd4/Ar89XJFrhWvDuH1tbitL6JRT1PZyte4R93QEbu2HmvRfloxjjJ5+P4lZKeNp+g0S5OrgATqbHREqdhBVXEUUc1EI3uyvzXZpzCsKpz2U8joxd4aSFz0swmiYXkmRmlzs4bpe/UOnrs2oi9ykvBJ4fwlkAbVPeJJCCGgCwb1+KvVBwhjmUgzC2YlwHgpy2hOU4qUuyLpynNtvIUHGA40Zy3sCC7yU5YuDXNIdqDuonHfFx9zOLw0zltjqtkOMNw/MwtdJm1DQbZSsq2n9HqHMGrd2+XRV0uF1Ewknp4y9oOo5/DqkNFWlftlxg6kFVYvj6Lyh4hpqmQRysMLjsXG4Px5KbVVYii7QjTZjT7R6+S4dscjn5Ax2a9rEWsrx0kkgaZXFzg0C/wsndbbGpfD2Uu0kIyTj0eOcXOLnG7ibkrxOduatKHCnSEPqbtZvk5nz6LkV1zteEVnOMFyR6GgkqjfVkfN3XyV9TU0VKzJG23U9VoqK+mo25AbuGgYzkqerxCapJGYsj5Mb+qfTp0y95CrVlz9kW1bikUALYiJJPDYeaoZpXzSGSR2Zx5rBEldqZ2vnoYrqjDrsIiJc1CIiACyijdK8MjF3HZY76K+wykFPFnePrHDXw8FvRS7ZY8Gdk1BGyjpGUsVhq8+s7qpKIu1GKisIQbbeWEutc80cMZfI4ABaaaWSo+sLezjPqt5u81G9Z2+SccZJQUCuxEU0mRjQ94310CmTHJC5w3AJXLlxcc7jcnUlLau51pKPbNqK1N5Z0tJUNqYRI0W5EHkVuVbgebsJL+rn0/VWS3qm5wUmZTSUmkERaK2cU1O6Q7jQDxV5SUVllUsvCMpamCI2kla09CV7FNFM3NE8OHULmSXSPJN3PcfmVf4dTGmpwHes45neCVp1ErZ9cG1lUYLvkV9E2qZcWEg2PXwKoHNcx5Y4WINiF1ShVuHiqkY9pDTezz1H7qNTpt/wAUewqt28PogYdh5qfrJTlhHPm7/C9rq1paaalAZANCW+0ssUqxpSwWbGzQ25+CrUpZONa9OH8s2hFze6QRESjNwiKZSUYfGZ6l3ZwN583eSvCDm8IrKSiuTCjo5Kp9mCzB6ziNArJ9TSYawx07e0l5n9yoNRiD3s7Knb2MI0DRobKEmFbGlYhy/cy2Ss5l17Empr6ioPfeQ33W6BRkRLTnKbzJmyio8IIiKpJccV5f4wcpueybcfNVcbBlL33yjYDdx6K04mtJjb2gi4jZmtuNCVUyPzkaWAFgOgT2uwtTN/UV03NMUHvL3XPwA2C4bibhzFMTxx89O1joHtaGvdIAGADYj5rt0WOn1M6JuUSdTpYaiChPoo+HeG6bBmmRzhNVOFjKRYAdGjkrxEWdt07ZbpvLNKqYUx2QWEERFmahERABP13RFKeAwvJrdTwOdmdBEXdTGCfyW6J7onAxHIRtZGMfI7Kxpc7oBdTGYZPlzSlkTerjqtYK2fRm/Tj3gl0mKMeAyfuO97kVYtIcAQqb0fD4/taovPMMC3Q1VDSn6p87vC5sujVbKKxY1/kVnFP5UWm6gvwundNn7wF7lg2WAxaHMczXhultFtbitIb6vB8Wq7son20VSnHolgACwXqijEaS32o+RWDsUpRs9x8mlaetWv7kRsk/Bqx/GqTAcOfW1ru6NGRj1pHcmj/ei+U1/wBJHEFTITTyw0cZ9VkcYJHmTddtxhQUvE1LDDIZYXwOLo5BY77gjmNB8lyP/ADc4JxI5b/9HX81pXq9NFcy5Ofq9PrJyxWuPuV1HxrjzquIVE4rmlwAhlaNfAEWIX1KTEJTGI4miBg9lupC5nCeGMOwuZs0TXyzt2klN8viBsrr8EhrdVCcl6XH1HtBpbaov13k9JJNydeZKzp4JKiTJE3MefQeakUtBJPZ0n1UXN7v0UyWup6SLsqEBx962nn4paFP91rwh2VjztjyzbFBS4ZH2k7g6Q8+fwCg1mKSz92K8UfQblQ5ZHyvL5HFzjzKwRZqXjbDhBGlJ7pcsfmiIlTYIiIAIiIAIiDew1KEBNwmAzVGYjusF/ir5RsPp/R6drXesdXeakrt6evZWkIWz3SyFi94jYXONgBuslXYk90ssVLGbZ9XHoFpZPbHJSKy8GqBjsRqTLICIWHut6q0e7ILNGuwAXkMTYo2sYLACyzUV17E89kyllnlrtseehVQ7CHmbuyN7InpqPBXCIsphZ8wRnKPRrgibBGI2CwatiItEklhFQotfSmqja1r8pBuL7FSl5a9gDuqyipLDBNp5Rz2K4lhPCsLJ8Rlc6WS4jYxuZ562HTxW3hninD+I4pPQ88c8Wr4ZbBwHUdQvjvGWJz4txHWzTXtHIYo2n2WtNgPwv8AFdL9EWHzyYvUYjYinhhMRcdnPdbQeQF/kmVp4V18HLWvtt1G3wfWl4b2IBsSvUWD6OocvNE+J5bIDcE6nn4rBXeMQ56cSAaxm/wVIuJqK/Tm0P1T3RyERFgaEmjjjJM1R9izcc3HolZVvqngnusb6rBsFpc8ua1uzW7D9VgtXZiO2JRRzLLCIiyLhERABERAFpxMwMxyoc12r2R3ty0tb8lVq24paBjTyAATEy/juqlOfiH7mf3F9L+jEIiJMYCIiACIiACIpNHRS1ZuwWYDq87K8ISm8RREpKKyyOAXEBoJJ2A5qwhw5rGdrXSCNtvVB1W6Salw5pjpgJJ7Wc88v99FVSyPlkL5HFzjzK321098v/RknKzrhFg/Eo4WdnQxBg9525UGWaSbWV7nn+orWiyndOfDfBeNcYhERZZLhERABERGQCItkEXaOJc4MY3VzzsFKTbwiG8cnkUT5ZAyNpc48lPaKSgF32nqOg2ao76oMYYaUGOPm72nqKtlONfXLKNSn3wjfU1U1S68ryRybyC0IiylKUnlsvFKKwgiIqkhERABekEbiy3U8AeHSykthZufePQLXNJ2shdbKNmtHIdFdwwssqpZeEYIiKhYKZhcHbVQJHdZ3j58lDV3g0WSnMhGrz+ATOlhvsX0MrpbYlgiIuyIhQ6dmesnmPI5G+QUxa4GOY12bcuJ/FVay0Sng2IiKxBFr6o00OYAFxNhdQIv4hV2eJCxvW9grOeBkxYZBcMN7ciVHnxKCA5GgvcPd2CVujmWZywjWD4wlybaaCeI/WVHaDoW/qpKrGYuzNZ8TgOt7qyY4PaHNNwditapwksQeSk4yXZor6j0WjmmtcsaSAuJ/iFV6WKp0zjIDffS19rdF2WLxmXDaljRdxjNguRwrDpa+cANIiae+8jTyTlWEm2P6P01XKUjzFfo4o8Uxh2IMrXwU9Q7tJYGsubnfK7lfy0XY4fQU2G0cVHRQtigiFmtH5nqT1UloDQAOWiiU1R2tZUNBu1trLGd3Kizk16euDcoomIiKDU1zsEkT2H2mkLmF1ZK5ioaGVEjQdA4rn66PCYzp/KNaIi5o0EREAEREAEREAEREAWvE7s2NzDM05Y4xYbjQ7qqU7iH/wCRV33Yv7VBTv4h+6n9xfSfoxCIiSGAiIgAiKypaeKmjFRW2vuyPmfgtK63N/QpOaihQ4b2jRNVHJENcpNrjxKzrsSZ2fYUejdi4C2ngolbXS1Zse7H7gP59VFW8rowWyr/ACZxqcnun/gIiJQ3CIiACIiACIiACIlidhfwQgMmML3Brfn0WUrwQGM9Rv4nqsnObG3s4zqR33dT0HgtKu2orCK9vLCIioWCIiACIvWtLnBrQS46ADmhJsMni301OZbuccsTfWkOw8lLZSwUjBJXG7zq2Fv6qNVVb6izbBkY9VjdgmPTjWsz79jLe5cR69zCpm7VwawZYmaMb0HU+K0oixlJyeWaJYWAiIqkhdHQjLTsZe+UALnN10FARedo5P8A0Ce0PEmL6joloixke2Npc8gAbldRihkiqJsYOf6mMFg5u3Km0NWKtrjkLcpsblYxvhOW1MvKuUVlolIiLYoYysEkZYbgEWNjZVceEls4zuDohy2PkrZL3Wc6YzeZItGbj0VP8IvKby/Vk7AaqzhjEMbY23s0WF1miK6oV/KglOUu2F41rWjutAHgF6o1bWR0zNw6Q+qy6tKagstkJN8I14nV+jxZWH6x4sPAdVowRhySSdTZQA2auqL3Jc7c20CvoIm08DWN0AG6UqcrrN76RtJKENvk2rxzg1pcdgLlepodDzTpgcbiuNz1MjmU7zHANBlOrlpoYJWv7STMBbTMdT4rqX0dFSDtWU0QcXAXy8yVX4r/APev+6FlrLkqmoo6Nd8GtkI4IaIi4RYIiIAIiIAIiIAIiIAncQn/ANxVv3Iv7VBU7iH/AOR1v3If7VBTv4j+5n9xbSfoxCIiSGQiKRBlhaJ3gF3/AC2na/U+CtGOWRJ4RvhiZRsbPUNzSHWOL9Soc0sk7y+V2Zx/BeSSPleXvcXOO5KxV52ZW2PCKxhh7n2ERFkXCIiACIiACIiACIiACyByi43OyxRSngAiIoAIiIAINTYankApNHRy1Tu6LM5uOyvqSigpm9xt3c3HcpqjSzt56RjZfGHHkqKTCpprOl+qZ47rdPPT4fmipGB02znnWyzxTErEwU7rHZ7/ANAqbdXsnXT8Na59ysIys5n17GT3ue4ue4ucdyViiJNvJvhLhBERQSERZRRvleGRtLnHYAKUm+gbxyzFXWHH/wBTLY6SMa8fktUOCvIvLKG+DRdWlNSx00QY25tzO66Wl09kXmSwKXWxksIy53VLjNQXyiAHut1cOpV1zUSow6Col7R+YE72O6ZvhOcNsDOqUYyzIooonzyBkTbk/guhoqcU0DWA3O7j1KzggjgbliYGjn4rYqafTqrl9k22ufHgclzvF3FVLw1AztGGeqlBMUDXW0HtE8h+a6LTmdOa+B8cS1knFWJHELiZsuVrb3DWD1QPC1k9TBSfJzdbfKmvMe2fWODOK4uJqaYmD0epgI7WMOzNsdiD8/JX8lQyM2kDh45TZfOvobpSIcTrbmznMhGnQEn8wvpSrdHbJqJppJysqUpkU4hSNNjMPkVrkxWmbo3M/wAgpjo2O9ZrT5hOzZ7rfksGrPDX+BtbfKKqSuq6kEU8RaNrjU/NIcLlkdnqX2vuAbk/FW4FtAiz/LqTzN5LepjiKwYQwxwsDI2hoC8cwvdr6oNwOpWw7KPX1TKKkfPJ7I0HU8gmFFdIost48m9xDQSTYBOYVFgWLPrpH09UAX6vaRtbp8Ffc/FTJOL5LWVyrltl2RMRF4G+ErSPmqrFDetf4AD8FbYgfq42D2pWj8VTVzs1ZMf6rLnax8NfY2o7yR0RFzRoIiIAIiIAIiIAIiIAmY/rxLiHgyH+xQ0RO/iP7qf3F9J+hEIiJIYNsEYeXOf6kbczrbnwWEjnPcXH4Ach0RFrLiK+pRfMYoiLIuEREAEREAEREAEREAEREAEREAEREAFZ4ZhonAlmPc5NHNETejrjOzEjDUScY8F4xjY2hrAABsAFV4niRYXQQEh2zndPJEXQ1dkoVfDwK0RUp8lIiIuIdEIiIAIiIAkUVK+slysIAGrieS6GlpIqZlom+ZO5RF19FXFQ345ENROW7BlUVMdOzPIbdLDdUlRikkszSy7ImkHKNyiKurtmpKCeEy2nri8tl01wc0OGxFwvUROLoxCIikDw8lxHG3ArsdqjiGGShlc7K2RkrrMcALXva4KIrQk4y4Mrqo2x2yOi4ZwWDAMJioYXZ3C75ZLWzvO58lbcyiKJNttsvGKglFdBERQWCIiACouLc3oUQaO72l3fJEV6/mRvpf1o/cj8K0Tg51a492xYwfmul8URFr5YaqTla2yurJM2IQR8mHOVTPcXOc47kkoi4uqbcn9//SNalg8REShuEREAEREAEREAEREAf//Z");
  background-position:center;
}

/* gentle reveal */
.active #foodDisc{
  animation:foodSlideIn .72s cubic-bezier(.22,1,.36,1) both;
}
.active #foodDisc:before,
.active #foodDisc:after{
  animation:foodTextureSettle .85s cubic-bezier(.22,1,.36,1) both;
}

@keyframes foodSlideIn{
  0%{opacity:0;transform:translateX(-14px)}
  100%{opacity:1;transform:translateX(0)}
}
@keyframes foodTextureSettle{
  0%{opacity:0;transform:translateY(-6px)}
  100%{opacity:1;transform:translateY(0)}
}

.waste-tag{
  position:absolute;left:50%;bottom:-14px;transform:translateX(-50%);
  background:#172e22;color:var(--ink);border:1px solid rgba(255,255,255,.16);
  padding:11px 17px;border-radius:999px;font-size:14px;font-weight:850;white-space:nowrap;
}
.result-copy{padding-right:2vw}
.result-overline{font-size:14px;font-weight:850;letter-spacing:.16em;text-transform:uppercase;color:var(--lime);margin-bottom:18px}
.result-weight{font-size:clamp(101.8px,12.72vw,193px);line-height:.82;font-weight:900;letter-spacing:-.03em}
.result-weight small{font-size:.18em;letter-spacing:0;opacity:.65;margin-left:10px}


.result-caption-rotator{display:none}
.result-spotlight{
  margin-top:22px;display:inline-flex;align-items:center;gap:14px;
  padding:16px 18px;border-radius:22px;border:1px solid rgba(201,239,116,.18);
  background:linear-gradient(145deg, rgba(255,255,255,.09), rgba(255,255,255,.04));
  box-shadow:0 20px 40px rgba(0,0,0,.12)
}
.result-spotlight .big{
  font-size:clamp(36px,4.77vw,68px);font-weight:900;letter-spacing:-.06em;line-height:.88;color:var(--lime)
}
.result-spotlight .small{
  max-width:250px;font-size:15px;line-height:1.4;color:#d5ded1;font-weight:700
}
.result-plates{
  margin-top:18px;display:flex;gap:10px;align-items:center;flex-wrap:wrap
}
.result-plate-mini{
  width:34px;height:34px;border-radius:50%;border:2px solid rgba(255,255,255,.28);position:relative;box-shadow:inset 0 0 0 3px rgba(255,255,255,.04)
}
.result-plate-mini.full:after{content:"";position:absolute;inset:5px;border-radius:50%;background:#e4c277}
.result-plate-mini.partial:after{content:"";position:absolute;inset:5px;border-radius:50%;background:conic-gradient(#e4c277 0 var(--rfrac,50%), transparent var(--rfrac,50%) 100%)}




.result-band{
  margin-top:20px;
  display:inline-block;
  padding:10px 15px;
  border:1px solid rgba(255,255,255,.13);
  border-radius:999px;
  background:rgba(255,255,255,.055);
  font-size:clamp(17px,1.8vw,24.4px);
  font-weight:900;
  letter-spacing:.03em;
  text-transform:uppercase;
  color:var(--lime)
}


.result-community{margin-top:14px;padding-top:16px;border-top:1px solid rgba(255,255,255,.08);font-size:15px;line-height:1.45;color:#aebcab;max-width:710px}
.result-community strong{color:#dce59d;font-weight:800}
.result-hint{margin-top:25px;font-size:13px;letter-spacing:.08em;text-transform:uppercase;opacity:.43}
.drift{
  position:absolute;width:9px;height:14px;border-radius:70% 25% 70% 25%;background:var(--lime);opacity:0;pointer-events:none;
}
.active .drift{animation:drift 4s ease-out forwards}
.d1{left:10%;top:62%;animation-delay:.35s!important}.d2{left:22%;top:26%;animation-delay:.5s!important}.d3{right:12%;top:30%;animation-delay:.7s!important}.d4{right:20%;bottom:15%;animation-delay:.85s!important}



.result-copy{
  position:relative;
  min-height:520px;
}
.result-personal-phase,
.result-impact-phase{
  transition:opacity .20s ease,transform .28s var(--ease);
}
.result-personal-phase{
  opacity:1;
  transform:translateY(0);
}
.result-impact-phase{
  position:absolute;
  inset:0;
  display:flex;
  flex-direction:column;
  justify-content:center;
  opacity:0;
  pointer-events:none;
  transform:translateY(26px);
}
#resultScreen.impact-phase .result-personal-phase{
  opacity:0;
  transform:translateY(-22px);
  pointer-events:none;
}
#resultScreen.impact-phase .result-impact-phase{
  opacity:1;
  transform:translateY(0);
}

.journey-wrap{
  margin-top:34px;
  max-width:560px;
}
.journey-label{
  margin-bottom:15px;
  font-size:12px;
  font-weight:850;
  letter-spacing:.03em;
  color:#aebbac;
}
.food-journey{
  display:grid;
  grid-template-columns:76px 54px 76px 66px 138px;
  align-items:center;
  gap:8px;
  width:max-content;
  max-width:100%;
}
.journey-step{
  display:flex;
  flex-direction:column;
  align-items:center;
  gap:9px;
  min-width:0;
  font-size:13px;
  font-weight:900;
  letter-spacing:.03em;
  color:#cbd5c8;
  transition:transform .28s var(--ease),background .28s ease,border-color .28s ease,color .28s ease;
}
.journey-dot{
  width:20px;
  height:20px;
  border-radius:50%;
  border:2px solid rgba(255,255,255,.28);
  background:rgba(255,255,255,.06);
}
.journey-line{
  position:relative;
  height:2px;
  background:rgba(255,255,255,.16);
  overflow:hidden;
  color:var(--journey-color,var(--lime));
}
.journey-line:after{
  content:"";
  position:absolute;
  top:50%;
  left:-12px;
  width:10px;
  height:10px;
  border-radius:50%;
  background:currentColor;
  transform:translateY(-50%);
  opacity:0;
}
#resultScreen.active:not(.impact-phase) .line-one:after{
  animation:journeyTravel 1.0s var(--ease) .35s forwards;
}
#resultScreen.active:not(.impact-phase) .line-two:after{
  animation:journeyTravel 1.0s var(--ease) 1.45s forwards;
}
.journey-step.outcome{
  min-width:138px;
  min-height:78px;
  justify-content:center;
  padding:11px 14px;
  border:1px solid rgba(255,255,255,.10);
  border-radius:22px;
  background:rgba(255,255,255,.035);
  color:var(--journey-color,var(--lime));
  transform-origin:center;
}
.journey-step.outcome span:last-child{
  font-size:15px;
  line-height:1;
  transition:font-size .28s var(--ease),letter-spacing .28s ease;
}
.journey-step.outcome .journey-dot{
  border-color:var(--journey-color,var(--lime));
}
.journey-step.outcome.arrived{
  transform:scale(1.16);
  border-color:var(--journey-color,var(--lime));
  background:rgba(255,255,255,.075);
  box-shadow:0 18px 48px rgba(0,0,0,.17);
}
.journey-step.outcome.arrived span:last-child{
  font-size:22.3px;
  letter-spacing:.02em;
}
.result-action{
  margin-top:34px;
  max-width:620px;
  font-size:clamp(24.4px,2.65vw,36px);
  line-height:1.12;
  font-weight:800;
  letter-spacing:-.03em;
  color:#edf2e6;
}

/* SECOND RESULT PHASE: COLLECTIVE IMPACT */
.impact-lead{
  margin-top:20px;
  max-width:760px;
  min-height:2.4em;
  font-size:clamp(25.4px,2.86vw,40.3px);
  line-height:1.18;
  font-weight:760;
  color:#e6ebdf;
}
.impact-lead .run-word{
  display:inline-block;
  white-space:nowrap;
  margin-right:.28em;
}
.impact-lead .run-char{
  display:inline-block;
  opacity:0;
  transform:translateY(7px);
  filter:blur(3px);
  animation:impactCharIn .58s var(--ease) forwards;
}
.impact-equation{
  margin-top:24px;
  display:flex;
  align-items:baseline;
  gap:18px;
  min-height:150px;
  flex-wrap:wrap;
}
.impact-major{
  display:inline-block;
  font-size:clamp(134px,15.9vw,231px);
  line-height:.72;
  font-weight:950;
  letter-spacing:-.04em;
  color:var(--lime);
  opacity:0;
  transform:scale(.72) translateY(14px);
}

.impact-four{
  color:#f3f6e9;
}

/* Sentence finishes at roughly 3 seconds.
   Then the impact appears in sequence. */

#resultScreen.impact-phase #impactNumber{
  animation:impactNumberPop .68s cubic-bezier(.2,1.35,.35,1) 3.15s forwards;
}

.impact-words{
  font-size:clamp(25px,3.07vw,44.5px);
  line-height:1;
  font-weight:780;
  letter-spacing:-.025em;
  color:#dfe6da;

  opacity:0;
  transform:translateY(8px);
}

#resultScreen.impact-phase .impact-words{
  animation:impactWordsIn .38s var(--ease) 3.48s forwards;
}

#resultScreen.impact-phase .impact-four{
  animation:impactNumberPop .68s cubic-bezier(.2,1.35,.35,1) 3.72s forwards;
}
.impact-words{
  font-size:clamp(25.4px,3.07vw,44.5px);
  line-height:1;
  font-weight:780;
  letter-spacing:-.025em;
  color:#dfe6da;
}
.family-mark{
  margin-top:28px;
  display:flex;
  gap:14px;
  align-items:flex-end;
  opacity:0;
  transform:translateY(10px);
  transition:opacity .35s ease,transform .4s var(--ease);
}
#resultScreen.impact-phase .family-mark{
  opacity:1;
  transform:translateY(0);
  transition-delay:4.25s;
}
.person-mark{
  position:relative;
  display:block;
  width:34px;
  height:52px;
  border-radius:18px 18px 12px 12px;
  border:2px solid rgba(201,239,116,.72);
  border-top-width:0;
}
.person-mark:before{
  content:"";
  position:absolute;
  width:18px;
  height:18px;
  border-radius:50%;
  border:2px solid rgba(201,239,116,.82);
  top:-16px;
  left:6px;
  background:rgba(201,239,116,.06);
}
.person-mark:nth-child(2),
.person-mark:nth-child(4){
  height:44px;
  width:30px;
}
.impact-close{
  margin-top:28px;
  max-width:680px;
  font-size:clamp(19px,2.12vw,28.6px);
  line-height:1.2;
  font-weight:730;
  color:#bdc9ba;
  opacity:0;
  transform:translateY(8px);
  transition:opacity .35s ease 4.65s,transform .4s var(--ease) 4.65s;
}
#resultScreen.impact-phase .impact-close{
  opacity:1;
  transform:translateY(0);
}
@keyframes impactCharIn{
  0%{
    opacity:0;
    transform:translateY(7px);
    filter:blur(3px);
  }
  55%{
    opacity:.78;
    filter:blur(.8px);
  }
  100%{
    opacity:1;
    transform:translateY(0);
    filter:blur(0);
  }
}
@keyframes impactNumberPop{
  0%{
    opacity:0;
    transform:scale(.78) translateY(14px);
  }
  62%{
    opacity:1;
    transform:scale(1.08) translateY(-3px);
  }
  100%{
    opacity:1;
    transform:scale(1) translateY(0);
  }
}
@keyframes impactWordsIn{
  from{
    opacity:0;
    transform:translateY(8px);
  }
  to{
    opacity:1;
    transform:translateY(0);
  }
}
@keyframes journeyTravel{
  0%{left:-12px;opacity:0}
  12%{opacity:.9}
  88%{opacity:.9}
  100%{left:calc(100% - 2px);opacity:0}
}

/* admin */
.admin{
  position:absolute;
  z-index:80;
  right:24px;
  bottom:22px;
  display:flex;
  flex-direction:column;
  gap:8px;
}
.admin button{
  border:1px solid rgba(201,239,116,.24);
  background:rgba(11,35,26,.82);
  color:#eaf5d7;
  padding:10px 14px;
  border-radius:14px;
  font-size:12px;
  font-weight:800;
  letter-spacing:.03em;
  box-shadow:0 10px 28px rgba(0,0,0,.16);
  transition:transform .25s var(--ease),background .25s ease,opacity .25s ease;
  opacity:.78;
}
.admin button:hover{
  transform:translateY(-2px);
  background:rgba(18,52,38,.92);
  opacity:1;
}
.admin button:disabled{
  opacity:.5;
  cursor:wait;
}
.admin .reset-btn{
  border-color:rgba(255,255,255,.14);
  background:rgba(20,32,27,.76);
  color:#d7ded6;
}
#plateFractionLabel{
  display:block!important;
  font-size:13px;
  font-weight:900;
  letter-spacing:.03em;
  text-transform:none;
}
.hidden-data{display:none}

@keyframes floatArrow{0%,100%{transform:translateY(0)}50%{transform:translateY(8px)}}
@keyframes plateFloat{0%,100%{transform:translateY(0) rotate(-3deg)}50%{transform:translateY(-13px) rotate(2deg)}}
@keyframes breathe{0%,100%{transform:scale(.96);opacity:.55}50%{transform:scale(1.04);opacity:1}}
@keyframes plateArrive{to{transform:scale(1) rotate(0);opacity:1}}
@keyframes drift{0%{transform:translate(0,25px) rotate(0);opacity:0}15%{opacity:.8}100%{transform:translate(55px,-120px) rotate(180deg);opacity:0}}

@media(max-width:1050px){
  .food-journey{transform:scale(.92);transform-origin:left center}
  .journey-wrap{max-width:520px}
}
@media(max-width:850px){
  .idle-wrap,.measure-grid,.result-layout{grid-template-columns:1fr}
  .plate-totem{display:none}
  #measureScreen{overflow:auto}
  .measure-grid{height:auto}.weight-stage{min-height:55vh}
  .rail{display:grid;grid-template-columns:1fr 1fr}
  .result-layout{text-align:center}.impact-plate-wrap{min-height:330px}
  .impact-plate{width:min(65vw,360px)}
  .result-copy{padding:0}.result-community{margin-left:auto;margin-right:auto}
}
@media(prefers-reduced-motion:reduce){
  *,*:before,*:after{animation:none!important;transition:none!important}
}
</style>
</head>
<body>
<div class="app">
  <div class="topbar">
    <div class="brand"><span class="seed"></span><span>Food Waste Scale</span></div>
    <div class="date" id="rtcDate">10 AUG 2026</div>
    <div class="status-dot"><i></i><span id="connectionText">Live</span></div>
  </div>

  <section class="screen active" id="idleScreen">
    <div class="idle-wrap">
      <div>
        <div class="eyebrow">Today, in this mess hall</div>
        <h1 class="hero" id="impactTitle">Today’s leftovers equal about <span class="accent">5 plates.</span></h1>
        <div class="idle-sub" id="idleSub">Food waste, shown in a way people can feel.</div>
        <div class="walk"><span class="arrow">↓</span><span>Walk up to measure</span></div>
      </div>
      <div class="plate-totem">
        <div class="big-plate" id="idlePlate"></div>
        <div class="totem-label" id="lowWasteLine">8 in 10 plates kept waste low today</div>
      </div>
    </div>
  </section>

  <section class="screen" id="measureScreen">
    <div class="measure-grid">
      <div class="weight-stage">
        <div class="weight-center">
          <div class="weight-kicker">Food being measured</div>
          <div class="weight-number" id="currentWeight">0</div>
          <div class="weight-unit">grams</div>
        </div>
      </div>

      <aside class="rail">
        <div class="metric">
          <div class="m-label">Food wasted today</div>
          <div class="m-value"><span id="todayWaste">0</span> g</div>
        </div>

        <div class="metric">
          <div class="m-label">Low waste plates today</div>
          <div class="m-value" id="lowWasteMetric">0 / 0</div>
          <div class="compare-bar"><div class="compare-fill" id="lowWasteBar"></div></div>
        </div>
      </aside>
    </div>
  </section>

  <section class="screen" id="resultScreen">
    <span class="drift d1"></span><span class="drift d2"></span><span class="drift d3"></span><span class="drift d4"></span>
    <div class="result-layout">
      <div class="impact-plate-wrap">
        <div class="impact-plate" id="impactPlate" data-theme="ramen">
          <div class="tray-surface"></div>
          <div class="tray-fill" id="foodDisc"></div>
          <div class="tray-lines">
            <div class="vline"></div>
            <div class="hline"></div>
          </div>
          <div class="waste-tag" id="plateFractionLabel">about half a plate</div>
        </div>
      </div>
      <div class="result-copy">
        <div class="result-personal-phase" id="resultPersonalPhase">
          <div class="result-overline">YOUR RESULT</div>

          <div class="result-weight"><span id="resultWeight">176</span><small>g</small></div>
          <div class="result-band" id="resultBand">A LOT WASTED</div>

          <div class="journey-wrap">
            <div class="journey-label">WHERE THE FOOD ENDED UP</div>

            <div class="food-journey">
              <div class="journey-step">
                <span class="journey-dot"></span>
                <span>COOKED</span>
              </div>

              <div class="journey-line line-one"></div>

              <div class="journey-step">
                <span class="journey-dot"></span>
                <span>SERVED</span>
              </div>

              <div class="journey-line line-two"></div>

              <div class="journey-step outcome" id="journeyOutcome">
                <span class="journey-dot"></span>
                <span id="journeyOutcomeText">BIN</span>
              </div>
            </div>
          </div>

          <div class="result-action" id="resultAction">
            Take less first. Seconds are always there.
          </div>
        </div>

        <div class="result-impact-phase" id="resultImpactPhase">
          <div class="result-overline">COLLECTIVE IMPACT TODAY</div>

          <div class="impact-lead" id="impactLead">
            Together, today's wasted food could have fed
          </div>

          <div class="impact-equation" id="impactEquation">
            <span class="impact-major" id="impactNumber">1</span>
            <span class="impact-words" id="impactWords">family of</span>
            <span class="impact-major impact-four" id="impactFour">4</span>
          </div>

          <div class="family-mark" id="familyMark" aria-hidden="true">
            <span class="person-mark"></span>
            <span class="person-mark"></span>
            <span class="person-mark"></span>
            <span class="person-mark"></span>
          </div>

          <div class="impact-close">Food meant for people should stay on plates.</div>
        </div>
      </div>
    </div>
  </section>

  <!-- Existing IDs retained for ESP32 compatibility / diagnostics -->
  <div class="hidden-data">
    <span id="lastEvent"></span><span id="lastMeals"></span><span id="sessionTotal"></span>
    <span id="weeklyEvents"></span><span id="systemState"></span><span id="eventNumber"></span>
    <span id="foodComparison"></span><span id="wasteComment"></span><span id="cloudStatus"></span>
    <span id="pendingUploads"></span><div id="eventOverlay"></div><div id="particles"></div>
    <div id="resultCard"></div><span id="resultComment"></span><span id="resultComparison"></span><span id="resultEmoji"></span>
    <div id="screensaver"></div><div id="toast"></div>
  </div>

  <div class="admin">
    <button id="googleExportBtn" onclick="exportToSheets()">Export to Sheets</button>
    <button id="resetSessionBtn" class="reset-btn" onclick="resetSession()">Reset Session</button>
  </div>
</div>

<script>
const GRAMS_PER_PLATE = 350;
const RESULT_PERSONAL_MS = 5400;
const RESULT_MS = 12400;

let lastSeenEventNumber = -1;
let resultTimer = null;
let resultPhaseTimer = null;
let journeyArrivalTimer = null;
let impactTextTimer = null;
let resultCaptionTimer = null;
let foodThemeIndex = 0;
let displayWeightSmoothed = 0;
let displayWeightReady = false;
const FOOD_THEMES = ["ramen","rice","thali"];
let demoMode = location.protocol === "file:";
let demoData = {
  currentWeight: 0, lastEvent: 0, mealsWasted: 0, sessionTotal: 0,
  todayWaste: 2450, todayEvents: 36, weeklyWaste: 16780, weeklyEvents: 241,
  dailyAverage: 2397, rtcDate: "10 AUG 2026", personPresent: false,
  lowWasteToday: 29, lowWastePercent: 80.6, eventNumber: 0, state:"IDLE"
};

const $ = id => document.getElementById(id);
const screens = {
  idle:$("idleScreen"), measure:$("measureScreen"), result:$("resultScreen")
};

let currentScreen = "idle";
let idleRotationIndex = -1;

const idleHeadlineBuilders = [
  d => `<span class="accent">${formatNumber(d.lowWastePercent,0)}%</span> of plates kept waste low today.`,
  d => {
    const people=Math.floor((Number(d.todayWaste)||0)/GRAMS_PER_PLATE);
    if(people >= 8){
      const families=Math.floor(people/4);
      return `Today’s wasted food could have served <span class="accent">${families} ${families===1?"family":"families"} of four.</span>`;
    }
    if(people > 0){
      return `Today’s wasted food could have served <span class="accent">${people} ${people===1?"person":"people"}.</span>`;
    }
    return `The next clean plate starts <span class="accent">right here.</span>`;
  },
  d => `Food was cooked for people, <span class="accent">not for the bin.</span>`
];

function getWasteBand(grams){
  if (grams <= 70){
    return {
      name:"WASTE KEPT LOW",
      color:"#c9ef74",
      outcome:"MOSTLY EATEN",
      action:"Keep this habit."
    };
  }

  if (grams <= 130){
    return {
      name:"SOME FOOD WASTED",
      color:"#eadf78",
      outcome:"LEFT OVER",
      action:"Take a little less next time."
    };
  }

  if (grams <= 200){
    return {
      name:"A LOT WASTED",
      color:"#f0ba61",
      outcome:"BIN",
      action:"Take less first. Seconds are always there."
    };
  }

  return {
    name:"TOO MUCH WASTED",
    color:"#ed765b",
    outcome:"BIN",
    action:"Start smaller. Come back for more."
  };
}

function formatMealFraction(grams){
  const f = Math.max(0, grams / GRAMS_PER_PLATE);
  if (grams <= 0) return "No food yet";
  if (f < .08) return "Just a few bites";
  if (f < .18) return "A small patch of the plate";
  if (f < .32) return "About one quarter of a plate";
  if (f < .46) return "About one third of a plate";
  if (f < .7)  return "About half a plate";
  if (f < .95) return "Most of a plate";
  return "Almost a full plate";
}

function showScreen(name){
  if(name !== "result"){
    clearTimeout(resultPhaseTimer);
    clearTimeout(journeyArrivalTimer);
    clearTimeout(impactTextTimer);
    screens.result.classList.remove("impact-phase");
    if($("journeyOutcome")) $("journeyOutcome").classList.remove("arrived");
  }

  if (name === "idle" && currentScreen !== "idle"){
    idleRotationIndex = (idleRotationIndex + 1) % idleHeadlineBuilders.length;
    updateIdle(demoMode ? demoData : (window.lastStatusData || demoData));
  }

  Object.values(screens).forEach(s=>s.classList.remove("active"));
  screens[name].classList.add("active");
  currentScreen = name;
}

function formatNumber(n,dec=0){
  return Number(n||0).toLocaleString(undefined,{maximumFractionDigits:dec,minimumFractionDigits:dec});
}

function humanPlatePhrase(grams){
  return getWasteBand(grams).human;
}

function fractionLabel(grams){
  const f = Math.max(0, grams / GRAMS_PER_PLATE);
  if (f < .08) return "a few bites";
  if (f < .18) return "about 1/10 plate";
  if (f < .32) return "about 1/4 plate";
  if (f < .46) return "about 1/3 plate";
  if (f < .7)  return "about 1/2 plate";
  if (f < .95) return "most of 1 plate";
  return "almost 1 full plate";
}

function spoonfulText(grams){
  const spoons = Math.max(1, Math.round(grams / 26));
  if (grams <= 0) return "No food yet";
  if (spoons <= 2) return `About ${spoons} spoonful${spoons > 1 ? "s" : ""}`;
  return `That’s about ${spoons} spoonfuls`;
}

function resultImpactText(totalGrams){
  const people = Math.floor(Math.max(0,totalGrams) / GRAMS_PER_PLATE);

  if(people < 1){
    return `Today’s total is still under one full serving.`;
  }

  // For larger totals, a family comparison is easier to picture.
  if(people >= 8){
    const families = Math.floor(people / 4);
    return `Today’s waste could have fed about <strong>${families} ${families === 1 ? "family" : "families"} of 4.</strong>`;
  }

  return `Today’s waste could have fed about <strong>${people} ${people === 1 ? "person" : "people"}.</strong>`;
}

function buildFamilyImpact(totalGrams){
  const servings=Math.floor(Math.max(0,Number(totalGrams)||0)/GRAMS_PER_PLATE);

  if(servings < 1){
    return {
      number:"<1",
      words:"full serving",
      four:"",
      showFamily:false
    };
  }

  if(servings < 4){
    return {
      number:String(servings),
      words:servings===1 ? "person" : "people",
      four:"",
      showFamily:false
    };
  }

  const families=Math.floor(servings/4);

  return {
    number:String(families),
    words:families===1 ? "family of" : "families of",
    four:"4",
    showFamily:true
  };
}

function runImpactSentence(){
  const el=$("impactLead");
  if(!el) return;

  const words=["Together,","today's","wasted","food","could","have","fed"];
  el.innerHTML="";

  let charIndex=0;

  words.forEach(word=>{
    const wordWrap=document.createElement("span");
    wordWrap.className="run-word";

    [...word].forEach(char=>{
      const span=document.createElement("span");
      span.className="run-char";
      span.textContent=char;
      span.style.animationDelay=`${charIndex*48}ms`;
      wordWrap.appendChild(span);
      charIndex++;
    });

    el.appendChild(wordWrap);
    charIndex+=2;
  });
}

function nextFoodTheme(){
  const theme = FOOD_THEMES[foodThemeIndex % FOOD_THEMES.length];
  foodThemeIndex++;
  return theme;
}

function startResultCaptionRotation(lines){
  
  const el = $("resultHuman");
  if(!el || !lines || !lines.length){
    return;
  }
  let idx = 0;
  el.textContent = lines[0];
  resultCaptionTimer = setInterval(() => {
    idx = (idx + 1) % lines.length;
    el.style.opacity = "0";
    el.style.transform = "translateY(8px)";
    setTimeout(() => {
      el.textContent = lines[idx];
      el.style.opacity = "1";
      el.style.transform = "translateY(0)";
    }, 180);
  }, 4200);
}

function buildMiniPlates(totalGrams){
  const el=$("miniPlates"); el.innerHTML="";
  let p=Math.max(0,totalGrams/GRAMS_PER_PLATE);
  const full=Math.min(8,Math.floor(p));
  for(let i=0;i<full;i++){
    const d=document.createElement("span");d.className="mini-plate full";el.appendChild(d);
  }
  const rem=p-full;
  if(rem>.04 && full<8){
    const d=document.createElement("span");d.className="mini-plate partial";
    d.style.setProperty("--mini-frac",(rem*100)+"%"); el.appendChild(d);
  }
}

function buildResultPlates(totalGrams){
  const el = $("resultPlates");
  if(!el) return;
  el.innerHTML = "";
  let p = Math.max(0,totalGrams/GRAMS_PER_PLATE);
  const full = Math.min(10, Math.floor(p));
  for(let i=0;i<full;i++){
    const d=document.createElement("span");
    d.className="result-plate-mini full";
    el.appendChild(d);
  }
  const rem = p-full;
  if(rem>.04 && full<10){
    const d=document.createElement("span");
    d.className="result-plate-mini partial";
    d.style.setProperty("--rfrac",(rem*100)+"%");
    el.appendChild(d);
  }
}

function updateIdle(d){
  const eq=(Number(d.todayWaste)||0)/GRAMS_PER_PLATE;
  const pct=Number(d.lowWastePercent)||0;
  const avg = (Number(d.todayWaste)||0)/Math.max(1,Number(d.todayEvents)||1);

  const idx = idleRotationIndex < 0 ? 0 : idleRotationIndex;
  let headline;
  if ((Number(d.todayEvents)||0) === 0){
    headline = `The next clean plate starts <span class="accent">right here.</span>`;
    $("idleSub").textContent="Walk up to measure";
  } else {
    headline = idleHeadlineBuilders[idx % idleHeadlineBuilders.length](d);
    $("idleSub").textContent =
      idx % 3 === 0 ? `${formatNumber(d.todayEvents,0)} plates checked so far`
      : idx % 3 === 1 ? `Average waste today: ${formatNumber(avg,0)} g`
      : `Today’s pile is at ${formatNumber(eq,1)} plates of food`;
  }
  $("impactTitle").innerHTML = headline;
  $("lowWasteLine").textContent = d.todayEvents ? `${formatNumber(d.lowWasteToday)} of ${formatNumber(d.todayEvents)} plates stayed low waste` : "No plates measured yet";
  $("idlePlate").style.setProperty("--fraction",Math.min(100,Math.max(6,pct))+"%");
}

function updateMeasuring(d){
  const raw=Math.max(0,Number(d.currentWeight)||0);

  if(!displayWeightReady || Math.abs(raw-displayWeightSmoothed) > 35){
    displayWeightSmoothed=raw;
    displayWeightReady=true;
  }else{
    displayWeightSmoothed=displayWeightSmoothed*0.72 + raw*0.28;
  }

  if(raw < 3) displayWeightSmoothed=0;

  const w=Math.max(0,Math.round(displayWeightSmoothed));
  $("currentWeight").textContent=formatNumber(w,0);
  $("todayWaste").textContent=formatNumber(d.todayWaste,0);
  $("lowWasteMetric").textContent=`${formatNumber(d.lowWasteToday,0)} / ${formatNumber(d.todayEvents,0)}`;
  $("lowWasteBar").style.width=Math.max(2,Math.min(100,Number(d.lowWastePercent)||0))+"%";
}

function showResult(grams,d){
  grams=Math.max(0,Number(grams)||0);
  const frac=Math.min(1,grams/GRAMS_PER_PLATE);
  const pct=Math.max(4,Math.min(100,frac*100));
  const band=getWasteBand(grams);
  const theme=nextFoodTheme();

  clearTimeout(resultTimer);
  clearTimeout(resultPhaseTimer);
  clearTimeout(journeyArrivalTimer);

  screens.result.classList.remove("impact-phase");
  $("journeyOutcome").classList.remove("arrived");

  $("impactPlate").dataset.theme=theme;
  $("resultWeight").textContent=formatNumber(grams,0);
  $("foodDisc").style.width=`calc((100% - 32px) * ${pct/100})`;
  $("impactPlate").style.setProperty("--waste",pct+"%");
  $("plateFractionLabel").textContent=formatMealFraction(grams);

  $("resultBand").textContent=band.name;
  $("resultBand").style.color=band.color;

  $("journeyOutcomeText").textContent=band.outcome;
  $("journeyOutcome").style.setProperty("--journey-color",band.color);

  document.querySelectorAll(".journey-line").forEach(el=>{
    el.style.setProperty("--journey-color",band.color);
  });

  $("resultAction").textContent=band.action;

  const impact=buildFamilyImpact(Number(d.todayWaste)||grams);
  $("impactNumber").textContent=impact.number;
  $("impactWords").textContent=impact.words;
  $("impactFour").textContent=impact.four;
  $("familyMark").style.display=impact.showFamily ? "flex" : "none";

  showScreen("result");

  journeyArrivalTimer=setTimeout(()=>{
    $("journeyOutcome").classList.add("arrived");
  },2450);

  resultPhaseTimer=setTimeout(()=>{
    screens.result.classList.add("impact-phase");
    runImpactSentence();
  },RESULT_PERSONAL_MS);

  resultTimer=setTimeout(()=>{
    showScreen(d.personPresent ? "measure" : "idle");
  },RESULT_MS);
}

function applyStatus(d){
  window.lastStatusData = d;
  $("rtcDate").textContent=(d.rtcDate||"").toUpperCase();
  $("connectionText").textContent=d.personPresent ? "Measuring" : "Ready";
  updateIdle(d); updateMeasuring(d);

  // Existing compatibility IDs
  ["lastEvent","sessionTotal","weeklyEvents","eventNumber","cloudStatus","pendingUploads"].forEach(id=>{
    const key={lastEvent:"lastEvent",sessionTotal:"sessionTotal",weeklyEvents:"weeklyEvents",eventNumber:"eventNumber",cloudStatus:"cloudStatus",pendingUploads:"pendingUploads"}[id];
    if($(id)) $(id).textContent=d[key] ?? "";
  });
  $("systemState").textContent=d.state||"";
  $("foodComparison").textContent=d.comparison||"";
  $("wasteComment").textContent=d.comment||"";

  const ev=Number(d.eventNumber)||0;
  if(lastSeenEventNumber<0) lastSeenEventNumber=ev;
  if(ev>lastSeenEventNumber){
    lastSeenEventNumber=ev;
    showResult(Number(d.lastEvent)||0,d);
    return;
  }

  if(!screens.result.classList.contains("active")){
    showScreen(d.personPresent ? "measure" : "idle");
  }
}

async function poll(){
  if(demoMode) return;
  try{
    const r=await fetch("/status",{cache:"no-store"});
    if(!r.ok) throw new Error("status");
    const d=await r.json();
    applyStatus(d);
  }catch(e){
    $("connectionText").textContent="Reconnecting";
  }
}

async function exportToSheets(){
  const btn = $("googleExportBtn");
  if(!btn) return;

  const original = btn.textContent;
  btn.disabled = true;
  btn.textContent = "Exporting...";

  try{
    const r = await fetch("/export-sheets",{
      method:"POST",
      cache:"no-store"
    });

    const msg = await r.text();

    if(r.ok){
      btn.textContent = "Exported ✓";
    }else{
      btn.textContent = "Export failed";
      console.log("Sheets export:", msg);
    }
  }catch(e){
    btn.textContent = "Export failed";
    console.log("Sheets export error:", e);
  }

  setTimeout(()=>{
    btn.textContent = original;
    btn.disabled = false;
  },2500);
}

async function resetSession(){
  const btn = $("resetSessionBtn");
  if(!btn) return;

  const original = btn.textContent;
  btn.disabled = true;
  btn.textContent = "Resetting...";

  try{
    const r = await fetch("/reset",{
      method:"POST",
      cache:"no-store"
    });

    const msg = await r.text();

    if(r.ok){
      btn.textContent = "Reset ✓";
    }else{
      btn.textContent = "Reset failed";
      console.log("Reset session:", msg);
    }
  }catch(e){
    btn.textContent = "Reset failed";
    console.log("Reset session error:", e);
  }

  setTimeout(()=>{
    btn.textContent = original;
    btn.disabled = false;
  },2200);
}

function demoState(which){
  demoMode=true;
  if(which==="idle"){demoData.personPresent=false;updateIdle(demoData);showScreen("idle");}
  else{
    demoData.personPresent=true;
    demoData.currentWeight=126;
    updateMeasuring(demoData);showScreen("measure");
  }
}
function demoResult(g){
  demoMode=true;
  demoData.lastEvent=g;
  demoData.todayWaste=1840+g;
  demoData.todayEvents=37;
  demoData.lowWasteToday= g<=70 ? 30 : 29;
  demoData.lowWastePercent=demoData.lowWasteToday/demoData.todayEvents*100;
  updateIdle(demoData);updateMeasuring(demoData);showResult(g,demoData);
}

idleRotationIndex = 0;
$("foodDisc").style.width = "calc((100% - 32px) * 0.62)";

if(demoMode){
  applyStatus(demoData);
  showScreen("idle");
}else{
  poll();
}

setInterval(poll,180);
</script>
</body>
</html>
<style>
#idleSub,
.result-hint {
  display: none !important;
}
#resultSpoonfuls {
  display: block !important;
}
</style>

)rawliteral";

// =====================================================
// NON-BLOCKING HX711 ROLLING FILTER
// =====================================================

bool readPlatformWeight(float &weight)
{
  static float rolling[NUM_SAMPLES] = {0,0,0,0,0};
  static uint8_t rollingCount = 0;
  static uint8_t rollingIndex = 0;
  static unsigned long lastGoodSampleTime = 0;

  if (!(lc1.is_ready() && lc2.is_ready() && lc3.is_ready() && lc4.is_ready()))
  {
    if (lastGoodSampleTime > 0 && millis() - lastGoodSampleTime > 2500)
    {
      Serial.println("ERROR: HX711 data timeout.");
      return false;
    }

    return true;
  }

  long raw1 = lc1.read();
  long raw2 = lc2.read();
  long raw3 = lc3.read();
  long raw4 = lc4.read();

  long long totalCounts = 0;
  totalCounts += (long long)raw1 - OFFSET1;
  totalCounts += (long long)raw2 - OFFSET2;
  totalCounts += (long long)raw3 - OFFSET3;
  totalCounts += (long long)raw4 - OFFSET4;

  float instantWeight = (float)totalCounts / PLATFORM_FACTOR;

  rolling[rollingIndex] = instantWeight;
  rollingIndex = (rollingIndex + 1) % NUM_SAMPLES;
  if (rollingCount < NUM_SAMPLES) rollingCount++;
  lastGoodSampleTime = millis();

  if (rollingCount < NUM_SAMPLES)
  {
    float sum = 0.0;
    for (int i = 0; i < rollingCount; i++) sum += rolling[i];
    weight = sum / (float)rollingCount;
  }
  else
  {
    float sorted[NUM_SAMPLES];
    for (int i = 0; i < NUM_SAMPLES; i++) sorted[i] = rolling[i];

    for (int i = 0; i < NUM_SAMPLES - 1; i++)
    {
      for (int j = i + 1; j < NUM_SAMPLES; j++)
      {
        if (sorted[i] > sorted[j])
        {
          float t = sorted[i];
          sorted[i] = sorted[j];
          sorted[j] = t;
        }
      }
    }

    float sum = 0.0;
    for (int i = TRIM_COUNT; i < NUM_SAMPLES - TRIM_COUNT; i++) sum += sorted[i];
    weight = sum / (float)(NUM_SAMPLES - 2 * TRIM_COUNT);
  }

  if (weight > -2.5 && weight < 2.5) weight = 0.0;

  return true;
}

// =====================================================
// EVENT-WEIGHT CORRECTION
// =====================================================

float correctEventWeight(float measuredFood)
{
  float correctedFood =
    (measuredFood * EVENT_GAIN) +
    EVENT_OFFSET;

  if (correctedFood < 0.0)
  {
    correctedFood = 0.0;
  }

  return correctedFood;
}

// =====================================================
// STABILITY DETECTOR
// =====================================================

bool updateStability(float weight)
{
  float difference =
    fabs(
      weight -
      stabilityReference
    );

  if (difference <= STABLE_THRESHOLD)
  {
    if (!stabilityTimerRunning)
    {
      stabilityStartTime = millis();
      stabilityTimerRunning = true;
    }

    if (
      millis() -
      stabilityStartTime >=
      STABLE_TIME
    )
    {
      currentReadingStable = true;
    }
  }
  else
  {
    stabilityReference = weight;
    stabilityStartTime = millis();

    stabilityTimerRunning = true;
    currentReadingStable = false;
  }

  return currentReadingStable;
}

// =====================================================
// RESET AUTOMATIC ZERO TIMER
// =====================================================

void resetZeroTracking()
{
  zeroTrackTimerRunning = false;
  zeroTrackStartTime = 0;
}

// =====================================================
// AUTOMATIC ZERO TRACKING
// =====================================================

void updateAutomaticZero(bool stable)
{
  if (systemState != IDLE)
  {
    resetZeroTracking();
    return;
  }

  float baselineDifference =
    currentWeight -
    baselineWeight;

  bool eligible =
    stable &&
    fabs(baselineDifference) <=
    ZERO_TRACK_RANGE;

  if (!eligible)
  {
    resetZeroTracking();
    return;
  }

  if (!zeroTrackTimerRunning)
  {
    zeroTrackStartTime = millis();
    lastZeroTrackUpdate = millis();

    zeroTrackTimerRunning = true;
    return;
  }

  if (
    millis() -
    zeroTrackStartTime <
    ZERO_TRACK_DELAY
  )
  {
    return;
  }

  if (
    millis() -
    lastZeroTrackUpdate >=
    ZERO_TRACK_INTERVAL
  )
  {
    baselineWeight =
      baselineWeight *
      (1.0 - ZERO_TRACK_ALPHA) +
      currentWeight *
      ZERO_TRACK_ALPHA;

    if (
      eventNumber == 0 &&
      accumulatedFoodWeight < 1.0
    )
    {
      startupWeight =
        startupWeight *
        (1.0 - ZERO_TRACK_ALPHA) +
        currentWeight *
        ZERO_TRACK_ALPHA;
    }

    lastZeroTrackUpdate = millis();
  }
}

// =====================================================
// NON-BLOCKING BUZZER TUNES
// =====================================================

struct BuzzerNote
{
  uint16_t frequency;
  uint16_t duration;
};

const BuzzerNote SUCCESS_NOTES[] =
{
  {523,100},{659,110},{784,130},{1175,110},{1319,130},{1760,360}
};

const BuzzerNote MIDDLE_NOTES[] =
{
  {1319,100},{1175,110},{1047,120},{988,130},{1047,280}
};

const BuzzerNote DISAPPOINTING_NOTES[] =
{
  {1760,80},{1568,90},{1319,100},{1175,110},{988,130},{784,360}
};

const BuzzerNote* activeTune = nullptr;
uint8_t activeTuneLength = 0;
uint8_t activeTuneIndex = 0;
unsigned long buzzerStepStart = 0;
bool buzzerGap = false;

void startBuzzerTune(const BuzzerNote* notes, uint8_t noteCount)
{
  activeTune = notes;
  activeTuneLength = noteCount;
  activeTuneIndex = 0;
  buzzerGap = false;
  buzzerStepStart = millis();

  if (activeTuneLength > 0)
  {
    ledcWriteTone(BUZZER_PIN, activeTune[0].frequency);
  }
}

void updateBuzzer()
{
  if (activeTune == nullptr || activeTuneIndex >= activeTuneLength) return;

  unsigned long elapsed = millis() - buzzerStepStart;

  if (!buzzerGap)
  {
    if (elapsed >= activeTune[activeTuneIndex].duration)
    {
      ledcWriteTone(BUZZER_PIN, 0);
      buzzerGap = true;
      buzzerStepStart = millis();
    }
    return;
  }

  if (elapsed < 25) return;

  activeTuneIndex++;

  if (activeTuneIndex >= activeTuneLength)
  {
    activeTune = nullptr;
    activeTuneLength = 0;
    ledcWriteTone(BUZZER_PIN, 0);
    return;
  }

  buzzerGap = false;
  buzzerStepStart = millis();
  ledcWriteTone(BUZZER_PIN, activeTune[activeTuneIndex].frequency);
}

void playWasteFeedbackTune(float foodWaste)
{
  if (foodWaste <= WASTE_ACCEPTABLE_MAX)
  {
    startBuzzerTune(SUCCESS_NOTES, sizeof(SUCCESS_NOTES)/sizeof(SUCCESS_NOTES[0]));
    return;
  }

  if (foodWaste <= WASTE_HIGH_MAX)
  {
    startBuzzerTune(MIDDLE_NOTES, sizeof(MIDDLE_NOTES)/sizeof(MIDDLE_NOTES[0]));
    return;
  }

  startBuzzerTune(DISAPPOINTING_NOTES, sizeof(DISAPPOINTING_NOTES)/sizeof(DISAPPOINTING_NOTES[0]));
}

// =====================================================
// TOF PRESENCE DETECTION + IMPACT METRICS
// =====================================================

void initializeToF()
{
  Serial.println("Starting VL53L0X...");
  delay(200);

  if (!tof.begin())
  {
    Serial.println("WARNING: VL53L0X ToF sensor not detected.");
    Serial.println("Dashboard will stay in measuring mode.");
    tofAvailable = false;
    personPresent = true;
    return;
  }

  tofAvailable = true;
  personPresent = false;
  lastPersonSeenTime = millis();

  Serial.println("VL53L0X presence sensor ready.");
}

void updatePresenceDetection()
{
  if (!tofAvailable)
  {
    personPresent = true;
    return;
  }

  static unsigned long lastRangeRead = 0;

  if (millis() - lastRangeRead < 45)
  {
    return;
  }

  lastRangeRead = millis();

  VL53L0X_RangingMeasurementData_t measure;

  tof.rangingTest(&measure, false);

  bool validReading =
    measure.RangeStatus != 4 &&
    measure.RangeMilliMeter > 0 &&
    measure.RangeMilliMeter < 8000;

  if (validReading)
  {
    lastDistanceMm = measure.RangeMilliMeter;
  }

  bool nearby =
    validReading &&
    measure.RangeMilliMeter <= PERSON_DISTANCE_MM;

  if (nearby)
  {
    lastPersonSeenTime = millis();

    if (personPresent)
    {
      approachTimerRunning = false;
      return;
    }

    if (!approachTimerRunning)
    {
      approachTimerRunning = true;
      approachStartTime = millis();
      return;
    }

    if (millis() - approachStartTime >= PERSON_DEBOUNCE_MS)
    {
      personPresent = true;
      approachTimerRunning = false;

      Serial.print("Person detected at ");
      Serial.print(measure.RangeMilliMeter);
      Serial.println(" mm");
    }
  }
  else
  {
    approachTimerRunning = false;

    if (
      personPresent &&
      millis() - lastPersonSeenTime >= PERSON_LEAVE_TIMEOUT_MS
    )
    {
      personPresent = false;
      Serial.println("Person left. Returning to screensaver.");
    }
  }
}

float getTodayAveragePerPlate()
{
  if (todayEvents == 0)
  {
    return 0.0;
  }

  return todayWaste /
    (float)todayEvents;
}

float getRecentAveragePerPlate()
{
  float waste = 0.0;
  uint32_t events = 0;

  // Previous six completed days.
  for (int i = 0; i < 6; i++)
  {
    waste +=
      dailyWasteHistory[i];

    events +=
      dailyEventHistory[i];
  }

  if (events == 0)
  {
    return 0.0;
  }

  return waste /
    (float)events;
}

float getLowWastePercentToday()
{
  if (todayEvents == 0)
  {
    return 0.0;
  }

  return
    (100.0 *
     (float)lowWasteEventsToday) /
    (float)todayEvents;
}

float getImprovementPercent()
{
  float recentAverage =
    getRecentAveragePerPlate();

  float todayAverage =
    getTodayAveragePerPlate();

  if (
    recentAverage <= 0.0 ||
    todayEvents == 0
  )
  {
    return 0.0;
  }

  return
    ((recentAverage - todayAverage) /
     recentAverage) *
    100.0;
}

// =====================================================
// WASTE BAND
// =====================================================

String getWasteBand(float foodWaste)
{
  if (foodWaste <= WASTE_GOOD_MAX)
  {
    return "good";
  }

  if (foodWaste <= WASTE_ACCEPTABLE_MAX)
  {
    return "acceptable";
  }

  if (foodWaste <= WASTE_NOTICEABLE_MAX)
  {
    return "noticeable";
  }

  if (foodWaste <= WASTE_MID_MAX)
  {
    return "mid";
  }

  if (foodWaste <= WASTE_HIGH_MAX)
  {
    return "high";
  }

  if (foodWaste <= WASTE_HEAVY_MAX)
  {
    return "heavy";
  }

  if (foodWaste <= WASTE_TOO_MUCH_MAX)
  {
    return "too-much";
  }

  if (foodWaste <= WASTE_VERY_HIGH_MAX)
  {
    return "very-high";
  }

  if (foodWaste < 290.0)
  {
    return "severe";
  }

  return "extreme";
}

// =====================================================
// HUMOROUS FOOD COMPARISON
// =====================================================

String getFoodComparison(float foodWaste)
{
  if (foodWaste <= WASTE_GOOD_MAX)
  {
    return "Just a few bites.";
  }

  if (foodWaste <= WASTE_ACCEPTABLE_MAX)
  {
    return "A small part of a plate.";
  }

  if (foodWaste <= WASTE_NOTICEABLE_MAX)
  {
    return "A noticeable part of a plate.";
  }

  if (foodWaste <= WASTE_MID_MAX)
  {
    return "About one third of a plate.";
  }

  if (foodWaste <= WASTE_HIGH_MAX)
  {
    return "Getting close to half a plate.";
  }

  if (foodWaste <= WASTE_HEAVY_MAX)
  {
    return "About half a plate.";
  }

  if (foodWaste <= WASTE_TOO_MUCH_MAX)
  {
    return "More than half a plate.";
  }

  if (foodWaste <= WASTE_VERY_HIGH_MAX)
  {
    return "Most of a plate.";
  }

  if (foodWaste < 290.0)
  {
    return "Almost a full plate.";
  }

  return "Nearly a full plate or more.";
}

// =====================================================
// ROTATING COMMENTS
// =====================================================

String getWasteComment(float foodWaste)
{
  uint8_t bandIndex = 9;

  if (foodWaste <= WASTE_GOOD_MAX) bandIndex = 0;
  else if (foodWaste <= WASTE_ACCEPTABLE_MAX) bandIndex = 1;
  else if (foodWaste <= WASTE_NOTICEABLE_MAX) bandIndex = 2;
  else if (foodWaste <= WASTE_MID_MAX) bandIndex = 3;
  else if (foodWaste <= WASTE_HIGH_MAX) bandIndex = 4;
  else if (foodWaste <= WASTE_HEAVY_MAX) bandIndex = 5;
  else if (foodWaste <= WASTE_TOO_MUCH_MAX) bandIndex = 6;
  else if (foodWaste <= WASTE_VERY_HIGH_MAX) bandIndex = 7;
  else if (foodWaste < 290.0) bandIndex = 8;

  const char* comments[10][2] =
  {
    {"Almost all of your food was eaten. Nice work.", "Very little food came back."},
    {"Good. Most of your food was eaten.", "The bin barely got invited."},
    {"Some of your food still ended up in the bin.", "Those leftovers still had value."},
    {"Quite a bit came back this time.", "A little more of that meal could have been eaten."},
    {"A large part of this food was never eaten.", "That food made it all the way to the plate and then to the bin."},
    {"Too much edible food came back.", "That is a lot of food and effort ending in the bin."},
    {"A lot of this meal was left behind.", "This food was cooked and served but never eaten."},
    {"Most of this food could have been avoided as waste.", "A lot was prepared just to be thrown away."},
    {"Almost a full plate came back.", "That could have been food on someone else's plate."},
    {"This was a lot of edible food to throw away.", "A lot was cooked, carried and served just to be discarded."}
  };

  uint8_t &index = commentIndexByBand[bandIndex];
  String selected = comments[bandIndex][index];
  index = (index + 1) % 2;
  return selected;
}

// =====================================================
// JSON STRING ESCAPING
// =====================================================

String escapeJson(const String &text)
{
  String escaped;

  escaped.reserve(
    text.length() + 10
  );

  for (
    size_t i = 0;
    i < text.length();
    i++
  )
  {
    char character =
      text.charAt(i);

    if (character == '\\')
    {
      escaped += "\\\\";
    }
    else if (character == '"')
    {
      escaped += "\\\"";
    }
    else if (character == '\n')
    {
      escaped += "\\n";
    }
    else if (character == '\r')
    {
      escaped += "\\r";
    }
    else
    {
      escaped += character;
    }
  }

  return escaped;
}


// =====================================================
// DATE + STATISTICS HELPERS
// =====================================================

uint32_t dateToDaySerial(const DateTime &dateTime)
{
  DateTime midnight(
    dateTime.year(),
    dateTime.month(),
    dateTime.day(),
    0,
    0,
    0
  );

  return midnight.unixtime() / 86400UL;
}

String getRTCDateText()
{
  if (!rtcAvailable)
  {
    return "RTC unavailable";
  }

  DateTime now = rtc.now();

  char text[16];

  snprintf(
    text,
    sizeof(text),
    "%04d-%02d-%02d",
    now.year(),
    now.month(),
    now.day()
  );

  return String(text);
}

void saveStatistics()
{
  preferences.begin(
    "foodstats",
    false
  );

  preferences.putUInt(
    "day",
    currentDaySerial
  );

  preferences.putFloat(
    "todayW",
    todayWaste
  );

  preferences.putUInt(
    "todayE",
    todayEvents
  );

  preferences.putUInt(
    "lowToday",
    lowWasteEventsToday
  );

  for (int i = 0; i < 7; i++)
  {
    String wasteKey =
      "w" + String(i);

    String eventKey =
      "e" + String(i);

    preferences.putFloat(
      wasteKey.c_str(),
      dailyWasteHistory[i]
    );

    preferences.putUInt(
      eventKey.c_str(),
      dailyEventHistory[i]
    );
  }

  preferences.putUChar(
    "pendN",
    pendingUploadCount
  );

  for (
    int i = 0;
    i < MAX_PENDING_UPLOADS;
    i++
  )
  {
    String number =
      String(i);

    preferences.putString(
      ("pd" + number).c_str(),
      pendingDate[i]
    );

    preferences.putFloat(
      ("pw" + number).c_str(),
      pendingWaste[i]
    );

    preferences.putUInt(
      ("pe" + number).c_str(),
      pendingEvents[i]
    );

    preferences.putFloat(
      ("pww" + number).c_str(),
      pendingWeeklyWaste[i]
    );

    preferences.putUInt(
      ("pwe" + number).c_str(),
      pendingWeeklyEvents[i]
    );

    preferences.putFloat(
      ("pav" + number).c_str(),
      pendingDailyAverage[i]
    );
  }

  preferences.end();
}

void loadStatistics()
{
  preferences.begin(
    "foodstats",
    true
  );

  currentDaySerial =
    preferences.getUInt(
      "day",
      0
    );

  todayWaste =
    preferences.getFloat(
      "todayW",
      0.0
    );

  todayEvents =
    preferences.getUInt(
      "todayE",
      0
    );

  lowWasteEventsToday =
    preferences.getUInt(
      "lowToday",
      0
    );

  for (int i = 0; i < 7; i++)
  {
    String wasteKey =
      "w" + String(i);

    String eventKey =
      "e" + String(i);

    dailyWasteHistory[i] =
      preferences.getFloat(
        wasteKey.c_str(),
        0.0
      );

    dailyEventHistory[i] =
      preferences.getUInt(
        eventKey.c_str(),
        0
      );
  }

  pendingUploadCount =
    preferences.getUChar(
      "pendN",
      0
    );

  if (
    pendingUploadCount >
    MAX_PENDING_UPLOADS
  )
  {
    pendingUploadCount = 0;
  }

  for (
    int i = 0;
    i < MAX_PENDING_UPLOADS;
    i++
  )
  {
    String number =
      String(i);

    pendingDate[i] =
      preferences.getString(
        ("pd" + number).c_str(),
        ""
      );

    pendingWaste[i] =
      preferences.getFloat(
        ("pw" + number).c_str(),
        0.0
      );

    pendingEvents[i] =
      preferences.getUInt(
        ("pe" + number).c_str(),
        0
      );

    pendingWeeklyWaste[i] =
      preferences.getFloat(
        ("pww" + number).c_str(),
        0.0
      );

    pendingWeeklyEvents[i] =
      preferences.getUInt(
        ("pwe" + number).c_str(),
        0
      );

    pendingDailyAverage[i] =
      preferences.getFloat(
        ("pav" + number).c_str(),
        0.0
      );
  }

  preferences.end();
}

void shiftOneCompletedDay()
{
  for (int i = 6; i > 0; i--)
  {
    dailyWasteHistory[i] =
      dailyWasteHistory[i - 1];

    dailyEventHistory[i] =
      dailyEventHistory[i - 1];
  }

  dailyWasteHistory[0] =
    todayWaste;

  dailyEventHistory[0] =
    todayEvents;

  todayWaste = 0.0;
  todayEvents = 0;
  lowWasteEventsToday = 0;
}


String daySerialToDateText(
  uint32_t daySerial
)
{
  DateTime dateTime(
    daySerial * 86400UL
  );

  char buffer[11];

  snprintf(
    buffer,
    sizeof(buffer),
    "%04d-%02d-%02d",
    dateTime.year(),
    dateTime.month(),
    dateTime.day()
  );

  return String(buffer);
}

float getWeeklyWaste();
uint32_t getWeeklyEvents();

bool queueCompletedDay(
  uint32_t completedDaySerial
)
{
  if (
    pendingUploadCount >=
    MAX_PENDING_UPLOADS
  )
  {
    Serial.println(
      "WARNING: Google upload queue is full."
    );

    lastCloudMessage =
      "Upload queue full";

    return false;
  }

  uint8_t index =
    pendingUploadCount;

  pendingDate[index] =
    daySerialToDateText(
      completedDaySerial
    );

  pendingWaste[index] =
    todayWaste;

  pendingEvents[index] =
    todayEvents;

  pendingWeeklyWaste[index] =
    getWeeklyWaste();

  pendingWeeklyEvents[index] =
    getWeeklyEvents();

  pendingDailyAverage[index] =
    pendingWeeklyWaste[index] /
    7.0;

  pendingUploadCount++;

  saveStatistics();

  Serial.print(
    "Queued completed day for Google Sheets: "
  );

  Serial.println(
    pendingDate[index]
  );

  return true;
}

bool queueManualExport()
{
  if (
    pendingUploadCount >=
    MAX_PENDING_UPLOADS
  )
  {
    lastCloudMessage =
      "Upload queue full";

    return false;
  }

  uint8_t index =
    pendingUploadCount;

  String testDate =
    "MANUAL-";

  if (rtcAvailable)
  {
    DateTime now = rtc.now();

    char stamp[24];

    snprintf(
      stamp,
      sizeof(stamp),
      "%04d-%02d-%02d-%02d%02d%02d",
      now.year(),
      now.month(),
      now.day(),
      now.hour(),
      now.minute(),
      now.second()
    );

    testDate +=
      String(stamp);
  }
  else
  {
    testDate +=
      String(millis());
  }

  pendingDate[index] =
    testDate;

  pendingWaste[index] =
    todayWaste;

  pendingEvents[index] =
    todayEvents;

  pendingWeeklyWaste[index] =
    getWeeklyWaste();

  pendingWeeklyEvents[index] =
    getWeeklyEvents();

  pendingDailyAverage[index] =
    getWeeklyDailyAverage();

  pendingUploadCount++;

  saveStatistics();

  Serial.println();
  Serial.print(
    "Queued manual Google Sheets export: "
  );

  Serial.println(
    pendingDate[index]
  );

  return true;
}

void removeFirstPendingUpload()
{
  if (pendingUploadCount == 0)
  {
    return;
  }

  for (
    int i = 0;
    i < pendingUploadCount - 1;
    i++
  )
  {
    pendingDate[i] =
      pendingDate[i + 1];

    pendingWaste[i] =
      pendingWaste[i + 1];

    pendingEvents[i] =
      pendingEvents[i + 1];

    pendingWeeklyWaste[i] =
      pendingWeeklyWaste[i + 1];

    pendingWeeklyEvents[i] =
      pendingWeeklyEvents[i + 1];

    pendingDailyAverage[i] =
      pendingDailyAverage[i + 1];
  }

  int lastIndex =
    pendingUploadCount - 1;

  pendingDate[lastIndex] = "";
  pendingWaste[lastIndex] = 0.0;
  pendingEvents[lastIndex] = 0;
  pendingWeeklyWaste[lastIndex] = 0.0;
  pendingWeeklyEvents[lastIndex] = 0;
  pendingDailyAverage[lastIndex] = 0.0;

  pendingUploadCount--;

  saveStatistics();
}

bool googleConfigurationReady()
{
  String url =
    String(GOOGLE_SCRIPT_URL);

  return
    url.startsWith("https://") &&
    url.indexOf("/exec") >= 0 &&
    url.indexOf("PASTE_") < 0;
}

bool uploadFirstPendingDay()
{
  if (pendingUploadCount == 0)
  {
    lastCloudMessage =
      "Up to date";
    return true;
  }

  if (WiFi.status() != WL_CONNECTED)
  {
    lastCloudMessage =
      "Waiting for internet";
    return false;
  }

  if (!googleConfigurationReady())
  {
    lastCloudMessage =
      "Add Apps Script URL";
    return false;
  }

  WiFiClientSecure secureClient;
  secureClient.setInsecure();

  HTTPClient https;

  https.setTimeout(20000);
  https.setConnectTimeout(15000);

  https.setFollowRedirects(
    HTTPC_FORCE_FOLLOW_REDIRECTS
  );

  String url =
    String(GOOGLE_SCRIPT_URL);

  url += "?token=";
  url += GOOGLE_TOKEN;

  url += "&device=";
  url += DEVICE_NAME;

  url += "&date=";
  url += pendingDate[0];

  url += "&dailyWaste=";
  url += String(pendingWaste[0], 1);

  url += "&dailyEvents=";
  url += String(pendingEvents[0]);

  url += "&weeklyWaste=";
  url += String(pendingWeeklyWaste[0], 1);

  url += "&weeklyEvents=";
  url += String(pendingWeeklyEvents[0]);

  url += "&dailyAverage=";
  url += String(pendingDailyAverage[0], 1);

  Serial.println();
  Serial.print(
    "Uploading to Google Sheets: "
  );
  Serial.println(
    pendingDate[0]
  );

  if (!https.begin(
        secureClient,
        url
      ))
  {
    Serial.println(
      "HTTPS connection failed."
    );
    lastCloudMessage =
      "HTTPS failed";
    return false;
  }

  Serial.print(
    "Wi-Fi RSSI: "
  );
  Serial.print(
    WiFi.RSSI()
  );
  Serial.println(
    " dBm"
  );

  int responseCode =
    https.GET();

  String responseBody =
    https.getString();

  https.end();

  Serial.print(
    "Google HTTP response: "
  );
  Serial.println(
    responseCode
  );

  Serial.print(
    "Google response body: "
  );
  Serial.println(
    responseBody
  );

  if (
    responseCode >= 200 &&
    responseCode < 300 &&
    (
      responseBody.indexOf(
        "\"ok\":true"
      ) >= 0 ||
      responseBody.indexOf(
        "\"duplicate\":true"
      ) >= 0
    )
  )
  {
    Serial.println(
      "Google Sheets upload successful."
    );

    lastCloudMessage =
      "Synced";

    removeFirstPendingUpload();

    return true;
  }

  Serial.println(
    "Google Sheets upload failed."
  );

  lastCloudMessage =
    "Upload failed";

  return false;
}

void processPendingUploads()
{
  if (pendingUploadCount == 0)
  {
    lastCloudMessage =
      "Up to date";

    return;
  }

  if (
    millis() -
    lastGoogleRetryTime <
    GOOGLE_RETRY_INTERVAL
  )
  {
    return;
  }

  lastGoogleRetryTime =
    millis();

  uploadFirstPendingDay();
}

void maintainInternetWiFi()
{
  static bool connectionAttemptRunning = false;
  static unsigned long connectionAttemptStart = 0;

  if (WiFi.status() == WL_CONNECTED)
  {
    connectionAttemptRunning = false;
    return;
  }

  if (connectionAttemptRunning)
  {
    if (
      millis() -
      connectionAttemptStart <
      15000UL
    )
    {
      return;
    }

    Serial.println(
      "Internet Wi-Fi connection timed out."
    );

    WiFi.disconnect();

    connectionAttemptRunning = false;

    lastWiFiRetryTime =
      millis();

    return;
  }

  if (
    millis() -
    lastWiFiRetryTime <
    WIFI_RETRY_INTERVAL
  )
  {
    return;
  }

  Serial.println(
    "Trying internet Wi-Fi again..."
  );

  WiFi.disconnect();
  delay(100);

  WiFi.begin(
    INTERNET_SSID,
    INTERNET_PASSWORD
  );

  connectionAttemptRunning = true;

  connectionAttemptStart =
    millis();

  lastWiFiRetryTime =
    millis();
}

void moveStatisticsToDay(
  uint32_t targetDaySerial
)
{
  if (targetDaySerial == 0)
  {
    return;
  }

  if (currentDaySerial == 0)
  {
    currentDaySerial =
      targetDaySerial;

    saveStatistics();
    return;
  }

  if (
    targetDaySerial >
    currentDaySerial
  )
  {
    while (
      currentDaySerial <
      targetDaySerial
    )
    {
      // Capture this completed day's totals BEFORE
      // todayWaste/todayEvents are cleared.
      bool queuedForSheets =
        queueCompletedDay(
          currentDaySerial
        );

      // At the date change (12:00 AM), immediately try to
      // export the completed day's totals to Google Sheets.
      // If internet is unavailable, the row remains safely
      // stored in Preferences and processPendingUploads()
      // will retry it later.
      if (queuedForSheets)
      {
        lastGoogleRetryTime =
          millis() - GOOGLE_RETRY_INTERVAL;

        processPendingUploads();
      }

      shiftOneCompletedDay();

      currentDaySerial++;

      saveStatistics();
    }

    Serial.println();
    Serial.println(
      "New calendar day detected."
    );

    Serial.println(
      "Completed day queued/exported to Google Sheets at midnight."
    );

    Serial.println(
      "Daily totals moved into 7-day history."
    );

    Serial.println();
  }
  else if (
    targetDaySerial <
    currentDaySerial
  )
  {
    // The RTC was moved backwards manually.
    // Preserve all totals and simply follow the
    // corrected date from now on.
    currentDaySerial =
      targetDaySerial;

    saveStatistics();

    Serial.println(
      "RTC date moved backwards; date marker updated."
    );
  }
}

void initializeRTC()
{
  Wire.begin(
    I2C_SDA,
    I2C_SCL
  );

  if (!rtc.begin(&Wire))
  {
    rtcAvailable = false;

    Serial.println(
      "ERROR: DS3231 RTC not detected."
    );

    Serial.println(
      "Daily totals will continue, but dates will not roll automatically."
    );

    return;
  }

  rtcAvailable = true;

  if (rtc.lostPower())
  {
    Serial.println(
      "RTC lost power. Setting date and time to sketch compile time."
    );

    rtc.adjust(
      DateTime(
        F(__DATE__),
        F(__TIME__)
      )
    );
  }

  DateTime now = rtc.now();

  moveStatisticsToDay(
    dateToDaySerial(now)
  );

  Serial.print(
    "RTC date: "
  );

  Serial.println(
    getRTCDateText()
  );
}

void updateDateTracking(
  bool forceCheck = false
)
{
  if (!rtcAvailable)
  {
    return;
  }

  if (
    !forceCheck &&
    millis() - lastRTCCheckTime <
    1000
  )
  {
    return;
  }

  lastRTCCheckTime =
    millis();

  DateTime now = rtc.now();

  moveStatisticsToDay(
    dateToDaySerial(now)
  );
}

float getWeeklyWaste()
{
  float total =
    todayWaste;

  // Today plus the previous six completed days.
  for (int i = 0; i < 6; i++)
  {
    total +=
      dailyWasteHistory[i];
  }

  return total;
}

uint32_t getWeeklyEvents()
{
  uint32_t total =
    todayEvents;

  for (int i = 0; i < 6; i++)
  {
    total +=
      dailyEventHistory[i];
  }

  return total;
}

float getWeeklyDailyAverage()
{
  return getWeeklyWaste() / 7.0;
}

// =====================================================
// RECORD FOOD DISPOSAL
// =====================================================

void recordFoodEvent(float foodAdded)
{
  // Make sure an event just after midnight is assigned
  // to the correct calendar day.
  updateDateTracking(true);

  todayWaste +=
    foodAdded;

  todayEvents++;

  if (foodAdded <= LOW_WASTE_MAX)
  {
    lowWasteEventsToday++;
  }

  saveStatistics();

  eventNumber++;

  accumulatedFoodWeight +=
    foodAdded;

  lastFoodAdded =
    foodAdded;

  lastMealsWasted =
    foodAdded /
    GRAMS_PER_MEAL;

  currentBand =
    getWasteBand(
      foodAdded
    );

  currentComparison =
    getFoodComparison(
      foodAdded
    );

  currentComment =
    getWasteComment(
      foodAdded
    );

  // Audible feedback follows the revised waste ranges:
  // 0-70 g    -> success
  // 71-170 g  -> middle
  // 171 g+    -> disappointing
  playWasteFeedbackTune(
    foodAdded
  );

  Serial.println();
  Serial.println(
    "===================================="
  );

  Serial.println(
    "       FOOD DISPOSAL DETECTED"
  );

  Serial.println(
    "===================================="
  );

  Serial.print(
    "Event Number       : "
  );

  Serial.println(
    eventNumber
  );

  Serial.print(
    "Food Added         : "
  );

  Serial.print(
    foodAdded,
    1
  );

  Serial.println(" g");

  Serial.print(
    "Meal Equivalent    : "
  );

  Serial.print(
    lastMealsWasted,
    2
  );

  Serial.println(" meals");

  Serial.print(
    "Comparison         : "
  );

  Serial.println(
    currentComparison
  );

  Serial.print(
    "Comment            : "
  );

  Serial.println(
    currentComment
  );

  Serial.print(
    "Session Waste Total: "
  );

  Serial.print(
    accumulatedFoodWeight,
    1
  );

  Serial.println(" g");

  Serial.println(
    "===================================="
  );

  Serial.println();
}

// =====================================================
// UPDATE BASELINE AFTER REMOVAL
// =====================================================

void updateEmptyPlatformBaseline(
  float newBaseline
)
{
  baselineWeight =
    newBaseline;

  startupWeight =
    newBaseline;

  lastMotionWeight =
    newBaseline;

  resetZeroTracking();

  Serial.println();
  Serial.println(
    "===================================="
  );

  Serial.println(
    "Platform returned to empty state."
  );

  Serial.println(
    "Physical baseline updated."
  );

  Serial.println(
    "Session waste total preserved."
  );

  Serial.println(
    "Event counter preserved."
  );

  Serial.println(
    "===================================="
  );

  Serial.println();
}

// =====================================================
// MANUAL SESSION RESET
// =====================================================

void manuallyResetSession()
{
  baselineWeight =
    currentWeight;

  startupWeight =
    currentWeight;

  lastMotionWeight =
    currentWeight;

  accumulatedFoodWeight =
    0.0;

  lastFoodAdded =
    0.0;

  lastMealsWasted =
    0.0;

  eventNumber =
    0;

  currentComment =
    "Waiting for the next food disposal...";

  currentComparison =
    "Waiting for the next plate...";

  currentBand =
    "neutral";

  for (int i = 0; i < 10; i++)
  {
    commentIndexByBand[i] = 0;
  }

  resetZeroTracking();

  Serial.println();
  Serial.println(
    "===================================="
  );

  Serial.println(
    "SESSION RESET FROM IPAD"
  );

  Serial.println(
    "Waste total reset to 0 g."
  );

  Serial.println(
    "Event counter reset to 0."
  );

  Serial.println(
    "===================================="
  );

  Serial.println();
}

// =====================================================
// SYSTEM STATE NAME
// =====================================================

const char* getStateName()
{
  switch (systemState)
  {
    case FINDING_BASELINE:
      return "Calibrating";

    case IDLE:
      return "Ready";

    case EVENT_IN_PROGRESS:
      return "Measuring";

    default:
      return "Unknown";
  }
}

// =====================================================
// WEB SERVER HANDLERS
// =====================================================

void handleHome()
{
  // Force Safari/iPad to fetch the latest interface after every firmware update.
  server.sendHeader(
    "Cache-Control",
    "no-store, no-cache, must-revalidate, max-age=0"
  );

  server.sendHeader(
    "Pragma",
    "no-cache"
  );

  server.sendHeader(
    "Expires",
    "0"
  );

  server.send_P(
    200,
    "text/html; charset=utf-8",
    MAIN_PAGE
  );
}

void handleStatus()
{
  float displayedChange =
    0.0;

  if (
    systemState ==
    EVENT_IN_PROGRESS
  )
  {
    displayedChange =
      currentWeight -
      baselineWeight;

    if (
      fabs(displayedChange) <
      3.0
    )
    {
      displayedChange =
        0.0;
    }

    if (
      displayedChange <
      0.0
    )
    {
      displayedChange =
        0.0;
    }
  }

  String json;

  json.reserve(1200);

  json += "{";

  json +=
    "\"currentWeight\":";

  json +=
    String(
      displayedChange,
      1
    );

  json +=
    ",\"lastEvent\":";

  json +=
    String(
      lastFoodAdded,
      1
    );

  json +=
    ",\"mealsWasted\":";

  json +=
    String(
      lastMealsWasted,
      2
    );

  json +=
    ",\"sessionTotal\":";

  json +=
    String(
      accumulatedFoodWeight,
      1
    );

  json +=
    ",\"todayWaste\":";

  json +=
    String(
      todayWaste,
      1
    );

  json +=
    ",\"todayEvents\":";

  json +=
    String(todayEvents);

  json +=
    ",\"weeklyWaste\":";

  json +=
    String(
      getWeeklyWaste(),
      1
    );

  json +=
    ",\"weeklyEvents\":";

  json +=
    String(
      getWeeklyEvents()
    );

  json +=
    ",\"dailyAverage\":";

  json +=
    String(
      getWeeklyDailyAverage(),
      1
    );

  json +=
    ",\"rtcDate\":\"";

  json +=
    escapeJson(
      getRTCDateText()
    );

  json += "\"";

  json +=
    ",\"cloudStatus\":\"";

  json +=
    escapeJson(
      lastCloudMessage
    );

  json += "\"";

  json +=
    ",\"pendingUploads\":";

  json +=
    String(
      pendingUploadCount
    );

  json +=
    ",\"personPresent\":";

  json +=
    personPresent
      ? "true"
      : "false";

  json +=
    ",\"distanceMm\":";

  json +=
    String(
      lastDistanceMm
    );

  json +=
    ",\"lowWasteToday\":";

  json +=
    String(
      lowWasteEventsToday
    );

  json +=
    ",\"lowWastePercent\":";

  json +=
    String(
      getLowWastePercentToday(),
      1
    );

  json +=
    ",\"todayAvgPerPlate\":";

  json +=
    String(
      getTodayAveragePerPlate(),
      1
    );

  json +=
    ",\"recentAvgPerPlate\":";

  json +=
    String(
      getRecentAveragePerPlate(),
      1
    );

  json +=
    ",\"improvementPercent\":";

  json +=
    String(
      getImprovementPercent(),
      1
    );

  json +=
    ",\"eventNumber\":";

  json +=
    String(eventNumber);

  json +=
    ",\"state\":\"";

  json +=
    getStateName();

  json += "\"";

  json +=
    ",\"band\":\"";

  json +=
    currentBand;

  json += "\"";

  json +=
    ",\"comparison\":\"";

  json +=
    escapeJson(
      currentComparison
    );

  json += "\"";

  json +=
    ",\"comment\":\"";

  json +=
    escapeJson(
      currentComment
    );

  json += "\"";

  json += "}";

  server.sendHeader(
    "Cache-Control",
    "no-store"
  );

  server.send(
    200,
    "application/json; charset=utf-8",
    json
  );
}

void handleExportToSheets()
{
  if (!googleConfigurationReady())
  {
    server.send(
      400,
      "text/plain",
      "Add your Google Apps Script /exec URL first"
    );

    return;
  }

  if (
    WiFi.status() !=
    WL_CONNECTED
  )
  {
    server.send(
      503,
      "text/plain",
      "ESP32 is not connected to internet Wi-Fi"
    );

    return;
  }

  if (!queueManualExport())
  {
    server.send(
      507,
      "text/plain",
      "Upload queue is full"
    );

    return;
  }

  bool success =
    uploadFirstPendingDay();

  if (success)
  {
    server.send(
      200,
      "text/plain",
      "Exported current statistics to Google Sheets."
    );
  }
  else
  {
    server.send(
      502,
      "text/plain",
      "Google upload failed. Check Serial Monitor."
    );
  }
}

void handleResetSession()
{
  if (
    systemState ==
    FINDING_BASELINE
  )
  {
    server.send(
      409,
      "text/plain",
      "Wait until calibration is complete"
    );

    return;
  }

  if (
    systemState ==
    EVENT_IN_PROGRESS
  )
  {
    server.send(
      409,
      "text/plain",
      "Wait until the current measurement is complete"
    );

    return;
  }

  manuallyResetSession();

  server.send(
    200,
    "text/plain",
    "Session reset successfully"
  );
}

// =====================================================
// START WI-FI ACCESS POINT
// =====================================================

void startWiFi()
{
  WiFi.mode(
    WIFI_AP_STA
  );

  // -----------------------------------------------
  // Direct dashboard network for the iPad
  // -----------------------------------------------

  bool apStarted =
    WiFi.softAP(
      AP_NAME,
      AP_PASSWORD
    );

  if (!apStarted)
  {
    Serial.println(
      "ERROR: ESP32 dashboard access point failed."
    );
  }

  IPAddress apIP =
    WiFi.softAPIP();

  // -----------------------------------------------
  // Internet Wi-Fi for Google Sheets
  // -----------------------------------------------

  Serial.print(
    "Connecting to internet Wi-Fi: "
  );

  Serial.println(
    INTERNET_SSID
  );

  WiFi.begin(
    INTERNET_SSID,
    INTERNET_PASSWORD
  );

  unsigned long connectStart =
    millis();

  while (
    WiFi.status() !=
    WL_CONNECTED &&
    millis() - connectStart <
    12000UL
  )
  {
    server.handleClient();
    delay(250);
  }

  if (
    WiFi.status() ==
    WL_CONNECTED
  )
  {
    Serial.println(
      "Internet Wi-Fi connected."
    );

    Serial.print(
      "Router IP: "
    );

    Serial.println(
      WiFi.localIP()
    );

    lastCloudMessage =
      pendingUploadCount > 0
        ? "Waiting to sync"
        : "Up to date";
  }
  else
  {
    Serial.println(
      "Internet Wi-Fi not connected."
    );

    Serial.println(
      "The iPad dashboard will still work."
    );

    lastCloudMessage =
      "Waiting for internet";

    WiFi.disconnect();

    lastWiFiRetryTime =
      millis();
  }

  // -----------------------------------------------
  // Web dashboard routes
  // -----------------------------------------------

  server.on(
    "/",
    HTTP_GET,
    handleHome
  );

  server.on(
    "/status",
    HTTP_GET,
    handleStatus
  );

  server.on(
    "/export-sheets",
    HTTP_POST,
    handleExportToSheets
  );

  // Old endpoint kept as a compatibility alias.
  server.on(
    "/test-upload",
    HTTP_POST,
    handleExportToSheets
  );

  server.on(
    "/reset",
    HTTP_POST,
    handleResetSession
  );

  server.onNotFound([]()
  {
    server.send(
      404,
      "text/plain",
      "Page not found"
    );
  });

  server.begin();

  Serial.println();
  Serial.println(
    "===================================="
  );

  Serial.println(
    " WI-FI + GOOGLE SHEETS READY"
  );

  Serial.println(
    "===================================="
  );

  Serial.print(
    "iPad Wi-Fi name : "
  );

  Serial.println(
    AP_NAME
  );

  Serial.print(
    "iPad password   : "
  );

  Serial.println(
    AP_PASSWORD
  );

  Serial.print(
    "Dashboard       : http://"
  );

  Serial.println(
    apIP
  );

  Serial.print(
    "Google pending  : "
  );

  Serial.println(
    pendingUploadCount
  );

  Serial.println(
    "===================================="
  );

  Serial.println();
}

// =====================================================
// SETUP
// =====================================================

void setup()
{
  Serial.begin(115200);

  delay(1000);

  // Passive buzzer PWM setup
  ledcAttach(
    BUZZER_PIN,
    2000,
    8
  );

  ledcWriteTone(
    BUZZER_PIN,
    0
  );

  lc1.begin(
    DT1,
    SCK1
  );

  lc2.begin(
    DT2,
    SCK2
  );

  lc3.begin(
    DT3,
    SCK3
  );

  lc4.begin(
    DT4,
    SCK4
  );

  Serial.println();
  Serial.println(
    "===================================="
  );

  Serial.println(
    " FOOD WASTE MONITOR + IPAD"
  );

  Serial.println(
    " ToF Screensaver + RTC + Google Sheets + Buzzer"
  );

  Serial.println(
    "===================================="
  );

  Serial.println();

  loadStatistics();
  initializeRTC();
  initializeToF();

  startWiFi();

  // Allow an immediate cloud attempt during startup.
  if (pendingUploadCount > 0)
  {
    lastGoogleRetryTime =
      millis() - GOOGLE_RETRY_INTERVAL;

    processPendingUploads();
  }

  Serial.print(
    "Loaded today's stored waste: "
  );

  Serial.print(
    todayWaste,
    1
  );

  Serial.println(" g");

  Serial.print(
    "Loaded today's stored events: "
  );

  Serial.println(
    todayEvents
  );

  Serial.println(
    "Keep the platform untouched."
  );

  Serial.println(
    "Finding initial baseline..."
  );
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop()
{
  server.handleClient();
  updateBuzzer();

  updateDateTracking(false);
  updatePresenceDetection();
  maintainInternetWiFi();
  processPendingUploads();

  if (
    !readPlatformWeight(
      currentWeight
    )
  )
  {
    resetZeroTracking();

    unsigned long errorWaitStart =
      millis();

    while (
      millis() -
      errorWaitStart <
      1000
    )
    {
      server.handleClient();
      delay(1);
    }

    return;
  }

  bool stable =
    updateStability(
      currentWeight
    );

  if (
    systemState ==
    FINDING_BASELINE
  )
  {
    if (stable)
    {
      startupWeight =
        currentWeight;

      baselineWeight =
        currentWeight;

      lastMotionWeight =
        currentWeight;

      accumulatedFoodWeight =
        0.0;

      lastFoodAdded =
        0.0;

      lastMealsWasted =
        0.0;

      eventNumber =
        0;

      currentComment =
        "Waiting for the next food disposal...";

      currentComparison =
        "Waiting for the next plate...";

      currentBand =
        "neutral";

      systemState =
        IDLE;

      Serial.println();

      Serial.println(
        "Initial baseline established."
      );

      Serial.print(
        "Internal sensor offset: "
      );

      Serial.print(
        baselineWeight,
        1
      );

      Serial.println(" g");

      Serial.println(
        "Displayed weight: 0.0 g"
      );

      Serial.println(
        "Session Waste Total: 0.0 g"
      );

      Serial.println(
        "System ready."
      );

      Serial.println();
    }

    server.handleClient();

    delay(20);

    return;
  }

  updateAutomaticZero(
    stable
  );

  if (
    systemState ==
    IDLE
  )
  {
    float changeFromBaseline =
      currentWeight -
      baselineWeight;

    if (
      changeFromBaseline >=
      EVENT_START_THRESHOLD
    )
    {
      resetZeroTracking();

      systemState =
        EVENT_IN_PROGRESS;

      lastMotionWeight =
        currentWeight;

      lastEventMovementTime =
        millis();

      Serial.println(
        "Food detected..."
      );

      Serial.println(
        "Waiting for the weight to settle."
      );
    }

    else if (
      stable &&
      changeFromBaseline <=
      -REMOVAL_THRESHOLD
    )
    {
      resetZeroTracking();

      if (
        fabs(
          currentWeight -
          startupWeight
        ) <=
        EMPTY_TOLERANCE
      )
      {
        updateEmptyPlatformBaseline(
          currentWeight
        );
      }
      else
      {
        baselineWeight =
          currentWeight;

        lastMotionWeight =
          currentWeight;

        Serial.println();
        Serial.println(
          "Weight removal detected."
        );

        Serial.println(
          "Physical baseline updated."
        );

        Serial.println(
          "Session waste total preserved."
        );

        Serial.println(
          "Event counter preserved."
        );

        Serial.println();
      }
    }
  }

  else if (
    systemState ==
    EVENT_IN_PROGRESS
  )
  {
    float movement =
      fabs(
        currentWeight -
        lastMotionWeight
      );

    if (
      movement >=
      MOTION_THRESHOLD
    )
    {
      lastMotionWeight =
        currentWeight;

      lastEventMovementTime =
        millis();
    }

    float measuredIncrease =
      currentWeight -
      baselineWeight;

    if (
      stable &&
      measuredIncrease <=
      EVENT_CANCEL_RANGE
    )
    {
      Serial.println();

      Serial.println(
        "Event cancelled."
      );

      Serial.println(
        "Weight returned to baseline."
      );

      Serial.println();

      systemState =
        IDLE;

      lastMotionWeight =
        currentWeight;

      resetZeroTracking();
    }

    else if (
      stable &&
      millis() -
      lastEventMovementTime >=
      EVENT_SETTLE_TIME
    )
    {
      float correctedFoodAdded =
        correctEventWeight(
          measuredIncrease
        );

      if (
        correctedFoodAdded >=
        MINIMUM_EVENT_WEIGHT
      )
      {
        recordFoodEvent(
          correctedFoodAdded
        );
      }

      baselineWeight =
        currentWeight;

      lastMotionWeight =
        currentWeight;

      systemState =
        IDLE;

      resetZeroTracking();

      Serial.println(
        "System ready for the next disposal."
      );

      Serial.println();
    }
  }

  server.handleClient();

  delay(20);
}

