from fastapi import APIRouter, Depends, HTTPException, status
from Library import base_model_bm,def_bm,segurity_bm
from datetime import datetime, timezone
from pymongo import MongoClient

db = MongoClient()
engineer_log_collection = db.local.engineer_log

engineer = APIRouter(
    prefix="/engineer_log",
    tags=["engineer Log"]
)

@engineer.post("/new")
def read_engineer_log(body:base_model_bm.engineer_log = Depends(segurity_bm.verify_role_Depends_4)):
    number = engineer_log_collection.count_documents({}) + 1
    body.onwer = "O5-1"
    body.number = number
    engineer_log_collection.insert_one(body.model_dump())
    http_exception = HTTPException(status_code=status.HTTP_201_CREATED, detail="Engineer log created")
    return http_exception

@engineer.get("/all")
def get_engineer_logs(nane:None = Depends(segurity_bm.verify_role_Depends_4)):
    logs = list(engineer_log_collection.find({}))
    return logs

@engineer.get("/number")
def get_engineer_log(number: int = Depends(segurity_bm.verify_role_Depends_4)):
    log = engineer_log_collection.find_one({"number": number})
    if not log:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Engineer log not found")
    return log

@engineer.delete("/number")
def delete_engineer_log(number: int = Depends(segurity_bm.verify_role_Depends_O5)):
    result = engineer_log_collection.delete_one({"number": number})
    logs = list(engineer_log_collection.find({}))
    number: int = 1
    for log in logs:
        log["number"] = number
        engineer_log_collection.update_one({"_id": log["_id"]}, {"$set": {"number": number}})
        number += 1
    if result.deleted_count == 0:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Engineer log not found")
    return {"message": "Engineer log deleted"}

