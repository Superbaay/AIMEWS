"""
Video de la ESP32-CAM por WebSocket (ejemplo enbedded/Examples/Camara_12fps).

Protocolo
---------
Camara  ->  ws://<api>/camera_rt/ws/machine
    1. Primer mensaje (texto):  {"token": "<token de /machine_rt/token_machine>"}
    2. Cada frame: mensaje BINARIO con el JPEG tal cual sale de fb->buf / fb->len.
    3. Opcional (texto): el JSON de esp32cam_status_json(); se reenvia a los visores.
    La API le puede mandar comandos (texto) que vienen del panel:
        {"type": "set", "name": "quality", "value": 20, "from": "superbaay"}
        {"type": "flash", "value": 0..255, "from": "superbaay"}

Panel   ->  ws://<api>/camera_rt/ws/view
    1. Primer mensaje (texto):  {"token": "<token de /token>", "machine": "kanan_01"}
    2. Recibe frames BINARIOS (JPEG) y eventos de texto:
        {"type": "stats", "fps": 12.0, "kbps": 640, "dropped": 0}
        {"type": "status", ...}        estado que mando la camara
        {"type": "camera_online"} / {"type": "camera_offline"}
    3. Puede mandar comandos: {"set": "quality", "value": 20} o {"flash": 128}

Si el primer mensaje no llega en auth_timeout_s segundos o el token no sirve,
se cierra con 1008 (policy violation) sin aceptar nada mas.
"""
import asyncio
import json
import time
from fastapi import APIRouter, WebSocket, WebSocketDisconnect, HTTPException, Depends, status
from fastapi.responses import Response
from Library import segurity_bm
from Library.base_model_bm import db

router = APIRouter(
    prefix="/camera_rt",
    tags=["Camera RT"]
)

fps_target = 12                     # FPS_OBJETIVO del ejemplo
fps_ceiling = fps_target + 3        # tolera el "recupero" de plazos absolutos del firmware
auth_timeout_s = 5
max_frame_bytes = 256 * 1024        # TAM_COPIA_MAX del ejemplo; un UXGA q12 ronda 55 KB
jpeg_soi = b"\xff\xd8"              # todo JPEG empieza con Start Of Image

#Ajustes que acepta esp32cam_set() (esp32cam.h) + framesize; cualquier otro nombre se rechaza aqui
camera_settings = {
    "framesize", "quality", "brightness", "contrast", "saturation", "sharpness", "denoise",
    "special_effect", "wb_mode", "awb", "awb_gain", "aec", "aec2", "ae_level", "aec_value",
    "agc", "agc_gain", "gainceiling", "bpc", "wpc", "raw_gma", "lenc", "hmirror", "vflip",
    "dcw", "colorbar",
}


class viewer_conn:
    """Un visor. Un solo escritor por socket: todo lo que se le manda pasa por sender()."""

    def __init__(self, websocket: WebSocket, name: str):
        self.websocket = websocket
        self.name = name
        self.frame: bytes | None = None     # solo el MAS RECIENTE: si el visor va lento, se saltan frames
        self.events: list[dict] = []
        self.wakeup = asyncio.Event()

    def push_frame(self, frame: bytes):
        self.frame = frame
        self.wakeup.set()

    def push_event(self, event: dict):
        if len(self.events) < 50:           # un visor colgado no acumula memoria sin limite
            self.events.append(event)
        self.wakeup.set()

    async def sender(self):
        while True:
            await self.wakeup.wait()
            self.wakeup.clear()
            events, self.events = self.events, []
            for event in events:
                await self.websocket.send_text(json.dumps(event))
            frame, self.frame = self.frame, None
            if frame is not None:
                await self.websocket.send_bytes(frame)


class camera_channel:
    """Una camara (por nombre de maquina) y quienes la estan viendo."""

    def __init__(self, name: str):
        self.name = name
        self.source: WebSocket | None = None
        self.source_lock = asyncio.Lock()   # varios visores pueden mandar comandos a la vez
        self.viewers: set[viewer_conn] = set()
        self.last_frame: bytes | None = None
        self.last_status: dict | None = None
        self.window_start = time.monotonic()
        self.window_frames = 0
        self.window_bytes = 0
        self.window_dropped = 0

    def broadcast_event(self, event: dict):
        for viewer in self.viewers:
            viewer.push_event(event)

    def accept_frame(self, frame: bytes) -> bool:
        """Cuenta el frame, aplica el tope de fps y cada segundo publica estadisticas."""
        now = time.monotonic()
        elapsed = now - self.window_start
        if elapsed >= 1.0:
            self.broadcast_event({
                "type": "stats",
                "fps": round(self.window_frames / elapsed, 1),
                "kbps": round(self.window_bytes / elapsed / 1024),
                "dropped": self.window_dropped,
            })
            self.window_start, self.window_frames, self.window_bytes, self.window_dropped = now, 0, 0, 0
        if self.window_frames >= fps_ceiling:
            self.window_dropped += 1
            return False
        self.window_frames += 1
        self.window_bytes += len(frame)
        return True

    def broadcast_frame(self, frame: bytes):
        self.last_frame = frame
        for viewer in self.viewers:
            viewer.push_frame(frame)

    async def send_to_source(self, command: dict) -> bool:
        if self.source is None:
            return False
        async with self.source_lock:
            try:
                await self.source.send_text(json.dumps(command))
                return True
            except (WebSocketDisconnect, RuntimeError):
                return False


channels: dict[str, camera_channel] = {}


def get_channel(name: str) -> camera_channel:
    if name not in channels:
        channels[name] = camera_channel(name)
    return channels[name]


def machine_exists(name: str) -> bool:
    return db.machine_rt.user.find_one({"name": name}, {"_id": 1}) is not None


async def receive_first_message(websocket: WebSocket) -> tuple[dict, dict] | None:
    """Verificacion inicial: primer mensaje con el token. Devuelve (token decodificado, mensaje) o None."""
    try:
        first = await asyncio.wait_for(websocket.receive_json(), auth_timeout_s)
        if not isinstance(first, dict):
            raise ValueError("first message must be a JSON object")
        token = await segurity_bm.verify_token(first.get("token"))
        return token, first
    except WebSocketDisconnect:
        return None
    #TimeoutError: no mando nada; ValueError: no era JSON; KeyError: mando binario; HTTPException: token invalido
    except (asyncio.TimeoutError, ValueError, KeyError, HTTPException):
        await reject(websocket, "Unauthorized")
        return None


async def reject(websocket: WebSocket, reason: str):
    try:
        await websocket.send_text(json.dumps({"type": "error", "message": reason}))
        await websocket.close(code=status.WS_1008_POLICY_VIOLATION, reason=reason)
    except (WebSocketDisconnect, RuntimeError):
        pass


# ---------------------------------------------------------------------------
# Camara (ESP32-CAM)
# ---------------------------------------------------------------------------
@router.websocket("/ws/machine")
async def camera_source(websocket: WebSocket):
    await websocket.accept()
    first = await receive_first_message(websocket)
    if first is None:
        return
    token, _ = first
    name = token.get("name")
    if token.get("type") != "machine" or token.get("role") != "4" or not name:
        await reject(websocket, "Only machine tokens can stream")
        return
    #El token dura 24 h: si la maquina se borro en ese tiempo, ya no puede transmitir
    if not machine_exists(name):
        await reject(websocket, "Machine not found")
        return

    channel = get_channel(name)
    #Si la camara se reconecta (WiFi caido) antes de que se detecte el corte, gana la conexion nueva
    old_source = channel.source
    channel.source = websocket
    if old_source is not None:
        try:
            await old_source.close(code=4000, reason="Replaced by a new connection")
        except RuntimeError:
            pass
    await websocket.send_text(json.dumps({
        "type": "auth_ok", "machine": name, "fps": fps_target, "max_frame_bytes": max_frame_bytes,
    }))
    channel.broadcast_event({"type": "camera_online"})

    try:
        while True:
            message = await websocket.receive()
            if message["type"] == "websocket.disconnect":
                break
            frame = message.get("bytes")
            if frame is not None:
                #Se descarta en silencio: cortar el stream por un frame raro seria peor
                if len(frame) > max_frame_bytes or not frame.startswith(jpeg_soi):
                    channel.window_dropped += 1
                    continue
                if channel.accept_frame(frame):
                    channel.broadcast_frame(frame)
                continue
            text = message.get("text")
            if text:
                try:
                    camera_status = json.loads(text)
                except ValueError:
                    continue
                if isinstance(camera_status, dict):
                    channel.last_status = camera_status
                    channel.broadcast_event({"type": "status", **camera_status})
    except (WebSocketDisconnect, RuntimeError):
        pass
    finally:
        #Solo limpia si sigue siendo la fuente; una reconexion ya pudo haberla reemplazado
        if channel.source is websocket:
            channel.source = None
            channel.broadcast_event({"type": "camera_offline"})


# ---------------------------------------------------------------------------
# Visores (panel de control)
# ---------------------------------------------------------------------------
def build_command(message: dict) -> dict | None:
    """Traduce lo que manda el panel a un comando valido para la camara, o None si no lo es."""
    value = message.get("value") if "set" in message else message.get("flash")
    if not isinstance(value, int) or isinstance(value, bool):
        return None
    if "set" in message:
        if message["set"] not in camera_settings:
            return None
        return {"type": "set", "name": message["set"], "value": value}
    if "flash" in message and 0 <= value <= 255:
        return {"type": "flash", "value": value}
    return None


async def viewer_receiver(websocket: WebSocket, channel: camera_channel, viewer: viewer_conn):
    while True:
        message = await websocket.receive()
        if message["type"] == "websocket.disconnect":
            return
        try:
            data = json.loads(message.get("text") or "")
        except ValueError:
            continue
        command = build_command(data) if isinstance(data, dict) else None
        if command is None:
            viewer.push_event({"type": "error", "message": "Invalid command"})
            continue
        command["from"] = viewer.name
        if not await channel.send_to_source(command):
            viewer.push_event({"type": "error", "message": "Camera offline"})


@router.websocket("/ws/view")
async def camera_viewer(websocket: WebSocket):
    await websocket.accept()
    first = await receive_first_message(websocket)
    if first is None:
        return
    token, message = first
    #Mismos permisos que el /ws del panel: personas con rol 4 u O5-1
    if token.get("type", "user") != "user" or token.get("role") not in ("4", "O5-1"):
        await reject(websocket, "Role insuficiente")
        return
    machine = message.get("machine")
    if not isinstance(machine, str) or not machine_exists(machine):
        await reject(websocket, "Machine not found")
        return

    channel = get_channel(machine)
    viewer = viewer_conn(websocket, token.get("name"))
    await websocket.send_text(json.dumps({
        "type": "auth_ok", "machine": machine, "online": channel.source is not None,
        "fps": fps_target, "status": channel.last_status,
    }))
    channel.viewers.add(viewer)
    #El ultimo frame conocido: el visor ve imagen al instante y no espera al siguiente
    if channel.last_frame is not None and channel.source is not None:
        viewer.push_frame(channel.last_frame)

    tasks = {
        asyncio.create_task(viewer.sender()),
        asyncio.create_task(viewer_receiver(websocket, channel, viewer)),
    }
    try:
        #Cuando una de las dos termina (desconexion o error al enviar) se cancela la otra
        done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        for task in done:
            task.exception()    # marca la excepcion como vista; la desconexion es el fin normal
    finally:
        for task in tasks:
            task.cancel()
        channel.viewers.discard(viewer)


# ---------------------------------------------------------------------------
# HTTP: estado y foto fija
# ---------------------------------------------------------------------------
@router.get("/status")
async def camera_status(token: dict = Depends(segurity_bm.verify_role_Depends_4)):
    return [
        {"machine": c.name, "online": c.source is not None, "viewers": len(c.viewers),
         "last_frame_bytes": len(c.last_frame) if c.last_frame else 0}
        for c in channels.values()
    ]


@router.get("/snapshot")
async def camera_snapshot(machine: str, token: dict = Depends(segurity_bm.verify_role_Depends_4)):
    channel = channels.get(machine)
    if channel is None or channel.source is None or channel.last_frame is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, detail="No frame available")
    return Response(content=channel.last_frame, media_type="image/jpeg")
