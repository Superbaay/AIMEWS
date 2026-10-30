// Gestión de usuarios. Depende de ../../main.js (api_url, clave_token y el observador de .revelar)
// y de ../sesion_panel.js (sesión, pedir_api, enviar_json y fechas).
// La jerarquía replica la de la API (segurity_bm.py): O5-1 > 4 > 3 > 2 > 1. La API es quien la hace cumplir;
// aquí solo sirve para no mostrar opciones que la API rechazaría.
const rol_administrador = 'O5-1';
const rol_por_defecto = '1'; // un usuario nuevo empieza con el menor privilegio
const roles_disponibles = [
  ['O5-1', 'O5-1 · Administración'],
  ['4', 'Nivel 4'],
  ['3', 'Nivel 3'],
  ['2', 'Nivel 2'],
  ['1', 'Nivel 1'],
];
const rango_por_rol = { 1: 1, 2: 2, 3: 3, 4: 4, 'O5-1': 5 };
const rango_maximo = rango_por_rol[rol_administrador];

// Un rol desconocido (por ejemplo "admin") vale 0
function rango_de(rol) {
  return rango_por_rol[rol] ?? 0;
}

function puede_ver(rol_propio, rol_objetivo) {
  return rango_de(rol_objetivo) <= rango_de(rol_propio);
}

function puede_gestionar(rol_propio, rol_objetivo) {
  const rango_propio = rango_de(rol_propio);
  return rango_propio === rango_maximo || rango_de(rol_objetivo) < rango_propio;
}

function puede_asignar(rol_propio, rol) {
  if (!(rol in rango_por_rol)) return false;
  const rango_propio = rango_de(rol_propio);
  return rango_propio === rango_maximo || rango_por_rol[rol] < rango_propio;
}
const patron_nombre_usuario = /^[A-Za-z0-9_.-]{3,32}$/;
const largo_minimo_contrasena = 8;

const sesion = leer_sesion();
if (!sesion) {
  // salir_del_panel();
} else {
  iniciar_usuarios();
}

function iniciar_usuarios() {
  const aviso_permisos = document.getElementById('aviso_permisos');
  const gestion_usuarios = document.getElementById('gestion_usuarios');
  const total_usuarios = document.getElementById('total_usuarios');
  // Alta
  const formulario_nuevo_usuario = document.getElementById('formulario_nuevo_usuario');
  const campo_usuario_nuevo = document.getElementById('campo_usuario_nuevo');
  const campo_contrasena_nuevo = document.getElementById('campo_contrasena_nuevo');
  const campo_rol_nuevo = document.getElementById('campo_rol_nuevo');
  const campo_empresa_nuevo = document.getElementById('campo_empresa_nuevo');
  const campo_telefono_nuevo = document.getElementById('campo_telefono_nuevo');
  const campo_correo_nuevo = document.getElementById('campo_correo_nuevo');
  const mensaje_error_usuario = document.getElementById('mensaje_error_usuario');
  const mensaje_error_usuario_texto = document.getElementById('mensaje_error_usuario_texto');
  const boton_crear_usuario = document.getElementById('boton_crear_usuario');
  // Edición
  const vista_editar_usuario = document.getElementById('vista_editar_usuario');
  const ficha_avatar = document.getElementById('ficha_avatar');
  const ficha_nombre = document.getElementById('ficha_nombre');
  const ficha_meta = document.getElementById('ficha_meta');
  const nota_cuenta_propia = document.getElementById('nota_cuenta_propia');
  const nota_solo_lectura = document.getElementById('nota_solo_lectura');
  const formulario_perfil = document.getElementById('formulario_perfil');
  const campo_empresa_editar = document.getElementById('campo_empresa_editar');
  const campo_telefono_editar = document.getElementById('campo_telefono_editar');
  const campo_correo_editar = document.getElementById('campo_correo_editar');
  const boton_guardar_perfil = document.getElementById('boton_guardar_perfil');
  const nota_perfil = document.getElementById('nota_perfil');
  const formulario_rol = document.getElementById('formulario_rol');
  const campo_rol_editar = document.getElementById('campo_rol_editar');
  const boton_cambiar_rol = document.getElementById('boton_cambiar_rol');
  const nota_rol = document.getElementById('nota_rol');
  const formulario_contrasena = document.getElementById('formulario_contrasena');
  const campo_contrasena_editar = document.getElementById('campo_contrasena_editar');
  const boton_cambiar_contrasena = document.getElementById('boton_cambiar_contrasena');
  const nota_contrasena = document.getElementById('nota_contrasena');
  const bloque_eliminar_usuario = document.getElementById('bloque_eliminar_usuario');
  const boton_eliminar_usuario = document.getElementById('boton_eliminar_usuario');
  const nota_eliminar = document.getElementById('nota_eliminar');
  const boton_volver = document.getElementById('boton_volver');
  // Lista
  const campo_busqueda = document.getElementById('campo_busqueda');
  const estado_lista = document.getElementById('estado_lista');
  const lista_usuarios = document.getElementById('lista_usuarios');
  const plantilla_usuario = document.getElementById('plantilla_usuario');

  let usuarios = [];
  let usuario_seleccionado = null;

  // Solo los roles por debajo del propio (O5-1 asigna cualquiera)
  const roles_asignables = roles_disponibles.filter(([valor]) => puede_asignar(sesion.rol, valor));

  mostrar_datos_sesion(sesion);

  // Sin roles que asignar no hay nada que administrar (y la API respondería 401 a los roles 1)
  if (roles_asignables.length === 0) {
    aviso_permisos.hidden = false;
    gestion_usuarios.hidden = true;
    return;
  }
  llenar_selector_rol(campo_rol_nuevo, roles_asignables[roles_asignables.length - 1][0]);

  // ===== Utilidades =====
  function nombre_rol(rol) {
    const encontrado = roles_disponibles.find(([valor]) => valor === rol);
    return encontrado ? encontrado[1] : rol || 'Sin rol';
  }

  // El rol actual que no se puede asignar (desconocido o del mismo nivel) se muestra deshabilitado
  function llenar_selector_rol(selector, rol_actual) {
    selector.replaceChildren();
    if (rol_actual && !roles_asignables.some(([valor]) => valor === rol_actual)) {
      const conocido = roles_disponibles.some(([valor]) => valor === rol_actual);
      const opcion = new Option(conocido ? nombre_rol(rol_actual) : `${rol_actual} · no reconocido`, rol_actual);
      opcion.disabled = true;
      selector.append(opcion);
    }
    roles_asignables.forEach(([valor, texto]) => selector.append(new Option(texto, valor)));
    selector.value = rol_actual || rol_por_defecto;
  }

  function texto_opcional(campo) {
    return campo.value.trim() || null;
  }

  // phone_number es int en el BaseModel user: se quitan espacios, guiones y paréntesis
  function leer_telefono(campo) {
    const digitos = campo.value.replace(/[\s()-]/g, '');
    if (!digitos) return null;
    if (!/^\d{7,15}$/.test(digitos)) throw new Error('El teléfono debe tener entre 7 y 15 dígitos.');
    return Number(digitos);
  }

  function leer_correo(campo) {
    const correo = campo.value.trim();
    if (!correo) return null;
    if (campo.validity.typeMismatch) throw new Error('El correo no tiene un formato válido.');
    return correo;
  }

  function mostrar_resultado(nota, texto, es_error = false) {
    nota.textContent = texto;
    nota.classList.toggle('nota_resultado_error', es_error);
  }

  function mostrar_error_alta(texto) {
    mensaje_error_usuario_texto.textContent = texto;
    mensaje_error_usuario.hidden = false;
  }

  function es_cuenta_propia(usuario) {
    return usuario && usuario.name === sesion.nombre;
  }

  function es_gestionable(usuario) {
    return es_cuenta_propia(usuario) || puede_gestionar(sesion.rol, usuario.role);
  }

  // Cuenta propia: perfil y contraseña, sin rol ni eliminar. Mismo nivel: solo lectura
  function aplicar_restricciones() {
    const usuario = usuario_seleccionado;
    const propia = es_cuenta_propia(usuario);
    const gestionable = Boolean(usuario) && es_gestionable(usuario);
    nota_cuenta_propia.hidden = !propia;
    nota_solo_lectura.hidden = !usuario || gestionable;
    [campo_empresa_editar, campo_telefono_editar, campo_correo_editar].forEach((campo) => { campo.disabled = !gestionable; });
    boton_guardar_perfil.hidden = !gestionable;
    formulario_contrasena.hidden = !gestionable;
    formulario_rol.hidden = !gestionable || propia;
    bloque_eliminar_usuario.hidden = !gestionable || propia;
  }

  async function con_boton_ocupado(boton, accion) {
    boton.disabled = true;
    boton.classList.add('esta_cargando');
    try {
      await accion();
    } finally {
      boton.disabled = false;
      boton.classList.remove('esta_cargando');
      aplicar_restricciones();
    }
  }

  // ===== Lista =====
  async function cargar_usuarios(nombre_a_mostrar = null) {
    estado_lista.hidden = false;
    estado_lista.textContent = 'Cargando usuarios…';
    try {
      // La API ya filtra por nivel; el filtro local es solo una segunda barrera
      usuarios = (await pedir_api('/user/get_user_all')).filter((usuario) => puede_ver(sesion.rol, usuario.role));
      // Orden de creación: el más antiguo primero
      usuarios.sort((a, b) => (leer_fecha(a.date)?.getTime() ?? 0) - (leer_fecha(b.date)?.getTime() ?? 0));
      total_usuarios.textContent = usuarios.length;
      pintar_lista();
      const usuario = usuarios.find((candidato) => candidato.name === nombre_a_mostrar);
      if (usuario) mostrar_usuario(usuario);
    } catch (error) {
      if (error.message === 'sesion_invalida') return;
      estado_lista.textContent = error.message;
    }
  }

  function pintar_lista() {
    const filtro = campo_busqueda.value.trim().toLowerCase();
    const visibles = usuarios.filter((usuario) =>
      [usuario.name, usuario.email, usuario.company].some((valor) => valor && valor.toLowerCase().includes(filtro)));

    lista_usuarios.replaceChildren();
    estado_lista.hidden = visibles.length > 0;
    estado_lista.textContent = usuarios.length === 0 ? 'Aún no hay usuarios.' : 'Ningún usuario coincide con la búsqueda.';

    visibles.forEach((usuario) => {
      const fila = plantilla_usuario.content.firstElementChild.cloneNode(true);
      fila.dataset.nombre = usuario.name;
      fila.classList.toggle('tarjeta_usuario_activa', usuario.name === usuario_seleccionado?.name);
      fila.querySelector('.tarjeta_usuario_avatar').textContent = usuario.name.charAt(0);
      fila.querySelector('.tarjeta_usuario_texto_nombre').textContent = usuario.name;
      fila.querySelector('.etiqueta_tu').hidden = !es_cuenta_propia(usuario);
      fila.querySelector('.tarjeta_usuario_contacto').textContent =
        [usuario.email, usuario.company].filter(Boolean).join(' · ') || 'Sin datos de contacto';

      const insignia = fila.querySelector('.insignia_rol');
      insignia.textContent = usuario.role || 'Sin rol';
      insignia.classList.toggle('insignia_rol_admin', usuario.role === rol_administrador);
      insignia.classList.toggle('insignia_rol_desconocido', !roles_disponibles.some(([valor]) => valor === usuario.role));

      const boton = fila.querySelector('.tarjeta_usuario_boton');
      const accion = es_gestionable(usuario) ? 'Editar' : 'Ver';
      boton.textContent = accion;
      boton.setAttribute('aria-label', `${accion} a ${usuario.name}`);
      boton.addEventListener('click', () => mostrar_usuario(usuario));
      lista_usuarios.append(fila);
    });
  }

  function marcar_seleccionado(nombre) {
    lista_usuarios.querySelectorAll('.tarjeta_usuario').forEach((fila) => {
      fila.classList.toggle('tarjeta_usuario_activa', fila.dataset.nombre === nombre);
    });
  }

  // ===== Vistas del editor =====
  function mostrar_usuario(usuario) {
    usuario_seleccionado = usuario;
    marcar_seleccionado(usuario.name);
    ficha_avatar.textContent = usuario.name.charAt(0);
    ficha_nombre.textContent = usuario.name;
    ficha_meta.textContent = `${nombre_rol(usuario.role)} · Alta ${formatear_fecha(usuario.date)}`;
    campo_empresa_editar.value = usuario.company ?? '';
    campo_telefono_editar.value = usuario.phone_number ?? '';
    campo_correo_editar.value = usuario.email ?? '';
    campo_contrasena_editar.value = '';
    llenar_selector_rol(campo_rol_editar, usuario.role);
    [nota_perfil, nota_rol, nota_contrasena, nota_eliminar].forEach((nota) => mostrar_resultado(nota, ''));
    aplicar_restricciones();
    formulario_nuevo_usuario.hidden = true;
    vista_editar_usuario.hidden = false;
  }

  function mostrar_alta() {
    usuario_seleccionado = null;
    marcar_seleccionado(null);
    vista_editar_usuario.hidden = true;
    formulario_nuevo_usuario.hidden = false;
  }

  // ===== Eventos =====
  document.querySelectorAll('[data-mostrar]').forEach((boton) => {
    boton.addEventListener('click', () => {
      const campo = document.getElementById(boton.dataset.mostrar);
      const es_oculta = campo.type === 'password';
      campo.type = es_oculta ? 'text' : 'password';
      boton.textContent = es_oculta ? 'Ocultar' : 'Mostrar';
    });
  });

  campo_busqueda.addEventListener('input', pintar_lista);
  boton_volver.addEventListener('click', mostrar_alta);
  formulario_nuevo_usuario.addEventListener('input', () => {
    mensaje_error_usuario.hidden = true;
    formulario_nuevo_usuario.querySelectorAll('.campo_entrada_invalido').forEach((campo) => campo.classList.remove('campo_entrada_invalido'));
  });
  formulario_perfil.addEventListener('input', () => mostrar_resultado(nota_perfil, ''));
  formulario_rol.addEventListener('input', () => mostrar_resultado(nota_rol, ''));
  formulario_contrasena.addEventListener('input', () => mostrar_resultado(nota_contrasena, ''));

  // POST /user espera el BaseModel user completo (name, password y date son obligatorios)
  formulario_nuevo_usuario.addEventListener('submit', async (evento) => {
    evento.preventDefault();
    const nombre = campo_usuario_nuevo.value.trim();
    const contrasena = campo_contrasena_nuevo.value;
    let cuerpo;
    try {
      if (!patron_nombre_usuario.test(nombre)) {
        campo_usuario_nuevo.classList.add('campo_entrada_invalido');
        throw new Error('El usuario debe tener de 3 a 32 caracteres: letras, números, punto, guion o guion bajo.');
      }
      if (usuarios.some((usuario) => usuario.name === nombre)) {
        campo_usuario_nuevo.classList.add('campo_entrada_invalido');
        throw new Error('Ese usuario ya existe.');
      }
      if (contrasena.length < largo_minimo_contrasena) {
        campo_contrasena_nuevo.classList.add('campo_entrada_invalido');
        throw new Error(`La contraseña debe tener al menos ${largo_minimo_contrasena} caracteres.`);
      }
      cuerpo = {
        name: nombre,
        password: contrasena,
        role: campo_rol_nuevo.value,
        company: texto_opcional(campo_empresa_nuevo),
        email: leer_correo(campo_correo_nuevo),
        phone_number: leer_telefono(campo_telefono_nuevo),
        date: new Date().toISOString(),
      };
    } catch (error) {
      mostrar_error_alta(error.message);
      return;
    }

    await con_boton_ocupado(boton_crear_usuario, async () => {
      try {
        await enviar_json('/user', 'POST', cuerpo);
        formulario_nuevo_usuario.reset();
        llenar_selector_rol(campo_rol_nuevo, roles_asignables[roles_asignables.length - 1][0]);
        await cargar_usuarios(nombre);
      } catch (error) {
        if (error.message === 'sesion_invalida') return;
        // 409 también ocurre si el nombre pertenece a alguien de nivel superior: el mensaje no lo distingue
        mostrar_error_alta(error.estado === 409 ? 'Ese nombre de usuario no está disponible.' : error.message);
      }
    });
  });

  // PATCH /user/update_user recibe user_not_password (exige date); la API solo cambia empresa, correo y teléfono
  formulario_perfil.addEventListener('submit', async (evento) => {
    evento.preventDefault();
    const usuario = usuario_seleccionado;
    let cuerpo;
    try {
      cuerpo = {
        name: usuario.name,
        date: usuario.date,
        role: usuario.role,
        company: texto_opcional(campo_empresa_editar),
        email: leer_correo(campo_correo_editar),
        phone_number: leer_telefono(campo_telefono_editar),
      };
    } catch (error) {
      mostrar_resultado(nota_perfil, error.message, true);
      return;
    }

    await con_boton_ocupado(boton_guardar_perfil, async () => {
      try {
        await enviar_json('/user/update_user', 'PATCH', cuerpo);
        await cargar_usuarios(usuario.name);
        mostrar_resultado(nota_perfil, 'Perfil actualizado.');
      } catch (error) {
        if (error.message === 'sesion_invalida') return;
        mostrar_resultado(nota_perfil, error.message, true);
      }
    });
  });

  formulario_rol.addEventListener('submit', async (evento) => {
    evento.preventDefault();
    const usuario = usuario_seleccionado;
    const rol_nuevo = campo_rol_editar.value;
    if (es_cuenta_propia(usuario)) return;
    if (rol_nuevo === usuario.role) {
      mostrar_resultado(nota_rol, 'Ya tiene ese rol.');
      return;
    }
    if (rol_nuevo === rol_administrador
      && !window.confirm(`${usuario.name} tendrá acceso total, incluida la administración de usuarios. ¿Continuar?`)) return;

    await con_boton_ocupado(boton_cambiar_rol, async () => {
      try {
        await enviar_json('/user/update_user_role', 'PATCH', { name: usuario.name, role: rol_nuevo });
        await cargar_usuarios(usuario.name);
        mostrar_resultado(nota_rol, `Rol cambiado a ${nombre_rol(rol_nuevo)}.`);
      } catch (error) {
        if (error.message === 'sesion_invalida') return;
        mostrar_resultado(nota_rol, error.message, true);
      }
    });
  });

  // PATCH /user/update_user_password recibe el BaseModel name_password
  formulario_contrasena.addEventListener('submit', async (evento) => {
    evento.preventDefault();
    const usuario = usuario_seleccionado;
    const contrasena = campo_contrasena_editar.value;
    if (contrasena.length < largo_minimo_contrasena) {
      mostrar_resultado(nota_contrasena, `La contraseña debe tener al menos ${largo_minimo_contrasena} caracteres.`, true);
      return;
    }

    await con_boton_ocupado(boton_cambiar_contrasena, async () => {
      try {
        await enviar_json('/user/update_user_password', 'PATCH', { name: usuario.name, password: contrasena });
        campo_contrasena_editar.value = '';
        mostrar_resultado(nota_contrasena, 'Contraseña actualizada.');
      } catch (error) {
        if (error.message === 'sesion_invalida') return;
        mostrar_resultado(nota_contrasena, error.message, true);
      }
    });
  });

  boton_eliminar_usuario.addEventListener('click', async () => {
    const usuario = usuario_seleccionado;
    if (es_cuenta_propia(usuario)) return;
    if (!window.confirm(`¿Eliminar a ${usuario.name}? Perderá el acceso de inmediato.`)) return;

    await con_boton_ocupado(boton_eliminar_usuario, async () => {
      try {
        await enviar_json('/user/delete_user', 'DELETE', { name: usuario.name });
        mostrar_alta();
        await cargar_usuarios();
      } catch (error) {
        if (error.message === 'sesion_invalida') return;
        mostrar_resultado(nota_eliminar, error.message, true);
      }
    });
  });

  // ===== Arranque =====
  cargar_usuarios();
}
