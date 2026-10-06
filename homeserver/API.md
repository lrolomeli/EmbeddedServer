# API del Homeserver (ESP32-C3)

Servidor HTTP en la red local que expone el sensor AHT10 y el envío de comandos IR
(aire York por `HAIER_AC_YRW02` y NEC).

- **Base URL:** `http://<IP-del-ESP32>` (la IP se imprime por el monitor Serie al conectar WiFi).
- **Puerto:** `80`
- **Formato:** JSON UTF-8 (`Content-Type: application/json`)
- **Autenticación:** ninguna (pensado para red local)
- **CORS:** no se envían cabeceras CORS. Si tu app corre en un navegador y en otro origen, pide que se agreguen.

## Resumen de endpoints

| Método | Ruta | Descripción |
|---|---|---|
| GET | `/api/sensors` | Temperatura y humedad |
| GET | `/api/ac/status` | Estado del último comando IR enviado |
| GET | `/api/ir/list` | Lista de tramas IR guardadas |
| GET / POST | `/api/ac/send?name=<nombre>` | Envía una trama guardada |
| GET / POST | `/api/ac/on` | Envía la trama `on` |
| GET / POST | `/api/ac/off` | Envía la trama `off` |

> El IR es de **una sola vía**: `status` refleja el último comando que **tú** enviaste
> (o capturaste), no una lectura del equipo.

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

Devuelve el estado decodificado del último comando de aire enviado.

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
  "swing_v": "sin etiqueta (6)",
  "swing_v_raw": 6,
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

### Campos

| Campo | Tipo | Notas |
|---|---|---|
| `valid` | bool | `false` si aún no se envió ningún comando AC |
| `power` | bool | encendido / apagado |
| `temp` | int | °C (16–30) |
| `mode` / `mode_raw` | string / int | etiqueta y valor crudo |
| `fan` / `fan_raw` | string / int | etiqueta y valor crudo |
| `swing_v` / `swing_v_raw` | string / int | swing vertical |
| `swing_h` / `swing_h_raw` | string / int | swing horizontal |
| `turbo`, `quiet`, `sleep`, `health` | bool | flags |

### Valores de `*_raw`

- **mode_raw:** `0`=auto, `1`=cool, `2`=dry, `4`=heat, `6`=fan
- **fan_raw:** `1`=high, `2`=med, `3`=low, `5`=auto
- **swing_v_raw:** `0`=off, `1`=top, `2`=middle, `3`=bottom, `10`=down, `12`=auto
- **swing_h_raw:** `0`=middle, `3`=left_max, `4`=left, `5`=right, `6`=right_max, `7`=auto

Si un valor no está mapeado, la etiqueta es `"sin etiqueta (N)"` (usa el `*_raw`).

---

## GET /api/ir/list

Lista las tramas guardadas en memoria (NVS).

**Respuesta 200**
```json
[
  { "name": "off",       "len": 229, "hasState": true  },
  { "name": "on",        "len": 229, "hasState": true  },
  { "name": "temp_up",   "len": 229, "hasState": true  },
  { "name": "temp_down", "len": 229, "hasState": true  },
  { "name": "fan",       "len": 229, "hasState": true  },
  { "name": "swing",     "len": 229, "hasState": true  },
  { "name": "nec_50_17", "len": 67,  "hasState": false }
]
```

`hasState: true` significa que además de la trama raw hay estado AC decodificado.

### Tramas por defecto
`off`, `on`, `temp_up`, `temp_down`, `fan`, `swing`, `nec_50_17`.
Puedes aprender más con el sketch `receptor.ino` (se guardan en la misma NVS).

---

## GET/POST /api/ac/send?name=<nombre>

Envía por IR una trama guardada.

**Parámetros**

| Nombre | Ubicación | Requerido | Descripción |
|---|---|---|---|
| `name` | query string (GET o POST) o body `application/x-www-form-urlencoded` | sí | nombre de la trama (ver `/api/ir/list`) |

**Respuesta 200**
```json
{ "ok": true, "name": "temp_up" }
```

**Respuesta 404** (no existe la trama)
```json
{ "ok": false, "name": "lo_que_sea" }
```

**Respuesta 400** (falta `name`)
```json
{ "ok": false, "error": "falta parametro name" }
```

---

## GET/POST /api/ac/on  ·  /api/ac/off

Atajos que envían las tramas `on` / `off`.

**Respuesta 200**
```json
{ "ok": true, "name": "on" }
```

---

## Errores

| Código | Cuerpo |
|---|---|
| 400 | `{"ok":false,"error":"falta parametro name"}` |
| 404 | `{"ok":false,"error":"not found"}` (ruta desconocida) |
| 503 | `{"status":"error","error":"AHT10 no encontrado"}` |

---

## Ejemplos

### cURL
```bash
IP=192.168.1.50

curl "http://$IP/api/sensors"
curl "http://$IP/api/ac/status"
curl "http://$IP/api/ir/list"
curl "http://$IP/api/ac/on"
curl "http://$IP/api/ac/off"
curl "http://$IP/api/ac/send?name=temp_up"

# POST form-urlencoded
curl -X POST -d "name=fan" "http://$IP/api/ac/send"
```

### JavaScript (fetch)
```js
const BASE = "http://192.168.1.50";

async function getStatus() {
  const r = await fetch(`${BASE}/api/ac/status`);
  return r.json();               // { valid, power, temp, mode, ... }
}

async function sendCommand(name) {
  const r = await fetch(`${BASE}/api/ac/send?name=${encodeURIComponent(name)}`);
  return r.json();               // { ok: true, name }
}

// Atajos
const on  = () => fetch(`${BASE}/api/ac/on`).then(r => r.json());
const off = () => fetch(`${BASE}/api/ac/off`).then(r => r.json());
```

### Python (requests)
```python
import requests

BASE = "http://192.168.1.50"

def sensors():
    return requests.get(f"{BASE}/api/sensors", timeout=5).json()

def status():
    return requests.get(f"{BASE}/api/ac/status", timeout=5).json()

def send(name):
    return requests.get(f"{BASE}/api/ac/send", params={"name": name}, timeout=5).json()

def power(on: bool):
    path = "on" if on else "off"
    return requests.get(f"{BASE}/api/ac/{path}", timeout=5).json()

# Ejemplos
print(sensors())
print(status())
send("temp_up")
power(True)
power(False)
```

---

## Notas de integración

- Las rutas aceptan **GET** para facilitar pruebas desde navegador/curl.
- El envío IR es **bloqueante** unos ~100–200 ms (duración de la trama).
- No hay autenticación ni HTTPS; úsalo solo en tu red local o detrás de un proxy.
- `status` se pierde al reiniciar (no se persiste). Si necesitas el estado tras
  reinicio, guarda `swing`/`temp` en tu propia app o pide soporte de persistencia.
- Si tu app está en un navegador de otro origen y necesitas CORS, hay que agregar
  cabeceras `Access-Control-Allow-Origin` en el firmware (no están incluidas).

### Flujo típico
1. `GET /api/ac/status` para pintar el estado actual.
2. `GET /api/ac/on` o `.../off` para encender/apagar.
3. `GET /api/ac/send?name=temp_up` / `temp_down` para subir/bajar 1 °C.
4. `GET /api/ac/send?name=fan` / `swing` para ciclar ventilador / ángulo.
5. Volver a `GET /api/ac/status` para refrescar la UI.
