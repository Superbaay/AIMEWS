"""Revisa si la base ya tiene valores repetidos donde la API pone índices únicos.

Uso (desde la carpeta API):   python -m Library.revisar_duplicados

Solo lee; no borra nada. Si encuentra duplicados, corrígelos a mano (por ejemplo desde
MongoDB Compass) antes de reiniciar la API, o el índice único de ese campo no se crea.
"""
from Library.db_bm import db, indices


def duplicados(nombre: str, campo: str) -> list[dict]:
    return list(db[nombre].aggregate([
        {"$group": {"_id": f"${campo}", "veces": {"$sum": 1}}},
        {"$match": {"veces": {"$gt": 1}}},
    ]))


if __name__ == "__main__":
    encontrados = False
    for nombre, campos, unico in indices:
        if not unico or len(campos) != 1:
            continue
        campo = campos[0][0]
        for grupo in duplicados(nombre, campo):
            encontrados = True
            print(f"{nombre}.{campo} = {grupo['_id']!r} aparece {grupo['veces']} veces")
    print("Hay duplicados: corrígelos antes de reiniciar la API" if encontrados else "Sin duplicados")
