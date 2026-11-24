"""
Preparación de las pruebas de la API (contratos.md §7.4).

    cd API && SAMATI_SECRET_KEY_FILE=<carpeta temporal>/clave.txt \
        SAMATI_DB_NOMBRE=SAMATI_sim_api python3 -m pytest tests -q

- La clave que firma los JWT sale de SAMATI_SECRET_KEY_FILE (por defecto
  <tempfile.gettempdir()>/samati_api_pruebas/clave.txt): nunca se lee ni se crea
  API/secret_key.txt.
- La base es SAMATI_DB_NOMBRE (por defecto SAMATI_sim_api); la base "SAMATI" está prohibida.
- Sin SAMATI_MONGO_URL se usa mongomock (Mongo en memoria, no hace falta mongod).
  Con SAMATI_MONGO_URL se usa ese Mongo real y se vacía la base de pruebas al empezar.
"""
import os
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace

ruta_clave = Path(os.environ.setdefault(
    "SAMATI_SECRET_KEY_FILE", str(Path(tempfile.gettempdir()) / "samati_api_pruebas" / "clave.txt")))
ruta_clave.parent.mkdir(parents=True, exist_ok=True)
#Clave X-OTA-Key de Routers_BM/ota_rt.py: tampoco se toca API/ota_deploy_key.txt
os.environ.setdefault("SAMATI_OTA_KEY_FILE", str(ruta_clave.parent / "clave_ota.txt"))
#Fotos de las alertas (Library/misiones_bm.py): tampoco se escriben en API/uploads
os.environ.setdefault("SAMATI_ALERTAS_DIR", str(ruta_clave.parent / "alertas"))
nombre_base = os.environ.setdefault("SAMATI_DB_NOMBRE", "SAMATI_sim_api")
if nombre_base == "SAMATI":
    raise RuntimeError("Las pruebas nunca usan la base SAMATI: usa SAMATI_DB_NOMBRE=SAMATI_sim_api")

if not os.environ.get("SAMATI_MONGO_URL"):
    #db_bm hace "from pymongo import MongoClient": se cambia antes de importarlo
    import mongomock
    import pymongo
    pymongo.MongoClient = mongomock.MongoClient

#Los módulos de la API se importan como "Library..." y "Routers_BM...", desde API/
ruta_api = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ruta_api))

import pytest
from fastapi.testclient import TestClient

from Library import db_bm, segurity_bm
import main

nombre_maquina = "sim_prueba"
nombre_maquina_2 = "sim_prueba_2"
clave_maquina = "clave-de-prueba-123"
nombre_panel = "piloto_prueba"


@pytest.fixture(scope="session")
def db():
    base = db_bm.db
    assert base.name == nombre_base != "SAMATI"
    base.client.drop_database(nombre_base)
    for numero, nombre in enumerate((nombre_maquina, nombre_maquina_2), start=1):
        base.machine_rt.user.insert_one({
            "name": nombre, "number": numero, "role": "4", "model": "simulador",
            "password": segurity_bm.encode_password(clave_maquina),
            "date_creation": "2026-11-17T00:00:00Z", "type": "machine",
        })
    #Las rutas con rol vuelven a leer el rol en la base (segurity_bm.rol_efectivo): el usuario
    #de los tokens de prueba tiene que existir. Con O5-1 en la base, el rol que manda es el del token
    base.user.insert_one({"name": nombre_panel, "role": "O5-1", "type": "user",
                          "password": segurity_bm.encode_password("clave-panel-123"),
                          "date": "2026-11-17T00:00:00Z"})
    yield base
    base.client.drop_database(nombre_base)


@pytest.fixture(scope="session")
def cliente(db):
    with TestClient(main.app) as cliente:
        yield cliente


def pedir_token_maquina(cliente, nombre):
    respuesta = cliente.post("/machine_rt/token_machine", data={"username": nombre, "password": clave_maquina})
    assert respuesta.status_code == 200, respuesta.text
    return respuesta.json()["token"]


@pytest.fixture(scope="session")
def token_maquina(cliente):
    return pedir_token_maquina(cliente, nombre_maquina)


@pytest.fixture(scope="session")
def token_maquina_2(cliente):
    return pedir_token_maquina(cliente, nombre_maquina_2)


def token_usuario(nombre=nombre_panel, rol="O5-1", tipo="user"):
    return segurity_bm.create_token(SimpleNamespace(name=nombre, role=rol, type=tipo))


@pytest.fixture
def token_panel():
    return token_usuario()
