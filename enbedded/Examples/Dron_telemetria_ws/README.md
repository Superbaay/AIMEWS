# Dron_telemetria_ws

ESP32-S3 (S3R8) con IMU MPU6500, barómetro HW-291 (MS5607) y GPS NEO-6M que manda
su telemetría al panel de control SAMATI por WebSocket.

| Sensor     | Ritmo                          | Medido en la placa                 |
|------------|--------------------------------|------------------------------------|
| IMU        | 1000 Hz (INT + FIFO)           | 999-1000 Hz, 0 muestras perdidas   |
| Barómetro  | ~89 Hz (OSR 4096, máx. estable)| 79-91 Hz, 0 errores I2C            |
| GPS        | 5 Hz (115200 baud)             | pendiente: no llega NMEA (cableado)|
| Telemetría | 10 msg/s al panel              | ~10 msg/s                          |

## Conexiones

| Módulo            | Pin       | ESP32-S3 |
|-------------------|-----------|----------|
| IMU MPU6050/6500  | SDA / SCL | GPIO12 / GPIO15 (bus compartido) |
|                   | INT       | GPIO16 (opcional: sin él usa solo el FIFO) |
| Barómetro HW-291  | SDA / SCL | GPIO12 / GPIO15 (mismos cables)  |
| GPS NEO-6M        | TX / RX   | GPIO18 / GPIO17 (cruzados; si se invierten, el firmware lo detecta) |
| Todos             | VCC / GND | 3V3 / GND común                  |

USB al puerto **UART** de la placa (CH343, COM9): ahí van el flasheo y los logs.

## Configurar, compilar y flashear

```powershell
copy sdkconfig.defaults.local.ejemplo sdkconfig.defaults.local   # WiFi + máquina (gitignored)
idf.py build
idf.py -p COM9 flash monitor
```

Por defecto se conecta a `wss://api.superbaay.dev/ws/machine` (túnel de Cloudflare;
el prefijo `/ws` ya está permitido). Para una API en la red local: `idf.py menuconfig`
→ "Dron SAMATI" → host = IP de la PC, sin TLS, puerto de uvicorn.

## Protocolo (API/Routers_BM/drone_ws_rt.py + API/main.py)

1. `POST /machine_rt/token_machine` → token de la máquina (24 h).
2. `/ws/machine`, primer mensaje `{"token": ...}` → `{"tipo": "auth_ok"}`.
3. Dron → panel: `estado_dron`, `telemetria` (forma de `sensor_data`), `autoprueba`.
4. Panel → dron (por `/ws`): `iniciar_autoprueba`, `cero_altura`.

## Decisiones medidas (por qué está así)

- **INT + FIFO, leyendo cada 2 muestras.** Leer los registros en cada INT ocupaba el
  bus compartido el 65-90 % del tiempo y perdía 1-4 muestras/s; con el FIFO, una
  lectura retrasada no pierde nada (caben 42 ms) y la ocupación baja a la mitad.
- **Interrupción del I2C en el núcleo 1** (el bus se crea desde una tarea del núcleo 1).
- **Barómetro con prioridad 19 en el núcleo 0**: por encima de la pila de red, para que
  nunca se quede sin CPU con el candado del bus tomado (el IMU esperaba >42 ms
  durante los handshakes TLS).
- **Timeouts I2C de 5 ms** (antes 20/50): un bus colgado se recupera antes de llenar el FIFO.
- **Pull-up en el RX del GPS**: con el TX del GPS suelto el pin flotaba, captaba ruido y
  las interrupciones del UART tumbaban el WebSocket cada ~15 s.
- **Giroscopio a ±2000 °/s**: con ±500 se saturaba al mover la placa.
- **FIFO siempre alineado**: si una lectura del FIFO se corta o el bus devuelve basura,
  las muestras quedan corridas (se midió ~480 °/s con la placa quieta: la gravedad leída
  como giro, y luego ángulos girando solos). La temperatura viaja en el FIFO (14 B por
  muestra, mismo orden que los registros) como **marca de sincronía**: si su valor salta
  más de 300 LSB (~0.9 °C) se reinicia el FIFO y se descarta la ráfaga. En la placa salta
  cada ~10 s (siempre con 0xFFFF = el sensor no respondió: ruido en el bus I2C con
  jumpers largos) y los ángulos quedan estables (± 0.01°, 997-1000 Hz).
- **IMU con reintentos al arrancar**: tras flashear a veces no responde al primer intento;
  se libera el bus (9 pulsos de SCL) y se reintenta hasta 5 veces.
- **GPS con pines autodetectados**: escucha pasivamente GPIO17/18 antes de configurar el
  UART; si no acepta comandos, sigue en modo solo lectura.
