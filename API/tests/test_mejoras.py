"""
Pruebas del plan de mejoras (23-nov-2026):
    F1  rutas def / to_thread           (lo cubren todas las pruebas: deben seguir pasando)
    F4  números que no se repiten e índices únicos
    F2  la telemetría del WebSocket se guarda
    F3  misiones, lecturas y alertas a partir de los eventos del dron
    F5  el rol se vuelve a leer en la base; response_model sin password
"""
from datetime import datetime, timezone

import pytest
from pymongo.errors import DuplicateKeyError

from conftest import nombre_maquina, nombre_panel, token_usuario
from Library import db_bm
from Library.escritor_bm import escritor_global
from Routers_BM import camera_rt
from test_drone_ws import conectar_dron, conectar_panel, esperar, recibir_hasta

telemetria_ejemplo = {
    "acelerometer_gyroscope": {"roll_angle": 1.0, "pitch_angle": -0.5, "yaw_angle": 90.0},
    "gps": {"latitude": 21.16, "longitude": -86.85},
    "lidar_distance": {"distance": 12.3},
}
waypoints = [{"latitude": 21.16, "longitude": -86.85, "altitude": 12.0},
             {"latitude": 21.1601, "longitude": -86.8502},
             {"latitude": 21.1602, "longitude": -86.8504}]


def auth(token):
    return {"Authorization": f"Bearer {token}"}


def crear_usuario(db, nombre, rol):
    db.user.delete_many({"name": nombre})
    db.user.insert_one({"name": nombre, "role": rol, "type": "user", "password": "x",
                        "date": datetime.now(timezone.utc)})


# ---------------------------------------------------------------------------
# F4: números e índices
# ---------------------------------------------------------------------------
def test_contador_arranca_en_el_mayor_existente(db):
    db.counters.delete_many({"_id": "prueba_numeros"})
    db.prueba_numeros.insert_many([{"number": 3}, {"number": 7}])
    assert db_bm.siguiente_numero("prueba_numeros", db.prueba_numeros) == 8
    assert db_bm.siguiente_numero("prueba_numeros", db.prueba_numeros) == 9


def test_maquina_borrada_no_reutiliza_numero_ni_renumera(cliente, db):
    token = token_usuario()
    numeros = []
    for nombre in ("maq_a", "maq_b"):
        r = cliente.post("/machine_rt/new_machine_rt", headers=auth(token),
                         json={"name": nombre, "password": "clave-123", "model": "x"})
        assert r.status_code == 200, r.text
        numeros.append(r.json()["number"])
    a, b = numeros
    assert cliente.request("DELETE", "/machine_rt/delete_machine_rt", headers=auth(token),
                           json={"number": a}).status_code == 200
    #b conserva su número y la siguiente máquina no recibe el de a
    assert db.machine_rt.user.find_one({"name": "maq_b"})["number"] == b
    r = cliente.post("/machine_rt/new_machine_rt", headers=auth(token),
                     json={"name": "maq_c", "password": "clave-123", "model": "x"})
    assert r.json()["number"] > b
    assert "password" not in r.json()


def test_bitacora_no_renumera(cliente, db):
    token = token_usuario()
    log = {"name": "prueba", "onwer": "x", "number": 0, "problem": "p", "hipothesis": "h",
           "solution": "s", "result": "r", "date_creation": "2026-11-23T00:00:00Z", "log": "l"}
    n1 = cliente.post("/engineer_log/new", headers=auth(token), json=log).json()["number"]
    n2 = cliente.post("/engineer_log/new", headers=auth(token), json=log).json()["number"]
    assert cliente.delete(f"/engineer_log/number?number={n1}", headers=auth(token)).status_code == 200
    assert cliente.get(f"/engineer_log/number?number={n2}", headers=auth(token)).status_code == 200
    n3 = cliente.post("/engineer_log/new", headers=auth(token), json=log).json()["number"]
    assert n3 > n2


def test_indice_unico_de_usuario(cliente, db):
    crear_usuario(db, "duplicado", "1")
    with pytest.raises(DuplicateKeyError):
        db.user.insert_one({"name": "duplicado", "role": "1"})


# ---------------------------------------------------------------------------
# F5: el rol se vuelve a leer en la base
# ---------------------------------------------------------------------------
def test_rol_bajado_tiene_efecto_inmediato(cliente, db):
    crear_usuario(db, "ana", "4")
    token_viejo = token_usuario(nombre="ana", rol="O5-1")      # token emitido cuando era O5-1
    r = cliente.get("/ota/all", headers=auth(token_viejo))
    assert r.status_code == 403
    #Rol 4 sí le alcanza para ver máquinas
    assert cliente.get("/machine_rt/search_all_machine_rt_not", headers=auth(token_viejo)).status_code == 200


def test_usuario_borrado_pierde_acceso(cliente, db):
    token = token_usuario(nombre="fantasma", rol="O5-1")
    assert cliente.get("/machine_rt/search_all_machine_rt_not", headers=auth(token)).status_code == 401


def test_panel_de_usuario_bajado_no_entra(cliente, db):
    crear_usuario(db, "beto", "2")
    with cliente.websocket_connect("/ws") as ws:
        ws.receive_json(); ws.receive_json()
        ws.send_json({"token": token_usuario(nombre="beto", rol="O5-1")})
        assert ws.receive_json() == {"message": "Unauthorized"}


def test_panel_pierde_el_control_al_bajarle_el_rol(cliente, db, token_maquina):
    crear_usuario(db, "carla", "4")
    with conectar_dron(cliente, token_maquina), conectar_panel(cliente, token_usuario(nombre="carla", rol="4")) as panel:
        db.user.update_one({"name": "carla"}, {"$set": {"role": "1"}})
        panel.send_json({"tipo": "comando", "action": "armar", "machine": nombre_maquina})
        assert recibir_hasta(panel, "comando")["api_response"]["message"] == "Role insuficiente"


def test_usuarios_sin_password(cliente):
    usuarios = cliente.get("/user/get_user_all", headers=auth(token_usuario())).json()
    assert usuarios and all("password" not in u for u in usuarios)


# ---------------------------------------------------------------------------
# F2: telemetría guardada
# ---------------------------------------------------------------------------
def test_telemetria_del_websocket_se_guarda(cliente, db, token_maquina):
    db.machine_telemetry_data.delete_many({})
    with conectar_dron(cliente, token_maquina) as dron:
        dron.send_json({"tipo": "telemetria", "datos": telemetria_ejemplo})
        esperar(lambda: db.machine_telemetry_data.count_documents({"machine_name": nombre_maquina}) >= 1)
    guardado = db.machine_telemetry_data.find_one({"machine_name": nombre_maquina})
    assert guardado["fuente"] == "ws" and guardado["lidar_distance"]["distance"] == 12.3
    r = cliente.get(f"/machine_telemetry_rt/sensor_data?machine_name={nombre_maquina}", headers=auth(token_usuario()))
    assert r.status_code == 200 and r.json()["data"]


# ---------------------------------------------------------------------------
# F3: misiones, lecturas y alertas
# ---------------------------------------------------------------------------
def evento(tipo_mision, **campos):
    return {"tipo": "mision", "mision": tipo_mision, **campos}


def correr_mision(cliente, panel, dron, tipo, eventos):
    panel.send_json({"tipo": "comando", "action": "iniciar_mision", "machine": nombre_maquina,
                     "mision": {"tipo": tipo, "coordinates_list": waypoints}})
    assert recibir_hasta(panel, "comando")["api_response"]["message"].startswith("Enviado a 1")
    assert recibir_hasta(dron, "comando")["action"] == "iniciar_mision"
    for e in eventos:
        dron.send_json(e)


def test_mision_completa_con_alerta_y_foto(cliente, db, token_maquina, monkeypatch):
    for nombre in ("missions", "readings", "alerts"):
        db[nombre].delete_many({})
    #Una cámara en línea con un frame: la alerta debe llevar esa foto
    canal = camera_rt.get_channel(nombre_maquina)
    monkeypatch.setattr(canal, "source", object())
    monkeypatch.setattr(canal, "last_frame", b"\xff\xd8JPEG-de-prueba\xff\xd9")

    medicion = dict(evento="medicion", valida=True, latitud=21.16, longitud=-86.85,
                    distancia_m=11.9, altura_dron_m=12.0, nivel_superficie_m=0.1, n_lecturas=40)
    with conectar_dron(cliente, token_maquina) as dron, conectar_panel(cliente, token_usuario()) as panel:
        correr_mision(cliente, panel, dron, "seco", [
            evento("seco", evento="waypoint", indice=0, total=3),
            evento("seco", indice=0, **{**medicion, "nivel_superficie_m": 0.0}),
            evento("seco", evento="fin", n_mediciones=1),
        ])
        esperar(lambda: db.missions.count_documents({"estado": "terminada"}) == 1)
        correr_mision(cliente, panel, dron, "inundado", [
            evento("inundado", evento="waypoint", indice=0, total=3),
            evento("inundado", indice=0, **medicion),
            evento("inundado", evento="alerta", indice=0, latitud=21.16, longitud=-86.85,
                   profundidad_m=0.31, umbral_m=0.15, destino="Protección Civil", mensaje="ALERTA"),
            evento("inundado", evento="fin", resultados=[{"indice": 0, "profundidad_m": 0.31, "alerta": True}]),
        ])
        aviso = recibir_hasta(panel, "alerta_registrada")
        assert aviso["profundidad_m"] == 0.31 and aviso["foto"] is True
        esperar(lambda: db.missions.count_documents({"estado": "terminada"}) == 2)

    token = token_usuario()
    misiones = cliente.get("/missions", headers=auth(token)).json()
    inundada, seca = misiones[0], misiones[1]
    assert inundada["tipo"] == "inundado" and inundada["referencia_number"] == seca["number"]
    assert inundada["n_alertas"] == 1 and inundada["creada_por"] == nombre_panel

    detalle = cliente.get(f"/missions/{inundada['number']}", headers=auth(token)).json()
    assert len(detalle["lecturas"]) == 1 and detalle["lecturas"][0]["distancia_m"] == 11.9

    comparacion = cliente.get(f"/missions/{inundada['number']}/comparacion", headers=auth(token)).json()
    fila = comparacion["filas"][0]
    assert fila["diferencia_niveles_m"] == 0.1 and fila["profundidad_m"] == 0.31 and fila["alerta"] is True

    alerta = cliente.get("/alerts?estado=pendiente", headers=auth(token)).json()[0]
    foto = cliente.get(f"/alerts/{alerta['number']}/foto", headers=auth(token))
    assert foto.status_code == 200 and foto.content.startswith(b"\xff\xd8")

    r = cliente.patch(f"/alerts/{alerta['number']}", headers=auth(token), json={"estado": "atendida"})
    assert r.status_code == 200 and r.json()["atendida_por"] == nombre_panel
    assert cliente.patch(f"/alerts/{alerta['number']}", headers=auth(token),
                         json={"estado": "otra"}).status_code == 422


def test_mision_rechazada_por_el_dron(cliente, db, token_maquina):
    db.missions.delete_many({})
    with conectar_dron(cliente, token_maquina) as dron, conectar_panel(cliente, token_usuario()) as panel:
        correr_mision(cliente, panel, dron, "seco", [evento("seco", evento="rechazada", motivo="desarmado")])
        esperar(lambda: db.missions.count_documents({"estado": "rechazada"}) == 1)
    assert db.missions.find_one({})["motivo"] == "desarmado"


def test_eventos_sin_mision_previa_crean_una(cliente, db, token_maquina):
    """El dron arrancó la misión por su cuenta (o la API se reinició): no se pierden las lecturas."""
    db.missions.delete_many({}); db.readings.delete_many({})
    with conectar_dron(cliente, token_maquina) as dron:
        dron.send_json(evento("seco", evento="medicion", indice=2, valida=False, mensaje="sin lecturas"))
        esperar(lambda: db.readings.count_documents({}) == 1)
    assert db.missions.find_one({})["creada_por"] == "dron"


def test_rol_1_no_ve_misiones(cliente, db):
    crear_usuario(db, "dani", "1")
    assert cliente.get("/missions", headers=auth(token_usuario(nombre="dani", rol="1"))).status_code == 403


def test_escritor_sin_errores(cliente):
    estado = cliente.get("/escritor/estado", headers=auth(token_usuario())).json()
    assert estado["errores"] == 0 and estado["descartados"] == 0
