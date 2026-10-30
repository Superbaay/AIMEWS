from fastapi import APIRouter, Depends
from Library import base_model_bm, def_bm, segurity_bm
from datetime import datetime, timezone
from pymongo import MongoClient

db = MongoClient()

router = APIRouter(
    prefix="/machine_rt",
    tags=["machine_rt"]
)

#El cuerpo o los parametros van sin valor por defecto; Depends solo entrega el token
@router.get("/get_machine_rt")
async def get_machine_rt(number: int, token: dict = Depends(segurity_bm.verify_role_Depends_4)):
    return def_bm.search_machine_rt_by_number(number)

@router.post("/new_machine_rt")
async def new_machine_rt(machine: base_model_bm.machine_new, token: dict = Depends(segurity_bm.verify_role_Depends_O5)):
    return def_bm.new_machine_rt(machine)

@router.get("/search_machine_rt_not_password")
async def search_machine_rt_not_password(number: int, name: str | None = None, token: dict = Depends(segurity_bm.verify_role_Depends_4)):
    return def_bm.search_machine_rt_not_password(number, name)

@router.get("/search_all_machine_rt_not")
async def search_all_machine_rt_not(token: dict = Depends(segurity_bm.verify_role_Depends_4)):
    return def_bm.search_all_machine_rt_not()

@router.patch("/update_machine_rt")
async def update_machine_rt(machine: base_model_bm.machine_update, token: dict = Depends(segurity_bm.verify_role_Depends_4)):
    return def_bm.update_machine_rt(machine)

@router.delete("/delete_machine_rt")
async def delete_machine_rt(data: base_model_bm.machine_number, token: dict = Depends(segurity_bm.verify_role_Depends_O5)):
    return def_bm.delete_machine_rt(data.number)

#POST porque recibe un cuerpo; un GET con cuerpo lo descartan muchos clientes y proxies
@router.post("/test_all_sensors")
async def test_all_sensors(data: base_model_bm.auto_test_machine, token: dict = Depends(segurity_bm.verify_role_Depends_4)):
    data.date = datetime.now(timezone.utc)
    def_bm.search_machine_rt_exception(data.name, None)
    db.local.machine_rt.test.sensors.insert_one(data.model_dump())
    return data
