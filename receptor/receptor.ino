/*
 * receptor.ino - Receptor/decodificador IR (ESP32-C3 Super Mini)
 *
 * Herramienta de aprendijaze: recibe IR, decodifica el aire York
 * (HAIER_AC_YRW02) o protocolos estandar (NEC, etc.), muestra el estado en JSON
 * y guarda tramas con nombre en NVS para que las use el transmisor.
 *
 * Hardware:
 *   Receptor IR (VS1838B / TSOP) OUT -> GPIO1
 *   (no usa emisor)
 *
 * Libreria: IRremoteESP8266 de crankyoldgit. NO convive con "IRremote".
 *
 * Comandos por monitor Serie (115200):
 *   r                = reimprimir el raw (microsegundos) del ultimo comando
 *   d                = volcar el array C (uint16_t, microsegundos)
 *   status           = mostrar en JSON el ultimo estado decodificado
 *   save <nombre>    = guardar el ultimo comando con ese nombre (NVS)
 *   list             = listar las tramas guardadas
 *   del <nombre>     = borrar una trama guardada
 *   h                = ayuda
 */

#include <Arduino.h>

#define RAW_BUFFER_LENGTH 750

#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>
#include <ir_Haier.h>
#include <Preferences.h>

#define IR_RECEIVE_PIN 1
#define IR_SEND_PIN 3              // no se usa para emitir, solo para parsear el estado

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

IRrecv irrecv(IR_RECEIVE_PIN);
decode_results results;

uint16_t lastRaw[RAW_BUFFER_LENGTH];
uint16_t lastRawLen = 0;
uint8_t lastStateBytes[HAIER_STATE_LEN];
bool lastHasState = false;
bool hasCode = false;

AcState lastState = { false, false, false, false, false, false, false, false,
                      0, 0, 0, 0, 0, 0, 0, 0 };

char lineBuf[64];
uint8_t lineLen = 0;

/* ------------------------------------------------------------------
 * Prototipos
 * ------------------------------------------------------------------ */
void handleCapture();
void updateStateFromBytes(const uint8_t* st);
void printRawFormatted();
void dumpArray();
void handleLine(char* line);
void printHelp();
void printStateJSON();
void saveCommand(const char* name);
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
void printQuotedLabel(const char* label, uint8_t raw);

void setup() {
    Serial.begin(115200);
    delay(2000);

    Serial.println();
    Serial.println(F("================================================"));
    Serial.println(F(" Receptor IR / aprendizaje - ESP32-C3 SuperMini"));
    Serial.println(F("================================================"));

    irrecv.enableIRIn();

    prefs.begin("irremote", false);
    loadCommands();
    ensureDefaults();

    Serial.print(F("Receptor IR en GPIO"));
    Serial.println(IR_RECEIVE_PIN);
    printHelp();
    listCommands();
}

void loop() {
    if (irrecv.decode(&results)) {
        handleCapture();
        irrecv.resume();
    }

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

void handleCapture() {
    hasCode = true;

    uint16_t corrected = getCorrectedRawLength(&results);
    uint16_t* raw = resultToRawArray(&results);
    if (raw != NULL && corrected > 0 && corrected <= RAW_BUFFER_LENGTH) {
        memcpy(lastRaw, raw, corrected * sizeof(uint16_t));
        lastRawLen = corrected;
    } else {
        lastRawLen = 0;
    }
    if (raw != NULL) {
        free(raw);
    }

    Serial.println(F("----------------------------------------------"));

    if (results.decode_type == decode_type_t::HAIER_AC_YRW02) {
        memcpy(lastStateBytes, results.state, HAIER_STATE_LEN);
        lastHasState = true;
        updateStateFromBytes(lastStateBytes);
        Serial.println(F("Comando decodificado: HAIER_AC_YRW02"));
        printStateJSON();
    } else {
        lastHasState = false;
        lastState.valid = false;
        if (results.decode_type == decode_type_t::UNKNOWN) {
            Serial.println(F("Trama RAW (protocolo no reconocido)."));
            Serial.print(F("Entradas raw: "));
            Serial.println(lastRawLen);
        } else {
            Serial.print(F("Comando decodificado: "));
            Serial.println(typeToString(results.decode_type));
            Serial.print(F("  Value=0x"));
            Serial.print((uint32_t)results.value, HEX);
            Serial.print(F(", Address=0x"));
            Serial.print(results.address, HEX);
            Serial.print(F(", Command=0x"));
            Serial.println(results.command, HEX);
        }
    }

    Serial.println(F("status=estado JSON, r=raw, d=array C, save <nombre>=guardar."));
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

void printRawFormatted() {
    if (lastRawLen == 0) {
        Serial.println(F("No hay raw disponible."));
        return;
    }
    Serial.print(F("rawIRTimings["));
    Serial.print(lastRawLen);
    Serial.println(F("]:"));
    for (uint16_t i = 0; i < lastRawLen; i++) {
        Serial.print((i % 2 == 0) ? '+' : '-');
        Serial.print(lastRaw[i]);
        if (i % 2 == 1) {
            Serial.println();
        } else {
            Serial.print(',');
        }
    }
    if (lastRawLen % 2 == 1) {
        Serial.println();
    }
}

void dumpArray() {
    if (lastRawLen == 0) {
        Serial.println(F("No hay raw disponible."));
        return;
    }
    Serial.println(F("--- Array C para sendRaw (microsegundos) ---"));
    Serial.print(F("const uint16_t rawData[] = {"));
    for (uint16_t i = 0; i < lastRawLen; i++) {
        if (i % 12 == 0) {
            Serial.println();
        }
        Serial.print(lastRaw[i]);
        if (i < lastRawLen - 1) {
            Serial.print(F(", "));
        }
    }
    Serial.println();
    Serial.println(F("};"));
    Serial.println(F("--- Fin del array ---"));
}

void handleLine(char* line) {
    char* cmd = strtok(line, " \t");
    if (cmd == NULL) {
        return;
    }

    if (!strcmp(cmd, "r") || !strcmp(cmd, "R")) {
        printRawFormatted();
    } else if (!strcmp(cmd, "d") || !strcmp(cmd, "D")) {
        dumpArray();
    } else if (!strcmp(cmd, "status")) {
        printStateJSON();
    } else if (!strcmp(cmd, "h") || !strcmp(cmd, "H") || !strcmp(cmd, "help")) {
        printHelp();
    } else if (!strcmp(cmd, "list")) {
        listCommands();
    } else if (!strcmp(cmd, "save")) {
        char* arg = strtok(NULL, " \t");
        if (arg) saveCommand(arg); else Serial.println(F("Uso: save <nombre>"));
    } else if (!strcmp(cmd, "del")) {
        char* arg = strtok(NULL, " \t");
        if (arg) deleteCommand(arg); else Serial.println(F("Uso: del <nombre>"));
    } else {
        Serial.print(F("Comando no reconocido: "));
        Serial.println(cmd);
        printHelp();
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

void printQuotedLabel(const char* label, uint8_t raw) {
    Serial.print('"');
    if (label != NULL) {
        Serial.print(label);
    } else {
        Serial.print(F("sin etiqueta ("));
        Serial.print(raw);
        Serial.print(')');
    }
    Serial.print('"');
}

void printStateJSON() {
    if (!lastState.valid) {
        Serial.println(F("{\"valid\":false}"));
        return;
    }
    Serial.print(F("{\"valid\":true,\"power\":"));
    Serial.print(lastState.power ? F("true") : F("false"));
    Serial.print(F(",\"temp\":"));
    Serial.print(lastState.tempC);
    Serial.print(F(",\"mode\":"));
    printQuotedLabel(modeLabel(lastState.mode), lastState.mode);
    Serial.print(F(",\"mode_raw\":"));
    Serial.print(lastState.mode);
    Serial.print(F(",\"fan\":"));
    printQuotedLabel(fanLabel(lastState.fan), lastState.fan);
    Serial.print(F(",\"fan_raw\":"));
    Serial.print(lastState.fan);
    Serial.print(F(",\"swing_v\":"));
    printQuotedLabel(swingVLabel(lastState.swingV), lastState.swingV);
    Serial.print(F(",\"swing_v_raw\":"));
    Serial.print(lastState.swingV);
    Serial.print(F(",\"swing_h\":"));
    printQuotedLabel(swingHLabel(lastState.swingH), lastState.swingH);
    Serial.print(F(",\"swing_h_raw\":"));
    Serial.print(lastState.swingH);
    Serial.print(F(",\"turbo\":"));
    Serial.print(lastState.turbo ? F("true") : F("false"));
    Serial.print(F(",\"quiet\":"));
    Serial.print(lastState.quiet ? F("true") : F("false"));
    Serial.print(F(",\"sleep\":"));
    Serial.print(lastState.sleep ? F("true") : F("false"));
    Serial.print(F(",\"health\":"));
    Serial.print(lastState.health ? F("true") : F("false"));
    Serial.println(F("}"));
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

void saveCommand(const char* name) {
    if (!hasCode || lastRawLen == 0) {
        Serial.println(F("No hay una trama capturada para guardar."));
        return;
    }
    if (strlen(name) == 0 || strlen(name) >= MAX_NAME_LEN) {
        Serial.print(F("Nombre invalido (max "));
        Serial.print(MAX_NAME_LEN - 1);
        Serial.println(F(" caracteres)."));
        return;
    }

    int idx = findCommand(name);
    bool overwrite = (idx >= 0);
    if (!overwrite) {
        idx = firstFreeSlot();
    }
    if (idx < 0) {
        Serial.println(F("No hay espacio para mas tramas (usa 'del <nombre>')."));
        return;
    }

    strncpy(commands[idx].name, name, MAX_NAME_LEN - 1);
    commands[idx].name[MAX_NAME_LEN - 1] = '\0';
    memcpy(commands[idx].data, lastRaw, lastRawLen * sizeof(uint16_t));
    commands[idx].len = lastRawLen;
    commands[idx].hasState = lastHasState;
    if (lastHasState) {
        memcpy(commands[idx].state, lastStateBytes, HAIER_STATE_LEN);
    }
    commands[idx].used = true;
    persistCommands();

    Serial.print(F("Trama '"));
    Serial.print(commands[idx].name);
    Serial.print(F("' guardada ("));
    Serial.print(commands[idx].len);
    Serial.println(overwrite ? F(" entradas, reemplazada).") : F(" entradas)."));
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

void printHelp() {
    Serial.println(F("Comandos:"));
    Serial.println(F("  r                ver raw del ultimo"));
    Serial.println(F("  d                array C (uint16_t us) del ultimo"));
    Serial.println(F("  status           estado del aire en JSON"));
    Serial.println(F("  save <nombre>    guardar ultimo comando"));
    Serial.println(F("  list             listar tramas guardadas"));
    Serial.println(F("  del <nombre>     borrar trama guardada"));
    Serial.println(F("  h                ayuda"));
}
