/*
 * ir_remote.ino - Prueba de control remoto infrarrojo (ESP32-C3 Super Mini)
 *
 * Captura y replica comandos IR. Pensado para aire acondicionado (tramas largas)
 * y tambien para TV (NEC/Samsung/Sony/etc.). NO usa WiFi ni webserver.
 *
 * Hardware:
 *   Receptor IR (VS1838B / TSOP)  OUT -> GPIO1
 *   LED emisor IR (mejor con transistor NPN) -> GPIO3
 *   Alimentacion del receptor: 3.3V y GND (condensador de 100nF entre VCC y GND)
 *
 * GPIO8 (SDA) y GPIO9 (SCL) quedan libres para el sensor AHT10 de temperatura/humedad.
 * Se evita GPIO2 porque es pin de arranque (strapping).
 *
 * Libreria: IRremote de Armin Joachimsmeyer, version 4.x
 *
 * Comandos por monitor Serie (115200):
 *   s = reenviar el ultimo comando capturado
 *   r = reimprimir el raw (timings) del ultimo comando
 *   d = volcar el array C (uint8_t rawTicks[]) listo para pegar en el codigo
 *   h = mostrar la ayuda
 */

#include <Arduino.h>

/* ------------------------------------------------------------------
 * Configuracion de IRremote (debe ir ANTES de #include <IRremote.hpp>)
 * ------------------------------------------------------------------ */
#define RAW_BUFFER_LENGTH 750      // buffer grande para tramas largas de aire acondicionado
#define RECORD_GAP_MICROS 12000    // 12000 ayuda con aires que mandan 2 subtramas (ej. LG); por defecto seria 8000
#define IR_RECEIVE_PIN 1           // GPIO1  -> receptor IR
#define IR_SEND_PIN 3              // GPIO3  -> LED emisor IR (no usar GPIO2)
#define NO_LED_SEND_FEEDBACK_CODE    // no parpadear el LED de la placa (esta en GPIO8 = SDA) al enviar
#define NO_LED_RECEIVE_FEEDBACK_CODE // no parpadear el LED de la placa al recibir
#define NO_LED_FEEDBACK_CODE         // alias para versiones antiguas de la libreria

#include <IRremote.hpp>

#define REPEATS 2
#define IR_FREQUENCY_KHZ 38

IRData lastIRData;                     // ultimo comando decodificado
uint8_t rawCode[RAW_BUFFER_LENGTH];    // ultimo comando en formato raw (ticks de 50us)
uint16_t rawCodeLength = 0;            // numero de entradas del array raw
bool lastWasRaw = false;               // true si el ultimo comando fue raw (protocolo no reconocido)
bool hasCode = false;                  // true si ya se capturo algo

void printHelp();
void storeCode();
void sendLastCode();
void printLastRaw();

void setup() {
    Serial.begin(115200);
    delay(2000); // espera a que el monitor Serie USB del C3 este listo

    Serial.println();
    Serial.println(F("================================================"));
    Serial.println(F(" Decodificador / Emisor IR - ESP32-C3 SuperMini"));
    Serial.println(F("================================================"));

    IrReceiver.begin(IR_RECEIVE_PIN, DISABLE_LED_FEEDBACK);

    Serial.print(F("Receptor IR en GPIO"));
    Serial.print(IR_RECEIVE_PIN);
    Serial.print(F(" | Emisor IR en GPIO"));
    Serial.println(IR_SEND_PIN);
    Serial.print(F("Protocolos activos: "));
    printActiveIRProtocols(&Serial);
    Serial.println();
    printHelp();
}

void loop() {
    /* --- Recepcion IR --- */
    if (IrReceiver.decode()) {
        storeCode();
        IrReceiver.resume();
    }

    /* --- Comandos por monitor Serie --- */
    if (Serial.available() > 0) {
        char option = Serial.read();
        switch (option) {
            case 's':
            case 'S':
                sendLastCode();
                break;
            case 'r':
            case 'R':
                printLastRaw();
                break;
            case 'd':
            case 'D':
                if (hasCode) {
                    Serial.println(F("--- Array C para sendRaw (ticks de 50us) ---"));
                    IrReceiver.compensateAndPrintIRResultAsCArray(&Serial, false);
                    Serial.println();
                    Serial.println(F("--- Fin del array ---"));
                } else {
                    Serial.println(F("Aun no hay ningun comando capturado."));
                }
                break;
            case 'h':
            case 'H':
                printHelp();
                break;
            default:
                break;
        }
    }
}

void storeCode() {
    if (IrReceiver.irparams.rawlen < 4) {
        return; // ruido
    }
    if (IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT) {
        Serial.println(F("(repeat ignorado)"));
        return;
    }
    if (IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_AUTO_REPEAT) {
        Serial.println(F("(auto-repeat ignorado)"));
        return;
    }
    if (IrReceiver.decodedIRData.flags & IRDATA_FLAGS_WAS_OVERFLOW) {
        Serial.println(F("Aviso: overflow del buffer raw, sube RAW_BUFFER_LENGTH."));
        return;
    }

    lastIRData = IrReceiver.decodedIRData;
    hasCode = true;

    auto protocol = IrReceiver.decodedIRData.protocol;

    Serial.println(F("----------------------------------------------"));
    if (protocol == UNKNOWN || protocol == PULSE_WIDTH || protocol == PULSE_DISTANCE) {
        lastWasRaw = true;
        rawCodeLength = IrReceiver.irparams.rawlen - 1;
        IrReceiver.compensateAndStoreIRResultInArray(rawCode);

        Serial.println(F("Trama RAW (protocolo no reconocido)."));
        Serial.print(F("Entradas raw: "));
        Serial.println(rawCodeLength);
        IrReceiver.printIRResultRawFormatted(&Serial, true);
    } else {
        lastWasRaw = false;
        Serial.println(F("Comando decodificado:"));
        IrReceiver.printIRResultShort(&Serial);
        IrReceiver.printIRSendUsage(&Serial);
        Serial.println();
    }
    Serial.println(F("Pulsa 's' para reenviar, 'r' ver raw, 'd' array C."));
}

void sendLastCode() {
    if (!hasCode) {
        Serial.println(F("Aun no hay ningun comando capturado."));
        return;
    }

    Serial.println(F("Enviando..."));
    IrReceiver.stop(); // detener la recepcion para no capturar nuestra propia emision

    if (lastWasRaw) {
        IrSender.sendRaw(rawCode, rawCodeLength, IR_FREQUENCY_KHZ);
        Serial.print(F("sendRaw("));
        Serial.print(rawCodeLength);
        Serial.print(F(" entradas a "));
        Serial.print(IR_FREQUENCY_KHZ);
        Serial.println(F("kHz)"));
    } else {
        size_t result = IrSender.write(&lastIRData, REPEATS);
        Serial.print(F("write(protocolo="));
        Serial.print(getProtocolString(lastIRData.protocol));
        Serial.print(F(", address=0x"));
        Serial.print(lastIRData.address, HEX);
        Serial.print(F(", command=0x"));
        Serial.print(lastIRData.command, HEX);
        Serial.print(F(", repeats="));
        Serial.print(REPEATS);
        Serial.println(F(")"));
        if (result == 0) {
            Serial.println(F("Aviso: este protocolo no se puede enviar con write(); usa el modo raw."));
        }
    }

    delay(100);
    IrReceiver.start(); // reactivar la recepcion
    Serial.println(F("Enviado."));
}

void printLastRaw() {
    if (!hasCode) {
        Serial.println(F("Aun no hay ningun comando capturado."));
        return;
    }
    if (!lastWasRaw) {
        Serial.println(F("El ultimo comando se decodifico (no es raw); usa 's' para reenviarlo."));
        return;
    }
    IrReceiver.printIRResultRawFormatted(&Serial, true);
}

void printHelp() {
    Serial.println(F("Comandos: s=reenviar  r=ver raw  d=array C  h=ayuda"));
}
