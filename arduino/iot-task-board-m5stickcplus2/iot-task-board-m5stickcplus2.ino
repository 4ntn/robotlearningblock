// This is a program to measure manipulation performance with the Electronic Task Board. 
// First written by Peter So, December 2020.
// Last updated July 2024
//
// Program will not run on board without being connected to the PbHub unit. 
// Default behavior is board will attempt to WiFi network. Hold M5 button during power up to use without WiFi.
//
// To connect to wifi you will need to ensure the correct credentials are added to the secrets.h file.

#include <M5StickCPlus2.h> // https://github.com/m5stack/M5StickC-Plus
#include <Preferences.h>
#include <Wire.h>
#include "porthub.h"
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager
#include <WiFi.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>

// USER CONFIGURABLE SETTINGS
#define TASK_BOARD_PW "robothon"
#define PROTOCOL_ID "ROBOTHON_2023"
#define TIMELIMIT 600  // Trial Time Limit in seconds (600 is 10min)
#define FADERSP 2000 // This value should be updateable via the web commands from Kaa
#define FADERSP2 1500 // This value should be updateable via the web commands from Kaa
#define FADERTOLERANCE 60 // This value should be updateable via the web commands from Kaa
#define ANGLESP 2400 // Make sure this works for all task boards with the installed set screw
#define PTS_BUTTON 1
#define PTS_FADER 1
#define PTS_CIRCUIT_PROBE 1
#define PTS_CABLEWRAP 1
#define PTS_PROBEINSERT 1

#define MAC WiFi.macAddress()
#define BUTTON_ON 0
#define BUTTON_OFF 1
#define SCREEN_ROWS 135 // pixels
#define SCREEN_COLS 240 // pixels

// This value should be updateable via the web commands from Kaa
int verbose = 0; // set to 1 to enable serial output, default 0

//////// SYSTEM SETTINGS /////////
const String TOKEN = "task_board_2000";                // Endpoint token - you get (or specify) it during device provisioning
const String APP_VERSION = "c1v9jqmgul2l1s47m6bg-v0";    // Application version - you specify it during device provisioning 
// const String APP_VERSION = "bvhkhrtbhnjc0btkj7r0-v0";    // Application version - you specify it during device provisioning FOR DEVELOPMENT ONLY
const String FW_VERSION = "1.0.1"; // Firmware Version for OTA management

// DO NOT CHANGE SETTINGS BELOW //
const char* mqtt_server = "mqtt.cloud.kaaiot.com";

const unsigned long trialPublishRate = 1 * 0.05 * 1000UL; //500ms
const unsigned long fiveSeconds = 1 * 5 * 1000UL; //5 seconds
static unsigned long lastPublish = 0 - fiveSeconds;

// Setup Persistent Memory
Preferences preferences;

// Setup PbHub device
PortHub porthub;
uint8_t HUB_ADDR[6]={HUB1_ADDR,HUB2_ADDR,HUB3_ADDR,HUB4_ADDR,HUB5_ADDR,HUB6_ADDR};

// Setup WiFi and PubSub client
WiFiClient espClient;
PubSubClient client(espClient);

// Setup stopwatch interrupt specific settings
// timer interrupt variable.
volatile unsigned long usecCount = 0;

hw_timer_t *interruptTimer = NULL;
portMUX_TYPE mutex = portMUX_INITIALIZER_UNLOCKED;
void IRAM_ATTR usecTimer()
{
  portENTER_CRITICAL_ISR(&mutex);
  usecCount += 5;
  portEXIT_CRITICAL_ISR(&mutex);
}

///////////////////////////////
/////// INITIALIZE VARS ///////
///////////////////////////////

// Initialize program variables before running the setu and main loops
String task_board_ssid, unique_ssid;

//min,sec,msec,usec display.
int display[4] = {0};
unsigned long now = 0;
unsigned long last = 0;

//timer start/stop check variable
int screenSelector = 0; // int to navigate multiple screens
int screenSelector_count = 0;
int coin = 0;

int forceStop = 0;
int trialCounter = 0;
unsigned long trialTime = 0;
int wifiEnabled = 0;
int countStart = 0;
int trialRunning = 0,  timeLeft = 0,  ptsCollected = 0;
int buttonPushLatch = 0,  faderLatch = 0,  angleLatch = 0,  angleDoorLatch = 0,  cableWrapProbeStowLatch = 0, cableWrapLatch = 0,  probeGoalLatch = 0,  OP180_1_Latch = 0,  OP180_2_Latch = 0, trialCompletedLatch = 0;
int faderLatch2 = 0;
int faderGoal2 = 0;
int humanStart = 0;

int startBtnState = -1,  stopBtnState = -1,  resetBtnState = -1,  buttonPushState = -1,  faderValue = -1;
int keyswitchRState = -1,  keyswitchLState = -1,  angleValue = -1,  portRState = -1,  portLState = -1;
int probeStartState = -1,  probeGoalState = -1,  OP180_1_State = -1,  OP180_2_State = -1,  buttonPushState_old = -1;
int stopBtnState_old = -1,  faderValue_old = -1,  angleValue_old = -1,  probeStartState_old = -1,  probeGoalState_old = -1;
int OP180_1_State_old = -1, OP180_2_State_old = -1;

int TS_button = 0,  TS_fader_mid = 0,  TS_fader = 0,  TS_angle = 0,  TS_angle_door =0,  TS_cableWrap = 0, TS_cableWrapProbeStow = 0,  TS_probeGoal = 0,  TS_OP180_1 = 0,  TS_OP180_2 = 0;

float force = 0.0F, cumForce = 0.0F;
float startaccX = 0.0F, startaccY = 0.0F, startaccZ = 0.0F;
float load = 0.0F;
float accX = 0.0F, accY = 0.0F, accZ = 0.0F;
float gyroX = 0.0F, gyroY = 0.0F, gyroZ = 0.0F;
float pitch = 0.0F, roll  = 0.0F, yaw = 0.0F; 
float temp = 0.0F;

uint8_t BoardState[6]={startBtnState,resetBtnState,faderValue,angleValue,probeStartState,probeGoalState};

/////////////////////////////////
// Custom Function Definitions //
/////////////////////////////////
void callback(char* topic, byte* payload, unsigned int length) {
  Serial.printf("\nHandling command message on topic: %s\n", topic);
  
  //Check if message is related to the OTA update
  if (String(topic).startsWith("kp1/" + APP_VERSION + "/cmx_ota/" + TOKEN)) {
    handleOtaUpdate(topic, payload, length); //seems to be running with every message related to cmx_ota... remove this and only check for new firmawre on powerup.
    return;
  }
  
  //Check if message is related to a command message
  if (!String(topic).startsWith("kp1/" + APP_VERSION + "/cex/" + TOKEN)) {
    // TODO: Debug handling of command messages sent from the Kaa dashboard
    return;
  }

  DynamicJsonDocument doc(2048);
  deserializeJson(doc, payload, length);
  JsonVariant json_var = doc.as<JsonVariant>();
  
  DynamicJsonDocument commandResponse(2048);
  for (int i = 0; i < json_var.size(); i++) {
    unsigned int command_id = json_var[i]["id"].as<unsigned int>();
    commandResponse.createNestedObject();
    commandResponse[i]["id"] = command_id;
    commandResponse[i]["statusCode"] = 200;
    commandResponse[i]["payload"] = "done";
  }

  String responseTopic = "kp1/" + APP_VERSION + "/cex/" + TOKEN + "/result/SWITCH";
  client.publish(responseTopic.c_str(), commandResponse.as<String>().c_str());
  Serial.println("Published response to SWITCH command on topic: " + responseTopic);

  // Receive and parse the payload
  Serial.println("Payload: " + doc.as<String>()); //TODO troubleshoot
}

void setup_wifi() {
  if (WiFi.status() != WL_CONNECTED) {
    delay(200);
    Serial.println();
    Serial.printf("Connecting to [%s]", unique_ssid);
    WiFi.begin(unique_ssid.c_str(), TASK_BOARD_PW);
    connectWiFi();
  }
}

void connectWiFi() {
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
}


void initServerConnection() {
  setup_wifi();
  if (!client.connected()) {
    reconnect();
  }
  client.loop();
}

void reconnect() {
  while (!client.connected()) {
    Serial.println("Attempting MQTT connection...");
    char *client_id = "client-id-123ab";
    if (client.connect(client_id)) {
      Serial.println("Connected to WiFi");
      // ... and resubscribe
      subscribeToCommand();
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

void subscribeToCommand() {
  String topic = "kp1/" + APP_VERSION + "/cex/" + TOKEN + "/command/SWITCH/status";
  client.subscribe(topic.c_str());
  Serial.println("Subscribed on topic: " + topic);

  //Inserting from OTA Example function subscribeToFirmwareUpdates()
  String serverPushOnConnect = "kp1/" + APP_VERSION + "/cmx_ota/" + TOKEN + "/config/json/#";
  client.subscribe(serverPushOnConnect.c_str());
  Serial.println("Subscribed to server firmware push on topic: " + serverPushOnConnect);

  String serverFirmwareResponse = "kp1/" + APP_VERSION + "/cmx_ota/" + TOKEN + "/config/json/status/#";
  client.subscribe(serverFirmwareResponse.c_str());
  Serial.println("Subscribed to server firmware response on topic: " + serverFirmwareResponse);

  String serverFirmwareErrorResponse = "kp1/" + APP_VERSION + "/cmx_ota/" + TOKEN + "/config/json/status/error";
  client.subscribe(serverFirmwareErrorResponse.c_str());
  Serial.println("Subscribed to server firmware response on topic: " + serverFirmwareErrorResponse);
}

//OTA Functions from Kaa
void reportCurrentFirmwareVersion() {
  String reportTopic = "kp1/" + APP_VERSION + "/cmx_ota/" + TOKEN + "/applied/json";
  String reportPayload = "{\"configId\":\"" + FW_VERSION + "\"}"; //UPDATE this to match the OTA upgradeable from field on Kaa
  Serial.println("Reporting current firmware version on topic: " + reportTopic + " and payload: " + reportPayload);
  client.publish(reportTopic.c_str(), reportPayload.c_str());
}

void requestNewFirmware() {
  int requestID = random(0, 99);
  String firmwareRequestTopic = "kp1/" + APP_VERSION + "/cmx_ota/" + TOKEN + "/config/json/" + requestID;
  Serial.println("Requesting firmware using topic: " + firmwareRequestTopic);
  client.publish(firmwareRequestTopic.c_str(), "{\"observe\":true}"); // observe is used to specify whether the client wants to accept server pushes
}

void handleOtaUpdate(char* topic, byte* payload, unsigned int length) {
  Serial.printf("\nHandling firmware update message on topic: %s and payload: ", topic);

  //DEBUG with Denys
  //This fixed the issue!
  //This checks that the topic is indeed a cmx_ota message. 
  if (!String(topic).startsWith("kp1/" + APP_VERSION + "/cmx_ota/" + TOKEN)) {
  return;
  }
  //END DEBUG
  
  DynamicJsonDocument doc(1023);
  deserializeJson(doc, payload, length);
  JsonVariant json_var = doc.as<JsonVariant>();
  Serial.println(json_var.as<String>());
  if (json_var.isNull()) {
    Serial.println("No new firmware version is available");
    return;
  }

  unsigned int statusCode = json_var["statusCode"].as<unsigned int>();
  if (statusCode != 200) {
    Serial.printf("Firmware message's status code is not 200, but: %d\n", statusCode);
    return;
  }

  //  return; //DEBUG escape do not actually update the firmware //commenting this line will enable OTA update.
  String firmwareLink = json_var["config"]["link"].as<String>();

  t_httpUpdate_return ret = httpUpdate.update(espClient, firmwareLink.c_str());

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("HTTP_UPDATE_FAILED Error (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
      break;

    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("HTTP_UPDATE_NO_UPDATES");
      break;

    case HTTP_UPDATE_OK:
      Serial.println("HTTP_UPDATE_OK");
      break;
  }
}

void resetCounter(){
    preferences.remove("trialCounter");  // Or remove the counter key only.
    trialCounter = preferences.getUInt("trialCounter", 0);  // Get the counter value in current namesapce, if no key exists then return default value as second parameter
    Serial.printf("Counter value reset!\n");  // Print the counter to Serial Monitor. 
}

void publish_telemetry(){
      DynamicJsonDocument telemetry(8192); // increased from 1023
      telemetry.createNestedObject();

      telemetry[0]["accX"] = accX * 1000; //Float
      telemetry[0]["accY"] = accY * 1000; //Float
      telemetry[0]["accZ"] = accZ * 1000; //Float
      telemetry[0]["gyroX"] = gyroX; //Float
      telemetry[0]["gyroY"] = gyroY; //Float
      telemetry[0]["gyroZ"] = gyroZ; //Float
      telemetry[0]["trialCounter"] = trialCounter; //INT
      telemetry[0]["faderValue"] = faderValue; //INT
      telemetry[0]["angleValue"] = angleValue; //INT
      telemetry[0]["startButtonState"] = startBtnState; //BOOL
      telemetry[0]["resetButtonState"] = resetBtnState; //BOOL
      telemetry[0]["pushButtonState"] = buttonPushState; // BOOL
      telemetry[0]["stopButtonState"] = stopBtnState; // BOOL
      telemetry[0]["probeStartState"] = probeStartState; //BOOL
      telemetry[0]["probeGoalState"] = probeGoalState; //BOOL
      telemetry[0]["WrapPostLeftState"] = OP180_1_State; //BOOL
      telemetry[0]["WrapPostRightState"] = OP180_2_State; //BOOL
      telemetry[0]["trialStarted"] = trialRunning; //BOOL
      telemetry[0]["trialTime"] = trialTime; //Float
      telemetry[0]["Time_ButtonPressed"] = TS_button; //INT
      telemetry[0]["Time_Button"] = TS_button; //INT legacy
      telemetry[0]["Time_Fader"] = TS_fader; //INT legacy
      telemetry[0]["Time_FaderSP1Achieved"] = TS_fader_mid; //INT
      telemetry[0]["Time_FaderSP2Achieved"] = TS_fader; //INT
      telemetry[0]["Time_TerminalBlockProbed"] = TS_angle; //INT
      telemetry[0]["Time_Angle"] = TS_angle; //INT legacy
      telemetry[0]["Time_Door_Opened"] = TS_angle_door; //INT
      telemetry[0]["Time_CableWrap"] = TS_cableWrap; //INT legacy
      telemetry[0]["Time_CableWrapped"] = TS_cableWrap; //INT
      telemetry[0]["Time_CableWrappedProbeTipReplaced"] = TS_cableWrapProbeStow; //INT
      telemetry[0]["Time_ProbePlugInserted"] = TS_probeGoal; //INT
      telemetry[0]["Time_ProbeGoal"] = TS_probeGoal; //INT legacy
      telemetry[0]["cumForce"] = cumForce; //Float
      telemetry[0]["trialPoints"] = ptsCollected; //INT 
      // telemetry[0]["FW_Version"] = String(FW_VERSION).c_str(); //STR 
      // telemetry[0]["PROTOCOL"] = String(PROTOCOL_ID).c_str(); //STR 
      telemetry[0]["battVoltage"] = StickCP2.Power.getBatteryVoltage(); //FLOAT 
      telemetry[0]["battCurrent(mA)"] = StickCP2.Power.getBatteryCurrent(); //FLOAT 
      telemetry[0]["M5BattLevel"] = StickCP2.Power.getBatteryLevel(); //FLOAT
      telemetry[0]["M5IsCharging"] = StickCP2.Power.isCharging(); //FLOAT
      telemetry[0]["HumanStartFlag"] = humanStart; //BOOL
      
      String topic = "kp1/" + APP_VERSION + "/dcx/" + TOKEN + "/json";
      client.publish(topic.c_str(), telemetry.as<String>().c_str());
      Serial.println("Published on topic: " + topic);
}

// SCREEN DEFINITIONS
void home_screen(){
    StickCP2.Display.drawCircle(10, 10, 10, WHITE);
    if (buttonPushLatch){
      StickCP2.Display.fillCircle(10, 10, 8, GREEN);
    }
    StickCP2.Display.drawCircle(40, 10, 10, WHITE);
    if (faderLatch){
      StickCP2.Display.fillCircle(40, 10, 8, GREEN);
    }
    StickCP2.Display.drawCircle(70, 10, 10, WHITE);
    if (probeGoalLatch){
      StickCP2.Display.fillCircle(70, 10, 8, GREEN);
    }
    StickCP2.Display.drawCircle(100, 10, 10, WHITE);
    if (angleLatch){
      StickCP2.Display.fillCircle(100, 10, 8, GREEN);
    }
    StickCP2.Display.drawCircle(130, 10, 10, WHITE);
    if (cableWrapProbeStowLatch){
      StickCP2.Display.fillCircle(130, 10, 8, GREEN);
    }
    StickCP2.Display.drawCircle(160, 10, 10, WHITE);
    if (trialCompletedLatch){
      StickCP2.Display.fillCircle(160, 10, 8, GREEN);
    }
    StickCP2.Display.setCursor(5, 25);
    StickCP2.Display.setTextSize(1);
    StickCP2.Display.printf("Home Screen \n");
    StickCP2.Display.printf(" Smart Task Board");
    StickCP2.Display.printf(" v%s \n", FW_VERSION);
    StickCP2.Display.printf(" Wifi On:%d Status:%d batt:%0.1fV %0.1fmA\n", wifiEnabled, WiFi.status(), StickCP2.Power.getBatteryVoltage(),StickCP2.Power.getBatteryCurrent());
    StickCP2.Display.printf(" Token: %s\n", TOKEN.c_str());
    StickCP2.Display.printf(" PROTOCOL: %s\n", PROTOCOL_ID);
    StickCP2.Display.printf(" Trial Counter:%d\n", trialCounter);
    StickCP2.Display.printf(" Points:%d Interaction:%0.2f, Human:%d\n", ptsCollected, cumForce, humanStart);
    StickCP2.Display.printf(" ST1:%0.2f, ST2:%0.2f, ST3:%0.2f\n", (float)TS_button/1000000.0, (float)TS_fader/1000000.0, (float)TS_probeGoal/1000000.0);
    StickCP2.Display.printf(" ST4:%0.2f, ST5:%0.2f\n", (float)TS_angle/1000000.0, (float)TS_cableWrapProbeStow/1000000.0);
    StickCP2.Display.printf(" Trial Time:\n ");
    StickCP2.Display.setTextSize(3);
    StickCP2.Display.printf("%02dm:%02ds:%03dms\n", display[0], display[1], display[2]);    
}
void develop_screen(){
    StickCP2.Display.setCursor(5, 5);
    StickCP2.Display.setTextSize(1);
    StickCP2.Display.printf("I/O SCREEN\n ");
    StickCP2.Display.printf("Smart Task Board ");
    StickCP2.Display.printf("v%s\n ", FW_VERSION);
    StickCP2.Display.printf("Wifi On:%d Status:%d\n ", wifiEnabled, WiFi.status());
    StickCP2.Display.printf("Token: %s\n ", TOKEN.c_str());
    StickCP2.Display.printf("PROTOCOL: %s\n ", PROTOCOL_ID);
    StickCP2.Display.printf("TrialCount:%d\n ", trialCounter);
    StickCP2.Display.printf("%d BLU_BTN:%d RED_BTN:%d\n ", buttonPushLatch, buttonPushState, stopBtnState); 
    StickCP2.Display.printf("%d SP:%d Tol:%d Fader:%d\n ", faderLatch, FADERSP, FADERTOLERANCE, faderValue); 
    StickCP2.Display.printf("%d P_TB:%d P_Holder:%d\n ", probeGoalLatch, probeStartState, probeGoalState); 
    StickCP2.Display.printf("%d SP:%d Angle:%d\n ", angleLatch, ANGLESP, angleValue); 
    StickCP2.Display.printf("%d Post1:%d Post2:%d\n ", cableWrapLatch, OP180_1_State, OP180_2_State); 
    StickCP2.Display.printf(" Points:%d Interaction:%0.2f, Human:%d\n", ptsCollected, cumForce, humanStart);
    StickCP2.Display.printf("ST1:%0.2f, ST2:%0.2f, ST3:%0.2f\n ", (float)TS_button/1000000.0, (float)TS_fader/1000000.0, (float)TS_probeGoal/1000000.0);
    StickCP2.Display.printf("ST4:%0.2f, ST5:%0.2f\n ", (float)TS_angle/1000000.0, (float)TS_cableWrapProbeStow/1000000.0);
    StickCP2.Display.printf("Trial Time (sec):%0.2f\n ", (float)trialTime/1000000.0);
    
    // StickCP2.Display.printf("Interaction: %0.2f\n ", cumForce);
    // StickCP2.Display.printf("Trial Time:\n ");
    // StickCP2.Display.setTextSize(3);
    // StickCP2.Display.printf("%02dm:%02ds:%03dms\n", display[0], display[1], display[2]);    
    // StickCP2.Display.printf("acX:%0.2f acY:%0.2f acZ:%0.2f\n  ", accX*1000, accY*1000, accZ*1000);
  //    StickCP2.Display.printf("gyX:%0.2f gyY:%0.2f gyZ:%0.2f\n  ", gyroX, gyroY, gyroZ);
}

void screen2(){
    StickCP2.Display.drawCircle(10, 10, 10, WHITE);
    StickCP2.Display.fillCircle(10, 10, 3, YELLOW);
    StickCP2.Display.drawCircle(40, 10, 10, WHITE);
    StickCP2.Display.drawCircle(70, 10, 10, WHITE);
    StickCP2.Display.drawCircle(100, 10, 10, WHITE);
    StickCP2.Display.drawCircle(130, 10, 10, WHITE);
    StickCP2.Display.drawCircle(160, 10, 10, WHITE);
    StickCP2.Display.setCursor(5, 15);
    StickCP2.Display.setTextSize(1);
    StickCP2.Display.printf("\n ");
    StickCP2.Display.printf("Subtask:1/6\n ");
    StickCP2.Display.printf("PROTOCOL: %s\n ", PROTOCOL_ID);
    StickCP2.Display.printf("Find Board, Press Button\n ");
    StickCP2.Display.setTextSize(3);
    StickCP2.Display.printf("%02dm:%02ds:%03dms\n", display[0], display[1], display[2]); 
    // StickCP2.Display.printf("ST1 Time: %d\n", TS_button);
}
void screen3(){
    StickCP2.Display.drawCircle(10, 10, 10, WHITE);
    StickCP2.Display.drawCircle(40, 10, 10, WHITE);
    StickCP2.Display.drawCircle(70, 10, 9, YELLOW);
    StickCP2.Display.fillCircle(70, 10, 3, YELLOW);
    StickCP2.Display.drawCircle(70, 10, 10, WHITE);
    StickCP2.Display.drawCircle(100, 10, 10, WHITE);
    StickCP2.Display.drawCircle(130, 10, 10, WHITE);
    StickCP2.Display.drawCircle(160, 10, 10, WHITE);
    StickCP2.Display.setCursor(5, 15);
    StickCP2.Display.setTextSize(1);
    StickCP2.Display.printf("\n ");
    StickCP2.Display.printf("Subtask:3/6\n ");
    StickCP2.Display.printf("PROTOCOL: %s\n ", PROTOCOL_ID);
    StickCP2.Display.printf("Insert Probe Cable Plug\n ");
    StickCP2.Display.setTextSize(3);
    StickCP2.Display.printf("%02dm:%02ds:%03dms\n", display[0], display[1], display[2]);    
    // StickCP2.Display.printf("ST2 Time: %d\n", TS_probeGoal);
}
void screen4(){
    StickCP2.Display.drawCircle(10, 10, 10, WHITE);
    StickCP2.Display.fillCircle(10, 10, 9, GREEN);
    StickCP2.Display.drawCircle(40, 10, 10, WHITE);
    StickCP2.Display.fillCircle(40, 10, 3, YELLOW);
    StickCP2.Display.drawCircle(70, 10, 10, WHITE);
    StickCP2.Display.drawCircle(100, 10, 10, WHITE);
    StickCP2.Display.drawCircle(130, 10, 10, WHITE);
    StickCP2.Display.drawCircle(160, 10, 10, WHITE);
    StickCP2.Display.setCursor(5, 15);
    StickCP2.Display.setTextSize(1);
    StickCP2.Display.printf("\n ");
    StickCP2.Display.printf("Subtask:2/6\n ");
    StickCP2.Display.printf("PROTOCOL: %s\n ", PROTOCOL_ID);
    StickCP2.Display.printf("Move Slider to Setpoint\n ");
    StickCP2.Display.setTextSize(3);
    StickCP2.Display.printf("%02dm:%02ds:%03dms\n", display[0], display[1], display[2]); 
    StickCP2.Display.setTextSize(1);
    // StickCP2.Display.printf("ST3 Time: %d\n", TS_fader);
    int x_offset = map(faderValue, 0, 4000, 10, 210); 
    int x_goal = map(FADERSP, 0, 4000,10,210);
    int x_goal2 = map(faderGoal2, 0, 4000,10,210);
    StickCP2.Display.fillRect(0, 80, 240, 25, BLACK);
    StickCP2.Display.fillTriangle(0+x_offset, 80, 20+x_offset, 80, 10+x_offset, 100, RED);
    StickCP2.Display.fillTriangle(0+x_goal, 120, 20+x_goal, 120, 10+x_goal, 100, YELLOW);
    // StickCP2.Display.setCursor(x_goal, 100);
    // StickCP2.Display.setTextColor(WHITE, GREEN);
    // StickCP2.Display.printf("1");
    if (faderLatch2 == 1){
      // 1st fader position has been reached...
      StickCP2.Display.fillTriangle(0+x_goal2, 120, 20+x_goal2, 120, 10+x_goal2, 100, GREEN);
      // StickCP2.Display.setCursor(x_goal2, 100);
      // StickCP2.Display.setTextColor(WHITE, YELLOW);
      // StickCP2.Display.printf("2");
    }
    // StickCP2.Display.setTextColor(WHITE, BLUE);
}
void screen5(){
    StickCP2.Display.drawCircle(10, 10, 10, WHITE);
    StickCP2.Display.drawCircle(40, 10, 10, WHITE);
    StickCP2.Display.drawCircle(70, 10, 10, WHITE);
    StickCP2.Display.drawCircle(100, 10, 10, WHITE);
    StickCP2.Display.drawCircle(100, 10, 9, YELLOW);
    StickCP2.Display.fillCircle(100, 10, 3, YELLOW);
    StickCP2.Display.drawCircle(130, 10, 10, WHITE);
    StickCP2.Display.drawCircle(160, 10, 10, WHITE);
    StickCP2.Display.setCursor(5, 15);
    StickCP2.Display.setTextSize(1);
    StickCP2.Display.printf("\n ");
    StickCP2.Display.printf("Subtask:4/6\n ");
    StickCP2.Display.printf("PROTOCOL: %s\n ", PROTOCOL_ID);
    StickCP2.Display.printf("Open door, probe circuit\n ");
    StickCP2.Display.setTextSize(3);
    StickCP2.Display.printf("%02dm:%02ds:%03dms\n", display[0], display[1], display[2]);    
    // StickCP2.Display.printf("ST4 Time: %d\n", TS_angle);
}
void screen6(){
    StickCP2.Display.drawCircle(10, 10, 10, WHITE);
    StickCP2.Display.drawCircle(40, 10, 10, WHITE);
    StickCP2.Display.drawCircle(70, 10, 10, WHITE);
    StickCP2.Display.drawCircle(100, 10, 10, WHITE);
    StickCP2.Display.drawCircle(130, 10, 10, WHITE);
    StickCP2.Display.drawCircle(130, 10, 9, YELLOW);
    StickCP2.Display.fillCircle(130, 10, 3, YELLOW);
    StickCP2.Display.drawCircle(160, 10, 10, WHITE);
    StickCP2.Display.setCursor(5, 15);
    StickCP2.Display.setTextSize(1);
    StickCP2.Display.printf("\n ");
    StickCP2.Display.printf("Subtask:5/6\n ");
    StickCP2.Display.printf("PROTOCOL: %s\n ", PROTOCOL_ID);
    StickCP2.Display.printf("Wrap probe cable and plug in probe\n ");
    StickCP2.Display.setTextSize(3);
    StickCP2.Display.printf("%02dm:%02ds:%03dms\n", display[0], display[1], display[2]);  
    StickCP2.Display.setTextSize(1);
    StickCP2.Display.setCursor(50, 120);
    StickCP2.Display.printf("Post Near");  
    StickCP2.Display.drawCircle(70, 100, 10, WHITE);
    if (OP180_1_State == 1){
      StickCP2.Display.fillCircle(70, 100, 8, GREEN);      
    } else {
      StickCP2.Display.fillCircle(70, 100, 8, BLACK);      
    }
    StickCP2.Display.setCursor(140, 120);
    StickCP2.Display.printf("Post Far");  
    StickCP2.Display.drawCircle(160, 100, 10, WHITE);
    if (OP180_2_State == 1){
      StickCP2.Display.fillCircle(160, 100, 8, GREEN);      
    } else {
      StickCP2.Display.fillCircle(160, 100, 8, BLACK);
    }
    // StickCP2.Display.printf("ST5 Time: %d\n", TS_cableWrap);
}
void screen7(){
    StickCP2.Display.drawCircle(10, 10, 10, WHITE);
    StickCP2.Display.drawCircle(40, 10, 10, WHITE);
    StickCP2.Display.drawCircle(70, 10, 10, WHITE);
    StickCP2.Display.drawCircle(100, 10, 10, WHITE);
    StickCP2.Display.drawCircle(130, 10, 10, WHITE);
    StickCP2.Display.drawCircle(160, 10, 10, WHITE);
    StickCP2.Display.drawCircle(160, 10, 9, YELLOW);
    StickCP2.Display.fillCircle(160, 10, 3, YELLOW);
    StickCP2.Display.setCursor(5, 15);
    StickCP2.Display.setTextSize(1);
    StickCP2.Display.printf("\n ");
    StickCP2.Display.printf("Subtask:6/6\n ");
    StickCP2.Display.printf("PROTOCOL: %s\n ", PROTOCOL_ID);
    StickCP2.Display.printf("Press trial stop button\n ");
    StickCP2.Display.setTextSize(3);
    StickCP2.Display.printf("%02dm:%02ds:%03dms\n", display[0], display[1], display[2]);   
    // StickCP2.Display.printf("ST6 Time: %d\n", trialTime - TS_cableWrap); 
}

void update_inputs(){
      /////// READ INPUTS //////
    StickCP2.update();
    // get device accel data
    auto imu_update = StickCP2.Imu.update();
    auto imu_data = StickCP2.Imu.getImuData();
    accX = imu_data.accel.x;
    accY = imu_data.accel.y;
    accZ = imu_data.accel.z;
    gyroX = imu_data.gyro.x;
    gyroY = imu_data.gyro.y;
    gyroZ = imu_data.gyro.z;

    startBtnState = !StickCP2.BtnA.wasPressed();
    resetBtnState = !StickCP2.BtnB.wasPressed();

    // this is the solution that fixed the floating values from the new STM vs MEGA chips on the PbHub
    porthub.hub_d_wire_value_A(HUB_ADDR[3], 1); //write value high
    porthub.hub_d_wire_value_B(HUB_ADDR[3], 1); //write value high

    // // Read from PbHub Module
    buttonPushState = porthub.hub_d_read_value_A(HUB_ADDR[0]); //blue button
    stopBtnState = porthub.hub_d_read_value_B(HUB_ADDR[0]);   //red button
    faderValue = porthub.hub_a_read_value(HUB_ADDR[5]); //fader
    angleValue = porthub.hub_a_read_value(HUB_ADDR[4]); //angle
    probeStartState = porthub.hub_d_read_value_A(HUB_ADDR[3]); //flying-probeStart
    probeGoalState = porthub.hub_d_read_value_B(HUB_ADDR[3]); //flying-probeGoal
    OP180_1_State = porthub.hub_d_read_value_A(HUB_ADDR[1]); //post1. This sensor only works with PbHub when connected to Port0 or Port1
    OP180_2_State = porthub.hub_d_read_value_A(HUB_ADDR[2]); //post2. This sensor only works with PbHub when connected to Port0 or Port1

}

bool check_blue_button(){
  if (buttonPushState == 0 && buttonPushState != buttonPushState_old){Serial.printf("%d us Task_Board_Event: Blue Push Button Pressed\n", usecCount);};
  if (buttonPushState == 1 && buttonPushState != buttonPushState_old){Serial.printf("%d us Task_Board_Event: Blue Push Button Released\n", usecCount);};
  buttonPushState_old = buttonPushState; // store current value
  // Button Check
  if (buttonPushState == BUTTON_ON && trialRunning == 1 && buttonPushLatch == 0)
  {
    delay(1);
    buttonPushLatch = 1;
    TS_button = usecCount;
    ptsCollected = ptsCollected + PTS_BUTTON;
    blink_LED(); //turn off LED when red button is pressed
    Serial.printf("%d us Trial_Event: Button pushed!\n", usecCount);
    StickCP2.Display.fillScreen(BLUE); //clear screen
    screenSelector = 2;
    // trialRunning++;
  }
  return buttonPushLatch;
}

bool check_slider(){
  // if (faderValue == 0 && faderValue != faderValue_old){Serial.println("Fader Matched");};
  // if (faderValue == 1 && faderValue != faderValue_old){Serial.println("Fader not Matched");};
  faderValue_old = faderValue; // store current value
  // Slider/Fader Task Check
  if (faderValue > FADERSP - FADERTOLERANCE && faderValue < FADERSP + FADERTOLERANCE && trialRunning == 1 && faderLatch2 == 0 && buttonPushLatch == 1)
  {
    faderLatch2 = 1;
    TS_fader_mid = usecCount;
    Serial.printf("%d us Trial_Event: Fader SP1 Matched! %d\n", usecCount, FADERSP);
    // draw 2nd goal arrow
    // flash screen
    StickCP2.Display.fillScreen(YELLOW); //clear screen
    delay(50);
    StickCP2.Display.fillScreen(BLUE); //clear screen
    
    // toss coin
    coin = random(5,100);
    if (coin >= 50){
      faderGoal2 = FADERSP - FADERSP2 * coin / 100;
    } else {
      faderGoal2 = FADERSP + FADERSP2 * coin / 100;
    }

  }
  if (faderValue > faderGoal2 - FADERTOLERANCE && faderValue < faderGoal2 + FADERTOLERANCE && faderLatch == 0 && faderLatch2 == 1)
  {
    delay(1);
    faderLatch = 1;
    TS_fader = usecCount;
    ptsCollected = ptsCollected + PTS_FADER;
    blink_LED(); //turn off LED when red button is pressed
    Serial.printf("%d us Trial_Event: Fader SP2 Matched! %d \n", usecCount, faderGoal2);
    StickCP2.Display.fillScreen(YELLOW); //clear screen
    delay(50);
    StickCP2.Display.fillScreen(BLUE); //clear screen
    // screenSelector = 4;
    screenSelector = 0;
    // trialRunning++;
  }
  return faderLatch;
}

bool check_probe_plug(){
  if (probeGoalState == 0 && probeGoalState != probeGoalState_old){Serial.printf("%d us Task_Board_Event: Probe plug is plugged in and Probe Tip is in holder\n", usecCount);};
  if (probeGoalState == 1 && probeGoalState != probeGoalState_old){Serial.printf("%d us Task_Board_Event: Probe plug is not plugged in\n", usecCount);};
  probeGoalState_old = probeGoalState; // store current value
  if (probeStartState == 0 && probeStartState != probeStartState_old){Serial.printf("%d us Task_Board_Event: Probe is touching goal circuit\n", usecCount);};
  if (probeStartState == 1 && probeStartState != probeStartState_old){Serial.printf("%d us Task_Board_Event: Probe is not touching goal circuit\n", usecCount);};
  probeStartState_old = probeStartState; // store current value
  // Insert Probe Plug Check
  if (probeGoalState == BUTTON_ON && trialRunning == 1 && probeGoalLatch == 0)
  {
    probeGoalLatch = 1;
    TS_probeGoal = usecCount;
    ptsCollected = ptsCollected + PTS_PROBEINSERT;
    blink_LED(); //turn off LED when red button is pressed
    Serial.printf("%d us Trial_Event: Probe Plug inserted!\n", usecCount);
    StickCP2.Display.fillScreen(BLUE); //clear screen
    // screenSelector = 0;
    // trialRunning++;
  }
  return probeGoalLatch;
}

bool check_door(){
  // if (angleValue < ANGLESP && angleValue != angleValue_old){Serial.println("Door Angle Target Achieved");};
  // if (angleValue >= ANGLESP && angleValue != angleValue_old){Serial.println("Door Angle Target not Achieved");};
  angleValue_old = angleValue; // store current value
  // Angle & Circuit Probed Check
  if (angleValue < ANGLESP && trialRunning == 1 && angleDoorLatch == 0){
    delay(1);
    angleDoorLatch = 1;
    TS_angle_door = usecCount;
    Serial.printf("%d us Task_Board_Event: Door Angle achieved!\n", usecCount);

  }
  if (angleValue < ANGLESP && trialRunning == 1 && angleLatch == 0 && probeStartState == BUTTON_ON)
  {
    delay(1);
    angleLatch = 1;
    TS_angle = usecCount;
    Serial.printf("%d us Trial_Event: Terminal Block Circuit Probed!\n", usecCount);
    ptsCollected = ptsCollected + PTS_CIRCUIT_PROBE;
    blink_LED(); //turn off LED when red button is pressed
    StickCP2.Display.fillScreen(BLUE); //clear screen
    // screenSelector = 5;
    // trialRunning++;
  }
  return angleDoorLatch;
}

// bool check_TB_circuit(){
      // return something
// }

bool check_cable_posts(){
  // Cable Wrapping Check
  if (OP180_1_State == BUTTON_OFF && OP180_2_State == BUTTON_OFF && trialRunning == 1 && cableWrapLatch == 0)
  {
    delay(1);
    cableWrapLatch = 1;
    TS_cableWrap = usecCount;
    Serial.printf("%d us Trial_Event: Cable successfully wrapped!\n", usecCount);
  }
  
  return cableWrapLatch;
}

bool check_probe_tip_holder(){
  // Probe Stowed Check
  if (OP180_1_State == BUTTON_OFF && OP180_2_State == BUTTON_OFF && trialRunning == 1 && cableWrapProbeStowLatch == 0 && angleLatch == 1 && probeGoalState == BUTTON_ON)
  {
    delay(1);
    cableWrapProbeStowLatch = 1;
    TS_cableWrapProbeStow = usecCount;
    Serial.printf("%d us Trial_Event: Cable successfully wrapped AND Probe Tip Stowed!\n", usecCount);
    ptsCollected = ptsCollected + PTS_CABLEWRAP;
    blink_LED(); //turn off LED when red button is pressed
    StickCP2.Display.fillScreen(BLUE); //clear screen
    // screenSelector = 6;
    // trialRunning++;
  }
  return cableWrapProbeStowLatch;
}

void check_trialStartStopLogic(){
  // Start Trial on M5 Button Press Check
  if (startBtnState == BUTTON_ON && trialRunning == 0 && stopBtnState == BUTTON_OFF && forceStop == 0) 
  {
    delay(1);
    if (OP180_1_State == 1 || OP180_2_State == 1){
      // flash alert screen to unwind cable
      StickCP2.Display.setCursor(5,5);
      StickCP2.Display.setTextColor(WHITE, RED);
      StickCP2.Display.setTextSize(2);
      StickCP2.Display.fillScreen(RED);
      StickCP2.Display.printf("Unwind the cable \n from the wrap \n posts!");
      delay(1000);
      StickCP2.Display.setTextColor(WHITE, BLACK);
      StickCP2.Display.fillScreen(BLACK);
    }
    if (faderValue > 20){
      // WARNING flash alert screen to adjust fader
      StickCP2.Display.setCursor(5,5);
      StickCP2.Display.setTextColor(WHITE, RED);
      StickCP2.Display.setTextSize(2);
      StickCP2.Display.fillScreen(RED);
      StickCP2.Display.printf("Move fader to left\n end stop!");
      delay(1000);
      StickCP2.Display.setTextColor(WHITE, BLACK);
      StickCP2.Display.fillScreen(BLACK);
    }
    if (angleValue < 3500){
      // WARNING flash alert screen to close door
      StickCP2.Display.setCursor(5,5);
      StickCP2.Display.setTextColor(WHITE, RED);
      StickCP2.Display.setTextSize(2);
      StickCP2.Display.fillScreen(RED);
      StickCP2.Display.printf("Close door!");
      delay(1000);
      StickCP2.Display.setTextColor(WHITE, BLACK);
      StickCP2.Display.fillScreen(BLACK);
    }
    if (probeGoalState == BUTTON_ON){
      // WARNING flash alert screen to close door
      StickCP2.Display.setCursor(5,5);
      StickCP2.Display.setTextColor(WHITE, RED);
      StickCP2.Display.setTextSize(2);
      StickCP2.Display.fillScreen(RED);
      StickCP2.Display.printf("Move probe plug to\n black port!");
      delay(1000);
      StickCP2.Display.setTextColor(WHITE, BLACK);
      StickCP2.Display.fillScreen(BLACK);
    }
    if (startBtnState == BUTTON_ON && faderValue < 20 && angleValue > 3500 && probeGoalState == BUTTON_OFF && OP180_1_State == 0 && OP180_2_State == 0){
      // Begin trial timer
      if (resetBtnState == BUTTON_ON){
        // human start option press and hold the reset button then pressing the trial start M5 button
        humanStart = 1;
      }
      countStart = 1;
      usecCount = 0; // reset trial timer
      TS_button = 0; TS_fader_mid = 0; TS_fader = 0; TS_probeGoal = 0; TS_angle = 0; TS_cableWrap = 0; TS_cableWrapProbeStow = 0;
      startaccX = accX;
      startaccY = accY;
      startaccZ = accZ;
      trialCounter++; //increment trial counter
      preferences.putUInt("trialCounter", trialCounter);  // Store the counter to the Preferences namespace
      Serial.printf("%d us Trial_Event: M5 Button pressed, Trial %d Started!\n", trialTime, trialCounter);
      StickCP2.Display.fillScreen(BLUE);
      StickCP2.Display.setTextColor(BLACK, BLUE);
      // screenSelector = 1;
    }
    delay(1);
  }

  // Stop Trial on RED Button Press Check
  if (stopBtnState == BUTTON_ON && trialRunning > 0 && buttonPushLatch == 1 && faderLatch == 1 && angleLatch == 1 && cableWrapProbeStowLatch == 1 && probeGoalLatch == 1)
  {
    delay(1);
    if (stopBtnState == BUTTON_ON){
      countStart = 0;
      turn_LED_off(); //turn off LED
      Serial.printf("%d us Trial_Event: Red Button pressed, End of Successful Trial %d! Congrats!\n", usecCount, trialCounter);
      // Serial.println("Trial_Event: Red Button pressed, End of Successful Trial! Congrats!");
      // trialRunning = 0; //this seems correct here but isn't compatible with the logic of countStart
      // screenSelector = 0;       
      trialCompletedLatch = 1;
      humanStart = 0;

    }
    delay(1);
  }
  // Report task board changes to Serial
  if (stopBtnState == 0 && stopBtnState != stopBtnState_old){Serial.printf("%d us Task_Board_Event: Red Push Button Pressed\n", usecCount);};
  if (stopBtnState == 1 && stopBtnState != stopBtnState_old){Serial.printf("%d us Task_Board_Event: Red Push Button Released\n", usecCount);};
  stopBtnState_old = stopBtnState; // store current value

  // FORCE Stop Trial on RED Button Press Check
  if (stopBtnState == BUTTON_ON && trialRunning > 0 && StickCP2.BtnA.wasPressed() == 1)
  {
    delay(1);
    if (stopBtnState == BUTTON_ON){
      countStart = 0; //stop the trial time counter
      turn_LED_off(); //turn off LED
      // Serial.printf("%d us Trial_Event: Trial Aborted!\n", trialTime);
      Serial.printf("Trial_Event: Trial %d Aborted!\n", trialCounter);
      trialRunning = 0; 
      StickCP2.Display.fillScreen(BLACK); //clear screen
      StickCP2.Display.setTextColor(WHITE, BLACK);
      timerAlarmDisable(interruptTimer);
      trialTime = usecCount;
      forceStop = 1;
      }
    delay(1);
  }

  // Reset the trial counter
  if (stopBtnState == BUTTON_ON && trialRunning == 0 && StickCP2.BtnA.wasPressed() == 1 && StickCP2.BtnB.wasPressed() ==1 && buttonPushState == BUTTON_ON)
  {
    resetCounter();
    StickCP2.Display.fillScreen(BLACK); //clear screen
  }

  // Time Limit Check
  timeLeft = round(TIMELIMIT - usecCount/1000000);
  if (trialRunning == 1 && timeLeft <= 0)
  {
    delay(1);
      countStart = 0;
      timerAlarmDisable(interruptTimer);
      Serial.printf("%d us Trial_Event: Trial Time Limit %d reached! Time's Up! \n", usecCount, TIMELIMIT);
      turn_LED_off(); //turn off LED
      for (int i = 0; i < 3; i++){
        StickCP2.Display.fillScreen(RED);
        StickCP2.Display.setCursor(5,5);
        StickCP2.Display.setTextSize(3);
        StickCP2.Display.setTextColor(BLACK);
        StickCP2.Display.printf("TRIAL OVER!");
        delay(200);
        StickCP2.Display.fillScreen(BLACK);
        delay(200);
        // screenSelector = 0;
      }
    delay(1);
  }

  

  

  

  

  

  // Time Count Start
  if (countStart == 1 && trialRunning == 0)
  {
    timerAlarmEnable(interruptTimer);
    trialRunning = 1;
  }

  // Time Count Stop
  if (countStart == 0 && trialRunning > 0)
  // if (countStart == 0)
  {
    timerAlarmDisable(interruptTimer);
    trialRunning = 0;
    trialTime = usecCount;
    StickCP2.Display.fillScreen(BLACK); //clear screen
    StickCP2.Display.setTextColor(WHITE, BLACK);
  }

  // Count Reset Check
  if (resetBtnState == BUTTON_ON && trialRunning == 0 && trialTime != 0)
  {
    Serial.println("Trial Reset Button pressed");
    forceStop = 0;
    usecCount = 0;
    buttonPushLatch = 0; faderLatch = 0; faderLatch2 = 0; angleLatch = 0; cableWrapProbeStowLatch = 0; probeGoalLatch = 0;
    TS_button = 0; TS_fader_mid = 0; TS_fader = 0; TS_angle = 0; TS_angle_door = 0; TS_cableWrap = 0; TS_cableWrapProbeStow = 0; TS_probeGoal = 0;
    trialTime = 0;
    display[0] = 0; display[1] = 0; display[2] = 0;
    ptsCollected = 0;
    cumForce = 0;
    turn_LED_off(); //turn off LED
    StickCP2.Display.fillScreen(BLACK); //clear screen
    StickCP2.Display.setTextColor(WHITE, BLACK);
    trialCompletedLatch = 0;
  }

  // collect "force" during trial
  if (trialRunning > 0)
  {
    force = abs(accX - startaccX) + abs(accY - startaccY) + abs(accZ - startaccZ);  
    cumForce = cumForce + force;
  }
}

void update_screen(){
  // Screen switching logic
  if(StickCP2.BtnB.wasPressed()){
    screenSelector_count++;
    if(screenSelector_count >= 50){
      StickCP2.Display.fillScreen(WHITE);
      delay(50);
      if (trialRunning == 1){
        StickCP2.Display.fillScreen(BLUE);  
      } else {
        StickCP2.Display.fillScreen(BLACK);
      }
      if (screenSelector < 7){
        screenSelector++;
      } else {
        screenSelector = 0;
      }
      screenSelector_count = 0; //reset button debounce
      Serial.printf("value of screenSelector is %d\n", screenSelector);
    }
  }

  // update home screen display
  switch(screenSelector){
    case 0: //Idle
      home_screen();
      break;
    case 1:
      screen2();
      break;
    case 2: 
      screen4();
      break;
    case 3: 
      screen3();
      break;
    case 4: 
      screen5();
      break;
    case 5:
      screen6();
      break;
    case 6:
      screen7();
      break;
    case 7:
      // developer screen showing all IO values
      develop_screen();
      break;
    default:
      Serial.print("No code is available");
      break;
  }
}

void send_telemetry(){
  if (wifiEnabled == 1)
  {
    // initServerConnection();
    // // Connect to wifi logic
    // if (!client.connected()) {
    //   Serial.println("Attempting to connect to WiFi...");
    //   reconnect();
    // }
    // //client.loop(); //SUSPICIOUS if this is really needed... causes irregular loop execution speeds

    // Reporting logic to remote server
    if (trialRunning == 1 && now - lastPublish >= trialPublishRate){
      lastPublish += trialPublishRate;
      publish_telemetry();
    }
    if (now - lastPublish >= fiveSeconds) // publish to topic every 5 seconds
    {
      lastPublish += fiveSeconds;
      publish_telemetry();
    }
  }
}

void turn_LED_on(){
    digitalWrite(19, HIGH); //turn on LED
}

void turn_LED_off(){
    digitalWrite(19, LOW); //turn off LED
}

void blink_LED(){
    digitalWrite(19, HIGH); //turn on LED
    delay(100);
    digitalWrite(19, LOW); //turn off LED
}

void setup()
{
  // initialize the M5Stack object
  auto cfg = M5.config();
  StickCP2.begin(cfg);
  porthub.begin();
  Serial.begin(115200);
  
  // // GPIO setting  
  pinMode(19, OUTPUT);  //GPIO19 for M5StickCPlus2 the builtin LED <<THIS WAS THE PROBLEM!

  // // Print out the device's unique MAC address
  Serial.print("ESP Board MAC Address: ");
  Serial.println(WiFi.macAddress());

  // Build string for SSID
  task_board_ssid = String("AutoConnect_");
  unique_ssid = String();
  unique_ssid = task_board_ssid + TOKEN.c_str();
  
  // Configure Lcd display setup
  StickCP2.Display.setRotation(3);
  StickCP2.Display.fillScreen(BLACK);
  StickCP2.Display.setTextColor(WHITE, BLACK);
  StickCP2.Display.setTextSize(1);
  StickCP2.Display.setCursor(0, 5);

  // // Setup WiFi connection or boot in LOCAL MODE
  if (StickCP2.BtnA.wasPressed() != 1) // this should be != 1 so that the default behavior is to connect to WiFi
  { 
    StickCP2.Display.print(" Smart Task Board ");
    StickCP2.Display.printf("v%s\n ", FW_VERSION);
    StickCP2.Display.printf("TOKEN:%s\n\n", TOKEN.c_str());
    StickCP2.Display.print(" Set default WiFi by connecting to board\n");
    StickCP2.Display.print(" SSID with a PC then browse to \n");
    StickCP2.Display.print(" \"192.168.4.1\" and select preferred\n");
    StickCP2.Display.print(" WiFi network and enter password.\n");
    StickCP2.Display.print(" Board will then attempt to autoconnect.\n\n");
    StickCP2.Display.printf(" Task Board SSID: \n %s\n", unique_ssid.c_str());
    StickCP2.Display.printf(" Password: %s\n\n", TASK_BOARD_PW);
    StickCP2.Display.print(" Connecting to last saved WiFi SSID...\n");
    StickCP2.Display.print(" Use w/o WiFi: reboot while holding M5 btn");
    wifiEnabled = 1; //replace this with res variable
    
    //Wifi Manager Config START
    WiFi.mode(WIFI_STA); // explicitly set mode, esp defaults to STA+AP
  
    Serial.printf("Starting wifimanager...\n");
    //WiFiManager, Local intialization. Once its business is done, there is no need to keep it around
    WiFiManager wm;
  
    //reset settings - wipe credentials for testing
    // wm.resetSettings(); //leave commented
  
    bool res; // connection status of the wifimanager
    res = wm.autoConnect(unique_ssid.c_str(), TASK_BOARD_PW); // password protected ap

    if(!res) {
        Serial.println("Failed to connect");
        // ESP.restart();
    } 
    else {
        //if you get here you have connected to the WiFi    
        Serial.println("connected...yeey :)");
    }
    //Wifi Manager Config END
    Serial.printf("End of wifimanager code block.\n");
    

    // Setup wireless connection
    client.setServer(mqtt_server, 1883);
    client.setBufferSize(8192); // Increase limit, this line fixed problem for my device
    // setup_wifi(); // <<== this is suspicious, what does this function do? No definition found. Delete!

    //Check for new firmware
    turn_LED_on();

    client.setCallback(handleOtaUpdate);
    initServerConnection();
    delay(1000); 
    reportCurrentFirmwareVersion();
    requestNewFirmware(); 

    turn_LED_off(); //turn off LED
  } else 
  { // Press and hold M5 Button during power up to enter LOCAL Mode
      StickCP2.Display.setCursor(5,5);
      StickCP2.Display.setTextSize(2);
      StickCP2.Display.print("Booting Local Mode, no WiFi!");
      Serial.printf("Booting Local Mode, no WiFi!\n");
      turn_LED_off(); //turn off LED
      wifiEnabled = 0;
      delay(1000);
  }

    // TODO: replace this timer with a millis() command.
    //interrupt timer setting
    //timerBegin is count per 100 microsec.
    interruptTimer = timerBegin(0, 80, true);
    //interrupt method setting
    timerAttachInterrupt(interruptTimer, &usecTimer, true);
    //interrupt timing setting.
    timerAlarmWrite(interruptTimer, 5, true);
    timerAlarmDisable(interruptTimer);
    //timerAlarmEnable(interupptTimer);
    
  //  if (client.setBufferSize(1023)) {
  //    Serial.println("Successfully reallocated internal buffer size");
  //  } else {
  //    Serial.println("Failed to reallocated internal buffer size");
  //  }

    

  //   // Setup load cell device
  //   //REMOVED

  preferences.begin("task-board",false); // second parameter must be false
  resetCounter();
  //   // trialCounter = preferences.getUInt("trialCounter", 0);  // Get the counter value in current namesapce, if no key exists then return default value as second parameter

  StickCP2.Display.fillScreen(BLACK); // clear screen
}

void loop()
{
  // put your main code here, to run repeatedly:
  last = now;
  now = millis();
  Serial.printf("scan timer: %dms\n", now - last);

  // main function refactor
  update_inputs();
  if (trialRunning) {
    // if (trialProtocolComplete || trialAborted || trialTimeUp) {
    //   trialRunning = 0;
    // }
    // update trialTime variable in telemetry while trial is running
    trialTime = usecCount;

    //time calculation
    display[2] = (int)((trialTime % 1000000) / 1000);   // milliseconds
    display[1] = (int)((trialTime / 1000000) % 60);     // seconds
    display[0] = (int)((trialTime / 60000000) % 3600);  // minutes
  }
  check_blue_button();
  check_slider();
  check_probe_plug();
  check_door();
  // check_TB_circuit(); //empty and causing crashes when uncommented
  check_cable_posts();
  check_probe_tip_holder();
  check_trialStartStopLogic();
  update_screen();
  send_telemetry();
  
  //print out seconds to the serial monitor
  if (verbose == 1){
     Serial.printf("DeviceToken:%s, State:Btn:%d,Fader:%d,Angle:%d,ProbeInserted:%d,CircuitProbed:%d,Post1:%d,Post2:%d, Protocol:%s, Batt:%d, TrialRunning:%d, TimeLeft_sec:%d, TrialPts:%d, TotalTrialForce:%0.2f, Time_us:%d\n", TOKEN.c_str(), buttonPushState, faderValue, angleValue, probeStartState, probeGoalState, OP180_1_State, OP180_2_State, PROTOCOL_ID, StickCP2.Power.getBatteryVoltage(), trialRunning, timeLeft, ptsCollected, cumForce, usecCount);
  }
}
