import logging
import os
from pymongo import ASCENDING, DESCENDING, MongoClient, ReturnDocument
from pymongo.errors import OperationFailure

#Unica conexion a Mongo de toda la API; el resto la importa de aqui para no abrir varias.
#Para pruebas: SAMATI_MONGO_URL (por defecto el Mongo local) y SAMATI_DB_NOMBRE (por defecto "SAMATI").
#Sin variables queda igual que siempre: MongoClient().SAMATI
db = MongoClient(os.environ.get("SAMATI_MONGO_URL"))[os.environ.get("SAMATI_DB_NOMBRE", "SAMATI")]

log = logging.getLogger("samati.db")


# ---------------------------------------------------------------------------
# Números que nunca se repiten
# ---------------------------------------------------------------------------
#Antes los números salían de count_documents() + 1 y se renumeraban al borrar: log_005 podía
#pasar a ser log_004 y una referencia vieja apuntaba a otro registro. Además, dos altas al mismo
#tiempo podían recibir el mismo número. Un contador con $inc es atómico (Mongo lo hace en un solo
#paso, sin que otra petición se meta en medio) y solo sube, así que un número borrado no vuelve.
#Los contadores viven en la colección "counters" con el formato que ya usaba ota_rt.py:
#    {"_id": "ota_firmware", "value": 7}
def siguiente_numero(nombre: str, coleccion=None, campo: str = "number") -> int:
    """Devuelve el siguiente número del contador `nombre`.

    coleccion: si se da y el contador aún no existe, el contador arranca en el número más alto
    que ya tenga esa colección. Así los registros creados antes de este cambio conservan sus
    números y el siguiente no choca con ninguno.
    """
    if coleccion is not None and db.counters.find_one({"_id": nombre}, {"_id": 1}) is None:
        mayor = coleccion.find_one({campo: {"$type": "number"}}, {campo: 1}, sort=[(campo, DESCENDING)])
        if mayor is not None:
            #$max solo sube el valor: si otra petición ya creó el contador, no lo baja
            db.counters.update_one({"_id": nombre}, {"$max": {"value": int(mayor[campo])}}, upsert=True)
    contador = db.counters.find_one_and_update({"_id": nombre}, {"$inc": {"value": 1}},
                                               upsert=True, return_document=ReturnDocument.AFTER)
    return contador["value"]


# ---------------------------------------------------------------------------
# Índices
# ---------------------------------------------------------------------------
#Un índice único hace que sea Mongo quien garantice que no hay dos usuarios con el mismo nombre.
#El find_one previo que hacen las rutas sigue sirviendo para dar un mensaje claro (409), pero si
#llegan dos altas a la vez las dos pasan ese find_one; con el índice, la segunda falla.
indices = [
    #(colección, campos, único)
    ("user", [("name", ASCENDING)], True),
    ("machine_rt.user", [("name", ASCENDING)], True),
    ("machine_rt.user", [("number", ASCENDING)], True),
    ("engineer_log", [("number", ASCENDING)], True),
    ("ota_firmware", [("number", ASCENDING)], True),
    ("missions", [("number", ASCENDING)], True),
    ("missions", [("machine", ASCENDING), ("number", DESCENDING)], False),
    ("readings", [("mission_number", ASCENDING), ("indice", ASCENDING)], False),
    ("alerts", [("number", ASCENDING)], True),
    ("alerts", [("date", DESCENDING)], False),
]


def crear_indices() -> list[str]:
    """Crea los índices (si ya existen no hace nada). Devuelve los avisos de los que fallaron.

    Si la base ya tiene duplicados (por ejemplo dos usuarios "ana"), Mongo no puede crear el
    índice único: se avisa en el registro y la API arranca igual. Hay que borrar o renombrar el
    duplicado y reiniciar la API (o correr: python -m Library.revisar_duplicados).
    """
    avisos = []
    for nombre, campos, unico in indices:
        try:
            db[nombre].create_index(campos, unique=unico)
        except OperationFailure as error:
            aviso = f"No se pudo crear el índice {campos} en {nombre}: {error}"
            log.warning(aviso)
            avisos.append(aviso)
    return avisos
