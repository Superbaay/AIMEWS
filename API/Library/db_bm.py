import os
from pymongo import MongoClient

#Unica conexion a Mongo de toda la API; el resto la importa de aqui para no abrir varias.
#Para pruebas: SAMATI_MONGO_URL (por defecto el Mongo local) y SAMATI_DB_NOMBRE (por defecto "SAMATI").
#Sin variables queda igual que siempre: MongoClient().SAMATI
db = MongoClient(os.environ.get("SAMATI_MONGO_URL"))[os.environ.get("SAMATI_DB_NOMBRE", "SAMATI")]
