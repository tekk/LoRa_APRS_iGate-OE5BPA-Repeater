#include <map>

#include <Arduino.h>
#include <ETH.h>
#include <WiFiMulti.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <APRS-IS.h>
#include <SPIFFS.h>
#include <ESP-FTP-Server-Lib.h>
#include <FTPFilesystem.h>

#include "logger.h"

#include "LoRa_APRS.h"

#include "pins.h"
#include "display.h"
#include "configuration.h"

#if defined(ARDUINO_T_Beam) && !defined(ARDUINO_T_Beam_V0_7)
#include "power_management.h"
PowerManagement powerManagement;
#endif

portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;
hw_timer_t * timer = NULL;
volatile uint secondsSinceLastAPRSISBeacon = 0;
volatile uint secondsSinceLastDigiBeacon = 0;
volatile uint secondsSinceStartup = 0;
volatile uint secondsSinceDisplay = 0;

WiFiMulti WiFiMulti;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, 60*60);
FTPServer ftpServer;
Configuration Config;
APRS_IS * aprs_is = 0;
LoRa_APRS lora_aprs;
std::shared_ptr<APRSMessage> BeaconMsg;

volatile bool eth_connected = false;

String create_lat_aprs(double lat);
String create_long_aprs(double lng);

#ifdef ETH_BOARD
void setup_eth();
#else
void setup_wifi();
#endif

void load_config();
void setup_wifi();
void setup_ota();
void setup_lora();
void setup_ntp();
void setup_aprs_is();
void setup_timer();
void setup_ftp();

std::map<uint, std::shared_ptr<APRSMessage>> lastMessages;

// cppcheck-suppress unusedFunction
void setup()
{
	Serial.begin(115200);

#if defined(ARDUINO_T_Beam) && !defined(ARDUINO_T_Beam_V0_7)
	Wire.begin(SDA, SCL);
	if (!powerManagement.begin(Wire))
	{
		logPrintlnI("AXP192 init done!");
	}
	else
	{
		logPrintlnE("AXP192 init failed!");
	}
	powerManagement.activateLoRa();
	powerManagement.activateOLED();
	powerManagement.deactivateGPS();
#endif

	delay(500);
	logPrintlnA("LoRa APRS iGate & Digi by OE5BPA (Peter Buchegger)");
	logPrintlnA("Version: 20.49.0-dev");
	setup_display();
	show_display("OE5BPA", "LoRa APRS iGate & Digi", "by Peter Buchegger", "20.49.0-dev", 3000);

	load_config();
	setup_lora();
#ifdef ETH_BOARD
	setup_eth();
	setup_ota();
	setup_ntp();
	setup_ftp();
	setup_aprs_is();
#else
	if(Config.wifi.active)
	{
		setup_wifi();
		setup_ota();
		setup_ntp();
		setup_ftp();
	}
	else
	{
		// make sure wifi and bt is off if we don't need it:
		WiFi.mode(WIFI_OFF);
		btStop();
	}
	if(Config.aprs_is.active) setup_aprs_is();
#endif
	setup_timer();

	if(Config.display.overwritePin != 0)
	{
		pinMode(Config.display.overwritePin, INPUT);
		pinMode(Config.display.overwritePin, INPUT_PULLUP);
	}

	delay(500);
	logPrintlnI("setup done...");
	secondsSinceDisplay = 0;
}

// cppcheck-suppress unusedFunction
void loop()
{
	static bool display_is_on = true;
	if(Config.display.overwritePin != 0 && !digitalRead(Config.display.overwritePin))
	{
		secondsSinceDisplay = 0;
		display_is_on = true;
		setup_display();
	} else
	if(!Config.display.alwaysOn && secondsSinceDisplay > Config.display.timeout && display_is_on)
	{
		turn_off_display();
		display_is_on = false;
	}

	static bool beacon_aprs_is = Config.aprs_is.active && Config.aprs_is.beacon;
	static bool beacon_digi = Config.digi.active && Config.digi.beacon;

	if(Config.aprs_is.active && Config.aprs_is.beacon && secondsSinceLastAPRSISBeacon >= (Config.aprs_is.beaconTimeout*60))
	{
		portENTER_CRITICAL(&timerMux);
		secondsSinceLastAPRSISBeacon -= (Config.aprs_is.beaconTimeout*60);
		portEXIT_CRITICAL(&timerMux);
		beacon_aprs_is = true;
	}
	if(Config.digi.active && Config.digi.beacon && secondsSinceLastDigiBeacon >= (Config.digi.beaconTimeout*60))
	{
		portENTER_CRITICAL(&timerMux);
		secondsSinceLastDigiBeacon -= (Config.digi.beaconTimeout*60);
		portEXIT_CRITICAL(&timerMux);
		beacon_digi = true;
	}

	if(Config.ftp.active)
	{
		ftpServer.handle();
		static bool configWasOpen = false;
		if(configWasOpen && ftpServer.countConnections() == 0)
		{
			logPrintlnW("Maybe the config has been changed via FTP, lets restart now to get the new config...");
			Serial.println();
			ESP.restart();
		}
		if(ftpServer.countConnections() > 0)
		{
			configWasOpen = true;
		}
	}

	if(Config.wifi.active || eth_connected) ArduinoOTA.handle();
	if(Config.wifi.active && WiFiMulti.run() != WL_CONNECTED)
	{
		setup_display(); secondsSinceDisplay = 0; display_is_on = true;
		logPrintlnE("WiFi not connected!");
		show_display("ERROR", "WiFi not connected!");
		delay(1000);
		return;
	}
	if((eth_connected && !aprs_is->connected()) || (Config.aprs_is.active && !aprs_is->connected()))
	{
		setup_display(); secondsSinceDisplay = 0; display_is_on = true;
		logPrintI("connecting to APRS-IS server: ");
		logPrintI(Config.aprs_is.server);
		logPrintI(" on port: ");
		logPrintlnI(String(Config.aprs_is.port));
		show_display("INFO", "Connecting to APRS-IS server");
		if(!aprs_is->connect(Config.aprs_is.server, Config.aprs_is.port))
		{
			logPrintlnE("Connection failed.");
			logPrintlnI("Waiting 5 seconds before retrying...");
			show_display("ERROR", "Server connection failed!", "waiting 5 sec");
			delay(5000);
			return;
		}
		logPrintlnI("Connected to APRS-IS server!");
	}
	if(Config.aprs_is.active && aprs_is->available() > 0)
	{
		String str = aprs_is->getMessage();
		logPrintD("[" + timeClient.getFormattedTime() + "] ");
		logPrintlnD(str);
	}
	if(lora_aprs.hasMessage())
	{
		std::shared_ptr<APRSMessage> msg = lora_aprs.getMessage();

		setup_display(); secondsSinceDisplay = 0; display_is_on = true;
		show_display(Config.callsign, timeClient.getFormattedTime() + "         LoRa", "RSSI: " + String(lora_aprs.packetRssi()) + ", SNR: " + String(lora_aprs.packetSnr()), msg->toString());
		logPrintD("[" + timeClient.getFormattedTime() + "] ");
		logPrintD(" Received packet '");
		logPrintD(msg->toString());
		logPrintD("' with RSSI ");
		logPrintD(String(lora_aprs.packetRssi()));
		logPrintD(" and SNR ");
		logPrintlnD(String(lora_aprs.packetSnr()));

		String newData = msg->getAPRSBody()->getData();
    	int lastIndex = newData.lastIndexOf("!");
    	int index = newData.substring(0, lastIndex).lastIndexOf("!");
    	newData.replace("\n", "");
    	String rssi = String(" DR:" + String(lora_aprs.packetRssi()) + " DS:" + String(lora_aprs.packetSnr()) + " ");
	    newData = String(newData.substring(0, index) + rssi + newData.substring(index) + "\n"); 
	    msg->getAPRSBody()->setData(newData);

		if(Config.aprs_is.active)
		{
			aprs_is->sendMessage(msg->encode());
		}
		if(Config.digi.active)
		{
			if(msg->getSource().indexOf(Config.callsign) != -1)
			{
				logPrintD("Message already received as repeater: '");
				logPrintD(msg->toString());
				logPrintD("' with RSSI ");
				logPrintD(String(lora_aprs.packetRssi()));
				logPrintD(" and SNR ");
				logPrintlnD(String(lora_aprs.packetSnr()));
				return;
			}

			// lets try not to flood the LoRa frequency in limiting the same messages:
			std::map<uint, std::shared_ptr<APRSMessage>>::iterator foundMsg = std::find_if(lastMessages.begin(), lastMessages.end(), [&](std::pair<const unsigned int, std::shared_ptr<APRSMessage> > & old_msg)
				{
					if(msg->getSource() == old_msg.second->getSource() &&
						msg->getDestination() == old_msg.second->getDestination() &&
						msg->getAPRSBody()->getData() == old_msg.second->getAPRSBody()->getData())
					{
						return true;
					}
					return false;
				});

			if(foundMsg == lastMessages.end())
			{
				setup_display(); secondsSinceDisplay = 0; display_is_on = true;
				show_display(Config.callsign, "RSSI: " + String(lora_aprs.packetRssi()) + ", SNR: " + String(lora_aprs.packetSnr()), msg->toString(), 0);
				logPrintD("Received packet '");
				logPrintD(msg->toString());
				logPrintD("' with RSSI ");
				logPrintD(String(lora_aprs.packetRssi()));
				logPrintD(" and SNR ");
				logPrintlnD(String(lora_aprs.packetSnr()));
				msg->setPath(String(Config.callsign) + "*");
				lora_aprs.sendMessage(msg);
				lastMessages.insert({secondsSinceStartup, msg});
			}
			else
			{
				logPrintD("Message already received (timeout): '");
				logPrintD(msg->toString());
				logPrintD("' with RSSI ");
				logPrintD(String(lora_aprs.packetRssi()));
				logPrintD(" and SNR ");
				logPrintlnD(String(lora_aprs.packetSnr()));
			}
			return;
		}
	}
	if(Config.digi.active)
	{
		for(std::map<uint, std::shared_ptr<APRSMessage>>::iterator iter = lastMessages.begin(); iter != lastMessages.end(); )
		{
			if(secondsSinceStartup >= iter->first + Config.digi.forwardTimeout*60)
			{
				iter = lastMessages.erase(iter);
			}
			else
			{
				++iter;
			}
		}
	}
	if(beacon_digi)
	{
		beacon_digi = false;
		setup_display(); secondsSinceDisplay = 0; display_is_on = true;
		show_display(Config.callsign, "Beacon to HF...");
		logPrintD("[" + timeClient.getFormattedTime() + "] ");
		logPrintlnD(BeaconMsg->encode());
		lora_aprs.sendMessage(BeaconMsg);
		logPrintlnD("finished TXing...");
		show_display(Config.callsign, "Standby...");
	}
	if(beacon_aprs_is)
	{
		beacon_aprs_is = false;
		setup_display(); secondsSinceDisplay = 0; display_is_on = true;
		show_display(Config.callsign, "Beacon to APRS-IS Server...");
		logPrintD("[" + timeClient.getFormattedTime() + "] ");
		logPrintlnD(BeaconMsg->encode());
		aprs_is->sendMessage(BeaconMsg);
		show_display(Config.callsign, "Standby...");
	}
}

void load_config()
{
	ConfigurationManagement confmg("/is-cfg.json");
	Config = confmg.readConfiguration();
	if(Config.callsign == "NOCALL-10")
	{
		logPrintlnE("You have to change your settings in 'data/is-cfg.json' and upload it via \"Upload File System image\"!");
		show_display("ERROR", "You have to change your settings in 'data/is-cfg.json' and upload it via \"Upload File System image\"!");
		while (true)
		{}
	}

#ifndef ETH_BOARD
	if(Config.aprs_is.active && !Config.wifi.active)
	{
		logPrintlnE("You have to activate Wifi for APRS IS to work, please check your settings!");
		show_display("ERROR", "You have to activate Wifi for APRS IS to work, please check your settings!");
		while (true)
		{}
	}
#endif

	if(KEY_BUILTIN != 0 && Config.display.overwritePin == 0)
	{
		Config.display.overwritePin = KEY_BUILTIN;
	}
	logPrintlnI("Configuration loaded!");
}

#ifdef ETH_BOARD
void WiFiEvent(WiFiEvent_t event)
{
	switch (event) {
	case SYSTEM_EVENT_ETH_START:
		logPrintlnI("ETH Started");
		ETH.setHostname("esp32-ethernet");
		break;
	case SYSTEM_EVENT_ETH_CONNECTED:
		logPrintlnI("ETH Connected");
		break;
	case SYSTEM_EVENT_ETH_GOT_IP:
		logPrintI("ETH MAC: ");
		logPrintI(ETH.macAddress());
		logPrintI(", IPv4: ");
		logPrintI(ETH.localIP().toString());
		if (ETH.fullDuplex()) {
			logPrintI(", FULL_DUPLEX");
		}
		logPrintI(", ");
		logPrintI(String(ETH.linkSpeed()));
		logPrintlnI("Mbps");
		eth_connected = true;
		break;
	case SYSTEM_EVENT_ETH_DISCONNECTED:
		logPrintlnW("ETH Disconnected");
		eth_connected = false;
		break;
	case SYSTEM_EVENT_ETH_STOP:
		logPrintlnW("ETH Stopped");
		eth_connected = false;
		break;
	default:
		break;
	}
}

void setup_eth()
{
	WiFi.onEvent(WiFiEvent);

	pinMode(NRST, OUTPUT);
	digitalWrite(NRST, 0);
	delay(200);
	digitalWrite(NRST, 1);
	delay(200);
	digitalWrite(NRST, 0);
	delay(200);
	digitalWrite(NRST, 1);

	ETH.begin(ETH_ADDR, ETH_POWER_PIN, ETH_MDC_PIN, ETH_MDIO_PIN, ETH_TYPE, ETH_CLK);
	while(!eth_connected)
	{
		sleep(1);
	}
}
#else
void setup_wifi()
{
	WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
	WiFi.setHostname(Config.callsign.c_str());
	for(Configuration::Wifi::AP ap : Config.wifi.APs)
	{
		logPrintD("Looking for AP: ");
		logPrintlnD(ap.SSID);
		WiFiMulti.addAP(ap.SSID.c_str(), ap.password.c_str());
	}
	logPrintlnI("Waiting for WiFi");
	show_display("INFO", "Waiting for WiFi");
	while(WiFiMulti.run() != WL_CONNECTED)
	{
		show_display("INFO", "Waiting for WiFi", "....");
		delay(500);
	}
	logPrintlnI("WiFi connected");
	logPrintD("IP address: ");
	logPrintlnD(WiFi.localIP().toString());
	show_display("INFO", "WiFi connected", "IP: ", WiFi.localIP().toString(), 2000);
}
#endif

void setup_ota()
{
	ArduinoOTA
		.onStart([]()
		{
			String type;
			if (ArduinoOTA.getCommand() == U_FLASH)
				type = "sketch";
			else // U_SPIFFS
				type = "filesystem";
			Serial.println("Start updating " + type);
			show_display("OTA UPDATE", "Start update", type);
		})
		.onEnd([]()
		{
			Serial.println();
			Serial.println("End");
		})
		.onProgress([](unsigned int progress, unsigned int total)
		{
			Serial.print("Progress: ");
			Serial.print(progress / (total / 100));
			Serial.println("%");
			show_display("OTA UPDATE", "Progress: ", String(progress / (total / 100)) + "%");
		})
		.onError([](ota_error_t error) {
			Serial.print("Error[");
			Serial.print(error);
			Serial.print("]: ");
			if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
			else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
			else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
			else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
			else if (error == OTA_END_ERROR) Serial.println("End Failed");
		});
	ArduinoOTA.setHostname(Config.callsign.c_str());
	ArduinoOTA.begin();
	logPrintlnI("OTA init done!");
}

void setup_lora()
{
	lora_aprs.setRxFrequency(Config.lora.frequencyRx);
	lora_aprs.setTxFrequency(Config.lora.frequencyTx);
	if (!lora_aprs.begin(lora_aprs.getRxFrequency()))
	{
		logPrintlnE("Starting LoRa failed!");
		show_display("ERROR", "Starting LoRa failed!");
		while (1);
	}
	lora_aprs.setTxPower(Config.lora.power);
	lora_aprs.setSpreadingFactor(Config.lora.spreadingFactor);
	lora_aprs.setSignalBandwidth(Config.lora.signalBandwidth);
	lora_aprs.setCodingRate4(Config.lora.codingRate4);
	logPrintlnI("LoRa init done!");
	show_display("INFO", "LoRa init done!", 2000);

	BeaconMsg = std::shared_ptr<APRSMessage>(new APRSMessage());
	BeaconMsg->setSource(Config.callsign);
	BeaconMsg->setDestination("APLG0");
	String lat = create_lat_aprs(Config.beacon.positionLatitude);
	String lng = create_long_aprs(Config.beacon.positionLongitude);
	BeaconMsg->getAPRSBody()->setData(String("=") + lat + "L" + lng + "&" + Config.beacon.message);
}

void setup_ntp()
{
	timeClient.begin();
	while(!timeClient.forceUpdate())
	{
		logPrintlnW("NTP Client force update issue! Waiting 1 sek...");
		show_display("WARN", "NTP Client force update issue! Waiting 1 sek...", 1000);
	}
	logPrintlnI("NTP Client init done!");
	show_display("INFO", "NTP Client init done!", 2000);
}

void setup_aprs_is()
{
	aprs_is = new APRS_IS(Config.callsign, Config.aprs_is.password , "ESP32-APRS-IS", "0.1");
}

void IRAM_ATTR onTimer()
{
	portENTER_CRITICAL_ISR(&timerMux);
	secondsSinceLastAPRSISBeacon++;
	secondsSinceLastDigiBeacon++;
	secondsSinceStartup++;
	secondsSinceDisplay++;
	portEXIT_CRITICAL_ISR(&timerMux);
}

void setup_timer()
{
	timer = timerBegin(0, 80, true);
	timerAlarmWrite(timer, 1000000, true);
	timerAttachInterrupt(timer, &onTimer, true);
	timerAlarmEnable(timer);
}

void setup_ftp()
{
	if(!Config.ftp.active)
	{
		return;
	}
	for(Configuration::Ftp::User user : Config.ftp.users)
	{
		logPrintD("Adding user to FTP Server: ");
		logPrintlnD(user.name);
		ftpServer.addUser(user.name, user.password);
	}
	ftpServer.addFilesystem("SPIFFS", &SPIFFS);
	ftpServer.begin();
	logPrintlnI("FTP Server init done!");
}

String create_lat_aprs(double lat)
{
	char str[20];
	char n_s = 'N';
	if(lat < 0)
	{
		n_s = 'S';
	}
	lat = std::abs(lat);
	sprintf(str, "%02d%05.2f%c", (int)lat, (lat - (double)((int)lat)) * 60.0, n_s);
	String lat_str(str);
	return lat_str;
}

String create_long_aprs(double lng)
{
	char str[20];
	char e_w = 'E';
	if(lng < 0)
	{
		e_w = 'W';
	}
	lng = std::abs(lng);
	sprintf(str, "%03d%05.2f%c", (int)lng, (lng - (double)((int)lng)) * 60.0, e_w);
	String lng_str(str);
	return lng_str;
}
