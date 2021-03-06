#include <XJM_EasyOTA.h>
#include <ESPAsyncWebServer.h>
#include <FS.h>
#include <SPIFFS.h>
#include <SPIFFSEditor.h>
#include "ReflowController_v1.h"
#include <ArduinoJson.h>
#include "AsyncJson.h"
#include "Config.h"

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
AsyncEventSource events("/event");
ControllerBase * controller = NULL;
ControllerBase * last_controller = NULL;
AsyncWebSocketClient * _client = NULL;
Config config("/config.json", "/profiles.json");


void textThem(const char * text) {
	int tryId = 0;
  for (int count = 0; count < ws.count();) {
    if (ws.hasClient(tryId)) {
      ws.client(tryId)->text(text);
      count++;
    }
    tryId++;
  }
}

void textThem(const String& text) {
	textThem(text.c_str());
}

void textThem(JsonObject &root, AsyncWebSocketClient * client) {
	String json;
	root.printTo(json);

	if (client != NULL)
		client->text(json);
	else
		textThem(json);
}

void textThem(JsonObject &root)
{
	textThem(root, NULL);
}

void send_reading(float reading, float target, float time, AsyncWebSocketClient * client, bool reset)
{
	S_printf("Sending readings...");
	char str[255] = "";

	StaticJsonBuffer<200> jsonBuffer;
	JsonObject &root = jsonBuffer.createObject();

	JsonObject& data = root.createNestedObject("readings");
	JsonArray &times = root.createNestedArray("times");
	JsonArray &readings = root.createNestedArray("readings");
	JsonArray &targets = root.createNestedArray("targets");

	times.add(time);
	readings.add(reading);
	targets.add(target);
	data["reset"] = reset;

	textThem(root);
}

void setupController(ControllerBase * c)
{
	ControllerBase * tmp = controller;
	controller = NULL;

	S_printf("Controller setup..");

	// report messages
	c->onMessage([](const char * msg) {
		StaticJsonBuffer<200> jsonBuffer;
		JsonObject &root = jsonBuffer.createObject();
		root["message"] = msg;
		textThem(root);
	});

	c->onHeater([](bool heater) {
		S_printf("Heater: %s", heater ? "on" : "off");
		StaticJsonBuffer<200> jsonBuffer;
		JsonObject &root = jsonBuffer.createObject();
		root["heater"] = heater;
		textThem(root);
	});

	// report readings
	c->onReadingsReport([](const std::vector<ControllerBase::Temperature_t>& readings, unsigned long elapsed){
		send_reading(controller->log_to_temperature(readings[readings.size() - 1]), controller->target(), elapsed/1000.0, NULL, readings.size() == 1);
	});

	// report mode change
	c->onMode([](ControllerBase::MODE_t last, ControllerBase::MODE_t current){
		S_printf("Change mode: from %s to %s", controller->translate_mode(last), controller->translate_mode(current));
		StaticJsonBuffer<200> jsonBuffer;
		JsonObject &root = jsonBuffer.createObject();
		root["mode"] = controller->translate_mode(current);

		textThem(root);
	});
	c->onStage([](const char * stage, float target){
		S_printf("Reflow stage: %s", stage);
		StaticJsonBuffer<200> jsonBuffer;
		JsonObject &root = jsonBuffer.createObject();
		root["stage"] = stage;
		root["target"] = target;
		textThem(root);
	});

	last_controller = tmp;
	controller = c;
	S_printf("Controller setup DONE");
}

void send_data(AsyncWebSocketClient * client)
{
	S_printf("Sending all data...");
	std::vector<ControllerBase::Temperature_t>::iterator I = controller->readings().begin();
	std::vector<ControllerBase::Temperature_t>::iterator end = controller->readings().end();
	DynamicJsonBuffer jsonBuffer;
	JsonObject &root = jsonBuffer.createObject();
	JsonArray &times = root.createNestedArray("times");
	JsonArray &readings = root.createNestedArray("readings");
	JsonArray &targets = root.createNestedArray("targets");
	root["reset"] = true;
	root["message"] = "INFO: Connected!";
	root["mode"] = controller->translate_mode();
	root["target"] = controller->target();
	root["profile"] = controller->profile();
	root["stage"] = controller->stage();
	root["heater"] = controller->heater();

	float seconds = 0;
	while (I != end)
	{
		times.add(seconds);
		readings.add(controller->log_to_temperature(*I));
		targets.add(controller->target());
		seconds += config.reportInterval / 1000.0;
		I ++;
	}

	textThem(root, client);
}

void setup() {
	Serial.begin(115200);

	// Seems we have to send an argument?
	SPIFFS.begin(false);
	config.load_config();
	config.load_profiles();
	config.setup_OTA();

	server.addHandler(&ws);
	server.addHandler(&events);
	server.serveStatic("/", SPIFFS, "/web").setDefaultFile("index.html");

	// Heap for general Servertest
	server.on("/heap", HTTP_GET, [](AsyncWebServerRequest *request) {

		Serial.println("** DEBUG - main.cpp - server.on GET(/heap)");
		request->send(200, "text/plain", String(ESP.getFreeHeap()));
	});
	server.on("/profiles", HTTP_GET, [](AsyncWebServerRequest *request) {
		Serial.println("** DEBUG - main.cpp - server.on GET(/profile)");
		AsyncWebServerResponse *response = request->beginResponse(SPIFFS, "/profiles.json");
		//request->send(SPIFFS, "/profiles.json");
		response->addHeader("Access-Control-Allow-Origin", "*");
		response->addHeader("Access-Control-Allow-Methods", "GET");
		response->addHeader("Content-Type", "application/json");
		request->send(response);
	});
	server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request) {
		Serial.println("** DEBUG - main.cpp - server.on GET(/config)");
		AsyncWebServerResponse *response = request->beginResponse(SPIFFS, "/config.json");
		//request->send(SPIFFS, "/profiles.json");
		response->addHeader("Access-Control-Allow-Origin", "*");
		response->addHeader("Access-Control-Allow-Methods", "GET");
		response->addHeader("Content-Type", "application/json");
		request->send(response);
	});
	server.on("/profiles", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
		config.save_profiles(request, data, len, index, total);
		config.load_profiles();
	});
	server.on("/config", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
		config.save_config(request, data, len, index, total);
	});
	server.on("/calibration", HTTP_GET, [](AsyncWebServerRequest *request) {
		AsyncWebServerResponse *response = request->beginResponse(200, "application/json", controller->calibrationString());
		response->addHeader("Access-Control-Allow-Origin", "*");
		response->addHeader("Access-Control-Allow-Methods", "GET");
		request->send(response);
	});
	server.onNotFound(
			[](AsyncWebServerRequest *request) { request->send(404); });

	ws.onEvent([](AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len){
		if (type == WS_EVT_DATA) {
			char cmd[256] = "";
			memcpy(cmd, data, min(len, sizeof(cmd) - 1));

			controller->watchdog(millis());

			Serial.print("** debug - main - onEvent cmd: ");
			Serial.println(cmd);
			if (strcmp(cmd, "WATCHDOG") == 0) {
			} else if (strncmp(cmd, "profile:", 8) == 0) {
				controller->profile(String(cmd + 8));
				sprintf(cmd, "{\"profile\": \"%s\"}", controller->profile().c_str());
				textThem(cmd);
			} else if (strcmp(cmd, "ON") == 0) {
				controller->mode(ControllerBase::ON);
			} else if (strcmp(cmd, "REBOOT") == 0) {
				ESP.restart();
			} else if (strcmp(cmd, "CALIBRATE") == 0) {
				controller->mode(ControllerBase::CALIBRATE);
			} else if (strcmp(cmd, "TARGET_PID") == 0) {
				controller->mode(ControllerBase::TARGET_PID);
			} else if (strcmp(cmd, "REFLOW") == 0) {
				controller->mode(ControllerBase::REFLOW);
			} else if (strcmp(cmd, "OFF") == 0) {
				controller->mode(ControllerBase::OFF);
			} else if (strcmp(cmd, "COOLDOWN") == 0) {
				controller->mode(ControllerBase::CALIBRATE_COOL);
			} else if (strcmp(cmd, "CURRENT-TEMPERATURE") == 0) {
				unsigned long now = millis();
				send_reading(controller->measure_temperature(now), controller->target(), controller->elapsed(now)/1000.0, NULL, false);
			} else if (strncmp(cmd, "target:", 7) == 0) {
				controller->target(max(0, min(atoi(cmd + 7), MAX_TEMPERATURE)));
				sprintf(cmd, "{\"target\": %.2f}", controller->target());
				textThem(cmd);
			}
		} else if (type == WS_EVT_CONNECT) {
			send_data(client);
			S_printf("Connected...");
		} else if (type == WS_EVT_DISCONNECT) {
			S_printf("Disconnected...");
			//controller->mode(ControllerBase::ERROR_OFF);
		}
	});
	uint64_t chipid;
	chipid = ESP.getEfuseMac();//The chip ID is essentially its MAC address(length: 6 bytes).
	Serial.printf("ESP32 Chip ID = %04X",(uint16_t)(chipid>>32));//print High 2 bytes
	Serial.printf("%08X\n",(uint32_t)chipid);//print Low 4bytes.
	delay(3000);
	
	Serial.println("** debug - main - Starting actual WebServer");
	server.begin();
	setupController(new ReflowController(config));

	S_printf("Server started..");
}

void loop() {
	unsigned long now = millis();

  	config.OTA->loop(now);
	// since this is single core, we don't care about
	// synchronization
	if (controller)
		controller->loop(now);
	if (last_controller) {
		delete last_controller;
		last_controller = NULL;
	}
}
