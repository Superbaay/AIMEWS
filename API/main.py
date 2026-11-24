import asyncio
from contextlib import asynccontextmanager
from fastapi import FastAPI,HTTPException,status,Depends
from pymongo.errors import DuplicateKeyError
from fastapi.security import OAuth2PasswordRequestForm
from  fastapi.middleware.cors import CORSMiddleware
from Library import base_model_bm
from Library import def_bm
from Library import segurity_bm
from datetime import datetime,timezone
from Library.base_model_bm import db
from Routers_BM.user_rt import user
from Routers_BM.machine_rt import router as machine_rt
from Routers_BM.enginer_log_rt import engineer
from Routers_BM.machine_telemetry_rt import router as machine_telemetry_rt
from Routers_BM.camera_rt import router as camera_rt
from Routers_BM import drone_ws_rt as drone_ws
from Routers_BM.ota_rt import router as ota_rt
from Routers_BM.mission_rt import router as mission_rt
from Library import db_bm
from Library.escritor_bm import escritor_global, preparar_coleccion_telemetria


#lifespan: lo que corre una vez al arrancar la API (antes del yield) y al apagarla (después).
#Al arrancar: índices únicos, colección de telemetría y el escritor de fondo que guarda en Mongo
#lo que llega por los WebSockets. Al apagar: guarda lo que quedó pendiente en la cola.
@asynccontextmanager
async def lifespan(app: FastAPI):
    await asyncio.to_thread(db_bm.crear_indices)
    await asyncio.to_thread(preparar_coleccion_telemetria)
    escritor_global.iniciar()
    yield
    await escritor_global.detener()


app = FastAPI(lifespan=lifespan)
app.include_router(user)
app.include_router(machine_rt)
app.include_router(engineer)
app.include_router(machine_telemetry_rt)
app.include_router(camera_rt)
app.include_router(drone_ws.router)
app.include_router(ota_rt)
app.include_router(mission_rt)



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
def post_token(fomrdata:OAuth2PasswordRequestForm = Depends()):
    user = def_bm.search_username_exception(fomrdata.username)
    segurity_bm.verify_password(fomrdata.password,user.password)
    return segurity_bm.create_token(user)

@app.post("/user", response_model=base_model_bm.user_not_password)
def new_user(user:base_model_bm.user, token:dict  = Depends(segurity_bm.verify_role_Depends_O5)):
    if not segurity_bm.can_assign_role(token, user.role):
        raise HTTPException(status.HTTP_403_FORBIDDEN,detail="Role insuficiente")
    if def_bm.search_username(user.name)  != None:
        raise HTTPException(status.HTTP_409_CONFLICT,detail="User exist")
    user.date = datetime.now(timezone.utc)
    user.password = segurity_bm.encode_password(user.password)
    user_dict = user.model_dump()
    try:
        db.user.insert_one(user_dict)
    except DuplicateKeyError:
        #Dos altas con el mismo nombre a la vez: el índice único de user.name deja pasar solo una
        raise HTTPException(status.HTTP_409_CONFLICT,detail="User exist")
    #Se responde sin la contraseña para no devolver el hash
    return base_model_bm.user_not_password(**user_dict)

#Los WebSockets viven en sus routers: panel (/ws) y dron (/ws/machine) en
#Routers_BM/drone_ws_rt.py, camara en Routers_BM/camera_rt.py; lo comun en Library/ws_bm.py.
