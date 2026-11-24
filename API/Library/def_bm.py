from Library.db_bm import db, siguiente_numero
from pymongo.errors import DuplicateKeyError
from fastapi import status,HTTPException
from Library import base_model_bm
from datetime import datetime, timedelta, timezone


#Busca un documento y lo devuelve como modelo. Sin error_404 devuelve None si no existe;
#con error_404 lanza 404 con ese texto. quitar_password borra el campo antes de crear el modelo
def _buscar_uno(coleccion, filtro:dict, modelo, quitar_password:bool = False, error_404:str | None = None):
    documento = coleccion.find_one(filtro)
    if documento == None :
        if error_404 == None :
            return None
        raise HTTPException(status.HTTP_404_NOT_FOUND,detail=error_404)
    del documento["_id"]
    if quitar_password :
        del documento["password"]
    return modelo(**documento)

def search_username(username:str):
    return _buscar_uno(db.user, {"name" : username}, base_model_bm.user)

def search_username_exception(username:str):
    return _buscar_uno(db.user, {"name" : username}, base_model_bm.user, error_404="User dont found")

def search_username_not_password(username:str):
    return _buscar_uno(db.user, {"name" : username}, base_model_bm.user_not_password,
                       quitar_password=True, error_404="User dont found")

# Busqueda de maquinas por numero

def search_machine_rt_by_number(number:int):
    return _buscar_uno(db.machine_rt.user, {"number" : number}, base_model_bm.machine_not_password,
                       error_404="Machine dont found")

def new_machine_rt(machine_data: base_model_bm.machine_new):
    machine = db.machine_rt.user.find_one({"name" : machine_data.name})
    if machine != None :
        raise HTTPException(status.HTTP_409_CONFLICT,detail="Machine already exists")
    #Número que nunca se reutiliza (ver db_bm.siguiente_numero)
    number = siguiente_numero("machine_rt", db.machine_rt.user)
    new_machine = machine_data.model_dump()
    #Import local: evita un ciclo de imports si segurity_bm vuelve a importar def_bm
    from Library import segurity_bm
    new_machine["password"] = segurity_bm.encode_password(machine_data.password)
    new_machine["number"] = number
    new_machine["role"] = "4"
    new_machine["date_creation"] = datetime.now(timezone.utc)
    try:
        db.machine_rt.user.insert_one(new_machine)
    except DuplicateKeyError:
        #Dos altas con el mismo nombre al mismo tiempo: el índice único deja pasar solo una
        raise HTTPException(status.HTTP_409_CONFLICT,detail="Machine already exists")
    del new_machine["_id"]
    machine_bm = base_model_bm.machine_not_password(**new_machine)
    return machine_bm

def search_machine_rt_not_password(name:str | None = None):
    return _buscar_uno(db.machine_rt.user, {"name" : name}, base_model_bm.machine_not_password,
                       quitar_password=True, error_404="Machine dont found")

def search_all_machine_rt_not():
    all_machines = db.machine_rt.user.find()
    machines_list = []
    for machine in all_machines:
        del machine["_id"]
        if "password" in machine:
            del machine["password"]
        machines_list.append(base_model_bm.machine_not_password(**machine))
    return machines_list

#Solo cambia nombre, modelo y descripcion; contraseña, rol y fechas de alta no se tocan
def update_machine_rt(machine_data: base_model_bm.machine_update):
    machine = db.machine_rt.user.find_one({"number" : machine_data.number})
    if machine == None :
        raise HTTPException(status.HTTP_404_NOT_FOUND,detail="Machine dont found")
    same_name = db.machine_rt.user.find_one({"name" : machine_data.name})
    if same_name != None and same_name["number"] != machine_data.number :
        raise HTTPException(status.HTTP_409_CONFLICT,detail="Machine already exists")
    changes = machine_data.model_dump(exclude={"number"})
    changes["date_updated"] = datetime.now(timezone.utc)
    try:
        db.machine_rt.user.update_one({"number" : machine_data.number}, {"$set": changes})
    except DuplicateKeyError:
        raise HTTPException(status.HTTP_409_CONFLICT,detail="Machine already exists")
    machine.update(changes)
    del machine["_id"]
    machine_bm = base_model_bm.machine_not_password(**machine)
    return machine_bm

def delete_machine_rt(number:int):
    machine = db.machine_rt.user.find_one({"number" : number})
    if machine == None :
        raise HTTPException(status.HTTP_404_NOT_FOUND,detail="Machine dont found")
    db.machine_rt.user.delete_one({"number" : number})
    #Ya no se renumeran las demás: el número de una máquina es fijo para siempre, así ninguna
    #referencia guardada (telemetría, misiones, bitácora) termina apuntando a otra máquina
    list_all_machines = search_all_machine_rt_not()
    return {"detail": "Machine deleted successfully", "all_machines": list_all_machines,
            "number_of_machines": len(list_all_machines)}

#funtion for test of machine
