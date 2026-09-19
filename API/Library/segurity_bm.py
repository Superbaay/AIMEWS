from jose import jwt
import bcrypt
from fastapi import status,HTTPException,Depends
from datetime import datetime,timedelta,timezone
from fastapi.security import OAuth2PasswordBearer,OAuth2PasswordRequestForm
from Library import base_model_bm
from Library import def_bm

algoticmo = "HS256"
secret_key = "hhdopiqjdiuhmvnbmlañqoryhrtmvjdklfgterweqczmxñpolgihnvb"
expire_time_hours = 24
segurity = OAuth2PasswordBearer("token")

def encode_password(password:str):
    salt = bcrypt.gensalt()
    password_encode = bcrypt.hashpw(password.encode("utf-8"),salt)
    return password_encode.decode("utf-8")

def verify_password(password:str,password_hashed):
    check = bcrypt.checkpw(password.encode("utf-8"),password_hashed.encode("utf-8"))
    if check == False:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED,detail="It isnt Password")
    return check

def veryfy_user( user : str, password : str ):
    full_user = def_bm.search_username_exception(user)
    verify_password(password,full_user.password)

#Token
def create_token(user:base_model_bm.user):
    expire_time = datetime.now(timezone.utc) + timedelta(hours=expire_time_hours)
    playload ={
        "name" : user.name,
        "exp" : expire_time,
        "role" : user.role
    }
    return jwt.encode(playload,secret_key,algoticmo)

async def verify_token(token : str) :
    try:
        decode_user = jwt.decode(token,secret_key,algoticmo)
        return decode_user
    except:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED,detail="Token expirado")

async def verify_token_Depends(token:str = Depends(segurity)):
    return await verify_token(token)

async def verify_role_Depends(token:base_model_bm.user = Depends(verify_token_Depends)):
    if token.get("role") == "admin":
        return token
    else:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED,detail="Role insuficiente")






