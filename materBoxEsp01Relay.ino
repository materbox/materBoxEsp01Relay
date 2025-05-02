/************* Includes *************/
#include <MaterBox.h>
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager 
/************* End Includes *************/

/************* Define default values *************/
constexpr uint32_t SERIAL_DEBUG_BAUD = 115200U;
constexpr char CURRENT_FIRMWARE_TITLE[] = "Event-Horizon";
constexpr char CURRENT_FIRMWARE_VERSION[] = "0.1.1";
const char* deviceName = "MB-ESP01-Relay";
unsigned long mtime = 0;
int TIME_TO_SEND_TELEMETRY  = 30; //every x seconds to send tellemetry
/************* End Define default values *************/

/************* Double Reset config *************/
#define ESP_DRD_USE_LITTLEFS      true
#define ESP_DRD_USE_SPIFFS        false
#define ESP_DRD_USE_EEPROM        false
#define ESP8266_DRD_USE_RTC       false
#define DOUBLERESETDETECTOR_DEBUG true  //false
#include <ESP_DoubleResetDetector.h>    //https://github.com/khoih-prog/ESP_DoubleResetDetector
#define DRD_TIMEOUT 5 // Number of seconds after reset during which a subseqent reset will be considered a double reset.
#define DRD_ADDRESS 0

DoubleResetDetector* drd;
/************* End Double Reset config *************/

/************* Thingsboard *************/
#define THINGSBOARD_ENABLE_PROGMEM 0  // Disable PROGMEM because the ESP8266WiFi library, does not support flash strings.
//#define THINGSBOARD_ENABLE_STREAM_UTILS 1 // Enables sending messages that are bigger than the predefined message size

#include <Arduino_MQTT_Client.h>
#include <Arduino_ESP8266_Updater.h>
#include <OTA_Firmware_Update.h>
#include <Server_Side_RPC.h>
#include <Client_Side_RPC.h>
#include <ThingsBoard.h>          //https://github.com/thingsboard/thingsboard-arduino-sdk

char THINGSBOARD_SERVER[40] = "materbox.io";
char TOKEN[40] = "TEST_TOKEN";
constexpr uint16_t THINGSBOARD_PORT = 1883U;

// Maximum size packets will ever be sent or received by the underlying MQTT client,
// if the size is to small messages might not be sent or received messages will be discarded
constexpr uint16_t MAX_MESSAGE_SEND_SIZE = 512U;
constexpr uint16_t MAX_MESSAGE_RECEIVE_SIZE = 512U;

// RPC
// Statuses for subscribing to rpc
bool subscribed = false;

constexpr const char RPC_SET_PUMP_STATE[] = "setPumpState";
constexpr const char RPC_GET_PUMP_STATE[] = "getPumpState";
constexpr uint8_t MAX_RPC_SUBSCRIPTIONS = 3U;
constexpr uint8_t MAX_RPC_RESPONSE = 5U;
constexpr uint8_t MAX_RPC_REQUEST = 5U;
constexpr uint64_t REQUEST_TIMEOUT_MICROSECONDS = 5000U * 1000U;

// OTA
// Maximum amount of retries we attempt to download each firmware chunck over MQTT
constexpr uint8_t FIRMWARE_FAILURE_RETRIES = 12U;

// Size of each firmware chunck downloaded over MQTT,
// increased packet size, might increase download speed
constexpr uint16_t FIRMWARE_PACKET_SIZE = 4096U;
// Statuses for updating
bool currentFWSent = false;
bool updateRequestSent = false;

WiFiClient espClient;
// Initalize the Mqtt client instance
Arduino_MQTT_Client mqttClient(espClient);

// Initialize used apis
OTA_Firmware_Update<> ota;
Server_Side_RPC<MAX_RPC_SUBSCRIPTIONS, MAX_RPC_RESPONSE> rpc;
Client_Side_RPC<MAX_RPC_SUBSCRIPTIONS, MAX_RPC_REQUEST> rpc_request;
const std::array<IAPI_Implementation*, 3U> apis = {
  &rpc,
  &rpc_request,
  &ota
};

// Initialize ThingsBoard instance with the maximum needed buffer size
ThingsBoard tb(mqttClient, MAX_MESSAGE_RECEIVE_SIZE, MAX_MESSAGE_SEND_SIZE, Default_Max_Stack_Size, apis);

// Initalize the Updater client instance used to flash binary to flash memory
Arduino_ESP8266_Updater updater;
// Statuses for updating

/************* End Thingsboard *************/

/************* Wifi Manager *************/
const char* modes[] = { "NULL", "STA", "AP", "STA+AP" };

WiFiManager wm;

String WM_DATA_FILE = "config.txt";
bool TEST_CP         = false; // always start the configportal, even if ap found
int  TESP_CP_TIMEOUT = 180; // test cp timeout
bool TEST_NET        = true; // do a network test after connect, (gets ntp time)
bool ALLOWONDEMAND   = false; // enable on demand
bool WMISBLOCKING    = true; // use blocking or non blocking mode, non global params wont work in non blocking
bool STAND_ALONE     = false; // use device without thingsboard server
bool RESET_SETTINGS  = false; //reset WIFI settings - for testing
bool WM_CONNECTED    = false;
bool DRD_DETECTED    = false;
bool SAVE_PARAMS     = false;
/************* End Wifi Manager *************/

/************* Relay control *************/
bool state = false;
#define RELAY 0   // GPIO
int PUMP_STATUS;
 
/************* End Relay control *************/

MATERBOX mb;

void setup() {
  Serial.begin(SERIAL_DEBUG_BAUD);
  while (!Serial) ; // wait for Arduino Serial Monitor
  mb.enqueueMessage("Starting", "INFO");
  mb.enqueueMessage("Test", "ERROR");
  mb.enqueueMessage("Test", "WARN");
  mb.enqueueMessage("CURRENT_FIRMWARE_TITLE: " + String(CURRENT_FIRMWARE_TITLE), "INFO");
  mb.enqueueMessage("CURRENT_FIRMWARE_VERSION: " + String(CURRENT_FIRMWARE_VERSION), "INFO");

  drd = new DoubleResetDetector(DRD_TIMEOUT, DRD_ADDRESS);

  if (drd->detectDoubleReset()) {
    mb.enqueueMessage("Double Reset Detected", "INFO");
      DRD_DETECTED = true;
    } else {
      mb.enqueueMessage("No Double Reset Detected", "INFO");
      DRD_DETECTED = false;
    }

  mb.begin();
  
  setupRelay();
  setupWifiManager(DRD_DETECTED);

  mtime = TIME_TO_SEND_TELEMETRY * 1000;
}

void loop() {
  if(millis()-mtime > (TIME_TO_SEND_TELEMETRY * 1000)){
    if(WiFi.status() == WL_CONNECTED){
      if (!tb.connected()) {
        // Reconnect to the ThingsBoard server,
        // if a connection was disrupted or has not yet been established
        mb.enqueueMessage("Connecting to: " + String(THINGSBOARD_SERVER) + " with token " + String(TOKEN), "INFO");
        if (!tb.connect(THINGSBOARD_SERVER, TOKEN, THINGSBOARD_PORT)) {
          mb.enqueueMessage("Failed to connect", "ERROR");
          subscribed = false;
        }
      } else {
        if (!subscribed) {
            rpcSubscribe();
        }
        // mandar información de bomba encendida o apagada
      }
    } else {
      mb.enqueueMessage("WiFi.status() == WL_CONNECTED " + String(WiFi.status()), "ERROR");
    }
    mtime = millis();
  }
  tb.loop();
}

void  setupRelay() {
  /************* Relay control *************/
  pinMode(RELAY, OUTPUT);
  digitalWrite(RELAY, HIGH);
  /************* End Relay control *************/
}

void sendTelemetryJson(const JsonDocument &data){
  tb.sendTelemetryJson(data, Helper::Measure_Json(data));
  serializeJsonPretty(data, Serial);
  Serial.println();
}

/************* RPC callbacks *************/
void rpcSubscribe(){
 mb.enqueueMessage("Subscribing for RPC", "INFO");

  const std::array<RPC_Callback, 2U> callbacks = {
    RPC_Callback{ RPC_SET_PUMP_STATE, processSetPumpState},
    RPC_Callback{ RPC_GET_PUMP_STATE, processGetPumpState}
  };

  // Perform a subscription. All consequent data processing will happen in
  // processTemperatureChange() and processSwitchChange() functions,
  // as denoted by callbacks array.
  if (!rpc.RPC_Subscribe(callbacks.cbegin(), callbacks.cend())) {
    mb.enqueueMessage("Failed to subscribe for RPC", "ERROR");
    return;
  }
  mb.enqueueMessage("Subscribe done", "INFO");

  mb.enqueueMessage("OTA Firwmare Update Subscription...", "INFO");
  const OTA_Update_Callback callback(CURRENT_FIRMWARE_TITLE, CURRENT_FIRMWARE_VERSION, &updater, &finished_callback, &progress_callback, &update_starting_callback, FIRMWARE_FAILURE_RETRIES, FIRMWARE_PACKET_SIZE);
  // See https://thingsboard.io/docs/user-guide/ota-updates/
  // to understand how to create a new OTA pacakge and assign it to a device so it can download it.
  // Sending the request again after a successfull update will automatically send the UPDATED firmware state,
  // because the assigned firmware title and version on the cloud and the firmware version and title we booted into are the same.
  updateRequestSent = ota.Subscribe_Firmware_Update(callback);

  subscribed = true;
}

/// @brief Processes function for RPC call "SetPumpState"
/// RPC_Data is a JSON variant, that can be queried using operator[]
/// See https://arduinojson.org/v5/api/jsonvariant/subscript/ for more details
/// @param data Data containing the rpc data that was called and its current value
/// @return Response that should be sent to the cloud. Useful for getMethods
void processSetPumpState(const JsonVariantConst &data, JsonDocument &response) {
  mb.enqueueMessage("Received SetPumpState method", "RCP");
  //CISTERN_PUMP_STATUS = data["CISTERN_PUMP_STATUS"];
  PUMP_STATUS = data;
  switch (PUMP_STATUS) {
    case 1:
      Serial.println("RELAY=ON");
      digitalWrite(RELAY,LOW);
      mb.enqueueMessage("Pump is on", "INFO");
      break;

    case 0:
      Serial.println("RELAY=OFF");
      digitalWrite(RELAY,HIGH);
      mb.enqueueMessage("Pump is off", "INFO");
      break;
    
    default:
      Serial.println("RELAY=OFF");
      digitalWrite(RELAY,HIGH);
      mb.enqueueMessage("Pump failed", "WARN");
      break;
  }
  response.set(42);
}

/// @brief Processes function for RPC call "GetPumpState"
/// RPC_Data is a JSON variant, that can be queried using operator[]
/// See https://arduinojson.org/v5/api/jsonvariant/subscript/ for more details
/// @param data Data containing the rpc data that was called and its current value
/// @return Response that should be sent to the cloud. Useful for getMethods
void processGetPumpState(const JsonVariantConst &data, JsonDocument &response) {
  mb.enqueueMessage("Received GetPumpState method", "RCP");
  bool state;

  if(PUMP_STATUS){
    mb.enqueueMessage("Pump is on", "INFO");
    state = true;
  } else {
    mb.enqueueMessage("Pump is off", "INFO");
    state = false;
  }
  response.set(state);
}
/************* End RPC callbacks *************/

/************* OTA *************/
/// @brief Update starting callback method that will be called as soon as the shared attribute firmware keys have been received and processed
/// and the moment before we subscribe the necessary topics for the OTA firmware update.
/// Is meant to give a moment were any additional processes or communication with the cloud can be stopped to ensure the update process runs as smooth as possible.
/// To ensure that calling the ThingsBoardSized::Cleanup_Subscriptions() method can be used which stops any receiving of data over MQTT besides the one for the OTA firmware update,
/// if this method is used ensure to call all subscribe methods again so they can be resubscribed, in the method passed to the finished_callback if the update failed and we do not restart the device
void update_starting_callback() {
  // Nothing to do
}

/// @brief End callback method that will be called as soon as the OTA firmware update, either finished successfully or failed.
/// Is meant to allow to either restart the device if the udpate was successfull or to restart any stopped services before the update started in the subscribed update_starting_callback
/// @param success Either true (update successful) or false (update failed)
void finished_callback(const bool & success) {
  if (success) {
    Serial.println("Done, Reboot now");
    ESP.restart();
    mb.enqueueMessage("Downloading firmware success", "OTA");

    return;
  }
  mb.enqueueMessage("Downloading firmware failed", "OTA");
  Serial.println();
}

/// @brief Progress callback method that will be called every time our current progress of downloading the complete firmware data changed,
/// meaning it will be called if the amount of already downloaded chunks increased.
/// Is meant to allow to display a progress bar or print the current progress of the update into the console with the currently already downloaded amount of chunks and the total amount of chunks
/// @param current Already received and processs amount of chunks
/// @param total Total amount of chunks we need to receive and process until the update has completed
void progress_callback(const size_t & current, const size_t & total) {
  Serial.printf("Progress %.2f%%\n", static_cast<float>(current * 100U) / total);
}

/************* End OTA *************/

/************* Wifi Manager *************/
void setupWifiManager(bool DRD_DETECTED){
  // get device id from macAddress
  char deviceid[32] = "";
  byte macAddressArray[6];
  WiFi.macAddress(macAddressArray);
  getDeviceId(macAddressArray, 6, deviceid);

  wm.setDebugOutput(false);
  wm.debugPlatformInfo();

  //reset settings - for testing
  if (RESET_SETTINGS){
    mb.deleteFileData(WM_DATA_FILE);
    wm.resetSettings();
    wm.erase();
  }

  JsonDocument json;
  json = mb.loadData(WM_DATA_FILE);
  
  if (!json["ERROR"]){
    strcpy(THINGSBOARD_SERVER, json["THINGSBOARD_SERVER"]);
    //strcpy(THINGSBOARD_PORT, json["THINGSBOARD_PORT"]);
    strcpy(TOKEN, json["TOKEN"]);
    STAND_ALONE = json["STAND_ALONE"];
    TIME_TO_SEND_TELEMETRY = json["TIME_TO_SEND_TELEMETRY"];
  }

  WiFiManagerParameter custom_server("server", "MaterBox server", THINGSBOARD_SERVER, 40);
  //WiFiManagerParameter custom_mqtt_port("port", "port", THINGSBOARD_PORT, 6);
  WiFiManagerParameter custom_api_token("apikey", "Token", TOKEN, 32);
  WiFiManagerParameter device_type("devicetype", "Tipo", deviceName, 40, " readonly");
  WiFiManagerParameter device_id("deviceid", "Device Id", deviceid, 40, " readonly");

  // callbacks
  wm.setAPCallback(configModeCallback);
  wm.setWebServerCallback(bindServerCallback);
  wm.setSaveConfigCallback(saveWifiCallback);
  wm.setSaveParamsCallback(saveParamCallback);
  
  // add all your parameters here
  wm.addParameter(&custom_server);
//  wm.addParameter(&custom_mqtt_port);
  wm.addParameter(&custom_api_token);
  wm.addParameter(&device_type);
  wm.addParameter(&device_id);

  // invert theme, dark
  wm.setDarkMode(true);

  std::vector<const char *> menu = {"wifi","sep","exit"};
  wm.setMenu(menu); // custom menu, pass vector

  wm.setCountry("US"); // crashing on esp32 2.0

  // set Hostname
  wm.setHostname(("WM_" + wm.getDefaultAPName()).c_str());
  // wm.setHostname("WM_RANDO_1234");

  // show password publicly in form
  wm.setShowPassword(true);
  
  if(!WMISBLOCKING){
    wm.setConfigPortalBlocking(false);
  }

  //sets timeout until configuration portal gets turned off
  wm.setConfigPortalTimeout(180);
  
  // This is sometimes necessary, it is still unknown when and why this is needed
  // but it may solve some race condition or bug in esp SDK/lib
  // wm.setCleanConnect(true); // disconnect before connect, clean connect
  wm.setBreakAfterConfig(true); // needed to use saveWifiCallback

  if(DRD_DETECTED || TEST_CP){
    delay(1000);
    if(!wm.startConfigPortal("MaterBox IoT", "123456789")){
      mb.enqueueMessage("Failed to connect and hit timeout", "INFO");
    } else {
      mb.enqueueMessage("Wifi connected :)", "INFO");
      wifiInfo();
    }
  } else {
    if(!wm.autoConnect("MaterBox IoT", "123456789")){
      mb.enqueueMessage("Failed to connect and hit timeout", "INFO");
    } else {
      mb.enqueueMessage("Wifi connected :)", "INFO");
      wifiInfo();
    }
  }

  //read updated parameters
  strcpy(THINGSBOARD_SERVER, custom_server.getValue());
  strcpy(TOKEN, custom_api_token.getValue());
  //strcpy(THINGSBOARD_PORT, custom_mqtt_port.getValue());

  if (SAVE_PARAMS){
    JsonDocument json;
    json["THINGSBOARD_SERVER"] = THINGSBOARD_SERVER;
    //json["mqtt_port"] = mqtt_port;
    json["TOKEN"] = TOKEN;
    json["STAND_ALONE"] = STAND_ALONE;
    json["TIME_TO_SEND_TELEMETRY"] = TIME_TO_SEND_TELEMETRY;
    mb.saveData(json, WM_DATA_FILE);
  }
}

void saveWifiCallback(){
  mb.enqueueMessage("wm save settings Callback fired ", "INFO");
}

//gets called when WiFiManager enters configuration mode
void configModeCallback (WiFiManager *myWiFiManager) {
  mb.enqueueMessage("wm config Mode Callback fired", "INFO");
}

void saveParamCallback(){
  mb.enqueueMessage("wm save Parameters Callback fired", "INFO");
  SAVE_PARAMS = true;
}

void bindServerCallback(){
  wm.server->on("/custom",handleRoute); // this is now crashing esp32 for some reason
  // wm.server->on("/info",handleRoute); // you can override wm!
}

void handleRoute(){
  wm.server->send(200, "text/plain", "hello from user code");
  mb.enqueueMessage("wm handle route", "INFO");
}

void wifiInfo(){
  // can contain gargbage on esp32 if wifi is not ready yet
  mb.enqueueMessage("Wifi debug data", "INFO");

  JsonDocument json;
  json["SAVED"] = (String)(wm.getWiFiIsSaved() ? "YES" : "NO");
  json["SSID"] = (String)wm.getWiFiSSID();
  json["Password"] = (String)wm.getWiFiPass();
  json["Hostname"] = (String)WiFi.getHostname();
  
  // WiFi.printDiag(Serial);
  mb.enqueueMessageJson(json, "INFO", true);
}
/************* End Wifi Manager *************/

void getDeviceId(byte macAddressArray[], unsigned int len, char buffer[]){
    for (unsigned int i = 0; i < len; i++){
        byte nib1 = (macAddressArray[i] >> 4) & 0x0F;
        byte nib2 = (macAddressArray[i] >> 0) & 0x0F;
        buffer[i*2+0] = nib1  < 0xA ? '0' + nib1  : 'A' + nib1  - 0xA;
        buffer[i*2+1] = nib2  < 0xA ? '0' + nib2  : 'A' + nib2  - 0xA;
    }
    buffer[len*2] = '\0';
}

/// @brief Attribute request did not receive a response in the expected amount of microseconds 
void requestTimedOut() {
  Serial.printf("RPC request timed out did not receive a response in (%llu) microseconds. Ensure client is connected to the MQTT broker and that the RPC method actually exist on the device Rule chain\n", REQUEST_TIMEOUT_MICROSECONDS);
}

struct tm getTime() {
  struct tm timeinfo;
  int tz           = -6;
  int dst          = 0;
  time_t now       = time(nullptr);
  unsigned timeout = 5000; // try for timeout
  unsigned start   = millis();
  configTime(tz * 3600, dst * 3600, "pool.ntp.org", "time.nist.gov");
  Serial.print("Waiting for NTP time sync: ");
  while (now < 8 * 3600 * 2 ) { // what is this ?
    delay(100);
    Serial.print(".");
    now = time(nullptr);
    if((millis() - start) > timeout){
      Serial.println("\n[ERROR] Failed to get NTP time.");
      timeinfo.tm_hour = -1;
      return timeinfo;
    }
  }
  Serial.println("");
  gmtime_r(&now, &timeinfo);
  Serial.print("Current time: ");
  Serial.println(asctime(&timeinfo));

  return timeinfo;
}