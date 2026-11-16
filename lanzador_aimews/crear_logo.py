"""Genera el logo minimalista del lanzador (logo.ico para el acceso directo y logo.png para la ventana).

Diseño: cuadrado redondeado azul marino de SAMATI con un pico (la "A" de AIMEWS / la
infraestructura vigilada) y un punto ámbar encima: la alerta temprana.
Se dibuja a 4x y se reduce para que los bordes salgan suaves.
"""
from pathlib import Path

from PIL import Image, ImageDraw

carpeta = Path(__file__).resolve().parent

color_fondo = (10, 20, 40, 255)       # --color_fondo del sitio
color_acento = (111, 180, 255, 255)   # --color_acento
color_alerta = (242, 196, 107, 255)   # --color_alerta

escala = 4
lado = 256 * escala


def dibujar_logo() -> Image.Image:
    imagen = Image.new("RGBA", (lado, lado), (0, 0, 0, 0))
    lienzo = ImageDraw.Draw(imagen)
    lienzo.rounded_rectangle((0, 0, lado - 1, lado - 1), radius=56 * escala, fill=color_fondo)

    # Pico: dos trazos que se unen arriba, como una "A" sin barra
    grosor = 22 * escala
    base_y = 196 * escala
    cima = (128 * escala, 92 * escala)
    lienzo.line([(58 * escala, base_y), cima, (198 * escala, base_y)], fill=color_acento,
                width=grosor, joint="curve")
    # Remates redondos en los extremos del trazo
    for x in (58, 198):
        centro = (x * escala, base_y)
        r = grosor // 2
        lienzo.ellipse((centro[0] - r, centro[1] - r, centro[0] + r, centro[1] + r), fill=color_acento)

    # Punto de alerta sobre la cima
    r = 15 * escala
    cx, cy = 128 * escala, 50 * escala
    lienzo.ellipse((cx - r, cy - r, cx + r, cy + r), fill=color_alerta)
    return imagen.resize((256, 256), Image.LANCZOS)


if __name__ == "__main__":
    logo = dibujar_logo()
    logo.save(carpeta / "logo.png")
    logo.save(carpeta / "logo.ico", sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    print("logo.png y logo.ico generados en", carpeta)
