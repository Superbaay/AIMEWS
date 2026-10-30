from fastapi import FastAPI,HTTPException,WebSocket,WebSocketDisconnect,WebSocketException,status,Depends,File
from fastapi.security import OAuth2PasswordRequestForm
from  fastapi.middleware.cors import CORSMiddleware
from Library import base_model_bm
from Library import def_bm
from Library import segurity_bm
from Library import api_bm
from datetime import datetime,timezone
from pymongo import MongoClient
from jose import JWTError
from Routers_BM.user_rt import user
from Routers_BM.machine_rt import router as machine_rt

db = MongoClient()
app = FastAPI()
app.include_router(user)
app.include_router(machine_rt)


app.add_middleware(
    CORSMiddleware,
    allow_origin_regex=r"^https:\/\/.*\.superbaay\.org$",
    allow_origins=[
        "https://superbaay.org",
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
async def new_user(user:base_model_bm.user, token:dict = Depends(segurity_bm.verify_role_Depends_2)):
    if not segurity_bm.can_assign_role(token, user.role):
        raise HTTPException(status.HTTP_403_FORBIDDEN,detail="Role insuficiente")
    if def_bm.search_username(user.name)  != None:
        raise HTTPException(status.HTTP_409_CONFLICT,detail="User exist")
    user.date = datetime.now(timezone.utc)
    user.password = segurity_bm.encode_password(user.password)
    user_dict = user.model_dump()
    db.local.user.insert_one(user_dict)
    #Se responde sin la contraseña para no devolver el hash
    return base_model_bm.user_not_password(**user_dict)

list_of_users = []
@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    await websocket.send_json({"message": "WebSocket connection established"})
    await websocket.send_json({"message": "please send a password"})
    try: # Receive and validate the token from the client
        data = await websocket.receive_json() #DATA IS A TOKEN
        data = await segurity_bm.decode_token(data.get("token"))
        role = data.get("role")
        name = data.get("name")
        if not (role in ("O5-1", "4")):
            await websocket.send_json({"message": "Unauthorized"})
            await websocket.close()
            return
        if name is None or name == "":
            await websocket.send_json({"message": "Unauthorized"})
            await websocket.close()
            return
        list_of_users.append(data)
        print(f"Current list of users: {list_of_users}")
        await websocket.send_json({"message": "Password received"})
    except (WebSocketException, WebSocketDisconnect,JWTError) as e:
        await websocket.send_json({"message": f"WebSocket error: {e}"})
        await websocket.close()
    
        return
    try:
        while True:
            info = await websocket.receive_json()
            info["name"] = name
            info["role"] = role
            api_response = api_bm.api_function(info.get("action"))
            info["api_response"] = api_response
            await websocket.send_json(info)
    except WebSocketException as e:
        print(f"WebSocket error: {e}")
        
        await websocket.close()
    finally:
        list_of_users.remove(data)


