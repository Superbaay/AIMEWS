from pydantic import BaseModel
from datetime import datetime
from typing import Literal
#db se re-exporta: varios routers hacen "from Library.base_model_bm import db"
from Library.db_bm import db

#-------------------------------------------------
#user model for the database
class user(BaseModel):
    name : str
    company: str | None = None
    password : str
    email  : str | None = None
    date : datetime
    phone_number : int | None = None
    role : str | None = None
    exp : str | None = None
    #Literal: un usuario solo puede ser "user"; el cliente no puede mandar "machine"
    type : Literal["user"] = "user"

class user_not_password(BaseModel):
    name : str
    company: str | None = None
    email  : str | None = None
    date : datetime
    phone_number : int | None = None
    role : str | None = None

class name(BaseModel):
    name : str
class name_password(BaseModel):
    name : str
    password : str
class name_role(BaseModel):
    name : str
    role : str

#-------------------------------------------------
#Engeneer log 
class media(BaseModel):
    filename : str
    content_type : str
    url_path : str
    onwer : str
class engineer_log(BaseModel):
    name : str
    onwer : str
    number : int #auto increment
    problem : str
    hipothesis : str
    solution : str  
    result : str
    evidence : list[media] | None = None
    next_steps : str | None = None
    date_creation : datetime
    date_update : datetime | None = None
    log : str
#-------------------------------------------------
#Machine model for the database

class machine(BaseModel):
    name : str
    number : int #auto increment
    password : str
    role : str | None = None
    model : str #esp32 s3, esp32 s2,etc
    description : str | None = None
    date_creation : datetime
    date_updated : datetime | None = None
    last_use : datetime | None = None
    last_user : str | None = None
    type : Literal["machine"] = "machine"

class machine_not_password(BaseModel):
    name : str
    number : int #auto increment
    model : str #esp32 s3, esp32 s2,etc
    description : str | None = None
    date_creation : datetime
    date_updated : datetime | None = None
    last_use : datetime | None = None
    last_user : str | None = None

#Datos que manda el panel: number, fechas y rol los pone la API
class machine_new(BaseModel):
    name : str
    password : str
    model : str
    description : str | None = None

class machine_update(BaseModel):
    number : int
    name : str
    model : str
    description : str | None = None

class machine_number(BaseModel):
    number : int
#information about the machine and its sensors
class auto_test_machine(BaseModel):
    microcontroller : bool
    acelerometer_gyroscope : bool
    magnetometer : bool
    barometer : bool
    gps : bool
    lidar_distance : bool
    servo_motor : bool
    camera : bool   
    esc_motor : bool
    name : str | None = None #name of the machine 
    date : datetime | None = None

class acelerometer_gyroscope_data(BaseModel):
    pitch_angle : float #grados
    roll_angle : float #grados
    yaw_angle : float #grados
    pitch_velocity : float #grados/s
    roll_velocity : float #grados/s
    yaw_velocity : float #grados/s
    date : datetime | None = None

class magnetometer_data(BaseModel):
    x : float
    y : float
    z : float
    date : datetime | None = None

class barometer_data(BaseModel):
    pressure : float #hPa
    temperature : float #C
    altitude : float #m
    date : datetime | None = None

class gps_data(BaseModel):
    #Sin fix el GPS no tiene posición: estos cuatro llegan en null (nunca 0,0 falso)
    latitude : float | None = None
    longitude : float | None = None
    altitude : float | None = None #m sobre el nivel del mar
    speed : float | None = None #m/s
    #Calidad de la señal: llegan siempre que el GPS responda, aunque no haya fix
    fix : bool = False
    fix_type : int | None = None #1 sin fix, 2 = 2D, 3 = 3D
    satellites : int | None = None #satélites usados en la solución
    satellites_in_view : int | None = None #satélites que el GPS ve en el cielo
    hdop : float | None = None #dilución horizontal: < 2 buena, > 5 mala
    date : datetime | None = None
class lidar_distance_data(BaseModel):
    distance : float #m
    date : datetime | None = None

class batery_6s(BaseModel):
    voltage : float
    current : float
    temperature : float
    date : datetime | None = None
    c1 : float
    c2 : float
    c3 : float
    c4 : float
    c5 : float
    c6 : float

class sensor_data(BaseModel):
    acelerometer_gyroscope : acelerometer_gyroscope_data
    magnetometer : magnetometer_data
    barometer : barometer_data
    gps : gps_data
    lidar_distance : lidar_distance_data
    batery : batery_6s
    date : datetime | None = None
    machine_name : str | None = None

#machine mision data
class coordinates(BaseModel):
    latitude : float
    longitude : float
    altitude : float | None = None

class machine_mision_data(BaseModel):
    number : int | None = None
    start_date : datetime | None = None
    end_date : datetime | None = None
    description : str | None = None
    status : str | None = None
    sensors : list[str] | None = None
    name : str  
    last_update : datetime | None = None
    coordinates_list : list[coordinates] = []


