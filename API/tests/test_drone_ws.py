"""
Pruebas del WebSocket del dron (/ws/machine) y del panel (/ws) de Routers_BM/drone_ws_rt.py.
Contrato: simulador/contratos.md §7.2-§7.4.
"""
import json
import os
import time
from contextlib import contextmanager
from pathlib import Path

import pytest
from starlette.websockets import WebSocketDisconnect

from conftest import nombre_maquina, nombre_maquina_2, nombre_panel, token_usuario
from Library import segurity_bm
from Routers_BM import drone_ws_rt

acciones_panel = {
    "armar", "desarmar", "despegar", "aterrizar", "mantener_posicion",
    "regresar_casa", "paro_emergencia", "modo", "pid", "pid_leer",
    "iniciar_autoprueba", "cero_altura", "iniciar_mision", "abortar_mision",
}
pid_ejemplo = {eje: {"kp": 1.2, "ki": 0.05, "kd": 0.3} for eje in ("alabeo", "cabeceo", "guinada", "altitud")}
mision_ejemplo = {
    "tipo": "inundado",
    "coordinates_list": [{"latitude": 19.43, "longitude": -99.13, "altitude": 12.5},
                         {"latitude": 19.4301, "longitude": -99.1302}],
    "demostracion": True,
}
ejes_ejemplo = {"potencia": 0.5, "guinada": -0.25, "cabeceo": 1, "alabeo": -1.0}


@contextmanager
def conectar_dron(cliente, token, acciones=tuple(sorted(acciones_panel)), nombre=nombre_maquina):
    """Dron autenticado. Con acciones (como el simulador) manda estado_dron con la lista y
    espera a que la API la registre; con acciones=None se comporta como el firmware real."""
    with cliente.websocket_connect("/ws/machine") as ws:
        ws.send_json({"token": token})
        assert ws.receive_json() == {"tipo": "auth_ok", "machine": nombre}
        if acciones is not None:
            ws.send_json({"tipo": "estado_dron", "version": "SIM-0.3", "maquina": nombre,
                          "acciones": list(acciones), "simulado": True})
            esperar(lambda: drone_ws_rt.manager.drones[nombre].actions == frozenset(acciones))
        yield ws


def esperar(condicion, limite_s=3.0):
    """La API procesa los mensajes del dron en su propio hilo: espera a que se note."""
    fin = time.monotonic() + limite_s
    while not condicion():
        assert time.monotonic() < fin, "la API no procesó el mensaje a tiempo"
        time.sleep(0.005)


@contextmanager
def conectar_panel(cliente, token):
    with cliente.websocket_connect("/ws") as ws:
        assert ws.receive_json() == {"message": "WebSocket connection established"}
        assert ws.receive_json() == {"message": "please send a password"}
        ws.send_json({"token": token})
        assert ws.receive_json() == {"message": "Password received"}
        yield ws


def recibir_hasta(ws, tipo):
    """Siguiente mensaje con ese "tipo" (salta estado/telemetría que el panel recibe al entrar)."""
    for _ in range(20):
        mensaje = ws.receive_json()
        if mensaje.get("tipo") == tipo:
            return mensaje
    raise AssertionError(f"no llegó ningún mensaje tipo {tipo}")


def comando(ws, action, **campos):
    ws.send_json({"tipo": "comando", "action": action, "machine": nombre_maquina, **campos})
    return recibir_hasta(ws, "comando")


# ---------------------------------------------------------------------------
# Configuración por variables de entorno (§7.4)
# ---------------------------------------------------------------------------
def test_clave_y_base_de_prueba(db):
    clave_real = Path(segurity_bm.__file__).resolve().parent.parent / "secret_key.txt"
    assert segurity_bm.ruta_secret_key == Path(os.environ["SAMATI_SECRET_KEY_FILE"])
    assert segurity_bm.ruta_secret_key.resolve() != clave_real
    assert db.name == os.environ["SAMATI_DB_NOMBRE"] != "SAMATI"
    if os.name == "posix":
        assert segurity_bm.ruta_secret_key.stat().st_mode & 0o077 == 0, "la clave debe ser 0600"


def test_listas_de_la_api():
    assert drone_ws_rt.drone_actions == acciones_panel
    assert {"estado_dron", "telemetria", "autoprueba", "pid", "mision"} <= drone_ws_rt.drone_types


# ---------------------------------------------------------------------------
# Autenticación del dron
# ---------------------------------------------------------------------------
def test_dron_token_valido(cliente, token_maquina):
    with conectar_dron(cliente, token_maquina):
        assert nombre_maquina in drone_ws_rt.manager.drones
    assert nombre_maquina not in drone_ws_rt.manager.drones


@pytest.mark.parametrize("primero, motivo", [
    ({"token": "basura"}, "Unauthorized"),
    ({"otro": 1}, "Unauthorized"),
])
def test_dron_token_invalido(cliente, primero, motivo):
    with cliente.websocket_connect("/ws/machine") as ws:
        ws.send_json(primero)
        assert ws.receive_json() == {"tipo": "error", "message": motivo}
        with pytest.raises(WebSocketDisconnect) as cierre:
            ws.receive_json()
        assert cierre.value.code == 1008


def test_dron_con_token_de_usuario(cliente, token_panel):
    with cliente.websocket_connect("/ws/machine") as ws:
        ws.send_json({"token": token_panel})
        assert ws.receive_json() == {"tipo": "error", "message": "Only machine tokens can send telemetry"}


def test_dron_maquina_borrada(cliente):
    token = token_usuario(nombre="no_existe", rol="4", tipo="machine")
    with cliente.websocket_connect("/ws/machine") as ws:
        ws.send_json({"token": token})
        assert ws.receive_json() == {"tipo": "error", "message": "Machine not found"}


def test_panel_rol_insuficiente(cliente):
    with cliente.websocket_connect("/ws") as ws:
        ws.receive_json(), ws.receive_json()
        ws.send_json({"token": token_usuario(rol="3")})
        assert ws.receive_json() == {"message": "Unauthorized"}


# ---------------------------------------------------------------------------
# Comandos panel -> dron
# ---------------------------------------------------------------------------
@pytest.mark.parametrize("accion", sorted(acciones_panel - {"modo", "pid", "iniciar_mision"}))
def test_reenvio_acciones_simples(cliente, token_maquina, token_panel, accion):
    with conectar_dron(cliente, token_maquina) as dron, conectar_panel(cliente, token_panel) as panel:
        respuesta = comando(panel, accion)
        assert respuesta["action"] == accion
        assert respuesta["api_response"] == {"message": "Enviado a 1 dron(es)"}
        assert dron.receive_json() == {"tipo": "comando", "action": accion, "from": nombre_panel}


@pytest.mark.parametrize("accion, campos", [
    ("modo", {"modo": "altitud"}),
    ("pid", {"pid": pid_ejemplo}),
    ("iniciar_mision", {"mision": mision_ejemplo}),
])
def test_reenvio_acciones_con_campos(cliente, token_maquina, token_panel, accion, campos):
    with conectar_dron(cliente, token_maquina) as dron, conectar_panel(cliente, token_panel) as panel:
        #"otro" no está en la lista blanca: no debe llegar al dron
        respuesta = comando(panel, accion, otro="x", **campos)
        assert respuesta["api_response"] == {"message": "Enviado a 1 dron(es)"}
        texto = dron.receive_text()
        assert texto.startswith('{"tipo": "comando"')     # el firmware busca la primera clave "tipo"
        assert json.loads(texto) == {"tipo": "comando", "action": accion, "from": nombre_panel, **campos}


def test_mision_sin_campos_extra(cliente, token_maquina, token_panel):
    """Solo se copian tipo, coordinates_list y demostracion (y lat/lon/alt de cada punto)."""
    mision = {"tipo": "seco", "coordinates_list": [{"latitude": 1, "longitude": 2, "extra": 3}], "x": 1}
    with conectar_dron(cliente, token_maquina) as dron, conectar_panel(cliente, token_panel) as panel:
        comando(panel, "iniciar_mision", mision=mision)
        assert dron.receive_json()["mision"] == {"tipo": "seco",
                                                 "coordinates_list": [{"latitude": 1, "longitude": 2}]}


@pytest.mark.parametrize("accion, campos", [
    ("modo", {"modo": 3}),
    ("modo", {"modo": "x" * 100}),
    ("pid", {"pid": {"alabeo": {"kp": "1", "ki": 0, "kd": 0}}}),
    ("pid", {"pid": {"alabeo": {"kp": 1, "ki": 0}}}),
    ("pid", {"pid": {"alabeo": {"kp": True, "ki": 0, "kd": 0}}}),
    ("pid", {"pid": [1, 2, 3]}),
    ("iniciar_mision", {"mision": {"tipo": "mojado", "coordinates_list": []}}),
    ("iniciar_mision", {"mision": {"tipo": "seco", "coordinates_list": [{"latitude": 200, "longitude": 0}]}}),
    ("iniciar_mision", {"mision": {"tipo": "seco", "coordinates_list": "no"}}),
    ("iniciar_mision", {"mision": {"tipo": "seco", "demostracion": "si"}}),
])
def test_campos_invalidos_no_se_reenvian(cliente, token_maquina, token_panel, accion, campos):
    with conectar_dron(cliente, token_maquina) as dron, conectar_panel(cliente, token_panel) as panel:
        respuesta = comando(panel, accion, **campos)
        assert respuesta["api_response"]["message"].startswith("Invalid fields")
        #Lo siguiente que recibe el dron es el comando válido, no el inválido
        comando(panel, "armar")
        assert dron.receive_json()["action"] == "armar"


def test_mensaje_demasiado_grande(cliente, token_maquina, token_panel):
    puntos = [{"latitude": 19.123456, "longitude": -99.123456, "altitude": 10.5}] * 300
    with conectar_dron(cliente, token_maquina) as dron, conectar_panel(cliente, token_panel) as panel:
        respuesta = comando(panel, "iniciar_mision", mision={"tipo": "seco", "coordinates_list": puntos})
        assert respuesta["api_response"] == {"message": "Message too large"}
        comando(panel, "armar")
        assert dron.receive_json()["action"] == "armar"


def test_accion_desconocida(cliente, token_maquina, token_panel):
    with conectar_dron(cliente, token_maquina) as dron, conectar_panel(cliente, token_panel) as panel:
        respuesta = comando(panel, "autodestruccion")
        assert respuesta["action"] == "autodestruccion"
        assert respuesta["api_response"] == {"message": "Unknown action"}
        comando(panel, "desarmar")
        assert dron.receive_json()["action"] == "desarmar"


def test_comando_sin_dron(cliente, token_panel):
    with conectar_panel(cliente, token_panel) as panel:
        assert comando(panel, "armar")["api_response"] == {"message": "Ningún dron conectado"}


# ---------------------------------------------------------------------------
# Palancas (tipo control) panel -> dron
# ---------------------------------------------------------------------------
def test_reenvio_movimiento_sin_respuesta(cliente, token_maquina, token_panel):
    with conectar_dron(cliente, token_maquina) as dron, conectar_panel(cliente, token_panel) as panel:
        for _ in range(5):
            panel.send_json({"tipo": "control", "action": "movimiento", "machine": nombre_maquina,
                             "ejes": ejes_ejemplo})
        for _ in range(5):
            assert dron.receive_json() == {"tipo": "control", "action": "movimiento",
                                           "ejes": ejes_ejemplo, "from": nombre_panel}
        #El panel no recibe api_response por paquete: antes de la respuesta al comando
        #solo puede llegar el estado_dron que recibe al conectarse, nunca un "control"
        panel.send_json({"tipo": "comando", "action": "aterrizar", "machine": nombre_maquina})
        while (respuesta := panel.receive_json())["tipo"] != "comando":
            assert respuesta["tipo"] == "estado_dron"
        assert respuesta["action"] == "aterrizar"
        assert dron.receive_json()["action"] == "aterrizar"


@pytest.mark.parametrize("mensaje", [
    {"ejes": {**ejes_ejemplo, "potencia": 1.5}},
    {"ejes": {**ejes_ejemplo, "alabeo": -1.01}},
    {"ejes": {k: v for k, v in ejes_ejemplo.items() if k != "guinada"}},
    {"ejes": {**ejes_ejemplo, "cabeceo": "0.5"}},
    {"ejes": ejes_ejemplo, "action": "girar"},
    {"ejes": ejes_ejemplo, "machine": None},
])
def test_movimiento_invalido(cliente, token_maquina, token_panel, mensaje):
    with conectar_dron(cliente, token_maquina) as dron, conectar_panel(cliente, token_panel) as panel:
        panel.send_json({"tipo": "control", "action": "movimiento", "machine": nombre_maquina, **mensaje})
        rechazo = recibir_hasta(panel, "control")
        assert "api_response" in rechazo
        comando(panel, "armar")
        assert dron.receive_json()["tipo"] == "comando"      # el movimiento inválido no llegó


# ---------------------------------------------------------------------------
# Dron -> panel
# ---------------------------------------------------------------------------
def test_pid_y_mision_llegan_al_panel(cliente, token_maquina, token_panel):
    with conectar_panel(cliente, token_panel) as panel, conectar_dron(cliente, token_maquina) as dron:
        dron.send_json({"tipo": "pid", "pid": pid_ejemplo})
        assert recibir_hasta(panel, "pid") == {"tipo": "pid", "pid": pid_ejemplo, "machine": nombre_maquina}
        evento = {"tipo": "mision", "evento": "punto_alcanzado", "indice": 1}
        dron.send_json(evento)
        assert recibir_hasta(panel, "mision") == {**evento, "machine": nombre_maquina}
        dron.send_json({"tipo": "raro", "x": 1})          # tipo desconocido: se descarta
        dron.send_json({"tipo": "mision", "evento": "resultados", "resultados": {"seco": 1}})
        assert recibir_hasta(panel, "mision")["evento"] == "resultados"


def test_firmware_real_sigue_funcionando(cliente, token_maquina, token_panel):
    """El ESP32-S3 (enlace_api.c) solo manda estado_dron, telemetria y autoprueba y solo
    entiende iniciar_autoprueba / cero_altura en mensajes de menos de 512 bytes."""
    estado = {"tipo": "estado_dron", "version": "V0.2", "maquina": nombre_maquina,
              "sensores": ["acelerometer_gyroscope", "barometer", "gps"]}
    telemetria = {"tipo": "telemetria", "datos": {"acelerometer_gyroscope": {"roll_angle": 1.5},
                                                   "barometer": {"altitude": 2.0}}}
    autoprueba = {"tipo": "autoprueba", "resultado": {"microcontroller": True, "servo_motor": False}}
    with conectar_panel(cliente, token_panel) as panel, \
            conectar_dron(cliente, token_maquina, acciones=None) as dron:
        dron.send_json(estado)
        assert recibir_hasta(panel, "estado_dron") == {**estado, "machine": nombre_maquina}
        dron.send_json(telemetria)
        assert recibir_hasta(panel, "telemetria") == {**telemetria, "machine": nombre_maquina}

        for accion in ("iniciar_autoprueba", "cero_altura"):
            assert comando(panel, accion)["api_response"] == {"message": "Enviado a 1 dron(es)"}
            texto = dron.receive_text()
            assert len(texto.encode()) < 512 and texto.startswith('{"tipo": "comando"')
            assert json.loads(texto) == {"tipo": "comando", "action": accion, "from": nombre_panel}

        dron.send_json(autoprueba)
        assert recibir_hasta(panel, "autoprueba") == {**autoprueba, "machine": nombre_maquina}
        #El panel recién conectado recibe al instante el último estado del dron
        with conectar_panel(cliente, token_panel) as otro_panel:
            assert recibir_hasta(otro_panel, "estado_dron")["version"] == "V0.2"
    #Al desconectarse el dron, el panel (ya cerrado aquí) no queda registrado
    assert not drone_ws_rt.manager.drones and not drone_ws_rt.manager.panels


def test_reconexion_gana_la_nueva(cliente, token_maquina, token_panel):
    with conectar_dron(cliente, token_maquina) as viejo:
        with conectar_dron(cliente, token_maquina) as nuevo:
            with pytest.raises(WebSocketDisconnect) as cierre:
                viejo.receive_json()
            assert cierre.value.code == 4000
            with conectar_panel(cliente, token_panel) as panel:
                comando(panel, "iniciar_autoprueba")
                assert nuevo.receive_json()["action"] == "iniciar_autoprueba"


# ---------------------------------------------------------------------------
# Acciones declaradas, destino obligatorio y límite de palancas (contratos.md §7.9)
# ---------------------------------------------------------------------------
def test_dron_sin_lista_solo_acciones_del_firmware(cliente, token_maquina, token_panel):
    """El firmware real no manda "acciones": solo se le reenvían iniciar_autoprueba y cero_altura."""
    with conectar_dron(cliente, token_maquina, acciones=None) as dron, \
            conectar_panel(cliente, token_panel) as panel:
        dron.send_json({"tipo": "estado_dron", "version": "V0.2", "maquina": nombre_maquina})
        recibir_hasta(panel, "estado_dron")
        for accion in ("armar", "despegar", "pid_leer", "iniciar_mision"):
            campos = {"mision": {"tipo": "seco"}} if accion == "iniciar_mision" else {}
            respuesta = comando(panel, accion, **campos)
            assert respuesta["api_response"] == {"message": f"El dron {nombre_maquina} no admite {accion}"}
        assert comando(panel, "cero_altura")["api_response"] == {"message": "Enviado a 1 dron(es)"}
        assert dron.receive_json()["action"] == "cero_altura"     # nada de lo anterior llegó


def test_dron_con_lista_declarada(cliente, token_maquina, token_panel):
    with conectar_dron(cliente, token_maquina, acciones=("armar", "desarmar")) as dron, \
            conectar_panel(cliente, token_panel) as panel:
        assert comando(panel, "armar")["api_response"] == {"message": "Enviado a 1 dron(es)"}
        assert comando(panel, "despegar")["api_response"] == \
            {"message": f"El dron {nombre_maquina} no admite despegar"}
        #iniciar_autoprueba no está en su lista: tampoco se le manda
        assert comando(panel, "iniciar_autoprueba")["api_response"] == \
            {"message": f"El dron {nombre_maquina} no admite iniciar_autoprueba"}
        comando(panel, "desarmar")
        assert [dron.receive_json()["action"] for _ in range(2)] == ["armar", "desarmar"]
        #Un estado_dron sin lista vuelve a lo del firmware
        dron.send_json({"tipo": "estado_dron", "version": "V0.2"})
        esperar(lambda: drone_ws_rt.manager.drones[nombre_maquina].actions == drone_ws_rt.firmware_actions)


def test_falta_machine(cliente, token_maquina, token_maquina_2, token_panel):
    with conectar_dron(cliente, token_maquina) as dron, \
            conectar_dron(cliente, token_maquina_2, acciones=None, nombre=nombre_maquina_2) as dron_2, \
            conectar_panel(cliente, token_panel) as panel:
        for accion in sorted(acciones_panel - {"iniciar_autoprueba", "cero_altura"}):
            panel.send_json({"tipo": "comando", "action": accion})
            assert recibir_hasta(panel, "comando")["api_response"] == {"message": "Falta machine"}
        panel.send_json({"tipo": "comando", "action": "armar", "machine": ""})
        assert recibir_hasta(panel, "comando")["api_response"] == {"message": "Falta machine"}
        #Las dos inofensivas sí van a todos los que las admiten
        panel.send_json({"tipo": "comando", "action": "iniciar_autoprueba"})
        assert recibir_hasta(panel, "comando")["api_response"] == {"message": "Enviado a 2 dron(es)"}
        assert dron.receive_json()["action"] == "iniciar_autoprueba"
        assert dron_2.receive_json()["action"] == "iniciar_autoprueba"


def test_limite_de_paquetes_control(cliente, token_maquina, token_panel):
    paquetes = 80
    with conectar_dron(cliente, token_maquina) as dron, conectar_panel(cliente, token_panel) as panel:
        inicio = time.monotonic()
        for _ in range(paquetes):
            panel.send_json({"tipo": "control", "action": "movimiento", "machine": nombre_maquina,
                             "ejes": ejes_ejemplo})
        comando(panel, "aterrizar")
        transcurrido = time.monotonic() - inicio
        recibidos = 0
        while (mensaje := dron.receive_json())["tipo"] == "control":
            recibidos += 1
        assert mensaje["action"] == "aterrizar"
        maximo = drone_ws_rt.max_control_hz * (1 + transcurrido) + 1
        assert drone_ws_rt.max_control_hz <= recibidos <= min(maximo, paquetes - 1)


def test_limite_de_bytes_utf8(cliente, token_maquina, token_panel):
    """8 KB en bytes: 5000 "ñ" son 5000 caracteres pero 10000 bytes."""
    with conectar_dron(cliente, token_maquina) as dron, conectar_panel(cliente, token_panel) as panel:
        texto = json.dumps({"tipo": "comando", "action": "modo", "machine": nombre_maquina,
                            "modo": "x", "relleno": "ñ" * 5000}, ensure_ascii=False)
        assert len(texto) < drone_ws_rt.max_message_bytes < len(texto.encode())
        panel.send_text(texto)
        assert recibir_hasta(panel, "comando")["api_response"] == {"message": "Message too large"}
        #Lado del dron: un pid con relleno multibyte se descarta y no llega al panel
        dron.send_text(json.dumps({"tipo": "pid", "pid": {}, "relleno": "ñ" * 5000}, ensure_ascii=False))
        dron.send_json({"tipo": "pid", "pid": pid_ejemplo})
        assert recibir_hasta(panel, "pid")["pid"] == pid_ejemplo
