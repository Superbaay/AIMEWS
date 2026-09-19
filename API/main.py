from fastapi import FastAPI,HTTPException,WebSocket,WebSocketDisconnect,WebSocketException,status,Depends,File
from fastapi.security import OAuth2PasswordRequestForm
from  fastapi.middleware.cors import CORSMiddleware
from Library import base_model_bm
from Library import def_bm
from Library import segurity_bm
from datetime import datetime,timezone
from pymongo import MongoClient

db = MongoClient()
app = FastAPI()


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
async def new_user(user:base_model_bm.user):
    if def_bm.search_username(user.name)  != None:
        raise HTTPException(status.HTTP_409_CONFLICT,detail="User exist")
    user.date = datetime.now(timezone.utc)
    user.password = segurity_bm.encode_password(user.password)
    user.role = "admin"
    user_dict = user.model_dump()
    db.local.user.insert_one(user_dict)
    return user

