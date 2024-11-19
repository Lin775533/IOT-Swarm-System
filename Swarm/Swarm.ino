#include <ESP8266WiFi.h>
#include <WiFiUdp.h>

// WiFi settings
const char* ssid = "YOUR_WIFI";
const char* password = "YOUR_PASSWORD";

// Network settings
WiFiUDP udp;
unsigned int localPort = 2910;      
const int BROADCAST_PORT = 2910;    
IPAddress broadcastIP(192, 168, 1, 255);
IPAddress raspberryPi(192, 168, 1, 81);

// Device settings
const int LIGHT_SENSOR_PIN = A0;    
const int STATUS_LED = 2;           // Built-in LED (GPIO2) for reading indicator
const int MASTER_LED = 16;          // Second LED (GPIO16) for master status

// Protocol settings
const int PACKET_SIZE = 64;
char packetBuffer[PACKET_SIZE];
const unsigned long BROADCAST_INTERVAL = 200;  
const unsigned long MASTER_TIMEOUT = 2000;     
unsigned long lastBroadcastTime = 0;
unsigned long lastReceivedTime = 0;
bool isActive = true;  // Flag to control data collection
unsigned long resetTime = 0;  // Time when reset was received

// Swarm state
int deviceID;                       
int currentReading;                 
bool isMaster = false;             
struct SwarmData {
    int reading;
    unsigned long lastUpdate;
    int deviceID;
};
SwarmData swarmReadings[3];
int numDevices = 0;

void setup() {
    Serial.begin(9600);
    Serial.println("\nStarting LightSwarm Node...");
    
    // Initialize pins
    pinMode(STATUS_LED, OUTPUT);
    pinMode(MASTER_LED, OUTPUT);
    
    // Initialize LEDs
    digitalWrite(STATUS_LED, HIGH);    // Built-in LED is active LOW, so HIGH = OFF
    digitalWrite(MASTER_LED, LOW);     // External LED is active HIGH, so LOW = OFF
    
    randomSeed(analogRead(0));
    deviceID = random(1000, 9999);
    
    setupWiFi();
    udp.begin(localPort);
    
    Serial.printf("\nDevice ID: %d\n", deviceID);
    Serial.printf("Local IP: %s\n", WiFi.localIP().toString().c_str());
}

void loop() {
    if (!isActive) {
        // When not active, only listen for commands
        handleIncomingPackets();
        return;
    }
    
    currentReading = analogRead(LIGHT_SENSOR_PIN);
    
    // Debug print
    static unsigned long lastPrint = 0;
    if (millis() - lastPrint > 1000) {
        Serial.printf("Reading: %d, Master: %s\n", currentReading, isMaster ? "YES" : "NO");
        lastPrint = millis();
    }
    
    handleIncomingPackets();
    
    if (millis() - lastReceivedTime > BROADCAST_INTERVAL) {
        broadcastReading();
    }
    
    updateMasterStatus();
    handleStatusLED();
}

void setupWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected!");
}

void broadcastReading() {
    char message[32];
    snprintf(message, sizeof(message), "LIGHT:%d:%d", deviceID, currentReading);
    
    // Broadcast to network
    udp.beginPacketMulticast(broadcastIP, BROADCAST_PORT, WiFi.localIP());
    udp.write(message);
    udp.endPacket();
    
    // Also send directly to Raspberry Pi
    udp.beginPacket(raspberryPi, BROADCAST_PORT);
    udp.write(message);
    udp.endPacket();
    
    Serial.printf("Broadcast: %s\n", message);
    lastBroadcastTime = millis();
}

void updateMasterStatus() {
    unsigned long now = millis();
    bool shouldBeMaster = true;
    int highestReading = currentReading;
    int activeDevices = 0;
    
    // Count active devices and find highest reading
    for (int i = 0; i < numDevices; i++) {
        if (now - swarmReadings[i].lastUpdate < MASTER_TIMEOUT) {
            activeDevices++;
            if (swarmReadings[i].reading > highestReading) {
                shouldBeMaster = false;
                highestReading = swarmReadings[i].reading;
            }
            else if (swarmReadings[i].reading == highestReading && 
                     swarmReadings[i].deviceID < deviceID) {
                shouldBeMaster = false;
            }
        }
    }
    
    // If no other active devices, we should be master
    if (activeDevices == 0) {
        shouldBeMaster = true;
    }
    
    // Update master status if changed
    if (shouldBeMaster != isMaster) {
        isMaster = shouldBeMaster;
        
        // FIXED LED LOGIC: LOW means OFF, HIGH means ON
        if (isMaster) {
            digitalWrite(MASTER_LED, LOW);  // Turn ON LED when master
            Serial.printf("\nBecome Master! Reading: %d, Active devices: %d\n", 
                         currentReading, activeDevices);
            sendToRaspberryPi();
        } else {
            digitalWrite(MASTER_LED, HIGH);   // Turn OFF LED when not master
            Serial.printf("\nLost Master status. Highest reading: %d\n", highestReading);
        }
    }
    
    // If master, keep sending updates to RPi
    if (isMaster) {
        static unsigned long lastMasterUpdate = 0;
        if (millis() - lastMasterUpdate > 1000) {
            sendToRaspberryPi();
            lastMasterUpdate = millis();
        }
    }
    
    // Debug output
    static unsigned long lastDebug = 0;
    if (millis() - lastDebug > 2000) {
        Serial.printf("\n--- Status Update ---\n");
        Serial.printf("My Reading: %d\n", currentReading);
        Serial.printf("Active Devices: %d\n", activeDevices);
        Serial.printf("Master Status: %s\n", isMaster ? "YES" : "NO");
        Serial.printf("Highest Reading: %d\n", highestReading);
        if (isMaster) {
            Serial.println("MASTER LED should be ON");
        } else {
            Serial.println("MASTER LED should be OFF");
        }
        lastDebug = millis();
    }
}

void updateSwarmData(int remoteID, int reading) {
    if (remoteID == deviceID) return;  // Ignore our own readings
    
    bool found = false;
    for (int i = 0; i < numDevices; i++) {
        if (swarmReadings[i].deviceID == remoteID) {
            swarmReadings[i].reading = reading;
            swarmReadings[i].lastUpdate = millis();
            found = true;
            break;
        }
    }
    
    if (!found && numDevices < 3) {
        swarmReadings[numDevices].deviceID = remoteID;
        swarmReadings[numDevices].reading = reading;
        swarmReadings[numDevices].lastUpdate = millis();
        numDevices++;
    }
}

void handleIncomingPackets() {
    int packetSize = udp.parsePacket();
    
    if (packetSize) {
        udp.read(packetBuffer, PACKET_SIZE);
        packetBuffer[packetSize] = 0;
        lastReceivedTime = millis();
        
        if (strncmp(packetBuffer, "LIGHT:", 6) == 0 && isActive) {  // Only process if active
            int remoteID, remoteReading;
            if (sscanf(packetBuffer, "LIGHT:%d:%d", &remoteID, &remoteReading) == 2) {
                if (remoteID != deviceID) {
                    updateSwarmData(remoteID, remoteReading);
                    Serial.printf("Received from %d: %d\n", remoteID, remoteReading);
                }
            }
        } else if (strncmp(packetBuffer, "RESET", 5) == 0) {
            handleResetCommand();
        } else if (strncmp(packetBuffer, "ACTIVATE", 8) == 0) {
            handleActivateCommand();
        }
    }
}

void handleResetCommand() {
    Serial.println("Reset command received!");
    isActive = false;  // Stop data collection
    resetTime = millis();
    
    // Clear all data
    numDevices = 0;
    isMaster = false;
    
    // Turn off all LEDs
    digitalWrite(STATUS_LED, HIGH);  // Built-in LED is active LOW
    digitalWrite(MASTER_LED, LOW);   // External LED is active HIGH
    
    Serial.println("System reset and waiting for activation");
}

void handleActivateCommand() {
    Serial.println("Activate command received!");
    isActive = true;
    resetTime = 0;
    Serial.println("System activated, resuming data collection");
}

void handleStatusLED() {
    // This LED flashes based on light sensor reading
    static unsigned long lastToggle = 0;
    int flashDelay = map(currentReading, 0, 1023, 1000, 100);  // Higher reading = faster flash
    
    if (millis() - lastToggle >= flashDelay) {
        digitalWrite(STATUS_LED, !digitalRead(STATUS_LED));  // Toggle LED
        lastToggle = millis();
    }
}

void sendToRaspberryPi() {
    if (!isMaster) return;  // Safety check
    
    char message[32];
    snprintf(message, sizeof(message), "MASTER:%d:%d", deviceID, currentReading);
    
    // Send to RPi
    udp.beginPacket(raspberryPi, BROADCAST_PORT);
    udp.write(message);
    udp.endPacket();
    
    Serial.printf("Sent to RPi: %s\n", message);
}
