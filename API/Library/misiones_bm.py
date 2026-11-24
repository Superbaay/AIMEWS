"""
Registro de misiones, lecturas y alertas a partir de los eventos del dron.

Quién calcula la profundidad
----------------------------
El dron (o el simulador) mide y calcula la profundidad a bordo: compara la misión en seco con
la inundada, corrige la deriva del barómetro con la estación de tierra y el punto de control, y
manda los resultados por el WebSocket como eventos {"tipo": "mision", "evento": ...}
(ver simulacion: mision.py y contratos.md §6). La API NO repite ese cálculo; lo que hace es:
    1. Guardar cada misión, cada medición por waypoint y cada alerta (historial para comparar,
       para el manual de operación y para la evaluación).
    2. Pegarle a cada alerta la foto más reciente de la ESP32-CAM.
    3. Avisar a los paneles con {"tipo": "alerta_registrada", ...} cuando la alerta ya quedó guardada.
El aviso inmediato al panel (< 10 s) no espera a Mongo: drone_ws_rt reenvía el evento "alerta"
en cuanto llega; esto solo agrega el registro.

Colecciones
-----------
    missions   {number, machine, tipo: seco|inundado, estado, waypoints, creada_por, demostracion,
                referencia_number, inicio, fin, waypoint_actual, total_waypoints, resultados,
                n_alertas, motivo}
    readings   {mission_number, machine, mision, indice, valida, latitud, longitud, distancia_m,
                altura_dron_m, nivel_superficie_m, profundidad_provisional_m, n_lecturas,
                desviacion_m, es_control, date}
    alerts     {number, mission_number, machine, indice, latitud, longitud, profundidad_m,
                umbral_m, destino, mensaje, foto, estado: pendiente|atendida|descartada,
                date, atendida_por, fecha_atencion}

Todas las funciones de este archivo son síncronas y bloquean (pymongo): se llaman desde el
escritor de fondo (escritor_bm), nunca directo desde un WebSocket.
"""
import os
from datetime import datetime, timezone
from pathlib import Path

from pymongo import DESCENDING

from Library.db_bm import db, siguiente_numero

estados_activos = ("enviada", "en_curso")
estados_alerta = ("pendiente", "atendida", "descartada")
#Fotos de las alertas: en API/uploads (ya está en .gitignore), nunca en git
#Las pruebas usan otra carpeta con SAMATI_ALERTAS_DIR (ver tests/conftest.py)
carpeta_fotos = Path(os.environ.get("SAMATI_ALERTAS_DIR")
                     or Path(__file__).resolve().parent.parent / "uploads" / "alertas")

#Campos del evento "medicion" que se copian a readings (lista blanca: lo demás se ignora)
campos_medicion = ("indice", "valida", "latitud", "longitud", "distancia_m", "altura_dron_m",
                   "nivel_superficie_m", "profundidad_provisional_m", "n_lecturas", "descartadas",
                   "desviacion_m", "es_control", "intentos", "mensaje", "t")
campos_alerta = ("indice", "latitud", "longitud", "profundidad_m", "umbral_m", "destino", "mensaje", "t")


def ahora():
    return datetime.now(timezone.utc)


def solo(evento: dict, campos) -> dict:
    return {campo: evento[campo] for campo in campos if campo in evento}


def crear_mision(machine: str, mision: dict, creada_por: str) -> dict:
    """El panel mandó iniciar_mision y llegó al dron. mision ya viene validada por check_mision."""
    #Si quedó otra misión abierta de este dron (se cortó el WiFi, se apagó), se cierra
    db.missions.update_many({"machine": machine, "estado": {"$in": list(estados_activos)}},
                            {"$set": {"estado": "interrumpida", "fin": ahora()}})
    tipo = mision.get("tipo")
    documento = {
        "number": siguiente_numero("missions", db.missions),
        "machine": machine,
        "tipo": tipo,
        "estado": "enviada",
        "waypoints": mision.get("coordinates_list", []),
        "total_waypoints": len(mision.get("coordinates_list", [])),
        "demostracion": mision.get("demostracion", False),
        "creada_por": creada_por,
        "inicio": ahora(),
        "fin": None,
        "referencia_number": None,
        "waypoint_actual": None,
        "n_alertas": 0,
    }
    if tipo == "inundado":
        #La misión inundada se compara con la última en seco terminada del mismo dron
        referencia = db.missions.find_one({"machine": machine, "tipo": "seco", "estado": "terminada"},
                                          {"number": 1}, sort=[("number", DESCENDING)])
        documento["referencia_number"] = referencia["number"] if referencia else None
    db.missions.insert_one(documento)
    documento.pop("_id", None)
    return documento


def mision_activa(machine: str, tipo: str | None) -> dict:
    """La misión abierta del dron. Si no hay (el dron arrancó la misión por su cuenta, o la API
    se reinició a media misión) se crea una para no perder las mediciones."""
    actual = db.missions.find_one({"machine": machine, "estado": {"$in": list(estados_activos)}},
                                  sort=[("number", DESCENDING)])
    if actual is not None:
        return actual
    return crear_mision(machine, {"tipo": tipo}, creada_por="dron")


def registrar_evento(machine: str, evento: dict, foto: bytes | None = None) -> dict | None:
    """Guarda un evento {"tipo": "mision", ...} del dron. Devuelve la alerta guardada (si era una)."""
    nombre = evento.get("evento")
    if nombre not in ("waypoint", "medicion", "alerta", "fin", "abortada", "rechazada"):
        return None
    if nombre == "rechazada":
        #El dron no aceptó la misión (por ejemplo, desarmado): se cierra la que se acaba de crear
        _cerrar_ultima(machine, "enviada", {"estado": "rechazada", "motivo": evento.get("motivo")})
        return None
    mision = mision_activa(machine, evento.get("mision"))
    numero = mision["number"]

    if nombre == "waypoint":
        db.missions.update_one({"number": numero}, {"$set": {
            "estado": "en_curso", "waypoint_actual": evento.get("indice"),
            "total_waypoints": evento.get("total", mision.get("total_waypoints"))}})
        return None

    if nombre == "medicion":
        lectura = {"mission_number": numero, "machine": machine, "mision": evento.get("mision"),
                   **solo(evento, campos_medicion), "date": ahora()}
        db.readings.insert_one(lectura)
        db.missions.update_one({"number": numero}, {"$set": {"estado": "en_curso"}})
        return None

    if nombre == "alerta":
        alerta = {"number": siguiente_numero("alerts", db.alerts), "mission_number": numero,
                  "machine": machine, **solo(evento, campos_alerta), "foto": None,
                  "estado": "pendiente", "date": ahora(), "atendida_por": None, "fecha_atencion": None}
        if foto:
            carpeta_fotos.mkdir(parents=True, exist_ok=True)
            nombre_foto = f"alerta_{alerta['number']}.jpg"
            (carpeta_fotos / nombre_foto).write_bytes(foto)
            alerta["foto"] = nombre_foto
        db.alerts.insert_one(alerta)
        db.missions.update_one({"number": numero}, {"$inc": {"n_alertas": 1}})
        alerta.pop("_id", None)
        return alerta

    if nombre == "fin":
        cambios = {"estado": "terminada", "fin": ahora()}
        if isinstance(evento.get("resultados"), list):
            cambios["resultados"] = evento["resultados"]
        db.missions.update_one({"number": numero}, {"$set": cambios})
        return None

    # abortada
    db.missions.update_one({"number": numero}, {"$set": {
        "estado": "abortada", "motivo": evento.get("motivo"), "fin": ahora()}})
    return None


def _cerrar_ultima(machine: str, estado: str, cambios: dict):
    """Cierra la misión más reciente del dron que esté en `estado`."""
    ultima = db.missions.find_one({"machine": machine, "estado": estado}, {"number": 1},
                                  sort=[("number", DESCENDING)])
    if ultima is not None:
        db.missions.update_one({"number": ultima["number"]}, {"$set": {**cambios, "fin": ahora()}})
