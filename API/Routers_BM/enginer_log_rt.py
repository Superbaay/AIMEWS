from fastapi import APIRouter, Depends, HTTPException, status, UploadFile, File
from fastapi.responses import FileResponse
from Library import base_model_bm,def_bm,segurity_bm
from datetime import datetime, timezone
from pymongo import MongoClient
from pathlib import Path
import re
import uuid
import aiofiles
import filetype

db = MongoClient()
engineer_log_collection = db.local.engineer_log

#Evidencia: el archivo va a disco (API/uploads/engineer_log) y en Mongo solo se guarda el BaseModel media
upload_dir = Path(__file__).resolve().parent.parent / "uploads" / "engineer_log"
#video/quicktime (.mov) es el formato de video del iPhone; image/webp lo usan varias galerias de Android
allowed_types = {"image/jpeg": "jpg", "image/png": "png", "image/webp": "webp", "video/mp4": "mp4", "video/quicktime": "mov", "application/pdf": "pdf"}
#Solo nombres generados por upload_evidence: uuid hex + extension permitida, sin rutas
file_name_pattern = re.compile(rf"[0-9a-f]{{32}}\.({'|'.join(allowed_types.values())})")
max_size = 200 * 1024 * 1024
chunk_size = 1024 * 1024

engineer = APIRouter(
    prefix="/engineer_log",
    tags=["engineer Log"]
)

@engineer.post("/new", status_code=status.HTTP_201_CREATED)
def read_engineer_log(body:base_model_bm.engineer_log , token: dict = Depends(segurity_bm.verify_role_Depends_4)):
    number = engineer_log_collection.count_documents({}) + 1
    body.onwer = token.get("name")
    body.number = number
    body.date_creation = datetime.now(timezone.utc)
    engineer_log_collection.insert_one(body.model_dump())
    return {"message": "Engineer log created", "number": number}

#{"_id": 0} excluye el ObjectId de Mongo, que FastAPI no puede convertir a JSON
@engineer.get("/all")
def get_engineer_logs(token: dict = Depends(segurity_bm.verify_role_Depends_4)):
    logs = list(engineer_log_collection.find({}, {"_id": 0}))
    return logs

@engineer.get("/number")
def get_engineer_log(number: int, token: dict = Depends(segurity_bm.verify_role_Depends_4)):
    log = engineer_log_collection.find_one({"number": number}, {"_id": 0})
    if not log:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Engineer log not found")
    return log

@engineer.delete("/number")
def delete_engineer_log(number: int, token: dict = Depends(segurity_bm.verify_role_Depends_O5)):
    #find_one_and_delete devuelve el documento borrado, asi se sabe que evidencia tenia
    deleted_log = engineer_log_collection.find_one_and_delete({"number": number})
    if not deleted_log:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Engineer log not found")
    logs = list(engineer_log_collection.find({}).sort("number", 1))
    number: int = 1
    for log in logs:
        log["number"] = number
        engineer_log_collection.update_one({"_id": log["_id"]}, {"$set": {"number": number}})
        number += 1
    #Solo se borran nombres generados por upload_evidence: un url_path manipulado no puede apuntar fuera de upload_dir
    files_deleted = 0
    for media in deleted_log.get("evidence") or []:
        file_name = Path(media.get("url_path", "")).name
        if not file_name_pattern.fullmatch(file_name):
            continue
        try:
            (upload_dir / file_name).unlink(missing_ok=True)
            files_deleted += 1
        except OSError:
            #En Windows falla si el archivo esta abierto; el log ya se borro, el archivo queda huerfano
            pass
    return {"message": "Engineer log deleted", "files_deleted": files_deleted}

#multipart/form-data: el archivo llega en el campo "file" y el numero del log en la query (?number=1)
@engineer.post("/evidence", status_code=status.HTTP_201_CREATED)
async def upload_evidence(number: int, file: UploadFile = File(...), token: dict = Depends(segurity_bm.verify_role_Depends_4)):
    log = engineer_log_collection.find_one({"number": number}, {"evidence": 1})
    if not log:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Engineer log not found")
    #El tipo se lee de los primeros bytes; file.content_type y file.filename los envia el cliente y no son fiables
    head = await file.read(261)
    kind = filetype.guess(head)
    if kind is None or kind.mime not in allowed_types:
        raise HTTPException(status_code=status.HTTP_415_UNSUPPORTED_MEDIA_TYPE, detail="File type not allowed")
    #Nombre en disco con uuid: el nombre original podria traer rutas como ../../main.py
    upload_dir.mkdir(parents=True, exist_ok=True)
    disk_name = f"{uuid.uuid4().hex}.{allowed_types[kind.mime]}"
    disk_path = upload_dir / disk_name
    size = len(head)
    try:
        async with aiofiles.open(disk_path, "wb") as out:
            await out.write(head)
            while chunk := await file.read(chunk_size):
                size += len(chunk)
                if size > max_size:
                    raise HTTPException(status_code=status.HTTP_413_CONTENT_TOO_LARGE, detail="File too large")
                await out.write(chunk)
    except HTTPException:
        disk_path.unlink(missing_ok=True)
        raise
    media = base_model_bm.media(
        filename=file.filename or disk_name,
        content_type=kind.mime,
        url_path=f"/engineer_log/files/{disk_name}",
        onwer=token.get("name"),
    )
    #$push falla si evidence es null (el BaseModel lo permite), en ese caso se crea la lista
    if log.get("evidence") is None:
        change = {"$set": {"evidence": [media.model_dump()]}}
    else:
        change = {"$push": {"evidence": media.model_dump()}}
    change.setdefault("$set", {})["date_update"] = datetime.now(timezone.utc)
    engineer_log_collection.update_one({"number": number}, change)
    return media

#Protegido con token: la web lo pide con fetch (un <img src> no puede enviar Authorization)
@engineer.get("/files/{file_name}")
def get_evidence_file(file_name: str, token: dict = Depends(segurity_bm.verify_role_Depends_4)):
    disk_path = upload_dir / file_name
    if not file_name_pattern.fullmatch(file_name) or not disk_path.is_file():
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="File not found")
    return FileResponse(disk_path)

