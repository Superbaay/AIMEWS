"""
Actualización del firmware del dron por OTA (Over The Air).

Flujo:
    1. Un O5-1 sube el .bin compilado (build/Dron_telemetria_ws.bin) con POST /ota/upload.
       La API lo valida leyendo la cabecera de imagen de ESP-IDF: así se rechaza un
       archivo que no sea firmware de ESP32-S3 y la versión sale del propio binario
       (PROJECT_VER del CMakeLists), no de lo que escriba el usuario.
    2. El O5-1 elige firmware y dron y llama POST /ota/deploy. La API le manda al dron
       por su WebSocket: {"tipo": "comando", "action": "ota", "url": "/ota/firmware/3",
       "version": "0.3.0", "size": 1070384, "from": "superbaay"}
    3. El dron descarga GET /ota/firmware/{number} con SU token de máquina, lo escribe
       en la partición OTA libre y reporta el avance como {"tipo": "ota", ...}, que el
       router del WebSocket reenvía a los paneles.
    4. Reinicia con la versión nueva. Si en 2 minutos no logra conectarse a la API, el
       bootloader vuelve solo a la versión anterior (rollback).

Todo es solo para O5-1, salvo la descarga, que también acepta tokens de máquina.

Despliegue desde la PC sin usuario (enbedded/tools/ota_subir.py, lo usa Claude): upload, all,
deploy y status aceptan también la cabecera X-OTA-Key con la clave de API/ota_deploy_key.txt.
Ese archivo se genera solo, está en .gitignore y nunca sale de la PC: el script lo lee del
disco, así nadie tiene que escribir (ni ver) una contraseña.
"""
import hashlib
import os
import re
import secrets
import struct
import uuid
from datetime import datetime, timezone
from pathlib import Path

import aiofiles
from fastapi import APIRouter, Depends, File, Form, Header, HTTPException, UploadFile, status
from fastapi.responses import FileResponse
from pydantic import BaseModel
from pymongo import ReturnDocument

from Library import segurity_bm, ws_bm
from Library.db_bm import db
from Routers_BM.drone_ws_rt import manager

ota_collection = db.ota_firmware
upload_dir = Path(__file__).resolve().parent.parent / "uploads" / "ota"
disk_name_pattern = re.compile(r"[0-9a-f]{32}\.bin")
#Partición ota_0/ota_1 de 4 MB en el firmware (partitions.csv): un .bin mayor no cabe
max_size = 4 * 1024 * 1024
chunk_size = 256 * 1024

#Cabecera de imagen de ESP-IDF (esp_image_format.h / esp_app_format.h)
image_magic = 0xE9
chip_id_esp32s3 = 9
app_desc_offset = 24 + 8          # cabecera de imagen + cabecera del primer segmento
app_desc_magic = 0xABCD5432
header_len = app_desc_offset + 256

#Igual que secret_key.txt: fuera de git, se crea sola. SAMATI_OTA_KEY_FILE la cambia (pruebas)
ruta_clave_ota = Path(os.environ.get("SAMATI_OTA_KEY_FILE")
                      or Path(__file__).resolve().parent.parent / "ota_deploy_key.txt")
if not ruta_clave_ota.exists():
    descriptor = os.open(ruta_clave_ota, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as archivo:
        archivo.write(secrets.token_urlsafe(32))
clave_ota = ruta_clave_ota.read_text(encoding="utf-8").strip()

router = APIRouter(
    prefix="/ota",
    tags=["OTA"]
)


class deploy_request(BaseModel):
    number: int
    machine: str


def leer_texto_c(datos: bytes) -> str:
    """Cadena de C de tamaño fijo: hasta el primer byte 0."""
    return datos.split(b"\0", 1)[0].decode("utf-8", "replace")


def leer_descripcion(cabecera: bytes) -> dict:
    """Valida que sea una app de ESP32-S3 y devuelve versión, proyecto y fecha de compilación.
    Lanza HTTPException 415 si no lo es."""
    if len(cabecera) < header_len or cabecera[0] != image_magic:
        raise HTTPException(status.HTTP_415_UNSUPPORTED_MEDIA_TYPE, detail="Not an ESP-IDF app image (.bin)")
    chip_id = struct.unpack_from("<H", cabecera, 12)[0]
    if chip_id != chip_id_esp32s3:
        raise HTTPException(status.HTTP_415_UNSUPPORTED_MEDIA_TYPE, detail="Image is not for ESP32-S3")
    magic = struct.unpack_from("<I", cabecera, app_desc_offset)[0]
    if magic != app_desc_magic:
        raise HTTPException(status.HTTP_415_UNSUPPORTED_MEDIA_TYPE,
                            detail="Missing app description (use build/<project>.bin, not the bootloader)")
    d = app_desc_offset
    return {
        "version": leer_texto_c(cabecera[d + 16:d + 48]),
        "project_name": leer_texto_c(cabecera[d + 48:d + 80]),
        "build_time": leer_texto_c(cabecera[d + 80:d + 96]),
        "build_date": leer_texto_c(cabecera[d + 96:d + 112]),
        "idf_version": leer_texto_c(cabecera[d + 112:d + 144]),
    }


#O5-1 con su JWT, o el script de la PC con X-OTA-Key. Devuelve un "token" con el nombre para el registro
async def verify_O5_or_ota_key(x_ota_key: str | None = Header(None),
                               authorization: str | None = Header(None)):
    if x_ota_key is not None:
        #compare_digest: tarda lo mismo acierte o no, así no se adivina la clave por tiempos
        if not secrets.compare_digest(x_ota_key.encode(), clave_ota.encode()):
            raise HTTPException(status.HTTP_401_UNAUTHORIZED, detail="Invalid OTA key")
        return {"name": "clave_ota", "role": "O5-1", "type": "ota_key"}
    if not authorization or not authorization.lower().startswith("bearer "):
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, detail="Not authenticated")
    token = await segurity_bm.verify_token(authorization[7:].strip())
    return await segurity_bm.requiere_rol("O5-1")(await segurity_bm.verify_user_Depends(token))


#El dron descarga con su token de máquina; un O5-1 también puede bajarlo desde el panel
async def verify_machine_or_O5(token: dict = Depends(segurity_bm.verify_token_Depends)):
    if token.get("type") == "machine" and token.get("role") == "4":
        return token
    if token.get("type", "user") == "user" and token.get("role") == "O5-1":
        return token
    raise HTTPException(status.HTTP_403_FORBIDDEN, detail="Role insuficiente")


@router.post("/upload", status_code=status.HTTP_201_CREATED)
async def upload_firmware(file: UploadFile = File(...), notes: str | None = Form(None),
                          token: dict = Depends(verify_O5_or_ota_key)):
    cabecera = await file.read(header_len)
    descripcion = leer_descripcion(cabecera)
    upload_dir.mkdir(parents=True, exist_ok=True)
    disk_name = f"{uuid.uuid4().hex}.bin"
    disk_path = upload_dir / disk_name
    sha256 = hashlib.sha256(cabecera)
    size = len(cabecera)
    try:
        async with aiofiles.open(disk_path, "wb") as out:
            await out.write(cabecera)
            while chunk := await file.read(chunk_size):
                size += len(chunk)
                if size > max_size:
                    raise HTTPException(status.HTTP_413_CONTENT_TOO_LARGE, detail="Firmware larger than the OTA partition (4 MB)")
                sha256.update(chunk)
                await out.write(chunk)
    except HTTPException:
        disk_path.unlink(missing_ok=True)
        raise
    #El número no se reutiliza al borrar (ni siquiera el último): un contador aparte que solo sube.
    #Así una orden de OTA vieja nunca apunta a otro archivo
    contador = db.counters.find_one_and_update({"_id": "ota_firmware"}, {"$inc": {"value": 1}},
                                               upsert=True, return_document=ReturnDocument.AFTER)
    number = contador["value"]
    documento = {
        "number": number,
        **descripcion,
        "size": size,
        "sha256": sha256.hexdigest(),
        "filename": file.filename or disk_name,
        "disk_name": disk_name,
        "notes": (notes or "").strip()[:500] or None,
        "onwer": token.get("name"),
        "date": datetime.now(timezone.utc),
    }
    ota_collection.insert_one(documento)
    documento.pop("_id", None)
    documento.pop("disk_name")
    return documento


@router.get("/all")
def get_firmwares(token: dict = Depends(verify_O5_or_ota_key)):
    return list(ota_collection.find({}, {"_id": 0, "disk_name": 0}).sort("number", -1))


@router.delete("/number")
def delete_firmware(number: int, token: dict = Depends(segurity_bm.verify_role_Depends_O5)):
    borrado = ota_collection.find_one_and_delete({"number": number})
    if not borrado:
        raise HTTPException(status.HTTP_404_NOT_FOUND, detail="Firmware not found")
    disk_name = borrado.get("disk_name", "")
    if disk_name_pattern.fullmatch(disk_name):
        try:
            (upload_dir / disk_name).unlink(missing_ok=True)
        except OSError:
            pass
    return {"message": "Firmware deleted", "number": number}


@router.post("/deploy")
async def deploy_firmware(body: deploy_request, token: dict = Depends(verify_O5_or_ota_key)):
    firmware = ota_collection.find_one({"number": body.number}, {"_id": 0})
    if not firmware:
        raise HTTPException(status.HTTP_404_NOT_FOUND, detail="Firmware not found")
    drone = manager.drones.get(body.machine)
    if drone is None:
        raise HTTPException(status.HTTP_409_CONFLICT, detail=f"Drone {body.machine} is not connected")
    #Solo los firmwares con OTA declaran "ota" en estado_dron.acciones; a uno viejo no se le manda
    if "ota" not in drone.actions:
        raise HTTPException(status.HTTP_409_CONFLICT, detail=f"Drone {body.machine} firmware does not support OTA")
    #Plano y corto: el lector JSON del firmware solo acepta mensajes de menos de 512 bytes
    command = {
        "tipo": "comando", "action": "ota",
        "url": f"/ota/firmware/{firmware['number']}",
        "version": firmware["version"],
        "size": firmware["size"],
        "from": token.get("name"),
    }
    if not await drone.send(command):
        raise HTTPException(status.HTTP_409_CONFLICT, detail=f"Drone {body.machine} disconnected")
    evento = {"tipo": "ota", "estado": "enviado", "machine": body.machine,
              "version": firmware["version"], "from": token.get("name")}
    drone.last_ota = evento   # borra el "confirmado" de una OTA anterior para GET /ota/status
    manager.broadcast_event(evento)
    return {"message": "OTA sent", "machine": body.machine, "version": firmware["version"]}


@router.get("/firmware/{number}")
async def download_firmware(number: int, token: dict = Depends(verify_machine_or_O5)):
    #Igual que en el WebSocket: una máquina borrada no puede seguir bajando firmware con un token viejo
    if token.get("type") == "machine" and not ws_bm.machine_exists(token.get("name")):
        raise HTTPException(status.HTTP_403_FORBIDDEN, detail="Machine not found")
    firmware = ota_collection.find_one({"number": number}, {"disk_name": 1, "version": 1})
    disk_name = (firmware or {}).get("disk_name", "")
    disk_path = upload_dir / disk_name
    if not firmware or not disk_name_pattern.fullmatch(disk_name) or not disk_path.is_file():
        raise HTTPException(status.HTTP_404_NOT_FOUND, detail="Firmware not found")
    return FileResponse(disk_path, media_type="application/octet-stream",
                        filename=f"firmware_{firmware.get('version', number)}.bin")


@router.get("/status")
def ota_status(machine: str, token: dict = Depends(verify_O5_or_ota_key)):
    """Para el script de la PC: versión que reporta el dron y su último aviso de OTA."""
    drone = manager.drones.get(machine)
    if drone is None:
        return {"machine": machine, "connected": False, "version": None, "ota": None}
    return {"machine": machine, "connected": True, "version": (drone.state or {}).get("version"),
            "acciones": sorted(drone.actions), "ota": drone.last_ota}
