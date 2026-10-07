/*
 * homeserver.ino - Servidor web con sensor AHT10 + emisor IR (ESP32-C3 Super Mini)
 *
 * Expone el sensor AHT10 y el estado del aire (York, HAIER_AC_YRW02). El estado
 * canonico lo posee y persiste el backend Node (all-node-app), que envia siempre
 * el estado absoluto via /api/ac/set. Este firmware solo aplica los setters del
 * protocolo y transmite.
 *
 * Hardware:
 *   AHT10  SDA -> GPIO8, SCL -> GPIO9
 *   LED emisor IR -> GPIO3
 *
 * Librerias: Adafruit AHTX0 e IRremoteESP8266 (crankyoldgit).
 *
 * API REST:
 *   GET  /api/sensors    -> temperatura y humedad
 *   GET  /api/ac/status  -> estado del aire (JSON)
 *   GET|POST /api/ac/set -> construye y envia estado absoluto del aire
 *
 * Comandos por monitor Serie (115200):
 *   status | on | off | set <temp> <mode> <fan> | h
 */

#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_AHTX0.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <ir_Haier.h>

/* --- WiFi --- */
const char* ssid = "HermososPA";
const char* password = "1823LomeliPlascencia";

/* --- Pines / IR --- */
#define IR_SEND_PIN 3

/* --- Robustez IR / WiFi ---
 * El WiFi comparte la fuente con el LED IR; los picos de TX y el modem sleep
 * pueden debilitar la trama. Repetimos la trama (idempotente por ser absoluta)
 * y usamos maxima potencia de WiFi (sleep off) para un enlace mas estable.
 */
#define WIFI_TX_POWER WIFI_POWER_19_5dBm
#define IR_REPEAT 3
#define IR_REPEAT_GAP_MS 50
#define IR_SETTLE_MS 30

/* ------------------------------------------------------------------
 * Estado del aire
 * ------------------------------------------------------------------ */
struct AcState {
    bool valid;
    bool power;
    bool turbo;
    bool quiet;
    bool sleep;
    bool health;
    uint8_t tempC;
    uint8_t mode;
    uint8_t fan;
    uint8_t swingV;
    uint8_t swingH;
    uint8_t button;
};

IRHaierACYRW02 ac(IR_SEND_PIN);

AcState lastState = { false, false, false, false, false, false,
                      24, 1, 2, 0, 0, kHaierAcYrw02ButtonPower };

WebServer server(80);
Adafruit_AHTX0 aht;
bool ahtOk = false;

char lineBuf[64];
uint8_t lineLen = 0;

/* ------------------------------------------------------------------
 * Prototipos
 * ------------------------------------------------------------------ */
void transmitState();
void handleSensors();
void handleAcStatus();
void handleAcSet();
void handleNotFound();

uint8_t parseModeArg(const String& v);
uint8_t parseFanArg(const String& v);
uint8_t parseSwingVArg(const String& v);
uint8_t parseSwingHArg(const String& v);
bool parseBoolArg(const String& v);

const char* modeLabel(uint8_t mode);
const char* fanLabel(uint8_t fan);
const char* swingVLabel(uint8_t pos);
const char* swingHLabel(uint8_t pos);
String quotedLabel(const char* label, uint8_t raw);
String stateToJSON();

void handleLine(char* line);
void printHelp();

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println(F("================================================"));
    Serial.println(F(" Homeserver: AHT10 + Emisor IR - ESP32-C3"));
    Serial.println(F("================================================"));

    ac.begin();

    if (!aht.begin()) {
        Serial.println(F("AHT10 no encontrado (el endpoint /api/sensors devolvera error)."));
        ahtOk = false;
    } else {
        ahtOk = true;
    }

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_TX_POWER);
    WiFi.begin(ssid, password);
    Serial.print(F("Conectando a WiFi"));
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print('.');
    }
    Serial.println();
    Serial.print(F("Conectado. IP: "));
    Serial.println(WiFi.localIP());
    Serial.print(F("WiFi TX power maximo, sleep off. IR repeat x"));
    Serial.println(IR_REPEAT);

    server.on("/api/sensors", HTTP_GET, handleSensors);
    server.on("/api/ac/status", HTTP_GET, handleAcStatus);
    server.on("/api/ac/set", HTTP_GET, handleAcSet);
    server.on("/api/ac/set", HTTP_POST, handleAcSet);
    server.onNotFound(handleNotFound);

    server.begin();
    Serial.println(F("Servidor web iniciado."));
    printHelp();
}

void loop() {
    server.handleClient();

    while (Serial.available() > 0) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (lineLen > 0) {
                lineBuf[lineLen] = '\0';
                handleLine(lineBuf);
                lineLen = 0;
            }
        } else if (lineLen < sizeof(lineBuf) - 1) {
            lineBuf[lineLen++] = c;
        }
    }
}

/* ------------------------------------------------------------------
 * Handlers HTTP
 * ------------------------------------------------------------------ */
uint8_t parseModeArg(const String& v) {
    if (v == "auto") return 0;
    if (v == "cool") return 1;
    if (v == "dry")  return 2;
    if (v == "heat") return 4;
    if (v == "fan")  return 6;
    return (uint8_t)v.toInt();
}

uint8_t parseFanArg(const String& v) {
    if (v == "high") return 1;
    if (v == "med")  return 2;
    if (v == "low")  return 3;
    if (v == "auto") return 5;
    return (uint8_t)v.toInt();
}

uint8_t parseSwingVArg(const String& v) {
    if (v == "off")    return 0;
    if (v == "top")    return 1;
    if (v == "middle") return 2;
    if (v == "bottom") return 3;
    if (v == "down")   return 0xA;
    if (v == "auto")   return 0xC;
    return (uint8_t)v.toInt();
}

uint8_t parseSwingHArg(const String& v) {
    if (v == "middle")    return 0;
    if (v == "left_max")  return 3;
    if (v == "left")      return 4;
    if (v == "right")     return 5;
    if (v == "right_max") return 6;
    if (v == "auto")      return 7;
    return (uint8_t)v.toInt();
}

bool parseBoolArg(const String& v) {
    return v == "1" || v == "on" || v == "true" || v == "yes";
}

void handleSensors() {
    if (!ahtOk) {
        server.send(503, "application/json",
                    F("{\"status\":\"error\",\"error\":\"AHT10 no encontrado\"}"));
        return;
    }
    sensors_event_t humidity, temp;
    aht.getEvent(&humidity, &temp);

    String j = "{\"temperature\":";
    j += String(temp.temperature, 2);
    j += ",\"humidity\":";
    j += String(humidity.relative_humidity, 2);
    j += ",\"unit\":\"celsius\",\"status\":\"success\"}";
    server.send(200, "application/json", j);
}

void handleAcStatus() {
    server.send(200, "application/json", stateToJSON());
}

void handleAcSet() {
    uint8_t prevTemp   = lastState.tempC;
    uint8_t prevMode   = lastState.mode;
    uint8_t prevFan    = lastState.fan;
    uint8_t prevSwingV = lastState.swingV;
    uint8_t prevSwingH = lastState.swingH;
    bool prevPower  = lastState.power;
    bool prevTurbo  = lastState.turbo;
    bool prevQuiet  = lastState.quiet;
    bool prevSleep  = lastState.sleep;
    bool prevHealth = lastState.health;

    bool power  = lastState.power;
    uint8_t temp   = lastState.tempC;
    uint8_t mode   = lastState.mode;
    uint8_t fan    = lastState.fan;
    uint8_t swingV = lastState.swingV;
    uint8_t swingH = lastState.swingH;
    bool turbo  = lastState.turbo;
    bool quiet  = lastState.quiet;
    bool sleep  = lastState.sleep;
    bool health = lastState.health;

    bool hasPower  = server.hasArg("power");
    bool hasTemp   = server.hasArg("temp");
    bool hasMode   = server.hasArg("mode");
    bool hasFan    = server.hasArg("fan");
    bool hasSwingV = server.hasArg("swing_v");
    bool hasSwingH = server.hasArg("swing_h");
    bool hasTurbo  = server.hasArg("turbo");
    bool hasQuiet  = server.hasArg("quiet");
    bool hasSleep  = server.hasArg("sleep");
    bool hasHealth = server.hasArg("health");

    if (hasPower)   power  = parseBoolArg(server.arg("power"));
    if (hasTemp)    temp   = (uint8_t)server.arg("temp").toInt();
    if (hasMode)    mode   = parseModeArg(server.arg("mode"));
    if (hasFan)     fan    = parseFanArg(server.arg("fan"));
    if (hasSwingV)  swingV = parseSwingVArg(server.arg("swing_v"));
    if (hasSwingH)  swingH = parseSwingHArg(server.arg("swing_h"));
    if (hasTurbo)   turbo  = parseBoolArg(server.arg("turbo"));
    if (hasQuiet)   quiet  = parseBoolArg(server.arg("quiet"));
    if (hasSleep)   sleep  = parseBoolArg(server.arg("sleep"));
    if (hasHealth)  health = parseBoolArg(server.arg("health"));

    if (temp < 16) temp = 16;
    if (temp > 30) temp = 30;

    /* El boton refleja el campo que cambio respecto al estado anterior. */
    uint8_t button = kHaierAcYrw02ButtonPower;
    if (hasPower && power != prevPower) {
        button = kHaierAcYrw02ButtonPower;
    } else if (hasMode && mode != prevMode) {
        button = kHaierAcYrw02ButtonMode;
    } else if (hasFan && fan != prevFan) {
        button = kHaierAcYrw02ButtonFan;
    } else if (hasTemp && temp != prevTemp) {
        button = (temp > prevTemp) ? kHaierAcYrw02ButtonTempUp
                                   : kHaierAcYrw02ButtonTempDown;
    } else if (hasSwingV && swingV != prevSwingV) {
        button = kHaierAcYrw02ButtonSwingV;
    } else if (hasSwingH && swingH != prevSwingH) {
        button = kHaierAcYrw02ButtonSwingH;
    } else if ((hasTurbo && turbo != prevTurbo) || (hasQuiet && quiet != prevQuiet)) {
        button = kHaierAcYrw02ButtonTurbo;
    } else if (hasSleep && sleep != prevSleep) {
        button = kHaierAcYrw02ButtonSleep;
    } else if (hasHealth && health != prevHealth) {
        button = kHaierAcYrw02ButtonHealth;
    }

    lastState.valid = true;
    lastState.power = power;
    lastState.tempC = temp;
    lastState.mode = mode;
    lastState.fan = fan;
    lastState.swingV = swingV;
    lastState.swingH = swingH;
    lastState.turbo = turbo;
    lastState.quiet = quiet;
    lastState.sleep = sleep;
    lastState.health = health;
    lastState.button = button;

    transmitState();

    String j = "{\"ok\":true,\"state\":";
    j += stateToJSON();
    j += "}";
    server.send(200, "application/json", j);
}

void handleNotFound() {
    server.send(404, "application/json", F("{\"ok\":false,\"error\":\"not found\"}"));
}

/* ------------------------------------------------------------------
 * Serie
 * ------------------------------------------------------------------ */
void handleLine(char* line) {
    char* cmd = strtok(line, " \t");
    if (cmd == NULL) {
        return;
    }

    if (!strcmp(cmd, "h") || !strcmp(cmd, "H") || !strcmp(cmd, "help")) {
        printHelp();
    } else if (!strcmp(cmd, "status")) {
        Serial.println(stateToJSON());
    } else if (!strcmp(cmd, "on") || !strcmp(cmd, "off")) {
        lastState.valid = true;
        lastState.power = !strcmp(cmd, "on");
        lastState.button = kHaierAcYrw02ButtonPower;
        transmitState();
        Serial.print(F("Enviado: "));
        Serial.println(lastState.power ? F("ENCENDIDO") : F("APAGADO"));
    } else if (!strcmp(cmd, "set")) {
        char* t = strtok(NULL, " \t");
        char* m = strtok(NULL, " \t");
        char* f = strtok(NULL, " \t");
        if (t && m && f) {
            lastState.valid = true;
            lastState.power = true;
            lastState.tempC = (uint8_t)atoi(t);
            if (lastState.tempC < 16) lastState.tempC = 16;
            if (lastState.tempC > 30) lastState.tempC = 30;
            lastState.mode = parseModeArg(String(m));
            lastState.fan = parseFanArg(String(f));
            lastState.button = kHaierAcYrw02ButtonPower;
            transmitState();
            Serial.println(F("Configuracion enviada."));
        } else {
            Serial.println(F("Uso: set <temp> <mode> <fan>   ej: set 24 cool high"));
        }
    } else {
        Serial.print(F("Comando no reconocido: "));
        Serial.println(cmd);
        printHelp();
    }
}

void printHelp() {
    Serial.println(F("Serie: status | on | off | set <temp> <mode> <fan> | h"));
}

/* ------------------------------------------------------------------
 * IR: envio
 * ------------------------------------------------------------------ */
void transmitState() {
    ac.setPower(lastState.power);
    ac.setTemp(lastState.tempC);
    ac.setMode(lastState.mode);
    ac.setFan(lastState.fan);
    ac.setSwingV(lastState.swingV);
    ac.setSwingH(lastState.swingH);
    ac.setTurbo(lastState.turbo);
    ac.setQuiet(lastState.quiet);
    ac.setSleep(lastState.sleep);
    ac.setHealth(lastState.health);
    ac.setButton(lastState.button);

    /* Deja asentar el trafico WiFi del request antes de emitir. */
    delay(IR_SETTLE_MS);
    for (uint8_t i = 0; i < IR_REPEAT; i++) {
        ac.send();
        if (i + 1 < IR_REPEAT) {
            delay(IR_REPEAT_GAP_MS);
        }
    }
}

/* ------------------------------------------------------------------
 * Estado en JSON
 * ------------------------------------------------------------------ */
const char* modeLabel(uint8_t mode) {
    switch (mode) {
        case 0: return "auto";
        case 1: return "cool";
        case 2: return "dry";
        case 4: return "heat";
        case 6: return "fan";
        default: return NULL;
    }
}

const char* fanLabel(uint8_t fan) {
    switch (fan) {
        case 1: return "high";
        case 2: return "med";
        case 3: return "low";
        case 5: return "auto";
        default: return NULL;
    }
}

const char* swingVLabel(uint8_t pos) {
    switch (pos) {
        case 0:  return "off";
        case 1:  return "top";
        case 2:  return "middle";
        case 3:  return "bottom";
        case 0xA: return "down";
        case 0xC: return "auto";
        default: return NULL;
    }
}

const char* swingHLabel(uint8_t pos) {
    switch (pos) {
        case 0: return "middle";
        case 3: return "left_max";
        case 4: return "left";
        case 5: return "right";
        case 6: return "right_max";
        case 7: return "auto";
        default: return NULL;
    }
}

String quotedLabel(const char* label, uint8_t raw) {
    String s = "\"";
    if (label != NULL) {
        s += label;
    } else {
        s += "sin etiqueta (";
        s += raw;
        s += ")";
    }
    s += "\"";
    return s;
}

String stateToJSON() {
    if (!lastState.valid) {
        return String(F("{\"valid\":false}"));
    }
    String j = "{\"valid\":true,\"power\":";
    j += lastState.power ? "true" : "false";
    j += ",\"temp\":";
    j += lastState.tempC;
    j += ",\"mode\":";
    j += quotedLabel(modeLabel(lastState.mode), lastState.mode);
    j += ",\"mode_raw\":";
    j += lastState.mode;
    j += ",\"fan\":";
    j += quotedLabel(fanLabel(lastState.fan), lastState.fan);
    j += ",\"fan_raw\":";
    j += lastState.fan;
    j += ",\"swing_v\":";
    j += quotedLabel(swingVLabel(lastState.swingV), lastState.swingV);
    j += ",\"swing_v_raw\":";
    j += lastState.swingV;
    j += ",\"swing_h\":";
    j += quotedLabel(swingHLabel(lastState.swingH), lastState.swingH);
    j += ",\"swing_h_raw\":";
    j += lastState.swingH;
    j += ",\"turbo\":";
    j += lastState.turbo ? "true" : "false";
    j += ",\"quiet\":";
    j += lastState.quiet ? "true" : "false";
    j += ",\"sleep\":";
    j += lastState.sleep ? "true" : "false";
    j += ",\"health\":";
    j += lastState.health ? "true" : "false";
    j += "}";
    return j;
}
