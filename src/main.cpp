// ECSE3038 - Tutorial 9 starter. Your job: TODOs 1-4.
// Every 10 seconds: read the DHT22 and POST {"mac", "temp"} to your API (as in Tutorial 8).
// New: the limit is no longer fetched with GET every 10 s. The board subscribes to
// ecse3038/<name>/limit on the class broker, and the limit is pushed to it the moment
// it changes. The broker keeps the last limit (retained), so a board that restarts
// gets it straight away.
//   ecse3038/<name>/limit      desired: the limit someone wants (retained)
//   ecse3038/<name>/led/state  reported: what the LED really is, on or off (retained)
//   ecse3038/<name>/status     online, or offline from the last will (retained)
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <DHTesp.h>
#include <PubSubClient.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#define WIFI_SSID "Wokwi-GUEST"
#define WIFI_PASSWORD ""
#define WIFI_CHANNEL 6
#endif

// Your server from Tutorial 6. http, not https, and no / at the end.
const char *API_URL = "http://iot-mwhittingham.northcentralus.cloudapp.azure.com";

// Your name on the class broker: lowercase letters and digits only, no spaces.
// The same name you used on Monday, so your tile on the class board is yours.
const char *MQTT_NAME = "thisisme";

// The class broker on Phillip's VM.
const char *BROKER_HOST = "135.232.203.144";
const uint16_t BROKER_PORT = 1883;

const unsigned long WIFI_TIMEOUT_MS = 15000;
const unsigned long RETRY_EVERY_MS = 5000;    // wait between reconnect attempts
const int DHT_PIN = 15;                       // the sensor's data wire
const unsigned long SEND_EVERY_MS = 10000;    // one reading every 10 s
const int LED_PIN = 2;                        // the LED, through a 220 ohm resistor

DHTesp dht;
WiFiClient net;                               // the TCP connection to the broker
PubSubClient mqtt(net);                       // MQTT on top of it

String statusTopic;                           // ecse3038/<name>/status
String limitTopic;                            // ecse3038/<name>/limit
String ledStateTopic;                         // ecse3038/<name>/led/state
String limitStateTopic;

unsigned long lastSend = 0;
unsigned long lastAttempt = 0;
float tempLimit = NAN;                        // no limit until the broker gives one
float lastTemp = NAN;                         // no reading yet
String reportedLed = "";                      // what we last told the broker: "on", "off" or nothing yet

bool connectWiFi() {
  Serial.print("Connecting to ");
  Serial.print(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_TIMEOUT_MS) {
      Serial.println();
      Serial.println("No Wi-Fi after 15 s. Trying again in 5 s.");
      return false;
    }
    delay(250);
    Serial.print(".");
  }

  Serial.println(" connected");
  Serial.print("MAC address: ");
  Serial.println(WiFi.macAddress());
  return true;
}

// As in Tutorial 8: send one reading. The server adds the time.
void sendReading(float temp) {
  char body[80];
  snprintf(body, sizeof(body), "{\"mac\": \"%s\", \"temp\": %.1f}",
           WiFi.macAddress().c_str(), temp);

  HTTPClient http;
  http.begin(String(API_URL) + "/readings");
  http.addHeader("Content-Type", "application/json");
  int status = http.POST(body);

  Serial.print("POST ");
  Serial.print(body);
  Serial.print(" -> ");
  Serial.println(status);
  if (status == 404) {
    Serial.println("Unknown MAC: register this board with POST /devices first.");
  } else if (status < 0) {
    Serial.println(http.errorToString(status));
  }
  http.end();
}

// Turns a message such as "30" or "27.5" into a number.
// Returns false for anything that is not a number ("abc", "30C", or empty),
// so a bad message never becomes a limit of 0.
bool parseLimit(const String &message, float &limit) {
  if (message.length() == 0) {
    return false;
  }
  char *end;
  float value = strtof(message.c_str(), &end);
  if (*end != '\0') {
    return false;                             // something after the number
  }
  limit = value;
  return true;
}

// LED on when the temperature is above the limit. Then tell the broker what the
// LED really is, but only when that has changed.
void updateLed() {
  bool tooHot = false;
  if (!isnan(tempLimit) && !isnan(lastTemp) && lastTemp > tempLimit) {
    tooHot = true;
  }

  String led = "off";
  if (tooHot) {
    led = "on";
  }
  if (tooHot) {
    digitalWrite(LED_PIN, HIGH);
  } else {
    digitalWrite(LED_PIN, LOW);
  }

  Serial.print("Limit: ");
  Serial.print(tempLimit);
  Serial.print("  Temperature: ");
  Serial.print(lastTemp);
  Serial.print("  -> LED ");
  Serial.println(led);

  // TODO 4: if led is different from reportedLed, and mqtt.connected():
  //         publish led on ledStateTopic, retained (true),
  //         then set reportedLed = led so the same state is not sent again.
  if (led != reportedLed && mqtt.connected()) {
    mqtt.publish(ledStateTopic.c_str(), led.c_str(), true);
    reportedLed = led;
  }
}

// PubSubClient calls this from mqtt.loop() for every message on a topic we subscribed to.
void onMessage(char *topic, byte *payload, unsigned int length) {
  String message;
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  message.trim();

  Serial.print("Got ");
  Serial.print(topic);
  Serial.print(" = ");
  Serial.println(message);

  String t = topic;
  
  // TODO 3: if t is limitTopic:
  //         float newLimit;
  //         if parseLimit(message, newLimit) is true, set tempLimit = newLimit
  //         and call updateLed(), so the LED reacts now, not at the next reading.
  //         Otherwise print "Not a number: limit unchanged."
  if (t == limitTopic) {
    float newLimit;
    if (parseLimit(message, newLimit)) {
      tempLimit = newLimit;
      updateLed();
    mqtt.publish(limitStateTopic.c_str(), String(tempLimit, 1).c_str(), true);
    } else {
      Serial.println("Not a number: limit unchanged.");
    }
  }
}

bool connectMqtt() {
  // Every client on the broker needs a different ID. Wokwi gives every student's
  // board the same chip ID, so your name is what keeps yours different.
  String clientId = "esp32-" + String(MQTT_NAME) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.print("Connecting to broker ");
  Serial.print(BROKER_HOST);
  Serial.print(" as ");
  Serial.println(clientId);

  // TODO 1: connect with a last will. The five arguments are:
  //         client ID, will topic (statusTopic), will QoS (1),
  //         will retained (true), will message ("offline").
  //         Remember .c_str() on the Strings.
  
bool ok = mqtt.connect(
    clientId.c_str(),
    statusTopic.c_str(),
    1,
    true,
    "offline"
);
  // replace false with your mqtt.connect(...) call
  if (!ok) {
    Serial.print("Broker said no, state ");
    Serial.println(mqtt.state());             // -2 = no TCP connection at all
    return false;
  }
  Serial.println("Connected to broker");
  mqtt.publish(
      statusTopic.c_str(),
      "online",
      true
  );

  mqtt.subscribe(limitTopic.c_str());
  // TODO 1 (continued): publish "online" on statusTopic, retained (true).

  // TODO 2: subscribe to limitTopic. The broker sends the retained limit straight away.

  reportedLed = "";                           // a new connection: report the LED again
  return true;
}

void setup() {
  Serial.begin(115200);
  Serial.println("Hello from the ESP32");
  dht.setup(DHT_PIN, DHTesp::DHT22);
  pinMode(LED_PIN, OUTPUT);

  String base = "ecse3038/" + String(MQTT_NAME);
  statusTopic = base + "/status";
  limitTopic = base + "/limit";
  ledStateTopic = base + "/led/state";
  limitStateTopic = base + "/limit/state";

  mqtt.setServer(BROKER_HOST, BROKER_PORT);
  mqtt.setCallback(onMessage);
  mqtt.setKeepAlive(15);                      // broker gives up on us after ~1.5 x 15 s of silence

  connectWiFi();
}

void loop() {
  // Wi-Fi first: without it, neither the broker nor your API can be reached.
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastAttempt >= RETRY_EVERY_MS) {
      lastAttempt = millis();
      connectWiFi();
    }
    return;
  }

  // The broker: reconnect if needed, at most one try every 5 s.
  if (mqtt.connected()) {
    mqtt.loop();                              // keepalive pings, and messages to onMessage()
  } else if (millis() - lastAttempt >= RETRY_EVERY_MS) {
    lastAttempt = millis();
    connectMqtt();
  }

  // Readings, as in Tutorial 8. No GET /settings any more.
  if (millis() - lastSend < SEND_EVERY_MS) {
    return;
  }
  lastSend = millis();

  float temp = dht.getTemperature();
  if (isnan(temp)) {
    Serial.println("No reading from the sensor. Check the three wires.");
    return;
  }
  lastTemp = temp;
  sendReading(temp);
  updateLed();
}