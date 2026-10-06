/*
 * ir_remote_test.ino - Control Remoto IR (AC Haier/York)
 * Hardware: ESP32-C3 Super Mini
 *
 * Controla el aire acondicionado Haier/York desde la consola serie (115200).
 * Protocolo: HAIER_AC_YRW02.
 */

#include <Arduino.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <ir_Haier.h>

#define IR_SEND_PIN 3 // GPIO3 -> LED Emisor IR

// Instancia persistente para el Aire Acondicionado Haier
IRHaierACYRW02 ac(IR_SEND_PIN);

char lineBuf[64];
uint8_t lineLen = 0;

/* --- Prototipos de Funciones --- */
void handleLine(char* line);
void printHelp();
void printStateJSON();
void setModeByString(const char* modeStr);
void setFanByString(const char* fanStr);

void setup() {
    Serial.begin(115200);
    delay(2000);

    Serial.println(F("\n================================================"));
    Serial.println(F(" Control HAIER_AC_YRW02 - ESP32-C3 SuperMini"));
    Serial.println(F("================================================"));

    ac.begin();

    Serial.print(F("Emisor IR activo en GPIO: "));
    Serial.println(IR_SEND_PIN);

    printHelp();
}

void loop() {
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
 * PARSER DE COMANDOS Y CONTROL DE ESTADOS
 * ------------------------------------------------------------------ */
void handleLine(char* line) {
    char* cmd = strtok(line, " \t");
    if (cmd == NULL) return;

    if (!strcmp(cmd, "off")) {
        ac.setPower(false);
        ac.setButton(kHaierAcYrw02ButtonPower);
        ac.send();
        Serial.println(F("-> AC: APAGADO"));

    } else if (!strcmp(cmd, "on")) {
        ac.setPower(true);
        ac.setButton(kHaierAcYrw02ButtonPower);
        ac.send();
        Serial.println(F("-> AC: ENCENDIDO"));

    } else if (!strcmp(cmd, "temp")) {
        char* arg = strtok(NULL, " \t");
        if (arg) {
            uint8_t t = atoi(arg);
            if (t >= 16 && t <= 30) {
                ac.setTemp(t);
                ac.send();
                Serial.print(F("-> AC Temp: "));
                Serial.print(t);
                Serial.println(F("°C"));
            } else {
                Serial.println(F("Error: Temperatura fuera de rango (16 - 30)"));
            }
        } else {
            Serial.println(F("Uso: temp <16-30>"));
        }

    } else if (!strcmp(cmd, "mode")) {
        char* arg = strtok(NULL, " \t");
        if (arg) {
            setModeByString(arg);
            ac.send();
        } else {
            Serial.println(F("Uso: mode <cool|heat|dry|fan|auto>"));
        }

    } else if (!strcmp(cmd, "fan")) {
        char* arg = strtok(NULL, " \t");
        if (arg) {
            setFanByString(arg);
            ac.send();
        } else {
            Serial.println(F("Uso: fan <low|med|high|auto>"));
        }

    } else if (!strcmp(cmd, "swing")) {
        char* arg = strtok(NULL, " \t");
        if (arg) {
            if (!strcmp(arg, "off"))      ac.setSwing(kHaierAcYrw02SwingVOff);
            else if (!strcmp(arg, "top")) ac.setSwing(kHaierAcYrw02SwingVTop);
            else if (!strcmp(arg, "mid")) ac.setSwing(kHaierAcYrw02SwingVMiddle);
            else if (!strcmp(arg, "bot")) ac.setSwing(kHaierAcYrw02SwingVBottom);
            else if (!strcmp(arg, "auto"))ac.setSwing(kHaierAcYrw02SwingVDown);
            else {
                Serial.println(F("Opciones: off, top, mid, bot, auto"));
                return;
            }
            ac.send();
            Serial.print(F("-> AC Swing: "));
            Serial.println(arg);
        } else {
            Serial.println(F("Uso: swing <off|top|mid|bot|auto>"));
        }

    } else if (!strcmp(cmd, "turbo")) {
        ac.setHealth(false);
        ac.setTurbo(true);
        ac.send();
        Serial.println(F("-> AC: TURBO activado"));

    } else if (!strcmp(cmd, "quiet")) {
        ac.setTurbo(false);
        ac.setHealth(true);
        ac.send();
        Serial.println(F("-> AC: SILENCIOSO activado"));

    } else if (!strcmp(cmd, "normal")) {
        ac.setTurbo(false);
        ac.setHealth(false);
        ac.send();
        Serial.println(F("-> AC: Modo NORMAL activado"));

    } else if (!strcmp(cmd, "set")) {
        char* tempArg = strtok(NULL, " \t");
        char* modeArg = strtok(NULL, " \t");
        char* fanArg  = strtok(NULL, " \t");

        if (tempArg && modeArg && fanArg) {
            ac.setPower(true);
            ac.setTemp(atoi(tempArg));
            setModeByString(modeArg);
            setFanByString(fanArg);
            ac.setButton(kHaierAcYrw02ButtonPower);
            ac.send();
            Serial.println(F("-> AC: Configuración enviada."));
        } else {
            Serial.println(F("Uso: set <temp> <mode> <fan> (ej: set 24 cool high)"));
        }

    } else if (!strcmp(cmd, "status")) {
        printStateJSON();

    } else if (!strcmp(cmd, "h") || !strcmp(cmd, "help")) {
        printHelp();

    } else {
        Serial.print(F("Comando no reconocido: "));
        Serial.println(cmd);
    }
}

/* ------------------------------------------------------------------
 * MÉTODOS DE AYUDA Y TRADUCCIÓN
 * ------------------------------------------------------------------ */
void setModeByString(const char* modeStr) {
    if (!strcmp(modeStr, "cool"))      ac.setMode(kHaierAcYrw02Cool);
    else if (!strcmp(modeStr, "heat")) ac.setMode(kHaierAcYrw02Heat);
    else if (!strcmp(modeStr, "dry"))  ac.setMode(kHaierAcYrw02Dry);
    else if (!strcmp(modeStr, "fan"))  ac.setMode(kHaierAcYrw02Fan);
    else if (!strcmp(modeStr, "auto")) ac.setMode(kHaierAcYrw02Auto);
}

void setFanByString(const char* fanStr) {
    if (!strcmp(fanStr, "low"))       ac.setFan(kHaierAcYrw02FanLow);
    else if (!strcmp(fanStr, "med"))  ac.setFan(kHaierAcYrw02FanMed);
    else if (!strcmp(fanStr, "high")) ac.setFan(kHaierAcYrw02FanHigh);
    else if (!strcmp(fanStr, "auto")) ac.setFan(kHaierAcYrw02FanAuto);
}

void printStateJSON() {
    Serial.print(F("{\"valid\":true,\"power\":"));
    Serial.print(ac.getPower() ? F("true") : F("false"));
    Serial.print(F(",\"temp\":"));
    Serial.print(ac.getTemp());
    Serial.print(F(",\"mode\":"));
    Serial.print(ac.getMode());
    Serial.print(F(",\"fan\":"));
    Serial.print(ac.getFan());
    Serial.print(F(",\"swing\":"));
    Serial.print(ac.getSwing());
    Serial.println(F("}"));
}

void printHelp() {
    Serial.println(F("\n--- Comandos AC Haier ---"));
    Serial.println(F("  on / off               -> Encender / Apagar el clima"));
    Serial.println(F("  temp <16-30>           -> Ajustar temperatura"));
    Serial.println(F("  mode <modo>            -> cool | heat | dry | fan | auto"));
    Serial.println(F("  fan <vel>              -> low | med | high | auto"));
    Serial.println(F("  swing <pos>            -> off | top | mid | bot | auto"));
    Serial.println(F("  turbo / quiet / normal -> Modos especiales"));
    Serial.println(F("  set <temp> <m> <f>     -> Configuración rápida (ej: set 24 cool high)"));
    Serial.println(F("  status / help          -> Estado del clima / Mensaje de ayuda"));
    Serial.println(F("-------------------------\n"));
}
