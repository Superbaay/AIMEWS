from pydantic import BaseModel
from datetime import datetime

class user(BaseModel):
    name : str
    company: str | None = None
    password : str
    email  : str | None = None
    date : datetime
    phone_number : int | None = None
    role : str | None = None
    exp : str | None = None

class user_not_password(BaseModel):
    name : str
    company: str | None = None
    email  : str | None = None
    date : datetime
    phone_number : int | None = None
    role : str | None = None
class order(BaseModel):
    code : str
    date : str

