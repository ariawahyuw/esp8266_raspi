#include <Wire.h>  // I2C library 
#include <BH1750.h>
#include "DHT.h"
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <time.h>
#include <TZ.h>
#include <FS.h>
#include <LittleFS.h>
#include <CertStoreBearSSL.h>

#define target_pin D0  // act as actuator
#define analog_pin A0
#define s0_pin D5
#define s1_pin D6
#define scl_pin D1
#define sda_pin D2
#define dht_pin D4
#define dht_type DHT11
#define send_button D8
#define MSG_BUFFER_SIZE (500)

char msg[MSG_BUFFER_SIZE];
int value = 0;
int button_state, target, moisture_value, raindrop_value, x;
float t, h;  //temperature and humidity variable
unsigned long lastMsg = 0;

// Update these with values suitable for your network.
String api_key = "...";     //  Write API key from ThingSpeak
const char* ssid =  "...";     //wifi ssid and wpa2 key
const char* pass =  "...";
const char* server = "...";
//For MQTT
const char* mqtt_server = "...";
const char* mqtt_sub_topic = "my/predict";
const char* mqtt_username = "...";
const char* mqtt_password = "...";
const char* mqtt_pub_topic = "my/check";

void checkIncomingMessage(int times=240);
BearSSL::CertStore certStore;
BH1750 lightMeter;
DHT dht(dht_pin, dht_type);
WiFiClient espClient;
PubSubClient * client;

void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  randomSeed(micros());
  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
}

void setDateTime() {
  // You can use your own timezone, but the exact time is not used at all.
  // Only the date is needed for validating the certificates.
  configTime(TZ_Europe_Berlin, "pool.ntp.org", "time.nist.gov");
  Serial.print("Waiting for NTP time sync: ");
  time_t now = time(nullptr);
  while (now < 8 * 3600 * 2) {
    delay(100);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println();
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);
  Serial.printf("%s %s", tzname[0], asctime(&timeinfo));
}

void callback(char* topic, byte* payload, unsigned int length) {
  // Receive message from MQTT communication. If the message is "1", activate the actuator. Otherwise, deactivate the actuator
  String message = "";
  for (int i = 0; i < length; i++) {
    message = message + (char)payload[i];
  }
  if(message == "1"){
    digitalWrite(target_pin, HIGH);
    delay(100);
  }
  else {
    digitalWrite(target_pin, LOW);  //deactivate actuator
  }
}

void reconnect() {
  // Loop until we’re reconnected
  while (!client->connected()) {
    Serial.print("Attempting MQTT connection…");
    String clientId = "ESP8266Client - MyClient";
    // Attempt to connect
    // Insert your password
    if (client->connect(clientId.c_str(), mqtt_username, mqtt_password)) {
      Serial.println("connected");
      // … and resubscribe
      client->subscribe(mqtt_sub_topic);
    } else {
      Serial.print("failed, rc = ");
      Serial.print(client->state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

void setup() {
  delay(200);
  Serial.begin(9600);
  LittleFS.begin();
  setup_wifi();
  setDateTime();
  pinMode(s0_pin, OUTPUT);
  pinMode(s1_pin, OUTPUT);
  pinMode(target_pin, OUTPUT); // actuator pin
  pinMode(send_button, INPUT);
  pinMode(LED_BUILTIN, OUTPUT);  // Initialize the LED_BUILTIN pin as an output
  dht.begin();
  Wire.begin(sda_pin, scl_pin);
  lightMeter.begin();

  //Connect to MQTT server using certificates
  int numCerts = certStore.initCertStore(LittleFS, PSTR("/certs.idx"), PSTR("/certs.ar"));
  Serial.printf("Number of CA certs read: %d\n", numCerts);
  if (numCerts == 0) {
    Serial.printf("No certs found. Did you run certs-from-mozilla.py and upload the LittleFS directory before running?\n");
    return; // Can't connect to anything w/o certs!
  }
  BearSSL::WiFiClientSecure *bear = new BearSSL::WiFiClientSecure();
  // Integrate the cert store with this connection
  bear->setCertStore(&certStore);
  client = new PubSubClient(*bear);
  client->setServer(mqtt_server, 8883);
  client->setCallback(callback);
}

void loop() {
  if (!client->connected()) {
    reconnect();
  }
  client->loop();
  //Read data from DHT11
  dhtRead(&t, &h);
  checkIncomingMessage();

  //Read data from BH1750
  float lux = lightMeter.readLightLevel();
  Serial.print("Light Meter: ");
  Serial.print(lux);
  Serial.println(" lx");
  checkIncomingMessage();

  // Change the multiplexer channel to C0
  digitalWrite(s0_pin, LOW);
  digitalWrite(s1_pin, LOW);
  
  readAnalogInput(&moisture_value, analog_pin);
  Serial.print("Soil Moisture: ");
  Serial.println(moisture_value);
  checkIncomingMessage(); 

  // Change the multiplexer channel to C1
  digitalWrite(s0_pin, HIGH);
  digitalWrite(s1_pin, LOW);
  readAnalogInput(&raindrop_value, analog_pin);
  Serial.print("Rain: ");
  Serial.println(raindrop_value);
  checkIncomingMessage();
  
  Serial.print("\n");
  if (espClient.connect(server,80)){   //   "184.106.153.149" or api.thingspeak.com  
    //Send data to Thingspeak API
    String data_to_send = api_key;
    data_to_send += "&field1=";
    data_to_send += h;
    data_to_send += "&field2=";
    data_to_send += t;
    data_to_send += "&field3=";
    data_to_send += lux;
    data_to_send += "&field4=";
    data_to_send += moisture_value; 
    data_to_send += "&field5=";
    data_to_send += raindrop_value; 
    data_to_send += "\r\n\r\n";
    espClient.print("POST /update HTTP/1.1\n");
    espClient.print("Host: api.thingspeak.com\n");
    espClient.print("Connection: close\n");
    espClient.print("X-THINGSPEAKAPIKEY: " + api_key + "\n");
    espClient.print("Content-Type: application/x-www-form-urlencoded\n");
    espClient.print("Content-Length: ");
    espClient.print(data_to_send.length());
    espClient.print("\n\n");
    espClient.print(data_to_send);
    delay(100);
    Serial.println("%. Send to Thingspeak. Waiting...");
    checkIncomingMessage(18000);  //check incoming message from MQTT again. The delayed time is needed because of the restriction on Thingspeak API
    client->publish(mqtt_pub_topic, "CHECK");  //send "CHECK" message as a sign that the data has been uploaded.
  }
  checkIncomingMessage(16800); //check incoming message again. The delayed time is needed because of the restriction on Thingspeak API
  Serial.println("Done\n");
  unsigned long now = millis();
  if (now - lastMsg > 2000) {
    lastMsg = now;
  }
}

void dhtRead(float* t, float* h){
  *t = dht.readTemperature();
  *h = dht.readHumidity();
  if (isnan(*h) || isnan(*t)) {
    Serial.println(F("Failed to read from DHT sensor!"));
    return;
  }
  Serial.print(F("Humidity: "));
  Serial.print(*h);
  Serial.println("% ");
  Serial.print(F("Temperature: "));
  Serial.print(*t);
  Serial.println("°C ");
}

void readAnalogInput(int* analog_value, uint8_t analog_pin){
  //Read analog input and map it to 0 to 255.
  delay(200);
  *analog_value = analogRead(analog_pin);
  *analog_value = map(*analog_value, 1024, 0, 0, 255);
}

void checkIncomingMessage(int times){
  //Check incoming messages from MQTT communication and add delay for 120ms each loop.
  int num_of_delay = times / 120;
  for (int i = 0; i <= num_of_delay; ++i){
    client->loop();
    delay(120);
  }
}
