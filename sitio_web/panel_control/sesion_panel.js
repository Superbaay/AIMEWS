// Sesión, peticiones, fechas, preferencias y utilidades de formulario compartidas por las páginas del panel. Depende de main.js del sitio (api_url y clave_token)
// La ruta se calcula desde este archivo para que funcione en cualquier subcarpeta del panel
const ruta_inicio_sesion = new URL('../index.html', document.currentScript.src).href;

function decodificar_token(token) {
  const carga_base64 = token.split('.')[1].replace(/-/g, '+').replace(/_/g, '/');
  const bytes = Uint8Array.from(atob(carga_base64), (caracter) => caracter.charCodeAt(0));
  return JSON.parse(new TextDecoder().decode(bytes));
}

function leer_sesion() {
  // leer_token (main.js) busca en localStorage, sessionStorage y cookie
  const token = leer_token();
  if (!token) return null;
  try {
    const carga = decodificar_token(token);
    if (carga.exp && carga.exp * 1000 <= Date.now()) return null;
    // El panel es solo para personas: la API rechaza un token de máquina en las rutas de usuario (verify_user_Depends).
    // Un token sin type es anterior a ese campo y solo podía ser de usuario
    const tipo = carga.type ?? 'user';
    if (tipo !== 'user') return null;
    return { token, nombre: carga.name, rol: carga.role, tipo, expira: carga.exp };
  } catch (error) {
    return null;
  }
}

// ===== Barra lateral: un solo lugar para el menú de todas las páginas =====
// Cada página deja <aside class="barra_lateral"></aside> vacío y aquí se llena. Las rutas son relativas
// a panel_control/ y se resuelven contra este archivo, igual que ruta_inicio_sesion
const secciones_panel = [
  { ruta: 'index.html', texto: 'Home', icono: '<path d="M3.5 11 12 4l8.5 7"></path><path d="M6 9.5V20h12V9.5"></path><path d="M10 20v-5.5h4V20"></path>' },
  { ruta: 'Control_dron/index.html', texto: 'Dron', icono: '<circle cx="6" cy="6" r="2.5"></circle><circle cx="18" cy="6" r="2.5"></circle><circle cx="6" cy="18" r="2.5"></circle><circle cx="18" cy="18" r="2.5"></circle><path d="M8 8l2.2 2.2M16 8l-2.2 2.2M8 16l2.2-2.2M16 16l-2.2-2.2"></path><rect x="10" y="10" width="4" height="4" rx="1"></rect>' },
  { ruta: 'ingineer_log/index.html', texto: 'Logbook', icono: '<path d="M2 9h20"></path><path d="M3.5 9l.6 4.2a2.5 2.5 0 0 0 2.5 2.3h.8a2.5 2.5 0 0 0 2.5-2.3L10.2 9"></path><path d="M13.8 9l.3 4.2a2.5 2.5 0 0 0 2.5 2.3h.8a2.5 2.5 0 0 0 2.5-2.3L20.5 9"></path>' },
  { ruta: 'user_shetings/index.html', texto: 'Users', icono: '<circle cx="12" cy="8" r="3.5"></circle><path d="M5 20c0-3.6 3.1-6 7-6s7 2.4 7 6"></path>' },
  { ruta: 'machine_shetings/index.html', texto: 'Machines', icono: '<circle cx="12" cy="12" r="3"></circle><circle cx="12" cy="12" r="6.5"></circle><path d="M12 2.5v3M12 18.5v3M2.5 12h3M18.5 12h3M5.3 5.3l2.1 2.1M16.6 16.6l2.1 2.1M5.3 18.7l2.1-2.1M16.6 7.4l2.1-2.1"></path>' },
];

// "carpeta/" y "carpeta/index.html" son la misma página
function normalizar_ruta(ruta) {
  return ruta.replace(/index\.html$/, '');
}

function construir_barra_lateral() {
  const barra = document.querySelector('.barra_lateral');
  if (!barra || barra.children.length) return;
  const pagina_actual = normalizar_ruta(location.pathname);
  const enlaces = secciones_panel.map(({ ruta, texto, icono }) => {
    const destino = new URL(ruta, document.currentScript.src);
    const es_activa = normalizar_ruta(destino.pathname) === pagina_actual;
    const clases = es_activa ? 'enlace_lateral enlace_lateral_activo' : 'enlace_lateral';
    return `<a href="${destino.pathname}" class="${clases}"${es_activa ? ' aria-current="page"' : ''}>
          <svg class="icono_lateral" viewBox="0 0 24 24" aria-hidden="true">${icono}</svg>
          <span>${texto}</span>
        </a>`;
  }).join('');
  // Retrae la barra a solo íconos; la preferencia la guarda iniciar_barra_lateral para todas las páginas
  barra.innerHTML = `
      <button type="button" class="boton_retraer_lateral" id="boton_retraer_lateral" aria-expanded="true">
        <svg class="icono_lateral" viewBox="0 0 24 24" aria-hidden="true">
          <rect x="3.5" y="4.5" width="17" height="15" rx="2.5"></rect>
          <path d="M9.5 4.5v15"></path>
          <path class="flecha_retraer" d="M16 9.5 13.5 12l2.5 2.5"></path>
        </svg>
        <span id="texto_retraer_lateral">Retraer menú</span>
      </button>
      <nav class="menu_lateral" aria-label="Secciones del panel">${enlaces}</nav>
      <div class="barra_lateral_pie">
        <span class="etiqueta_pequena">Sesión válida hasta</span>
        <span class="numero_mono" id="sesion_expira">—</span>
      </div>`;
}
construir_barra_lateral();

// ===== Barra lateral retraíble =====
// Se ejecuta al cargar, con o sin sesión. La preferencia vale para todas las páginas del panel
const clave_lateral_retraida = 'samati_lateral_retraida';

function iniciar_barra_lateral() {
  const boton = document.getElementById('boton_retraer_lateral');
  if (!boton) return;
  const texto = document.getElementById('texto_retraer_lateral');

  function aplicar(retraida) {
    document.body.classList.toggle('lateral_retraida', retraida);
    boton.setAttribute('aria-expanded', String(!retraida));
    texto.textContent = retraida ? 'Expandir menú' : 'Retraer menú';
  }

  let retraida = false;
  try { retraida = localStorage.getItem(clave_lateral_retraida) === '1'; } catch (error) { /* sin almacenamiento */ }
  // El estado guardado se aplica sin animación; las transiciones se activan después del primer pintado
  document.body.classList.add('sin_transicion_lateral');
  aplicar(retraida);
  requestAnimationFrame(() => requestAnimationFrame(() => document.body.classList.remove('sin_transicion_lateral')));

  boton.addEventListener('click', () => {
    retraida = !retraida;
    aplicar(retraida);
    try { localStorage.setItem(clave_lateral_retraida, retraida ? '1' : '0'); } catch (error) { /* sin almacenamiento */ }
  });
}
iniciar_barra_lateral();

function salir_del_panel() {
  borrar_token();
  window.location.replace(ruta_inicio_sesion);
}

// Barra superior y pie de la barra lateral: usuario, rol, expiración y cierre de sesión
function mostrar_datos_sesion(sesion) {
  const nombre_usuario = sesion.nombre || 'Usuario';
  document.getElementById('usuario_nombre').textContent = nombre_usuario;
  document.getElementById('usuario_avatar').textContent = nombre_usuario.charAt(0);
  document.getElementById('usuario_rol').textContent = sesion.rol || 'Sin rol';
  if (sesion.expira) {
    const fecha_expira = new Date(sesion.expira * 1000);
    document.getElementById('sesion_expira').textContent = fecha_expira.toLocaleString('es-MX', { day: '2-digit', month: 'short', hour: '2-digit', minute: '2-digit', hour12: false });
    // Cierra la sesión justo cuando el token deja de ser válido
    setTimeout(salir_del_panel, sesion.expira * 1000 - Date.now());
  }
  document.getElementById('boton_cerrar_sesion_panel').addEventListener('click', salir_del_panel);
}

// ===== Peticiones a la API =====
// Lanza Error con un mensaje listo para mostrar; error.estado guarda el código HTTP.
// Un 401 significa token inválido o expirado: cierra la sesión.
async function pedir_api(ruta, opciones = {}) {
  let respuesta;
  try {
    respuesta = await fetch(`${api_url}${ruta}`, {
      ...opciones,
      headers: { Authorization: `Bearer ${leer_token()}`, ...opciones.headers },
    });
  } catch (error) {
    throw new Error('No se pudo conectar con el servidor.');
  }
  if (respuesta.status === 401) {
    salir_del_panel();
    throw new Error('sesion_invalida');
  }
  if (!respuesta.ok) {
    const error = new Error(`La API respondió con un error (${respuesta.status}).`);
    error.estado = respuesta.status;
    throw error;
  }
  return respuesta.json();
}

function enviar_json(ruta, metodo, cuerpo) {
  return pedir_api(ruta, {
    method: metodo,
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(cuerpo),
  });
}

// ===== Fechas =====
// MongoDB devuelve las fechas sin zona horaria aunque se guarden en UTC
function leer_fecha(valor) {
  if (!valor) return null;
  const tiene_zona = /Z|[+-]\d{2}:\d{2}$/.test(valor);
  const fecha = new Date(tiene_zona ? valor : `${valor}Z`);
  return Number.isNaN(fecha.getTime()) ? null : fecha;
}

function formatear_fecha(valor) {
  const fecha = leer_fecha(valor);
  return fecha
    ? fecha.toLocaleString('es-MX', { day: '2-digit', month: 'short', year: 'numeric', hour: '2-digit', minute: '2-digit', hour12: false })
    : '—';
}

function formatear_hora(fecha) {
  return fecha.toLocaleTimeString('es-MX', { hour12: false });
}

// ===== Valores =====
function es_numero(valor) {
  return typeof valor === 'number' && Number.isFinite(valor);
}

// ===== Preferencias =====
// localStorage puede fallar (modo privado o almacenamiento bloqueado): solo guarda preferencias, en JSON
function leer_preferencia(clave) {
  try { return JSON.parse(localStorage.getItem(clave)); } catch (error) { return null; }
}

function guardar_preferencia(clave, valor) {
  try { localStorage.setItem(clave, JSON.stringify(valor)); } catch (error) { /* sin almacenamiento */ }
}

// ===== Formularios =====
// Campo de texto opcional: vacío o solo espacios se envía como null
function texto_opcional(campo) {
  return campo.value.trim() || null;
}

// Nota bajo un formulario (.nota_resultado); texto vacío la oculta
function mostrar_resultado(nota, texto, es_error = false) {
  nota.textContent = texto;
  nota.classList.toggle('nota_resultado_error', es_error);
}

function limpiar_invalidos(formulario) {
  formulario.querySelectorAll('.campo_entrada_invalido').forEach((campo) => campo.classList.remove('campo_entrada_invalido'));
}

// Deshabilita el botón y muestra su estado de carga mientras dura la acción.
// al_terminar se ejecuta al final, también si la acción falla (por ejemplo, para volver a aplicar permisos)
async function con_boton_ocupado(boton, accion, al_terminar = null) {
  boton.disabled = true;
  boton.classList.add('esta_cargando');
  try {
    await accion();
  } finally {
    boton.disabled = false;
    boton.classList.remove('esta_cargando');
    if (al_terminar) al_terminar();
  }
}
