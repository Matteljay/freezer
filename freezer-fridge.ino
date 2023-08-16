// Freezer - Fridge
//

#include <SoftwareSerial.h>

#define PIN_LED 0
#define PIN_COOL 1
#define PIN_AMPS A1
#define PIN_TSET A3
#define PIN_TEMP A2
#define PIN_NONE 255

//#define DEBUG
#define DEBUG_BOOT_DELAY_SEC 10
#define TOOCOLD_TEMP -30
#define TOOCOLD_ERROR_MINS 60
#define AMPS_EXPECTED 510 // in pin value
#define AMPS_BAD_SPIKE_RANGE 50 // in pin value
#define AMPS_BAD_ERROR_MINS 6
#define AMP_MEASURE_COUNT 20
#define PUMP_MAX_TIME_MINS 210
#define PUMP_MIN_RELAX_MINS 6
#define PUMP_MIN_RUN_MINS 8
#define IMPACT_TIME_MINS 60
#define IMPACT_TEMP_REQUIRED 2 // in Celsius
#define TEMP_HYSTERESIS 2 // in Celsius
#define LOOP_TIME_MS 2000

bool pumpRunning = false;
unsigned long pumpLastStartTime = 0;
unsigned long lastImpactTime = 0;
int lastImpactTemp = 0;
int pinTempEMA = 0;
int pinTsetEMA = 0;
int realTempEMA = 0;
int realTsetEMA = 0;

#ifdef DEBUG
  #define RX PIN_NONE
  #define TX PIN_LED
  SoftwareSerial mySerial(RX, TX);
  char msg[32];
  #define debugPrint(...) snprintf(msg, sizeof(msg), __VA_ARGS__); \
    msg[sizeof(msg) - 1] = '\0'; mySerial.println(msg);
#else
  #define debugPrint(...)
#endif

/* Varargs doesn't work (atm) with ATtiny85
void debugPrint(const char *fmt, ...) {
  #ifdef DEBUG
    char msg[40];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    msg[sizeof(msg) - 1] = '\0';
    mySerial.println(msg);
  #endif
}
*/

void setup() {
  pinMode(PIN_COOL, OUTPUT);
  #ifdef DEBUG
    mySerial.begin(9600);
    delay(DEBUG_BOOT_DELAY_SEC * 1000);
    debugPrint("Hi");
  #else
    pinMode(PIN_LED, OUTPUT);
  #endif
  delay(500);
}

void loop() {
  fixTimeOverflows();
  calcVARs();
  checkTempSensor();
  stopRunningZone();
  monitorAmps();
  pumpingTooLong();
  badTempImpact(); // Leaking cold air, probably door open
  startRunningZone();
  delay(LOOP_TIME_MS);
}

void fixTimeOverflows() {
  const unsigned long now = millis();
  if(now < pumpLastStartTime) pumpLastStartTime = 0;
  if(now < lastImpactTime) lastImpactTime = 0;
}

void calcVARs() {
  // EMA - Exponential moving average formula
  // (m - p) * k + p <=> (m + (1/k - 1) * p) / 1/k
  // m: measurement, p: previousEMA, k: constant steepness/weighting
  // using k = 0.2 for easy use with integer type
  if(!pinTempEMA) {
    pinTempEMA = analogRead(PIN_TEMP);
  } else {
    pinTempEMA = (analogRead(PIN_TEMP) + 4 * pinTempEMA) / 5;
  }
  if(!pinTsetEMA) {
    pinTsetEMA = analogRead(PIN_TSET);
  } else {
    pinTsetEMA = (analogRead(PIN_TSET) + 4 * pinTsetEMA) / 5;
  }
  //debugPrint("calcVARs %d %d", pinTempEMA, pinTsetEMA);
  // Line from 2 points: y - y1 = (y2 - y1) / (x2 - x1) * (x - x1)
  // <=> y = m * x + q && m = (y2 - y1) / (x2 – x1) && q = y1 – m * x1
  // PIN_TEMP -> degC : 133 -> -27 (; 600 -> 8); 650 -> 16
  realTempEMA = pinTempEMA * 43 / 517 - 38;
  // PIN_TSET -> degC : 0 -> -20 ; 1024 -> 10
  realTsetEMA = pinTsetEMA * 30 / 1024 - 20;
  debugPrint("temps %d (%d) -> %d (%d)",
    realTempEMA, pinTempEMA, realTsetEMA, pinTsetEMA);
}

void checkTempSensor() {
  if(realTempEMA <= TOOCOLD_TEMP) {
    pumpSTOP();
    debugPrint("ERROR: TooCold STOP %d", realTempEMA);
    #ifdef DEBUG
      delay(PUMP_MIN_RELAX_MINS * 60000);
    #else
      digitalWrite(PIN_LED, HIGH);
      delay(TOOCOLD_ERROR_MINS * 60000);
      digitalWrite(PIN_LED, LOW);
    #endif
  }  
}

void monitorAmps() {
  if(!pumpRunning) return;
  const int amps = analogRead(PIN_AMPS);
  if(abs(amps - AMPS_EXPECTED) < AMPS_BAD_SPIKE_RANGE) return;
  //debugPrint("WARNING: One bad amp");
  int averageSpike = 0;
  const int pointDelay = 1000 / AMP_MEASURE_COUNT;
  for(int i = 0; i < AMP_MEASURE_COUNT; i++) {
    delay(pointDelay);
    averageSpike += abs(analogRead(PIN_AMPS) - AMPS_EXPECTED);
  }
  averageSpike /= AMP_MEASURE_COUNT;
  debugPrint("Spike %d", averageSpike);
  if(averageSpike < AMPS_BAD_SPIKE_RANGE) {
    return;
  }
  pumpSTOP();
  debugPrint("ERROR: Amps");
  #ifdef DEBUG
    delay(1 * 60000);
  #else
    digitalWrite(PIN_LED, HIGH);
    delay(AMPS_BAD_ERROR_MINS * 60000);
    digitalWrite(PIN_LED, LOW);
  #endif
}

void pumpingTooLong() {
  if(!pumpRunning) return;
  if(millis() - pumpLastStartTime <= PUMP_MAX_TIME_MINS * 60000) {
    return;
  }
  pumpSTOP();
  debugPrint("FATAL: Pumping %d", realTempEMA);
  #ifdef DEBUG
    while(true) { 
      delay(1000);
    }
  #else
    bool flashLED = true;
    while(true) {
      digitalWrite(PIN_LED, flashLED ? HIGH : LOW);
      flashLED = !flashLED;
      delay(1000);
    }
  #endif
}

void badTempImpact() {
  if(!pumpRunning) return;
  if(millis() - lastImpactTime < IMPACT_TIME_MINS * 60000) return;
  //const int tempNow = pinTEMPtoCelsius();
  if(lastImpactTemp - realTempEMA > IMPACT_TEMP_REQUIRED) {
    //debugPrint("Impact OK %d %d", lastImpactTemp, realTempEMA);
    lastImpactTime = millis();
    lastImpactTemp = realTempEMA;
    return;
  }
  pumpSTOP();
  debugPrint("FATAL: Impact");
  #ifdef DEBUG
    while(true) { 
      delay(1000);
    }
  #else
    bool flashLED = true;
    while(true) {
      digitalWrite(PIN_LED, flashLED ? HIGH : LOW);
      flashLED = !flashLED;
      delay(333);
    }
  #endif
}

/* Non blocking (more cumbersome) implementation
unsigned long pumpLastStopTime = 0;
if(pumpRelaxed())
bool pumpRelaxed() {
  if(pumpRunning) return false;
  if(millis() - pumpLastStopTime > PUMP_MIN_RELAX_MINS * 1000) {
    return true;
  }
  debugPrint("WARNING: Pump not relaxed");
  return false;
}
*/

void startRunningZone() {
  if(pumpRunning) return;
  if(realTempEMA > TOOCOLD_TEMP && realTempEMA >= realTsetEMA) {
    debugPrint("Zone START %d", realTsetEMA);
    pumpSTART();
  }
}

void stopRunningZone() {
  if(!pumpRunning) return;
  if(millis() - pumpLastStartTime < PUMP_MIN_RUN_MINS * 60000) return;
  const int targetTemp = realTsetEMA - abs(TEMP_HYSTERESIS);
  if(realTempEMA <= targetTemp) {
    pumpSTOP();
    debugPrint("Zone STOP %d", targetTemp);
    delay(PUMP_MIN_RELAX_MINS * 60000);
    debugPrint("Pump relaxed");
  }
}

void pumpSTART() {
  digitalWrite(PIN_COOL, HIGH);
  pumpRunning = true;
  pumpLastStartTime = millis();
  // "Impact" is the temp-delta monitored to see if the pump is having effect
  // Otherwise the door is probably open
  lastImpactTime = pumpLastStartTime;
  lastImpactTemp = realTempEMA;
  debugPrint("PUMP_START");
}

void pumpSTOP() {
  digitalWrite(PIN_COOL, LOW);
  pumpRunning = false;
  debugPrint("PUMP_STOP");
}

// EOF
