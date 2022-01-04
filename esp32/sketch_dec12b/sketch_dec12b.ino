/*
 * @brief Simple sketch to control the Toilet Fan via Shelly v1 WiFi switch and DTH22 sensor
 * @details
 *
 * Code from:
 *   MQTTS: https://gist.github.com/gmag11/c565a18d361a46993039f8a515e67614
 *   TEMP : https://www.hackster.io/colinodell/mqtt-temperature-and-humidity-monitor-for-home-assistant-27b8d1
 *   JSON : https://techtutorialsx.com/2017/04/29/esp32-sending-json-messages-over-mqtt/
 *   HTTP : https://randomnerdtutorials.com/esp32-http-get-post-arduino/
 *  
 * Description:
 *   This sketch aims to make the ESP32 to publish data on a ThingsBoard instance
 *   via MQTT over SSL/TLS secure socket via tb-gateway instance:
 * 
 *     thingsboard:
 *       host: <tb-dns>
 *       port: 8883
 *       remoteConfiguration: false
 *       security:
 *         accessToken: (see note below)
 *         caCert: (see DSTroot_CA below)
 *
 *   Note: in ThingsBoard the MQTT User should be the ACCESS_TOKEN of the
 *   device added (so generate one and use it!)
 *
 *   From release 2 is also compare humidity to a setpoint and if greater send a command to a device (shelly v1)
 *
 * Note:
 *   Documentation for ESP-IDF can be found at:
 *     Wifi.h
 *       https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_wifi.html
 *     mqtt_client.h
 *       https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/mqtt.html#overview
 *
 *   Documentation for arduinojson (v5) can be found at:
 *     arduinojson.h
 *       https://arduinojson.org/v5/api/
 *
 *   DHT Sensor Library: https://github.com/adafruit/DHT-sensor-library
 *   Adafruit Unified Sensor Lib: https://github.com/adafruit/Adafruit_Sensor
 *
 * Changelog:
 *   0  13-dec-20  pf  First sketch, see #code-from above
 *   1  04-mar-21  pf  Added DHT22 library and readout
 *   2  30-dec-21  pf  Added humidity value checkont and http rpc operations
 *   3  02-jan-21  pf  Fixed control via ventola_ctrl_state: do not turn off if Ventola is ON from external command
 *
 */



#include "DHT.h"
#include "Arduino.h"
#include <WiFi.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "mqtt_client.h"
#include "HTTPClient.h"

// Digital pin connected to the DHT sensor
#define DHTPIN 4

// Type of DHT sensor used
#define DHTTYPE DHT22   // DHT 22  (AM2302), AM2321

// Unofficial JSON Library (ver. 5) (https://github.com/bblanchon/ArduinoJson)
// Ufficial Arduino JSON Library seems to be yet a Beta release  
#include <ArduinoJson.h>

// Ufficial Arduino JSON Library (https://github.com/arduino-libraries/Arduino_JSON)
#include <Arduino_JSON.h>

#define SECURE_MQTT // Comment this line if you are not using MQTT over SSL

#define LOG_LOCAL_LEVEL ESP_LOG_INFO

#ifdef SECURE_MQTT
#include "esp_tls.h"

// Let's Encrypt CA certificate. Change with the one you need
static const unsigned char DSTroot_CA[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
<generate_and_place_cert_here>
-----END CERTIFICATE-----
)EOF";
#endif // SECURE_MQTT

esp_mqtt_client_config_t mqtt_cfg;
esp_mqtt_client_handle_t client;

static const uint32_t FW_REL = 1;
static const uint32_t MQTT_DBG = 0; 

const char* WIFI_SSID = "lan_ssid";
const char* WIFI_PASSWD = "lan_passwd";

const char* MQTT_HOST = "<tb-dns>";
#ifdef SECURE_MQTT
const uint32_t MQTT_PORT = 8883;
#else
const uint32_t MQTT_PORT = 1883;
#endif // SECURE_MQTT
const char* MQTT_USER = "<ACCESS_TOKEN>";
const char* MQTT_PASSWD = "";

//Your Domain name with URL path or IP address with path
String lanServerName = "http://<shelly_lan_ip_or_dns>/relay/0";

int tb_upload_tim = 0;

static esp_err_t mqtt_event_handler (esp_mqtt_event_handle_t event) {
  if (event->event_id == MQTT_EVENT_CONNECTED) {
    ESP_LOGI ("TEST", "MQTT msgid= %d event: %d. MQTT_EVENT_CONNECTED", event->msg_id, event->event_id);
    //esp_mqtt_client_subscribe (client, "test/hello", 0);
    //esp_mqtt_client_publish (client, "test/status", "1", 1, 0, false);
  } 
  else if (event->event_id == MQTT_EVENT_DISCONNECTED) {
    ESP_LOGI ("TEST", "MQTT event: %d. MQTT_EVENT_DISCONNECTED", event->event_id);
    //esp_mqtt_client_reconnect (event->client); //not needed if autoconnect is enabled
  } else  if (event->event_id == MQTT_EVENT_SUBSCRIBED) {
    ESP_LOGI ("TEST", "MQTT msgid= %d event: %d. MQTT_EVENT_SUBSCRIBED", event->msg_id, event->event_id);
  } else  if (event->event_id == MQTT_EVENT_UNSUBSCRIBED) {
    ESP_LOGI ("TEST", "MQTT msgid= %d event: %d. MQTT_EVENT_UNSUBSCRIBED", event->msg_id, event->event_id);
  } else  if (event->event_id == MQTT_EVENT_PUBLISHED) {
    ESP_LOGI ("TEST", "MQTT event: %d. MQTT_EVENT_PUBLISHED", event->event_id);
  } else  if (event->event_id == MQTT_EVENT_DATA) {
    ESP_LOGI ("TEST", "MQTT msgid= %d event: %d. MQTT_EVENT_DATA", event->msg_id, event->event_id);
    ESP_LOGI ("TEST", "Topic length %d. Data length %d", event->topic_len, event->data_len);
    ESP_LOGI ("TEST","Incoming data: %.*s %.*s\n", event->topic_len, event->topic, event->data_len, event->data);

  } else  if (event->event_id == MQTT_EVENT_BEFORE_CONNECT) {
    ESP_LOGI ("TEST", "MQTT event: %d. MQTT_EVENT_BEFORE_CONNECT ", event->event_id);
  }
}

// Initialize DHT sensor.
// Note that older versions of this library took an optional third parameter to
// tweak the timings for faster processors.  This parameter is no longer needed
// as the current DHT reading algorithm adjusts itself to work on faster procs.
DHT dht(DHTPIN, DHTTYPE);

void setup () {
  Serial.begin (115200);

  Serial.print(" MQTT SmartHome Humidity sensor - fw release ");
  Serial.println(FW_REL);
  
  Serial.println("Configuring MQTT.. ");
  
  mqtt_cfg.host = MQTT_HOST;
  mqtt_cfg.port = MQTT_PORT;
  mqtt_cfg.username = MQTT_USER;
  //mqtt_cfg.password = MQTT_PASSWD;
  mqtt_cfg.keepalive = 15;
#ifdef SECURE_MQTT
  mqtt_cfg.transport = MQTT_TRANSPORT_OVER_SSL;
#else
  mqtt_cfg.transport = MQTT_TRANSPORT_OVER_TCP;
#endif // SECURE_MQTT
  mqtt_cfg.event_handle = mqtt_event_handler;
  //mqtt_cfg.lwt_topic;
  //mqtt_cfg.lwt_msg = "0";
  //mqtt_cfg.lwt_msg_len = 1;
  
  WiFi.mode (WIFI_MODE_STA);
  WiFi.begin (WIFI_SSID, WIFI_PASSWD);
  Serial.println("Connecting to Wifi.. ");
  while (!WiFi.isConnected ()) {
    Serial.print ('.');
    delay (100);
  }
  Serial.println ();
#ifdef SECURE_MQTT
  esp_err_t err = esp_tls_set_global_ca_store (DSTroot_CA, sizeof (DSTroot_CA));
  ESP_LOGI("TEST","CA store set. Error = %d %s", err, esp_err_to_name(err));
  Serial.println("CA store set...");
#endif // SECURE_MQTT
  client = esp_mqtt_client_init (&mqtt_cfg);
  //esp_mqtt_client_register_event (client, ESP_EVENT_ANY_ID, mqtt_event_handler, client); // not implemented in current Arduino core
  err = esp_mqtt_client_start (client);
  ESP_LOGI("TEST", "Client connect. Error = %d %s", err, esp_err_to_name (err));

  // Initilizing the DHT interface
  Serial.print("Initializing DHT Sensor interface...");
  dht.begin();
  Serial.print(" Done!\n\n");
  
}

void loop () {

  StaticJsonBuffer<300> JSONbuffer;
  JsonObject& JSONencoder = JSONbuffer.createObject();

  JSONencoder["device"] = "ESP32";
  JSONencoder["sensorType"] = "Temperature";
  JSONencoder["Temperature"] = 18.0;
  JSONencoder["Humidity"] = 60.0;

  char JSONmessageBuffer[100];

  float hum = 0.0;
  float temp = 0.0;
  float hic = 0.0;

  HTTPClient http;

  WiFiClient wifi_client;

  int http_json_pars_stat = 0;
  int ventola_ctrl_state = 0;
  String ventola_stat = String("false");  
  
  //esp_mqtt_client_publish (client, "v1/devices/me/telemetry", NULL, 0, 0, false);

  // MQTT Debug Test switch  
  if (MQTT_DBG >= 1) {
    // simple temperature increment to simulate device behaviour
    // (contains examples to use the arduinojson library)
    int i=0;
    for (i=1; i<=10; i++){
      JSONencoder.printTo(JSONmessageBuffer, sizeof(JSONmessageBuffer));
      esp_mqtt_client_publish (client, "v1/devices/me/telemetry", JSONmessageBuffer, 0, 0, false);
      Serial.println(i);
      Serial.println(JSONencoder.get<float>("Temperature"));
      Serial.print("Uploading ");
      Serial.print(JSONmessageBuffer);
      Serial.print(" to Thingsboard MQTT broker!\n");
      delay (60000);
      JSONencoder.set("Temperature", JSONencoder.get<float>("Temperature") +float(i) );
    }
  } else {
    
    Serial.print("Reading the DHT sensor...");
      
    // Reading temperature or humidity takes about 250 milliseconds!
    // Sensor readings may also be up to 2 seconds 'old' (its a very slow sensor)
    hum = dht.readHumidity();
    // Read temperature as Celsius (the default)
    temp = dht.readTemperature();
    
    // Check if any reads failed and exit early (to try again).
    if (isnan(hum) || isnan(temp)) {
      Serial.println(F("Failed to read from DHT sensor!"));
      return;
    }
    else {
      Serial.print(" Done!\n\n");
    }
    
    // Compute heat index in Celsius (isFahreheit = false)
    hic = dht.computeHeatIndex(temp, hum, false);

    // Inform the user 
    Serial.print("Humidity = ");
    Serial.print(hum);
    Serial.print("%\n");
    Serial.print("Temperature = ");
    Serial.print(temp);
    Serial.print(" C\n\n");


    // Add data to JSONencoder object 
    JSONencoder.set("Temperature", temp );
    JSONencoder.set("Humidity", hum );

    // Create the message buffer from JSONencoder object
    JSONencoder.printTo(JSONmessageBuffer, sizeof(JSONmessageBuffer));

    // Inform the user
    Serial.print("Uploading ");
    Serial.print(JSONmessageBuffer);
    Serial.print(" to Thingsboard MQTT broker!\n\n");

    // If WiFi is present...
    if(WiFi.status()== WL_CONNECTED){ 

      if (tb_upload_tim == 2) {
	// Publish to MQTT broker
	esp_mqtt_client_publish (client, "v1/devices/me/telemetry", JSONmessageBuffer, 0, 0, false);
	tb_upload_tim = 0;
      }
      
            
      // Connect
      http.begin(wifi_client, lanServerName.c_str());
      
      // Send GET
      int httpResponseCode = http.GET();
      
      // Declare the http payload
      String payload;
      
      if (httpResponseCode>0) {
          Serial.print("HTTP Response code: ");
          Serial.println(httpResponseCode);
          payload = http.getString();
	  Serial.print("HTTP Payload (as sting): ");
          Serial.println(payload);	
        }
        else {
          Serial.print("Error code: ");
          Serial.println(httpResponseCode);
        }
      
      // Free resources
      http.end();
      
      // Parse payload (if present)
      JSONVar http_json_payload = JSON.parse(payload);
      if (JSON.typeof(http_json_payload) == "undefined") {
          Serial.println("Parsing input failed!");
          http_json_pars_stat = 1;
      } else {   
	Serial.print("HTTP Payload (as JSON): ");
	Serial.print(http_json_payload);
	Serial.print("\n\n");	
      
	// Get the keys of the JSON payload
	JSONVar http_payload_keys = http_json_payload.keys();
      
	// Find the key with relay state and print
	Serial.print("Toilet Ventola state is: ");
	Serial.print(http_json_payload["ison"]);
	Serial.print("\n\n");

	// Set parsing status to OK 
	http_json_pars_stat = 0;

      }
      
      if (hum > 85.0) {
        Serial.print(">>>>> HUMIDITY IS HIGH! Turning Ventola on...");
        // Begin HTTP 
        http.begin(wifi_client, lanServerName.c_str());
        // Specify content-type header
        http.addHeader("Content-Type", "application/x-www-form-urlencoded");
        // Data to send with HTTP POST
        String httpRequestData = "turn=on&timer=0";           
        // Send HTTP POST request
        int httpResponseCode = http.POST(httpRequestData);
	// Free resources
	http.end();
	if (httpResponseCode == 200) {
	  Serial.print(" Done!\n\n");
	  ventola_ctrl_state = 1;
	  }
	  else {
	    Serial.print("\n ** HTTP ERROR! Response code: ");
	    Serial.print(httpResponseCode);
	    Serial.print("\n\n");
	  }
      }
      else {
	Serial.print(">>>>> HUMIDITY IS IN RANGE! ");	
	if (http_json_pars_stat == 0 && !(http_json_payload["ison"])) {	  
	  Serial.print(" Ventola is off, no action taken!\n\n");
	}
	else if (http_json_pars_stat == 0 && http_json_payload["ison"] && ventola_ctrl_state == 1) {
	  Serial.print(" Ventola is on (and it was me), turning off...");
	  // Begin HTTP 
	  http.begin(wifi_client, lanServerName.c_str());
	  // Specify content-type header
	  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
	  // Data to send with HTTP POST
	  String httpRequestData = "turn=off&timer=0";           
	  // Send HTTP POST request
	  int httpResponseCode = http.POST(httpRequestData);
	  // Free resources
	  http.end();	  
	  if (httpResponseCode == 200) {
	    Serial.print(" Done!\n\n");
	    ventola_ctrl_state = 0;
	  }
	  else {
	    Serial.print("\n ** HTTP ERROR! Response code: ");
	    Serial.print(httpResponseCode);
	    Serial.print("\n\n");
	  }	    
	}
	else {
	  Serial.print(" Ventola is on (and it was NOT me), no action taken!\n\n");
	}	  
      }
    }
    else {
      Serial.println("WiFi Disconnected");
    }
    
    // wait before taking the next sample
    delay (120000);

    // increment
    tb_upload_tim++;
  }

  
}
