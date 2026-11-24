"""
Escritor de fondo: todo lo que los WebSockets necesitan guardar en Mongo pasa por aquí.

Por qué existe
--------------
pymongo es síncrono: cada insert_one espera a que Mongo responda. Si eso se hace dentro del
WebSocket del dron, el event loop (el único hilo que atiende a TODOS los clientes por turnos)
se queda parado esperando, y mientras tanto no se reenvía telemetría ni comandos. Es el mismo
problema que poner un delay() dentro del loop() del ESP32.

La solución es separar "recibir" de "guardar":
    WebSocket  --encolar()-->  cola en memoria  --tarea de fondo-->  hilo aparte  -->  Mongo
El WebSocket solo deja el trabajo en la cola (instantáneo) y sigue. Una tarea de fondo lo saca
y lo ejecuta con asyncio.to_thread, en un hilo que sí puede esperar a Mongo sin frenar a nadie.

Telemetría por lotes
--------------------
La telemetría llega varias veces por segundo. En vez de un insert por mensaje, se acumula y
cada `intervalo_telemetria_s` se guarda todo junto con un solo insert_many (menos viajes a Mongo).
Se guardan como máximo `max_guardar_hz` mensajes por segundo y dron (la meta del Anexo es >= 1 Hz).

Límites
-------
Si Mongo se cae, la cola no crece sin límite: con más de `max_trabajos` pendientes se descartan
los trabajos nuevos y se cuenta cuántos (estado()). Perder telemetría es mejor que quedarse sin RAM.
"""
import asyncio
import logging
import time
from collections import deque
from datetime import datetime, timezone
from typing import Callable

from pymongo.errors import CollectionInvalid, OperationFailure, PyMongoError

from Library.db_bm import db

log = logging.getLogger("samati.escritor")

max_trabajos = 10_000           # trabajos pendientes como máximo
max_telemetria = 20_000         # mensajes de telemetría pendientes como máximo
intervalo_telemetria_s = 1.0    # cada cuánto se guarda el lote de telemetría
max_guardar_hz = 5              # mensajes de telemetría por segundo y dron que se guardan

coleccion_telemetria = "machine_telemetry_data"


class escritor:
    def __init__(self):
        self.trabajos: deque = deque()
        self.telemetria: deque = deque(maxlen=max_telemetria)
        self.despertar = asyncio.Event()
        self.tarea: asyncio.Task | None = None
        self.ultimo_guardado: dict[str, float] = {}     # dron -> instante del último mensaje aceptado
        self.descartados = 0
        self.errores = 0
        self.guardados_telemetria = 0

    # ---- lo que llaman los WebSockets (en el event loop, nunca bloquea)
    def encolar(self, funcion: Callable, *args, al_terminar: Callable | None = None):
        """Ejecuta funcion(*args) en un hilo aparte. al_terminar(resultado) corre luego en el
        event loop (ahí sí se puede tocar el manager del WebSocket, que no es seguro entre hilos)."""
        if len(self.trabajos) >= max_trabajos:
            self.descartados += 1
            return
        self.trabajos.append((funcion, args, al_terminar))
        self.despertar.set()

    def telemetria_recibida(self, machine: str, datos: dict):
        """Guarda una copia de la telemetría (a lo más max_guardar_hz por dron)."""
        ahora = time.monotonic()
        if ahora - self.ultimo_guardado.get(machine, 0.0) < 1.0 / max_guardar_hz:
            return
        self.ultimo_guardado[machine] = ahora
        #Misma forma que POST /machine_telemetry_rt/sensor_data, así GET /sensor_data lee las dos fuentes
        self.telemetria.append({**datos, "machine_name": machine,
                                "date": datetime.now(timezone.utc), "fuente": "ws"})

    # ---- tarea de fondo
    async def correr(self):
        proximo_lote = time.monotonic() + intervalo_telemetria_s
        while True:
            espera = max(0.0, proximo_lote - time.monotonic())
            try:
                await asyncio.wait_for(self.despertar.wait(), espera)
            except asyncio.TimeoutError:
                pass
            self.despertar.clear()
            await self.vaciar_trabajos()
            if time.monotonic() >= proximo_lote:
                await self.guardar_telemetria()
                proximo_lote = time.monotonic() + intervalo_telemetria_s

    async def vaciar_trabajos(self):
        #En orden: la misión se crea antes de guardar sus lecturas
        while self.trabajos:
            funcion, args, al_terminar = self.trabajos.popleft()
            try:
                resultado = await asyncio.to_thread(funcion, *args)
            except PyMongoError as error:
                self.errores += 1
                log.warning("Error de Mongo en %s: %s", getattr(funcion, "__name__", funcion), error)
                continue
            except Exception:       # un error de programación no debe matar al escritor
                self.errores += 1
                log.exception("Error en el escritor de fondo")
                continue
            if al_terminar is not None:
                try:
                    al_terminar(resultado)
                except Exception:
                    log.exception("Error en al_terminar")

    async def guardar_telemetria(self):
        if not self.telemetria:
            return
        lote = list(self.telemetria)
        self.telemetria.clear()
        try:
            await asyncio.to_thread(db[coleccion_telemetria].insert_many, lote, ordered=False)
            self.guardados_telemetria += len(lote)
        except PyMongoError as error:
            self.errores += 1
            log.warning("No se pudo guardar la telemetría (%d mensajes perdidos): %s", len(lote), error)

    async def vaciar_todo(self):
        """Guarda lo pendiente; se llama al apagar la API y en las pruebas."""
        await self.vaciar_trabajos()
        await self.guardar_telemetria()

    def iniciar(self):
        if self.tarea is None or self.tarea.done():
            self.despertar = asyncio.Event()    # el Event pertenece al loop que lo usa
            self.tarea = asyncio.create_task(self.correr())

    async def detener(self):
        if self.tarea is not None:
            self.tarea.cancel()
            try:
                await self.tarea
            except asyncio.CancelledError:
                pass
            self.tarea = None
        await self.vaciar_todo()

    def estado(self) -> dict:
        return {"trabajos_pendientes": len(self.trabajos), "telemetria_pendiente": len(self.telemetria),
                "telemetria_guardada": self.guardados_telemetria,
                "descartados": self.descartados, "errores": self.errores}


escritor_global = escritor()


def preparar_coleccion_telemetria() -> str:
    """Crea la colección de telemetría como time series si aún no existe.

    Una time series de MongoDB (versión 5.0 o más nueva) está hecha para lecturas de sensores:
    agrupa los documentos por dron y por tiempo y los comprime. Si la colección ya existe como
    colección normal (datos de antes), se deja como está y solo se le pone un índice por fecha.
    Devuelve "time_series", "normal" o "existente".
    """
    if coleccion_telemetria in db.list_collection_names():
        tipo = "existente"
    else:
        try:
            db.create_collection(coleccion_telemetria, timeseries={
                "timeField": "date", "metaField": "machine_name", "granularity": "seconds"})
            return "time_series"
        except (CollectionInvalid, OperationFailure, NotImplementedError, TypeError) as error:
            #Mongo < 5.0 o mongomock (pruebas): colección normal
            log.info("Telemetría en colección normal (sin time series): %s", error)
            tipo = "normal"
    try:
        db[coleccion_telemetria].create_index([("machine_name", 1), ("date", -1)])
    except OperationFailure as error:
        log.warning("No se pudo crear el índice de telemetría: %s", error)
    return tipo
