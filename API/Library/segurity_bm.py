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

async def decode_token(token: str):
    decode_user = jwt.decode(token, secret_key, algoticmo)
    return decode_user


async def verify_token_Depends(token:str = Depends(segurity)):
    return await verify_token(token)

async def verify_role_Depends_O5(token:base_model_bm.user = Depends(verify_token_Depends)):
    if token.get("role") == "O5-1":
        return token
    else:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED,detail="Role insuficiente")
    
async def verify_role_Depends_4(token:base_model_bm.user = Depends(verify_token_Depends)):
    if token.get("role") == "4" or token.get("role") == "O5-1":
        return token
    else:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED,detail="Role insuficiente")

async def verify_role_Depends_3(token:base_model_bm.user = Depends(verify_token_Depends)):
    if token.get("role") == "3" or token.get("role") == "O5-1" or token.get("role") == "4":
        return token
    else:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED,detail="Role insuficiente")

async def verify_role_Depends_2(token:base_model_bm.user = Depends(verify_token_Depends)):
    if token.get("role") == "2" or token.get("role") == "O5-1" or token.get("role") == "3" or token.get("role") == "4":
        return token
    else:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED,detail="Role insuficiente")

async def verify_role_Depends_1(token:base_model_bm.user = Depends(verify_token_Depends)):
    if token.get("role") == "1" or token.get("role") == "O5-1" or token.get("role") == "2" or token.get("role") == "3" or token.get("role") == "4":
        return token
    else:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED,detail="Role insuficiente")

#Jerarquia de roles: O5-1 > 4 > 3 > 2 > 1. Un rol desconocido (por ejemplo "admin") vale 0
role_rank = {"1": 1, "2": 2, "3": 3, "4": 4, "O5-1": 5}
top_rank = role_rank["O5-1"]

def rank_of(role):
    return role_rank.get(role, 0)

#Un usuario solo ve a los de su nivel o inferior
def can_see_user(token:dict, target_role):
    return rank_of(target_role) <= rank_of(token.get("role"))

#Solo gestiona a los de nivel inferior; O5-1 gestiona a todos
def can_manage_user(token:dict, target_role):
    own_rank = rank_of(token.get("role"))
    return own_rank == top_rank or rank_of(target_role) < own_rank

#Solo asigna roles inferiores al suyo; O5-1 asigna cualquiera
def can_assign_role(token:dict, role):
    if role not in role_rank:
        return False
    own_rank = rank_of(token.get("role"))
    return own_rank == top_rank or role_rank[role] < own_rank



