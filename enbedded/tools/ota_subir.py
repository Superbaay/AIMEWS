"""
ota_subir.py — compila el firmware del dron y lo instala por OTA, sin cable ni usuario.

Lo usa Claude (o cualquiera en esta PC) para subir código nuevo al dron:

    python enbedded/tools/ota_subir.py                      # compila, sube y despliega a dron_main_01
    python enbedded/tools/ota_subir.py --maquina dron_main_02
    python enbedded/tools/ota_subir.py --sin-compilar       # sube el build/*.bin que ya existe
    python enbedded/tools/ota_subir.py --solo-subir         # lo deja en la API sin mandarlo al dron

Qué hace:
    1. Compila con ESP-IDF 6.0 (arma el PATH a mano: export.ps1 no funciona en esta PC
       porque cmake/ninja del sistema son de otra versión).
    2. Sube build/<proyecto>.bin a POST /ota/upload con la cabecera X-OTA-Key.
       La clave se lee de API/ota_deploy_key.txt (la crea la API al arrancar, está en
       .gitignore): nadie escribe ni ve una contraseña.
    3. POST /ota/deploy al dron y sigue GET /ota/status hasta que el dron confirme que
       arrancó con la versión nueva y se conectó a la API. Si la versión nueva falla,
       el dron vuelve solo a la anterior (rollback) y el script lo reporta.

Requisitos: la API corriendo (por defecto http://127.0.0.1:8001), el dron conectado a
ella con un firmware que ya tenga OTA (0.3.0 o posterior), y /ota abierto en el túnel de
Cloudflare (el dron descarga por https://api.superbaay.dev). Solo usa la biblioteca estándar.
"""
import argparse
import glob
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request
import uuid
from pathlib import Path

raiz_repo = Path(__file__).resolve().parents[2]
ruta_clave = raiz_repo / "API" / "ota_deploy_key.txt"
herramientas = Path.home() / ".espressif" / "tools"
ruta_idf = Path(os.environ.get("IDF_PATH", r"C:\Espressif\frameworks\esp-idf-v6.0.3"))
tiempo_max_s = 300


def salir(texto, codigo=1):
    print(f"ERROR: {texto}", file=sys.stderr)
    sys.exit(codigo)


# ---------------------------------------------------------------------------
#  Compilación
# ---------------------------------------------------------------------------

def primera(patron):
    encontrados = sorted(glob.glob(str(herramientas / patron)))
    return encontrados[-1] if encontrados else None


def entorno_idf():
    python_idf = primera("../python_env/idf6.0_*/Scripts/python.exe")
    carpetas = [primera("xtensa-esp-elf/*/xtensa-esp-elf/bin"), primera("riscv32-esp-elf/*/riscv32-esp-elf/bin"),
                primera("esp32ulp-elf/*/esp32ulp-elf/bin"), primera("cmake/4.0.*/bin"), primera("ninja/1.12.*"),
                primera("ccache/*/ccache-*")]
    if not python_idf or None in carpetas:
        salir("no encontré ESP-IDF 6.0 completo en ~/.espressif (toolchain, cmake, ninja o su python)")
    entorno = dict(os.environ)
    # Desde Git Bash existe MSYSTEM: idf.py lo ve, dice "MSys/Mingw is no longer supported", NO compila
    # y aun así sale con código 0. Se quita para que corra igual que desde PowerShell.
    for variable in ("MSYSTEM", "MSYSTEM_PREFIX", "MSYSTEM_CHOST", "MSYSTEM_CARCH", "MINGW_PREFIX"):
        entorno.pop(variable, None)
    entorno["PATH"] = os.pathsep.join(carpetas + [str(Path(python_idf).parent), str(ruta_idf / "tools"),
                                                  entorno.get("PATH", "")])
    entorno.update(IDF_PATH=str(ruta_idf), IDF_PYTHON_ENV_PATH=str(Path(python_idf).parents[1]),
                   ESP_IDF_VERSION="6.0", ESP_ROM_ELF_DIR=primera("esp-rom-elfs/*") or "")
    return python_idf, entorno


def compilar(proyecto):
    python_idf, entorno = entorno_idf()
    print(f"Compilando {proyecto.name}…")
    resultado = subprocess.run([python_idf, str(ruta_idf / "tools" / "idf.py"), "build"], cwd=proyecto,
                               env=entorno, capture_output=True, text=True, encoding="utf-8", errors="replace")
    salida = resultado.stdout + resultado.stderr
    avisos = [linea for linea in salida.splitlines() if "warning:" in linea or "error:" in linea]
    for linea in avisos[:20]:
        print("  " + linea)
    # idf.py puede salir con 0 sin haber compilado (ver MSYSTEM arriba): se exige su mensaje final
    if resultado.returncode != 0 or "Project build complete" not in salida:
        print("\n".join(salida.splitlines()[-30:]))
        salir("la compilación falló")
    print(f"Compilado sin errores ({len(avisos)} avisos).")


# ---------------------------------------------------------------------------
#  API
# ---------------------------------------------------------------------------

def pedir(api, metodo, ruta, clave, cuerpo=None, tipo=None):
    cabeceras = {"X-OTA-Key": clave}
    if tipo:
        cabeceras["Content-Type"] = tipo
    peticion = urllib.request.Request(f"{api}{ruta}", data=cuerpo, method=metodo, headers=cabeceras)
    try:
        with urllib.request.urlopen(peticion, timeout=60) as respuesta:
            return json.loads(respuesta.read())
    except urllib.error.HTTPError as error:
        detalle = error.read().decode("utf-8", "replace")
        salir(f"{metodo} {ruta} respondió {error.code}: {detalle}")
    except urllib.error.URLError as error:
        salir(f"no se pudo conectar con la API en {api} ({error.reason}). ¿Está corriendo uvicorn?")


def subir(api, clave, binario, notas):
    limite = uuid.uuid4().hex
    partes = [f"--{limite}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"{binario.name}\"\r\n"
              f"Content-Type: application/octet-stream\r\n\r\n".encode(), binario.read_bytes(), b"\r\n"]
    if notas:
        partes += [f"--{limite}\r\nContent-Disposition: form-data; name=\"notes\"\r\n\r\n".encode(),
                   notas.encode("utf-8"), b"\r\n"]
    partes.append(f"--{limite}--\r\n".encode())
    return pedir(api, "POST", "/ota/upload", clave, b"".join(partes), f"multipart/form-data; boundary={limite}")


def seguir(api, clave, maquina, version, version_anterior):
    print(f"Esperando a {maquina}…")
    fin = time.monotonic() + tiempo_max_s
    ultimo = None
    visto_desconectado = False
    visto_instalado = False
    while time.monotonic() < fin:
        estado = pedir(api, "GET", f"/ota/status?machine={maquina}", clave)
        ota = estado.get("ota") or {}
        texto = (estado["connected"], ota.get("estado"), ota.get("porcentaje"))
        if texto != ultimo:
            ultimo = texto
            if not estado["connected"]:
                visto_desconectado = True
                print("  dron desconectado (reiniciando)…")
            else:
                pct = f" {ota['porcentaje']} %" if ota.get("porcentaje") is not None else ""
                print(f"  {ota.get('estado', '—')}{pct}" + (f": {ota['detalle']}" if ota.get("detalle") else ""))
        if ota.get("estado") == "error":
            salir(f"el dron rechazó la actualización: {ota.get('detalle')}")
        if estado["connected"] and ota.get("estado") == "confirmado" and estado.get("version") == version:
            print(f"Listo: {maquina} corre la versión {version} y está conectado a la API.")
            return
        if ota.get("estado") == "instalado":
            visto_instalado = True
        #Volvió con la versión vieja tras reiniciar
        if visto_desconectado and estado["connected"] and estado.get("version") == version_anterior \
                and version != version_anterior:
            if visto_instalado:
                salir(f"{maquina} volvió con {version_anterior}: la versión {version} no arrancó bien y se revirtió")
            salir(f"{maquina} se desconectó a media descarga y volvió con {version_anterior}: la OTA no se instaló "
                  "(¿reinicio por alimentación o Wi-Fi débil?). El firmware viejo sigue intacto")
        time.sleep(2)
    salir(f"pasaron {tiempo_max_s} s sin confirmación de {maquina}")


def main():
    argumentos = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    argumentos.add_argument("--proyecto", default=str(raiz_repo / "enbedded" / "Examples" / "Dron_telemetria_ws"))
    argumentos.add_argument("--maquina", default="dron_main_01")
    argumentos.add_argument("--api", default=os.environ.get("SAMATI_API_URL", "http://127.0.0.1:8001"))
    argumentos.add_argument("--notas", default=None, help="qué cambia en esta versión")
    argumentos.add_argument("--sin-compilar", action="store_true")
    argumentos.add_argument("--solo-subir", action="store_true")
    opciones = argumentos.parse_args()

    proyecto = Path(opciones.proyecto).resolve()
    if not ruta_clave.is_file():
        salir(f"falta {ruta_clave}: arranca la API una vez y se crea sola")
    clave = ruta_clave.read_text(encoding="utf-8").strip()
    api = opciones.api.rstrip("/")

    if not opciones.sin_compilar:
        compilar(proyecto)
    binario = proyecto / "build" / f"{proyecto.name}.bin"
    if not binario.is_file():
        salir(f"no existe {binario}")

    estado = None
    if not opciones.solo_subir:
        estado = pedir(api, "GET", f"/ota/status?machine={opciones.maquina}", clave)
        if not estado["connected"]:
            salir(f"{opciones.maquina} no está conectado a la API")
        if "ota" not in estado.get("acciones", []):
            salir(f"{opciones.maquina} corre {estado.get('version')}, sin OTA: esta vez hay que flashearlo por USB")

    subido = subir(api, clave, binario, opciones.notas)
    print(f"Subido #{subido['number']}: {subido['project_name']} v{subido['version']} "
          f"({subido['size'] / 1024:.0f} KB, sha256 {subido['sha256'][:12]})")
    if opciones.solo_subir:
        return
    if subido["version"] == estado.get("version"):
        print(f"Aviso: el dron ya corre la versión {subido['version']}. Sube PROJECT_VER en el CMakeLists "
              "para distinguirlas; se instala de todos modos.")
    pedir(api, "POST", "/ota/deploy", clave, json.dumps({"number": subido["number"], "machine": opciones.maquina}).encode(),
          "application/json")
    seguir(api, clave, opciones.maquina, subido["version"], estado.get("version"))


if __name__ == "__main__":
    main()
