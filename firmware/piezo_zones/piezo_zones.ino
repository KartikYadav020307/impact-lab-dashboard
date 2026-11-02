#include <Arduino.h>
#include <math.h>
#include "esp_log.h"

// =========================================================================
// IMPACT LAB FIRMWARE - SEEED STUDIO XIAO ESP32-C3
// =========================================================================
// WIRING:
//   Zone 1 -> D0 / GPIO 2 (ADC1_CH2, A0)
//   Zone 2 -> D1 / GPIO 3 (ADC1_CH3, A1)
//   Zone 3 -> D2 / GPIO 4 (ADC1_CH4, A2)
//   Zone 4 -> D3 / GPIO 5 (ADC2_CH0, A3)
// =========================================================================

#if !defined(CONFIG_IDF_TARGET_ESP32C3)
  #warning "Select 'XIAO_ESP32C3' or 'ESP32C3 Dev Module' in Arduino IDE."
#endif

constexpr uint8_t ACTIVE_SENSORS = 4; // 1 to 4 sensors

#if defined(D0) && defined(D1) && defined(D2) && defined(D3)
constexpr uint8_t PINS[4] = {D0, D1, D2, D3};
#else
constexpr uint8_t PINS[4] = {2, 3, 4, 5};
#endif

// Mode & Threshold Configuration:
// LARGE_SPIKE_MODE = true uses fixed spike thresholds above resting baseline.
// Set to false to use automatically calculated 6-sigma idle noise thresholds.
constexpr bool LARGE_SPIKE_MODE = true;

// Trigger delta above baseline (0-4095 range, 12-bit ADC):
// Set to 250 so idle noise (which in your setup peaks around ~97) is completely ignored!
// It will stay dark/idle at rest and ONLY shine when you tap it with a real impact.
// (Tuning: 200 = lighter taps, 250 = standard taps, 400+ = heavy impacts)
constexpr uint16_t SPIKE_THRESHOLD[4] = {250, 250, 250, 250};
constexpr uint16_t MIN_THRESHOLD[4]   = {100, 100, 100, 100};
constexpr uint16_t MAX_CALIBRATED_THRESHOLD = 1000;
constexpr float GAIN[4] = {1.0f, 1.0f, 1.0f, 1.0f};

// ---------------- TIMING CONFIGURATION ----------------
constexpr uint32_t SAMPLE_US       = 250;   // 4000 Hz scan interval
constexpr uint32_t PEAK_WINDOW_US  = 12000; // 12ms peak impact capture window
constexpr uint32_t COOLDOWN_US     = 80000; // 80ms refractory cooldown
constexpr uint32_t QUIET_REARM_US  = 15000; // 15ms settled quiet to rearm
constexpr float DOMINANCE_RATIO    = 1.15f; // Threshold ratio for confident vs ambiguous hit

static_assert(ACTIVE_SENSORS >= 1 && ACTIVE_SENSORS <= 4, "Active sensors must be between 1 and 4");

// ---------------- SENSOR DATA & STATE ----------------
float baseline[4] = {};
uint16_t threshold[4] = {};
uint16_t raw[4] = {};
uint16_t peaks[4] = {};
uint16_t rawAtPeak[4] = {};
uint16_t windowMin[4] = {4095, 4095, 4095, 4095};
uint16_t windowMax[4] = {};

bool calibrationOK = false;
bool capturing = false;
bool armed = false;
bool quietTracking = false;
bool cooldownActive = false;

uint32_t captureStart = 0;
uint32_t quietSince = 0;
uint32_t cooldownStart = 0;
uint32_t nextSample = 0;
uint32_t lastStatus = 0;
uint32_t eventCount = 0;

bool adcPin(uint8_t pin) {
  // Valid analog pins on XIAO ESP32-C3: GPIO 2, 3, 4, 5
  return pin == 2 || pin == 3 || pin == 4 || pin == 5;
}

bool detectionEnabled() {
  return LARGE_SPIKE_MODE || calibrationOK;
}

void readSensors() {
  for (uint8_t i = 0; i < ACTIVE_SENSORS; ++i) {
    raw[i] = analogRead(PINS[i]);
    if (raw[i] < windowMin[i]) windowMin[i] = raw[i];
    if (raw[i] > windowMax[i]) windowMax[i] = raw[i];
  }
}

uint16_t deviation(uint8_t i) {
  return (uint16_t)fabsf((float)raw[i] - baseline[i]);
}

void printArray(const uint16_t values[4]) {
  Serial.print('[');
  for (uint8_t i = 0; i < 4; ++i) {
    if (i > 0) Serial.print(',');
    if (i < ACTIVE_SENSORS) Serial.print(values[i]);
    else Serial.print("null");
  }
  Serial.print(']');
}

// ---------------- JSON STATUS STREAM ----------------
void status(const char *type) {
  Serial.printf(
    "{\"type\":\"%s\",\"firmware\":\"piezo-zones-1.2\",\"board\":\"XIAO-ESP32-C3\",\"uptime_ms\":%lu,\"active_sensors\":%u,\"pins\":[",
    type, (unsigned long)millis(), ACTIVE_SENSORS
  );
  for (uint8_t i = 0; i < 4; ++i) {
    if (i > 0) Serial.print(',');
    if (i < ACTIVE_SENSORS) Serial.print(PINS[i]); else Serial.print("null");
  }
  Serial.print("],\"readings\":"); printArray(raw);
  Serial.print(",\"thresholds\":"); printArray(threshold);
  Serial.print(",\"window_min\":"); printArray(windowMin);
  Serial.print(",\"window_max\":"); printArray(windowMax);
  Serial.print(",\"baseline\":[");
  for (uint8_t i = 0; i < 4; ++i) {
    if (i > 0) Serial.print(',');
    if (i < ACTIVE_SENSORS) Serial.print(baseline[i], 1); else Serial.print("null");
  }
  Serial.printf(
    "],\"mode\":\"%s\",\"calibration_ok\":%s,\"detection_enabled\":%s,\"armed\":%s,\"events\":%lu}\n",
    LARGE_SPIKE_MODE ? "large_spike" : "auto",
    calibrationOK ? "true" : "false",
    detectionEnabled() ? "true" : "false",
    armed ? "true" : "false",
    (unsigned long)eventCount
  );
  for (uint8_t i = 0; i < ACTIVE_SENSORS; ++i) {
    windowMin[i] = 4095;
    windowMax[i] = 0;
  }
}

// ---------------- CALIBRATION ----------------
void calibrate() {
  Serial.println("{\"type\":\"calibrating\",\"duration_ms\":1024}");
  capturing = armed = quietTracking = cooldownActive = false;
  calibrationOK = false;

  for (uint8_t i = 0; i < ACTIVE_SENSORS; ++i) {
    windowMin[i] = 4095;
    windowMax[i] = 0;
  }

  // 1. Measure DC baseline resting values
  constexpr uint16_t N = 512;
  uint32_t sum[4] = {};
  for (uint16_t n = 0; n < N; ++n) {
    readSensors();
    for (uint8_t i = 0; i < ACTIVE_SENSORS; ++i) sum[i] += raw[i];
    delay(1);
  }
  for (uint8_t i = 0; i < ACTIVE_SENSORS; ++i) baseline[i] = (float)sum[i] / N;

  // 2. Measure idle noise variance
  uint16_t noiseMax[4] = {};
  double noiseSquares[4] = {};
  for (uint16_t n = 0; n < N; ++n) {
    readSensors();
    for (uint8_t i = 0; i < ACTIVE_SENSORS; ++i) {
      uint16_t d = deviation(i);
      noiseSquares[i] += (double)d * d;
      if (d > noiseMax[i]) noiseMax[i] = d;
    }
    delay(1);
  }

  calibrationOK = true;
  for (uint8_t i = 0; i < ACTIVE_SENSORS; ++i) {
    float idleRMS = sqrtf((float)(noiseSquares[i] / N));
    float autoT = fmaxf(MIN_THRESHOLD[i], fmaxf(noiseMax[i] + 30.0f, 6.0f * idleRMS + 30.0f));
    threshold[i] = LARGE_SPIKE_MODE ? SPIKE_THRESHOLD[i] : (uint16_t)fminf(4095.0f, ceilf(autoT));

    float reachableDelta = fmaxf(baseline[i], 4095.0f - baseline[i]);
    if (autoT >= MAX_CALIBRATED_THRESHOLD || ceilf(autoT) >= reachableDelta) {
      calibrationOK = false;
      Serial.printf(
        "{\"type\":\"%s\",\"zone\":%u,\"reason\":\"unstable_input_or_unreachable_threshold\",\"required_threshold\":%.1f,\"idle_rms\":%.1f}\n",
        LARGE_SPIKE_MODE ? "calibration_warning" : "calibration_error",
        i + 1, autoT, idleRMS
      );
    }
  }

  nextSample = micros();
  lastStatus = millis();
  armed = true;
  status(detectionEnabled() ? "ready" : "not_ready");
}

// ---------------- FINISH IMPACT EVENT ----------------
void finishImpact() {
  int winner = -1;
  float best = 0.0f;
  float second = 0.0f;

  for (uint8_t i = 0; i < ACTIVE_SENSORS; ++i) {
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
    ++eventCount;

    Serial.printf(
      "{\"type\":\"hit\",\"event\":%lu,\"time_us\":%lu,\"zone\":%u,\"candidate_zone\":%u,\"ambiguous\":%s,\"peak\":%u,\"intensity\":%.3f,\"dominance\":",
      (unsigned long)eventCount,
      (unsigned long)captureStart,
      ambiguous ? 0 : winner + 1,
      winner + 1,
      ambiguous ? "true" : "false",
      peaks[winner],
      peaks[winner] / 4095.0f
    );

    if (second > 0.0f) Serial.print(best / second, 3); else Serial.print("null");
    Serial.print(",\"peaks\":"); printArray(peaks);
    Serial.print(",\"readings\":"); printArray(rawAtPeak);
    Serial.print(",\"thresholds\":"); printArray(threshold);
    Serial.printf(",\"clipped\":%s}\n", rawAtPeak[winner] >= 4090 ? "true" : "false");
  }

  capturing = false;
  armed = false;
  quietTracking = false;
  cooldownActive = true;
  cooldownStart = micros();
}

// ---------------- SETUP ----------------
void setup() {
  // Suppress ESP-IDF error logs from polluting JSON output to Serial
  esp_log_level_set("*", ESP_LOG_NONE);

  Serial.begin(115200);

  // Allow USB CDC serial on ESP32-C3 to connect
  delay(500);

  analogReadResolution(12);

  for (uint8_t i = 0; i < ACTIVE_SENSORS; ++i) {
    bool valid = adcPin(PINS[i]) && GAIN[i] > 0 &&
                 (!LARGE_SPIKE_MODE || (SPIKE_THRESHOLD[i] > 0 && SPIKE_THRESHOLD[i] <= 4095));
    for (uint8_t j = 0; j < i; ++j) if (PINS[i] == PINS[j]) valid = false;
    if (!valid) {
      Serial.println("{\"type\":\"config_error\",\"reason\":\"check_pins_gains_and_spike_threshold\"}");
      while (true) delay(1000);
    }

    pinMode(PINS[i], INPUT);
    analogSetPinAttenuation(PINS[i], ADC_11db); // Full 0 - ~3.3V range
  }

  delay(200);
  calibrate();
}

// ---------------- MAIN LOOP ----------------
void loop() {
  uint32_t now = micros();
  if ((int32_t)(now - nextSample) < 0) return;
  nextSample = now + SAMPLE_US;

  readSensors();

  // Slow DC baseline drift tracking when idle
  if (!capturing) {
    for (uint8_t i = 0; i < ACTIVE_SENSORS; ++i) {
      uint16_t d = deviation(i);
      if (d < threshold[i]) {
        baseline[i] += 0.0008f * ((float)raw[i] - baseline[i]);
      }
    }
  }

  bool triggered = false;
  bool quiet = true;
  for (uint8_t i = 0; i < ACTIVE_SENSORS; ++i) {
    uint16_t d = deviation(i);
    if (d >= threshold[i]) triggered = true;
    uint16_t release = LARGE_SPIKE_MODE ? (threshold[i] * 4u / 5u) : threshold[i] / 2;
    if (d >= release) quiet = false;
  }

  // Trigger impact acquisition
  if (detectionEnabled() && !capturing && armed && triggered) {
    capturing = true;
    armed = false;
    captureStart = now;
    for (uint8_t i = 0; i < ACTIVE_SENSORS; ++i) {
      peaks[i] = deviation(i);
      rawAtPeak[i] = raw[i];
    }
  }

  // Record peak values during impact window
  if (capturing) {
    for (uint8_t i = 0; i < ACTIVE_SENSORS; ++i) {
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

  // Rearm logic when signals return to quiet state
  if (detectionEnabled() && quiet) {
    if (!quietTracking) {
      quietSince = now;
      quietTracking = true;
    }
    bool settled = (uint32_t)(now - quietSince) >= QUIET_REARM_US;
    bool cooled  = !cooldownActive || ((uint32_t)(now - cooldownStart) >= COOLDOWN_US);
    if (settled && cooled) {
      armed = true;
      cooldownActive = false;
    }
  } else {
    quietTracking = false;
    // Safety auto-recovery: If signals get stuck un-rearmed for > 1.2s due to DC drift, re-snap baseline
    if (!armed && !capturing && (uint32_t)(now - quietSince) > 1200000UL) {
      for (uint8_t i = 0; i < ACTIVE_SENSORS; ++i) {
        baseline[i] = raw[i];
      }
      armed = true;
      cooldownActive = false;
      quietSince = now;
    }
  }

  // Handle serial incoming commands
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'c' || c == 'C') { calibrate(); return; }
    if (c == 's' || c == 'S') { status("status"); }
  }

  // Periodic status frame (1 Hz) for Dashboard
  if ((uint32_t)(millis() - lastStatus) >= 1000) {
    lastStatus = millis();
    status("status");
  }

  delay(0);
}
