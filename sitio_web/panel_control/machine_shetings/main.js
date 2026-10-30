// Gestión de máquinas. Depende de ../../main.js (api_url, clave_token y el observador de .revelar)
// y de ../sesion_panel.js (sesión, pedir_api, enviar_json y fechas).
// Permisos de la API (machine_rt.py): ver y editar desde nivel 4; alta y baja solo O5-1.
const rol_administrador = 'O5-1';
const roles_con_acceso = ['4', rol_administrador];
const patron_nombre_maquina = /^[A-Za-z0-9_.-]{3,32}$/;
const largo_minimo_contrasena = 8;

const sesion = leer_sesion();
if (!sesion) {
  // salir_del_panel();
} else {
  iniciar_maquinas();
}

function iniciar_maquinas() {
  const aviso_permisos = document.getElementById('aviso_permisos');
  const gestion_maquinas = document.getElementById('gestion_maquinas');
  const total_maquinas = document.getElementById('total_maquinas');
  // Alta
  const formulario_nueva_maquina = document.getElementById('formulario_nueva_maquina');
  const campo_nombre_nueva = document.getElementById('campo_nombre_nueva');
  const campo_modelo_nueva = document.getElementById('campo_modelo_nueva');
  const campo_contrasena_nueva = document.getElementById('campo_contrasena_nueva');
  const campo_descripcion_nueva = document.getElementById('campo_descripcion_nueva');
  const mensaje_error_maquina = document.getElementById('mensaje_error_maquina');
  const mensaje_error_maquina_texto = document.getElementById('mensaje_error_maquina_texto');
  const boton_crear_maquina = document.getElementById('boton_crear_maquina');
  const vista_sin_seleccion = document.getElementById('vista_sin_seleccion');
  // Edición
  const vista_editar_maquina = document.getElementById('vista_editar_maquina');
  const ficha_numero = document.getElementById('ficha_numero');
  const ficha_nombre = document.getElementById('ficha_nombre');
  const ficha_meta = document.getElementById('ficha_meta');
  const dato_alta = document.getElementById('dato_alta');
  const dato_actualizada = document.getElementById('dato_actualizada');
  const dato_ultimo_uso = document.getElementById('dato_ultimo_uso');
  const dato_ultimo_usuario = document.getElementById('dato_ultimo_usuario');
  const formulario_maquina = document.getElementById('formulario_maquina');
  const campo_nombre_editar = document.getElementById('campo_nombre_editar');
  const campo_modelo_editar = document.getElementById('campo_modelo_editar');
  const campo_descripcion_editar = document.getElementById('campo_descripcion_editar');
  const boton_guardar_maquina = document.getElementById('boton_guardar_maquina');
  const nota_maquina = document.getElementById('nota_maquina');
  const bloque_eliminar_maquina = document.getElementById('bloque_eliminar_maquina');
  const boton_eliminar_maquina = document.getElementById('boton_eliminar_maquina');
  const nota_eliminar = document.getElementById('nota_eliminar');
  const boton_volver = document.getElementById('boton_volver');
  // Lista
  const campo_busqueda = document.getElementById('campo_busqueda');
  const estado_lista = document.getElementById('estado_lista');
  const lista_maquinas = document.getElementById('lista_maquinas');
  const plantilla_maquina = document.getElementById('plantilla_maquina');

  let maquinas = [];
  let maquina_seleccionada = null;
  const es_administrador = sesion.rol === rol_administrador;

  mostrar_datos_sesion(sesion);

  // Sin nivel 4 la API respondería 401 y pedir_api cerraría la sesión: mejor no llamarla
  if (!roles_con_acceso.includes(sesion.rol)) {
    aviso_permisos.hidden = false;
    gestion_maquinas.hidden = true;
    return;
  }
  bloque_eliminar_maquina.hidden = !es_administrador;

  // ===== Utilidades =====
  // El número se renumera al borrar, así que solo se usa para mostrar y para la petición del momento
  function formatear_numero(numero) {
    return `#${String(numero).padStart(2, '0')}`;
  }

  function texto_opcional(campo) {
    return campo.value.trim() || null;
  }

  function validar_nombre(campo, numero_propio = null) {
    const nombre = campo.value.trim();
    if (!patron_nombre_maquina.test(nombre)) {
      campo.classList.add('campo_entrada_invalido');
      throw new Error('El nombre debe tener de 3 a 32 caracteres: letras, números, punto, guion o guion bajo.');
    }
    if (maquinas.some((maquina) => maquina.name === nombre && maquina.number !== numero_propio)) {
      campo.classList.add('campo_entrada_invalido');
      throw new Error('Ya existe una máquina con ese nombre.');
    }
    return nombre;
  }

  function validar_modelo(campo) {
    const modelo = campo.value.trim();
    if (!modelo) {
      campo.classList.add('campo_entrada_invalido');
      throw new Error('Indica el modelo de la placa (por ejemplo ESP32-S3).');
    }
    return modelo;
  }

  function mostrar_resultado(nota, texto, es_error = false) {
    nota.textContent = texto;
    nota.classList.toggle('nota_resultado_error', es_error);
  }

  function mostrar_error_alta(texto) {
    mensaje_error_maquina_texto.textContent = texto;
    mensaje_error_maquina.hidden = false;
  }

  function limpiar_invalidos(formulario) {
    formulario.querySelectorAll('.campo_entrada_invalido').forEach((campo) => campo.classList.remove('campo_entrada_invalido'));
  }

  async function con_boton_ocupado(boton, accion) {
    boton.disabled = true;
    boton.classList.add('esta_cargando');
    try {
      await accion();
    } finally {
      boton.disabled = false;
      boton.classList.remove('esta_cargando');
    }
  }

  // ===== Lista =====
  async function cargar_maquinas(nombre_a_mostrar = null) {
    estado_lista.hidden = false;
    estado_lista.textContent = 'Cargando máquinas…';
    try {
      maquinas = await pedir_api('/machine_rt/search_all_machine_rt_not');
      maquinas.sort((a, b) => a.number - b.number);
      total_maquinas.textContent = maquinas.length;
      pintar_lista();
      const maquina = maquinas.find((candidata) => candidata.name === nombre_a_mostrar);
      if (maquina) mostrar_maquina(maquina);
    } catch (error) {
      if (error.message === 'sesion_invalida') return;
      estado_lista.textContent = error.message;
    }
  }

  function pintar_lista() {
    const filtro = campo_busqueda.value.trim().toLowerCase();
    const visibles = maquinas.filter((maquina) =>
      [maquina.name, maquina.model].some((valor) => valor && valor.toLowerCase().includes(filtro)));

    lista_maquinas.replaceChildren();
    estado_lista.hidden = visibles.length > 0;
    estado_lista.textContent = maquinas.length === 0 ? 'Aún no hay máquinas registradas.' : 'Ninguna máquina coincide con la búsqueda.';

    visibles.forEach((maquina) => {
      const fila = plantilla_maquina.content.firstElementChild.cloneNode(true);
      fila.dataset.nombre = maquina.name;
      fila.classList.toggle('tarjeta_maquina_activa', maquina.name === maquina_seleccionada?.name);
      fila.querySelector('.tarjeta_maquina_numero').textContent = formatear_numero(maquina.number);
      fila.querySelector('.tarjeta_maquina_nombre').textContent = maquina.name;
      fila.querySelector('.tarjeta_maquina_detalle').textContent = maquina.last_use
        ? `Último uso ${formatear_fecha(maquina.last_use)}`
        : 'Sin uso registrado';
      fila.querySelector('.insignia_modelo').textContent = maquina.model;

      const boton = fila.querySelector('.tarjeta_maquina_boton');
      boton.setAttribute('aria-label', `Editar ${maquina.name}`);
      boton.addEventListener('click', () => mostrar_maquina(maquina));
      lista_maquinas.append(fila);
    });
  }

  function marcar_seleccionada(nombre) {
    lista_maquinas.querySelectorAll('.tarjeta_maquina').forEach((fila) => {
      fila.classList.toggle('tarjeta_maquina_activa', fila.dataset.nombre === nombre);
    });
  }

  // ===== Vistas del editor =====
  function mostrar_maquina(maquina) {
    maquina_seleccionada = maquina;
    marcar_seleccionada(maquina.name);
    ficha_numero.textContent = formatear_numero(maquina.number);
    ficha_nombre.textContent = maquina.name;
    ficha_meta.textContent = maquina.model;
    dato_alta.textContent = formatear_fecha(maquina.date_creation);
    dato_actualizada.textContent = formatear_fecha(maquina.date_updated);
    dato_ultimo_uso.textContent = formatear_fecha(maquina.last_use);
    dato_ultimo_usuario.textContent = maquina.last_user || '—';
    campo_nombre_editar.value = maquina.name;
    campo_modelo_editar.value = maquina.model;
    campo_descripcion_editar.value = maquina.description ?? '';
    limpiar_invalidos(formulario_maquina);
    [nota_maquina, nota_eliminar].forEach((nota) => mostrar_resultado(nota, ''));
    formulario_nueva_maquina.hidden = true;
    vista_sin_seleccion.hidden = true;
    vista_editar_maquina.hidden = false;
  }

  // O5-1 vuelve al formulario de alta; nivel 4 no puede crear y ve el aviso de selección
  function mostrar_inicio() {
    maquina_seleccionada = null;
    marcar_seleccionada(null);
    vista_editar_maquina.hidden = true;
    formulario_nueva_maquina.hidden = !es_administrador;
    vista_sin_seleccion.hidden = es_administrador;
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
  boton_volver.addEventListener('click', mostrar_inicio);
  formulario_nueva_maquina.addEventListener('input', () => {
    mensaje_error_maquina.hidden = true;
    limpiar_invalidos(formulario_nueva_maquina);
  });
  formulario_maquina.addEventListener('input', () => {
    mostrar_resultado(nota_maquina, '');
    limpiar_invalidos(formulario_maquina);
  });

  // POST /machine_rt/new_machine_rt recibe machine_new; número, fechas y rol los asigna la API
  formulario_nueva_maquina.addEventListener('submit', async (evento) => {
    evento.preventDefault();
    let cuerpo;
    try {
      const nombre = validar_nombre(campo_nombre_nueva);
      const modelo = validar_modelo(campo_modelo_nueva);
      if (campo_contrasena_nueva.value.length < largo_minimo_contrasena) {
        campo_contrasena_nueva.classList.add('campo_entrada_invalido');
        throw new Error(`La contraseña debe tener al menos ${largo_minimo_contrasena} caracteres.`);
      }
      cuerpo = {
        name: nombre,
        model: modelo,
        password: campo_contrasena_nueva.value,
        description: texto_opcional(campo_descripcion_nueva),
      };
    } catch (error) {
      mostrar_error_alta(error.message);
      return;
    }

    await con_boton_ocupado(boton_crear_maquina, async () => {
      try {
        await enviar_json('/machine_rt/new_machine_rt', 'POST', cuerpo);
        formulario_nueva_maquina.reset();
        await cargar_maquinas(cuerpo.name);
      } catch (error) {
        if (error.message === 'sesion_invalida') return;
        mostrar_error_alta(error.estado === 409 ? 'Ya existe una máquina con ese nombre.' : error.message);
      }
    });
  });

  // PATCH /machine_rt/update_machine_rt recibe machine_update; la API solo cambia nombre, modelo y descripción
  formulario_maquina.addEventListener('submit', async (evento) => {
    evento.preventDefault();
    const maquina = maquina_seleccionada;
    let cuerpo;
    try {
      cuerpo = {
        number: maquina.number,
        name: validar_nombre(campo_nombre_editar, maquina.number),
        model: validar_modelo(campo_modelo_editar),
        description: texto_opcional(campo_descripcion_editar),
      };
    } catch (error) {
      mostrar_resultado(nota_maquina, error.message, true);
      return;
    }

    await con_boton_ocupado(boton_guardar_maquina, async () => {
      try {
        await enviar_json('/machine_rt/update_machine_rt', 'PATCH', cuerpo);
        await cargar_maquinas(cuerpo.name);
        mostrar_resultado(nota_maquina, 'Cambios guardados.');
      } catch (error) {
        if (error.message === 'sesion_invalida') return;
        mostrar_resultado(nota_maquina, error.estado === 409 ? 'Ya existe una máquina con ese nombre.' : error.message, true);
      }
    });
  });

  boton_eliminar_maquina.addEventListener('click', async () => {
    const maquina = maquina_seleccionada;
    if (!es_administrador) return;
    if (!window.confirm(`¿Eliminar ${maquina.name}? Las máquinas con número mayor se renumerarán.`)) return;

    await con_boton_ocupado(boton_eliminar_maquina, async () => {
      try {
        await enviar_json('/machine_rt/delete_machine_rt', 'DELETE', { number: maquina.number });
        mostrar_inicio();
        await cargar_maquinas();
      } catch (error) {
        if (error.message === 'sesion_invalida') return;
        mostrar_resultado(nota_eliminar, error.message, true);
      }
    });
  });

  // ===== Arranque =====
  mostrar_inicio();
  cargar_maquinas();
}
