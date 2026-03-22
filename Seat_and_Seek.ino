#include <WiFiNINA.h>
#include <ThingSpeak.h>
#include <Arduino_MKRENV.h>

char ssid[] = "YOUR_WIFI_NAME";
char pass[] = "YOUR_WIFI_PASSWORD";

const unsigned long CHANNEL_ID = 1234567UL;
const char *CHANNEL_WRITE_API_KEY = "YOUR_WRITE_API_KEY";
const char *CHANNEL_READ_API_KEY  = "YOUR_READ_API_KEY";

const unsigned int COMMAND_FIELD = 8;

WiFiClient client;

const int TRIG_PIN = 2;
const int ECHO_PIN = 3;
const int BUTTON_PIN = 7;

const int LED_R_PIN = 4;
const int LED_G_PIN = 5;
const int LED_B_PIN = 6;

const int SOUND_PIN = A1;

const bool USE_SOUND_SENSOR = true;
const bool RGB_COMMON_ANODE = false;

const unsigned long SENSOR_INTERVAL_MS = 500;
const unsigned long THINGSPEAK_WRITE_INTERVAL_MS = 20000;
const unsigned long THINGSPEAK_READ_INTERVAL_MS = 20000;
const unsigned long AWAY_HOLD_MS = 120000;
const unsigned long BUTTON_DEBOUNCE_MS = 250;

const float OCCUPIED_DISTANCE_CM = 45.0f;
const float DISTANCE_HYSTERESIS_CM = 8.0f;
const float MIN_VALID_DISTANCE_CM = 2.0f;
const float MAX_VALID_DISTANCE_CM = 200.0f;
const float DISTANCE_ALPHA = 0.35f;

const float MIN_GOOD_LUX = 200.0f;
const float GOOD_TEMP_MIN_C = 19.0f;
const float GOOD_TEMP_MAX_C = 27.0f;
const float GOOD_HUM_MIN = 30.0f;
const float GOOD_HUM_MAX = 70.0f;
const float BAD_NOISE_LEVEL = 120.0f;

enum SeatState {
  SEAT_OCCUPIED = 0,
  SEAT_FREE = 1,
  SEAT_TEMP_BUSY = 2,
  SEAT_RESERVED = 3,
  SEAT_OUT_OF_SERVICE = 4
};

enum CloudMode {
  CLOUD_NORMAL = 0,
  CLOUD_RESERVED = 1,
  CLOUD_OUT_OF_SERVICE = 2
};

enum LedColour {
  COLOUR_OFF = 0,
  COLOUR_GREEN = 1,
  COLOUR_YELLOW = 2,
  COLOUR_RED = 3
};

volatile bool buttonInterruptFired = false;

unsigned long lastButtonHandledMs = 0;
unsigned long lastSensorMs = 0;
unsigned long lastWriteMs = 0;
unsigned long lastReadMs = 0;
unsigned long awayStartedMs = 0;

bool awayActive = false;
bool occupied = false;

CloudMode cloudMode = CLOUD_NORMAL;
SeatState seatState = SEAT_FREE;

float smoothedDistanceCm = -1.0f;
float latestTempC = 0.0f;
float latestHumidity = 0.0f;
float latestLux = 0.0f;
float latestNoise = 0.0f;
int comfortScore = 100;

void onButtonInterrupt() {
  buttonInterruptFired = true;
}

void writeLedPin(int pin, int value) {
  int out = RGB_COMMON_ANODE ? (255 - value) : value;
  analogWrite(pin, out);
}

void setSeatColour(LedColour colour) {
  int r = 0, g = 0, b = 0;

  switch (colour) {
    case COLOUR_OFF:
      r = 0;   g = 0;   b = 0;
      break;
    case COLOUR_GREEN:
      r = 0;   g = 255; b = 0;
      break;
    case COLOUR_YELLOW:
      r = 255; g = 255; b = 0;
      break;
    case COLOUR_RED:
      r = 255; g = 0;   b = 0;
      break;
  }

  writeLedPin(LED_R_PIN, r);
  writeLedPin(LED_G_PIN, g);
  writeLedPin(LED_B_PIN, b);
}

float readUltrasonicCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  unsigned long duration = pulseIn(ECHO_PIN, HIGH, 25000);
  if (duration == 0) {
    return -1.0f;
  }

  return duration * 0.0343f / 2.0f;
}

float readNoiseLevel() {
  if (!USE_SOUND_SENSOR) {
    return 0.0f;
  }

  int minVal = 1023;
  int maxVal = 0;
  unsigned long startMs = millis();

  while (millis() - startMs < 40) {
    int sample = analogRead(SOUND_PIN);
    if (sample < minVal) minVal = sample;
    if (sample > maxVal) maxVal = sample;
  }

  return float(maxVal - minVal);
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    WiFi.begin(ssid, pass);

    unsigned long attemptStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - attemptStart < 10000) {
      delay(500);
      Serial.print(".");
    }
  }

  Serial.println();
  Serial.println("Wi-Fi connected");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

void updateDistanceAndOccupancy() {
  float distanceCm = readUltrasonicCm();

  if (distanceCm < MIN_VALID_DISTANCE_CM || distanceCm > MAX_VALID_DISTANCE_CM) {
    return;
  }

  if (smoothedDistanceCm < 0.0f) {
    smoothedDistanceCm = distanceCm;
  } else {
    smoothedDistanceCm = DISTANCE_ALPHA * distanceCm +
                         (1.0f - DISTANCE_ALPHA) * smoothedDistanceCm;
  }

  if (occupied) {
    if (smoothedDistanceCm > OCCUPIED_DISTANCE_CM + DISTANCE_HYSTERESIS_CM) {
      occupied = false;
    }
  } else {
    if (smoothedDistanceCm < OCCUPIED_DISTANCE_CM - DISTANCE_HYSTERESIS_CM) {
      occupied = true;
    }
  }
}

int computeComfortScore() {
  int score = 100;

  if (latestLux < MIN_GOOD_LUX) {
    score -= 25;
  }

  if (latestTempC < GOOD_TEMP_MIN_C || latestTempC > GOOD_TEMP_MAX_C) {
    score -= 25;
  }

  if (latestHumidity < GOOD_HUM_MIN || latestHumidity > GOOD_HUM_MAX) {
    score -= 15;
  }

  if (USE_SOUND_SENSOR && latestNoise > BAD_NOISE_LEVEL) {
    score -= 20;
  }

  if (score < 0) score = 0;
  if (score > 100) score = 100;
  return score;
}

String seatStateText() {
  switch (seatState) {
    case SEAT_OCCUPIED:       return "occupied";
    case SEAT_FREE:           return "free";
    case SEAT_TEMP_BUSY:      return "temporary_busy";
    case SEAT_RESERVED:       return "reserved_from_cloud";
    case SEAT_OUT_OF_SERVICE: return "out_of_service";
    default:                  return "unknown";
  }
}

String comfortText() {
  if (comfortScore >= 80) return "good";
  if (comfortScore >= 60) return "fair";
  return "poor";
}

void applyActuator() {
  switch (seatState) {
    case SEAT_OCCUPIED:
      setSeatColour(COLOUR_OFF);
      break;
    case SEAT_FREE:
      setSeatColour(COLOUR_GREEN);
      break;
    case SEAT_TEMP_BUSY:
      setSeatColour(COLOUR_YELLOW);
      break;
    case SEAT_RESERVED:
    case SEAT_OUT_OF_SERVICE:
      setSeatColour(COLOUR_RED);
      break;
  }
}

void updateSeatState() {
  if (awayActive && (millis() - awayStartedMs >= AWAY_HOLD_MS)) {
    awayActive = false;
  }

  if (cloudMode == CLOUD_OUT_OF_SERVICE) {
    seatState = SEAT_OUT_OF_SERVICE;
  } else if (cloudMode == CLOUD_RESERVED) {
    seatState = SEAT_RESERVED;
  } else if (awayActive) {
    seatState = SEAT_TEMP_BUSY;
  } else if (occupied) {
    seatState = SEAT_OCCUPIED;
  } else {
    seatState = SEAT_FREE;
  }

  applyActuator();
}

void handleButtonEvent() {
  if (!buttonInterruptFired) {
    return;
  }

  noInterrupts();
  buttonInterruptFired = false;
  interrupts();

  unsigned long now = millis();
  if (now - lastButtonHandledMs < BUTTON_DEBOUNCE_MS) {
    return;
  }
  lastButtonHandledMs = now;

  if (cloudMode != CLOUD_NORMAL) {
    return;
  }

  if (awayActive) {
    awayActive = false;
    return;
  }

  if (occupied) {
    awayActive = true;
    awayStartedMs = now;
  }
}

void updateSensors() {
  updateDistanceAndOccupancy();

  latestTempC = ENV.readTemperature();
  latestHumidity = ENV.readHumidity();
  latestLux = ENV.readIlluminance();
  latestNoise = readNoiseLevel();

  comfortScore = computeComfortScore();
}

void readCloudCommand() {
  connectWiFi();

  long cmd = ThingSpeak.readLongField(CHANNEL_ID, COMMAND_FIELD, CHANNEL_READ_API_KEY);
  int readStatus = ThingSpeak.getLastReadStatus();

  if (readStatus == 200) {
    if (cmd < 0 || cmd > 2) {
      cmd = 0;
    }
    cloudMode = static_cast<CloudMode>(cmd);
  } else {
    Serial.print("ThingSpeak read error: ");
    Serial.println(readStatus);
  }
}

void writeDataToThingSpeak() {
  connectWiFi();

  ThingSpeak.setField(1, smoothedDistanceCm < 0.0f ? 0.0f : smoothedDistanceCm);
  ThingSpeak.setField(2, latestLux);
  ThingSpeak.setField(3, latestTempC);
  ThingSpeak.setField(4, latestHumidity);
  ThingSpeak.setField(5, static_cast<int>(seatState));
  ThingSpeak.setField(6, comfortScore);
  ThingSpeak.setField(7, latestNoise);
  ThingSpeak.setField(8, static_cast<int>(cloudMode));

  String statusText = seatStateText() + " | comfort=" + comfortText();
  ThingSpeak.setStatus(statusText);

  int writeStatus = ThingSpeak.writeFields(CHANNEL_ID, CHANNEL_WRITE_API_KEY);
  if (writeStatus != 200) {
    Serial.print("ThingSpeak write error: ");
    Serial.println(writeStatus);
  } else {
    Serial.print("Uploaded: ");
    Serial.println(statusText);
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 4000) { }

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  pinMode(LED_R_PIN, OUTPUT);
  pinMode(LED_G_PIN, OUTPUT);
  pinMode(LED_B_PIN, OUTPUT);

  if (USE_SOUND_SENSOR) {
    pinMode(SOUND_PIN, INPUT);
  }

  analogReadResolution(10);
  analogWriteResolution(8);

  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), onButtonInterrupt, FALLING);

  setSeatColour(COLOUR_GREEN);

  if (!ENV.begin()) {
    Serial.println("Failed to initialize MKR ENV Shield");
    setSeatColour(COLOUR_RED);
    while (true) { }
  }

  connectWiFi();
  ThingSpeak.begin(client);

  updateSensors();
  readCloudCommand();
  updateSeatState();
  writeDataToThingSpeak();

  lastSensorMs = millis();
  lastReadMs = millis();
  lastWriteMs = millis();
}

void loop() {
  unsigned long now = millis();

  handleButtonEvent();

  if (now - lastSensorMs >= SENSOR_INTERVAL_MS) {
    lastSensorMs = now;
    updateSensors();
    updateSeatState();
  }

  if (now - lastReadMs >= THINGSPEAK_READ_INTERVAL_MS) {
    lastReadMs = now;
    readCloudCommand();
    updateSeatState();
  }

  if (now - lastWriteMs >= THINGSPEAK_WRITE_INTERVAL_MS) {
    lastWriteMs = now;
    writeDataToThingSpeak();
  }
}