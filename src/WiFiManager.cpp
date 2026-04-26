#include "WiFiManager.h"
#include "IrrigationController.h"
#include "HomeAssistantIntegration.h"
#include "NodeManager.h"

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
      _homeAssistant(nullptr),
      _nodeManager(nullptr) {
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

bool WiFiManager::clearCredentials() {
    if (SPIFFS.exists(WIFI_CREDENTIALS_FILE)) {
        if (SPIFFS.remove(WIFI_CREDENTIALS_FILE)) {
            DEBUG_PRINTLN("WiFiManager: WiFi credentials removed successfully");
            return true;
        } else {
            DEBUG_PRINTLN("WiFiManager: Failed to remove WiFi credentials file");
            return false;
        }
    }
    DEBUG_PRINTLN("WiFiManager: No credentials file to remove");
    return true; // Not an error if file doesn't exist
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
                    "<body><h1>Success!</h1>"
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
    WiFiClientSecure client;
    client.setInsecure();  // Skip cert validation for GitHub

    // Build GitHub URL for version file
    String url = "https://raw.githubusercontent.com/";
    url += GITHUB_REPO_OWNER;
    url += "/";
    url += GITHUB_REPO_NAME;
    url += "/main/";
    url += GITHUB_VERSION_PATH;

    DEBUG_PRINTF("WiFiManager: Checking version at: %s\n", url.c_str());

    http.begin(client, url);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
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
    WiFiClientSecure client;
    client.setInsecure();  // Skip cert validation for GitHub

    http.begin(client, url);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(60000);  // 60s timeout for large file

    DEBUG_PRINTF("WiFiManager: Free heap before download: %d\n", ESP.getFreeHeap());

    int httpCode = http.GET();
    DEBUG_PRINTF("WiFiManager: HTTP response code: %d\n", httpCode);

    if (httpCode != HTTP_CODE_OK) {
        DEBUG_PRINTF("WiFiManager: Download failed, HTTP %d: %s\n",
                     httpCode, http.errorToString(httpCode).c_str());
        http.end();
        return false;
    }

    int contentLength = http.getSize();
    DEBUG_PRINTF("WiFiManager: Content-Length: %d\n", contentLength);

    if (contentLength <= 0) {
        DEBUG_PRINTLN("WiFiManager: No Content-Length, cannot determine size");
        http.end();
        return false;
    }

    bool canBegin = Update.begin(contentLength);
    if (!canBegin) {
        DEBUG_PRINTF("WiFiManager: Not enough space for OTA (need %d)\n", contentLength);
        http.end();
        return false;
    }

    // Chunked download with progress reporting
    WiFiClient* stream = http.getStreamPtr();
    DEBUG_PRINTF("WiFiManager: Starting firmware write, size: %d bytes\n", contentLength);

    uint8_t buf[1024];
    size_t written = 0;
    int lastPct = -1;
    unsigned long lastProgressLog = 0;

    while (written < (size_t)contentLength) {
        size_t available = stream->available();
        if (available == 0) {
            // Wait for data with timeout
            unsigned long waitStart = millis();
            while (!stream->available() && (millis() - waitStart < 10000)) {
                delay(10);
            }
            if (!stream->available()) {
                DEBUG_PRINTLN("WiFiManager: Download stalled — timeout");
                break;
            }
            continue;
        }

        size_t toRead = (available > sizeof(buf)) ? sizeof(buf) : available;
        size_t bytesRead = stream->readBytes(buf, toRead);
        if (bytesRead == 0) break;

        size_t bytesWritten = Update.write(buf, bytesRead);
        if (bytesWritten != bytesRead) {
            DEBUG_PRINTF("WiFiManager: Write mismatch: read=%d written=%d\n", bytesRead, bytesWritten);
            break;
        }
        written += bytesWritten;

        int pct = (int)((written * 100) / contentLength);
        if (pct != lastPct && (pct % 5 == 0 || millis() - lastProgressLog > 3000)) {
            DEBUG_PRINTF("WiFiManager: OTA progress: %d%% (%d / %d bytes)\n", pct, written, contentLength);
            lastPct = pct;
            lastProgressLog = millis();
        }
    }

    DEBUG_PRINTF("WiFiManager: Written %d of %d bytes\n", written, contentLength);

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
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
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
        .schedule-section {
            margin-top: 20px;
            padding: 20px;
            background: rgba(0,0,0,0.35);
            border-radius: 10px;
            border: 1px solid rgba(0,255,0,0.2);
        }
        .section-header {
            display: flex;
            align-items: center;
            justify-content: space-between;
            margin-bottom: 15px;
        }
        .section-header h2 {
            margin: 0;
            font-size: 20px;
            color: #00ffcc;
        }
        .add-btn {
            width: 42px;
            height: 42px;
            border-radius: 50%;
            border: none;
            font-size: 26px;
            font-weight: bold;
            background: #48bb78;
            color: #fff;
            cursor: pointer;
            transition: opacity 0.2s ease;
        }
        .add-btn:hover {
            opacity: 0.85;
        }
        .schedule-form {
            background: rgba(0,0,0,0.35);
            border: 1px solid rgba(255,255,255,0.1);
            border-radius: 8px;
            padding: 15px;
            margin-bottom: 15px;
        }
        .schedule-form.hidden {
            display: none;
        }
        .schedule-form label {
            display: block;
            font-weight: bold;
            margin-bottom: 5px;
        }
        .schedule-form input,
        .schedule-form select {
            width: 100%;
            padding: 10px;
            border-radius: 5px;
            border: 1px solid rgba(255,255,255,0.2);
            background: rgba(0,0,0,0.2);
            color: #fff;
            font-size: 16px;
            margin-bottom: 10px;
        }
        .schedule-form input[type="number"]::-webkit-inner-spin-button {
            filter: invert(1);
        }
        .day-picker {
            display: flex;
            gap: 6px;
            margin-bottom: 10px;
        }
        .day-picker label {
            display: inline-block !important;
            margin: 0 !important;
            font-weight: normal !important;
        }
        .day-btn {
            width: 38px;
            height: 38px;
            border-radius: 50%;
            border: 2px solid rgba(255,255,255,0.2);
            background: rgba(0,0,0,0.2);
            color: rgba(255,255,255,0.5);
            font-size: 13px;
            font-weight: bold;
            cursor: pointer;
            transition: all 0.15s;
        }
        .day-btn.active {
            border-color: #48bb78;
            background: rgba(72,187,120,0.25);
            color: #48bb78;
        }
        .day-label {
            font-size: 11px;
            color: rgba(255,255,255,0.4);
            margin-bottom: 8px;
            display: block;
            font-weight: bold;
        }
        .form-actions {
            display: flex;
            gap: 10px;
        }
        .form-actions button {
            flex: 1;
            padding: 10px;
            border-radius: 5px;
            border: none;
            font-weight: bold;
            cursor: pointer;
            background: #48bb78;
            color: #fff;
        }
        .form-actions .secondary {
            background: rgba(255,255,255,0.1);
            color: #fff;
        }
        .schedule-message {
            padding: 10px;
            border-radius: 5px;
            margin-bottom: 15px;
            font-size: 14px;
        }
        .channel-grid {
            display: flex;
            flex-direction: column;
            gap: 12px;
        }
        .channel-card {
            border: 1px solid rgba(0,255,0,0.2);
            border-radius: 8px;
            padding: 12px;
            background: rgba(0,0,0,0.3);
        }
        .channel-card-header {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 8px;
        }
        .channel-card h3 {
            margin: 0;
            font-size: 18px;
            color: #fff;
        }
        .schedule-pill {
            display: flex;
            justify-content: space-between;
            align-items: center;
            padding: 8px 10px;
            margin-bottom: 6px;
            border-radius: 5px;
            background: rgba(0,255,0,0.08);
            font-size: 14px;
        }
        .schedule-pill:last-child {
            margin-bottom: 0;
        }
        .pill-action {
            border: none;
            background: transparent;
            color: #ff6666;
            font-size: 20px;
            cursor: pointer;
            line-height: 1;
        }
        .empty-state {
            padding: 15px;
            text-align: center;
            color: #999;
            border: 1px dashed rgba(255,255,255,0.2);
            border-radius: 6px;
        }
        .manual-control {
            margin-top: 20px;
            padding: 20px;
            background: rgba(0,0,0,0.35);
            border-radius: 10px;
            border: 1px solid rgba(0,255,0,0.2);
        }
        .manual-control h2 {
            margin: 0 0 15px 0;
            font-size: 20px;
            color: #00ffcc;
        }
        .channel-row {
            display: flex;
            justify-content: space-between;
            align-items: center;
            padding: 12px;
            margin-bottom: 10px;
            background: rgba(0,0,0,0.3);
            border-radius: 8px;
            border: 1px solid rgba(0,255,0,0.15);
        }
        .channel-row:last-child {
            margin-bottom: 0;
        }
        .channel-label {
            font-size: 16px;
            color: #fff;
        }
        .channel-label .pin {
            color: #888;
            font-size: 12px;
        }
        .toggle-btns {
            display: flex;
            gap: 8px;
        }
        .toggle-btn {
            padding: 8px 20px;
            border: none;
            border-radius: 5px;
            font-weight: bold;
            cursor: pointer;
            transition: all 0.2s;
        }
        .toggle-btn.on {
            background: #48bb78;
            color: #fff;
        }
        .toggle-btn.on:hover {
            background: #38a169;
        }
        .toggle-btn.off {
            background: #e53e3e;
            color: #fff;
        }
        .toggle-btn.off:hover {
            background: #c53030;
        }
        .toggle-btn.active {
            box-shadow: 0 0 10px currentColor;
        }
        .invert-btn {
            padding: 6px 12px;
            border: 1px solid #888;
            border-radius: 4px;
            background: transparent;
            color: #888;
            font-size: 11px;
            cursor: pointer;
            margin-left: 8px;
        }
        .invert-btn.active {
            border-color: #f6ad55;
            color: #f6ad55;
            background: rgba(246,173,85,0.1);
        }
        .channel-status {
            padding: 4px 10px;
            border-radius: 4px;
            font-size: 12px;
            font-weight: bold;
            margin-left: 10px;
        }
        .channel-status.running {
            background: rgba(72,187,120,0.3);
            color: #48bb78;
        }
        .channel-status.stopped {
            background: rgba(160,160,160,0.2);
            color: #888;
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
    uint8_t nextCh = 0;
    time_t nextTime = _controller->getNextScheduledTime(&nextCh);
    if (nextTime > 0) {
        struct tm timeinfo;
        localtime_r(&nextTime, &timeinfo);
        char buf[32];
        sprintf(buf, "Next: Ch %d @ %02d:%02d", nextCh, timeinfo.tm_hour, timeinfo.tm_min);
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

    page += "<br><strong>Firmware Version:</strong> ";
    page += VERSION;

    page += R"rawliteral(
        </div>

        <div class="manual-control">
            <div class="section-header">
                <h2>Manual Control</h2>
                <button type="button" class="add-btn" onclick="showChannelSetup()">+</button>
            </div>
            <div id="pendingPairContainer"></div>
            <div id="manualChannels"></div>
            <div id="channelSetup" style="display:none;margin-top:10px;padding:10px;background:#2d3748;border-radius:6px;">
                <h3 style="margin:0 0 8px 0;">Enable/Disable Channels</h3>
                <div id="channelSetupList"></div>
            </div>
        </div>

        <div class="schedule-section">
            <div class="section-header">
                <h2>Channel Schedules</h2>
                <button type="button" class="add-btn" id="addScheduleBtn" onclick="toggleScheduleForm()">+</button>
            </div>
            <div id="scheduleForm" class="schedule-form hidden">
                <label for="channelSelect">Channel / Pin</label>
                <select id="channelSelect"></select>

                <label for="scheduleTime">Start Time</label>
                <input type="time" id="scheduleTime" value="06:00">

                <label for="scheduleDuration">Duration (minutes)</label>
                <input type="number" id="scheduleDuration" min="1" max="240" value="30">

                <span class="day-label">Days</span>
                <div class="day-picker" id="dayPicker">
                    <button type="button" class="day-btn active" data-day="0">S</button>
                    <button type="button" class="day-btn active" data-day="1">M</button>
                    <button type="button" class="day-btn active" data-day="2">T</button>
                    <button type="button" class="day-btn active" data-day="3">W</button>
                    <button type="button" class="day-btn active" data-day="4">T</button>
                    <button type="button" class="day-btn active" data-day="5">F</button>
                    <button type="button" class="day-btn active" data-day="6">S</button>
                </div>

                <div class="form-actions">
                    <button type="button" onclick="saveSchedule()">Save</button>
                    <button type="button" class="secondary" onclick="cancelSchedule()">Cancel</button>
                </div>
            </div>
            <div id="scheduleMessage" class="schedule-message" style="display:none;"></div>
            <div id="scheduleList" class="channel-grid"></div>
        </div>

        <script>
        let channelMeta = [];
        let channelStatus = {};
        let channelInverted = {};
        let slaveNodes = {};
        let editingScheduleId = null;

        const DAY_NAMES = ['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];

        document.addEventListener('DOMContentLoaded', function() {
            loadScheduleData();
            loadChannelStatus();
            loadNodeStatus();
            setInterval(loadChannelStatus, 2000);
            setInterval(loadNodeStatus, 2000);

            // Day picker toggle
            document.querySelectorAll('.day-btn').forEach(btn => {
                btn.addEventListener('click', function() {
                    this.classList.toggle('active');
                });
            });
        });

        function getWeekdaysMask() {
            let mask = 0;
            document.querySelectorAll('.day-btn').forEach(btn => {
                if (btn.classList.contains('active')) {
                    mask |= (1 << parseInt(btn.dataset.day));
                }
            });
            return mask;
        }

        function setWeekdaysMask(mask) {
            document.querySelectorAll('.day-btn').forEach(btn => {
                const day = parseInt(btn.dataset.day);
                if (mask & (1 << day)) {
                    btn.classList.add('active');
                } else {
                    btn.classList.remove('active');
                }
            });
        }

        function formatDays(weekdays) {
            if (weekdays === 0x7F) return 'Daily';
            if (weekdays === 0x3E) return 'Weekdays';
            if (weekdays === 0x41) return 'Weekends';
            let days = [];
            for (let i = 0; i < 7; i++) {
                if (weekdays & (1 << i)) days.push(DAY_NAMES[i]);
            }
            return days.join(', ');
        }

        function loadChannelStatus() {
            fetch('/api/channels/status')
                .then(response => response.json())
                .then(data => {
                    if (data.success) {
                        channelStatus = {};
                        channelInverted = {};
                        (data.channels || []).forEach(ch => {
                            channelStatus[ch.channel] = ch.running;
                            channelInverted[ch.channel] = ch.inverted;
                        });
                        renderManualControls();
                    }
                })
                .catch(() => {});
        }

        function loadNodeStatus() {
            fetch('/api/nodes/pending')
                .then(response => response.json())
                .then(data => {
                    if (!data.success) return;
                    const slaves = data.slaves || [];
                    const pending = data.pending;

                    // Store slave data keyed by virtual_channel for renderManualControls
                    slaveNodes = {};
                    slaves.forEach(s => { slaveNodes[s.virtual_channel] = s; });

                    // Pending pair request — shown above channel list
                    const pendingDiv = document.getElementById('pendingPairContainer');
                    if (pending) {
                        pendingDiv.innerHTML = `<div class="channel-row" style="border:1px solid #f6ad55;border-radius:6px;padding:8px;margin-bottom:8px;">
                            <div class="channel-label" style="flex:1;">
                                <strong style="color:#f6ad55;">Pair: ${pending.name}</strong>
                                <span class="pin">${pending.node_id}</span>
                            </div>
                            <div class="toggle-btns">
                                <button class="toggle-btn on" onclick="acceptPair()">Accept</button>
                                <button class="toggle-btn off" onclick="rejectPair()">Reject</button>
                            </div>
                        </div>`;
                    } else {
                        pendingDiv.innerHTML = '';
                    }

                    // Re-render manual controls to pick up online/offline status
                    renderManualControls();
                })
                .catch(() => {});
        }

        function acceptPair() {
            fetch('/api/nodes/accept', {method:'POST'})
                .then(r => r.json())
                .then(() => loadNodeStatus())
                .catch(() => {});
        }

        function rejectPair() {
            fetch('/api/nodes/reject', {method:'POST'})
                .then(r => r.json())
                .then(() => loadNodeStatus())
                .catch(() => {});
        }

        function startRename(nodeId, currentName) {
            const display = document.getElementById('name_display_' + nodeId);
            const input = document.getElementById('name_input_' + nodeId);
            if (!display || !input) return;
            display.style.display = 'none';
            input.style.display = 'inline-block';
            input.value = currentName;
            input.focus();
            input.select();
            input.onblur = function() { saveRename(nodeId); };
        }

        function saveRename(nodeId) {
            const input = document.getElementById('name_input_' + nodeId);
            if (!input) return;
            const newName = input.value.trim();
            if (!newName) { loadNodeStatus(); return; }
            fetch('/api/nodes/rename', {
                method:'POST',
                headers:{'Content-Type':'application/json'},
                body: JSON.stringify({node_id: nodeId, name: newName})
            })
            .then(r => r.json())
            .then(() => loadNodeStatus())
            .catch(() => {});
        }

        function unpairSlave(nodeId) {
            if (!confirm('Unpair this slave node?')) return;
            fetch('/api/nodes/unpair', {
                method:'POST',
                headers:{'Content-Type':'application/json'},
                body: JSON.stringify({node_id: nodeId})
            })
            .then(r => r.json())
            .then(() => loadNodeStatus())
            .catch(() => {});
        }

        function renderManualControls() {
            const container = document.getElementById('manualChannels');
            if (!container || channelMeta.length === 0) return;

            let html = '';
            channelMeta.forEach(ch => {
                const running = channelStatus[ch.channel] || false;
                const inverted = channelInverted[ch.channel] || false;
                const statusClass = running ? 'running' : 'stopped';
                const statusText = running ? 'RUNNING' : 'OFF';
                let pinLabel, onlineFlag = '';
                if (ch.remote) {
                    const slave = slaveNodes[ch.channel];
                    const name = (slave && slave.name) ? slave.name : (ch.slave || 'Remote');
                    const online = slave ? slave.online : false;
                    const nid = ch.node_id || '';
                    pinLabel = `<span class="pin">${name}</span>`;
                    onlineFlag = `<button class="invert-btn${online ? ' active' : ''}" style="font-size:9px;padding:1px 4px;" disabled>${online ? 'ON' : 'OFF'}</button>`
                        + (nid ? `<button class="invert-btn" style="font-size:9px;padding:1px 4px;color:#fc8181;" onclick="unpairSlave('${nid}')">UNPAIR</button>` : '');
                } else {
                    pinLabel = `<span class="pin">GPIO ${ch.pin}</span>`;
                }
                html += `<div class="channel-row">
                    <div class="channel-label">
                        Channel ${ch.channel} ${pinLabel}
                        <span class="channel-status ${statusClass}">${statusText}</span>
                        <button class="invert-btn${inverted ? ' active' : ''}" onclick="toggleInvert(${ch.channel})">INV</button>
                        ${onlineFlag}
                    </div>
                    <div class="toggle-btns">
                        <button class="toggle-btn on${running ? ' active' : ''}" onclick="toggleChannel(${ch.channel}, true)">ON</button>
                        <button class="toggle-btn off${!running ? ' active' : ''}" onclick="toggleChannel(${ch.channel}, false)">OFF</button>
                    </div>
                </div>`;
            });
            container.innerHTML = html;
        }

        function toggleChannel(channel, on) {
            const endpoint = on ? '/api/channel/start' : '/api/channel/stop';
            fetch(endpoint, {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({channel: channel, duration: 30})
            })
            .then(response => response.json())
            .then(data => {
                if (data.success) {
                    loadChannelStatus();
                }
            })
            .catch(() => {});
        }

        function toggleInvert(channel) {
            const current = channelInverted[channel] || false;
            fetch('/api/channel/invert', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({channel: channel, inverted: !current})
            })
            .then(response => response.json())
            .then(data => {
                if (data.success) {
                    loadChannelStatus();
                }
            })
            .catch(() => {});
        }

        function showChannelSetup() {
            const div = document.getElementById('channelSetup');
            if (div.style.display === 'none') {
                div.style.display = 'block';
                loadChannelSetup();
            } else {
                div.style.display = 'none';
            }
        }

        function loadChannelSetup() {
            fetch('/api/channels/available')
                .then(r => r.json())
                .then(data => {
                    const container = document.getElementById('channelSetupList');
                    if (!data.success) return;
                    let html = '';
                    data.channels.forEach(ch => {
                        const checked = ch.enabled ? 'checked' : '';
                        html += `<div style="display:flex;align-items:center;gap:8px;padding:4px 0;">
                            <label style="display:flex;align-items:center;gap:6px;cursor:pointer;">
                                <input type="checkbox" ${checked} onchange="toggleChannelEnabled(${ch.channel}, this.checked)">
                                Channel ${ch.channel} (GPIO ${ch.pin})
                            </label>
                        </div>`;
                    });
                    container.innerHTML = html;
                });
        }

        function toggleChannelEnabled(channel, enabled) {
            fetch('/api/channel/enable', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({channel: channel, enabled: enabled})
            })
            .then(r => r.json())
            .then(data => {
                if (data.success) {
                    loadScheduleData();
                    loadChannelStatus();
                }
            });
        }

        function loadScheduleData() {
            fetch('/api/schedules')
                .then(response => response.json())
                .then(data => {
                    if (!data.success) {
                        showScheduleMessage(data.message || 'Unable to load schedules.', true);
                        return;
                    }

                    channelMeta = data.channels || [];
                    renderChannelOptions();
                    renderScheduleList(data.schedules || []);
                    renderManualControls();
                    if (channelMeta.length === 0) {
                        showScheduleMessage('No channels detected. Check Config.h pins.', true);
                    }
                })
                .catch(error => {
                    showScheduleMessage('Failed to load schedules: ' + error, true);
                });
        }

        function renderChannelOptions() {
            const select = document.getElementById('channelSelect');
            if (!select) return;
            select.innerHTML = '';

            channelMeta.forEach(channel => {
                const option = document.createElement('option');
                option.value = channel.channel;
                option.textContent = channel.remote ? `Channel ${channel.channel} - ${channel.slave || 'Remote'}` : `Channel ${channel.channel} - GPIO ${channel.pin}`;
                select.appendChild(option);
            });
        }

        function renderScheduleList(schedules) {
            const container = document.getElementById('scheduleList');
            if (!container) return;

            if (!schedules || schedules.length === 0) {
                container.innerHTML = '<div class="empty-state">No schedules yet. Use the + button to add one.</div>';
                return;
            }

            const grouped = {};
            schedules.forEach(schedule => {
                const key = schedule.channel;
                if (!grouped[key]) {
                    grouped[key] = [];
                }
                grouped[key].push(schedule);
            });

            let html = '';
            Object.keys(grouped).sort((a, b) => Number(a) - Number(b)).forEach(channelKey => {
                const entries = grouped[channelKey].sort((a, b) => {
                    return (a.hour * 60 + a.minute) - (b.hour * 60 + b.minute);
                });
                const meta = channelMeta.find(entry => entry.channel === Number(channelKey));
                const pinLabel = meta ? (meta.remote ? ` - ${meta.slave || 'Remote'}` : ` - GPIO ${meta.pin}`) : '';
                html += `<div class="channel-card">
                    <div class="channel-card-header">
                        <h3>Channel ${channelKey}${pinLabel}</h3>
                    </div>
                    <div class="channel-card-body">`;
                entries.forEach(schedule => {
                    const skipped = schedule.skipped;
                    const skipStyle = skipped ? 'opacity:0.5;text-decoration:line-through;' : '';
                    const skipBtn = skipped
                        ? `<button class="pill-action" onclick="unskipSchedule(${schedule.id})" title="Unskip" style="color:#48bb78;">&#x21ba;</button>`
                        : `<button class="pill-action" onclick="skipSchedule(${schedule.id})" title="Skip next run" style="color:#f6ad55;">&#x23ed;</button>`;
                    const wd = schedule.weekdays !== undefined ? schedule.weekdays : 0x7F;
                    const dayText = formatDays(wd);
                    html += `<div class="schedule-pill">
                        <div style="${skipStyle}"><strong>${formatTime(schedule.hour, schedule.minute)}</strong> - ${schedule.duration} min<br><span style="font-size:11px;opacity:0.7;">${dayText}</span>${skipped ? ' (skipped)' : ''}</div>
                        <div style="display:flex;gap:4px;">
                            ${skipBtn}
                            <button class="pill-action" onclick="editSchedule(${schedule.id},${schedule.channel},'${formatTime(schedule.hour,schedule.minute)}',${schedule.duration},${wd})" title="Edit">&#9998;</button>
                            <button class="pill-action" onclick="deleteSchedule(${schedule.id})">&times;</button>
                        </div>
                    </div>`;
                });
                html += `</div></div>`;
            });
            container.innerHTML = html;
        }

        function formatTime(hour, minute) {
            const h = String(hour).padStart(2, '0');
            const m = String(minute).padStart(2, '0');
            return `${h}:${m}`;
        }

        function setScheduleForm(open) {
            const form = document.getElementById('scheduleForm');
            const button = document.getElementById('addScheduleBtn');
            if (!form || !button) return;
            if (open) {
                form.classList.remove('hidden');
                button.textContent = '×';
            } else {
                form.classList.add('hidden');
                button.textContent = '+';
            }
        }

        function toggleScheduleForm() {
            const form = document.getElementById('scheduleForm');
            if (!form) return;
            setScheduleForm(form.classList.contains('hidden'));
        }

        function cancelSchedule(resetMessage) {
            if (resetMessage === undefined) {
                resetMessage = true;
            }
            editingScheduleId = null;
            const time = document.getElementById('scheduleTime');
            const duration = document.getElementById('scheduleDuration');
            if (time) time.value = '06:00';
            if (duration) duration.value = 30;
            setWeekdaysMask(0x7F);
            if (resetMessage) {
                showScheduleMessage('', false);
            }
            setScheduleForm(false);
        }

        function showScheduleMessage(text, isError) {
            const msg = document.getElementById('scheduleMessage');
            if (!msg) return;
            if (!text) {
                msg.style.display = 'none';
                return;
            }
            msg.style.display = 'block';
            msg.style.background = isError ? 'rgba(255,0,0,0.2)' : 'rgba(0,255,0,0.15)';
            msg.style.color = isError ? '#ff6666' : '#0f0';
            msg.innerHTML = text;
        }

        function editSchedule(id, channel, time, duration, weekdays) {
            editingScheduleId = id;
            const select = document.getElementById('channelSelect');
            const timeInput = document.getElementById('scheduleTime');
            const durationInput = document.getElementById('scheduleDuration');
            if (select) select.value = channel;
            if (timeInput) timeInput.value = time;
            if (durationInput) durationInput.value = duration;
            setWeekdaysMask(weekdays !== undefined ? weekdays : 0x7F);
            setScheduleForm(true);
        }

        function saveSchedule() {
            const select = document.getElementById('channelSelect');
            const time = document.getElementById('scheduleTime');
            const duration = document.getElementById('scheduleDuration');

            if (!select || !time || !duration) {
                showScheduleMessage('Schedule form is not ready.', true);
                return;
            }

            if (!time.value) {
                showScheduleMessage('Select a start time.', true);
                return;
            }

            const parts = time.value.split(':');
            if (parts.length < 2) {
                showScheduleMessage('Invalid time format.', true);
                return;
            }

            const weekdays = getWeekdaysMask();
            if (weekdays === 0) {
                showScheduleMessage('Select at least one day.', true);
                return;
            }

            const payload = {
                channel: parseInt(select.value, 10),
                hour: parseInt(parts[0], 10),
                minute: parseInt(parts[1], 10),
                duration: parseInt(duration.value, 10),
                weekdays: weekdays
            };

            if (!payload.channel || payload.duration <= 0) {
                showScheduleMessage('Channel and duration are required.', true);
                return;
            }

            showScheduleMessage('Saving schedule...', false);

            if (editingScheduleId !== null) {
                payload.id = editingScheduleId;
            }

            fetch('/api/schedules', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify(payload)
            })
            .then(response => response.json())
            .then(data => {
                if (data.success) {
                    editingScheduleId = null;
                    cancelSchedule(false);
                    showScheduleMessage('Schedule saved.', false);
                    loadScheduleData();
                } else {
                    showScheduleMessage(data.message || 'Failed to save schedule.', true);
                }
            })
            .catch(error => {
                showScheduleMessage('Failed to save schedule: ' + error, true);
            });
        }

        function skipSchedule(id) {
            fetch('/api/schedule/skip', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({id: id})
            })
            .then(r => r.json())
            .then(() => loadScheduleData())
            .catch(() => {});
        }

        function unskipSchedule(id) {
            fetch('/api/schedule/unskip', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({id: id})
            })
            .then(r => r.json())
            .then(() => loadScheduleData())
            .catch(() => {});
        }

        function deleteSchedule(index) {
            if (index === undefined || index === null) return;
            if (!confirm('Remove this schedule?')) {
                return;
            }

            fetch('/api/schedules?id=' + index, {
                method: 'DELETE'
            })
            .then(response => response.json())
            .then(data => {
                if (data.success) {
                    showScheduleMessage('Schedule removed.', false);
                    loadScheduleData();
                } else {
                    showScheduleMessage(data.message || 'Failed to remove schedule.', true);
                }
            })
            .catch(error => {
                showScheduleMessage('Failed to remove schedule: ' + error, true);
            });
        }
        </script>
)rawliteral";

    // Add MQTT configuration form if not connected
    bool mqttConnected = _homeAssistant && _homeAssistant->isConnected();
    if (!mqttConnected) {
        String mqttBroker = "";
        uint16_t mqttPort = 1883;
        String mqttUser = "";
        if (_homeAssistant) {
            mqttBroker = _homeAssistant->getMqttBroker();
            mqttPort = _homeAssistant->getMqttPort();
            mqttUser = _homeAssistant->getMqttUser();
        } else if (SPIFFS.exists(MQTT_CREDENTIALS_FILE)) {
            File f = SPIFFS.open(MQTT_CREDENTIALS_FILE, "r");
            if (f) {
                StaticJsonDocument<512> cred;
                if (!deserializeJson(cred, f)) {
                    mqttBroker = cred["broker"] | "";
                    mqttPort = cred["port"] | 1883;
                    mqttUser = cred["user"] | "";
                }
                f.close();
            }
        }

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

                <div style="margin-top:15px; display:flex; gap:10px; flex-wrap:wrap;">
                    <button type="button" onclick="testMqtt()" style="padding:10px 20px; background:#48bb78; color:#fff; border:none; border-radius:5px; cursor:pointer;">Test Connection</button>
                    <button type="button" onclick="saveMqtt()" style="padding:10px 20px; background:#667eea; color:#fff; border:none; border-radius:5px; cursor:pointer;">Save & Restart</button>
                    <button type="button" onclick="removeMqtt()" style="padding:10px 20px; background:#dc3545; color:#fff; border:none; border-radius:5px; cursor:pointer;">Remove MQTT</button>
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
                <button type="button" onclick="removeWiFi()" style="flex:1; min-width:120px; padding:12px 20px; background:#dc3545; color:#fff; border:none; border-radius:5px; cursor:pointer; font-weight:bold;">Remove WiFi</button>
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

        function removeWiFi() {
            if (!confirm('Are you sure you want to remove WiFi credentials? The device will restart in configuration mode.')) {
                return;
            }

            var msg = document.getElementById('systemMessage');
            msg.style.display = 'block';
            msg.style.background = 'rgba(255,255,0,0.2)';
            msg.style.color = '#ff0';
            msg.innerHTML = 'Removing WiFi credentials...';

            fetch('/wifi/remove', {
                method: 'POST'
            })
            .then(response => response.json())
            .then(data => {
                msg.style.background = 'rgba(0,255,0,0.2)';
                msg.style.color = '#0f0';
                msg.innerHTML = 'WiFi credentials removed. Device is restarting...';
            })
            .catch(error => {
                msg.style.background = 'rgba(0,255,0,0.2)';
                msg.style.color = '#0f0';
                msg.innerHTML = 'WiFi credentials removed. Device is restarting...';
            });
        }

        function removeMqtt() {
            if (!confirm('Are you sure you want to remove MQTT credentials?')) {
                return;
            }

            var msg = document.getElementById('mqttMessage');
            msg.style.display = 'block';
            msg.style.background = 'rgba(255,255,0,0.2)';
            msg.style.color = '#ff0';
            msg.innerHTML = 'Removing MQTT credentials...';

            fetch('/mqtt/remove', {
                method: 'POST'
            })
            .then(response => response.json())
            .then(data => {
                if (data.success) {
                    msg.style.background = 'rgba(0,255,0,0.2)';
                    msg.style.color = '#0f0';
                    msg.innerHTML = 'SUCCESS: ' + data.message;
                    setTimeout(function() {
                        location.reload();
                    }, 2000);
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

    // Schedule management APIs
    _webServer->on("/api/schedules", HTTP_GET, [this]() {
        if (!_controller) {
            _webServer->send(500, "application/json", "{\"success\":false,\"message\":\"Controller not ready\"}");
            return;
        }

        DynamicJsonDocument doc(2048);
        doc["success"] = true;

        JsonArray channels = doc.createNestedArray("channels");
        // Only enabled local channels
        for (uint8_t i = 0; i < NUM_LOCAL_CHANNELS; i++) {
            if (!_controller->isChannelEnabled(i + 1)) continue;
            JsonObject channel = channels.createNestedObject();
            channel["channel"] = i + 1;
            channel["pin"] = CHANNEL_PINS[i];
        }
        // Virtual channels for paired slaves
        if (_nodeManager) {
            for (uint8_t i = 0; i < _nodeManager->getSlaveCount(); i++) {
                const NodePeer* slave = _nodeManager->getSlave(i);
                if (!slave) continue;
                for (uint8_t ch = 0; ch < slave->num_channels; ch++) {
                    JsonObject channel = channels.createNestedObject();
                    channel["channel"] = slave->base_virtual_ch + ch;
                    channel["pin"] = 0;
                    channel["remote"] = true;
                    channel["slave"] = slave->name[0] ? slave->name : slave->node_id;
                    channel["node_id"] = slave->node_id;
                }
            }
        }

        IrrigationSchedule schedules[MAX_SCHEDULES];
        uint8_t count = 0;
        _controller->getSchedules(schedules, count);

        JsonArray array = doc.createNestedArray("schedules");
        for (uint8_t i = 0; i < count; i++) {
            if (!schedules[i].enabled) {
                continue;
            }

            JsonObject entry = array.createNestedObject();
            entry["id"] = i;
            entry["channel"] = schedules[i].channel;
            entry["hour"] = schedules[i].hour;
            entry["minute"] = schedules[i].minute;
            entry["duration"] = schedules[i].durationMinutes;
            entry["weekdays"] = schedules[i].weekdays;
            entry["pin"] = _controller->getChannelPin(schedules[i].channel);
            entry["skipped"] = _controller->isScheduleSkipped(i);
        }

        String json;
        serializeJson(doc, json);
        _webServer->send(200, "application/json", json);
    });

    _webServer->on("/api/schedules", HTTP_POST, [this]() {
        if (!_controller) {
            _webServer->send(500, "application/json", "{\"success\":false,\"message\":\"Controller not ready\"}");
            return;
        }

        if (!_webServer->hasArg("plain")) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"Missing payload\"}");
            return;
        }

        DynamicJsonDocument doc(512);
        DeserializationError error = deserializeJson(doc, _webServer->arg("plain"));
        if (error) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
            return;
        }

        uint8_t channel = doc["channel"] | 0;
        uint8_t hour = doc["hour"] | 0;
        uint8_t minute = doc["minute"] | 0;
        uint16_t duration = doc["duration"] | DEFAULT_DURATION_MINUTES;
        uint8_t weekdays = doc["weekdays"] | 0x7F;
        int16_t editId = doc["id"] | -1;

        if (channel < 1 || channel > MAX_CHANNELS) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid channel\"}");
            return;
        }
        if (hour > 23 || minute > 59) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid start time\"}");
            return;
        }
        if (duration < MIN_DURATION_MINUTES || duration > MAX_DURATION_MINUTES) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid duration\"}");
            return;
        }

        if (editId >= 0) {
            // Update existing schedule
            if (_controller->updateSchedule((uint8_t)editId, channel, hour, minute, duration, weekdays)) {
                // Sync to slave if virtual channel
                if (_nodeManager && channel > NUM_LOCAL_CHANNELS) {
                    const NodePeer* slave = _nodeManager->getSlave(0);
                    for (uint8_t s = 0; s < _nodeManager->getSlaveCount(); s++) {
                        slave = _nodeManager->getSlave(s);
                        if (slave && channel >= slave->base_virtual_ch &&
                            channel < slave->base_virtual_ch + slave->num_channels) {
                            _nodeManager->sendScheduleSync(slave->node_id);
                            break;
                        }
                    }
                }
                _webServer->send(200, "application/json", "{\"success\":true,\"message\":\"Schedule updated\"}");
            } else {
                _webServer->send(500, "application/json", "{\"success\":false,\"message\":\"Unable to update schedule\"}");
            }
        } else {
            // Add new schedule
            int8_t index = _controller->addSchedule(channel, hour, minute, duration, weekdays);
            if (index >= 0) {
                // Sync to slave if virtual channel
                if (_nodeManager && channel > NUM_LOCAL_CHANNELS) {
                    for (uint8_t s = 0; s < _nodeManager->getSlaveCount(); s++) {
                        const NodePeer* slave = _nodeManager->getSlave(s);
                        if (slave && channel >= slave->base_virtual_ch &&
                            channel < slave->base_virtual_ch + slave->num_channels) {
                            _nodeManager->sendScheduleSync(slave->node_id);
                            break;
                        }
                    }
                }
                _webServer->send(200, "application/json", "{\"success\":true,\"message\":\"Schedule saved\",\"index\":" + String(index) + "}");
            } else {
                _webServer->send(500, "application/json", "{\"success\":false,\"message\":\"Unable to save schedule\"}");
            }
        }
    });

    _webServer->on("/api/schedules", HTTP_DELETE, [this]() {
        if (!_controller) {
            _webServer->send(500, "application/json", "{\"success\":false,\"message\":\"Controller not ready\"}");
            return;
        }

        if (!_webServer->hasArg("id")) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"Missing schedule id\"}");
            return;
        }

        uint8_t index = _webServer->arg("id").toInt();

        // Before removing, check if it belongs to a slave so we can sync after
        IrrigationSchedule sched = _controller->getSchedule(index);
        uint8_t schedCh = sched.channel;

        if (_controller->removeSchedule(index)) {
            // Sync to slave if the removed schedule was for a virtual channel
            if (_nodeManager && schedCh > NUM_LOCAL_CHANNELS) {
                for (uint8_t s = 0; s < _nodeManager->getSlaveCount(); s++) {
                    const NodePeer* slave = _nodeManager->getSlave(s);
                    if (slave && schedCh >= slave->base_virtual_ch &&
                        schedCh < slave->base_virtual_ch + slave->num_channels) {
                        _nodeManager->sendScheduleSync(slave->node_id);
                        break;
                    }
                }
            }
            _webServer->send(200, "application/json", "{\"success\":true,\"message\":\"Schedule removed\"}");
        } else {
            _webServer->send(500, "application/json", "{\"success\":false,\"message\":\"Failed to remove schedule\"}");
        }
    });

    // Channel status API
    _webServer->on("/api/channels/status", HTTP_GET, [this]() {
        if (!_controller) {
            _webServer->send(500, "application/json", "{\"success\":false,\"message\":\"Controller not ready\"}");
            return;
        }

        DynamicJsonDocument doc(512);
        doc["success"] = true;

        JsonArray channels = doc.createNestedArray("channels");
        // Only enabled local channels
        for (uint8_t i = 0; i < NUM_LOCAL_CHANNELS; i++) {
            if (!_controller->isChannelEnabled(i + 1)) continue;
            JsonObject ch = channels.createNestedObject();
            ch["channel"] = i + 1;
            ch["pin"] = CHANNEL_PINS[i];
            ch["running"] = _controller->isChannelIrrigating(i + 1);
            ch["inverted"] = _controller->isChannelInverted(i + 1);
        }
        // Virtual channels for paired slaves
        if (_nodeManager) {
            for (uint8_t s = 0; s < _nodeManager->getSlaveCount(); s++) {
                const NodePeer* slave = _nodeManager->getSlave(s);
                if (!slave) continue;
                for (uint8_t c = 0; c < slave->num_channels; c++) {
                    uint8_t vch = slave->base_virtual_ch + c;
                    JsonObject ch = channels.createNestedObject();
                    ch["channel"] = vch;
                    ch["pin"] = 0;
                    ch["running"] = _controller->isChannelIrrigating(vch);
                    ch["inverted"] = _controller->isChannelInverted(vch);
                    ch["remote"] = true;
                    ch["slave"] = slave->name[0] ? slave->name : slave->node_id;
                }
            }
        }

        String json;
        serializeJson(doc, json);
        _webServer->send(200, "application/json", json);
    });

    // Channel invert API
    _webServer->on("/api/channel/invert", HTTP_POST, [this]() {
        if (!_controller) {
            _webServer->send(500, "application/json", "{\"success\":false,\"message\":\"Controller not ready\"}");
            return;
        }

        if (!_webServer->hasArg("plain")) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"Missing payload\"}");
            return;
        }

        DynamicJsonDocument doc(256);
        DeserializationError error = deserializeJson(doc, _webServer->arg("plain"));
        if (error) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
            return;
        }

        uint8_t channel = doc["channel"] | 0;
        bool inverted = doc["inverted"] | false;

        if (channel < 1 || channel > MAX_CHANNELS) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid channel\"}");
            return;
        }

        _controller->setChannelInverted(channel, inverted);
        DEBUG_PRINTF("WiFiManager: Channel %d invert set to %d\n", channel, inverted);
        _webServer->send(200, "application/json", "{\"success\":true,\"message\":\"Invert setting updated\"}");
    });

    // Channel enable/disable API
    _webServer->on("/api/channel/enable", HTTP_POST, [this]() {
        if (!_controller || !_webServer->hasArg("plain")) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"Bad request\"}");
            return;
        }

        StaticJsonDocument<128> doc;
        deserializeJson(doc, _webServer->arg("plain"));
        uint8_t channel = doc["channel"] | 0;
        bool enabled = doc["enabled"] | true;

        if (channel < 1 || channel > NUM_LOCAL_CHANNELS) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid channel\"}");
            return;
        }

        _controller->setChannelEnabled(channel, enabled);
        _webServer->send(200, "application/json", "{\"success\":true}");
    });

    // List all available local channels (for the "Add Channel" UI)
    _webServer->on("/api/channels/available", HTTP_GET, [this]() {
        StaticJsonDocument<512> doc;
        doc["success"] = true;
        JsonArray channels = doc.createNestedArray("channels");
        for (uint8_t i = 0; i < NUM_LOCAL_CHANNELS; i++) {
            JsonObject ch = channels.createNestedObject();
            ch["channel"] = i + 1;
            ch["pin"] = CHANNEL_PINS[i];
            ch["enabled"] = _controller->isChannelEnabled(i + 1);
        }
        String json;
        serializeJson(doc, json);
        _webServer->send(200, "application/json", json);
    });

    // Channel start API
    _webServer->on("/api/schedule/skip", HTTP_POST, [this]() {
        if (!_controller) {
            _webServer->send(500, "application/json", "{\"success\":false,\"message\":\"Controller not ready\"}");
            return;
        }
        if (!_webServer->hasArg("plain")) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"Missing payload\"}");
            return;
        }
        DynamicJsonDocument doc(128);
        deserializeJson(doc, _webServer->arg("plain"));
        uint8_t id = doc["id"] | 255;
        if (id >= MAX_SCHEDULES) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid schedule id\"}");
            return;
        }
        _controller->skipSchedule(id);

        // Forward skip to slave if schedule is for a virtual channel
        if (_nodeManager) {
            IrrigationSchedule sched = _controller->getSchedule(id);
            if (sched.channel > NUM_LOCAL_CHANNELS) {
                _nodeManager->sendSkipToSlave(sched.channel, id);
            }
        }

        _webServer->send(200, "application/json", "{\"success\":true,\"message\":\"Schedule skipped\"}");
    });

    _webServer->on("/api/schedule/unskip", HTTP_POST, [this]() {
        if (!_controller) {
            _webServer->send(500, "application/json", "{\"success\":false,\"message\":\"Controller not ready\"}");
            return;
        }
        if (!_webServer->hasArg("plain")) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"Missing payload\"}");
            return;
        }
        DynamicJsonDocument doc(128);
        deserializeJson(doc, _webServer->arg("plain"));
        uint8_t id = doc["id"] | 255;
        if (id >= MAX_SCHEDULES) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid schedule id\"}");
            return;
        }
        _controller->unskipSchedule(id);

        // Forward unskip to slave if schedule is for a virtual channel
        if (_nodeManager) {
            IrrigationSchedule sched = _controller->getSchedule(id);
            if (sched.channel > NUM_LOCAL_CHANNELS) {
                _nodeManager->sendUnskipToSlave(sched.channel, id);
            }
        }

        _webServer->send(200, "application/json", "{\"success\":true,\"message\":\"Schedule unskipped\"}");
    });

    _webServer->on("/api/channel/start", HTTP_POST, [this]() {
        if (!_controller) {
            _webServer->send(500, "application/json", "{\"success\":false,\"message\":\"Controller not ready\"}");
            return;
        }

        if (!_webServer->hasArg("plain")) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"Missing payload\"}");
            return;
        }

        DynamicJsonDocument doc(256);
        DeserializationError error = deserializeJson(doc, _webServer->arg("plain"));
        if (error) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
            return;
        }

        uint8_t channel = doc["channel"] | 0;
        uint16_t duration = doc["duration"] | DEFAULT_DURATION_MINUTES;

        if (channel < 1 || channel > MAX_CHANNELS) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid channel\"}");
            return;
        }

        _controller->startIrrigation(channel, duration);
        DEBUG_PRINTF("WiFiManager: Manual start channel %d for %d min\n", channel, duration);
        _webServer->send(200, "application/json", "{\"success\":true,\"message\":\"Channel started\"}");
    });

    // Channel stop API
    _webServer->on("/api/channel/stop", HTTP_POST, [this]() {
        if (!_controller) {
            _webServer->send(500, "application/json", "{\"success\":false,\"message\":\"Controller not ready\"}");
            return;
        }

        if (!_webServer->hasArg("plain")) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"Missing payload\"}");
            return;
        }

        DynamicJsonDocument doc(256);
        DeserializationError error = deserializeJson(doc, _webServer->arg("plain"));
        if (error) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
            return;
        }

        uint8_t channel = doc["channel"] | 0;

        if (channel < 1 || channel > MAX_CHANNELS) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid channel\"}");
            return;
        }

        _controller->stopIrrigation(channel);
        DEBUG_PRINTF("WiFiManager: Manual stop channel %d\n", channel);
        _webServer->send(200, "application/json", "{\"success\":true,\"message\":\"Channel stopped\"}");
    });

    // ================================================================
    // Node pairing API endpoints
    // ================================================================

    // GET /api/nodes/pending — pending pair request + list of paired slaves
    _webServer->on("/api/nodes/pending", HTTP_GET, [this]() {
        DynamicJsonDocument doc(1024);
        doc["success"] = true;

        if (_nodeManager) {
            // Pending pair request
            if (_nodeManager->hasPendingPair()) {
                const PendingPairRequest& req = _nodeManager->getPendingPair();
                JsonObject pending = doc.createNestedObject("pending");
                pending["node_id"] = req.node_id;
                pending["name"] = req.name;
                pending["num_channels"] = req.num_channels;
                pending["ip"] = req.ip.toString();
            }

            // Paired slaves
            JsonArray slaves = doc.createNestedArray("slaves");
            for (uint8_t i = 0; i < _nodeManager->getSlaveCount(); i++) {
                const NodePeer* peer = _nodeManager->getSlave(i);
                if (!peer) continue;
                JsonObject s = slaves.createNestedObject();
                s["node_id"] = peer->node_id;
                s["name"] = peer->name;
                s["virtual_channel"] = peer->base_virtual_ch;
                s["num_channels"] = peer->num_channels;
                s["online"] = peer->online;
                s["rssi"] = peer->rssi;
            }
        }

        String json;
        serializeJson(doc, json);
        _webServer->send(200, "application/json", json);
    });

    // POST /api/nodes/accept — accept pending pair request
    _webServer->on("/api/nodes/accept", HTTP_POST, [this]() {
        if (!_nodeManager) {
            _webServer->send(500, "application/json", "{\"success\":false,\"message\":\"NodeManager not available\"}");
            return;
        }
        if (!_nodeManager->hasPendingPair()) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"No pending pair request\"}");
            return;
        }
        _nodeManager->acceptPendingPair();
        _webServer->send(200, "application/json", "{\"success\":true,\"message\":\"Pair accepted\"}");
    });

    // POST /api/nodes/reject — reject pending pair request
    _webServer->on("/api/nodes/reject", HTTP_POST, [this]() {
        if (!_nodeManager) {
            _webServer->send(500, "application/json", "{\"success\":false,\"message\":\"NodeManager not available\"}");
            return;
        }
        if (!_nodeManager->hasPendingPair()) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"No pending pair request\"}");
            return;
        }
        _nodeManager->rejectPendingPair(PAIR_REJECT_USER);
        _webServer->send(200, "application/json", "{\"success\":true,\"message\":\"Pair rejected\"}");
    });

    // POST /api/nodes/rename — rename a paired slave
    _webServer->on("/api/nodes/rename", HTTP_POST, [this]() {
        if (!_nodeManager) {
            _webServer->send(500, "application/json", "{\"success\":false,\"message\":\"NodeManager not available\"}");
            return;
        }

        if (!_webServer->hasArg("plain")) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"Missing payload\"}");
            return;
        }

        StaticJsonDocument<256> reqDoc;
        DeserializationError error = deserializeJson(reqDoc, _webServer->arg("plain"));
        if (error) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
            return;
        }

        const char* nodeId = reqDoc["node_id"] | "";
        const char* newName = reqDoc["name"] | "";
        if (strlen(nodeId) == 0 || strlen(newName) == 0) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"node_id and name required\"}");
            return;
        }

        if (_nodeManager->renameSlave(nodeId, newName)) {
            _webServer->send(200, "application/json", "{\"success\":true,\"message\":\"Slave renamed\"}");
        } else {
            _webServer->send(404, "application/json", "{\"success\":false,\"message\":\"Slave not found\"}");
        }
    });

    // POST /api/nodes/unpair — remove a paired slave
    _webServer->on("/api/nodes/unpair", HTTP_POST, [this]() {
        if (!_nodeManager) {
            _webServer->send(500, "application/json", "{\"success\":false,\"message\":\"NodeManager not available\"}");
            return;
        }

        if (!_webServer->hasArg("plain")) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"Missing payload\"}");
            return;
        }

        StaticJsonDocument<128> reqDoc;
        DeserializationError error = deserializeJson(reqDoc, _webServer->arg("plain"));
        if (error) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
            return;
        }

        const char* nodeId = reqDoc["node_id"] | "";
        if (strlen(nodeId) == 0) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"node_id required\"}");
            return;
        }

        if (_nodeManager->unpairSlave(nodeId)) {
            // Refresh HA discovery to remove stale channel entities
            if (_homeAssistant) _homeAssistant->refreshDiscovery();
            _webServer->send(200, "application/json", "{\"success\":true,\"message\":\"Slave unpaired\"}");
        } else {
            _webServer->send(404, "application/json", "{\"success\":false,\"message\":\"Slave not found\"}");
        }
    });

    // MQTT configuration save — works even when mqtt feature is currently disabled
    _webServer->on("/mqtt/save", HTTP_POST, [this]() {
        String broker = _webServer->hasArg("broker") ? _webServer->arg("broker") : "";
        uint16_t port = _webServer->hasArg("port") ? _webServer->arg("port").toInt() : 1883;
        String user = _webServer->hasArg("user") ? _webServer->arg("user") : "";
        String password = _webServer->hasArg("password") ? _webServer->arg("password") : "";

        if (broker.length() == 0) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"Broker required\"}");
            return;
        }

        // Save credentials directly to SPIFFS (works whether HomeAssistant is initialized or not)
        StaticJsonDocument<512> credDoc;
        credDoc["broker"] = broker;
        credDoc["port"] = port;
        credDoc["user"] = user;
        credDoc["password"] = password;

        File credFile = SPIFFS.open(MQTT_CREDENTIALS_FILE, "w");
        if (!credFile || serializeJson(credDoc, credFile) == 0) {
            if (credFile) credFile.close();
            _webServer->send(500, "application/json", "{\"success\":false,\"message\":\"Failed to save credentials\"}");
            return;
        }
        credFile.close();

        // Enable mqtt feature flag and persist to config.json so it survives reboot
        features.mqtt = true;

        StaticJsonDocument<1024> cfgDoc;
        File cfgRead = SPIFFS.open(CONFIG_FILE, "r");
        if (cfgRead) {
            deserializeJson(cfgDoc, cfgRead);
            cfgRead.close();
        }
        cfgDoc["node_id"] = nodeId;
        cfgDoc["role"] = nodeRole;
        JsonObject feat = cfgDoc.containsKey("features") ? cfgDoc["features"] : cfgDoc.createNestedObject("features");
        feat["mqtt"] = true;

        File cfgWrite = SPIFFS.open(CONFIG_FILE, "w");
        if (cfgWrite) {
            serializeJson(cfgDoc, cfgWrite);
            cfgWrite.close();
        }

        DEBUG_PRINTLN("WiFiManager: MQTT credentials saved, mqtt feature enabled");
        _webServer->send(200, "application/json", "{\"success\":true,\"message\":\"Saved. Restarting...\"}");
        delay(2000);
        ESP.restart();
    });

    // MQTT test connection — works even when mqtt feature is currently disabled
    _webServer->on("/mqtt/test", HTTP_POST, [this]() {
        String broker = _webServer->hasArg("broker") ? _webServer->arg("broker") : "";
        uint16_t port = _webServer->hasArg("port") ? _webServer->arg("port").toInt() : 1883;
        String user = _webServer->hasArg("user") ? _webServer->arg("user") : "";
        String password = _webServer->hasArg("password") ? _webServer->arg("password") : "";

        if (broker.length() == 0) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"Broker required\"}");
            return;
        }

        // Test connection directly (no need for HomeAssistant object)
        WiFiClient testClient;
        PubSubClient testMqtt(testClient);
        testMqtt.setServer(broker.c_str(), port);

        bool success = false;
        if (user.length() > 0) {
            success = testMqtt.connect("irrigation_test", user.c_str(), password.c_str());
        } else {
            success = testMqtt.connect("irrigation_test");
        }
        if (success) testMqtt.disconnect();

        if (success) {
            _webServer->send(200, "application/json", "{\"success\":true,\"message\":\"Connection successful!\"}");
        } else {
            _webServer->send(200, "application/json", "{\"success\":false,\"message\":\"Connection failed\"}");
        }
    });

    // WiFi credentials removal
    _webServer->on("/wifi/remove", HTTP_POST, [this]() {
        DEBUG_PRINTLN("WiFiManager: WiFi credentials removal requested via web interface");

        if (clearCredentials()) {
            _webServer->send(200, "application/json", "{\"success\":true,\"message\":\"WiFi credentials removed. Restarting...\"}");
            delay(2000);
            ESP.restart();
        } else {
            _webServer->send(500, "application/json", "{\"success\":false,\"message\":\"Failed to remove WiFi credentials\"}");
        }
    });

    // MQTT credentials removal
    _webServer->on("/mqtt/remove", HTTP_POST, [this]() {
        DEBUG_PRINTLN("WiFiManager: MQTT credentials removal requested via web interface");

        // Remove MQTT credentials file
        if (SPIFFS.exists(MQTT_CREDENTIALS_FILE)) {
            if (SPIFFS.remove(MQTT_CREDENTIALS_FILE)) {
                DEBUG_PRINTLN("WiFiManager: MQTT credentials removed successfully");

                // Disable mqtt feature flag in config.json
                features.mqtt = false;
                StaticJsonDocument<1024> cfgDoc;
                File cfgRead = SPIFFS.open(CONFIG_FILE, "r");
                if (cfgRead) {
                    deserializeJson(cfgDoc, cfgRead);
                    cfgRead.close();
                }
                cfgDoc["features"]["mqtt"] = false;
                File cfgWrite = SPIFFS.open(CONFIG_FILE, "w");
                if (cfgWrite) {
                    serializeJson(cfgDoc, cfgWrite);
                    cfgWrite.close();
                }

                _webServer->send(200, "application/json", "{\"success\":true,\"message\":\"MQTT credentials removed successfully\"}");
            } else {
                DEBUG_PRINTLN("WiFiManager: Failed to remove MQTT credentials file");
                _webServer->send(500, "application/json", "{\"success\":false,\"message\":\"Failed to remove MQTT credentials\"}");
            }
        } else {
            _webServer->send(200, "application/json", "{\"success\":true,\"message\":\"No MQTT credentials to remove\"}");
        }
    });

    // Feature flags config API
    _webServer->on("/api/config", HTTP_GET, [this]() {
        StaticJsonDocument<512> doc;
        doc["success"] = true;
        doc["node_id"] = nodeId;
        doc["role"] = nodeRole;

        JsonObject feat = doc.createNestedObject("features");
        feat["multi_node"] = features.multi_node;
        feat["mqtt"] = features.mqtt;
        feat["web_ui"] = features.web_ui;
        feat["sensors"] = features.sensors;
        feat["battery"] = features.battery;
        feat["ota"] = features.ota;
        feat["debug"] = features.debug;

        String json;
        serializeJson(doc, json);
        _webServer->send(200, "application/json", json);
    });

    _webServer->on("/api/config", HTTP_POST, [this]() {
        if (!_webServer->hasArg("plain")) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"No body\"}");
            return;
        }

        StaticJsonDocument<512> doc;
        DeserializationError error = deserializeJson(doc, _webServer->arg("plain"));
        if (error) {
            _webServer->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
            return;
        }

        // Update feature flags (only update fields that are present)
        JsonObject feat = doc["features"];
        if (!feat.isNull()) {
            if (feat.containsKey("multi_node")) features.multi_node = feat["multi_node"];
            if (feat.containsKey("mqtt")) features.mqtt = feat["mqtt"];
            if (feat.containsKey("web_ui")) features.web_ui = feat["web_ui"];
            if (feat.containsKey("sensors")) features.sensors = feat["sensors"];
            if (feat.containsKey("battery")) features.battery = feat["battery"];
            if (feat.containsKey("ota")) features.ota = feat["ota"];
            if (feat.containsKey("debug")) features.debug = feat["debug"];
        }

        // Update node identity if provided
        if (doc.containsKey("node_id")) nodeId = doc["node_id"].as<String>();
        if (doc.containsKey("role")) nodeRole = doc["role"].as<String>();

        // Save to SPIFFS
        StaticJsonDocument<1024> saveDoc;
        saveDoc["node_id"] = nodeId;
        saveDoc["role"] = nodeRole;
        JsonObject saveFeat = saveDoc.createNestedObject("features");
        saveFeat["multi_node"] = features.multi_node;
        saveFeat["mqtt"] = features.mqtt;
        saveFeat["web_ui"] = features.web_ui;
        saveFeat["sensors"] = features.sensors;
        saveFeat["battery"] = features.battery;
        saveFeat["ota"] = features.ota;
        saveFeat["debug"] = features.debug;

        File file = SPIFFS.open(CONFIG_FILE, "w");
        if (file) {
            serializeJson(saveDoc, file);
            file.close();
            DEBUG_PRINTLN("WiFiManager: Config saved to SPIFFS");
        }

        _webServer->send(200, "application/json",
            "{\"success\":true,\"message\":\"Config saved. Restart to apply changes.\"}");
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
