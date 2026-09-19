from pymongo import MongoClient
from fastapi import status,HTTPException
from Library import base_model_bm

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



