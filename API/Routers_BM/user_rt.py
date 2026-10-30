from fastapi import APIRouter, Depends, HTTPException, status
from Library import base_model_bm, def_bm, segurity_bm
from pymongo import MongoClient

db = MongoClient()

user = APIRouter(
    prefix="/user",
    tags=["user"]
)

#Un usuario de nivel superior se responde igual que uno inexistente (404) para no revelar que existe
def search_visible_user(name:str, token:dict):
    found = def_bm.search_username_not_password(name)
    if not segurity_bm.can_see_user(token, found.role):
        raise HTTPException(status.HTTP_404_NOT_FOUND,detail="User dont found")
    return found

def check_can_manage(target:base_model_bm.user_not_password, token:dict):
    if not segurity_bm.can_manage_user(token, target.role):
        raise HTTPException(status.HTTP_403_FORBIDDEN,detail="Role insuficiente")

def is_own_account(target:base_model_bm.user_not_password, token:dict):
    return target.name == token.get("name")

@user.get("/get_user")
async def get_user_not_password(name:str, token:dict = Depends(segurity_bm.verify_role_Depends_2)):
    return search_visible_user(name, token)

#Solo cambia los datos de perfil; nombre, fecha y rol no se tocan aqui
@user.patch("/update_user")
async def update_user(data:base_model_bm.user_not_password, token:dict = Depends(segurity_bm.verify_role_Depends_2)):
    target = search_visible_user(data.name, token)
    if not is_own_account(target, token):
        check_can_manage(target, token)
    changes = {"company" : data.company, "email" : data.email, "phone_number" : data.phone_number}
    db.local.user.update_one({"name" : data.name},{"$set" : changes})
    return target.model_copy(update=changes)

@user.patch("/update_user_password")
async def update_user_password(data:base_model_bm.name_password, token:dict = Depends(segurity_bm.verify_role_Depends_2)):
    target = search_visible_user(data.name, token)
    if not is_own_account(target, token):
        check_can_manage(target, token)
    db.local.user.update_one({"name" : data.name},{"$set" : {"password" : segurity_bm.encode_password(data.password)}})
    return {"detail" : "Password updated"}

@user.patch("/update_user_role")
async def update_user_role(data:base_model_bm.name_role, token:dict = Depends(segurity_bm.verify_role_Depends_2)):
    target = search_visible_user(data.name, token)
    if is_own_account(target, token):
        raise HTTPException(status.HTTP_403_FORBIDDEN,detail="No puedes cambiar tu propio rol")
    check_can_manage(target, token)
    if not segurity_bm.can_assign_role(token, data.role):
        raise HTTPException(status.HTTP_403_FORBIDDEN,detail="Role insuficiente")
    db.local.user.update_one({"name" : data.name},{"$set" : {"role" : data.role}})
    return {"detail" : "Role updated"}

@user.delete("/delete_user")
async def delete_user(data:base_model_bm.name, token:dict = Depends(segurity_bm.verify_role_Depends_2)):
    target = search_visible_user(data.name, token)
    if is_own_account(target, token):
        raise HTTPException(status.HTTP_403_FORBIDDEN,detail="No puedes eliminar tu propia cuenta")
    check_can_manage(target, token)
    db.local.user.delete_one({"name" : data.name})
    return {"detail" : "User deleted"}

#Solo devuelve usuarios del mismo nivel o inferior al de quien consulta
@user.get("/get_user_all")
async def get_user_all(token:dict = Depends(segurity_bm.verify_role_Depends_2)):
    users = db.local.user.find()
    user_list = []
    for user in users:
        if not segurity_bm.can_see_user(token, user.get("role")):
            continue
        del user["_id"]
        del user["password"]
        user_bm = base_model_bm.user_not_password(**user)
        user_list.append(user_bm)
    return user_list
