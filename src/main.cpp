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

// Global variables for BLE connection
struct BlindConnection {
    BLEClient* client = nullptr;
    boolean isConnected = false;
    int currentPosition = 0;
    String name;
    BLEAddress address;
};

std::vector<BlindConnection> blindConnections(BLIND_COUNT);

// Utility functions for hex conversion
byte hexToByte(const char* hex) {
    byte val = 0;
    for(int i = 0; i < 2; i++) {
        char c = hex[i];
        val <<= 4;
        if(c >= '0' && c <= '9') val |= c - '0';
        else if(c >= 'a' && c <= 'f') val |= c - 'a' + 10;
        else if(c >= 'A' && c <= 'F') val |= c - 'A' + 10;
    }
    return val;
}

std::vector<byte> hexStringToBytes(const String& hexString) {
    std::vector<byte> bytes;
    for(unsigned int i = 0; i < hexString.length(); i += 2) {
        String byteString = hexString.substring(i, i + 2);
        byte b = hexToByte(byteString.c_str());
        bytes.push_back(b);
    }
    return bytes;
}

String byteToHex(byte b) {
    String hex = String(b, HEX);
    if(hex.length() == 1) hex = "0" + hex;
    return hex;
}

// BLE notification callback
void notifyCallback(BLERemoteCharacteristic* pRemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
    String response = "";
    for(int i = 0; i < length; i++) {
        response += byteToHex(pData[i]);
    }
    Serial.printf("Received notification: %s\n", response.c_str());
    
    if(length >= 9 && pData[4] == 0xD1) {
        int position = (pData[7] + (256 * pData[8])) / 10;
        std::string clientAddress = pRemoteCharacteristic->getRemoteService()->getClient()->getPeerAddress().toString();
        
        for(auto& blind : blindConnections) {
            if(blind.client && blind.address.toString().compare(clientAddress) == 0) {
                blind.currentPosition = 100 - position;
                Serial.printf("Current position for blind %s: %d\n", blind.address.toString().c_str(), blind.currentPosition);
                break;
            }
        }
    }
}

// Main blind control class
class BlindControl : public Service::WindowCovering {
private:
    SpanCharacteristic *current;
    SpanCharacteristic *target;
    SpanCharacteristic *state;
    int pendingPosition = -1;
    bool pendingDebounce = false;
    unsigned long lastChange = 0;
    unsigned long lastKeepalive = 0;
    static const unsigned long DEBOUNCE_DELAY = 2000;    // 2 seconds
    static const unsigned long BLE_TIMEOUT = 10000;      // 10 seconds
    static const unsigned long KEEPALIVE_INTERVAL = 30000; // 30 seconds

    String calculateSetPositionCommand(int position) {
        String callStr = "ff78ea41bf03";
        int outHex = round(((position * 10) % 256));
        if(outHex == 256) outHex = 0;
        
        String groupStr;
        if(position < 23.2) groupStr = "00";
        else if(position < 48.8) groupStr = "01";
        else if(position < 74.4) groupStr = "02";
        else groupStr = "03";
        
        String hexVal = byteToHex(outHex);
        return callStr + hexVal + groupStr;
    }

    bool connectToBlind() {
        bool foundAny = false;
        int connectedCount = 0;
        std::vector<BLEAdvertisedDevice> foundBlinds;
        
        for(auto& blind : blindConnections) {
            if(blind.isConnected && blind.client && blind.client->isConnected()) {
                connectedCount++;
                Serial.printf("Found existing connection: %s\n", blind.address.toString().c_str());
            }
        }
        
        if(connectedCount == BLIND_COUNT) {
            Serial.println("All blinds already connected");
            return true;
        }

        Serial.println("Starting BLE scan...");
        BLEScan* pScan = BLEDevice::getScan();
        
        class BlindScanCallback: public BLEAdvertisedDeviceCallbacks {
        public:
            std::vector<BLEAdvertisedDevice>* blinds;
            BlindScanCallback(std::vector<BLEAdvertisedDevice>* b) : blinds(b) {}
            
            void onResult(BLEAdvertisedDevice* device) {
                if(device->haveName() && device->getName() == BLIND_NAME) {
                    blinds->push_back(*device);
                    if(blinds->size() >= BLIND_COUNT) {
                        BLEDevice::getScan()->stop();
                    }
                }
            }
        };
        
        pScan->setAdvertisedDeviceCallbacks(new BlindScanCallback(&foundBlinds));
        pScan->setInterval(100);
        pScan->setWindow(99);
        pScan->clearResults();
        
        esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
        esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN, ESP_PWR_LVL_P9);

        uint32_t scanStart = millis();
        pScan->start(0, false);
        
        while(foundBlinds.size() < BLIND_COUNT && (millis() - scanStart < BLE_TIMEOUT)) {
            delay(10);
        }
        pScan->stop();
        
        Serial.printf("Scan complete. Found %d blinds\n", foundBlinds.size());
        
        for(auto& device : foundBlinds) {
            Serial.printf("Processing blind: %s\n", device.getAddress().toString().c_str());
            
            bool alreadyConnected = false;
            for(auto& blind : blindConnections) {
                if(blind.isConnected && blind.client && blind.client->isConnected() && 
                   blind.address.equals(device.getAddress())) {
                    alreadyConnected = true;
                    break;
                }
            }
            
            if(alreadyConnected) continue;
            
            for(auto& blind : blindConnections) {
                if(!blind.isConnected || !blind.client || !blind.client->isConnected()) {
                    if(blind.client != nullptr) {
                        if(blind.client->isConnected()) blind.client->disconnect();
                        blind.client = nullptr;
                    }
                    
                    blind.client = BLEDevice::createClient();
                    if(!blind.client) continue;
                    
                    Serial.printf("Connecting to %s\n", device.getAddress().toString().c_str());
                    
                    if(!blind.client->connect(device.getAddress())) {
                        Serial.println("Connection failed");
                        blind.client = nullptr;
                        continue;
                    }
                    
                    Serial.println("Connected, setting up services...");
                    
                    BLERemoteService* pService = blind.client->getService(SERVICE_UUID);
                    if(!pService) {
                        blind.client->disconnect();
                        blind.client = nullptr;
                        continue;
                    }
                    
                    BLERemoteCharacteristic* pChar = pService->getCharacteristic(NOTIFY_UUID);
                    if(!pChar) {
                        blind.client->disconnect();
                        blind.client = nullptr;
                        continue;
                    }
                    
                    if(pChar->canNotify()) {
                        pChar->subscribe(true, notifyCallback);
                    }
                    
                    sendCommandToBlind(KEEP_ALIVE, blind);
                    
                    blind.isConnected = true;
                    blind.name = String(BLIND_NAME);
                    blind.address = device.getAddress();
                    connectedCount++;
                    foundAny = true;
                    
                    Serial.printf("Blind %s ready\n", blind.address.toString().c_str());
                    break;
                }
            }
            
            if(connectedCount >= BLIND_COUNT) break;
        }
        
        Serial.printf("Connection process complete. Connected count: %d\n", connectedCount);
        return foundAny;
    }

    void maintainConnections() {
        if (millis() - lastKeepalive >= KEEPALIVE_INTERVAL) {
            for (auto& blind : blindConnections) {
                if (blind.isConnected && blind.client && blind.client->isConnected()) {
                    sendCommandToBlind(KEEP_ALIVE, blind);
                }
            }
            lastKeepalive = millis();
        }
        
        // Check if any connections were lost
        for (auto& blind : blindConnections) {
            if (blind.isConnected && (!blind.client || !blind.client->isConnected())) {
                blind.isConnected = false;
            }
        }
        
        if (!areAllBlindsConnected()) {
            connectToBlind();
        }
    }

    bool areAllBlindsConnected() {
        int count = 0;
        for (const auto& blind : blindConnections) {
            if (blind.isConnected && blind.client && blind.client->isConnected()) count++;
        }
        return count == BLIND_COUNT;
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

    void getPosition() {
        sendCommandToAllBlinds("ff78ea41d10301");
    }

    void stopBlinds() {
        sendCommandToAllBlinds("ff78ea415f0301");
    }

public:
    BlindControl() : Service::WindowCovering() {
        current = new Characteristic::CurrentPosition(0);
        target = new Characteristic::TargetPosition(0);
        state = new Characteristic::PositionState(2);
    }

    boolean update() {
        if(target->getNewVal() != target->getVal()) {
            pendingPosition = target->getNewVal();
            lastChange = millis();
            pendingDebounce = true;
            Serial.printf("Position change requested: %d (debouncing for 2s)\n", pendingPosition);
            return true;
        }
        return true;
    }

    void checkAndHandleMove() {
        maintainConnections();  // Keep connections alive
        
        if (pendingDebounce && (millis() - lastChange >= DEBOUNCE_DELAY)) {
            pendingDebounce = false;
            
            if (areAllBlindsConnected() || connectToBlind()) {
                state->setVal(pendingPosition > current->getVal() ? 1 : 0);
                
                String command = calculateSetPositionCommand(pendingPosition);
                sendCommandToAllBlinds(command);
                
                delay(500);
                
                current->setVal(pendingPosition);
                target->setVal(pendingPosition);
                state->setVal(2);
            }
            
            pendingPosition = -1;
        }
    }
};

// Store a pointer to our BlindControl instance
BlindControl* blindControlInstance = nullptr;

void setup() {
    Serial.begin(115200);
    while(!Serial) delay(100);
    Serial.println("Starting up...");

    homeSpan.setStatusPin(2);
    homeSpan.setControlPin(0);
    homeSpan.setPairingCode("46637726");
    
    Serial.println("Initializing HomeSpan...");
    homeSpan.begin(Category::WindowCoverings, "Blind Controller");
    
    Serial.println("Initializing BLE...");
    
    BLEDevice::init("HomeKit Blind Controller");
    
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN, ESP_PWR_LVL_P9);
    
    Serial.println("BLE initialized successfully");
    Serial.printf("Free heap: %d\n", ESP.getFreeHeap());
    
    new SpanAccessory();
        new Service::AccessoryInformation();
            new Characteristic::Name("Smart Blinds");
            new Characteristic::Manufacturer("ADEMA Group");
            new Characteristic::SerialNumber("123-ABC");
            new Characteristic::Model("Smart Blind Controller");
            new Characteristic::FirmwareRevision("1.0");
            new Characteristic::Identify();
    
    blindControlInstance = new BlindControl();
        
    Serial.println("Setup complete!");
}

void loop() {
    homeSpan.poll();
    
    if (blindControlInstance) {
        blindControlInstance->checkAndHandleMove();
    }
}