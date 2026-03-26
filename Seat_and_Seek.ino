#include <WiFiNINA.h>
#include <ThingSpeak.h>
#include <Arduino_MKRENV.h>
#include <utility/wifi_drv.h>

char ssid[] = "wifi_name";
char pass[] = "wifi_password";

const unsigned long CHANNEL_ID = 3287884;
const char *CHANNEL_WRITE_API_KEY = "SXEUC422YV4UT9PF";
const char *CHANNEL_READ_API_KEY = "FSDLNE6W9Z9867HA";

const unsigned long DEBUG_PRINT_INTERVAL_MS = 1000;
unsigned long lastDebugPrintMs = 0;

// Field 8 is the remote override from the cloud.
// 0 = normal
// 1 = reserved
// 2 = out of service
const unsigned int COMMAND_FIELD = 8;

WiFiClient client;

// Hardware layout
const int ULTRASONIC_PIN = 2;
const int BUTTON_PIN = 1;
const int SOUND_PIN = A0;

const uint8_t RGB_RED_PIN = 25;
const uint8_t RGB_GREEN_PIN = 26;
const uint8_t RGB_BLUE_PIN = 27;

// Normal timing while the seat is active
const unsigned long NORMAL_SENSOR_INTERVAL_MS = 500;
const unsigned long NORMAL_READ_INTERVAL_MS = 20000;
const unsigned long NORMAL_WRITE_INTERVAL_MS = 20000;

// Slower timing while the seat is free and there is no remote override
const unsigned long IDLE_SENSOR_INTERVAL_MS = 2000;
const unsigned long IDLE_READ_INTERVAL_MS = 30000;
const unsigned long IDLE_WRITE_INTERVAL_MS = 60000;

// The seat stays temporarily busy for this long after the user leaves
const unsigned long AWAY_HOLD_MS = 150000;

// Simple debounce for the button interrupt
const unsigned long BUTTON_DEBOUNCE_MS = 250;

// Occupancy thresholds
const float OCCUPIED_DISTANCE_CM = 45.0f;
const float DISTANCE_HYSTERESIS_CM = 8.0f;
const float MIN_VALID_DISTANCE_CM = 2.0f;
const float MAX_VALID_DISTANCE_CM = 200.0f;
const float DISTANCE_ALPHA = 0.35f;

volatile bool buttonInterruptFired = false;

unsigned long lastButtonHandledMs = 0;
unsigned long lastSensorMs = 0;
unsigned long lastWriteMs = 0;
unsigned long lastReadMs = 0;
unsigned long awayStartedMs = 0;

bool occupied = false;

// awayPressed means the user pressed the button while still seated.
// awayActive means they have left and the seat is being held temporarily.
bool awayPressed = false;
bool awayActive = false;

float smoothedDistanceCm = -1.0f;
float latestTemp = 0.0f;
float latestHumidity = 0.0f;
float latestLightLevel = 0.0f;
float latestNoise = 0.0f;
String lastUploadedStatusText = "";

enum SeatState
{
  SEAT_OCCUPIED = 0,
  SEAT_FREE = 1,
  SEAT_TEMP_BUSY = 2,
  SEAT_RESERVED = 3,
  SEAT_OUT_OF_SERVICE = 4
};

enum CloudOverride
{
  OVERRIDE_NORMAL = 0,
  OVERRIDE_RESERVED = 1,
  OVERRIDE_OUT_OF_SERVICE = 2
};

SeatState seatState = SEAT_FREE;
CloudOverride cloudOverride = OVERRIDE_NORMAL;

void onButtonInterrupt() {
  buttonInterruptFired = true;
}

void setupBuiltinRGB()
{
  WiFiDrv::pinMode(RGB_RED_PIN, OUTPUT);
  WiFiDrv::pinMode(RGB_GREEN_PIN, OUTPUT);
  WiFiDrv::pinMode(RGB_BLUE_PIN, OUTPUT);
}

void setBuiltinRGB(uint8_t red, uint8_t green, uint8_t blue)
{
  WiFiDrv::analogWrite(RGB_RED_PIN, red);
  WiFiDrv::analogWrite(RGB_GREEN_PIN, green);
  WiFiDrv::analogWrite(RGB_BLUE_PIN, blue);
}

float readUltrasonicCm() {
  pinMode(ULTRASONIC_PIN, OUTPUT);
  digitalWrite(ULTRASONIC_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(ULTRASONIC_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(ULTRASONIC_PIN, LOW);

  pinMode(ULTRASONIC_PIN, INPUT);
  unsigned long duration = pulseIn(ULTRASONIC_PIN, HIGH, 30000);

  if (duration == 0) {
    return -1.0f;
  }

  return duration * 0.0343f / 2.0f;
}

float readNoiseLevel()
{
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
}

void updateDistanceAndOccupancy() {
  float distanceCm = readUltrasonicCm();

  if (distanceCm < MIN_VALID_DISTANCE_CM || distanceCm > MAX_VALID_DISTANCE_CM) {
    return;
  }

  if (smoothedDistanceCm < 0.0f) {
    smoothedDistanceCm = distanceCm;
  } else {
    smoothedDistanceCm =
        DISTANCE_ALPHA * distanceCm +
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

bool lowPowerModeActive()
{
  return !occupied &&
         !awayPressed &&
         !awayActive &&
         cloudOverride == OVERRIDE_NORMAL;
}

unsigned long currentSensorInterval()
{
  return lowPowerModeActive() ? IDLE_SENSOR_INTERVAL_MS : NORMAL_SENSOR_INTERVAL_MS;
}

unsigned long currentReadInterval()
{
  return lowPowerModeActive() ? IDLE_READ_INTERVAL_MS : NORMAL_READ_INTERVAL_MS;
}

unsigned long currentWriteInterval()
{
  return lowPowerModeActive() ? IDLE_WRITE_INTERVAL_MS : NORMAL_WRITE_INTERVAL_MS;
}

int powerModeCode()
{
  return lowPowerModeActive() ? 1 : 0;
}

String powerModeText()
{
  return lowPowerModeActive() ? "low_power" : "normal_power";
}

String seatStateText() {
  switch (seatState) {
  case SEAT_OCCUPIED:
    return "occupied";
  case SEAT_FREE:
    return "free";
  case SEAT_TEMP_BUSY:
    return "temporarily_busy";
  case SEAT_RESERVED:
    return "reserved";
  case SEAT_OUT_OF_SERVICE:
    return "out_of_service";
  default:
    return "unknown";
  }
}

String cloudOverrideText()
{
  switch (cloudOverride)
  {
  case OVERRIDE_NORMAL:
    return "normal";
  case OVERRIDE_RESERVED:
    return "reserved";
  case OVERRIDE_OUT_OF_SERVICE:
    return "out_of_service";
  default:
    return "unknown";
  }
}

String noiseLevelText()
{
  if (latestNoise < 20.0f)
    return "silent_study";
  if (latestNoise < 60.0f)
    return "quiet_study";
  if (latestNoise < 120.0f)
    return "quiet_discussion";
  return "discussion";
}

String lightLevelText()
{
  if (latestLightLevel < 5.0f)
    return "very_dim";
  if (latestLightLevel < 20.0f)
    return "dim";
  if (latestLightLevel < 60.0f)
    return "ambient";
  if (latestLightLevel < 80.0f)
    return "bright";
  return "very_bright";
}

String temperatureLevelText()
{
  if (latestTemp < 17.0f)
    return "cool";
  if (latestTemp < 24.0f)
    return "comfortable";
  if (latestTemp < 27.0f)
    return "warm";
  return "hot";
}

String humidityLevelText()
{
  if (latestHumidity < 30.0f)
    return "dry";
  if (latestHumidity < 60.0f)
    return "comfortable";
  return "humid";
}

void applyActuator() {
  switch (seatState) {
    case SEAT_OCCUPIED:
      setBuiltinRGB(0, 0, 0);
      break;
    case SEAT_FREE:
      setBuiltinRGB(0, 255, 0); // green
      break;
    case SEAT_TEMP_BUSY:
    case SEAT_RESERVED:
      setBuiltinRGB(255, 180, 0); // yellow
      break;
    case SEAT_OUT_OF_SERVICE:
      setBuiltinRGB(255, 0, 0); // red
      break;
  }
}

void updateAwayState()
{
  unsigned long now = millis();

  // The button was pressed while the user was still seated.
  // Once the seat becomes empty, start the temporary hold timer.
  if (awayPressed && !occupied)
  {
    awayPressed = false;
    awayActive = true;
    awayStartedMs = now;
  }

  // If the user comes back before the timer ends, cancel the hold.
  if (awayActive && occupied)
  {
    awayActive = false;
  }

  // End the temporary hold when the timer expires.
  if (awayActive && (now - awayStartedMs >= AWAY_HOLD_MS))
  {
    awayActive = false;
  }
}

void updateSeatState()
{
  updateAwayState();

  if (cloudOverride == OVERRIDE_OUT_OF_SERVICE)
  {
    seatState = SEAT_OUT_OF_SERVICE;
  }
  else if (cloudOverride == OVERRIDE_RESERVED)
  {
    seatState = SEAT_RESERVED;
  }
  else if (awayActive)
  {
    seatState = SEAT_TEMP_BUSY;
  }
  else if (occupied)
  {
    seatState = SEAT_OCCUPIED;
  }
  else
  {
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

  if (cloudOverride != OVERRIDE_NORMAL)
  {
    return;
  }

  // Press once while seated to request a temporary hold after leaving.
  // Press again to cancel it.
  if (awayPressed || awayActive)
  {
    awayPressed = false;
    awayActive = false;
    return;
  }

  if (occupied) {
    awayPressed = true;
  }
}

void updateSensors() {
  updateDistanceAndOccupancy();

  latestTemp = ENV.readTemperature();
  latestHumidity = ENV.readHumidity();
  latestLightLevel = ENV.readIlluminance();
  latestNoise = readNoiseLevel();
}

void readCloudOverride()
{
  connectWiFi();

  long cmd = ThingSpeak.readLongField(CHANNEL_ID, COMMAND_FIELD, CHANNEL_READ_API_KEY);
  int readStatus = ThingSpeak.getLastReadStatus();

  if (readStatus == 200) {
    if (cmd < 0 || cmd > 2) {
      cmd = 0;
    }
    cloudOverride = static_cast<CloudOverride>(cmd);
  } else {
    Serial.print("ThingSpeak read error: ");
    Serial.println(readStatus);
  }
}

bool writeDataToThingSpeak()
{
  connectWiFi();

  ThingSpeak.setField(1, smoothedDistanceCm < 0.0f ? 0.0f : smoothedDistanceCm);
  ThingSpeak.setField(2, latestLightLevel);
  ThingSpeak.setField(3, latestTemp);
  ThingSpeak.setField(4, latestHumidity);
  ThingSpeak.setField(5, static_cast<int>(seatState));
  ThingSpeak.setField(6, powerModeCode());
  ThingSpeak.setField(7, latestNoise);
  String statusText = buildStatusText();
  if (statusText != lastUploadedStatusText)
  {
    ThingSpeak.setStatus(statusText);
  }

  int writeStatus = ThingSpeak.writeFields(CHANNEL_ID, CHANNEL_WRITE_API_KEY);
  if (writeStatus != 200) {
    Serial.print("ThingSpeak write error: ");
    Serial.println(writeStatus);
    return false;
  } else {
    if (statusText != lastUploadedStatusText)
    {
      lastUploadedStatusText = statusText;
    }
    Serial.print("Status updated: ");
    Serial.println(statusText);
  }

  return true;
}

String buildStatusText()
{
  return String("seat=") + seatStateText() +
         " | noise=" + noiseLevelText() +
         " | light=" + lightLevelText() +
         " | temp=" + temperatureLevelText() +
         " | humidity=" + humidityLevelText() +
         " | power=" + powerModeText() +
         " | override=" + cloudOverrideText();
}

void printDebugReadings()
{
  Serial.print("distance=");
  Serial.print(smoothedDistanceCm);

  Serial.print(" | occupied=");
  Serial.print(occupied ? "yes" : "no");

  Serial.print(" | seat=");
  Serial.print(seatStateText());

  Serial.print(" | override=");
  Serial.print(cloudOverrideText());

  Serial.print(" | noiseRaw=");
  Serial.print(latestNoise);
  Serial.print(" | noiseLevel=");
  Serial.print(noiseLevelText());

  Serial.print(" | lightLevel=");
  Serial.print(latestLightLevel);
  Serial.print(" | lightLevel=");
  Serial.print(lightLevelText());

  Serial.print(" | tempC=");
  Serial.print(latestTemp);
  Serial.print(" | tempLevel=");
  Serial.print(temperatureLevelText());

  Serial.print(" | humidity=");
  Serial.print(latestHumidity);
  Serial.print(" | humidityLevel=");
  Serial.print(humidityLevelText());

  Serial.print(" | awayPressed=");
  Serial.print(awayPressed ? "yes" : "no");

  Serial.print(" | awayActive=");
  Serial.print(awayActive ? "yes" : "no");

  Serial.print(" | power=");
  Serial.println(powerModeText());
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 4000) { }

  pinMode(SOUND_PIN, INPUT);
  pinMode(BUTTON_PIN, INPUT);

  setupBuiltinRGB();
  setBuiltinRGB(0, 255, 0);

  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), onButtonInterrupt, RISING);

  if (!ENV.begin()) {
    Serial.println("Failed to initialize MKR ENV Shield");
    setBuiltinRGB(255, 0, 0);
    while (true) { }
  }

  connectWiFi();
  ThingSpeak.begin(client);

  updateSensors();
  readCloudOverride();
  updateSeatState();

  lastSensorMs = millis();
  lastReadMs = millis();
  lastWriteMs = 0;
}

void loop() {
  unsigned long now = millis();

  handleButtonEvent();

  if (now - lastSensorMs >= currentSensorInterval())
  {
    lastSensorMs = now;
    updateSensors();
    updateSeatState();
  }

  if (now - lastReadMs >= currentReadInterval())
  {
    lastReadMs = now;
    readCloudOverride();
    updateSeatState();
  }

  if (now - lastWriteMs >= currentWriteInterval())
  {
    writeDataToThingSpeak();
    lastWriteMs = now;
  }

  if (now - lastDebugPrintMs >= DEBUG_PRINT_INTERVAL_MS)
  {
    lastDebugPrintMs = now;
    printDebugReadings();
  }
}
