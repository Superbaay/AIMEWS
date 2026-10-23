---
name: samati-frontend
description: Crea y amplía páginas del frontend de SAMATI (Sistema Autónomo de Monitoreo y Alerta Temprana de Infraestructura) con el mismo diseño azul marino, tipografías, componentes, animaciones y tecnologías del sitio original (HTML + CSS + JavaScript vanilla, nomenclatura snake_case, API FastAPI con JWT). Úsala cuando el usuario pida nuevas páginas, secciones, formularios o componentes para SAMATI, o cuando pida "mantener el mismo diseño" del sitio SAMATI.
---

# SAMATI Frontend

Esta skill sirve para construir páginas nuevas del sitio SAMATI que se vean y funcionen igual que la página principal (`sitio_web/index.html`).

## 1. Tecnologías (obligatorio)

- **HTML5 semántico**: `header`, `nav`, `main`, `section`, `article`, `footer`, `form`, `label`.
- **CSS3 puro** en archivos externos. Sin frameworks (sin Bootstrap ni Tailwind) y sin preprocesadores.
- **JavaScript vanilla**, solo cuando haga falta (login, animación al hacer scroll, consumo de la API). Sin jQuery, React ni otras librerías.
- **Fuentes de Google Fonts**: Sora (títulos), IBM Plex Sans (texto) e IBM Plex Mono (etiquetas, números y datos técnicos).
- **Backend**: API en FastAPI en `http://127.0.0.1:8000`. CORS permite `http://127.0.0.1:5500` (Live Server), así que hay que probar siempre con Live Server en el puerto 5500.

## 2. Nomenclatura snake_case (obligatorio)

Todo identificador va en **snake_case** :

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
AIMEWS/
├── API/                          ← backend FastAPI
│   ├── main.py                   ← app, CORS y rutas (/token, /user)
│   ├── Library/
│   │   ├── __init__.py
│   │   ├── base_model_bm.py      ← modelos Pydantic (user, order)
│   │   ├── def_bm.py             ← búsqueda de usuarios en MongoDB
│   │   └── segurity_bm.py        ← bcrypt, JWT y verificación de rol
│   └── Routers_BM/
│       └── rt_bm.py              ← reservado para routers (vacío)
├── context/                      ← documentación del proyecto (PDF, solo local)
└── sitio_web/                    ← frontend
    ├── index.html                ← página principal pública (incluye el login)
    ├── styless.css               ← variables, base y componentes compartidos
    ├── main.js                   ← observador de scroll, login y lógica compartida
    ├── nombre_pagina.html        ← páginas públicas nuevas, en la raíz
    ├── estilos_nombre_pagina.css (solo lo exclusivo de esa página)
    ├── script_nombre_pagina.js   (solo lo exclusivo de esa página)
    └── panel_control/            ← sitio protegido: solo con sesión iniciada
        ├── index.html            ← entrada del panel (destino tras el login)
        ├── nombre_pagina.html
        ├── estilos_nombre_pagina.css
        └── script_nombre_pagina.js
```

**Dónde va cada página**
- **Pública** (cualquiera puede verla): en la raíz de `sitio_web/`. Carga `styless.css` y `main.js`.
- **Protegida** (requiere token): dentro de `panel_control/`. Carga `../styless.css` y `../main.js`, con `../` porque están un nivel más arriba.

No crees subcarpetas `css/` ni `js/`: el CSS y el JS de cada página van junto a su HTML. Cada página carga primero el CSS compartido y después el suyo, y `main.js` antes que su script propio. Nunca dupliques variables ni componentes que ya existen.

## 4. Sistema visual

Las variables completas están en `sitio_web/styless.css` (`:root`). Usa siempre las variables y no escribas colores sueltos.

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

La única fuente de verdad es `sitio_web/index.html`. Antes de crear un componente, busca su clase raíz ahí y copia el HTML tal cual. Así no hay una segunda copia que se quede desactualizada.

| Componente | Clase raíz | Dónde está en `index.html` | Úsalo para |
|---|---|---|---|
| Barra de navegación | `barra_navegacion` | `<header>` | Todas las páginas. En el panel, cambia los enlaces de ancla por las secciones del panel |
| Encabezado de sección | `encabezado_seccion` | `#sistema` | Abrir cualquier sección: `etiqueta_mono` + `titulo_seccion` + texto opcional |
| Cuadrícula de datos | `cuadricula_datos` → `dato` | `#inicio` | Cifras cortas con unidad (telemetría, KPIs) |
| Tarjeta de componente | `tarjeta_componente` | `#sistema` | Bloques con número, etiqueta, texto y lista |
| Línea de pasos | `linea_pasos` → `paso` | `#funcionamiento` | Procesos secuenciales (`<ol>`) |
| Tarjeta de dependencia | `tarjeta_dependencia` | `#dependencias` | Tarjetas simples de título y texto |
| Tarjeta de login | `tarjeta_login` | `#inicio` | Solo en `index.html`; no la dupliques en el panel |
| Mensaje de error | `mensaje_error` | Dentro de `tarjeta_login` | Cualquier error de formulario o de la API (`role="alert"`) |
| Botones | `boton_primario`, `boton_acceso`, `boton_texto` | Login y navegación | Acción principal, acceso, acción secundaria |
| Pie de página | `pie_pagina` | `<footer>` | Todas las páginas |

**Reglas**
- **Reutiliza antes de crear.** Si un componente existente sirve con otro contenido, úsalo sin cambiar sus clases.
- **Variantes con modificador.** Si necesitas una variación, agrega una clase con sufijo (`tarjeta_componente_alerta`) en el CSS de la página. No edites la clase base.
- **Promueve lo que se repite.** Si un componente nuevo aparece en dos o más páginas, muévelo a `styless.css` y agrégalo a esta tabla.
- **Componentes nuevos:** clase raíz en snake_case y elementos hijos con el prefijo de la raíz (`tabla_puntos`, `tabla_puntos_fila`, `tabla_puntos_celda`).

## 6. Animaciones y transiciones

- **Aparición al hacer scroll**: agrega la clase `revelar` y escalonas con `style="--retraso:120ms"` (pasos de 60 o 120ms). Hace falta el observador de `sitio_web/main.js`.
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
- Tras un login exitoso, el botón "Ir al panel de control" lleva a `panel_control/index.html`.
- **Páginas de `panel_control/`:** al cargar, comprueban si existe `samati_token` en `localStorage`. Si no existe, redirigen a `../index.html`. Si una petición protegida responde 401, borran el token y redirigen igual.
- La comprobación en el navegador solo sirve para la experiencia de usuario. La seguridad real la da la API, que valida el JWT en cada ruta protegida.

## 8. Fuentes de información (`context/`)

La carpeta `AIMEWS/context/` tiene la documentación del proyecto. Consúltala antes de escribir textos, cifras o secciones nuevas.

| Archivo | Qué contiene | Úsalo para |
|---|---|---|
| `Documento de referencia tecnica SAMATI (18).pdf` | Problemática, objetivo, características físicas, arquitectura de software, cálculos de autonomía y alcance, funcionamiento, dependencias de gobierno y esquemas de PCB | **Fuente principal** de textos y cifras del sitio |
| `Cómo programar un dron multirrotor autónomo mediano (2–4 kg) en C C++.pdf` | Guía de firmware: PID, control en cascada, filtro de Kalman, normativa (NOM-107-SCT3-2019) y hoja de ruta | Solo contexto técnico, para páginas que expliquen el control de vuelo |

**Reglas**
- **Prioridad:** el documento de referencia manda sobre `index.html`. Si se contradicen, usa el documento y avisa al usuario.
- **Configuración vigente:** motor de 380KV, con 55 min de autonomía, 13 km de radio de misión, crucero a 35 km/h y 3.6 kg de peso. Las cifras del motor de 320KV (67 min, 29.8 km/h) son una alternativa: úsalas solo si se pide una comparación.
- **Sensores:** el documento lista dos generaciones (MPU6050, HMC5883 y NEO-M6 en las características; ICM-20948, ICM-42688-P y ZED-F9P en las PCB). En el sitio usa nombres genéricos ("acelerómetro y giroscopio", "magnetómetro", "GPS"), igual que en `index.html`. Solo ESP32-S3 y LiDAR TF Mini Plus van con modelo, porque son los mismos en todo el documento.
- **Estimaciones:** las cifras que la guía de firmware marca como "estimación" son rangos típicos y no datos de SAMATI. No las publiques como datos del proyecto.
- **Cómo leer los PDF:** `Read` no puede abrirlos. Desde Bash usa `pdftotext -layout -enc UTF-8 "context/<archivo>.pdf" -`. Sin `-enc UTF-8`, los acentos salen como `�`.
- **Solo local:** `context/` está en `.gitignore`. No enlaces los PDF desde el sitio y no copies su texto tal cual, porque tiene erratas: reescríbelo con el tono de la sección 9.

## 9. Redacción

- Tono técnico, claro y directo.
- Usa las cifras de `context/` según la sección 8 (configuración vigente: 55 min, 13 km, 35 km/h, 3.6 kg) y los nombres reales de la tecnología (ESP32-S3, LiDAR TF Mini Plus, FastAPI, MongoDB, JWT, WebSockets). No inventes datos.
- No rellenes con texto de ejemplo ni con secciones decorativas sin contenido.

## 10. Lista de verificación antes de entregar

1. Todos los nombres van en snake_case.
2. La página carga `styless.css`, `main.js` y las tres fuentes (con `../` si está en `panel_control/`).
3. Si la página es protegida, está en `panel_control/` y redirige a `../index.html` cuando no hay token.
4. No hay colores sueltos: todo usa variables.
5. El diseño es responsivo hasta 360px de ancho.
6. Los elementos que entran en pantalla tienen `revelar` y un retraso escalonado.
7. Los campos tienen `label` con `for` y `autocomplete`.
8. El contraste del texto es de al menos 4.5:1.
9. No se usan frameworks, librerías ni emojis.
10. Los componentes reutilizados mantienen las clases de `index.html` y no se duplican estilos de `styless.css`.
11. Las cifras y los nombres de hardware coinciden con el documento de referencia de `context/`.
12. Funciona abriéndolo con Live Server (puerto 5500).
