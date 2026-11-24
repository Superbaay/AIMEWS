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

## Medido en la placa (2026-11-23)

- Arranque: firmware en `ota_0`, OV3660 en HD30, escaneo de las dos redes y conexión a la de
  casa (−59 dBm) en ~7 s.
- Con la API apagada, Cloudflare responde HTTP 530 (error 1033) y la cámara reintenta cada
  ~8 s sin bloquear nada; la tarea de vídeo sigue capturando.
- Vídeo hacia la API, OTA real y cambio de red: **pendientes** (la API estaba apagada).

Notas de la cámara (de `esp32s3-cam-ov3660`): con PCLK 10 MHz los modos con binning (VGA60,
XGA30) pierden frames en escenas con mucho detalle; HD30 no. El WiFi de casa medido de noche
daba 0,5–1,4 Mbit/s: suficiente para HD q25 a ~5 fps, no para 12; el hotspot del teléfono o
bajar la calidad (`set quality 35`) ayudan.
