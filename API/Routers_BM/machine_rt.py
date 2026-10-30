from fastapi import APIRouter, Depends
from Library import base_model_bm, def_bm, segurity_bm
from datetime import datetime, timezone
from pymongo import MongoClient

db = MongoClient()

router = APIRouter(
    prefix="/machine_rt",
    tags=["machine_rt"]
)

@router.get("/get_machine_rt")
async def get_machine_rt(machine: base_model_bm.machine = Depends(segurity_bm.verify_role_Depends_4)):
    collection = db.local.machine_rt.user
    result = collection.find_one({"number": machine.number})
    if result:
        return base_model_bm.machine(**result)
    return None

@router.post("/new_machine_rt")
async def new_machine_rt(machine: base_model_bm.machine = Depends(segurity_bm.verify_role_Depends_O5)):
    return def_bm.new_machine_rt(machine)

@router.get("/search_machine_rt_not_password")
async def search_machine_rt_not_password(number: int, name: str | None = Depends(segurity_bm.verify_role_Depends_4)):
    return def_bm.search_machine_rt_not_password(number, name)

@router.get("/search_all_machine_rt_not")
async def search_all_machine_rt_not(data: None = Depends(segurity_bm.verify_role_Depends_4)):
    return def_bm.search_all_machine_rt_not()

@router.patch("/update_machine_rt")
async def update_machine_rt(machine: base_model_bm.machine = Depends(segurity_bm.verify_role_Depends_4)):
    return def_bm.update_machine_rt(machine)

@router.delete("/delete_machine_rt")
async def delete_machine_rt(number: int = Depends(segurity_bm.verify_role_Depends_O5)):
    return def_bm.delete_machine_rt(number)

@router.get("/test_all_sensors")
async def test_all_sensors(data: base_model_bm.auto_test_machine = Depends(segurity_bm.verify_role_Depends_4)):
    data.date = datetime.now(timezone.utc)
    def_bm.search_machine_rt_exception(data.name)
    db.local.machine_rt.test.sensors.insert_one(data.model_dump())
    return data