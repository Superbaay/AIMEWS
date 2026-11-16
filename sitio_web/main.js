// Por el túnel la página también abre con http://. Ese origen no coincide con el de la API (https), el navegador
// descarta sus respuestas por CORS y el login decía "No se pudo conectar". Se pasa a https antes de todo
if (location.protocol === 'http:' && location.hostname.endsWith('superbaay.dev')) {
  location.replace(`https://${location.host}${location.pathname}${location.search}${location.hash}`);
}

// Configuración de la API (FastAPI)
// Abierto en local (Live Server en localhost:5500) usa la API de esta PC directo, sin depender del túnel.
// Publicado, usa el dominio del túnel, que apunta a esa misma API (uvicorn :8001)
const es_sitio_local = ['localhost', '127.0.0.1'].includes(location.hostname);
const api_url = es_sitio_local ? 'http://127.0.0.1:8001' : 'https://api.superbaay.dev';
const clave_token = 'samati_token';
const duracion_sesion_s = 24 * 60 * 60; // el JWT de la API dura 24 h

// ===== Sesión guardada =====
// Algunos navegadores de teléfono bloquean localStorage (ajustes de privacidad o el navegador interno de otras apps).
// Se prueba localStorage, luego sessionStorage (dura mientras la pestaña siga abierta) y al final una cookie segura.
const almacenes_sesion = ['localStorage', 'sessionStorage'];

function leer_cookie(nombre) {
  try {
    const par = document.cookie.split('; ').find((cookie) => cookie.startsWith(`${nombre}=`));
    return par ? decodeURIComponent(par.slice(nombre.length + 1)) : null;
  } catch (error) {
    return null;
  }
}

function escribir_cookie(nombre, valor, segundos) {
  const seguro = location.protocol === 'https:' ? '; Secure' : '';
  document.cookie = `${nombre}=${encodeURIComponent(valor)}; Max-Age=${segundos}; Path=/; SameSite=Strict${seguro}`;
}

// Por qué falló cada opción la última vez (se muestra en el error del login para diagnosticar en el teléfono)
let fallos_almacen = [];

// Último recurso, si el navegador bloquea TODO (almacenamiento y cookies): el token vive en la memoria de la página
// y viaja a la siguiente en el fragmento del enlace (#sesion=...). El fragmento no se envía al servidor ni a Cloudflare,
// y se borra de la barra de direcciones en cuanto la página lo lee. Recargar la página obliga a iniciar sesión de nuevo
const prefijo_fragmento_sesion = '#sesion=';
let token_en_memoria = null;

if (location.hash.startsWith(prefijo_fragmento_sesion)) {
  token_en_memoria = decodeURIComponent(location.hash.slice(prefijo_fragmento_sesion.length));
  history.replaceState(null, '', location.pathname + location.search);
}

// Con la sesión en memoria, los enlaces internos llevan el token a la página siguiente
document.addEventListener('click', (evento) => {
  if (!token_en_memoria || evento.defaultPrevented) return;
  const enlace = evento.target.closest?.('a[href]');
  if (!enlace || enlace.target === '_blank' || enlace.hasAttribute('download')) return;
  const destino = new URL(enlace.href, location.href);
  if (destino.origin !== location.origin) return;
  // Un enlace a la misma página la recargaría y la sesión en memoria se perdería
  if (destino.pathname === location.pathname && destino.search === location.search) {
    evento.preventDefault();
    return;
  }
  destino.hash = prefijo_fragmento_sesion.slice(1) + encodeURIComponent(token_en_memoria);
  enlace.href = destino.href;
});

// Devuelve true si el token quedó guardado en alguna parte
function guardar_token(token) {
  fallos_almacen = [];
  for (const nombre of almacenes_sesion) {
    try {
      window[nombre].setItem(clave_token, token);
      return true;
    } catch (error) {
      fallos_almacen.push(`${nombre}: ${error.name}`);
    }
  }
  try {
    escribir_cookie(clave_token, token, duracion_sesion_s);
    if (leer_cookie(clave_token) === token) return true;
    fallos_almacen.push(`cookie: ${navigator.cookieEnabled ? 'no se guardó' : 'desactivadas'}`);
  } catch (error) {
    fallos_almacen.push(`cookie: ${error.name}`);
  }
  fallos_almacen.push(window.top !== window ? 'dentro de un iframe' : 'ventana principal');
  token_en_memoria = token;
  console.warn(`Sesión solo en memoria (${fallos_almacen.join(' · ')})`);
  return true;
}

function sesion_solo_en_memoria() {
  return token_en_memoria !== null && fallos_almacen.length > 0;
}

// En modo memoria, cualquier página con barra de sesión avisa que recargar cierra la sesión
document.addEventListener('DOMContentLoaded', () => {
  const pie = document.querySelector('.barra_lateral_pie');
  if (!token_en_memoria || !pie) return;
  const aviso = document.createElement('span');
  aviso.className = 'etiqueta_pequena';
  aviso.textContent = 'Sesión temporal: no recargues';
  pie.append(aviso);
});

function leer_token() {
  for (const nombre of almacenes_sesion) {
    try {
      const token = window[nombre].getItem(clave_token);
      if (token) return token;
    } catch (error) { /* bloqueado */ }
  }
  return leer_cookie(clave_token) || token_en_memoria;
}

function borrar_token() {
  for (const nombre of almacenes_sesion) {
    try { window[nombre].removeItem(clave_token); } catch (error) { /* bloqueado */ }
  }
  try { escribir_cookie(clave_token, '', 0); } catch (error) { /* sin cookies */ }
  token_en_memoria = null;
}

const formulario_login = document.getElementById('formulario_login');
const campo_usuario = document.getElementById('campo_usuario');
const campo_contrasena = document.getElementById('campo_contrasena');
const boton_mostrar_contrasena = document.getElementById('boton_mostrar_contrasena');
const boton_iniciar_sesion = document.getElementById('boton_iniciar_sesion');
const texto_boton_login = document.getElementById('texto_boton_login');
const mensaje_error = document.getElementById('mensaje_error');
const mensaje_error_texto = document.getElementById('mensaje_error_texto');
const vista_acceso_exitoso = document.getElementById('vista_acceso_exitoso');
const nombre_usuario_exito = document.getElementById('nombre_usuario_exito');
const boton_cerrar_sesion = document.getElementById('boton_cerrar_sesion');
const boton_acceso = document.getElementById('boton_acceso');

// ===== Animación de aparición al hacer scroll =====
const observador_revelar = new IntersectionObserver((entradas) => {
  entradas.forEach((entrada) => {
    if (entrada.isIntersecting) {
      entrada.target.classList.add('esta_visible');
      observador_revelar.unobserve(entrada.target);
    }
  });
}, { threshold: 0.12 });
document.querySelectorAll('.revelar').forEach((elemento) => observador_revelar.observe(elemento));

// El login solo existe en index.html; las páginas del panel también cargan este archivo
if (formulario_login) {
  // ===== Utilidades de login =====
  function mostrar_error(texto) {
    mensaje_error_texto.textContent = texto;
    mensaje_error.hidden = false;
    mensaje_error.style.animation = 'none';
    void mensaje_error.offsetWidth; // reinicia la animación
    mensaje_error.style.animation = '';
  }

  function ocultar_error() {
    mensaje_error.hidden = true;
    campo_usuario.classList.remove('campo_entrada_invalido');
    campo_contrasena.classList.remove('campo_entrada_invalido');
  }

  function establecer_cargando(esta_cargando) {
    boton_iniciar_sesion.disabled = esta_cargando;
    boton_iniciar_sesion.classList.toggle('esta_cargando', esta_cargando);
    texto_boton_login.textContent = esta_cargando ? 'Verificando…' : 'Iniciar sesión';
  }

  function mostrar_acceso_exitoso(nombre_usuario) {
    nombre_usuario_exito.textContent = nombre_usuario;
    formulario_login.hidden = true;
    vista_acceso_exitoso.hidden = false;
  }

  // ===== Eventos =====
  campo_usuario.addEventListener('input', ocultar_error);
  campo_contrasena.addEventListener('input', ocultar_error);

  boton_mostrar_contrasena.addEventListener('click', () => {
    const es_oculta = campo_contrasena.type === 'password';
    campo_contrasena.type = es_oculta ? 'text' : 'password';
    boton_mostrar_contrasena.textContent = es_oculta ? 'Ocultar' : 'Mostrar';
  });

  boton_acceso.addEventListener('click', () => {
    window.scrollTo({ top: 0, behavior: 'smooth' });
    setTimeout(() => campo_usuario.focus({ preventScroll: true }), 350);
  });

  boton_cerrar_sesion.addEventListener('click', () => {
    borrar_token();
    campo_contrasena.value = '';
    vista_acceso_exitoso.hidden = true;
    formulario_login.hidden = false;
  });

  formulario_login.addEventListener('submit', async (evento) => {
    evento.preventDefault();
    const nombre_usuario = campo_usuario.value.trim();
    const contrasena = campo_contrasena.value;

    if (!nombre_usuario || !contrasena) {
      if (!nombre_usuario) campo_usuario.classList.add('campo_entrada_invalido');
      if (!contrasena) campo_contrasena.classList.add('campo_entrada_invalido');
      mostrar_error('Ingresa tu usuario y contraseña.');
      return;
    }

    ocultar_error();
    establecer_cargando(true);

    // Cada paso tiene su propio mensaje: antes todo caía en "No se pudo conectar" y escondía la causa real
    try {
      let respuesta;
      try {
        // POST /token espera OAuth2PasswordRequestForm (form-urlencoded)
        respuesta = await fetch(`${api_url}/token`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: new URLSearchParams({ username: nombre_usuario, password: contrasena }),
        });
      } catch (error) {
        mostrar_error('No se pudo conectar con el servidor.');
        return;
      }

      if (!respuesta.ok) {
        const es_credencial_invalida = respuesta.status === 401 || respuesta.status === 404;
        mostrar_error(es_credencial_invalida
          ? 'Usuario o contraseña incorrectos.'
          : `El servidor respondió con un error (${respuesta.status}).`);
        return;
      }

      let token;
      try {
        token = await respuesta.json();
      } catch (error) {
        mostrar_error(`La respuesta del servidor llegó incompleta (${error.name}). Intenta de nuevo.`);
        return;
      }

      // guardar_token prueba localStorage, sessionStorage y cookie; si el navegador bloquea todo, queda en memoria
      guardar_token(token);
      mostrar_acceso_exitoso(nombre_usuario);
      if (sesion_solo_en_memoria()) {
        vista_acceso_exitoso.querySelector('.texto_secundario').textContent =
          'Tu navegador bloquea los datos de este sitio: la sesión es temporal y se cierra si recargas la página. Para que se guarde, permite cookies y datos de sitios para superbaay.dev.';
      }
    } catch (error) {
      console.error(error);
      mostrar_error(`No se pudo completar el inicio de sesión: ${error.message}`);
    } finally {
      establecer_cargando(false);
    }
  });
}
