from fastapi import routes, Depends
from Library import base_model_bm, def_bm, segurity_bm
from pymongo import MongoClient

db = MongoClient()

router = routes.APIRouter(
    prefix="/machine_rt",
    tags=["machine_rt"]
)

@router.post("/get_machine_rt")
async def get_machine_rt(machine: base_model_bm.MachineRt = Depends(segurity_bm.verify_role_Depends_4)):
    collection = db["machine_rt"]
    result = collection.find_one({"machine_id": machine.machine_id})
    if result:
        return base_model_bm.MachineRt(**result)
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
