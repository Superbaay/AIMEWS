from fastapi import APIRouter, Depends, HTTPException,status
from Library import base_model_bm, def_bm, segurity_bm
from datetime import datetime, timezone
from Library.base_model_bm import db
from fastapi.security import OAuth2PasswordRequestForm 


router = APIRouter(
    prefix="/machine_rt",
    tags=["machine_rt"]
)

#El cuerpo o los parametros van sin valor por defecto; Depends solo entrega el token
@router.post("/token_machine")
def token_machine(requestform: OAuth2PasswordRequestForm = Depends()):
    true_machine = db.machine_rt.user.find_one({"name": requestform.username})
    if not true_machine:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, detail="Machine not found")
    segurity_bm.verify_password(requestform.password, true_machine["password"])
    token = segurity_bm.create_token(base_model_bm.machine(**true_machine))
    return {"token": token}

@router.get("/get_machine_rt", response_model=base_model_bm.machine_not_password)
def get_machine_rt(number: int, token: dict = Depends(segurity_bm.verify_role_Depends_4)):
    return def_bm.search_machine_rt_by_number(number)

@router.post("/new_machine_rt", response_model=base_model_bm.machine_not_password)
def new_machine_rt(machine: base_model_bm.machine_new, token: dict = Depends(segurity_bm.verify_role_Depends_O5)):
    return def_bm.new_machine_rt(machine)

@router.get("/search_machine_rt_not_password", response_model=base_model_bm.machine_not_password)
def search_machine_rt_not_password( name: str | None = None, token: dict = Depends(segurity_bm.verify_role_Depends_4)):
    return def_bm.search_machine_rt_not_password( name)

@router.get("/search_all_machine_rt_not", response_model=list[base_model_bm.machine_not_password])
def search_all_machine_rt_not(token: dict = Depends(segurity_bm.verify_role_Depends_4)):
    return def_bm.search_all_machine_rt_not()

@router.patch("/update_machine_rt", response_model=base_model_bm.machine_not_password)
def update_machine_rt(machine: base_model_bm.machine_update, token: dict = Depends(segurity_bm.verify_role_Depends_4)):
    return def_bm.update_machine_rt(machine)

@router.delete("/delete_machine_rt")
def delete_machine_rt(data: base_model_bm.machine_number, token: dict = Depends(segurity_bm.verify_role_Depends_O5)):
    return def_bm.delete_machine_rt(data.number)

#POST porque recibe un cuerpo; un GET con cuerpo lo descartan muchos clientes y proxies

