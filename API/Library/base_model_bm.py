from pydantic import BaseModel
from datetime import datetime


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

class machine_not_password(BaseModel):
    name : str
    number : int #auto increment
    model : str #esp32 s3, esp32 s2,etc
    description : str | None = None
    date_creation : datetime
    date_updated : datetime | None = None
    last_use : datetime | None = None
    last_user : str | None = None
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
    name : str #name of the machine
    date : datetime

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
    latitude : float
    longitude : float
    altitude : float
    speed : float #m/s
    date : datetime | None = None
class lidar_distance_data(BaseModel):
    distance : float #m
    date : datetime | None = None

class sensor_data(BaseModel):
    acelerometer_gyroscope : acelerometer_gyroscope_data
    magnetometer : magnetometer_data
    barometer : barometer_data
    gps : gps_data
    lidar_distance : lidar_distance_data

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


