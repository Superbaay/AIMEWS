from pymongo import MongoClient
from fastapi import status,HTTPException
from Library import base_model_bm
from datetime import datetime, timedelta, timezone

db = MongoClient()

def search_username(username:str):
    user = db.local.user.find_one({"name" : username})
    if user == None :
        return user
    del user["_id"]
    user_bm = base_model_bm.user(**user)
    return user_bm

def search_username_exception(username:str):
    user = db.local.user.find_one({"name" : username})
    if user == None :
        raise HTTPException(status.HTTP_404_NOT_FOUND,detail="User dont found")
    del user["_id"]
    user_bm = base_model_bm.user(**user)
    return user_bm

def search_username_not_password(username:str):
    user = db.local.user.find_one({"name" : username})
    if user == None :
        raise HTTPException(status.HTTP_404_NOT_FOUND,detail="User dont found")
    del user["_id"]
    del user["password"]
    user_bm = base_model_bm.user_not_password(**user)
    return user_bm

# Function to search for a machine by its name and number

def search_machine_rt_exception(name:str, number:int):
    machine = db.local.machine_rt.user.find_one({"name" : name})
    if machine == None :
        machine = db.local.machine_rt.user.find_one({ "number" : number})
        if machine == None :
            raise HTTPException(status.HTTP_404_NOT_FOUND,detail="Machine dont found")
    del machine["_id"]
    machine_bm = base_model_bm.machine(**machine)
    return machine_bm

def search_machine_rt(name:str, number:int):
    machine = db.local.machine_rt.user.find_one({"name" : name})
    if machine == None :
        machine = db.local.machine_rt.user.find_one({ "number" : number})
        if machine == None :
            return None
    del machine["_id"]
    machine_bm = base_model_bm.machine(**machine)
    return machine_bm

def search_machine_rt_by_number(number:int):
    machine = db.local.machine_rt.user.find_one({"number" : number})
    if machine == None :
        raise HTTPException(status.HTTP_404_NOT_FOUND,detail="Machine dont found")
    del machine["_id"]
    return base_model_bm.machine_not_password(**machine)

def new_machine_rt(machine_data: base_model_bm.machine_new):
    machine = db.local.machine_rt.user.find_one({"name" : machine_data.name})
    if machine != None :
        raise HTTPException(status.HTTP_409_CONFLICT,detail="Machine already exists")
    machine = db.local.machine_rt.user.count_documents({})
    number = machine + 1
    new_machine = machine_data.model_dump()
    #Import local para evitar el ciclo: segurity_bm ya importa def_bm
    from Library import segurity_bm
    new_machine["password"] = segurity_bm.encode_password(machine_data.password)
    new_machine["number"] = number
    new_machine["role"] = "4"
    new_machine["date_creation"] = datetime.now(timezone.utc)
    db.local.machine_rt.user.insert_one(new_machine)
    del new_machine["_id"]
    machine_bm = base_model_bm.machine_not_password(**new_machine)
    return machine_bm

def search_machine_rt_not_password(number:int, name:str | None):
    machine = db.local.machine_rt.user.find_one({"number" : number})
    if machine == None :
        raise HTTPException(status.HTTP_404_NOT_FOUND,detail="Machine dont found")
    machine = db.local.machine_rt.user.find_one({"name" : name}) 
    if machine == None :
        raise HTTPException(status.HTTP_404_NOT_FOUND,detail="Machine dont found")
    del machine["_id"]
    del machine["password"]
    machine_bm = base_model_bm.machine_not_password(**machine)
    return machine_bm

def search_all_machine_rt_not():
    all_machines = db.local.machine_rt.user.find()
    machines_list = []
    for machine in all_machines:
        del machine["_id"]
        if "password" in machine:
            del machine["password"]
        machines_list.append(base_model_bm.machine_not_password(**machine))
    return machines_list

#Solo cambia nombre, modelo y descripcion; contraseña, rol y fechas de alta no se tocan
def update_machine_rt(machine_data: base_model_bm.machine_update):
    machine = db.local.machine_rt.user.find_one({"number" : machine_data.number})
    if machine == None :
        raise HTTPException(status.HTTP_404_NOT_FOUND,detail="Machine dont found")
    same_name = db.local.machine_rt.user.find_one({"name" : machine_data.name})
    if same_name != None and same_name["number"] != machine_data.number :
        raise HTTPException(status.HTTP_409_CONFLICT,detail="Machine already exists")
    changes = machine_data.model_dump(exclude={"number"})
    changes["date_updated"] = datetime.now(timezone.utc)
    db.local.machine_rt.user.update_one({"number" : machine_data.number}, {"$set": changes})
    machine.update(changes)
    del machine["_id"]
    machine_bm = base_model_bm.machine_not_password(**machine)
    return machine_bm

def delete_machine_rt(number:int):
    machine = db.local.machine_rt.user.find_one({"number" : number})
    if machine == None :
        raise HTTPException(status.HTTP_404_NOT_FOUND,detail="Machine dont found")
    db.local.machine_rt.user.delete_one({"number" : number})
    number_of_user = db.local.machine_rt.user.count_documents({})
    list_all_machines = search_all_machine_rt_not()
    update_number = 1
    for machine in list_all_machines:
        machine.number = update_number
        db.local.machine_rt.user.update_one({"name": machine.name}, {"$set": {"number": update_number}})
        update_number += 1
    
    return {"detail": "Machine deleted successfully", "all_machines": list_all_machines, "number_of_machines": number_of_user}

#funtion for test of machine
