// Configuración de la API (FastAPI)
const api_url = 'http://127.0.0.1:8000';
const clave_token = 'samati_token';

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
  localStorage.removeItem(clave_token);
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

  try {
    // POST /token espera OAuth2PasswordRequestForm (form-urlencoded)
    const respuesta = await fetch(`${api_url}/token`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: new URLSearchParams({ username: nombre_usuario, password: contrasena }),
    });

    if (!respuesta.ok) {
      const es_credencial_invalida = respuesta.status === 401 || respuesta.status === 404;
      mostrar_error(es_credencial_invalida
        ? 'Usuario o contraseña incorrectos.'
        : `El servidor respondió con un error (${respuesta.status}).`);
      return;
    }

    const token = await respuesta.json();
    localStorage.setItem(clave_token, token);
    mostrar_acceso_exitoso(nombre_usuario);
  } catch (error) {
    mostrar_error('No se pudo conectar con el servidor.');
  } finally {
    establecer_cargando(false);
  }
});
