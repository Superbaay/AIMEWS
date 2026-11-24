"""
Telemetría del dron por WebSocket (ejemplo enbedded/Examples/Dron_telemetria_ws).

Une dos lados, los dos en este router y con el mismo `manager`:
    Dron   ->  ws(s)://<api>/ws/machine
    Panel  ->  ws(s)://<api>/ws             (ruta "" bajo el prefijo /ws, sin barra final)
Lo común con la cámara (token inicial, reject, machine_exists, cola de un solo
escritor) vive en Library/ws_bm.py.

Protocolo del dron
------------------
    1. Primer mensaje (texto):  {"token": "<token de /machine_rt/token_machine>"}
       Si no llega en auth_timeout_s segundos o el token no sirve -> cierre 1008.
    2. La API responde {"tipo": "auth_ok", "machine": "kanan_01"}.
    3. El dron manda JSON de texto (máx. 8 KB) con un campo "tipo":
        {"tipo": "estado_dron", "version": "V0.2", "acciones": [...], ...}   al conectarse y al cambiar
           "acciones" = lo que el dron entiende (contratos.md §7.9). La API la guarda por dron;
           si no viene (firmware real) asume firmware_actions = iniciar_autoprueba y cero_altura.
        {"tipo": "telemetria", "datos": {<forma de sensor_data>}, ...}   N veces/s
        {"tipo": "autoprueba", "resultado": {<forma de auto_test_machine>}}
        {"tipo": "pid", "pid": {"alabeo": {"kp", "ki", "kd"}, ...}}   tras "pid" o "pid_leer"
        {"tipo": "mision", "evento": ..., ...}                  eventos de la misión
        {"tipo": "ota", "estado": "descargando|instalado|error|confirmado", "porcentaje"?, ...}
           avance de una actualización (la manda POST /ota/deploy, ver ota_rt.py)
       Todo se reenvía a los paneles con "machine" añadido. Otro "tipo" se descarta.
       El firmware real (enlace_api.c) solo manda estado_dron, telemetria y autoprueba.
    4. La API le reenvía lo que manda el panel ("from" = usuario del panel):
        {"tipo": "comando", "action": "armar", "from": "superbaay"}
        {"tipo": "comando", "action": "modo", "modo": "altitud", "from": ...}
        {"tipo": "comando", "action": "pid", "pid": {"alabeo": {"kp": 1, "ki": 0, "kd": 0.1}, ...}, "from": ...}
        {"tipo": "comando", "action": "iniciar_mision", "from": ...,
         "mision": {"tipo": "seco|inundado", "coordinates_list": [{"latitude", "longitude", "altitude"?}],
                    "demostracion": bool}}
        {"tipo": "control", "action": "movimiento", "from": ...,
         "ejes": {"potencia", "guinada", "cabeceo", "alabeo"}}      palancas a 10 Hz, de -1 a 1
       "tipo" va siempre primero: el lector del firmware busca la primera clave "tipo".
       El dron ignora las acciones y tipos que no conoce (el firmware solo atiende
       iniciar_autoprueba y cero_altura, y no lee mensajes de 512 bytes o más).

Protocolo del panel (sitio_web/panel_control/main.js y Control_dron/main.js)
---------------------------------------------------------------------------
    1. Recibe {"message": "WebSocket connection established"} y
       {"message": "please send a password"}.
    2. Primer mensaje {"token": "<token de /token>"}; sin él en auth_timeout_s, o con
       un token inválido o un rol que no sea 4 / O5-1, recibe {"message": "Unauthorized"}
       y se cierra con 1008. Si sirve, recibe {"message": "Password received"}.
    3. Recibe lo que manda el dron: estado_dron, telemetria, autoprueba (con "machine").
    3b. También recibe "pid" y "mision" del dron (con "machine").
    4. Manda {"tipo": "comando", "action": <drone_actions>, "machine": "kanan_01", ...campos}
       y recibe la respuesta {"tipo": "comando", "action": ..., "api_response": {"message": ...}}.
       "machine" es obligatorio ("Falta machine") salvo en broadcast_actions
       (iniciar_autoprueba, cero_altura), que sin él van a todos los drones.
       Una acción que el dron no declaró responde "El dron <nombre> no admite <acción>"
       y no se reenvía.
       Campos que se reenvían (lista blanca, ver command_fields): "modo", "pid", "mision".
       Si un campo no tiene la forma esperada, el comando no se reenvía y api_response lo dice.
    5. Manda {"tipo": "control", "action": "movimiento", "ejes": {...}, "machine": "kanan_01"}
       a 10 Hz. Se reenvía SOLO al dron indicado ("machine" obligatorio) y SIN respuesta
       por paquete. Más de max_control_hz paquetes por segundo y panel se descartan en silencio. Solo si el paquete es inválido (ejes fuera de [-1, 1], falta un eje,
       sin "machine") recibe {"tipo": "control", "action": ..., "api_response": {"message": ...}},
       que el panel ignora.
    Un mensaje de más de 8 KB (bytes UTF-8) recibe {"tipo": "comando", "action": null,
    "api_response": {"message": "Message too large"}}.

Lo que se guarda en Mongo (sin frenar el WebSocket: todo pasa por Library/escritor_bm.py)
------------------------------------------------------------------------------------------
    - "telemetria": hasta escritor_bm.max_guardar_hz mensajes por segundo y dron, en
      machine_telemetry_data (la misma que lee GET /machine_telemetry_rt/sensor_data).
    - "iniciar_mision" que llegó al dron: crea la misión en "missions".
    - "mision" del dron (waypoint, medicion, alerta, fin, abortada, rechazada): actualiza la
      misión, guarda la medición en "readings" y la alerta en "alerts" con la foto de la
      ESP32-CAM. Cuando la alerta queda guardada, los paneles reciben
      {"tipo": "alerta_registrada", "machine", "number", "mission_number", "foto": bool}.
      Ver Library/misiones_bm.py y Routers_BM/mission_rt.py.

Roles: el rol del panel se vuelve a leer en la base al conectarse, en cada comando y cada
role_check_s en las palancas (segurity_bm.rol_efectivo). Bajar de rol o borrar a un usuario
le quita el control del dron sin esperar a que venza su token de 24 h.

El prefijo /ws ya lo deja pasar el túnel de Cloudflare (samati_web.yml), por
eso la ruta del dron es /ws/machine y no hace falta tocar el túnel.
"""
import asyncio
import json
import math
import time
from fastapi import APIRouter, Depends, WebSocket, WebSocketDisconnect, status
from jose import JWTError
from Library import misiones_bm
from Library import segurity_bm
from Library import ws_bm
from Library.escritor_bm import escritor_global

router = APIRouter(
    prefix="/ws",
    tags=["Drone WS"]
)

auth_timeout_s = ws_bm.auth_timeout_s
max_message_bytes = 8 * 1024                # una telemetría ronda 700 bytes
drone_types = {"estado_dron", "telemetria", "autoprueba", "pid", "mision", "ota"}
#Acciones del panel (copia de ACCIONES_PANEL de simulador/contratos.py + abortar_mision; la API
#no importa del simulador). Cualquier otra se rechaza en el panel. El firmware real solo
#atiende iniciar_autoprueba y cero_altura e ignora en silencio el resto
drone_actions = {
    "armar", "desarmar", "despegar", "aterrizar", "mantener_posicion",
    "regresar_casa", "paro_emergencia", "modo", "pid", "pid_leer",
    "iniciar_autoprueba", "cero_altura",
    "iniciar_mision", "abortar_mision",
}
#Lo único que entiende el firmware real (enlace_api.c): se asume si el dron no declara "acciones"
firmware_actions = frozenset({"iniciar_autoprueba", "cero_altura"})
#Acciones inofensivas que sin "machine" pueden ir a todos los drones; el resto exige destino
broadcast_actions = frozenset({"iniciar_autoprueba", "cero_altura"})
max_control_hz = 20                         # paquetes de palancas por segundo y panel (el panel manda 10)
control_axes = ("potencia", "guinada", "cabeceo", "alabeo")
pid_gains = ("kp", "ki", "kd")
max_pid_axes = 8
max_name_len = 32                           # modo y nombres de eje del PID
mision_types = {"seco", "inundado"}
role_check_s = 10                           # cada cuánto se revisa el rol de un panel que solo mueve palancas


def is_number(value) -> bool:
    """int o float finito; bool no cuenta (en Python True es un int). json.loads acepta NaN."""
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def check_pid(pid) -> dict:
    """{eje: {kp, ki, kd}} con números. Devuelve una copia limpia o lanza ValueError."""
    if not isinstance(pid, dict) or not pid or len(pid) > max_pid_axes:
        raise ValueError("pid must be an object {axis: {kp, ki, kd}}")
    clean = {}
    for axis, gains in pid.items():
        if not isinstance(axis, str) or not 0 < len(axis) <= max_name_len:
            raise ValueError("invalid pid axis")
        if not isinstance(gains, dict) or set(gains) != set(pid_gains) \
                or not all(is_number(gains[g]) for g in pid_gains):
            raise ValueError(f"pid.{axis} must be {{kp, ki, kd}} with numbers")
        clean[axis] = {g: gains[g] for g in pid_gains}
    return clean


def check_point(point) -> dict:
    if not isinstance(point, dict):
        raise ValueError("each point must be an object")
    latitude, longitude = point.get("latitude"), point.get("longitude")
    if not is_number(latitude) or not -90 <= latitude <= 90 \
            or not is_number(longitude) or not -180 <= longitude <= 180:
        raise ValueError("each point needs numeric latitude and longitude")
    clean = {"latitude": latitude, "longitude": longitude}
    if "altitude" in point and point["altitude"] is not None:
        if not is_number(point["altitude"]):
            raise ValueError("altitude must be a number")
        clean["altitude"] = point["altitude"]
    return clean


def check_mision(mision) -> dict:
    """{"tipo": "seco|inundado", "coordinates_list": [...], "demostracion": bool}.
    Solo se copian esas claves. Lanza ValueError si algo no tiene la forma esperada."""
    if not isinstance(mision, dict):
        raise ValueError("mision must be an object")
    if mision.get("tipo") not in mision_types:
        raise ValueError("mision.tipo must be seco or inundado")
    clean = {"tipo": mision["tipo"]}
    if "coordinates_list" in mision:
        points = mision["coordinates_list"]
        if not isinstance(points, list):
            raise ValueError("mision.coordinates_list must be a list")
        clean["coordinates_list"] = [check_point(point) for point in points]
    if "demostracion" in mision:
        if not isinstance(mision["demostracion"], bool):
            raise ValueError("mision.demostracion must be true or false")
        clean["demostracion"] = mision["demostracion"]
    return clean


def command_fields(info: dict) -> dict:
    """Campos extra de un comando del panel que se reenvían al dron (lista blanca).
    Lo que no está en la lista se descarta; lo que está con mala forma lanza ValueError."""
    fields = {}
    if "modo" in info:
        if not isinstance(info["modo"], str) or not 0 < len(info["modo"]) <= max_name_len:
            raise ValueError("modo must be a short string")
        fields["modo"] = info["modo"]
    if "pid" in info:
        fields["pid"] = check_pid(info["pid"])
    if "mision" in info:
        fields["mision"] = check_mision(info["mision"])
    if len(json.dumps(fields)) > max_message_bytes:
        raise ValueError("fields too large")
    return fields


def declared_actions(acciones) -> frozenset:
    """estado_dron.acciones -> conjunto de acciones. Sin lista válida, las del firmware real."""
    if not isinstance(acciones, list) or not all(isinstance(a, str) for a in acciones):
        return firmware_actions
    return frozenset(acciones)


def check_axes(ejes) -> dict:
    """Los cuatro ejes de las palancas, números en [-1, 1]. Lanza ValueError si no."""
    if not isinstance(ejes, dict):
        raise ValueError("ejes must be an object")
    clean = {}
    for axis in control_axes:
        value = ejes.get(axis)
        if not is_number(value) or not -1 <= value <= 1:
            raise ValueError(f"ejes.{axis} must be a number between -1 and 1")
        clean[axis] = value
    return clean


class panel_conn(ws_bm.single_writer):
    """Un panel conectado. Un solo escritor por socket: todo pasa por sender().

    De la telemetría solo se guarda la MÁS RECIENTE por dron: si el navegador va
    lento (datos móviles), se salta mensajes en vez de acumular retraso. Los
    demás eventos (autoprueba, comandos, errores) sí se entregan todos (hasta 100
    pendientes).
    """
    max_events = 100

    def __init__(self, websocket: WebSocket, name: str):
        super().__init__(websocket, name)
        self.telemetry: dict[str, dict] = {}
        #Cubeta de fichas para los paquetes control: hasta max_control_hz por segundo
        self.control_tokens = float(max_control_hz)
        self.control_time = time.monotonic()
        self.role_checked = time.monotonic()    # última vez que se leyó el rol en la base

    def allow_control(self) -> bool:
        now = time.monotonic()
        self.control_tokens = min(max_control_hz,
                                  self.control_tokens + (now - self.control_time) * max_control_hz)
        self.control_time = now
        if self.control_tokens < 1:
            return False
        self.control_tokens -= 1
        return True

    def push_telemetry(self, machine: str, message: dict):
        self.telemetry[machine] = message
        self.wakeup.set()

    async def flush(self, events: list[dict]):
        telemetry, self.telemetry = self.telemetry, {}
        await super().flush(events + list(telemetry.values()))


class drone_conn:
    def __init__(self, websocket: WebSocket, name: str):
        self.websocket = websocket
        self.name = name
        self.lock = asyncio.Lock()          # varios paneles pueden mandar comandos a la vez
        self.state: dict | None = None      # último estado_dron
        self.actions: frozenset = firmware_actions     # lo que declaró en estado_dron.acciones
        self.last_telemetry: dict | None = None
        self.last_ota: dict | None = None   # último {"tipo": "ota"} (lo consulta GET /ota/status)
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
            if isinstance(message.get("datos"), dict):
                escritor_global.telemetria_recibida(drone.name, message["datos"])
            return
        if message["tipo"] == "mision":
            self.save_mission_event(drone.name, message)
        if message["tipo"] == "ota":
            drone.last_ota = message
        if message["tipo"] == "estado_dron":
            drone.state = message
            drone.actions = declared_actions(message.get("acciones"))
        self.broadcast_event(message)

    def save_mission_event(self, machine: str, message: dict):
        """Guarda el evento de misión en segundo plano. El reenvío a los paneles no espera esto."""
        #La foto se toma AHORA (es solo copiar la referencia al último frame en memoria); guardarla
        #en disco y en Mongo lo hace el escritor
        foto = alert_photo(machine) if message.get("evento") == "alerta" else None

        def avisar(alerta):
            if alerta:
                self.broadcast_event({"tipo": "alerta_registrada", "machine": machine,
                                      "number": alerta["number"], "mission_number": alerta["mission_number"],
                                      "profundidad_m": alerta.get("profundidad_m"), "foto": bool(alerta.get("foto"))})
        escritor_global.encolar(misiones_bm.registrar_evento, machine, dict(message), foto, al_terminar=avisar)

    async def send_command(self, action: str, sender: str, machine: str | None = None,
                           fields: dict | None = None) -> int:
        """Reenvía un comando del panel. Devuelve a cuántos drones llegó.
        fields: campos ya validados por command_fields (modo, pid, mision)."""
        targets = [d for d in self.drones.values() if machine in (None, d.name) and action in d.actions]
        #"tipo" primero: el firmware busca la primera aparición de "tipo" (la misión trae otro dentro)
        command = {"tipo": "comando", "action": action, "from": sender, **(fields or {})}
        results = await asyncio.gather(*(d.send(command) for d in targets))
        return sum(results)

    async def send_control(self, ejes: dict, sender: str, machine: str) -> bool:
        """Reenvía un paquete de palancas SOLO al dron indicado. True si llegó."""
        drone = self.drones.get(machine)
        if drone is None:
            return False
        return await drone.send({"tipo": "control", "action": "movimiento", "ejes": ejes, "from": sender})

    def status(self) -> list[dict]:
        return [
            {"machine": d.name, "version": (d.state or {}).get("version"),
             "telemetry_hz": d.rate_hz, "panels": len(self.panels),
             "acciones": sorted(d.actions)}
            for d in self.drones.values()
        ]


manager = connection_manager()


def alert_photo(machine: str) -> bytes | None:
    """Último frame JPEG de la ESP32-CAM del dron: la de su mismo nombre, la "<nombre>_cam", o
    la única cámara en línea si hay solo una. None si no hay cámara transmitiendo."""
    from Routers_BM.camera_rt import channels   # import local: camera_rt no debe depender de este archivo
    for name in (machine, f"{machine}_cam"):
        channel = channels.get(name)
        if channel is not None and channel.source is not None and channel.last_frame:
            return channel.last_frame
    online = [c for c in channels.values() if c.source is not None and c.last_frame]
    return online[0].last_frame if len(online) == 1 else None


async def reject(websocket: WebSocket, reason: str):
    """{"tipo": "error", "message": reason} y cierre 1008."""
    await ws_bm.reject(websocket, reason, "tipo")


async def receive_token(websocket: WebSocket) -> dict | None:
    """Verificación inicial: primer mensaje {"token": ...}. Devuelve el token decodificado o None."""
    first = await ws_bm.receive_token(websocket, "tipo")
    return None if first is None else first[0]


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
    if not await asyncio.to_thread(ws_bm.machine_exists, name):
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
            if len(text.encode("utf-8")) > max_message_bytes:
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


# ---------------------------------------------------------------------------
# Panel de control (navegador)
# ---------------------------------------------------------------------------
panel_roles = ("O5-1", "4")


async def panel_auth(websocket: WebSocket) -> dict | None:
    await websocket.send_json({"message": "WebSocket connection established"})
    await websocket.send_json({"message": "please send a password"})
    try:
        token, _ = await ws_bm.read_first_message(websocket)
        #decode_token (no verify_token): un token inválido llega como JWTError
        user = await segurity_bm.decode_token(token)
    except WebSocketDisconnect:
        return None
    #Antes un mensaje sin "token" lanzaba AttributeError fuera del except y tumbaba la conexion
    except (asyncio.TimeoutError, ValueError, KeyError, JWTError):
        user = None
    if user is not None and user.get("type", "user") == "user" and user.get("name"):
        #El rol del token puede ser viejo (dura 24 h): manda el de la base si es menor
        user = {**user, "role": await segurity_bm.rol_efectivo_async(user)}
    if user is None or user.get("type", "user") != "user" or user.get("role") not in panel_roles \
            or not user.get("name"):
        try:
            await websocket.send_json({"message": "Unauthorized"})
            await websocket.close(code=status.WS_1008_POLICY_VIOLATION, reason="Unauthorized")
        except (WebSocketDisconnect, RuntimeError):
            pass
        return None
    await websocket.send_json({"message": "Password received"})
    return user


async def panel_control(panel: panel_conn, user: dict, info: dict):
    """Palancas a 10 Hz: se reenvían sin api_response. Solo se avisa si el paquete es inválido.
    Lo que pase de max_control_hz se descarta en silencio (antes de validar: tampoco genera avisos)."""
    if not panel.allow_control():
        return
    if time.monotonic() - panel.role_checked >= role_check_s:
        panel.role_checked = time.monotonic()
        if await segurity_bm.rol_efectivo_async(user) not in panel_roles:
            panel.push_event({"tipo": "control", "action": info.get("action"),
                              "api_response": {"message": "Role insuficiente"}})
            return
    action = info.get("action")
    machine = info.get("machine")
    try:
        if action != "movimiento":
            raise ValueError("Unknown action")
        if not isinstance(machine, str) or not machine:
            raise ValueError("machine is required")
        ejes = check_axes(info.get("ejes"))
    except ValueError as error:
        panel.push_event({"tipo": "control", "action": action, "api_response": {"message": str(error)}})
        return
    await manager.send_control(ejes, user.get("name"), machine)


async def panel_receiver(websocket: WebSocket, panel: panel_conn, user: dict):
    while True:
        text = await websocket.receive_text()
        if len(text.encode("utf-8")) > max_message_bytes:
            panel.push_event({"tipo": "comando", "action": None, "api_response": {"message": "Message too large"}})
            continue
        try:
            info = json.loads(text)
        except ValueError:
            continue    #un mensaje mal formado no tumba el panel
        if not isinstance(info, dict):
            continue
        if info.get("tipo") == "control":
            await panel_control(panel, user, info)
            continue
        action = info.get("action")
        answer = {"tipo": "comando", "action": action, "name": user.get("name"), "role": user.get("role")}
        answer["api_response"] = {"message": await panel_command(user, info, action)}
        panel.push_event(answer)


async def panel_command(user: dict, info: dict, action) -> str:
    """Valida y reenvía un comando del panel. Devuelve el texto de api_response."""
    if action not in drone_actions:
        return "Unknown action"
    if await segurity_bm.rol_efectivo_async(user) not in panel_roles:
        return "Role insuficiente"
    machine = info.get("machine")
    #"machine" elige el dron; sin él solo las broadcast_actions van a todos
    if machine is None and action not in broadcast_actions \
            or machine is not None and (not isinstance(machine, str) or not machine):
        return "Falta machine"
    try:
        fields = command_fields(info)
    except ValueError as error:
        return f"Invalid fields: {error}"
    drone = manager.drones.get(machine) if machine is not None else None
    if drone is not None and action not in drone.actions:
        return f"El dron {machine} no admite {action}"
    sent = await manager.send_command(action, user.get("name"), machine, fields)
    if sent and action == "iniciar_mision" and "mision" in fields:
        escritor_global.encolar(misiones_bm.crear_mision, machine, fields["mision"], user.get("name"))
    return f"Enviado a {sent} dron(es)" if sent else "Ningún dron conectado"


#"" bajo prefix="/ws" -> la ruta final es exactamente /ws (sin barra final)
@router.websocket("")
async def panel_endpoint(websocket: WebSocket):
    await websocket.accept()
    user = await panel_auth(websocket)
    if user is None:
        return
    panel = panel_conn(websocket, user.get("name"))
    manager.add_panel(panel)
    try:
        await ws_bm.run_until_first_done(panel.sender(), panel_receiver(websocket, panel, user))
    finally:
        manager.remove_panel(panel)


@router.get("/drones")
async def drones_status(token: dict = Depends(segurity_bm.verify_role_Depends_4)):
    return manager.status()
