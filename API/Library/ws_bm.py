"""
Piezas comunes de los tres WebSockets de la API:

    /ws                    panel de control      (Routers_BM/drone_ws_rt.py)
    /ws/machine            dron ESP32-S3         (Routers_BM/drone_ws_rt.py)
    /camera_rt/ws/machine  ESP32-CAM             (Routers_BM/camera_rt.py)
    /camera_rt/ws/view     visor de la camara    (Routers_BM/camera_rt.py)

Los protocolos NO cambian por estar aqui: cada router sigue mandando sus mismos
JSON. La unica diferencia entre ellos es la clave del tipo de mensaje: el dron y
el panel usan "tipo" y la camara "type", por eso reject() la recibe como parametro.
"""
import asyncio
import json
from fastapi import WebSocket, WebSocketDisconnect, HTTPException, status
from Library import segurity_bm
from Library.db_bm import db

auth_timeout_s = 5      # segundos para mandar el primer mensaje con el token


def machine_exists(name: str) -> bool:
    """El token de maquina dura 24 h: comprueba que la maquina siga registrada."""
    return db.machine_rt.user.find_one({"name": name}, {"_id": 1}) is not None


async def reject(websocket: WebSocket, reason: str, clave_tipo: str):
    """Avisa {clave_tipo: "error", "message": reason} y cierra con 1008 (policy violation)."""
    try:
        await websocket.send_text(json.dumps({clave_tipo: "error", "message": reason}))
        await websocket.close(code=status.WS_1008_POLICY_VIOLATION, reason=reason)
    except (WebSocketDisconnect, RuntimeError):
        pass


async def read_first_message(websocket: WebSocket) -> tuple[str, dict]:
    """Espera el primer mensaje {"token": "...", ...}. Devuelve (token sin decodificar, mensaje).

    No captura nada; quien llama decide que hacer con cada error:
        WebSocketDisconnect   el cliente se fue
        asyncio.TimeoutError  no mando nada en auth_timeout_s
        ValueError            no era JSON, no era un objeto o faltaba el token
        KeyError              mando binario en vez de texto
    """
    first = await asyncio.wait_for(websocket.receive_json(), auth_timeout_s)
    if not isinstance(first, dict):
        raise ValueError("first message must be a JSON object")
    token = first.get("token")
    if not isinstance(token, str):
        raise ValueError("missing token")
    return token, first


async def receive_token(websocket: WebSocket, clave_tipo: str) -> tuple[dict, dict] | None:
    """Verificacion inicial de dron y camara. Devuelve (token decodificado, primer mensaje) o None.

    Si falla, ya rechazo la conexion con "Unauthorized" (salvo si el cliente se desconecto).
    """
    try:
        token, first = await read_first_message(websocket)
        return await segurity_bm.verify_token(token), first
    except WebSocketDisconnect:
        return None
    #HTTPException: token invalido o vencido; el resto, ver read_first_message
    except (asyncio.TimeoutError, ValueError, KeyError, HTTPException):
        await reject(websocket, "Unauthorized", clave_tipo)
        return None


class single_writer:
    """Un cliente con un solo escritor por socket: todo lo que se le manda pasa por sender().

    Los eventos se entregan todos, hasta max_events pendientes (un cliente colgado no
    acumula memoria sin limite). Cada subclase define flush() para mandar ademas su
    dato "solo el mas reciente" (telemetria en el panel, frame en el visor).
    """
    max_events = 100

    def __init__(self, websocket: WebSocket, name: str):
        self.websocket = websocket
        self.name = name
        self.events: list[dict] = []
        self.wakeup = asyncio.Event()

    def push_event(self, event: dict):
        if len(self.events) < self.max_events:
            self.events.append(event)
        self.wakeup.set()

    async def flush(self, events: list[dict]):
        for event in events:
            await self.websocket.send_text(json.dumps(event))

    async def sender(self):
        while True:
            await self.wakeup.wait()
            self.wakeup.clear()
            events, self.events = self.events, []
            await self.flush(events)


async def run_until_first_done(*coros):
    """Corre las corrutinas (sender + receiver) hasta que una termine y cancela el resto."""
    tasks = {asyncio.create_task(coro) for coro in coros}
    try:
        #Cuando una termina (el cliente cerro o recargo la pestaña) se cancela la otra
        done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        for task in done:
            task.exception()    # marca la excepcion como vista; la desconexion es el fin normal
    finally:
        for task in tasks:
            task.cancel()
