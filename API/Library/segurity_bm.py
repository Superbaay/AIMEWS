import asyncio
import os
import secrets
from pathlib import Path
from jose import jwt
import bcrypt
from fastapi import status,HTTPException,Depends
from datetime import datetime,timedelta,timezone
from fastapi.security import OAuth2PasswordBearer,OAuth2PasswordRequestForm
from Library import base_model_bm

algoticmo = "HS256"
# La clave que firma los JWT vive fuera de git, en API/secret_key.txt (está en .gitignore).
# Si no existe se genera una aleatoria. Cambiarla cierra todas las sesiones abiertas.
# Para pruebas, SAMATI_SECRET_KEY_FILE apunta a otro archivo y entonces API/secret_key.txt
# ni se lee ni se crea (se crea, si falta, el archivo de la variable)
ruta_secret_key = Path(os.environ.get('SAMATI_SECRET_KEY_FILE')
                       or Path(__file__).resolve().parent.parent / 'secret_key.txt')
if not ruta_secret_key.exists():
    if not ruta_secret_key.parent.is_dir():
        raise RuntimeError(f"No existe la carpeta de la clave JWT: {ruta_secret_key.parent} "
                           "(revisa SAMATI_SECRET_KEY_FILE)")
    #0600: solo el dueño puede leer la clave (en Windows os.open ignora casi todo el modo)
    descriptor = os.open(ruta_secret_key, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, 'w', encoding='utf-8') as archivo:
        archivo.write(secrets.token_urlsafe(48))
secret_key = ruta_secret_key.read_text(encoding='utf-8').strip()
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

#Token
#El type sale del modelo: base_model_bm.user da "user" y base_model_bm.machine da "machine"
def create_token(user:base_model_bm.user | base_model_bm.machine):
    expire_time = datetime.now(timezone.utc) + timedelta(hours=expire_time_hours)
    playload ={
        "name" : user.name,
        "exp" : expire_time,
        "role" : user.role,
        "type" : user.type
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

#Solo personas. Un token sin "type" es de antes del cambio y solo podia ser de usuario
async def verify_user_Depends(token:dict = Depends(verify_token_Depends)):
    if token.get("type", "user") != "user":
        raise HTTPException(status.HTTP_403_FORBIDDEN,detail="Machine tokens cannot access user routes")
    return token

#Jerarquia de roles: O5-1 > 4 > 3 > 2 > 1. Un rol desconocido (por ejemplo "admin") vale 0
role_rank = {"1": 1, "2": 2, "3": 3, "4": 4, "O5-1": 5}
top_rank = role_rank["O5-1"]

def rank_of(role):
    return role_rank.get(role, 0)

#El rol viaja dentro del token, que dura 24 h. Si bajas de rol a alguien (o lo borras), con solo
#el token seguiría teniendo el rol viejo hasta que expire. Por eso cada ruta con rol vuelve a leer
#el rol en la base y usa el MENOR de los dos: bajar de rol tiene efecto inmediato, y subir de rol
#pide volver a iniciar sesión (el token nuevo trae el rol nuevo). Es una consulta pequeña por petición.
def rol_efectivo(token: dict) -> str | None:
    """Rol con el que se atiende esta petición, o None si el usuario ya no existe. Bloquea (pymongo):
    desde código async se llama con asyncio.to_thread."""
    from Library.db_bm import db    # import local: db_bm no depende de este archivo, pero así se ve de dónde sale
    guardado = db.user.find_one({"name": token.get("name")}, {"role": 1})
    if guardado is None:
        return None
    rol_token, rol_base = token.get("role"), guardado.get("role")
    return rol_token if rank_of(rol_token) <= rank_of(rol_base) else rol_base

async def rol_efectivo_async(token: dict) -> str | None:
    return await asyncio.to_thread(rol_efectivo, token)

#Fabrica de dependencias por rol minimo: deja pasar al rol pedido y a todos los superiores.
#Va encima de verify_user_Depends, asi los tokens de maquina siguen bloqueados.
#Un rol desconocido vale 0 y nunca llega al minimo, igual que antes con las cadenas de "or"
def requiere_rol(rol_minimo: str):
    rango_minimo = role_rank[rol_minimo]
    async def verificar_rol(token:dict = Depends(verify_user_Depends)):
        rol = await rol_efectivo_async(token)
        if rol is None:
            #401: el panel lo toma como sesión terminada y vuelve al login, que es lo correcto
            raise HTTPException(status.HTTP_401_UNAUTHORIZED,detail="User not found")
        if rank_of(rol) < rango_minimo:
            raise HTTPException(status.HTTP_403_FORBIDDEN,detail="Role insuficiente")
        #Las rutas deciden con token["role"] (can_see_user, can_manage_user...): va el rol vigente
        return {**token, "role": rol}
    return verificar_rol

#Alias con los nombres de siempre: los routers los importan por nombre
verify_role_Depends_O5 = requiere_rol("O5-1")
verify_role_Depends_4 = requiere_rol("4")
verify_role_Depends_3 = requiere_rol("3")
verify_role_Depends_2 = requiere_rol("2")
verify_role_Depends_1 = requiere_rol("1")

#Rutas que usan tanto el dron como los ingenieros desde el panel (telemetria, misiones)
async def verify_machine_or_role_4(token:dict = Depends(verify_token_Depends)):
    if token.get("type") == "machine":
        if token.get("role") != "4":
            raise HTTPException(status.HTTP_403_FORBIDDEN, detail="Role insuficiente")
        #Igual que en el WebSocket: una máquina borrada no sigue usando su token de 24 h
        from Library import ws_bm
        if not await asyncio.to_thread(ws_bm.machine_exists, token.get("name")):
            raise HTTPException(status.HTTP_403_FORBIDDEN, detail="Machine not found")
        return token
    if token.get("type", "user") != "user":
        raise HTTPException(status.HTTP_403_FORBIDDEN, detail="Role insuficiente")
    rol = await rol_efectivo_async(token)
    if rol is None:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, detail="User not found")
    if rol not in ("4", "O5-1"):
        raise HTTPException(status.HTTP_403_FORBIDDEN, detail="Role insuficiente")
    return {**token, "role": rol}

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
