"""
Pruebas del router OTA (Routers_BM/ota_rt.py): subir, listar, desplegar, descargar y borrar.
El firmware de prueba es una imagen mínima de ESP32-S3 armada aquí (cabecera + esp_app_desc_t);
si existe el .bin real compilado, también se prueba con él.
"""
import struct
from pathlib import Path

import pytest

from conftest import nombre_maquina, token_usuario
from test_drone_ws import conectar_dron, conectar_panel, recibir_hasta
from Routers_BM import ota_rt

bin_real = (Path(__file__).resolve().parents[2] / "enbedded" / "Examples" / "Dron_telemetria_ws"
            / "build" / "Dron_telemetria_ws.bin")


def imagen_falsa(version="9.9.9", proyecto="Dron_telemetria_ws", chip=ota_rt.chip_id_esp32s3, tam=4096):
    cabecera = bytearray(24)
    cabecera[0] = ota_rt.image_magic
    struct.pack_into("<H", cabecera, 12, chip)
    segmento = bytes(8)
    desc = bytearray(256)
    struct.pack_into("<I", desc, 0, ota_rt.app_desc_magic)
    desc[16:16 + len(version)] = version.encode()
    desc[48:48 + len(proyecto)] = proyecto.encode()
    datos = bytes(cabecera) + segmento + bytes(desc)
    return datos + bytes(tam - len(datos))


@pytest.fixture(autouse=True)
def carpeta_temporal(tmp_path, monkeypatch):
    #Los .bin de prueba no deben quedar en API/uploads/ota
    monkeypatch.setattr(ota_rt, "upload_dir", tmp_path)


def auth(token):
    return {"Authorization": f"Bearer {token}"}


def subir(cliente, datos, token=None, nombre="fw.bin"):
    return cliente.post("/ota/upload", headers=auth(token or token_usuario()),
                        files={"file": (nombre, datos, "application/octet-stream")}, data={"notes": "prueba"})


def test_subir_lee_version_del_binario(cliente):
    respuesta = subir(cliente, imagen_falsa("1.2.3"))
    assert respuesta.status_code == 201, respuesta.text
    cuerpo = respuesta.json()
    assert cuerpo["version"] == "1.2.3"
    assert cuerpo["project_name"] == "Dron_telemetria_ws"
    assert cuerpo["size"] == 4096 and len(cuerpo["sha256"]) == 64
    assert "disk_name" not in cuerpo


@pytest.mark.skipif(not bin_real.is_file(), reason="sin firmware compilado")
def test_subir_firmware_real(cliente):
    respuesta = subir(cliente, bin_real.read_bytes())
    assert respuesta.status_code == 201, respuesta.text
    assert respuesta.json()["project_name"] == "Dron_telemetria_ws"


@pytest.mark.parametrize("datos", [b"hola" * 100, imagen_falsa(chip=0), imagen_falsa()[:100]])
def test_rechaza_lo_que_no_es_firmware_s3(cliente, datos):
    assert subir(cliente, datos).status_code == 415


def test_rechaza_firmware_demasiado_grande(cliente):
    assert subir(cliente, imagen_falsa(tam=ota_rt.max_size + 1)).status_code == 413


@pytest.mark.parametrize("rol,tipo", [("4", "user"), ("4", "machine"), ("3", "user")])
def test_solo_o5_administra(cliente, rol, tipo):
    token = token_usuario(rol=rol, tipo=tipo)
    assert subir(cliente, imagen_falsa(), token=token).status_code == 403
    assert cliente.get("/ota/all", headers=auth(token)).status_code == 403
    assert cliente.post("/ota/deploy", headers=auth(token), json={"number": 1, "machine": "x"}).status_code == 403


def test_desplegar_y_descargar(cliente, token_maquina):
    numero = subir(cliente, imagen_falsa("2.0.0")).json()["number"]
    panel = token_usuario()

    #Un dron que no declara "ota" (firmware viejo) no recibe nada
    with conectar_dron(cliente, token_maquina, acciones=None):
        respuesta = cliente.post("/ota/deploy", headers=auth(panel), json={"number": numero, "machine": nombre_maquina})
        assert respuesta.status_code == 409

    with conectar_panel(cliente, panel) as ws_panel, \
            conectar_dron(cliente, token_maquina, acciones=("iniciar_autoprueba", "cero_altura", "ota")) as ws_dron:
        respuesta = cliente.post("/ota/deploy", headers=auth(panel), json={"number": numero, "machine": nombre_maquina})
        assert respuesta.status_code == 200, respuesta.text
        orden = ws_dron.receive_json()
        assert orden == {"tipo": "comando", "action": "ota", "url": f"/ota/firmware/{numero}",
                         "version": "2.0.0", "size": 4096, "from": "piloto_prueba"}
        assert len(str(orden)) < 512   # el lector del firmware no acepta mensajes más largos
        assert recibir_hasta(ws_panel, "ota")["estado"] == "enviado"

        #El dron descarga con su token de máquina
        descarga = cliente.get(orden["url"], headers=auth(token_maquina))
        assert descarga.status_code == 200 and descarga.content == imagen_falsa("2.0.0")

        #El avance que manda el dron llega al panel
        ws_dron.send_json({"tipo": "ota", "estado": "descargando", "version": "2.0.0", "porcentaje": 50})
        aviso = recibir_hasta(ws_panel, "ota")
        assert aviso["estado"] == "descargando" and aviso["machine"] == nombre_maquina


def test_descarga_exige_maquina_u_o5(cliente):
    numero = subir(cliente, imagen_falsa()).json()["number"]
    assert cliente.get(f"/ota/firmware/{numero}").status_code == 401
    assert cliente.get(f"/ota/firmware/{numero}", headers=auth(token_usuario(rol="4"))).status_code == 403
    assert cliente.get(f"/ota/firmware/{numero}", headers=auth(token_usuario())).status_code == 200


def test_desplegar_a_dron_desconectado(cliente):
    numero = subir(cliente, imagen_falsa()).json()["number"]
    respuesta = cliente.post("/ota/deploy", headers=auth(token_usuario()), json={"number": numero, "machine": "no_existe"})
    assert respuesta.status_code == 409


def test_borrar_no_reutiliza_numero(cliente, tmp_path):
    primero = subir(cliente, imagen_falsa()).json()["number"]
    assert cliente.delete(f"/ota/number?number={primero}", headers=auth(token_usuario())).status_code == 200
    assert cliente.get(f"/ota/firmware/{primero}", headers=auth(token_usuario())).status_code == 404
    segundo = subir(cliente, imagen_falsa()).json()["number"]
    assert segundo > primero
    assert len(list(tmp_path.glob("*.bin"))) >= 1


def test_clave_ota_para_el_script(cliente, token_maquina):
    """enbedded/tools/ota_subir.py: sube, despliega y consulta con X-OTA-Key, sin usuario."""
    clave = {"X-OTA-Key": ota_rt.clave_ota}
    assert ota_rt.ruta_clave_ota.name == "clave_ota.txt"   # nunca API/ota_deploy_key.txt en pruebas
    respuesta = cliente.post("/ota/upload", headers=clave, files={"file": ("fw.bin", imagen_falsa("3.0.0"))})
    assert respuesta.status_code == 201 and respuesta.json()["onwer"] == "clave_ota"
    numero = respuesta.json()["number"]
    assert cliente.get("/ota/all", headers={"X-OTA-Key": "mala"}).status_code == 401
    assert cliente.get("/ota/status?machine=nadie", headers=clave).json()["connected"] is False
    with conectar_dron(cliente, token_maquina, acciones=("ota",)) as ws_dron:
        assert cliente.post("/ota/deploy", headers=clave, json={"number": numero, "machine": nombre_maquina}).status_code == 200
        assert ws_dron.receive_json()["action"] == "ota"
        ws_dron.send_json({"tipo": "ota", "estado": "confirmado", "version": "3.0.0"})
        from test_drone_ws import esperar
        esperar(lambda: cliente.get(f"/ota/status?machine={nombre_maquina}", headers=clave).json()["ota"]["estado"] == "confirmado")
    #La clave no sirve para borrar: eso queda para una persona O5-1
    assert cliente.delete(f"/ota/number?number={numero}", headers=clave).status_code == 401
