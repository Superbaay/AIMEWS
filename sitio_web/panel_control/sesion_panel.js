// Sesión, peticiones y fechas compartidas por las páginas del panel. Depende de main.js del sitio (api_url y clave_token)
// La ruta se calcula desde este archivo para que funcione en cualquier subcarpeta del panel
const ruta_inicio_sesion = new URL('../index.html', document.currentScript.src).href;

function decodificar_token(token) {
  const carga_base64 = token.split('.')[1].replace(/-/g, '+').replace(/_/g, '/');
  const bytes = Uint8Array.from(atob(carga_base64), (caracter) => caracter.charCodeAt(0));
  return JSON.parse(new TextDecoder().decode(bytes));
}

function leer_sesion() {
  const token = localStorage.getItem(clave_token);
  if (!token) return null;
  try {
    const carga = decodificar_token(token);
    if (carga.exp && carga.exp * 1000 <= Date.now()) return null;
    return { token, nombre: carga.name, rol: carga.role, expira: carga.exp };
  } catch (error) {
    return null;
  }
}

function salir_del_panel() {
  localStorage.removeItem(clave_token);
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
      headers: { Authorization: `Bearer ${localStorage.getItem(clave_token)}`, ...opciones.headers },
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
