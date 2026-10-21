---
name: samati-frontend
description: Crea y amplía páginas del frontend de SAMATI (Sistema Autónomo de Monitoreo y Alerta Temprana de Infraestructura) con el mismo diseño azul marino, tipografías, componentes, animaciones y tecnologías del sitio original (HTML + CSS + JavaScript vanilla, nomenclatura snake_case, API FastAPI con JWT). Úsala cuando el usuario pida nuevas páginas, secciones, formularios o componentes para SAMATI, o cuando pida "mantener el mismo diseño" del sitio SAMATI.
---

# SAMATI Frontend

Esta skill sirve para construir páginas nuevas del sitio SAMATI que se vean y funcionen igual que la página principal (`assets/ejemplo_index.html`).

## 1. Tecnologías (obligatorio)

- **HTML5 semántico**: `header`, `nav`, `main`, `section`, `article`, `footer`, `form`, `label`.
- **CSS3 puro** en archivos externos. Sin frameworks (sin Bootstrap ni Tailwind) y sin preprocesadores.
- **JavaScript vanilla**, solo cuando haga falta (login, animación al hacer scroll, consumo de la API). Sin jQuery, React ni otras librerías.
- **Fuentes de Google Fonts**: Sora (títulos), IBM Plex Sans (texto) e IBM Plex Mono (etiquetas, números y datos técnicos).
- **Backend**: API en FastAPI en `http://127.0.0.1:8000`. CORS permite `http://127.0.0.1:5500` (Live Server), así que hay que probar siempre con Live Server en el puerto 5500.

## 2. Nomenclatura snake_case (obligatorio)

Todo identificador va en **snake_case** y en **español**:

- Archivos: `panel_control.html`, `estilos_panel.css`, `script_panel.js`
- Clases CSS: `tarjeta_login`, `boton_primario`, `campo_entrada`
- IDs: `formulario_login`, `campo_usuario`
- Variables CSS: `--color_fondo`, `--fuente_titulos`
- Variables y funciones JS: `const api_url`, `function mostrar_error()`
- Keyframes: `animacion_giro`, `animacion_entrada`
- Estados: prefijo `esta_` (`esta_visible`, `esta_cargando`)
- Modificadores: sufijo descriptivo (`seccion_alterna`, `campo_entrada_invalido`)

Nunca uses camelCase, kebab-case ni BEM (`__`, `--`) en clases. La única excepción son los atributos estándar de HTML (`autocomplete`, `aria-*`) y los nombres de campo que exige la API (`username`, `password`).

## 3. Estructura de carpetas

```
sitio_web/
├── index.html
├── nombre_pagina.html
├── css/
│   ├── estilos_principal.css   ← variables, base y componentes compartidos
│   └── estilos_nombre_pagina.css (solo lo exclusivo de esa página)
├── js/
│   ├── script_principal.js
│   └── script_nombre_pagina.js
└── img/
```

Cada página nueva carga primero `css/estilos_principal.css` y después su CSS propio. Nunca dupliques variables ni componentes que ya existen.

## 4. Sistema visual

Las variables completas están en `assets/css/estilos_principal.css` (`:root`). Usa siempre las variables y no escribas colores sueltos.

**Paleta azul marino**
- `--color_fondo` #0a1428: fondo general
- `--color_fondo_alterno` #0c1830: secciones alternas
- `--color_superficie` #0d1a33 / `--color_superficie_hover` #102042: tarjetas
- `--color_texto` #eef2fa · `--color_texto_secundario` #a9b8d4 · `--color_texto_tenue` #8a9bbd
- `--color_acento` #6fb4ff · `--color_acento_claro` #8cc4ff · `--color_acento_hover` #9bcbff
- `--color_alerta` #f2c46b (solo alertas de infraestructura) · `--color_error` #ff8a78
- `--color_linea` rgba(140,175,235,.14): bordes y divisores
- Texto sobre un botón de acento: #071226

**Tipografía**
- Título principal: Sora 600, `clamp(38px, 5.4vw, 68px)`, interlineado 1.04, `letter-spacing: -.025em`
- Título de sección: Sora 600, `clamp(30px, 3.6vw, 44px)`
- Título de tarjeta: Sora 600, 17 a 22px
- Texto: IBM Plex Sans 15 a 18px, interlineado 1.6
- Etiquetas: IBM Plex Mono 11 a 12px, MAYÚSCULAS, `letter-spacing: .14em`, color de acento

**Forma y espacio**
- Radios: 10px en campos y botones, 14 a 18px en tarjetas, 20px en la tarjeta de login, 999px en botones píldora
- Contenedor: `max-width: 1240px`. Secciones con `padding: 120px 28px` (88px 22px en móvil)
- Layout con `flex` o `grid` y `gap`. Las cuadrículas usan `repeat(auto-fit, minmax(min(100%, Xpx), 1fr))`
- Efecto vidrio solo en la navegación y la tarjeta de login: `backdrop-filter: blur(14–16px)`
- Marca: rombo con borde (`.logo_marca`, un cuadrado rotado 45°). No uses íconos de librerías ni emojis.

## 5. Componentes disponibles

Consulta `references/componentes.md` para ver el HTML listo para copiar de cada uno:
barra de navegación, encabezado de sección, cuadrícula de datos, tarjeta de componente con lista, línea de pasos, tarjeta de dependencia, tarjeta de login (formulario, error y éxito), botones y pie de página.

## 6. Animaciones y transiciones

- **Aparición al hacer scroll**: agrega la clase `revelar` y escalonas con `style="--retraso:120ms"` (pasos de 60 o 120ms). Hace falta el observador de `assets/js/script_principal.js`.
- **Hover**: transiciones de .2 a .3s en color, borde, fondo y `translate` (−1 a −3px). No uses escalados grandes.
- **Curva**: `--curva_suave` cubic-bezier(.2,.7,.2,1).
- **Keyframes existentes**: `animacion_giro`, `animacion_pulso`, `animacion_entrada`, `animacion_sacudida`. Reutilízalos antes de crear nuevos.
- Respeta siempre `prefers-reduced-motion` (el bloque ya está en el CSS base).
- Las animaciones solo apoyan lo visual: nada parpadea ni se mueve sin un propósito.

## 7. Autenticación con la API

- El login pide **solo usuario y contraseña**.
- Petición: `POST {api_url}/token` con `Content-Type: application/x-www-form-urlencoded` y cuerpo `username` y `password` (OAuth2PasswordRequestForm).
- La respuesta es el JWT. Se guarda en `localStorage` con la clave `samati_token` y dura 24 h.
- Las rutas protegidas se llaman con la cabecera `Authorization: Bearer <token>`.
- Mensajes de error: 401/404 → "Usuario o contraseña incorrectos."; sin conexión → "No se pudo conectar con el servidor."
- Estado de carga: añade `esta_cargando` al botón, cambia el texto a "Verificando…" y desactiva el botón.

## 8. Redacción

- Todo en español, con tono técnico, claro y directo.
- Usa los datos reales del proyecto (autonomía de 55 min, radio de 13 km, crucero a 35 km/h, 3.6 kg, ESP32-S3, LiDAR TF Mini Plus, FastAPI, MongoDB, JWT, WebSockets) y no inventes cifras.
- No rellenes con texto de ejemplo ni con secciones decorativas sin contenido.

## 9. Lista de verificación antes de entregar

1. Todos los nombres van en snake_case y en español.
2. La página carga `estilos_principal.css` y las tres fuentes.
3. No hay colores sueltos: todo usa variables.
4. El diseño es responsivo hasta 360px de ancho.
5. Los elementos que entran en pantalla tienen `revelar` y un retraso escalonado.
6. Los campos tienen `label` con `for` y `autocomplete`.
7. El contraste del texto es de al menos 4.5:1.
8. No se usan frameworks, librerías ni emojis.
9. Funciona abriéndolo con Live Server (puerto 5500).
