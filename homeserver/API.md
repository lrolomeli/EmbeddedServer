# API del Homeserver (ESP32-C3)

Servidor HTTP en la red local que expone el sensor AHT10 y el control del aire
acondicionado York (`HAIER_AC_YRW02`).

- **Base URL:** `http://<IP-del-ESP32>` (la IP se imprime por el monitor Serie al conectar WiFi).
- **Puerto:** `80`
- **Formato:** JSON UTF-8 (`Content-Type: application/json`)
- **Autenticación:** ninguna (pensado para red local)

> El estado canónico del aire lo posee y persiste el backend Node (`all-node-app`).
> El backend envía siempre el **estado absoluto** vía `/api/ac/set`. El ESP32 no
> guarda tramas NEC ni raw: solo aplica los setters del protocolo y transmite.

## Resumen de endpoints

| Método | Ruta | Descripción |
|---|---|---|
| GET | `/api/sensors` | Temperatura y humedad |
| GET | `/api/ac/status` | Estado del último estado de aire aplicado |
| GET / POST | `/api/ac/set` | Construye y envía un estado de aire absoluto (power/temp/mode/fan/...) |

---

## GET /api/sensors

Lee el sensor AHT10.

**Respuesta 200**
```json
{
  "temperature": 25.34,
  "humidity": 48.20,
  "unit": "celsius",
  "status": "success"
}
```

**Respuesta 503** (sensor no detectado)
```json
{ "status": "error", "error": "AHT10 no encontrado" }
```

---

## GET /api/ac/status

Estado del último comando de aire aplicado.

**Respuesta 200 (hay estado)**
```json
{
  "valid": true,
  "power": true,
  "temp": 26,
  "mode": "cool",
  "mode_raw": 1,
  "fan": "med",
  "fan_raw": 2,
  "swing_v": "off",
  "swing_v_raw": 0,
  "swing_h": "middle",
  "swing_h_raw": 0,
  "turbo": false,
  "quiet": false,
  "sleep": false,
  "health": false
}
```

**Respuesta 200 (sin estado todavía)**
```json
{ "valid": false }
```

### Valores de `*_raw`

- **mode_raw:** `0`=auto, `1`=cool, `2`=dry, `4`=heat, `6`=fan
- **fan_raw:** `1`=high, `2`=med, `3`=low, `5`=auto
- **swing_v_raw:** `0`=off, `1`=top, `2`=middle, `3`=bottom, `10`=down, `12`=auto
- **swing_h_raw:** `0`=middle, `3`=left_max, `4`=left, `5`=right, `6`=right_max, `7`=auto

---

## GET/POST /api/ac/set

Construye un estado `HAIER_AC_YRW02` con los parámetros recibidos y lo envía por IR.
Los campos omitidos conservan el último estado conocido (o valores por defecto si aún
no se envió ningún comando). La respuesta incluye el estado resultante.

**Parámetros** (query string, o body `application/x-www-form-urlencoded`)

| Nombre | Tipo | Valores | Notas |
|---|---|---|---|
| `power` | bool | `0/1`, `on/off`, `true/false` | |
| `temp` | int | 16–30 | se recorta al rango |
| `mode` | string/int | `auto`, `cool`, `dry`, `heat`, `fan` o raw | |
| `fan` | string/int | `auto`, `high`, `med`, `low` o raw | |
| `swing_v` | string/int | `off`, `top`, `middle`, `bottom`, `down`, `auto` o raw | |
| `swing_h` | string/int | `middle`, `left_max`, `left`, `right`, `right_max`, `auto` o raw | |
| `turbo` | bool | | |
| `quiet` | bool | | |
| `sleep` | bool | | |
| `health` | bool | | |

**Respuesta 200**
```json
{
  "ok": true,
  "state": {
    "valid": true, "power": true, "temp": 24, "mode": "cool", "mode_raw": 1,
    "fan": "med", "fan_raw": 2, "swing_v": "off", "swing_v_raw": 0,
    "swing_h": "middle", "swing_h_raw": 0,
    "turbo": false, "quiet": false, "sleep": false, "health": false
  }
}
```

**Ejemplos**
```bash
curl "http://$IP/api/ac/set?power=on&temp=24&mode=cool&fan=med"
curl "http://$IP/api/ac/set?power=off"
curl "http://$IP/api/ac/set?temp=26&swing_v=auto"
```

---

## Errores

| Código | Cuerpo |
|---|---|
| 404 | `{"ok":false,"error":"not found"}` (ruta desconocida) |
| 503 | `{"status":"error","error":"AHT10 no encontrado"}` |

---

## Notas de integración

- Las rutas aceptan **GET** para facilitar pruebas desde navegador/curl.
- El envío IR es **bloqueante** unos ~100–200 ms (duración de la trama).
- No hay autenticación ni HTTPS; úsalo solo en tu red local o detrás de un proxy.
- `status` se pierde al reiniciar el ESP32. El backend Node conserva el estado
  canónico y reenvía el estado absoluto, por lo que la UI no depende de él.
