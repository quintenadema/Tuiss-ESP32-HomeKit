#include "HomeSpan.h"
#include <string>
#include <vector>
#include <NimBLEDevice.h>

// BLE UUIDs and Constants
#define SERVICE_UUID "00010203-0405-0607-0809-0A0B0C0D1910"
#define WRITE_UUID "00010405-0405-0607-0809-0A0B0C0D1910"
#define NOTIFY_UUID "00010304-0405-0607-0809-0A0B0C0D1910"
#define KEEP_ALIVE "ff03030303787878787878"
#define BLIND_NAME "TS3000"
#define BLIND_COUNT 2

struct BlindConnection {
    BLEClient* client = nullptr;
    boolean isConnected = false;
    String name;
    BLEAddress address;
};

std::vector<BlindConnection> blindConnections(BLIND_COUNT);
unsigned long lastKeepalive = 0;
static const unsigned long KEEPALIVE_INTERVAL = 30000; // 30 seconds

// Global variables for position control
int pendingPosition = -1;
unsigned long lastPositionChange = 0;
static const unsigned long DEBOUNCE_DELAY = 1200; // 1.2 seconds (as homekit sends every 1 sec)

// Utility functions for hex conversion
byte hexToByte(const char* hex) {
    byte val = 0;
    for(int i = 0; i < 2; i++) {
        val <<= 4;
        val |= (hex[i] >= 'A') ? (hex[i] - 'A' + 10) : (hex[i] - '0');
    }
    return val;
}

std::vector<byte> hexStringToBytes(const String& hexString) {
    std::vector<byte> bytes;
    for(unsigned int i = 0; i < hexString.length(); i += 2) {
        bytes.push_back(hexToByte(hexString.substring(i, i+2).c_str()));
    }
    return bytes;
}

// Use the original working version
String calculateSetPositionCommand(int position) {
    String callStr = "ff78ea41bf03";
    int outHex = round(((position * 10) % 256));
    if(outHex == 256) outHex = 0;
    
    String groupStr;
    if(position < 23.2) groupStr = "00";
    else if(position < 48.8) groupStr = "01";
    else if(position < 74.4) groupStr = "02";
    else groupStr = "03";
    
    char hexVal[3];
    sprintf(hexVal, "%02X", outHex);
    return callStr + String(hexVal) + groupStr;
}

void sendCommandToBlind(const String& command, BlindConnection& blind) {
    if(!blind.isConnected || !blind.client || !blind.client->isConnected()) return;
    
    BLERemoteService* pService = blind.client->getService(SERVICE_UUID);
    if(pService) {
        BLERemoteCharacteristic* pChar = pService->getCharacteristic(WRITE_UUID);
        if(pChar) {
            std::vector<byte> bytes = hexStringToBytes(command);
            pChar->writeValue((const uint8_t*)bytes.data(), bytes.size());
            Serial.printf("Sent command to blind %s: %s\n", blind.address.toString().c_str(), command.c_str());
        }
    }
}

void sendCommandToAllBlinds(const String& command) {
    for(auto& blind : blindConnections) {
        sendCommandToBlind(command, blind);
    }
}

bool connectToBlinds() {
    Serial.println("Starting BLE scan...");
    BLEScan* pScan = BLEDevice::getScan();
    std::vector<BLEAdvertisedDevice> foundBlinds;
    
    class BlindScanCallback: public BLEAdvertisedDeviceCallbacks {
    public:
        std::vector<BLEAdvertisedDevice>* blinds;
        BlindScanCallback(std::vector<BLEAdvertisedDevice>* b) : blinds(b) {}
        
        void onResult(BLEAdvertisedDevice* device) {
            if(device->haveName() && device->getName() == BLIND_NAME) {
                blinds->push_back(*device);
            }
        }
    };
    
    pScan->setAdvertisedDeviceCallbacks(new BlindScanCallback(&foundBlinds));
    pScan->setInterval(100);
    pScan->setWindow(99);
    pScan->start(5, false);
    
    for(auto& device : foundBlinds) {
        for(auto& blind : blindConnections) {
            if(!blind.isConnected) {
                blind.client = BLEDevice::createClient();
                if(!blind.client->connect(device.getAddress())) continue;
                
                BLERemoteService* pService = blind.client->getService(SERVICE_UUID);
                if(!pService) {
                    blind.client->disconnect();
                    continue;
                }
                
                blind.isConnected = true;
                blind.address = device.getAddress();
                blind.name = String(device.getName().c_str());
                
                sendCommandToBlind(KEEP_ALIVE, blind);
                Serial.printf("Connected to blind: %s\n", blind.address.toString().c_str());
                break;
            }
        }
    }
    
    int connectedCount = 0;
    for(const auto& blind : blindConnections) {
        if(blind.isConnected) connectedCount++;
    }
    
    return connectedCount == BLIND_COUNT;
}

class BlindControl : public Service::WindowCovering {
private:
    SpanCharacteristic *current;
    SpanCharacteristic *target;
    SpanCharacteristic *state;

public:
    BlindControl() : Service::WindowCovering() {
        current = new Characteristic::CurrentPosition(0);
        target = new Characteristic::TargetPosition(0);
        state = new Characteristic::PositionState(2);
    }

    boolean update() {
        if(target->getNewVal() != target->getVal()) {
            pendingPosition = target->getNewVal();
            lastPositionChange = millis();
            Serial.printf("Position change requested: %d\n", pendingPosition);
            return true;
        }
        return true;
    }
};

void setup() {
    Serial.begin(115200);
    
    homeSpan.setPairingCode("46637726");
    homeSpan.begin(Category::WindowCoverings, "Blind Controller");
    
    BLEDevice::init("HomeKit Blind Controller");
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN, ESP_PWR_LVL_P9);
    
    new SpanAccessory();
        new Service::AccessoryInformation();
            new Characteristic::Name("Smart Blinds");
            new Characteristic::Manufacturer("ADEMA Group");
            new Characteristic::SerialNumber("123-ABC");
            new Characteristic::Model("Smart Blind Controller");
            new Characteristic::FirmwareRevision("1.0");
            new Characteristic::Identify();
        new BlindControl();
    
    // Initial connection to blinds
    connectToBlinds();
}

void loop() {
    homeSpan.poll();
    
    // Check for disconnected blinds
    static unsigned long lastConnectionCheck = 0;
    if(millis() - lastConnectionCheck > 5000) {  // Check every 5 seconds
        bool needReconnect = false;
        for(auto& blind : blindConnections) {
            if(blind.isConnected && (!blind.client || !blind.client->isConnected())) {
                blind.isConnected = false;
                needReconnect = true;
            }
        }
        if(needReconnect) {
            connectToBlinds();
        }
        lastConnectionCheck = millis();
    }
    
    // Send keepalive
    if(millis() - lastKeepalive > KEEPALIVE_INTERVAL) {
        sendCommandToAllBlinds(KEEP_ALIVE);
        lastKeepalive = millis();
    }
    
    // Handle pending position changes
    if(pendingPosition >= 0 && (millis() - lastPositionChange > DEBOUNCE_DELAY)) {
        String command = calculateSetPositionCommand(pendingPosition);
        sendCommandToAllBlinds(command);
        pendingPosition = -1;
    }
}