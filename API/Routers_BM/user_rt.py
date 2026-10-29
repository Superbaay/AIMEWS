from fastapi import APIRouter, Depends
from Library import base_model_bm, def_bm, segurity_bm
from pymongo import MongoClient

db = MongoClient()

user = APIRouter(
    prefix="/user",
    tags=["user"]
)

@user.get("/get_user")
async def get_user_not_password(name: base_model_bm.name = Depends(segurity_bm.verify_role_Depends_O5)):
    user = def_bm.search_username_not_password(name.name)
    return user

@user.patch("/update_user")
async def update_user( name: base_model_bm.user_not_password = Depends(segurity_bm.verify_role_Depends_O5)):
    user = def_bm.search_username_exception(name.name)
    def_bm.search_username_exception(name.name)
    user.password = user.password
    user_dict = user.model_dump()
    db.local.user.update_one({"name" : name.name},{"$set" : user_dict})
    return user

@user.patch("/update_user_password")
async def update_user_password( name: base_model_bm.user = Depends(segurity_bm.verify_role_Depends_O5)):
    user = def_bm.search_username_exception(name.name)
    def_bm.search_username_exception(name.name)
    user.password = segurity_bm.encode_password(user.password)
    db.local.user.update_one({"name" : name.name},{"$set" : {"password" : user.password}})
    return {"detail" : "Password updated"}

@user.patch("/update_user_role")
async def update_user_role( data: base_model_bm.name_password = Depends(segurity_bm.verify_role_Depends_O5)):
    def_bm.search_username_exception(data.name)
    db.local.user.update_one({"name" : data.name},{"$set" : {"role" : data.role}})
    return {"detail" : "Role updated"}

@user.delete("/delete_user")
async def delete_user(name: base_model_bm.name = Depends(segurity_bm.verify_role_Depends_O5)):
    def_bm.search_username_exception(name.name)
    db.local.user.delete_one({"name" : name.name})
    return {"detail" : "User deleted"}

@user.get("/get_user_all")
async def get_user_all(name: base_model_bm.name | None = Depends(segurity_bm.verify_role_Depends_O5)):
    users = db.local.user.find()
    user_list = []
    for user in users:
        del user["_id"]
        del user["password"]
        user_bm = base_model_bm.user_not_password(**user)
        user_list.append(user_bm)
    return user_list




