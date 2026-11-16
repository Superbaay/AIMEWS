"""
Telemetría del dron por WebSocket (ejemplo enbedded/Examples/Dron_telemetria_ws).

Une dos lados:
    Dron   ->  ws(s)://<api>/ws/machine     (este router)
    Panel  ->  ws(s)://<api>/ws             (main.py, usa el `manager` de aquí)

Protocolo del dron
------------------
    1. Primer mensaje (texto):  {"token": "<token de /machine_rt/token_machine>"}
       Si no llega en auth_timeout_s segundos o el token no sirve -> cierre 1008.
    2. La API responde {"tipo": "auth_ok", "machine": "kanan_01"}.
    3. El dron manda JSON de texto con un campo "tipo":
        {"tipo": "estado_dron", "version": "V0.2", ...}       al conectarse
        {"tipo": "telemetria", "datos": {<forma de sensor_data>}, ...}   N veces/s
        {"tipo": "autoprueba", "resultado": {<forma de auto_test_machine>}}
       Todo se reenvía a los paneles con "machine" añadido.
    4. La API le reenvía los comandos del panel:
        {"tipo": "comando", "action": "iniciar_autoprueba", "from": "superbaay"}

El prefijo /ws ya lo deja pasar el túnel de Cloudflare (samati_web.yml), por
eso la ruta del dron es /ws/machine y no hace falta tocar el túnel.
"""
import asyncio
import json
import time
from fastapi import APIRouter, Depends, WebSocket, WebSocketDisconnect, HTTPException, status
from Library import segurity_bm
from Library.base_model_bm import db

router = APIRouter(
    prefix="/ws",
    tags=["Drone WS"]
)

auth_timeout_s = 5
max_message_bytes = 8 * 1024                # una telemetría ronda 700 bytes
drone_types = {"estado_dron", "telemetria", "autoprueba"}
#Acciones que el firmware entiende (enlace_api.c); cualquier otra se rechaza en el panel
drone_actions = {"iniciar_autoprueba", "cero_altura"}


class panel_conn:
    """Un panel conectado. Un solo escritor por socket: todo pasa por sender().

    De la telemetría solo se guarda la MÁS RECIENTE por dron: si el navegador va
    lento (datos móviles), se salta mensajes en vez de acumular retraso. Los
    demás eventos (autoprueba, comandos, errores) sí se entregan todos.
    """

    def __init__(self, websocket: WebSocket, name: str):
        self.websocket = websocket
        self.name = name
        self.telemetry: dict[str, dict] = {}
        self.events: list[dict] = []
        self.wakeup = asyncio.Event()

    def push_telemetry(self, machine: str, message: dict):
        self.telemetry[machine] = message
        self.wakeup.set()

    def push_event(self, event: dict):
        if len(self.events) < 100:          # un panel colgado no acumula memoria sin límite
            self.events.append(event)
        self.wakeup.set()

    async def sender(self):
        while True:
            await self.wakeup.wait()
            self.wakeup.clear()
            events, self.events = self.events, []
            telemetry, self.telemetry = self.telemetry, {}
            for message in events + list(telemetry.values()):
                await self.websocket.send_text(json.dumps(message))


class drone_conn:
    def __init__(self, websocket: WebSocket, name: str):
        self.websocket = websocket
        self.name = name
        self.lock = asyncio.Lock()          # varios paneles pueden mandar comandos a la vez
        self.state: dict | None = None      # último estado_dron
        self.last_telemetry: dict | None = None
        self.messages = 0
        self.window_start = time.monotonic()
        self.rate_hz = 0.0

    def count_message(self):
        self.messages += 1
        elapsed = time.monotonic() - self.window_start
        if elapsed >= 5:
            self.rate_hz = round(self.messages / elapsed, 1)
            self.messages, self.window_start = 0, time.monotonic()

    async def send(self, message: dict) -> bool:
        async with self.lock:
            try:
                await self.websocket.send_text(json.dumps(message))
                return True
            except (WebSocketDisconnect, RuntimeError):
                return False


class connection_manager:
    """Drones (por nombre de máquina) y paneles conectados."""

    def __init__(self):
        self.drones: dict[str, drone_conn] = {}
        self.panels: set[panel_conn] = set()

    def broadcast_event(self, event: dict):
        for panel in self.panels:
            panel.push_event(event)

    def add_panel(self, panel: panel_conn):
        self.panels.add(panel)
        #Lo último de cada dron en línea: el panel se pinta al instante sin esperar
        for drone in self.drones.values():
            if drone.state:
                panel.push_event(drone.state)
            if drone.last_telemetry:
                panel.push_telemetry(drone.name, drone.last_telemetry)

    def remove_panel(self, panel: panel_conn):
        self.panels.discard(panel)

    def from_drone(self, drone: drone_conn, message: dict):
        message["machine"] = drone.name
        if message["tipo"] == "telemetria":
            drone.count_message()
            drone.last_telemetry = message
            for panel in self.panels:
                panel.push_telemetry(drone.name, message)
            return
        if message["tipo"] == "estado_dron":
            drone.state = message
        self.broadcast_event(message)

    async def send_command(self, action: str, sender: str, machine: str | None = None) -> int:
        """Reenvía un comando del panel. Devuelve a cuántos drones llegó."""
        targets = [d for d in self.drones.values() if machine in (None, d.name)]
        command = {"tipo": "comando", "action": action, "from": sender}
        results = await asyncio.gather(*(d.send(command) for d in targets))
        return sum(results)

    def status(self) -> list[dict]:
        return [
            {"machine": d.name, "version": (d.state or {}).get("version"),
             "telemetry_hz": d.rate_hz, "panels": len(self.panels)}
            for d in self.drones.values()
        ]


manager = connection_manager()


def machine_exists(name: str) -> bool:
    return db.machine_rt.user.find_one({"name": name}, {"_id": 1}) is not None


async def reject(websocket: WebSocket, reason: str):
    try:
        await websocket.send_text(json.dumps({"tipo": "error", "message": reason}))
        await websocket.close(code=status.WS_1008_POLICY_VIOLATION, reason=reason)
    except (WebSocketDisconnect, RuntimeError):
        pass


async def receive_token(websocket: WebSocket) -> dict | None:
    """Verificación inicial: primer mensaje {"token": ...}. Devuelve el token decodificado o None."""
    try:
        first = await asyncio.wait_for(websocket.receive_json(), auth_timeout_s)
        if not isinstance(first, dict):
            raise ValueError("first message must be a JSON object")
        token = first.get("token")
        if not isinstance(token, str):
            raise ValueError("missing token")
        return await segurity_bm.verify_token(token)
    except WebSocketDisconnect:
        return None
    #TimeoutError: no mandó nada; ValueError: no era JSON o faltaba el token;
    #KeyError: mandó binario; HTTPException: token inválido o vencido
    except (asyncio.TimeoutError, ValueError, KeyError, HTTPException):
        await reject(websocket, "Unauthorized")
        return None


# ---------------------------------------------------------------------------
# Dron (ESP32-S3)
# ---------------------------------------------------------------------------
@router.websocket("/machine")
async def drone_endpoint(websocket: WebSocket):
    await websocket.accept()
    token = await receive_token(websocket)
    if token is None:
        return
    name = token.get("name")
    if token.get("type") != "machine" or token.get("role") != "4" or not name:
        await reject(websocket, "Only machine tokens can send telemetry")
        return
    #El token dura 24 h: si la máquina se borró en ese tiempo, ya no puede transmitir
    if not await asyncio.to_thread(machine_exists, name):
        await reject(websocket, "Machine not found")
        return

    drone = drone_conn(websocket, name)
    #Si el dron se reconecta (WiFi caído) antes de detectar el corte, gana la conexión nueva
    old = manager.drones.get(name)
    manager.drones[name] = drone
    if old is not None:
        try:
            await old.websocket.close(code=4000, reason="Replaced by a new connection")
        except RuntimeError:
            pass
    await drone.send({"tipo": "auth_ok", "machine": name})

    try:
        while True:
            text = await websocket.receive_text()
            if len(text) > max_message_bytes:
                continue
            try:
                message = json.loads(text)
            except ValueError:
                continue
            #Se descarta en silencio lo que no tenga forma conocida: cortar la
            #telemetría por un mensaje raro sería peor
            if isinstance(message, dict) and message.get("tipo") in drone_types:
                manager.from_drone(drone, message)
    except (WebSocketDisconnect, RuntimeError, KeyError):
        pass
    finally:
        #Solo limpia si sigue siendo la conexión vigente; una reconexión ya pudo reemplazarla
        if manager.drones.get(name) is drone:
            del manager.drones[name]
            manager.broadcast_event({"tipo": "error", "detalle": f"Dron {name} desconectado", "machine": name})


@router.get("/drones")
async def drones_status(token: dict = Depends(segurity_bm.verify_role_Depends_4)):
    return manager.status()
