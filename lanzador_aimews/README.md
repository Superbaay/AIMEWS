# Lanzador de AIMEWS

Ventana pequeña (Tkinter) para iniciar y detener los servicios de SAMATI y ver sus eventos.
Se abre con el acceso directo **AIMEWS** del escritorio.

| Servicio | Cómo lo arranca | Listo cuando |
|---|---|---|
| Base de datos | servicio de Windows `MongoDB` (`sc start/stop`, pide UAC) | escucha en 127.0.0.1:27017 |
| API | `python -m uvicorn main:app --host 127.0.0.1 --port 8001 --log-config uvicorn_log.json` en `API/` | escucha en :8001 |
| Sitio web | `python servidor_sitio.py sitio_web 5500` | escucha en :5500 |
| Túnel de Cloudflare | `cloudflared tunnel --config ~/.cloudflared/samati_web.yml --metrics 127.0.0.1:20247 run` | `/ready` de métricas responde 200 |

- **Eventos**: la salida de cada proceso va a `logs/<servicio>.log` y la ventana la sigue en vivo
  (pestañas API, Sitio web, Túnel). Los eventos de la API llevan hora (`uvicorn_log.json`).
- Los servicios **no dependen de la ventana**: al cerrarla pregunta si detenerlos; si no, siguen
  corriendo y al reabrir el lanzador los reconoce por `logs/<servicio>.pid`.
- **Fuera del lanzador** (punto azul): el servicio ya corría desde otra terminal. Sus eventos no se
  pueden leer; «Reiniciar aquí» lo cierra y lo vuelve a abrir desde el lanzador. Solo cierra
  procesos `python`/`cloudflared`; si el puerto lo ocupa otro programa (p. ej. Live Server de
  VS Code en :5500) muestra «Puerto ocupado» y no lo toca.
- «Detener todo» detiene API, sitio y túnel; la base de datos se deja encendida (arranca con Windows).
- El túnel del servicio de Windows `Cloudflared` (superbaay.org, con token) no se toca.

## Archivos

- `lanzador.pyw`: la aplicación (usa Python 3.14 del sistema: uvicorn, psutil, Pillow).
- `crear_logo.py`: genera `logo.ico` / `logo.png` (pico azul + punto de alerta ámbar).
- `crear_acceso_directo.ps1`: crea o actualiza `AIMEWS.lnk` en el escritorio.
  `powershell -ExecutionPolicy Bypass -File crear_acceso_directo.ps1`
- `uvicorn_log.json`: formato de logs de la API con fecha y hora, sin colores ANSI.
