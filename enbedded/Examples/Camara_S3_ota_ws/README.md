# Camara_S3_ota_ws

ESP32-S3-WROOM CAM N16R8 con sensor **OV3660** que manda su vídeo al panel SAMATI y se
actualiza por **OTA**. Se conecta sola a la **red de casa o al hotspot del teléfono**.

| Qué | Valor |
|---|---|
| Placa | ESP32-S3 N16R8 (16 MB flash, 8 MB PSRAM octal), patillaje Freenove, puerto UART CH343 (**COM5**) |
| Cámara | OV3660, modo **HD30** (1280×720 sin pérdidas de frames), q25 ≈ 20 KB/frame |
| Vídeo a la API | 12 fps (la API acepta hasta 15 por cámara), siempre el frame más reciente |
| WiFi | dos redes: escanea y entra a la conocida con mejor señal; si la pierde, cambia a la otra |
| OTA | dos ranuras de 4 MB, rollback automático si la versión nueva no llega a la API en 2 min |

## Archivos

```
components/esp32s3cam/   librería de cámara (copia de Cloudcode/esp32s3-cam-ov3660):
                         modos VGA60, XGA30, HD30, HD45, FHD22 verificados en la placa
components/net_wifi/     WiFi con dos redes y reconexión (copia de Dron_telemetria_ws)
main/main.c              cámara + tarea de vídeo (núcleo 1)
main/enlace_api.c        token + WebSocket de vídeo + WebSocket de control (núcleo 0)
main/ota_camara.c        descarga e instalación de la OTA (copia de ota_dron.c)
partitions.csv           nvs, otadata, ota_0 y ota_1 de 4 MB (la misma del dron)
```

## Configurar, compilar y flashear

```powershell
copy sdkconfig.defaults.local.ejemplo sdkconfig.defaults.local   # WiFi, hotspot y máquina (gitignored)
idf.py build
idf.py -p COM5 flash monitor      # SOLO la primera vez: instala bootloader con rollback y tabla OTA
```

En `sdkconfig.defaults.local`: red 1 (casa), red 2 (hotspot del teléfono, **2.4 GHz**) y la
máquina registrada en la API (por defecto `dron_camera_01`, rol 4). Si ya existe `sdkconfig`,
bórralo después de cambiar ese archivo (los defaults no pisan un `sdkconfig` existente).

Por defecto se conecta a `api.superbaay.dev` por TLS (túnel de Cloudflare): sirve igual desde
casa que desde el hotspot. Para una API en la red local: `idf.py menuconfig` → "Camara S3
SAMATI" → IP de la PC, sin TLS, puerto de uvicorn.

## Actualizar por OTA (sin cable)

1. Sube `PROJECT_VER` en `CMakeLists.txt` (la cámara rechaza un .bin cuya versión no sea la
   anunciada).
2. Con la API corriendo y la cámara conectada a ella:

```powershell
python enbedded/tools/ota_subir.py --proyecto enbedded/Examples/Camara_S3_ota_ws --maquina dron_camera_01
```

El script compila, sube el `.bin` a `POST /ota/upload`, lo despliega con `POST /ota/deploy` y
espera a que la cámara arranque con la versión nueva y se conecte. También se puede hacer desde
el panel (O5-1). Mientras descarga, la cámara deja de mandar vídeo; el panel ve el avance.

## Protocolo

Dos WebSockets con el mismo token de máquina (`POST /machine_rt/token_machine`):

| Canal | Ruta | Para qué |
|---|---|---|
| Vídeo | `/camera_rt/ws/machine` | JPEG binarios; estado de la cámara en texto; recibe `{"type":"set","name":"quality","value":20}` del panel |
| Control | `/ws/machine` | `estado_dron` con `"acciones": ["ota"]` y la versión; recibe `{"tipo":"comando","action":"ota",...}`; manda `{"tipo":"ota",...}` |

Son dos porque la OTA de la API (`ota_rt.py`) solo habla con las máquinas del router del
dron: así no hay que cambiar la API. **En el panel la cámara aparece también en la lista de
drones**, solo con la acción OTA.

Ajustes que llegan del panel (`set`): los de `camera_rt.camera_settings` (quality,
brightness, framesize, vflip...). `flash` se ignora: esta placa no tiene LED de flash.

## Imagen adaptativa (CONFIG_CAM_FPS_ADAPTATIVO)

La red decide cuántos bytes caben, no la cámara. Cada segundo se miden los fps enviados:
2 s por debajo del 80 % de `CAM_FPS` → baja un escalón; 5 s a tope y con menos del 45 % del
tiempo enviando → sube uno. Escalones: **HD30 q25 → SVGA q30 → VGA q35 → QVGA q25**. Una
subida que hay que deshacer en menos de 10 s no se reintenta en 60 s. Un `set framesize` o
`set quality` desde el panel la apaga (manda el ajuste manual). El log de cada 10 s dice fps,
kbit/s, KB/frame, tiempo de envío medio y máximo, y el escalón.

## Medido en la placa (2026-11-23/24, WiFi de casa −61 a −66 dBm + túnel de Cloudflare)

| Versión | Qué cambió | Resultado |
|---|---|---|
| 1.0.0 | HD fijo, búfer de envío TCP 64 KB, con Nagle | 4–5 fps; envío ~220 ms por frame; QVGA daba **los mismos** fps (coste fijo por mensaje) |
| 1.1.0 | sin Nagle (`TCP_NODELAY` con transporte propio) | envío medio 65–90 ms, pero con 64 KB de búfer se encolaban 2–4 s: el canal de control no autenticaba a tiempo ("Unauthorized") |
| 1.1.4 | búfer de envío 23 KB + escalera simple | **12,0 fps estables**: QVGA y en cuanto hay margen VGA a 11,9–12 fps (~730 kbit/s), envío medio 5–13 ms, 0 fallos |

- OTA real por `ota_subir.py` (1.1.1 → 1.1.4): descarga, instalación, reinicio y confirmación
  sin cable en ~1 min cada una.
- Primera OTA colgada en 0 %: `CONFIG_LWIP_TCP_WND_DEFAULT=65534` sin buffers de RX (el WiFi
  tiraba lo que no cabía). Vuelto a 5760 y límite de 180 s en `ota_camara.c`.
- Con la API apagada, Cloudflare responde HTTP 530 (error 1033) y la cámara reintenta cada
  ~8 s sin bloquear nada.

Para HD a 12 fps hace falta un enlace mejor (~2 Mbit/s): el hotspot del teléfono cerca de la
placa o un canal WiFi menos saturado. Notas de la cámara (de `esp32s3-cam-ov3660`): con PCLK
10 MHz los modos con binning (VGA60, XGA30) pierden frames en escenas con mucho detalle.
