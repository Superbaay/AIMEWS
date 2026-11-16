import asyncio
import json
from fastapi import FastAPI,HTTPException,WebSocket,WebSocketDisconnect,WebSocketException,status,Depends,File
from fastapi.security import OAuth2PasswordRequestForm
from  fastapi.middleware.cors import CORSMiddleware
from Library import base_model_bm
from Library import def_bm
from Library import segurity_bm
from datetime import datetime,timezone
from Library.base_model_bm import db
from jose import JWTError
from Routers_BM.user_rt import user
from Routers_BM.machine_rt import router as machine_rt
from Routers_BM.enginer_log_rt import engineer
from Routers_BM.machine_telemetry_rt import router as machine_telemetry_rt
from Routers_BM.camera_rt import router as camera_rt
from Routers_BM import drone_ws_rt as drone_ws
app = FastAPI()
app.include_router(user)
app.include_router(machine_rt)
app.include_router(engineer)
app.include_router(machine_telemetry_rt)
app.include_router(camera_rt)
app.include_router(drone_ws.router)



app.add_middleware(
    CORSMiddleware,
    #Subdominios de superbaay.org y superbaay.dev (el sitio se publica por el tunel en superbaay.dev)
    allow_origin_regex=r"^https:\/\/([a-z0-9-]+\.)*superbaay\.(org|dev)$",
    allow_origins=[
        "https://superbaay.org",
        "https://superbaay.dev",
        "http://127.0.0.1:5500",
        "http://localhost:5500"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

@app.post("/token")
async def post_token(fomrdata:OAuth2PasswordRequestForm = Depends()):
    user = def_bm.search_username_exception(fomrdata.username)
    segurity_bm.verify_password(fomrdata.password,user.password)
    return segurity_bm.create_token(user)

@app.post("/user")
async def new_user(user:base_model_bm.user, token:dict  = Depends(segurity_bm.verify_role_Depends_O5)):
    if not segurity_bm.can_assign_role(token, user.role):
        raise HTTPException(status.HTTP_403_FORBIDDEN,detail="Role insuficiente")
    if def_bm.search_username(user.name)  != None:
        raise HTTPException(status.HTTP_409_CONFLICT,detail="User exist")
    user.date = datetime.now(timezone.utc)
    user.password = segurity_bm.encode_password(user.password)
    user_dict = user.model_dump()
    db.user.insert_one(user_dict)
    #Se responde sin la contraseña para no devolver el hash
    return base_model_bm.user_not_password(**user_dict)

#Panel de control <-> dron. El dron entra por /ws/machine (Routers_BM/drone_ws_rt.py);
#aquí entran los paneles y comparten el mismo manager para recibir su telemetría.
#Protocolo del panel (sitio_web/panel_control/main.js):
#   1. Primer mensaje {"token": "<token de /token>"}; sin él en 5 s, o con un token
#      inválido o un rol que no sea 4 / O5-1, se cierra con 1008.
#   2. Recibe lo que manda el dron: estado_dron, telemetria, autoprueba (con "machine").
#   3. Manda {"tipo": "comando", "action": "iniciar_autoprueba"} y recibe la respuesta
#      {"tipo": "comando", "action": ..., "api_response": {"message": ...}}.
panel_roles = ("O5-1", "4")

async def panel_auth(websocket: WebSocket) -> dict | None:
    await websocket.send_json({"message": "WebSocket connection established"})
    await websocket.send_json({"message": "please send a password"})
    try:
        data = await asyncio.wait_for(websocket.receive_json(), drone_ws.auth_timeout_s)
        token = data.get("token") if isinstance(data, dict) else None
        if not isinstance(token, str):
            raise ValueError("missing token")
        user = await segurity_bm.decode_token(token)
    except WebSocketDisconnect:
        return None
    #Antes un mensaje sin "token" lanzaba AttributeError fuera del except y tumbaba la conexion
    except (asyncio.TimeoutError, ValueError, KeyError, JWTError):
        user = None
    if user is None or user.get("type", "user") != "user" or user.get("role") not in panel_roles             or not user.get("name"):
        try:
            await websocket.send_json({"message": "Unauthorized"})
            await websocket.close(code=status.WS_1008_POLICY_VIOLATION, reason="Unauthorized")
        except (WebSocketDisconnect, RuntimeError):
            pass
        return None
    await websocket.send_json({"message": "Password received"})
    return user

async def panel_receiver(websocket: WebSocket, panel: drone_ws.panel_conn, user: dict):
    while True:
        try:
            info = json.loads(await websocket.receive_text())
        except ValueError:
            continue    #un mensaje mal formado no tumba el panel
        if not isinstance(info, dict):
            continue
        action = info.get("action")
        answer = {"tipo": "comando", "action": action, "name": user.get("name"), "role": user.get("role")}
        if action not in drone_ws.drone_actions:
            answer["api_response"] = {"message": "Unknown action"}
        else:
            sent = await drone_ws.manager.send_command(action, user.get("name"), info.get("machine"))
            answer["api_response"] = {"message": f"Enviado a {sent} dron(es)" if sent else "Ningún dron conectado"}
        panel.push_event(answer)

@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    user = await panel_auth(websocket)
    if user is None:
        return
    panel = drone_ws.panel_conn(websocket, user.get("name"))
    drone_ws.manager.add_panel(panel)
    tasks = {asyncio.create_task(panel.sender()), asyncio.create_task(panel_receiver(websocket, panel, user))}
    try:
        #Cuando una termina (el panel cerró o recargó la pestaña) se cancela la otra
        done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        for task in done:
            task.exception()    #marca la excepcion como vista; la desconexion es el fin normal
    finally:
        for task in tasks:
            task.cancel()
        drone_ws.manager.remove_panel(panel)
