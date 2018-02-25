#ifndef CONFIG_H
#define CONFIG_H

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <EasyOTA.h>
#include <map>
#include "wificonfig.h"

class Config {
	typedef struct {
		float P, I, D;
	} PID_t;

	class Stage {
	public:
		Stage(const String& n, const String& p, float t, float s) :
		 name(n), pid(p), target(t), stay(s) {
			 stage_start_time = 0;
		 }

		String name;
		String pid;
		float target;
		float stay;

		unsigned long stage_start_time;

	};

	class Profile {
	public:
		Profile(JsonObject& json)
		{
			name = json["name"].asString();
			Serial.println("Profile long name: " + name);
			JsonArray& jo = json["stages"];
			JsonArray::iterator I = jo.begin();
			String stage_name;
			while (I != jo.end())
			{
				stage_name = I->asString();
				Stage s(
					stage_name,
					json[stage_name]["pid"].asString(),
					json[stage_name]["target"],
					json[stage_name]["stay"]
				);
				stages.push_back(s);
				Serial.println("Profile " + name + " stage: " + s.name);
				++I;
			}
		};

		std::vector<Stage>::iterator begin() {return stages.begin();}
		std::vector<Stage>::iterator end() {return stages.end();}

		std::vector<Stage> stages;
		String name;
	};

public:
	String cfgName;
	String profilesName;
	std::map<String, String> networks;

	std::map<String, PID_t> pid;
	std::map<String, Profile> profiles;

public:
	String hostname;
	String user;
	String password;
	String otaPassword;
	float measureInterval;
	float reportInterval;

	typedef std::function<bool(JsonObject& json, Config * self)> THandlerFunction_parse;

public:
	Config(const String& cfg, const String& profiles) : cfgName(cfg), profilesName(profiles) {}

	bool load_config() {
		return load_json(cfgName, 1024, [](JsonObject& json, Config* self){
			self->networks.empty();

			self->hostname = json["hostname"].asString();
			self->user = json["user"].asString();
			self->password = json["password"].asString();
			self->otaPassword = json["otaPassword"].asString();
			self->measureInterval = json["measureInterval"];
			self->reportInterval = json["reportInterval"];

			Serial.println("Config hostname:" + self->hostname);
			Serial.println("Config OTA Password:" + self->otaPassword);
			Serial.println("Config user:" + self->user);
			Serial.println("Config password:" + self->password);
			Serial.println("Config measure interval:" + String(self->measureInterval));
			Serial.println("Config report interval:" + String(self->reportInterval));

			JsonObject& jo = json["networks"];
			JsonObject::iterator I = jo.begin();
			while (I != jo.end())
			{
				self->networks.insert(std::pair<String, String>(I->key, I->value.asString()));
				Serial.println("Config network: " + String(I->key) + " @ " + I->value.asString());
				++I;
			}
			return true;
		});
	}

	bool load_profiles() {
		return load_json(profilesName, 10240, [](JsonObject& json, Config* self){
			self->pid.empty();
			JsonObject::iterator I;
			JsonObject& pid = json["pid"];
			I = pid.begin();
			while (I != pid.end())
			{
				PID_t p = {
					I->value[0],
					I->value[1],
					I->value[2],
				};
				self->pid.insert(std::pair<String, PID_t>(I->key, p));
				Serial.println("Profiles PID:" + String(I->key) + " [" + String(p.P) + ", " + String(p.I) + ", " + String(p.D) + "]");
				++I;
			}

			self->profiles.empty();
			JsonObject& profiles = json["profiles"];
			I = profiles.begin();
			while (I != profiles.end())
			{
				JsonObject &profile = I->value;
				Serial.println("Profile: " + String(I->key));
				Profile p(profile);
				self->profiles.insert(std::pair<String, Profile>(I->key, p));
				++I;
			}
			return true;
		});
	}

	bool load_json(const String& name, size_t max_size, THandlerFunction_parse parser) {
		Serial.println("Loading config " + name);
		File configFile = SPIFFS.open(name, "r");
		if (!configFile) {
			Serial.println("Could not open config file");
			return false;
		}

		size_t size = configFile.size();
		if (size > max_size) {
			configFile.close();
			Serial.println("config file size is too large: " + String(size));
			return false;
		}

		DynamicJsonBuffer jsonBuffer;
		JsonObject &json = jsonBuffer.parseObject(configFile);

		if (!json.success()) {
			Serial.println("Failed parsing config file");
			configFile.close();
			return false;
		}

		bool parsed = false;
		if (parser)
		 	parsed = parser(json, this);

		configFile.close();
		Serial.println("Loading config " + name + " DONE");
		return parsed;
	}

	bool setup_OTA(EasyOTA& OTA) {
		std::map<String, String>::iterator I = networks.begin();
		while (I != networks.end()) {
			OTA.addAP(I->first, I->second);
			I++;
		}

		OTA.addAP(WIFI_SSID, WIFI_PASSWORD);
	}

	bool save_config(AsyncWebServerRequest *request, uint8_t * data, size_t len, size_t index, size_t total) {
		return save_file(request, cfgName, data, len, index, total);
	}
	bool save_profiles(AsyncWebServerRequest *request, uint8_t * data, size_t len, size_t index, size_t total) {
		return save_file(request, profilesName, data, len, index, total);
	}

	bool save_file(AsyncWebServerRequest *request, const String& fname, uint8_t * data, size_t len, size_t index, size_t total)
	{
		Serial.println("Saving config " + fname +" len/index: " + String(len) + "/" +  String(index));

		File f = SPIFFS.open(fname, index != 0 ? "a" : "w");
	  if (!f) {
			request->send(404, "application/json", "{\"msg\": \"ERROR: couldn't " + fname + " file for writing!\"}");
			return false;
		}

		// TODO sanity checks

		f.write(data, len);

		if (f.size() >= total)
		{
			request->send(200, "application/json", "{\"msg\": \"INFO: " + fname + " saved!\"}");
			Serial.println("Saving config... DONE");
		}

		f.close();
		return true;
	}
};

#endif
