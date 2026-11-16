"""Lanzador de AIMEWS.

Ventana pequeña para iniciar y detener los cuatro servicios del proyecto y ver sus eventos:
    - Base de datos: servicio de Windows "MongoDB" (127.0.0.1:27017). Iniciarlo o detenerlo
      pide permiso de administrador (UAC), igual que en services.msc.
    - API: uvicorn main:app en 127.0.0.1:8001 (carpeta API/).
    - Sitio web: servidor_sitio.py sirviendo sitio_web/ en 127.0.0.1:5500 sin caché.
    - Túnel de Cloudflare: cloudflared con ~/.cloudflared/samati_web.yml (api.superbaay.dev).

La salida de cada proceso va a logs/<clave>.log (no a una tubería): así los servicios siguen
vivos aunque se cierre la ventana y, al volver a abrirla, el lanzador los reconoce por su
logs/<clave>.pid y sigue mostrando sus eventos.
Se abre con pythonw.exe (sin consola) desde el acceso directo del escritorio.
"""
import ctypes
import json
import os
import queue
import shutil
import socket
import subprocess
import sys
import threading
import time
import tkinter as tk
import urllib.request
import webbrowser
from datetime import datetime
from pathlib import Path
from tkinter import messagebox, ttk

import psutil
from PIL import Image, ImageTk

carpeta_lanzador = Path(__file__).resolve().parent
raiz = carpeta_lanzador.parent
carpeta_logs = carpeta_lanzador / "logs"
#La ventana corre con pythonw.exe; los servicios con python.exe del mismo intérprete (tiene uvicorn)
python_consola = str(Path(sys.executable).with_name("python.exe"))
cloudflared_exe = shutil.which("cloudflared") or r"C:\Program Files (x86)\cloudflared\cloudflared.exe"
config_tunel = Path.home() / ".cloudflared" / "samati_web.yml"
puerto_metricas_tunel = 20247   #/ready responde 200 cuando hay al menos una conexión con Cloudflare
puerto_instancia = 47123        #solo para impedir dos lanzadores abiertos a la vez
max_lineas = 4000               #por pestaña de eventos
tamano_max_log = 5 * 1024 * 1024
banderas_proceso = subprocess.CREATE_NO_WINDOW | subprocess.CREATE_NEW_PROCESS_GROUP
#Solo estos programas se cierran si ya ocupaban el puerto de un servicio ("Reiniciar aquí" / "Detener todo")
programas_cerrables = {"python.exe", "pythonw.exe", "uvicorn.exe", "cloudflared.exe"}

colores = {
    "fondo": "#0a1428", "superficie": "#0d1a33", "hover": "#102042", "linea": "#1b2a4a",
    "texto": "#eef2fa", "secundario": "#a9b8d4", "tenue": "#8a9bbd",
    "acento": "#6fb4ff", "alerta": "#f2c46b", "error": "#ff8a78", "ok": "#7ddca0",
}
color_estado = {
    "activo": colores["ok"], "iniciando": colores["alerta"], "externo": colores["acento"],
    "detenido": "#4a5a7a", "error": colores["error"],
}


def clasificar_evento(linea: str) -> str:
    """Decide el color de una línea del visor de eventos.

    Devuelve una de las etiquetas que configura el visor:
        "error" (rojo), "aviso" (ámbar), "ok" (verde), "ws" (azul, WebSocket) o "info" (normal).
    Llegan líneas de tres programas distintos, por ejemplo:
        API (uvicorn): '16/11 12:00:01 INFO:     189.0.0.1:0 - "GET /machine_rt/ HTTP/1.1" 200 OK'
                       '16/11 12:00:02 INFO:     189.0.0.1:0 - "WebSocket /ws/machine" [accepted]'
                       '16/11 12:00:02 INFO:     connection closed'
                       '16/11 12:00:03 ERROR:    Exception in ASGI application'
        Sitio web:     '127.0.0.1 - - [16/Nov/2026 12:00:00] "GET / HTTP/1.1" 200 -'
        Túnel:         '2026-11-16T18:00:00Z ERR  error="..." connIndex=0'
    """
    # TODO(human): clasificar la línea (por ahora todo sale en color normal)
    return "info"


#--------------------------------------------------------------------------- utilidades de procesos

def pids_escuchando() -> dict[int, int]:
    """Puerto TCP en escucha -> PID dueño."""
    mapa = {}
    try:
        conexiones = psutil.net_connections(kind="tcp")
    except psutil.Error:
        return mapa
    for conexion in conexiones:
        if conexion.status == psutil.CONN_LISTEN and conexion.laddr and conexion.pid:
            mapa.setdefault(conexion.laddr.port, conexion.pid)
    return mapa


def procesos_cloudflared() -> list[tuple[int, str]]:
    lista = []
    for proceso in psutil.process_iter(["name", "cmdline"]):
        if (proceso.info["name"] or "").lower() == "cloudflared.exe":
            lista.append((proceso.pid, " ".join(proceso.info["cmdline"] or [])))
    return lista


def arbol(proceso: psutil.Process) -> set[int]:
    try:
        return {proceso.pid, *(hijo.pid for hijo in proceso.children(recursive=True))}
    except psutil.Error:
        return {proceso.pid}


def terminar_arbol(pid: int, espera: float = 4.0):
    try:
        padre = psutil.Process(pid)
    except psutil.NoSuchProcess:
        return
    procesos = [padre]
    try:
        procesos += padre.children(recursive=True)
    except psutil.Error:
        pass
    for proceso in procesos:
        try:
            proceso.terminate()
        except psutil.Error:
            pass
    _, vivos = psutil.wait_procs(procesos, timeout=espera)
    for proceso in vivos:
        try:
            proceso.kill()
        except psutil.Error:
            pass


def ejecutar_como_admin(programa: str, argumentos: str) -> bool:
    """Lanza con UAC; False si el usuario cancela el permiso."""
    return ctypes.windll.shell32.ShellExecuteW(None, "runas", programa, argumentos, None, 0) > 32


def rotar_log(ruta: Path):
    try:
        if ruta.stat().st_size > tamano_max_log:
            ruta.replace(ruta.with_suffix(".log.1"))
    except OSError:
        pass    #no existe o algún proceso externo lo tiene abierto: se sigue anexando


#--------------------------------------------------------------------------- servicios

class servicio_proceso:
    """Servicio que el lanzador arranca como proceso hijo (API, sitio, túnel)."""

    texto_iniciando = "Iniciando…"

    def __init__(self, clave, nombre, detalle, comando, cwd, puerto=None, url=None, marca=None):
        self.clave = clave
        self.nombre = nombre
        self.detalle = detalle
        self.comando = comando
        self.cwd = cwd
        self.puerto = puerto
        self.url = url
        self.marca = marca  #texto de la línea de comandos que identifica al proceso si no tiene puerto
        self.log = carpeta_logs / f"{clave}.log"
        self.archivo_pid = carpeta_logs / f"{clave}.pid"
        self.popen = None
        self.pid_externo = None
        self.programa_externo = ""
        self.estado = ("detenido", "Detenido")
        self.ocupado = False

    def proceso_propio(self) -> psutil.Process | None:
        """El proceso que inició el lanzador (en esta sesión o en una anterior), si sigue vivo."""
        try:
            datos = json.loads(self.archivo_pid.read_text())
            proceso = psutil.Process(datos["pid"])
            #Se compara la hora de creación para no confundirlo con otro proceso que reutilice el PID
            if abs(proceso.create_time() - datos["creado"]) < 1.0:
                return proceso
        except (OSError, ValueError, KeyError, psutil.Error):
            pass
        return None

    def buscar_externo(self, escuchando, cloudflareds) -> int | None:
        if self.puerto is not None:
            return escuchando.get(self.puerto)
        for pid, linea in cloudflareds:
            if self.marca and self.marca in linea:
                return pid
        return None

    def listo(self, propio, escuchando) -> bool:
        return self.puerto is None or escuchando.get(self.puerto) in arbol(propio)

    def calcular_estado(self, escuchando, cloudflareds):
        propio = self.proceso_propio()
        if propio:
            self.pid_externo = None
            if self.listo(propio, escuchando):
                self.estado = ("activo", f"Activo · PID {propio.pid}")
            else:
                self.estado = ("iniciando", self.texto_iniciando)
            return
        self.pid_externo = self.buscar_externo(escuchando, cloudflareds)
        if self.pid_externo:
            try:
                self.programa_externo = psutil.Process(self.pid_externo).name()
            except psutil.Error:
                self.programa_externo = "?"
            programa = self.programa_externo.removesuffix(".exe")
            self.estado = ("externo", f"Fuera del lanzador · {programa} (PID {self.pid_externo})")
        elif self.popen is not None and self.popen.poll() is not None:
            self.estado = ("error", f"Se detuvo (código {self.popen.returncode}) · mira sus eventos")
        else:
            self.estado = ("detenido", "Detenido")

    def iniciar(self):
        if self.proceso_propio():
            return
        carpeta_logs.mkdir(exist_ok=True)
        rotar_log(self.log)
        entorno = {**os.environ, "PYTHONUNBUFFERED": "1", "PYTHONIOENCODING": "utf-8"}
        with open(self.log, "ab") as salida:
            marca = f"──── Iniciado desde el lanzador {datetime.now():%d/%m/%Y %H:%M:%S} ────\n"
            salida.write(marca.encode("utf-8"))
            salida.flush()
            self.popen = subprocess.Popen(self.comando, cwd=self.cwd, stdin=subprocess.DEVNULL,
                                          stdout=salida, stderr=subprocess.STDOUT,
                                          env=entorno, creationflags=banderas_proceso)
        proceso = psutil.Process(self.popen.pid)
        self.archivo_pid.write_text(json.dumps({"pid": proceso.pid, "creado": proceso.create_time()}))

    def detener(self):
        propio = self.proceso_propio()
        pid = propio.pid if propio else self.pid_externo
        if not propio and self.programa_externo.lower() not in programas_cerrables:
            pid = None  #un programa ajeno (VS Code…) nunca se cierra desde aquí
        if pid:
            terminar_arbol(pid)
        self.popen = None   #detenerlo a mano no es un error
        self.archivo_pid.unlink(missing_ok=True)


class servicio_tunel(servicio_proceso):
    texto_iniciando = "Conectando con Cloudflare…"

    def listo(self, propio, escuchando) -> bool:
        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{puerto_metricas_tunel}/ready", timeout=0.5) as r:
                return r.status == 200
        except OSError:
            return False


class servicio_mongo:
    """MongoDB corre como servicio de Windows: se controla con sc.exe (requiere administrador)."""

    clave = "db"
    nombre = "Base de datos"
    detalle = "MongoDB 8.2 · servicio de Windows · 127.0.0.1:27017"
    url = None
    log = None

    def __init__(self):
        self.estado = ("detenido", "Detenido")
        self.ocupado = False

    def calcular_estado(self, escuchando, cloudflareds):
        try:
            estado = psutil.win_service_get("MongoDB").status()
        except psutil.Error:
            self.estado = ("error", "El servicio MongoDB no está instalado")
            return
        if estado == "running" and 27017 in escuchando:
            self.estado = ("activo", "Activo · servicio de Windows")
        elif estado in ("running", "start_pending"):
            self.estado = ("iniciando", "Iniciando…")
        elif estado == "stop_pending":
            self.estado = ("iniciando", "Deteniéndose…")
        else:
            self.estado = ("detenido", "Detenido")

    def iniciar(self):
        if not ejecutar_como_admin("sc.exe", "start MongoDB"):
            raise RuntimeError("Se canceló el permiso de administrador para iniciar MongoDB.")

    def detener(self):
        if not ejecutar_como_admin("sc.exe", "stop MongoDB"):
            raise RuntimeError("Se canceló el permiso de administrador para detener MongoDB.")


#--------------------------------------------------------------------------- lectura de logs

class seguidor_log(threading.Thread):
    """Sigue un archivo de log como `tail -f` y manda cada línea nueva a la cola."""

    def __init__(self, clave: str, ruta: Path, cola: queue.Queue):
        super().__init__(daemon=True)
        self.clave = clave
        self.ruta = ruta
        self.cola = cola

    def run(self):
        posicion = None
        resto = b""
        descartar_primera = False
        while True:
            try:
                tamano = self.ruta.stat().st_size
            except OSError:
                time.sleep(0.5)
                continue
            if posicion is None:
                posicion = max(0, tamano - 32768)   #al abrir se muestra solo lo último
                descartar_primera = posicion > 0
            if tamano < posicion:   #se rotó el log
                posicion, resto = 0, b""
            if tamano > posicion:
                try:
                    with open(self.ruta, "rb") as archivo:
                        archivo.seek(posicion)
                        datos = archivo.read(tamano - posicion)
                        posicion = archivo.tell()
                except OSError:
                    time.sleep(0.5)
                    continue
                lineas = (resto + datos).split(b"\n")
                resto = lineas.pop()    #línea a medio escribir: se completa en la siguiente vuelta
                if descartar_primera and lineas:
                    lineas.pop(0)
                    descartar_primera = False
                for linea in lineas:
                    self.cola.put((self.clave, linea.decode("utf-8", "replace").rstrip("\r")))
            time.sleep(0.25)


#--------------------------------------------------------------------------- ventana

class lanzador(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("AIMEWS · Lanzador")
        self.geometry("880x660")
        self.minsize(760, 520)
        self.configure(bg=colores["fondo"])
        try:
            self.iconbitmap(default=str(carpeta_lanzador / "logo.ico"))
        except tk.TclError:
            pass

        self.cola = queue.Queue()
        self.cerrando = False
        api = raiz / "API"
        self.servicios = [
            servicio_mongo(),
            servicio_proceso("api", "API", "FastAPI · uvicorn · 127.0.0.1:8001",
                             [python_consola, "-m", "uvicorn", "main:app", "--host", "127.0.0.1",
                              "--port", "8001", "--log-config", str(carpeta_lanzador / "uvicorn_log.json")],
                             cwd=api, puerto=8001, url="http://127.0.0.1:8001/docs"),
            servicio_proceso("sitio", "Sitio web", "servidor_sitio.py · sitio_web · 127.0.0.1:5500",
                             [python_consola, "-u", str(raiz / "servidor_sitio.py"), str(raiz / "sitio_web"), "5500"],
                             cwd=raiz, puerto=5500, url="http://127.0.0.1:5500"),
            servicio_tunel("tunel", "Túnel de Cloudflare", "cloudflared · samati_web.yml · api.superbaay.dev",
                           [cloudflared_exe, "tunnel", "--config", str(config_tunel),
                            "--metrics", f"127.0.0.1:{puerto_metricas_tunel}", "run"],
                           cwd=raiz, url="https://api.superbaay.dev", marca="samati_web.yml"),
        ]
        self.por_clave = {s.clave: s for s in self.servicios}

        self.crear_estilos()
        self.crear_cabecera()
        self.crear_filas()
        self.crear_eventos()

        carpeta_logs.mkdir(exist_ok=True)
        for s in self.servicios:
            if s.log is not None:
                seguidor_log(s.clave, s.log, self.cola).start()
        threading.Thread(target=self.vigilar, daemon=True).start()
        self.after(150, self.refrescar)
        self.protocol("WM_DELETE_WINDOW", self.al_cerrar)

    def report_callback_exception(self, tipo, valor, rastro):
        """Con pythonw no hay consola: los errores de la ventana se guardan en un log."""
        import traceback
        carpeta_logs.mkdir(exist_ok=True)
        with open(carpeta_logs / "lanzador_errores.log", "a", encoding="utf-8") as archivo:
            archivo.write(f"\n{datetime.now():%d/%m/%Y %H:%M:%S}\n")
            traceback.print_exception(tipo, valor, rastro, file=archivo)

    #---------------------------------------------------------------- construcción
    def crear_estilos(self):
        estilo = ttk.Style(self)
        estilo.theme_use("clam")
        c = colores
        estilo.configure(".", background=c["fondo"], foreground=c["texto"], font=("Segoe UI", 10),
                         bordercolor=c["linea"], lightcolor=c["linea"], darkcolor=c["linea"])
        estilo.configure("TFrame", background=c["fondo"])
        estilo.configure("Tarjeta.TFrame", background=c["superficie"])
        estilo.configure("TLabel", background=c["fondo"], foreground=c["texto"])
        estilo.configure("Titulo.TLabel", font=("Segoe UI Semibold", 17))
        estilo.configure("Subtitulo.TLabel", foreground=c["tenue"], font=("Segoe UI", 10))
        estilo.configure("Nombre.TLabel", background=c["superficie"], font=("Segoe UI Semibold", 11))
        estilo.configure("Detalle.TLabel", background=c["superficie"], foreground=c["tenue"], font=("Segoe UI", 9))
        estilo.configure("Estado.TLabel", background=c["superficie"], foreground=c["secundario"], font=("Segoe UI", 9))
        estilo.configure("Aviso.TLabel", background=c["hover"], foreground=c["alerta"], font=("Segoe UI", 9),
                         padding=(10, 6))
        estilo.configure("TButton", background=c["hover"], foreground=c["texto"], padding=(12, 5),
                         borderwidth=0, focusthickness=0)
        estilo.map("TButton", background=[("disabled", c["superficie"]), ("active", c["linea"])],
                   foreground=[("disabled", c["tenue"])])
        estilo.configure("Acento.TButton", background=c["acento"], foreground=c["fondo"],
                         font=("Segoe UI Semibold", 10))
        estilo.map("Acento.TButton", background=[("disabled", c["hover"]), ("active", c["acento"])])
        estilo.configure("Enlace.TButton", background=c["superficie"], foreground=c["acento"], padding=(8, 5))
        estilo.map("Enlace.TButton", background=[("active", c["hover"])], foreground=[("disabled", c["linea"])])
        estilo.configure("TCheckbutton", background=c["fondo"], foreground=c["secundario"],
                         indicatorbackground=c["superficie"], indicatorforeground=c["acento"])
        estilo.map("TCheckbutton", background=[("active", c["fondo"])],
                   indicatorbackground=[("selected", c["superficie"])])
        estilo.configure("TNotebook", background=c["fondo"], borderwidth=0, tabmargins=(0, 0, 0, 0))
        estilo.configure("TNotebook.Tab", background=c["superficie"], foreground=c["tenue"], padding=(14, 6),
                         borderwidth=0)
        estilo.map("TNotebook.Tab", background=[("selected", c["hover"])], foreground=[("selected", c["texto"])])
        estilo.configure("Vertical.TScrollbar", background=c["hover"], troughcolor=c["superficie"],
                         arrowcolor=c["tenue"], borderwidth=0, bordercolor=c["superficie"],
                         lightcolor=c["hover"], darkcolor=c["hover"], arrowsize=12)
        estilo.map("Vertical.TScrollbar", background=[("active", c["linea"])])

    def crear_cabecera(self):
        cabecera = ttk.Frame(self, padding=(18, 14, 18, 8))
        cabecera.pack(fill="x")
        imagen = Image.open(carpeta_lanzador / "logo.png").resize((40, 40), Image.LANCZOS)
        self.logo = ImageTk.PhotoImage(imagen)
        self.iconphoto(True, ImageTk.PhotoImage(Image.open(carpeta_lanzador / "logo.png").resize((64, 64))))
        ttk.Label(cabecera, image=self.logo).pack(side="left", padx=(0, 12))
        textos = ttk.Frame(cabecera)
        textos.pack(side="left")
        ttk.Label(textos, text="AIMEWS", style="Titulo.TLabel").pack(anchor="w")
        ttk.Label(textos, text="Servicios de SAMATI", style="Subtitulo.TLabel").pack(anchor="w")
        botones = ttk.Frame(cabecera)
        botones.pack(side="right")
        ttk.Button(botones, text="Iniciar todo", style="Acento.TButton",
                   command=self.iniciar_todo).pack(side="left", padx=4)
        ttk.Button(botones, text="Detener todo", command=self.detener_todo).pack(side="left", padx=4)

    def crear_filas(self):
        contenedor = ttk.Frame(self, padding=(18, 4, 18, 6))
        contenedor.pack(fill="x")
        self.filas = {}
        for s in self.servicios:
            tarjeta = ttk.Frame(contenedor, style="Tarjeta.TFrame", padding=(12, 8))
            tarjeta.pack(fill="x", pady=3)
            tarjeta.columnconfigure(1, weight=1)
            punto = tk.Canvas(tarjeta, width=14, height=14, bg=colores["superficie"], highlightthickness=0)
            punto.grid(row=0, column=0, rowspan=2, padx=(0, 12))
            ovalo = punto.create_oval(2, 2, 12, 12, fill=color_estado["detenido"], outline="")
            ttk.Label(tarjeta, text=s.nombre, style="Nombre.TLabel").grid(row=0, column=1, sticky="w")
            ttk.Label(tarjeta, text=s.detalle, style="Detalle.TLabel").grid(row=1, column=1, sticky="w")
            estado = ttk.Label(tarjeta, text="…", style="Estado.TLabel", width=44, anchor="e")
            estado.grid(row=0, column=2, rowspan=2, padx=10)
            enlace = ttk.Button(tarjeta, text="Abrir ↗", style="Enlace.TButton",
                                command=lambda u=s.url: webbrowser.open(u))
            if s.url:
                enlace.grid(row=0, column=3, rowspan=2, padx=(0, 6))
            boton = ttk.Button(tarjeta, text="Iniciar", width=14, command=lambda srv=s: self.accion(srv))
            boton.grid(row=0, column=4, rowspan=2)
            self.filas[s.clave] = {"punto": punto, "ovalo": ovalo, "estado": estado, "boton": boton,
                                   "enlace": enlace}

    def crear_eventos(self):
        marco = ttk.Frame(self, padding=(18, 6, 18, 14))
        marco.pack(fill="both", expand=True)
        barra = ttk.Frame(marco)
        barra.pack(fill="x", pady=(0, 6))
        ttk.Label(barra, text="Eventos", font=("Segoe UI Semibold", 12)).pack(side="left")
        self.seguir_final = tk.BooleanVar(value=True)
        ttk.Button(barra, text="Abrir carpeta de logs",
                   command=lambda: os.startfile(carpeta_logs)).pack(side="right", padx=(6, 0))
        ttk.Button(barra, text="Limpiar", command=self.limpiar_eventos).pack(side="right", padx=(6, 0))
        ttk.Checkbutton(barra, text="Seguir el final", variable=self.seguir_final).pack(side="right", padx=6)

        self.pestanas = ttk.Notebook(marco)
        self.pestanas.pack(fill="both", expand=True)
        self.textos = {}
        self.avisos = {}
        for s in self.servicios:
            if s.log is None:
                continue
            pagina = ttk.Frame(self.pestanas)
            self.pestanas.add(pagina, text=s.nombre)
            aviso = ttk.Label(pagina, style="Aviso.TLabel",
                              text=f"{s.nombre} ya estaba corriendo fuera del lanzador (por ejemplo en una terminal): "
                                   "sus eventos no llegan aquí. Pulsa «Reiniciar aquí» para verlos.")
            texto = tk.Text(pagina, bg=colores["superficie"], fg=colores["secundario"], insertbackground=colores["texto"],
                            relief="flat", borderwidth=0, padx=10, pady=8, wrap="word",
                            font=("Consolas", 9), state="disabled", selectbackground=colores["linea"])
            barra_y = ttk.Scrollbar(pagina, orient="vertical", command=texto.yview)
            texto.configure(yscrollcommand=barra_y.set)
            barra_y.pack(side="right", fill="y")
            texto.pack(side="left", fill="both", expand=True)
            for etiqueta, color in (("error", colores["error"]), ("aviso", colores["alerta"]),
                                    ("ok", colores["ok"]), ("ws", colores["acento"]),
                                    ("info", colores["secundario"])):
                texto.tag_configure(etiqueta, foreground=color)
            texto.tag_configure("separador", foreground=colores["tenue"], font=("Consolas", 9, "italic"))
            self.textos[s.clave] = texto
            self.avisos[s.clave] = (aviso, barra_y)  #el aviso se empaqueta antes que las barras: ocupa todo el ancho

    #---------------------------------------------------------------- bucle
    def vigilar(self):
        """Hilo aparte: psutil y la consulta al túnel no deben congelar la ventana."""
        while not self.cerrando:
            escuchando = pids_escuchando()
            cloudflareds = procesos_cloudflared()
            for s in self.servicios:
                try:
                    s.calcular_estado(escuchando, cloudflareds)
                except Exception as error:     #un fallo raro de un servicio no apaga la vigilancia
                    s.estado = ("error", str(error)[:60])
            time.sleep(1.0)

    def refrescar(self):
        for s in self.servicios:
            codigo, texto = s.estado
            fila = self.filas[s.clave]
            fila["punto"].itemconfigure(fila["ovalo"], fill=color_estado[codigo])
            fila["estado"].configure(text=texto)
            if s.ocupado:
                fila["boton"].configure(text="Espera…", state="disabled")
            else:
                etiqueta = {"activo": "Detener", "iniciando": "Detener", "externo": "Reiniciar aquí"}.get(codigo, "Iniciar")
                if codigo == "externo" and getattr(s, "programa_externo", "").lower() not in programas_cerrables:
                    etiqueta = "Puerto ocupado"
                fila["boton"].configure(text=etiqueta, state="normal",
                                        style="TButton" if etiqueta != "Iniciar" else "Acento.TButton")
            fila["enlace"].configure(state="normal" if codigo in ("activo", "externo") else "disabled")
            if s.clave in self.avisos:
                aviso, primero = self.avisos[s.clave]
                if codigo == "externo" and not aviso.winfo_manager():
                    aviso.pack(side="top", fill="x", before=primero)
                elif codigo != "externo" and aviso.winfo_manager():
                    aviso.pack_forget()
        self.volcar_eventos()
        self.after(250, self.refrescar)

    def volcar_eventos(self):
        nuevas = {}
        for _ in range(800):
            try:
                clave, linea = self.cola.get_nowait()
            except queue.Empty:
                break
            if clave == "_error":
                messagebox.showerror("AIMEWS", linea, parent=self)
                continue
            nuevas.setdefault(clave, []).append(linea)
        for clave, lineas in nuevas.items():
            texto = self.textos.get(clave)
            if texto is None:
                continue
            texto.configure(state="normal")
            for linea in lineas:
                if linea.startswith("────"):
                    etiqueta = "separador"
                else:
                    try:
                        etiqueta = clasificar_evento(linea)
                    except Exception:
                        etiqueta = "info"
                texto.insert("end", linea + "\n", etiqueta)
            sobrantes = int(texto.index("end-1c").split(".")[0]) - max_lineas
            if sobrantes > 0:
                texto.delete("1.0", f"{sobrantes + 1}.0")
            texto.configure(state="disabled")
            if self.seguir_final.get():
                texto.see("end")

    def limpiar_eventos(self):
        pagina = self.pestanas.select()
        indice = self.pestanas.index(pagina)
        texto = list(self.textos.values())[indice]
        texto.configure(state="normal")
        texto.delete("1.0", "end")
        texto.configure(state="disabled")

    #---------------------------------------------------------------- acciones
    def en_hilo(self, servicio, trabajo):
        def correr():
            servicio.ocupado = True
            try:
                trabajo()
            except Exception as error:
                self.cola.put(("_error", f"{servicio.nombre}: {error}"))
            finally:
                servicio.ocupado = False
        threading.Thread(target=correr, daemon=True).start()

    def accion(self, s):
        codigo = s.estado[0]
        if codigo == "externo" and s.programa_externo.lower() not in programas_cerrables:
            #p. ej. Live Server de VS Code en :5500: cerrarlo desde aquí tumbaría VS Code
            messagebox.showinfo(
                "Puerto ocupado",
                f"El puerto de {s.nombre} lo está usando {s.programa_externo} (PID {s.pid_externo}), "
                "que no es parte del proyecto.\n\nCiérralo desde ese programa (en VS Code: botón "
                "«Port : 5500» de la barra inferior para parar Live Server) y luego pulsa «Iniciar».",
                parent=self)
            return
        if codigo == "externo":
            if not messagebox.askyesno(
                    "Reiniciar desde el lanzador",
                    f"{s.nombre} se inició fuera del lanzador (PID {s.pid_externo}).\n\n"
                    "Se va a cerrar ese proceso y abrir uno nuevo desde aquí para poder ver sus eventos. ¿Continuar?",
                    parent=self):
                return

            def reiniciar():
                s.detener()
                time.sleep(0.5)     #que el puerto quede libre
                s.iniciar()
            self.en_hilo(s, reiniciar)
        elif codigo in ("activo", "iniciando"):
            self.en_hilo(s, s.detener)
        else:
            self.en_hilo(s, s.iniciar)

    def iniciar_todo(self):
        def trabajo():
            db = self.por_clave["db"]
            if db.estado[0] in ("detenido", "error"):
                db.iniciar()
                limite = time.time() + 15   #la API necesita la base de datos al atender peticiones
                while db.estado[0] != "activo" and time.time() < limite:
                    time.sleep(0.5)
            for clave in ("api", "sitio", "tunel"):
                s = self.por_clave[clave]
                if s.estado[0] in ("detenido", "error"):
                    s.iniciar()
        self.en_hilo(self.por_clave["db"], trabajo)

    def detener_todo(self):
        objetivos = []
        for clave in ("tunel", "sitio", "api"):
            s = self.por_clave[clave]
            codigo = s.estado[0]
            if codigo in ("activo", "iniciando") or (
                    codigo == "externo" and s.programa_externo.lower() in programas_cerrables):
                objetivos.append(s)
        if not objetivos:
            return
        nombres = ", ".join(s.nombre for s in objetivos)
        if not messagebox.askyesno("Detener todo", f"Se detendrán: {nombres}.\n\n"
                                   "La base de datos se deja encendida (es un servicio de Windows que "
                                   "arranca con el equipo; se detiene con su propio botón).", parent=self):
            return
        for s in objetivos:
            self.en_hilo(s, s.detener)

    def al_cerrar(self):
        propios = [s for s in self.servicios if isinstance(s, servicio_proceso) and s.proceso_propio()]
        if propios:
            nombres = ", ".join(s.nombre for s in propios)
            respuesta = messagebox.askyesnocancel(
                "Cerrar lanzador",
                f"¿Detener también lo que se inició desde aquí ({nombres})?\n\n"
                "Sí: se detienen.\nNo: siguen funcionando en segundo plano; al abrir de nuevo el "
                "lanzador se reconocen y se siguen viendo sus eventos.", parent=self)
            if respuesta is None:
                return
            if respuesta:
                self.title("AIMEWS · Deteniendo servicios…")
                self.update()
                for s in propios:
                    s.detener()
        self.cerrando = True
        self.destroy()


def ya_hay_otro_abierto() -> socket.socket | None:
    """Reserva un puerto local; si ya está tomado, el lanzador ya está abierto."""
    candado = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        candado.bind(("127.0.0.1", puerto_instancia))
    except OSError:
        return None
    return candado


if __name__ == "__main__":
    try:
        #Icono propio en la barra de tareas (si no, Windows agrupa la ventana con pythonw)
        ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID("AIMEWS.Lanzador")
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except (AttributeError, OSError):
        pass
    candado = ya_hay_otro_abierto()
    if candado is None:
        raiz_tk = tk.Tk()
        raiz_tk.withdraw()
        messagebox.showinfo("AIMEWS", "El lanzador de AIMEWS ya está abierto.")
        sys.exit(0)
    lanzador().mainloop()
