# EmbeddedServer

Sketches:
- `homeserver/homeserver.ino` — servidor web (WiFi + AHT10 + aire York). Documentación de la API: [`homeserver/API.md`](homeserver/API.md).
- `ir_remote_test/ir_remote_test.ino` — control del aire Haier/York por monitor Serie.
- `receptor/receptor.ino` — receptor/decodificador IR (aprendizaje, guarda en NVS).
- `transmisor/transmisor.ino` — emisor IR (solo envía).
- `webhum.ino` — versión previa (WiFi + AHT10, sin IR).

El estado canónico del aire lo posee el backend Node (`all-node-app`), que envía
siempre el estado absoluto al ESP32 vía `/api/ac/set`.
