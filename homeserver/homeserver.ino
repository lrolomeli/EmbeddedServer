/*
 * homeserver.ino - Servidor web con sensor AHT10 + emisor IR (ESP32-C3 Super Mini)
 *
 * Une webhum.ino (WiFi + AHT10) con el emisor IR (tramas guardadas en NVS por
 * receptor.ino). Solo reenvia comandos capturados; no decodifica.
 *
 * Hardware:
 *   AHT10  SDA -> GPIO8, SCL -> GPIO9
 *   LED emisor IR -> GPIO3
 *
 * Librerias: Adafruit AHTX0 e IRremoteESP8266 (crankyoldgit).
 *
 * API REST:
 *   GET  /api/sensors            -> temperatura y humedad
 *   GET  /api/ac/status          -> estado del ultimo comando enviado (JSON)
 *   GET  /api/ir/list            -> lista de tramas guardadas (JSON)
 *   GET|POST /api/ac/send?name=X -> envia la trama guardada X
 *   GET|POST /api/ac/on          -> envia "on"
 *   GET|POST /api/ac/off         -> envia "off"
 *
 * Comandos por monitor Serie (115200):
 *   send <nombre> / <nombre> (atajo) / nec <a> <c> / status / list / del <nombre> / h
 */

#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_AHTX0.h>

#define RAW_BUFFER_LENGTH 750

#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRutils.h>
#include <ir_Haier.h>
#include <Preferences.h>

/* --- WiFi --- */
const char* ssid = "HermososPA";
const char* password = "1823LomeliPlascencia";

/* --- Pines / IR --- */
#define IR_SEND_PIN 3
#define IR_FREQUENCY_KHZ 38

#define MAX_COMMANDS 10
#define MAX_NAME_LEN 16
#define HAIER_STATE_LEN 14

/* ------------------------------------------------------------------
 * Estados por defecto (aire York, HAIER_AC_YRW02 modelo A).
 * ------------------------------------------------------------------ */
static const uint8_t STATE_OFF[HAIER_STATE_LEN]       = {0xA6, 0xA4, 0x08, 0x00, 0x17, 0x40, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x05, 0xCE};
static const uint8_t STATE_ON[HAIER_STATE_LEN]        = {0xA6, 0xA4, 0x08, 0x00, 0x57, 0x40, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x05, 0x0E};
static const uint8_t STATE_TEMP_UP[HAIER_STATE_LEN]   = {0xA6, 0xB4, 0x08, 0x00, 0x5B, 0x40, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1D};
static const uint8_t STATE_TEMP_DOWN[HAIER_STATE_LEN] = {0xA6, 0xA4, 0x08, 0x00, 0x5D, 0x40, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x01, 0x10};
static const uint8_t STATE_FAN[HAIER_STATE_LEN]       = {0xA6, 0xA4, 0x08, 0x00, 0x5F, 0x20, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x04, 0xF5};
static const uint8_t STATE_SWING[HAIER_STATE_LEN]     = {0xA6, 0xA6, 0x08, 0x00, 0x60, 0x20, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x02, 0xF6};

struct AcDefault {
    const char* name;
    const uint8_t* state;
};
static const AcDefault AC_DEFAULTS[] = {
    { "off",       STATE_OFF       },
    { "on",        STATE_ON        },
    { "temp_up",   STATE_TEMP_UP   },
    { "temp_down", STATE_TEMP_DOWN },
    { "fan",       STATE_FAN       },
    { "swing",     STATE_SWING     },
};
static const uint8_t AC_DEFAULTS_COUNT = sizeof(AC_DEFAULTS) / sizeof(AC_DEFAULTS[0]);

struct NecDefault {
    const char* name;
    uint8_t addr;
    uint8_t cmd;
};
static const NecDefault NEC_DEFAULTS[] = {
    { "nec_50_17", 0x50, 0x17 },
};
static const uint8_t NEC_DEFAULTS_COUNT = sizeof(NEC_DEFAULTS) / sizeof(NEC_DEFAULTS[0]);

/* ------------------------------------------------------------------
 * Estado y almacen de comandos
 * ------------------------------------------------------------------ */
struct AcState {
    bool valid;
    bool power;
    bool turbo;
    bool quiet;
    bool sleep;
    bool health;
    bool lock;
    bool fahrenheit;
    uint8_t tempC;
    uint8_t mode;
    uint8_t fan;
    uint8_t swingV;
    uint8_t swingH;
    uint8_t button;
    uint16_t onTimerMin;
    uint16_t offTimerMin;
};

struct IRCommand {
    char name[MAX_NAME_LEN];
    uint16_t data[RAW_BUFFER_LENGTH];
    uint16_t len;
    uint8_t state[HAIER_STATE_LEN];
    bool hasState;
    bool used;
};

IRCommand commands[MAX_COMMANDS];
Preferences prefs;
IRsend irsend(IR_SEND_PIN);

uint8_t lastStateBytes[HAIER_STATE_LEN];
bool lastHasState = false;

AcState lastState = { false, false, false, false, false, false, false, false,
                      0, 0, 0, 0, 0, 0, 0, 0 };

WebServer server(80);
Adafruit_AHTX0 aht;
bool ahtOk = false;

char lineBuf[64];
uint8_t lineLen = 0;

/* ------------------------------------------------------------------
 * Prototipos
 * ------------------------------------------------------------------ */
void updateStateFromBytes(const uint8_t* st);
bool sendByName(const char* name);
void sendNec(uint8_t addr, uint8_t cmd);
void deleteCommand(const char* name);
void listCommands();
void loadCommands();
void persistCommands();
void preloadDefaults();
void ensureDefaults();
void installAcCommand(int slot, const char* name, const uint8_t* st);
void installNecCommand(int slot, const char* name, uint8_t addr, uint8_t cmd);
uint16_t buildYrw02Raw(const uint8_t* st, uint16_t* out);
uint16_t buildNecRaw(uint32_t data, uint16_t* out);
int findCommand(const char* name);
int firstFreeSlot();

const char* modeLabel(uint8_t mode);
const char* fanLabel(uint8_t fan);
const char* swingVLabel(uint8_t pos);
const char* swingHLabel(uint8_t pos);
String quotedLabel(const char* label, uint8_t raw);
String stateToJSON();
String commandListJSON();

void handleSensors();
void handleAcStatus();
void handleAcSend();
void handleAcOn();
void handleAcOff();
void handleIrList();
void handleNotFound();
void handleLine(char* line);
void printHelp();

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println(F("================================================"));
    Serial.println(F(" Homeserver: AHT10 + Emisor IR - ESP32-C3"));
    Serial.println(F("================================================"));

    irsend.begin();

    prefs.begin("irremote", false);
    loadCommands();
    ensureDefaults();

    if (!aht.begin()) {
        Serial.println(F("AHT10 no encontrado (el endpoint /api/sensors devolvera error)."));
        ahtOk = false;
    } else {
        ahtOk = true;
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    Serial.print(F("Conectando a WiFi"));
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print('.');
    }
    Serial.println();
    Serial.print(F("Conectado. IP: "));
    Serial.println(WiFi.localIP());

    server.on("/api/sensors", HTTP_GET, handleSensors);
    server.on("/api/ac/status", HTTP_GET, handleAcStatus);
    server.on("/api/ir/list", HTTP_GET, handleIrList);
    server.on("/api/ac/send", HTTP_GET, handleAcSend);
    server.on("/api/ac/send", HTTP_POST, handleAcSend);
    server.on("/api/ac/on", HTTP_GET, handleAcOn);
    server.on("/api/ac/on", HTTP_POST, handleAcOn);
    server.on("/api/ac/off", HTTP_GET, handleAcOff);
    server.on("/api/ac/off", HTTP_POST, handleAcOff);
    server.onNotFound(handleNotFound);

    server.begin();
    Serial.println(F("Servidor web iniciado."));
    printHelp();
    listCommands();
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

void handleIrList() {
    server.send(200, "application/json", commandListJSON());
}

void handleAcSend() {
    if (!server.hasArg("name")) {
        server.send(400, "application/json",
                    F("{\"ok\":false,\"error\":\"falta parametro name\"}"));
        return;
    }
    String name = server.arg("name");
    bool ok = sendByName(name.c_str());
    String j = "{\"ok\":";
    j += ok ? "true" : "false";
    j += ",\"name\":\"";
    j += name;
    j += "\"}";
    server.send(ok ? 200 : 404, "application/json", j);
}

void handleAcOn() {
    sendByName("on");
    server.send(200, "application/json", F("{\"ok\":true,\"name\":\"on\"}"));
}

void handleAcOff() {
    sendByName("off");
    server.send(200, "application/json", F("{\"ok\":true,\"name\":\"off\"}"));
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
    } else if (!strcmp(cmd, "list")) {
        listCommands();
    } else if (!strcmp(cmd, "send")) {
        char* arg = strtok(NULL, " \t");
        if (arg) sendByName(arg); else Serial.println(F("Uso: send <nombre>"));
    } else if (!strcmp(cmd, "del")) {
        char* arg = strtok(NULL, " \t");
        if (arg) deleteCommand(arg); else Serial.println(F("Uso: del <nombre>"));
    } else if (!strcmp(cmd, "nec")) {
        char* a = strtok(NULL, " \t");
        char* b = strtok(NULL, " \t");
        if (a && b) {
            sendNec((uint8_t)strtoul(a, NULL, 16), (uint8_t)strtoul(b, NULL, 16));
        } else {
            Serial.println(F("Uso: nec <addr_hex> <cmd_hex>   ej: nec 50 17"));
        }
    } else if (findCommand(cmd) >= 0) {
        sendByName(cmd);
    } else {
        Serial.print(F("Comando no reconocido: "));
        Serial.println(cmd);
        printHelp();
    }
}

void printHelp() {
    Serial.println(F("Serie: send <nombre> | <nombre> | nec <a> <c> | status | list | del <nombre> | h"));
}

/* ------------------------------------------------------------------
 * IR: envio y estado
 * ------------------------------------------------------------------ */
bool sendByName(const char* name) {
    int idx = findCommand(name);
    if (idx < 0) {
        Serial.print(F("No existe la trama: "));
        Serial.println(name);
        return false;
    }
    Serial.print(F("Enviando '"));
    Serial.print(commands[idx].name);
    Serial.print(F("' ("));
    Serial.print(commands[idx].len);
    Serial.println(F(" entradas)..."));
    irsend.sendRaw(commands[idx].data, commands[idx].len, IR_FREQUENCY_KHZ);
    if (commands[idx].hasState) {
        memcpy(lastStateBytes, commands[idx].state, HAIER_STATE_LEN);
        lastHasState = true;
        updateStateFromBytes(lastStateBytes);
    }
    return true;
}

void sendNec(uint8_t addr, uint8_t cmd) {
    static uint16_t raw[RAW_BUFFER_LENGTH];
    uint32_t data = ((uint32_t)(addr) & 0xFF)
                  | (((uint32_t)(~addr) & 0xFF) << 8)
                  | (((uint32_t)(cmd) & 0xFF) << 16)
                  | (((uint32_t)(~cmd) & 0xFF) << 24);

    uint16_t len = buildNecRaw(data, raw);
    lastHasState = false;
    lastState.valid = false;

    Serial.print(F("Enviando NEC addr=0x"));
    Serial.print(addr, HEX);
    Serial.print(F(" cmd=0x"));
    Serial.println(cmd, HEX);
    irsend.sendRaw(raw, len, IR_FREQUENCY_KHZ);
}

void updateStateFromBytes(const uint8_t* st) {
    IRHaierACYRW02 ac(IR_SEND_PIN);
    ac.setRaw(st);

    lastState.valid = true;
    lastState.power = ac.getPower();
    lastState.tempC = ac.getTemp();
    lastState.mode = ac.getMode();
    lastState.fan = ac.getFan();
    lastState.swingV = ac.getSwingV();
    lastState.swingH = ac.getSwingH();
    lastState.turbo = ac.getTurbo();
    lastState.quiet = ac.getQuiet();
    lastState.sleep = ac.getSleep();
    lastState.health = ac.getHealth();
    lastState.lock = ac.getLock();
    lastState.fahrenheit = ac.getUseFahrenheit();
    lastState.onTimerMin = ac.getOnTimer();
    lastState.offTimerMin = ac.getOffTimer();
    lastState.button = ac.getButton();
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

String commandListJSON() {
    String j = "[";
    bool first = true;
    for (int i = 0; i < MAX_COMMANDS; i++) {
        if (!commands[i].used) {
            continue;
        }
        if (!first) {
            j += ",";
        }
        first = false;
        j += "{\"name\":\"";
        j += commands[i].name;
        j += "\",\"len\":";
        j += commands[i].len;
        j += ",\"hasState\":";
        j += commands[i].hasState ? "true" : "false";
        j += "}";
    }
    j += "]";
    return j;
}

/* ------------------------------------------------------------------
 * Almacen de comandos (NVS, compacto)
 * ------------------------------------------------------------------ */
int findCommand(const char* name) {
    for (int i = 0; i < MAX_COMMANDS; i++) {
        if (commands[i].used && !strcmp(commands[i].name, name)) {
            return i;
        }
    }
    return -1;
}

int firstFreeSlot() {
    for (int i = 0; i < MAX_COMMANDS; i++) {
        if (!commands[i].used) {
            return i;
        }
    }
    return -1;
}

uint16_t buildYrw02Raw(const uint8_t* st, uint16_t* out) {
    uint16_t i = 0;
    out[i++] = 3000;
    out[i++] = 3000;
    out[i++] = 3000;
    out[i++] = 4300;
    for (uint8_t b = 0; b < HAIER_STATE_LEN; b++) {
        for (int8_t bit = 7; bit >= 0; bit--) {
            out[i++] = 550;
            out[i++] = ((st[b] >> bit) & 1) ? 1650 : 550;
        }
    }
    out[i++] = 550;
    return i;
}

uint16_t buildNecRaw(uint32_t data, uint16_t* out) {
    uint16_t i = 0;
    out[i++] = 9000;
    out[i++] = 4500;
    for (uint8_t b = 0; b < 32; b++) {
        out[i++] = 560;
        out[i++] = ((data >> b) & 1) ? 1690 : 560;
    }
    out[i++] = 560;
    return i;
}

void installAcCommand(int slot, const char* name, const uint8_t* st) {
    strncpy(commands[slot].name, name, MAX_NAME_LEN - 1);
    commands[slot].name[MAX_NAME_LEN - 1] = '\0';
    memcpy(commands[slot].state, st, HAIER_STATE_LEN);
    commands[slot].hasState = true;
    commands[slot].len = buildYrw02Raw(st, commands[slot].data);
    commands[slot].used = true;
}

void installNecCommand(int slot, const char* name, uint8_t addr, uint8_t cmd) {
    uint32_t data = ((uint32_t)(addr) & 0xFF)
                  | (((uint32_t)(~addr) & 0xFF) << 8)
                  | (((uint32_t)(cmd) & 0xFF) << 16)
                  | (((uint32_t)(~cmd) & 0xFF) << 24);
    strncpy(commands[slot].name, name, MAX_NAME_LEN - 1);
    commands[slot].name[MAX_NAME_LEN - 1] = '\0';
    commands[slot].hasState = false;
    commands[slot].len = buildNecRaw(data, commands[slot].data);
    commands[slot].used = true;
}

void preloadDefaults() {
    memset(commands, 0, sizeof(commands));

    int slot = 0;
    for (uint8_t i = 0; i < AC_DEFAULTS_COUNT && slot < MAX_COMMANDS; i++) {
        installAcCommand(slot++, AC_DEFAULTS[i].name, AC_DEFAULTS[i].state);
    }
    for (uint8_t i = 0; i < NEC_DEFAULTS_COUNT && slot < MAX_COMMANDS; i++) {
        installNecCommand(slot++, NEC_DEFAULTS[i].name, NEC_DEFAULTS[i].addr, NEC_DEFAULTS[i].cmd);
    }

    persistCommands();
    Serial.println(F("Tramas por defecto cargadas."));
}

void ensureDefaults() {
    bool changed = false;

    for (uint8_t i = 0; i < AC_DEFAULTS_COUNT; i++) {
        if (findCommand(AC_DEFAULTS[i].name) < 0) {
            int slot = firstFreeSlot();
            if (slot < 0) break;
            installAcCommand(slot, AC_DEFAULTS[i].name, AC_DEFAULTS[i].state);
            changed = true;
        }
    }
    for (uint8_t i = 0; i < NEC_DEFAULTS_COUNT; i++) {
        if (findCommand(NEC_DEFAULTS[i].name) < 0) {
            int slot = firstFreeSlot();
            if (slot < 0) break;
            installNecCommand(slot, NEC_DEFAULTS[i].name, NEC_DEFAULTS[i].addr, NEC_DEFAULTS[i].cmd);
            changed = true;
        }
    }

    if (changed) {
        persistCommands();
        Serial.println(F("Defaults faltantes agregados."));
    }
}

void persistCommands() {
    size_t total = 1;
    for (int i = 0; i < MAX_COMMANDS; i++) {
        if (commands[i].used) {
            total += MAX_NAME_LEN + sizeof(uint16_t) + 1 + HAIER_STATE_LEN +
                     (commands[i].len * sizeof(uint16_t));
        }
    }

    uint8_t* buf = (uint8_t*)malloc(total);
    if (buf == NULL) {
        Serial.println(F("Sin RAM para persistir comandos."));
        return;
    }

    size_t p = 1;
    uint8_t count = 0;
    for (int i = 0; i < MAX_COMMANDS; i++) {
        if (!commands[i].used) {
            continue;
        }
        count++;
        memcpy(buf + p, commands[i].name, MAX_NAME_LEN);
        p += MAX_NAME_LEN;
        memcpy(buf + p, &commands[i].len, sizeof(uint16_t));
        p += sizeof(uint16_t);
        buf[p++] = commands[i].hasState ? 1 : 0;
        memcpy(buf + p, commands[i].state, HAIER_STATE_LEN);
        p += HAIER_STATE_LEN;
        memcpy(buf + p, commands[i].data, commands[i].len * sizeof(uint16_t));
        p += commands[i].len * sizeof(uint16_t);
    }
    buf[0] = count;

    prefs.putBytes("cmds", buf, total);
    free(buf);
}

void loadCommands() {
    memset(commands, 0, sizeof(commands));

    size_t n = prefs.getBytesLength("cmds");
    if (n < 2) {
        preloadDefaults();
        return;
    }

    uint8_t* buf = (uint8_t*)malloc(n);
    if (buf == NULL) {
        preloadDefaults();
        return;
    }
    if (prefs.getBytes("cmds", buf, n) != n) {
        free(buf);
        preloadDefaults();
        return;
    }

    bool ok = true;
    size_t p = 1;
    uint8_t count = buf[0];
    for (uint8_t c = 0; c < count && ok; c++) {
        if (c >= MAX_COMMANDS) {
            ok = false;
            break;
        }
        if (p + MAX_NAME_LEN + sizeof(uint16_t) + 1 + HAIER_STATE_LEN > n) {
            ok = false;
            break;
        }
        memcpy(commands[c].name, buf + p, MAX_NAME_LEN);
        p += MAX_NAME_LEN;
        uint16_t len = 0;
        memcpy(&len, buf + p, sizeof(uint16_t));
        p += sizeof(uint16_t);
        bool hasState = buf[p++] != 0;
        memcpy(commands[c].state, buf + p, HAIER_STATE_LEN);
        p += HAIER_STATE_LEN;
        if (len > RAW_BUFFER_LENGTH || p + (len * sizeof(uint16_t)) > n) {
            ok = false;
            break;
        }
        memcpy(commands[c].data, buf + p, len * sizeof(uint16_t));
        p += len * sizeof(uint16_t);
        commands[c].len = len;
        commands[c].hasState = hasState;
        commands[c].used = true;
    }
    free(buf);

    if (!ok) {
        preloadDefaults();
    }
}

void listCommands() {
    Serial.println(F("--- Tramas guardadas ---"));
    bool any = false;
    for (int i = 0; i < MAX_COMMANDS; i++) {
        if (commands[i].used) {
            any = true;
            Serial.print(F("  "));
            Serial.print(commands[i].name);
            Serial.print(F(" ("));
            Serial.print(commands[i].len);
            Serial.print(F(" entradas"));
            Serial.println(commands[i].hasState ? F(", con estado)") : F(")"));
        }
    }
    if (!any) {
        Serial.println(F("  (ninguna)"));
    }
    Serial.println(F("-----------------------"));
}

void deleteCommand(const char* name) {
    int idx = findCommand(name);
    if (idx < 0) {
        Serial.print(F("No existe la trama: "));
        Serial.println(name);
        return;
    }
    commands[idx].used = false;
    commands[idx].name[0] = '\0';
    commands[idx].len = 0;
    commands[idx].hasState = false;
    persistCommands();
    Serial.print(F("Trama '"));
    Serial.print(name);
    Serial.println(F("' borrada."));
}
