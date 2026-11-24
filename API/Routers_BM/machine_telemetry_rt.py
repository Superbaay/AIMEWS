from fastapi import APIRouter, HTTPException, Depends
import random
from Library import base_model_bm, def_bm, segurity_bm
from Library.base_model_bm import db
from datetime import datetime, timezone



router = APIRouter(
    prefix="/machine_telemetry_rt",
    tags=["Machine Telemetry RT"]
)

@router.post("/test")
def get_machine_telemetry_rt(test: base_model_bm.auto_test_machine , token: dict = Depends(segurity_bm.verify_machine_or_role_4)):
    test.date = datetime.now(timezone.utc)
    test.name = token.get("name")
    db.machine_telemetry_init.insert_one(test.model_dump())
    return {"message": "Machine telemetry data inserted"}

@router.get("/tests")
def get_machine_telemetry_rt(token: dict = Depends(segurity_bm.verify_machine_or_role_4)):
    data = list(db.machine_telemetry_init.find({}))
    if not data:
        raise HTTPException(status_code=404, detail="No machine telemetry data found")
    for item in data:
        del item["_id"]
    test_data = list(data)
    return {"data": test_data}

@router.get("/sensor_data")
def get_machine_telemetry_sensor_data(machine_name: str | None = None, limit: int = 100,token: dict = Depends(segurity_bm.verify_machine_or_role_4)):
    filtro = {}
    if token.get("type") == "machine":
        filtro["machine_name"] = token.get("name")      # un dron solo ve lo suyo
    elif machine_name:
        filtro["machine_name"] = machine_name
    limit = min(limit, 5000)                             # tope para que nadie pida un millón
    data = list(db.machine_telemetry_data.find(filtro, {"_id": 0}).sort("date", -1).limit(limit))
    return {"data": data}


@router.post("/sensor_data")
def post_machine_telemetry_rt(data: base_model_bm.sensor_data, token: dict = Depends(segurity_bm.verify_machine_or_role_4)):
    data.date = datetime.now(timezone.utc)
    data.machine_name = token.get("name")
    db.machine_telemetry_data.insert_one(data.model_dump())
    return {"message": f"Machine telemetry data inserted "}

@router.post("/mision")
def post_machine_mision_data(data: base_model_bm.machine_mision_data, token: dict = Depends(segurity_bm.verify_machine_or_role_4)):
    name = db.machine_mision.find_one({"name": data.name})
    if name is not None:
        raise HTTPException(status_code=400, detail="Mission with this name already exists")
    data.last_update = datetime.now(timezone.utc)
    data.number = random.randint(1, 999999999)
    db.machine_mision.insert_one(data.model_dump())
    return {"message": f"Machine mision data inserted "}

@router.post("/mision_new_cordenates")
def post_mision_new_cordenates(number: base_model_bm.coordinates,name:str, token: dict = Depends(segurity_bm.verify_machine_or_role_4)):
    mision = db.machine_mision.find_one({"name": name})
    if not mision:
        raise HTTPException(status_code=404, detail="Mission not found")
    db.machine_mision.update_one({"name": name}, {"$push": {"coordinates_list": number.model_dump()}})
    mision = db.machine_mision.find_one({"name": name})
    return {"coordinates": mision.get("coordinates_list", [])}

@router.get("/mision_coordinates")
def get_mision_coordinates(name: str, token: dict = Depends(segurity_bm.verify_machine_or_role_4)):
    mision = db.machine_mision.find_one({"name": name})
    if not mision:
        raise HTTPException(status_code=404, detail="Mission not found")
    del mision["_id"]
    return {"coordinates": mision.get("coordinates_list", [])}

@router.get("/mision_list")
def get_mision_list(token: dict = Depends(segurity_bm.verify_machine_or_role_4)):
    mision_list = list(db.machine_mision.find({}))
    for m in mision_list:
        del m["_id"]
    return {"missions": [m.get("name") for m in mision_list]}