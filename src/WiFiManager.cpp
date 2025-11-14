#include "WiFiManager.h"
#include "IrrigationController.h"
#include "HomeAssistantIntegration.h"

WiFiManager::WiFiManager()
    : _timeSynced(false),
      _configMode(false),
      _lastReconnectAttempt(0),
      _lastTimeSync(0),
      _lastUpdateCheck(0),
      _reconnectRetries(0),
      _ntpUDP(nullptr),
      _ntpClient(nullptr),
      _webServer(nullptr),
      _dnsServer(nullptr),
      _timeUpdateCallback(nullptr),
      _controller(nullptr),
      _homeAssistant(nullptr) {
}

WiFiManager::~WiFiManager() {
    if (_ntpClient) delete _ntpClient;
    if (_ntpUDP) delete _ntpUDP;
    if (_webServer) delete _webServer;
    if (_dnsServer) delete _dnsServer;
}

bool WiFiManager::begin(const char* ssid, const char* password) {
    DEBUG_PRINTLN("WiFiManager: Initializing...");

    // Initialize SPIFFS if not already initialized
    if (!SPIFFS.begin(true)) {
        DEBUG_PRINTLN("WiFiManager: Failed to initialize SPIFFS");
    }

    // Try to load saved credentials first
    bool hasCredentials = false;
    if (ssid != nullptr && password != nullptr) {
        // Use provided credentials
        _ssid = String(ssid);
        _password = String(password);
        hasCredentials = true;
        DEBUG_PRINTLN("WiFiManager: Using provided credentials");
    } else {
        // Try to load from SPIFFS
        hasCredentials = loadCredentials();
    }

    if (hasCredentials && _ssid.length() > 0) {
        // Set hostname
        WiFi.setHostname(WIFI_HOSTNAME);

        // Try to connect
        DEBUG_PRINTLN("WiFiManager: Attempting to connect with saved credentials...");
        connectWiFi();

        // Check if connected
        if (isConnected()) {
            DEBUG_PRINTLN("WiFiManager: Connected successfully!");

            // Setup OTA
            setupOTA();

            // Initialize NTP client
            _ntpUDP = new WiFiUDP();
            _ntpClient = new NTPClient(*_ntpUDP, NTP_SERVER,
                                       TIMEZONE_OFFSET * 3600 + DAYLIGHT_OFFSET,
                                       NTP_UPDATE_INTERVAL);
            _ntpClient->begin();

            // Start web server for status page
            startWebServer();

            // Check for firmware updates on startup
            DEBUG_PRINTLN("WiFiManager: Checking for firmware updates on startup...");
            checkForUpdates();

            DEBUG_PRINTLN("WiFiManager: Initialized");
            return true;
        } else {
            DEBUG_PRINTLN("WiFiManager: Failed to connect, starting config portal...");
        }
    } else {
        DEBUG_PRINTLN("WiFiManager: No credentials found, starting config portal...");
    }

    // Start config portal if no credentials or connection failed
    startConfigPortal(WIFI_AP_NAME);
    return false;
}

bool WiFiManager::loadCredentials() {
    if (!SPIFFS.exists(WIFI_CREDENTIALS_FILE)) {
        DEBUG_PRINTLN("WiFiManager: No credentials file found");
        return false;
    }

    File file = SPIFFS.open(WIFI_CREDENTIALS_FILE, "r");
    if (!file) {
        DEBUG_PRINTLN("WiFiManager: Failed to open credentials file");
        return false;
    }

    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        DEBUG_PRINTF("WiFiManager: Failed to parse credentials: %s\n", error.c_str());
        return false;
    }

    _ssid = doc["ssid"] | "";
    _password = doc["password"] | "";

    if (_ssid.length() > 0) {
        DEBUG_PRINTF("WiFiManager: Loaded credentials for SSID: %s\n", _ssid.c_str());
        return true;
    }

    return false;
}

bool WiFiManager::saveCredentials(const String& ssid, const String& password) {
    StaticJsonDocument<256> doc;
    doc["ssid"] = ssid;
    doc["password"] = password;

    File file = SPIFFS.open(WIFI_CREDENTIALS_FILE, "w");
    if (!file) {
        DEBUG_PRINTLN("WiFiManager: Failed to open credentials file for writing");
        return false;
    }

    if (serializeJson(doc, file) == 0) {
        DEBUG_PRINTLN("WiFiManager: Failed to write credentials");
        file.close();
        return false;
    }

    file.close();
    DEBUG_PRINTLN("WiFiManager: Credentials saved successfully");
    return true;
}

void WiFiManager::connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) {
        return;
    }

    DEBUG_PRINT("WiFiManager: Connecting to ");
    DEBUG_PRINTLN(_ssid.c_str());

    WiFi.mode(WIFI_STA);
    WiFi.begin(_ssid.c_str(), _password.c_str());

    // Wait for connection (non-blocking with timeout)
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        DEBUG_PRINT(".");
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        DEBUG_PRINTLN();
        DEBUG_PRINT("WiFiManager: Connected! IP: ");
        DEBUG_PRINTLN(WiFi.localIP().toString());
        _reconnectRetries = 0;

        // Sync time after connecting
        syncTime();
    } else {
        DEBUG_PRINTLN();
        DEBUG_PRINTLN("WiFiManager: Connection failed");
    }
}

void WiFiManager::startConfigPortal(const char* apName) {
    DEBUG_PRINTLN("WiFiManager: Starting configuration portal...");

    _configMode = true;
    _apName = String(apName);

    // Stop any existing connection
    WiFi.disconnect();

    // Start Access Point
    WiFi.mode(WIFI_AP);

    if (strlen(WIFI_AP_PASSWORD) > 0) {
        WiFi.softAP(apName, WIFI_AP_PASSWORD);
    } else {
        WiFi.softAP(apName);
    }

    IPAddress IP = WiFi.softAPIP();
    DEBUG_PRINT("WiFiManager: AP IP address: ");
    DEBUG_PRINTLN(IP.toString());
    DEBUG_PRINTF("WiFiManager: Connect to '%s' and navigate to http://", apName);
    DEBUG_PRINTLN(IP.toString());

    // Setup DNS server for captive portal
    setupDNS();

    // Setup web server
    setupWebServer();
}

void WiFiManager::stopConfigPortal() {
    DEBUG_PRINTLN("WiFiManager: Stopping configuration portal...");

    _configMode = false;

    if (_dnsServer) {
        _dnsServer->stop();
        delete _dnsServer;
        _dnsServer = nullptr;
    }

    if (_webServer) {
        _webServer->stop();
        delete _webServer;
        _webServer = nullptr;
    }

    WiFi.softAPdisconnect(true);
}

void WiFiManager::setupDNS() {
    if (_dnsServer) {
        delete _dnsServer;
    }

    _dnsServer = new DNSServer();
    _dnsServer->start(DNS_PORT, "*", WiFi.softAPIP());
    DEBUG_PRINTLN("WiFiManager: DNS server started");
}

void WiFiManager::setupWebServer() {
    if (_webServer) {
        delete _webServer;
    }

    _webServer = new WebServer(80);

    // Serve the config page
    _webServer->on("/", HTTP_GET, [this]() {
        _webServer->send(200, "text/html", getConfigPage());
    });

    // Handle network scan
    _webServer->on("/scan", HTTP_GET, [this]() {
        String json = scanNetworks();
        _webServer->send(200, "application/json", json);
    });

    // Handle credential save
    _webServer->on("/save", HTTP_POST, [this]() {
        String ssid = "";
        String password = "";

        if (_webServer->hasArg("ssid")) {
            ssid = _webServer->arg("ssid");
        }
        if (_webServer->hasArg("password")) {
            password = _webServer->arg("password");
        }

        if (ssid.length() > 0) {
            DEBUG_PRINTF("WiFiManager: Received credentials for SSID: %s\n", ssid.c_str());

            // Save credentials
            if (saveCredentials(ssid, password)) {
                _ssid = ssid;
                _password = password;

                // Try to connect to get the IP address
                String ipMessage = "";
                WiFi.mode(WIFI_STA);
                WiFi.begin(_ssid.c_str(), _password.c_str());

                int attempts = 0;
                while (WiFi.status() != WL_CONNECTED && attempts < 20) {
                    delay(500);
                    attempts++;
                }

                if (WiFi.status() == WL_CONNECTED) {
                    ipMessage = "<p><strong>IP Address: " + WiFi.localIP().toString() + "</strong></p>";
                    ipMessage += "<p>You can also access via: http://" + String(WIFI_HOSTNAME) + ".local</p>";
                } else {
                    ipMessage = "<p>Connecting to network... Check serial monitor for IP address.</p>";
                }

                _webServer->send(200, "text/html",
                    "<html><head><style>"
                    "body{font-family:Arial;text-align:center;background:#667eea;color:#fff;padding:50px;}"
                    "h1{font-size:2em;margin-bottom:20px;}"
                    "p{font-size:1.2em;margin:10px;}"
                    ".info{background:rgba(255,255,255,0.2);padding:20px;border-radius:10px;margin:20px auto;max-width:400px;}"
                    "</style></head>"
                    "<body><h1>✓ Success!</h1>"
                    "<div class='info'>" +
                    ipMessage +
                    "</div>"
                    "<p>Device will restart in 5 seconds...</p>"
                    "</body></html>");

                // Delay then restart to apply new settings
                delay(5000);
                ESP.restart();
            } else {
                _webServer->send(500, "text/html",
                    "<html><body><h1>Error</h1>"
                    "<p>Failed to save credentials. Please try again.</p>"
                    "<a href='/'>Go Back</a></body></html>");
            }
        } else {
            _webServer->send(400, "text/html",
                "<html><body><h1>Error</h1>"
                "<p>SSID cannot be empty.</p>"
                "<a href='/'>Go Back</a></body></html>");
        }
    });

    // Captive portal redirect
    _webServer->onNotFound([this]() {
        _webServer->sendHeader("Location", "/", true);
        _webServer->send(302, "text/plain", "");
    });

    _webServer->begin();
    DEBUG_PRINTLN("WiFiManager: Web server started");
}

String WiFiManager::getConfigPage() {
    String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>WiFi Configuration</title>
    <style>
        body {
            font-family: Arial, sans-serif;
            margin: 0;
            padding: 20px;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
        }
        .container {
            max-width: 500px;
            margin: 0 auto;
            background: white;
            padding: 30px;
            border-radius: 10px;
            box-shadow: 0 10px 40px rgba(0,0,0,0.2);
        }
        h1 {
            color: #333;
            text-align: center;
            margin-bottom: 30px;
        }
        .form-group {
            margin-bottom: 20px;
        }
        label {
            display: block;
            margin-bottom: 8px;
            color: #555;
            font-weight: bold;
        }
        input, select {
            width: 100%;
            padding: 12px;
            border: 2px solid #ddd;
            border-radius: 5px;
            box-sizing: border-box;
            font-size: 16px;
        }
        input:focus, select:focus {
            outline: none;
            border-color: #667eea;
        }
        button {
            width: 100%;
            padding: 14px;
            background: #667eea;
            color: white;
            border: none;
            border-radius: 5px;
            font-size: 16px;
            cursor: pointer;
            font-weight: bold;
            transition: background 0.3s;
        }
        button:hover {
            background: #5568d3;
        }
        button:disabled {
            background: #ccc;
            cursor: not-allowed;
        }
        .scan-btn {
            background: #48bb78;
            margin-bottom: 10px;
        }
        .scan-btn:hover {
            background: #38a169;
        }
        .loading {
            text-align: center;
            color: #666;
            display: none;
        }
        .network-list {
            max-height: 200px;
            overflow-y: auto;
            border: 2px solid #ddd;
            border-radius: 5px;
            margin-bottom: 10px;
        }
        .network-item {
            padding: 12px;
            border-bottom: 1px solid #eee;
            cursor: pointer;
            transition: background 0.2s;
        }
        .network-item:hover {
            background: #f7fafc;
        }
        .network-item:last-child {
            border-bottom: none;
        }
        .signal {
            float: right;
            color: #48bb78;
        }
        .info {
            background: #e6fffa;
            padding: 15px;
            border-radius: 5px;
            margin-bottom: 20px;
            border-left: 4px solid #48bb78;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>Irrigation WiFi Setup</h1>
        <div class="info">
            <strong>Welcome!</strong><br>
            Configure your WiFi connection to get started.
        </div>

        <button class="scan-btn" onclick="scanNetworks()">Scan for Networks</button>
        <div id="networks"></div>
        <div class="loading" id="loading">Scanning networks...</div>

        <form action="/save" method="post" id="wifiForm">
            <div class="form-group">
                <label for="ssid">WiFi Network (SSID):</label>
                <input type="text" id="ssid" name="ssid" required placeholder="Enter SSID or scan above">
            </div>

            <div class="form-group">
                <label for="password">Password:</label>
                <input type="password" id="password" name="password" placeholder="Leave empty for open networks">
            </div>

            <button type="submit">Save & Connect</button>
        </form>
    </div>

    <script>
        function scanNetworks() {
            document.getElementById('loading').style.display = 'block';
            document.getElementById('networks').innerHTML = '';

            fetch('/scan')
                .then(response => response.json())
                .then(data => {
                    document.getElementById('loading').style.display = 'none';

                    if (data.networks && data.networks.length > 0) {
                        let html = '<div class="network-list">';
                        data.networks.forEach(network => {
                            let security = network.encryption ? '[Secure]' : '[Open]';
                            html += `<div class="network-item" onclick="selectNetwork('${network.ssid}')">
                                ${security} ${network.ssid}
                                <span class="signal">${network.rssi}dBm</span>
                            </div>`;
                        });
                        html += '</div>';
                        document.getElementById('networks').innerHTML = html;
                    } else {
                        document.getElementById('networks').innerHTML = '<p>No networks found. Try scanning again.</p>';
                    }
                })
                .catch(error => {
                    document.getElementById('loading').style.display = 'none';
                    document.getElementById('networks').innerHTML = '<p>Error scanning networks.</p>';
                });
        }

        function selectNetwork(ssid) {
            document.getElementById('ssid').value = ssid;
            document.getElementById('password').focus();
        }

        // Auto-scan on page load
        window.onload = function() {
            scanNetworks();
        };
    </script>
</body>
</html>
)rawliteral";

    return page;
}

String WiFiManager::scanNetworks() {
    DEBUG_PRINTLN("WiFiManager: Scanning networks...");

    int n = WiFi.scanNetworks();

    DynamicJsonDocument doc(2048);
    JsonArray networks = doc.createNestedArray("networks");

    for (int i = 0; i < n; i++) {
        JsonObject network = networks.createNestedObject();
        network["ssid"] = WiFi.SSID(i);
        network["rssi"] = WiFi.RSSI(i);
        network["encryption"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }

    String json;
    serializeJson(doc, json);

    WiFi.scanDelete();

    DEBUG_PRINTF("WiFiManager: Found %d networks\n", n);
    return json;
}

void WiFiManager::setupOTA() {
    DEBUG_PRINTLN("WiFiManager: Setting up OTA...");

    // Port defaults to 3232
    ArduinoOTA.setPort(3232);

    // Hostname
    ArduinoOTA.setHostname(WIFI_HOSTNAME);

    // Password
    ArduinoOTA.setPassword(OTA_PASSWORD);

    // OTA callbacks
    ArduinoOTA.onStart([]() {
        String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
        DEBUG_PRINTLN("OTA: Start updating " + type);
    });

    ArduinoOTA.onEnd([]() {
        DEBUG_PRINTLN("\nOTA: Update complete");
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        DEBUG_PRINTF("OTA Progress: %u%%\r", (progress / (total / 100)));
    });

    ArduinoOTA.onError([](ota_error_t error) {
        DEBUG_PRINTF("OTA Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) DEBUG_PRINTLN("Auth Failed");
        else if (error == OTA_BEGIN_ERROR) DEBUG_PRINTLN("Begin Failed");
        else if (error == OTA_CONNECT_ERROR) DEBUG_PRINTLN("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR) DEBUG_PRINTLN("Receive Failed");
        else if (error == OTA_END_ERROR) DEBUG_PRINTLN("End Failed");
    });

    ArduinoOTA.begin();
    DEBUG_PRINTLN("WiFiManager: OTA ready");
}

void WiFiManager::update() {
    unsigned long currentMillis = millis();

    // Handle DNS server and web server in config mode
    if (_configMode) {
        if (_dnsServer) {
            _dnsServer->processNextRequest();
        }
        if (_webServer) {
            _webServer->handleClient();
        }
        return; // Don't do anything else in config mode
    }

    // Handle web server when connected
    if (_webServer && isConnected()) {
        _webServer->handleClient();
    }

    // Handle WiFi reconnection
    if (WiFi.status() != WL_CONNECTED) {
        if (currentMillis - _lastReconnectAttempt >= WIFI_RECONNECT_INTERVAL) {
            _lastReconnectAttempt = currentMillis;

            if (_reconnectRetries < WIFI_MAX_RETRIES) {
                DEBUG_PRINTLN("WiFiManager: Attempting to reconnect...");
                connectWiFi();
                _reconnectRetries++;
            } else {
                DEBUG_PRINTLN("WiFiManager: Max reconnection attempts reached");
                // Could optionally restart config portal here
                // if (_reconnectRetries == WIFI_MAX_RETRIES) {
                //     startConfigPortal(WIFI_AP_NAME);
                // }

                // Reset counter after a longer wait
                if (_reconnectRetries >= WIFI_MAX_RETRIES + 10) {
                    _reconnectRetries = 0;
                }
                _reconnectRetries++;
            }
        }
    } else {
        _reconnectRetries = 0;

        // Handle OTA updates
        ArduinoOTA.handle();

        // Sync time periodically (or immediately if not synced yet)
        unsigned long syncInterval = _timeSynced ? NTP_UPDATE_INTERVAL : 30000; // 30 sec if not synced
        if (currentMillis - _lastTimeSync >= syncInterval) {
            syncTime();
        }

        // Check for firmware updates periodically
        if (currentMillis - _lastUpdateCheck >= OTA_CHECK_INTERVAL) {
            _lastUpdateCheck = currentMillis;
            checkForUpdates();
        }
    }
}

void WiFiManager::syncTime() {
    if (!isConnected()) {
        return;
    }

    DEBUG_PRINTLN("WiFiManager: Syncing time with NTP...");

    if (!_ntpClient) {
        DEBUG_PRINTLN("WiFiManager: NTP client not initialized");
        return;
    }

    // Force update with retries
    bool success = false;
    for (int i = 0; i < 3; i++) {
        if (_ntpClient->forceUpdate()) {
            success = true;
            break;
        }
        DEBUG_PRINTF("WiFiManager: NTP retry %d/3\n", i + 1);
        delay(1000);
    }

    if (success) {
        time_t currentTime = _ntpClient->getEpochTime();
        _timeSynced = true;
        _lastTimeSync = millis();

        DEBUG_PRINTF("WiFiManager: Time synced successfully: %lu\n", currentTime);

        // Print formatted time for debugging
        struct tm timeinfo;
        localtime_r(&currentTime, &timeinfo);
        DEBUG_PRINTF("WiFiManager: Current time: %02d:%02d:%02d\n",
                     timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

        // Call callback if set
        if (_timeUpdateCallback) {
            _timeUpdateCallback(currentTime);
        }

        // Update system time
        struct timeval tv;
        tv.tv_sec = currentTime;
        tv.tv_usec = 0;
        settimeofday(&tv, nullptr);
    } else {
        DEBUG_PRINTLN("WiFiManager: Time sync failed after retries");
    }
}

time_t WiFiManager::getCurrentTime() {
    if (_timeSynced && _ntpClient) {
        return _ntpClient->getEpochTime();
    }
    return 0;
}

void WiFiManager::checkForUpdates() {
    if (!isConnected()) {
        DEBUG_PRINTLN("WiFiManager: Not connected, skipping update check");
        return;
    }

    DEBUG_PRINTLN("WiFiManager: Checking for firmware updates...");

    String latestVersion;
    if (checkGitHubVersion(latestVersion)) {
        DEBUG_PRINTF("WiFiManager: Latest version: %s, Current: %s\n",
                     latestVersion.c_str(), VERSION);

        if (latestVersion != String(VERSION)) {
            DEBUG_PRINTLN("WiFiManager: New version available! Starting update...");
            performOTA();
        } else {
            DEBUG_PRINTLN("WiFiManager: Firmware is up to date");
        }
    } else {
        DEBUG_PRINTLN("WiFiManager: Failed to check for updates");
    }
}

bool WiFiManager::checkGitHubVersion(String& latestVersion) {
    HTTPClient http;

    // Build GitHub URL for version file
    String url = "https://raw.githubusercontent.com/";
    url += GITHUB_REPO_OWNER;
    url += "/";
    url += GITHUB_REPO_NAME;
    url += "/main/";
    url += GITHUB_VERSION_PATH;

    DEBUG_PRINTF("WiFiManager: Checking version at: %s\n", url.c_str());

    http.begin(url);
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
        latestVersion = http.getString();
        latestVersion.trim();
        http.end();
        return true;
    } else {
        DEBUG_PRINTF("WiFiManager: HTTP GET failed, error: %s\n",
                     http.errorToString(httpCode).c_str());
        http.end();
        return false;
    }
}

void WiFiManager::performOTA() {
    if (!isConnected()) {
        DEBUG_PRINTLN("WiFiManager: Not connected, cannot perform OTA");
        return;
    }

    // Build GitHub URL for firmware binary
    String url = "https://raw.githubusercontent.com/";
    url += GITHUB_REPO_OWNER;
    url += "/";
    url += GITHUB_REPO_NAME;
    url += "/main/";
    url += GITHUB_FIRMWARE_PATH;

    DEBUG_PRINTF("WiFiManager: Downloading firmware from: %s\n", url.c_str());

    if (downloadFirmware(url)) {
        DEBUG_PRINTLN("WiFiManager: Firmware updated successfully! Rebooting...");
        delay(1000);
        ESP.restart();
    } else {
        DEBUG_PRINTLN("WiFiManager: Firmware update failed");
    }
}

bool WiFiManager::downloadFirmware(const String& url) {
    HTTPClient http;
    http.begin(url);

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        DEBUG_PRINTF("WiFiManager: Download failed, error: %s\n",
                     http.errorToString(httpCode).c_str());
        http.end();
        return false;
    }

    int contentLength = http.getSize();
    if (contentLength <= 0) {
        DEBUG_PRINTLN("WiFiManager: Invalid content length");
        http.end();
        return false;
    }

    bool canBegin = Update.begin(contentLength);
    if (!canBegin) {
        DEBUG_PRINTLN("WiFiManager: Not enough space for OTA");
        http.end();
        return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    size_t written = 0;
    uint8_t buff[128];

    DEBUG_PRINTF("WiFiManager: Starting update, size: %d bytes\n", contentLength);

    while (http.connected() && (written < contentLength)) {
        size_t available = stream->available();
        if (available) {
            int bytesRead = stream->readBytes(buff, min(available, sizeof(buff)));
            written += Update.write(buff, bytesRead);

            // Print progress
            if (contentLength > 0) {
                int progress = (written * 100) / contentLength;
                DEBUG_PRINTF("WiFiManager: Update progress: %d%%\r", progress);
            }
        }
        delay(1);
    }

    DEBUG_PRINTLN();

    if (Update.end()) {
        if (Update.isFinished()) {
            DEBUG_PRINTLN("WiFiManager: Update successfully completed");
            http.end();
            return true;
        } else {
            DEBUG_PRINTLN("WiFiManager: Update not finished");
        }
    } else {
        DEBUG_PRINTF("WiFiManager: Update error: %s\n", Update.errorString());
    }

    http.end();
    return false;
}

String WiFiManager::getStatusPage() {
    if (!_controller) {
        return "<html><body><h1>Controller not initialized</h1></body></html>";
    }

    SystemStatus status = _controller->getStatus();
    
    String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <meta http-equiv="refresh" content="5">
    <title>Irrigation Status</title>
    <style>
        body {
            font-family: 'Courier New', monospace;
            margin: 0;
            padding: 20px;
            background: #1a1a1a;
            color: #00ff00;
        }
        .lcd-container {
            max-width: 600px;
            margin: 0 auto;
            background: #2a4a2a;
            border: 3px solid #00ff00;
            border-radius: 10px;
            padding: 20px;
            box-shadow: 0 0 20px rgba(0,255,0,0.3);
        }
        .lcd-screen {
            background: #1a3a1a;
            padding: 15px;
            border-radius: 5px;
            font-size: 18px;
            line-height: 1.8;
            letter-spacing: 2px;
        }
        .lcd-line {
            margin: 5px 0;
            min-height: 25px;
        }
        h1 {
            text-align: center;
            color: #00ff00;
            text-shadow: 0 0 10px rgba(0,255,0,0.5);
        }
        .status-ok { color: #00ff00; }
        .status-warn { color: #ffaa00; }
        .status-error { color: #ff0000; }
        .info {
            margin-top: 20px;
            padding: 15px;
            background: rgba(0,255,0,0.1);
            border-radius: 5px;
            font-size: 14px;
        }
    </style>
</head>
<body>
    <div class="lcd-container">
        <h1>IRRIGATION CONTROLLER</h1>
        <div class="lcd-screen">)rawliteral";

    // Line 1: WiFi and MQTT status
    page += "<div class='lcd-line'>";
    page += "WiFi:";
    page += status.wifiConnected ? "<span class='status-ok'>OK </span>" : "<span class='status-error'>-- </span>";
    page += " MQTT:";
    page += status.mqttConnected ? "<span class='status-ok'>OK</span>" : "<span class='status-warn'>--</span>";
    page += "</div>";

    // Line 2: Irrigation status
    page += "<div class='lcd-line'>";
    if (status.irrigating) {
        page += "<span class='status-ok'>IRRIGATING</span> ";
        if (status.manualMode) {
            page += "(MAN)";
        } else {
            page += "(SCH)";
        }
    } else {
        page += "IDLE            ";
    }
    page += "</div>";

    // Line 3: Time remaining or last run
    page += "<div class='lcd-line'>";
    if (status.irrigating) {
        unsigned long remaining = _controller->getTimeRemaining();
        unsigned long minutes = remaining / 60000;
        unsigned long seconds = (remaining % 60000) / 1000;
        char buf[20];
        sprintf(buf, "Remaining: %02lu:%02lu", minutes, seconds);
        page += buf;
    } else if (status.lastIrrigationTime > 0) {
        struct tm timeinfo;
        time_t t = status.lastIrrigationTime;
        localtime_r(&t, &timeinfo);
        char buf[20];
        sprintf(buf, "Last: %02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
        page += buf;
    } else {
        page += "No recent run";
    }
    page += "</div>";

    // Line 4: Next scheduled time
    page += "<div class='lcd-line'>";
    time_t nextTime = _controller->getNextScheduledTime();
    if (nextTime > 0) {
        struct tm timeinfo;
        localtime_r(&nextTime, &timeinfo);
        char buf[20];
        sprintf(buf, "Next: %02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
        page += buf;
    } else {
        page += "No schedules";
    }
    page += "</div>";

    page += R"rawliteral(
        </div>
        <div class="info">
            <strong>Network:</strong> )rawliteral";
    page += WiFi.SSID();
    page += "<br><strong>IP:</strong> ";
    page += WiFi.localIP().toString();
    page += "<br><strong>Hostname:</strong> ";
    page += WIFI_HOSTNAME;
    page += ".local<br><strong>RSSI:</strong> ";
    page += String(WiFi.RSSI());
    page += " dBm<br><strong>Current Time:</strong> ";

    // Add current time
    if (_timeSynced && _controller) {
        time_t currentTime = getCurrentTime();
        if (currentTime > 0) {
            struct tm timeinfo;
            localtime_r(&currentTime, &timeinfo);
            char timeBuf[20];
            sprintf(timeBuf, "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
            page += timeBuf;
        } else {
            page += "Not synced";
        }
    } else {
        page += "Not synced";
    }

    page += "<br><strong>Uptime:</strong> ";

    unsigned long uptime = millis() / 1000;
    unsigned long hours = uptime / 3600;
    unsigned long minutes = (uptime % 3600) / 60;
    unsigned long seconds = uptime % 60;
    char uptimeBuf[30];
    sprintf(uptimeBuf, "%02luh %02lum %02lus", hours, minutes, seconds);
    page += uptimeBuf;

    page += R"rawliteral(
        </div>
)rawliteral";

    // Add MQTT configuration form if not connected
    bool mqttConnected = _homeAssistant && _homeAssistant->isConnected();
    if (!mqttConnected) {
        String mqttBroker = _homeAssistant ? _homeAssistant->getMqttBroker() : "";
        uint16_t mqttPort = _homeAssistant ? _homeAssistant->getMqttPort() : 1883;
        String mqttUser = _homeAssistant ? _homeAssistant->getMqttUser() : "";

        page += R"rawliteral(
        <div class="info" style="background: rgba(255,165,0,0.2); border-left: 4px solid #ff8800;">
            <h2 style="margin-top:0; color:#ff8800;">WARNING: MQTT Not Connected</h2>
            <form id="mqttForm" style="text-align:left;">
                <label style="display:block; margin-top:10px;"><strong>Broker:</strong></label>
                <input type="text" id="broker" name="broker" value=")rawliteral";
        page += mqttBroker;
        page += R"rawliteral(" placeholder="home.hackster.me or 192.168.0.X" style="width:100%; padding:8px; margin-top:5px; border:1px solid #0f0; background:#0a0a0a; color:#0f0; font-family:'Courier New';">

                <label style="display:block; margin-top:10px;"><strong>Port:</strong></label>
                <input type="number" id="port" name="port" value=")rawliteral";
        page += String(mqttPort);
        page += R"rawliteral(" style="width:100%; padding:8px; margin-top:5px; border:1px solid #0f0; background:#0a0a0a; color:#0f0; font-family:'Courier New';">

                <label style="display:block; margin-top:10px;"><strong>Username (optional):</strong></label>
                <input type="text" id="user" name="user" value=")rawliteral";
        page += mqttUser;
        page += R"rawliteral(" placeholder="Leave empty if no auth" style="width:100%; padding:8px; margin-top:5px; border:1px solid #0f0; background:#0a0a0a; color:#0f0; font-family:'Courier New';">

                <label style="display:block; margin-top:10px;"><strong>Password (optional):</strong></label>
                <input type="password" id="password" name="password" placeholder="Leave empty if no auth" style="width:100%; padding:8px; margin-top:5px; border:1px solid #0f0; background:#0a0a0a; color:#0f0; font-family:'Courier New';">

                <div style="margin-top:15px;">
                    <button type="button" onclick="testMqtt()" style="padding:10px 20px; background:#48bb78; color:#fff; border:none; border-radius:5px; cursor:pointer; margin-right:10px;">Test Connection</button>
                    <button type="button" onclick="saveMqtt()" style="padding:10px 20px; background:#667eea; color:#fff; border:none; border-radius:5px; cursor:pointer;">Save & Restart</button>
                </div>
                <div id="mqttMessage" style="margin-top:10px; padding:10px; border-radius:5px; display:none;"></div>
            </form>
        </div>

        <script>
        function testMqtt() {
            var broker = document.getElementById('broker').value;
            var port = document.getElementById('port').value;
            var user = document.getElementById('user').value;
            var password = document.getElementById('password').value;

            var msg = document.getElementById('mqttMessage');
            msg.style.display = 'block';
            msg.style.background = 'rgba(255,255,0,0.2)';
            msg.style.color = '#ff0';
            msg.innerHTML = 'Testing connection...';

            fetch('/mqtt/test', {
                method: 'POST',
                headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                body: 'broker=' + encodeURIComponent(broker) + '&port=' + port + '&user=' + encodeURIComponent(user) + '&password=' + encodeURIComponent(password)
            })
            .then(response => response.json())
            .then(data => {
                if (data.success) {
                    msg.style.background = 'rgba(0,255,0,0.2)';
                    msg.style.color = '#0f0';
                    msg.innerHTML = 'SUCCESS: ' + data.message;
                } else {
                    msg.style.background = 'rgba(255,0,0,0.2)';
                    msg.style.color = '#f00';
                    msg.innerHTML = 'ERROR: ' + data.message;
                }
            })
            .catch(error => {
                msg.style.background = 'rgba(255,0,0,0.2)';
                msg.style.color = '#f00';
                msg.innerHTML = 'ERROR: ' + error;
            });
        }

        function saveMqtt() {
            var broker = document.getElementById('broker').value;
            if (!broker) {
                alert('Broker is required');
                return;
            }

            var port = document.getElementById('port').value;
            var user = document.getElementById('user').value;
            var password = document.getElementById('password').value;

            var msg = document.getElementById('mqttMessage');
            msg.style.display = 'block';
            msg.style.background = 'rgba(255,255,0,0.2)';
            msg.style.color = '#ff0';
            msg.innerHTML = 'Saving...';

            fetch('/mqtt/save', {
                method: 'POST',
                headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                body: 'broker=' + encodeURIComponent(broker) + '&port=' + port + '&user=' + encodeURIComponent(user) + '&password=' + encodeURIComponent(password)
            })
            .then(response => response.json())
            .then(data => {
                if (data.success) {
                    msg.style.background = 'rgba(0,255,0,0.2)';
                    msg.style.color = '#0f0';
                    msg.innerHTML = 'SUCCESS: ' + data.message;
                } else {
                    msg.style.background = 'rgba(255,0,0,0.2)';
                    msg.style.color = '#f00';
                    msg.innerHTML = 'ERROR: ' + data.message;
                }
            })
            .catch(error => {
                msg.style.background = 'rgba(255,0,0,0.2)';
                msg.style.color = '#f00';
                msg.innerHTML = 'ERROR: ' + error;
            });
        }
        </script>
)rawliteral";
    }

    page += R"rawliteral(
        <div class="info" style="background: rgba(0,100,255,0.2); border-left: 4px solid #0066ff; margin-top:20px;">
            <h2 style="margin-top:0; color:#00aaff;">System Controls</h2>
            <div style="display:flex; gap:10px; flex-wrap:wrap;">
                <button type="button" onclick="checkUpdates()" style="flex:1; min-width:120px; padding:12px 20px; background:#48bb78; color:#fff; border:none; border-radius:5px; cursor:pointer; font-weight:bold;">Check for Updates</button>
                <button type="button" onclick="restartDevice()" style="flex:1; min-width:120px; padding:12px 20px; background:#ff8800; color:#fff; border:none; border-radius:5px; cursor:pointer; font-weight:bold;">Restart Device</button>
            </div>
            <div id="systemMessage" style="margin-top:15px; padding:12px; border-radius:5px; display:none;"></div>
        </div>

        <script>
        function checkUpdates() {
            var msg = document.getElementById('systemMessage');
            msg.style.display = 'block';
            msg.style.background = 'rgba(255,255,0,0.2)';
            msg.style.color = '#ff0';
            msg.innerHTML = 'Checking for updates...';

            fetch('/system/check-updates', {
                method: 'POST'
            })
            .then(response => response.json())
            .then(data => {
                if (data.success) {
                    msg.style.background = 'rgba(0,255,0,0.2)';
                    msg.style.color = '#0f0';
                    msg.innerHTML = 'SUCCESS: ' + data.message;
                    if (data.updating) {
                        msg.innerHTML += '<br><strong>Device is updating and will restart automatically...</strong>';
                    }
                } else {
                    msg.style.background = 'rgba(255,0,0,0.2)';
                    msg.style.color = '#f00';
                    msg.innerHTML = 'ERROR: ' + data.message;
                }
            })
            .catch(error => {
                msg.style.background = 'rgba(255,0,0,0.2)';
                msg.style.color = '#f00';
                msg.innerHTML = 'ERROR: ' + error;
            });
        }

        function restartDevice() {
            if (!confirm('Are you sure you want to restart the device?')) {
                return;
            }

            var msg = document.getElementById('systemMessage');
            msg.style.display = 'block';
            msg.style.background = 'rgba(255,255,0,0.2)';
            msg.style.color = '#ff0';
            msg.innerHTML = 'Restarting device...';

            fetch('/system/restart', {
                method: 'POST'
            })
            .then(response => response.json())
            .then(data => {
                msg.style.background = 'rgba(0,255,0,0.2)';
                msg.style.color = '#0f0';
                msg.innerHTML = 'Device is restarting... Page will reload in 10 seconds.';
                setTimeout(function() {
                    location.reload();
                }, 10000);
            })
            .catch(error => {
                msg.style.background = 'rgba(0,255,0,0.2)';
                msg.style.color = '#0f0';
                msg.innerHTML = 'Device is restarting... Page will reload in 10 seconds.';
                setTimeout(function() {
                    location.reload();
                }, 10000);
            });
        }
        </script>
    </div>
</body>
</html>)rawliteral";

    return page;
}

void WiFiManager::startWebServer() {
    if (_webServer) {
        return; // Already running
    }

    _webServer = new WebServer(80);

    // Status page (root)
    _webServer->on("/", HTTP_GET, [this]() {
        _webServer->send(200, "text/html", getStatusPage());
    });

    // MQTT configuration save
    _webServer->on("/mqtt/save", HTTP_POST, [this]() {
        if (!_homeAssistant) {
            _webServer->send(500, "text/plain", "MQTT not initialized");
            return;
        }

        String broker = _webServer->hasArg("broker") ? _webServer->arg("broker") : "";
        uint16_t port = _webServer->hasArg("port") ? _webServer->arg("port").toInt() : 1883;
        String user = _webServer->hasArg("user") ? _webServer->arg("user") : "";
        String password = _webServer->hasArg("password") ? _webServer->arg("password") : "";

        if (broker.length() == 0) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"Broker required\"}");
            return;
        }

        // Save credentials
        if (_homeAssistant->saveCredentials(broker, port, user, password)) {
            _webServer->send(200, "application/json", "{\"success\":true,\"message\":\"Saved. Restarting...\"}");
            delay(2000);
            ESP.restart();
        } else {
            _webServer->send(500, "application/json", "{\"success\":false,\"message\":\"Failed to save\"}");
        }
    });

    // MQTT test connection
    _webServer->on("/mqtt/test", HTTP_POST, [this]() {
        if (!_homeAssistant) {
            _webServer->send(500, "application/json", "{\"success\":false,\"message\":\"MQTT not initialized\"}");
            return;
        }

        String broker = _webServer->hasArg("broker") ? _webServer->arg("broker") : "";
        uint16_t port = _webServer->hasArg("port") ? _webServer->arg("port").toInt() : 1883;
        String user = _webServer->hasArg("user") ? _webServer->arg("user") : "";
        String password = _webServer->hasArg("password") ? _webServer->arg("password") : "";

        if (broker.length() == 0) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"Broker required\"}");
            return;
        }

        // Test connection
        bool success = _homeAssistant->testConnection(broker, port, user, password);
        if (success) {
            _webServer->send(200, "application/json", "{\"success\":true,\"message\":\"Connection successful!\"}");
        } else {
            _webServer->send(200, "application/json", "{\"success\":false,\"message\":\"Connection failed\"}");
        }
    });

    // System restart
    _webServer->on("/system/restart", HTTP_POST, [this]() {
        DEBUG_PRINTLN("WiFiManager: Restart requested via web interface");
        _webServer->send(200, "application/json", "{\"success\":true,\"message\":\"Restarting device...\"}");
        delay(1000);
        ESP.restart();
    });

    // System update check
    _webServer->on("/system/check-updates", HTTP_POST, [this]() {
        DEBUG_PRINTLN("WiFiManager: Update check requested via web interface");

        if (!isConnected()) {
            _webServer->send(200, "application/json", "{\"success\":false,\"message\":\"Not connected to WiFi\"}");
            return;
        }

        String latestVersion;
        if (checkGitHubVersion(latestVersion)) {
            DEBUG_PRINTF("WiFiManager: Latest version: %s, Current: %s\n", latestVersion.c_str(), VERSION);

            if (latestVersion != String(VERSION)) {
                _webServer->send(200, "application/json",
                    "{\"success\":true,\"message\":\"Update found! Version " + latestVersion + " is available. Downloading...\",\"updating\":true}");
                delay(1000);
                performOTA();
            } else {
                _webServer->send(200, "application/json",
                    "{\"success\":true,\"message\":\"Firmware is up to date (v" + String(VERSION) + ")\",\"updating\":false}");
            }
        } else {
            _webServer->send(200, "application/json",
                "{\"success\":false,\"message\":\"Failed to check for updates. Check GitHub repository settings.\"}");
        }
    });

    _webServer->begin();
    DEBUG_PRINTLN("WiFiManager: Status web server started");
}

void WiFiManager::stopWebServer() {
    if (_webServer) {
        _webServer->stop();
        delete _webServer;
        _webServer = nullptr;
    }
}
