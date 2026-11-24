"""
Consulta de misiones, lecturas por waypoint y alertas (fase 3 del plan de mejoras).

Las misiones NO se crean por HTTP: nacen cuando el panel manda iniciar_mision por el WebSocket
y el dron la recibe, y se llenan con los eventos que manda el dron (ver Library/misiones_bm.py).
Este router solo las consulta y deja marcar una alerta como atendida o descartada.

    GET   /missions                      lista (filtros: machine, estado, tipo)       rol 3+
    GET   /missions/{number}             misión con sus lecturas                      rol 3+
    GET   /missions/{number}/comparacion lecturas de una misión inundada junto a las
                                         de su misión en seco de referencia, por waypoint
    GET   /alerts                        lista (filtros: estado, machine)             rol 3+
    PATCH /alerts/{number}               {"estado": "atendida" | "descartada"}        rol 4+
    GET   /alerts/{number}/foto          JPEG de la ESP32-CAM al llegar la alerta     rol 3+
    GET   /escritor/estado               cola del escritor de fondo (diagnóstico)     O5-1

Todas son def (no async): solo consultan Mongo y FastAPI las corre en un hilo aparte, así
no frenan los WebSockets.
"""
from datetime import datetime, timezone
from typing import Literal

from fastapi import APIRouter, Depends, HTTPException, Query, status
from fastapi.responses import FileResponse
from pydantic import BaseModel
from pymongo import DESCENDING

from Library import misiones_bm, segurity_bm
from Library.db_bm import db
from Library.escritor_bm import escritor_global

router = APIRouter(tags=["Missions"])

sin_id = {"_id": 0}


class alerta_cambio(BaseModel):
    estado: Literal["atendida", "descartada", "pendiente"]


def buscar_mision(number: int) -> dict:
    mision = db.missions.find_one({"number": number}, sin_id)
    if mision is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, detail="Mission not found")
    return mision


def lecturas_de(number: int) -> list[dict]:
    return list(db.readings.find({"mission_number": number}, sin_id).sort("indice", 1))


@router.get("/missions")
def list_missions(machine: str | None = None, estado: str | None = None,
                  tipo: Literal["seco", "inundado"] | None = None,
                  limit: int = Query(50, ge=1, le=500),
                  token: dict = Depends(segurity_bm.verify_role_Depends_3)):
    filtro = {}
    if machine:
        filtro["machine"] = machine
    if estado:
        filtro["estado"] = estado
    if tipo:
        filtro["tipo"] = tipo
    #Sin waypoints ni resultados: la lista solo necesita el resumen
    proyeccion = {"_id": 0, "waypoints": 0, "resultados": 0}
    return list(db.missions.find(filtro, proyeccion).sort("number", DESCENDING).limit(limit))


@router.get("/missions/{number}")
def get_mission(number: int, token: dict = Depends(segurity_bm.verify_role_Depends_3)):
    mision = buscar_mision(number)
    mision["lecturas"] = lecturas_de(number)
    mision["alertas"] = list(db.alerts.find({"mission_number": number}, sin_id).sort("number", 1))
    return mision


@router.get("/missions/{number}/comparacion")
def compare_mission(number: int, token: dict = Depends(segurity_bm.verify_role_Depends_3)):
    """Una fila por waypoint: lo medido en seco, lo medido inundado y la profundidad que reportó el dron.

    La profundidad oficial es la del dron (en "resultados" del evento fin), porque él aplica las
    correcciones del barómetro. La diferencia simple de niveles se muestra solo como referencia.
    """
    mision = buscar_mision(number)
    if mision.get("tipo") != "inundado":
        raise HTTPException(status.HTTP_400_BAD_REQUEST, detail="Only flooded missions have a comparison")
    referencia = mision.get("referencia_number")
    seco = {l.get("indice"): l for l in lecturas_de(referencia)} if referencia else {}
    resultados = {r.get("indice"): r for r in mision.get("resultados") or []}
    filas = []
    for lectura in lecturas_de(number):
        indice = lectura.get("indice")
        base = seco.get(indice)
        diferencia = None
        if base and lectura.get("valida") and base.get("valida") \
                and lectura.get("nivel_superficie_m") is not None and base.get("nivel_superficie_m") is not None:
            diferencia = round(lectura["nivel_superficie_m"] - base["nivel_superficie_m"], 3)
        filas.append({
            "indice": indice, "latitud": lectura.get("latitud"), "longitud": lectura.get("longitud"),
            "seco": base, "inundado": lectura,
            "diferencia_niveles_m": diferencia,
            "profundidad_m": (resultados.get(indice) or {}).get("profundidad_m"),
            "alerta": (resultados.get(indice) or {}).get("alerta"),
        })
    return {"mission": number, "referencia": referencia, "filas": filas}


@router.get("/alerts")
def list_alerts(estado: Literal["pendiente", "atendida", "descartada"] | None = None,
                machine: str | None = None, limit: int = Query(100, ge=1, le=1000),
                token: dict = Depends(segurity_bm.verify_role_Depends_3)):
    filtro = {}
    if estado:
        filtro["estado"] = estado
    if machine:
        filtro["machine"] = machine
    return list(db.alerts.find(filtro, sin_id).sort("number", DESCENDING).limit(limit))


@router.patch("/alerts/{number}")
def update_alert(number: int, cambio: alerta_cambio, token: dict = Depends(segurity_bm.verify_role_Depends_4)):
    cambios = {"estado": cambio.estado, "atendida_por": token.get("name"),
               "fecha_atencion": datetime.now(timezone.utc)}
    if cambio.estado == "pendiente":
        cambios.update(atendida_por=None, fecha_atencion=None)
    resultado = db.alerts.update_one({"number": number}, {"$set": cambios})
    if resultado.matched_count == 0:
        raise HTTPException(status.HTTP_404_NOT_FOUND, detail="Alert not found")
    return db.alerts.find_one({"number": number}, sin_id)


#Protegida con token: el panel la pide con fetch y la muestra con un blob (un <img src> no manda Authorization)
@router.get("/alerts/{number}/foto")
def get_alert_photo(number: int, token: dict = Depends(segurity_bm.verify_role_Depends_3)):
    alerta = db.alerts.find_one({"number": number}, {"foto": 1})
    nombre = (alerta or {}).get("foto")
    #El nombre lo genera misiones_bm ("alerta_<n>.jpg"); se compara exacto para no servir otra ruta
    if not nombre or nombre != f"alerta_{number}.jpg" or not (misiones_bm.carpeta_fotos / nombre).is_file():
        raise HTTPException(status.HTTP_404_NOT_FOUND, detail="Photo not found")
    return FileResponse(misiones_bm.carpeta_fotos / nombre, media_type="image/jpeg")


@router.get("/escritor/estado")
def writer_status(token: dict = Depends(segurity_bm.verify_role_Depends_O5)):
    return escritor_global.estado()
