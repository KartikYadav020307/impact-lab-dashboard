#include <Arduino.h>
#include <math.h>

// =====================================================
// IMPACT-SHIELD - Seeed Studio XIAO ESP32-C3
// =====================================================
// Zone 1: D0 / GPIO 2 (ADC1_CH2)
// Zone 2: D1 / GPIO 3 (ADC1_CH3)
// Zone 3: D2 / GPIO 4 (ADC1_CH4)
// Zone 4: D4 / GPIO 6 (Digital Pulse) or D3 / GPIO 5
// =====================================================

constexpr uint8_t ACTIVE_SENSORS = 4;
const uint8_t PINS[4] = {2, 3, 4, 6};

// Trigger when delta from baseline exceeds 50 counts
constexpr bool LARGE_SPIKE_MODE = false; // Enable dynamic adaptive baseline
constexpr uint16_t SPIKE_THRESHOLD[4] = {50, 50, 50, 50};
constexpr uint16_t MIN_THRESHOLD[4] = {30, 30, 30, 30};

constexpr uint16_t MAX_CALIBRATED_THRESHOLD = 1000;
constexpr float GAIN[4] = {1.0f, 1.0f, 1.0f, 1.0f};

// ---------------- TIMING ----------------
constexpr uint32_t SAMPLE_US = 250;
constexpr uint32_t PEAK_WINDOW_US = 15000; // 15ms impact window to capture peak
constexpr uint32_t COOLDOWN_US = 80000;    // 80ms refractory cooldown
constexpr uint32_t QUIET_REARM_US = 20000;  // 20ms quiet to rearm

constexpr float DOMINANCE_RATIO = 1.15f;

// ---------------- SENSOR DATA ----------------
float baseline[4] = {0, 0, 0, 0};
uint16_t threshold[4] = {50, 50, 50, 50};
uint16_t raw[4] = {};
uint16_t peaks[4] = {};
uint16_t rawAtPeak[4] = {};

uint16_t windowMin[4] = {4095, 4095, 4095, 4095};
uint16_t windowMax[4] = {};

bool calibrationOK = true;
bool capturing = false;
bool armed = false;
bool quietTracking = false;
bool cooldownActive = false;

uint32_t captureStart = 0;
uint32_t quietSince = 0;
uint32_t cooldownStart = 0;
uint32_t nextSample = 0;
uint32_t lastStatus = 0;
uint32_t lastStuckCheck = 0;
uint32_t eventCount = 0;

// ---------------- BASIC FUNCTIONS ----------------
bool detectionEnabled() {
  return true;
}

uint16_t deviation(uint8_t i) {
  return (uint16_t)fabsf((float)raw[i] - baseline[i]);
}

void readSensors() {
  raw[0] = analogRead(2); // Zone 1: D0
  raw[1] = analogRead(3); // Zone 2: D1
  raw[2] = analogRead(4); // Zone 3: D2

  // Zone 4: Read D4 (GPIO 6) or D3 (GPIO 5)
  // Piezo impacts generate a fast voltage spike that registers as HIGH
  bool hit4 = (digitalRead(6) == HIGH) || (digitalRead(5) == HIGH);
  raw[3] = hit4 ? 2000 : 0;

  for (uint8_t i = 0; i < ACTIVE_SENSORS; i++) {
    if (raw[i] < windowMin[i]) windowMin[i] = raw[i];
    if (raw[i] > windowMax[i]) windowMax[i] = raw[i];
  }
}

void printArray(const uint16_t values[4]) {
  Serial.print('[');
  for (uint8_t i = 0; i < 4; i++) {
    if (i > 0) Serial.print(',');
    if (i < ACTIVE_SENSORS) {
      Serial.print(values[i]);
    } else {
      Serial.print("null");
    }
  }
  Serial.print(']');
}

// ---------------- STATUS JSON ----------------
void status(const char *type) {
  Serial.printf(
    "{\"type\":\"%s\","
    "\"firmware\":\"piezo-zones-xiao-c3-1.2\","
    "\"board\":\"XIAO ESP32-C3\","
    "\"uptime_ms\":%lu,"
    "\"active_sensors\":%u,"
    "\"pins\":[",
    type,
    (unsigned long)millis(),
    ACTIVE_SENSORS
  );

  for (uint8_t i = 0; i < 4; i++) {
    if (i > 0) Serial.print(',');
    if (i < ACTIVE_SENSORS) {
      Serial.print(PINS[i]);
    } else {
      Serial.print("null");
    }
  }

  Serial.print("],\"readings\":");
  printArray(raw);

  Serial.print(",\"thresholds\":");
  printArray(threshold);

  Serial.print(",\"window_min\":");
  printArray(windowMin);

  Serial.print(",\"window_max\":");
  printArray(windowMax);

  Serial.print(",\"baseline\":[");
  for (uint8_t i = 0; i < 4; i++) {
    if (i > 0) Serial.print(',');
    if (i < ACTIVE_SENSORS) {
      Serial.print(baseline[i], 1);
    } else {
      Serial.print("null");
    }
  }

  Serial.printf(
    "],\"mode\":\"auto\","
    "\"calibration_ok\":true,"
    "\"detection_enabled\":true,"
    "\"armed\":%s,"
    "\"events\":%lu}\n",
    armed ? "true" : "false",
    (unsigned long)eventCount
  );

  for (uint8_t i = 0; i < ACTIVE_SENSORS; i++) {
    windowMin[i] = 4095;
    windowMax[i] = 0;
  }
}

// ---------------- BASELINE CALIBRATION ----------------
void calibrate() {
  Serial.println("{\"type\":\"calibrating\",\"duration_ms\":500}");

  capturing = false;
  armed = false;
  quietTracking = false;
  cooldownActive = false;

  constexpr uint16_t N = 256;
  uint32_t sum[4] = {};

  for (uint16_t n = 0; n < N; n++) {
    readSensors();
    for (uint8_t i = 0; i < ACTIVE_SENSORS; i++) {
      sum[i] += raw[i];
    }
    delay(1);
  }

  for (uint8_t i = 0; i < ACTIVE_SENSORS; i++) {
    baseline[i] = (float)sum[i] / N;
    threshold[i] = SPIKE_THRESHOLD[i];
    windowMin[i] = 4095;
    windowMax[i] = 0;
  }

  calibrationOK = true;
  nextSample = micros();
  lastStatus = millis();
  armed = true;

  status("ready");
}

// ---------------- IMPACT CLASSIFICATION ----------------
void finishImpact() {
  int winner = -1;
  float best = 0.0f;
  float second = 0.0f;

  for (uint8_t i = 0; i < ACTIVE_SENSORS; i++) {
    if (peaks[i] < threshold[i]) continue;

    float score = peaks[i] * GAIN[i];
    if (score > best) {
      second = best;
      best = score;
      winner = i;
    } else if (score > second) {
      second = score;
    }
  }

  if (winner >= 0) {
    bool ambiguous = (second > 0.0f) && (best < second * DOMINANCE_RATIO);
    eventCount++;

    Serial.printf(
      "{\"type\":\"hit\","
      "\"event\":%lu,"
      "\"time_us\":%lu,"
      "\"zone\":%u,"
      "\"candidate_zone\":%u,"
      "\"ambiguous\":%s,"
      "\"peak\":%u,"
      "\"intensity\":%.3f,"
      "\"dominance\":",
      (unsigned long)eventCount,
      (unsigned long)captureStart,
      ambiguous ? 0 : winner + 1,
      winner + 1,
      ambiguous ? "true" : "false",
      peaks[winner],
      fminf(1.0f, peaks[winner] / 3000.0f)
    );

    if (second > 0.0f) {
      Serial.print(best / second, 3);
    } else {
      Serial.print("null");
    }

    Serial.print(",\"peaks\":");
    printArray(peaks);

    Serial.print(",\"readings\":");
    printArray(rawAtPeak);

    Serial.print(",\"thresholds\":");
    printArray(threshold);

    Serial.printf(
      ",\"clipped\":%s}\n",
      rawAtPeak[winner] >= 4090 ? "true" : "false"
    );
  }

  capturing = false;
  armed = false;
  quietTracking = false;
  cooldownActive = true;
  cooldownStart = micros();
}

// ---------------- SETUP ----------------
void setup() {
  Serial.begin(115200);

  analogReadResolution(12);

  pinMode(2, INPUT); // D0
  pinMode(3, INPUT); // D1
  pinMode(4, INPUT); // D2
  pinMode(5, INPUT_PULLDOWN); // D3
  pinMode(6, INPUT_PULLDOWN); // D4

  analogSetPinAttenuation(2, ADC_11db);
  analogSetPinAttenuation(3, ADC_11db);
  analogSetPinAttenuation(4, ADC_11db);

  delay(400);
  calibrate();
}

// ---------------- MAIN LOOP ----------------
void loop() {
  uint32_t now = micros();

  if ((int32_t)(now - nextSample) < 0) {
    return;
  }
  nextSample = now + SAMPLE_US;

  readSensors();

  // Dynamic baseline adaptation when not in an impact
  if (!capturing) {
    for (uint8_t i = 0; i < ACTIVE_SENSORS; i++) {
      uint16_t d = deviation(i);
      // If reading is near baseline, smoothly pull baseline towards it
      if (d < threshold[i] * 2) {
        baseline[i] += 0.001f * ((float)raw[i] - baseline[i]);
      }
    }
  }

  bool triggered = false;
  bool quiet = true;

  for (uint8_t i = 0; i < ACTIVE_SENSORS; i++) {
    uint16_t d = deviation(i);
    if (d >= threshold[i]) {
      triggered = true;
    }
    // Quiet when within 60% of threshold
    if (d >= (threshold[i] * 3u / 5u)) {
      quiet = false;
    }
  }

  // Start recording when any sensor crosses threshold
  if (!capturing && armed && triggered) {
    capturing = true;
    armed = false;
    captureStart = now;

    for (uint8_t i = 0; i < ACTIVE_SENSORS; i++) {
      peaks[i] = deviation(i);
      rawAtPeak[i] = raw[i];
    }
  }

  // Capture peaks from all sensors during impact window
  if (capturing) {
    for (uint8_t i = 0; i < ACTIVE_SENSORS; i++) {
      uint16_t d = deviation(i);
      if (d > peaks[i]) {
        peaks[i] = d;
        rawAtPeak[i] = raw[i];
      }
    }

    if ((uint32_t)(now - captureStart) >= PEAK_WINDOW_US) {
      finishImpact();
    }
    return;
  }

  // Rearm logic
  if (quiet) {
    if (!quietTracking) {
      quietSince = now;
      quietTracking = true;
    }

    bool settled = (uint32_t)(now - quietSince) >= QUIET_REARM_US;
    bool cooled = !cooldownActive || ((uint32_t)(now - cooldownStart) >= COOLDOWN_US);

    if (settled && cooled) {
      armed = true;
      cooldownActive = false;
    }
  } else {
    quietTracking = false;

    // Safety: If not quiet for > 800ms, snap baseline to current resting level and rearm!
    if (!armed && (uint32_t)(now - quietSince) > 800000UL) {
      for (uint8_t i = 0; i < ACTIVE_SENSORS; i++) {
        baseline[i] = raw[i];
      }
      armed = true;
      cooldownActive = false;
      quietSince = now;
    }
  }

  // Serial commands
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'c' || c == 'C') {
      calibrate();
      return;
    }
    if (c == 's' || c == 'S') {
      status("status");
    }
  }

  // Periodic JSON status for GUI
  if ((uint32_t)(millis() - lastStatus) >= 1000) {
    lastStatus = millis();
    status("status");
  }

  delay(0);
}
