// Misiones y alertas. Depende de ../../main.js (api_url, clave_token, observador de .revelar)
// y de ../sesion_panel.js (sesión, pedir_api, enviar_json, formatear_fecha).
// Permisos de la API (Routers_BM/mission_rt.py): ver desde nivel 3; atender alertas desde nivel 4.
// Las misiones no se crean aquí: nacen cuando Control_dron manda iniciar_mision y el dron la recibe.
const rango_roles = { '1': 1, '2': 2, '3': 3, '4': 4, 'O5-1': 5 };
const rango_para_ver = 3;
const rango_para_atender = 4;
const intervalo_actualizacion_ms = 10000;   // la meta del Anexo es alerta en < 10 s

const sesion = leer_sesion();
if (sesion) iniciar_misiones();

function iniciar_misiones() {
  const aviso_permisos = document.getElementById('aviso_permisos');
  const contenido_misiones = document.getElementById('contenido_misiones');
  const total_pendientes = document.getElementById('total_pendientes');
  const filtro_alertas = document.getElementById('filtro_alertas');
  const estado_alertas = document.getElementById('estado_alertas');
  const lista_alertas = document.getElementById('lista_alertas');
  const estado_misiones = document.getElementById('estado_misiones');
  const lista_misiones = document.getElementById('lista_misiones');
  const detalle_mision = document.getElementById('detalle_mision');
  const detalle_titulo = document.getElementById('detalle_titulo');
  const detalle_meta = document.getElementById('detalle_meta');
  const detalle_cabecera = document.getElementById('detalle_cabecera');
  const detalle_cuerpo = document.getElementById('detalle_cuerpo');
  const detalle_nota = document.getElementById('detalle_nota');
  const dialogo_foto = document.getElementById('dialogo_foto');
  const imagen_alerta = document.getElementById('imagen_alerta');

  mostrar_datos_sesion(sesion);
  const rango = rango_roles[sesion.rol] ?? 0;
  if (rango < rango_para_ver) {
    aviso_permisos.hidden = false;
    contenido_misiones.hidden = true;
    return;
  }
  const puede_atender = rango >= rango_para_atender;

  // ===== Utilidades =====
  // Todo el texto que viene de la API va con textContent, nunca con innerHTML
  function crear(etiqueta, clase, texto) {
    const elemento = document.createElement(etiqueta);
    if (clase) elemento.className = clase;
    if (texto !== undefined && texto !== null) elemento.textContent = texto;
    return elemento;
  }

  function insignia(estado) {
    return crear('span', `insignia_estado insignia_estado_${estado}`, estado.replace('_', ' '));
  }

  function metros(valor, decimales = 2) {
    return es_numero(valor) ? `${valor.toFixed(decimales)} m` : '—';
  }

  function centimetros(valor) {
    return es_numero(valor) ? `${Math.round(valor * 100)} cm` : '—';
  }

  // ===== Alertas =====
  async function cargar_alertas() {
    try {
      const filtro = filtro_alertas.value ? `?estado=${filtro_alertas.value}` : '';
      const [alertas, pendientes] = await Promise.all([
        pedir_api(`/alerts${filtro}`),
        filtro_alertas.value === 'pendiente' ? null : pedir_api('/alerts?estado=pendiente'),
      ]);
      total_pendientes.textContent = String((pendientes ?? alertas).length);
      pintar_alertas(alertas);
    } catch (error) {
      if (error.message !== 'sesion_invalida') estado_alertas.textContent = error.message;
    }
  }

  function pintar_alertas(alertas) {
    lista_alertas.replaceChildren();
    estado_alertas.hidden = alertas.length > 0;
    estado_alertas.textContent = 'Sin alertas con este filtro.';
    for (const alerta of alertas) {
      const tarjeta = crear('li', 'tarjeta_mision');
      if (alerta.estado === 'pendiente') tarjeta.classList.add('tarjeta_alerta_pendiente');

      const fila = crear('div', 'tarjeta_mision_fila');
      fila.append(crear('span', 'profundidad_alerta', centimetros(alerta.profundidad_m)), insignia(alerta.estado));
      tarjeta.append(fila);
      tarjeta.append(crear('span', 'tarjeta_mision_detalle',
        `Alerta #${alerta.number} · ${alerta.machine} · misión #${alerta.mission_number} · waypoint ${Number(alerta.indice) + 1}`));
      tarjeta.append(crear('span', 'tarjeta_mision_detalle',
        `(${alerta.latitud}, ${alerta.longitud}) · umbral ${centimetros(alerta.umbral_m)} · ${formatear_fecha(alerta.date)}`));
      if (alerta.atendida_por) {
        tarjeta.append(crear('span', 'tarjeta_mision_detalle', `${alerta.estado} por ${alerta.atendida_por}, ${formatear_fecha(alerta.fecha_atencion)}`));
      }

      const acciones = crear('div', 'tarjeta_mision_acciones');
      if (alerta.foto) {
        const boton_foto = crear('button', 'boton_acceso', 'Ver foto');
        boton_foto.type = 'button';
        boton_foto.addEventListener('click', () => ver_foto(alerta.number));
        acciones.append(boton_foto);
      }
      if (puede_atender && alerta.estado === 'pendiente') {
        for (const [estado, texto] of [['atendida', 'Atendida'], ['descartada', 'Descartar']]) {
          const boton = crear('button', 'boton_acceso', texto);
          boton.type = 'button';
          boton.addEventListener('click', () => cambiar_alerta(alerta.number, estado, boton));
          acciones.append(boton);
        }
      }
      if (acciones.childElementCount) tarjeta.append(acciones);
      lista_alertas.append(tarjeta);
    }
  }

  async function cambiar_alerta(numero, estado, boton) {
    boton.disabled = true;
    try {
      await enviar_json(`/alerts/${numero}`, 'PATCH', { estado });
      await cargar_alertas();
    } catch (error) {
      boton.disabled = false;
      if (error.message !== 'sesion_invalida') {
        estado_alertas.hidden = false;
        estado_alertas.textContent = error.message;
      }
    }
  }

  // La foto pide token: se baja con fetch y se muestra como blob (un <img src> no manda Authorization)
  async function ver_foto(numero) {
    try {
      const respuesta = await fetch(`${api_url}/alerts/${numero}/foto`, { headers: { Authorization: `Bearer ${leer_token()}` } });
      if (!respuesta.ok) throw new Error(`No se pudo cargar la foto (${respuesta.status}).`);
      if (imagen_alerta.src) URL.revokeObjectURL(imagen_alerta.src);
      imagen_alerta.src = URL.createObjectURL(await respuesta.blob());
      dialogo_foto.showModal();
    } catch (error) {
      estado_alertas.hidden = false;
      estado_alertas.textContent = error.message;
    }
  }

  // ===== Misiones =====
  async function cargar_misiones() {
    try {
      pintar_misiones(await pedir_api('/missions?limit=50'));
    } catch (error) {
      if (error.message !== 'sesion_invalida') estado_misiones.textContent = error.message;
    }
  }

  function pintar_misiones(misiones) {
    lista_misiones.replaceChildren();
    estado_misiones.hidden = misiones.length > 0;
    estado_misiones.textContent = 'Todavía no hay misiones. Se crean al mandar iniciar_mision desde Control del dron.';
    for (const mision of misiones) {
      const tarjeta = crear('li', 'tarjeta_mision');
      const fila = crear('div', 'tarjeta_mision_fila');
      fila.append(crear('span', 'tarjeta_mision_titulo', `#${mision.number} · ${mision.tipo ?? 'sin tipo'} · ${mision.machine}`), insignia(mision.estado));
      tarjeta.append(fila);
      const progreso = es_numero(mision.waypoint_actual) ? `waypoint ${mision.waypoint_actual + 1} de ${mision.total_waypoints}` : `${mision.total_waypoints} waypoints`;
      tarjeta.append(crear('span', 'tarjeta_mision_detalle', `${formatear_fecha(mision.inicio)} · ${progreso} · ${mision.n_alertas} alerta(s)`));
      if (mision.motivo) tarjeta.append(crear('span', 'tarjeta_mision_detalle', `Motivo: ${mision.motivo}`));
      const acciones = crear('div', 'tarjeta_mision_acciones');
      const boton = crear('button', 'boton_acceso', 'Ver lecturas');
      boton.type = 'button';
      boton.addEventListener('click', () => ver_mision(mision));
      acciones.append(boton);
      tarjeta.append(acciones);
      lista_misiones.append(tarjeta);
    }
  }

  function pintar_tabla(columnas, filas) {
    const encabezado = crear('tr');
    for (const columna of columnas) encabezado.append(crear('th', null, columna));
    detalle_cabecera.replaceChildren(encabezado);
    detalle_cuerpo.replaceChildren(...filas.map(({ celdas, alerta }) => {
      const fila = crear('tr', alerta ? 'fila_alerta' : null);
      for (const celda of celdas) fila.append(crear('td', null, celda));
      return fila;
    }));
  }

  async function ver_mision(mision) {
    detalle_mision.hidden = false;
    detalle_titulo.textContent = `Misión #${mision.number} · ${mision.tipo ?? ''}`;
    detalle_nota.textContent = 'Cargando…';
    try {
      // Una misión inundada se muestra junto a su misión en seco de referencia
      if (mision.tipo === 'inundado') {
        const comparacion = await pedir_api(`/missions/${mision.number}/comparacion`);
        detalle_meta.textContent = comparacion.referencia
          ? `Comparada con la misión en seco #${comparacion.referencia}. La profundidad es la que calculó el dron con las correcciones del barómetro.`
          : 'Sin misión en seco de referencia: no hay con qué comparar.';
        pintar_tabla(['Waypoint', 'Distancia seco', 'Distancia inundado', 'Δ niveles', 'Profundidad', 'Alerta'],
          comparacion.filas.map((fila) => ({
            alerta: fila.alerta,
            celdas: [String(fila.indice + 1), metros(fila.seco?.distancia_m), metros(fila.inundado?.distancia_m),
              metros(fila.diferencia_niveles_m, 3), centimetros(fila.profundidad_m), fila.alerta ? 'Sí' : '—'],
          })));
        detalle_nota.textContent = comparacion.filas.length ? '' : 'La misión todavía no tiene lecturas.';
      } else {
        const detalle = await pedir_api(`/missions/${mision.number}`);
        detalle_meta.textContent = `${detalle.machine} · creada por ${detalle.creada_por} · ${formatear_fecha(detalle.inicio)}`;
        pintar_tabla(['Waypoint', 'Válida', 'Distancia LiDAR', 'Altura dron', 'Nivel superficie', 'Lecturas'],
          detalle.lecturas.map((lectura) => ({
            celdas: [String(lectura.indice + 1), lectura.valida ? 'Sí' : 'No', metros(lectura.distancia_m),
              metros(lectura.altura_dron_m), metros(lectura.nivel_superficie_m, 3), String(lectura.n_lecturas ?? '—')],
          })));
        detalle_nota.textContent = detalle.lecturas.length ? '' : 'La misión todavía no tiene lecturas.';
      }
      detalle_mision.scrollIntoView({ behavior: 'smooth', block: 'start' });
    } catch (error) {
      if (error.message !== 'sesion_invalida') detalle_nota.textContent = error.message;
    }
  }

  // ===== Eventos =====
  filtro_alertas.addEventListener('change', cargar_alertas);
  document.getElementById('boton_cerrar_detalle').addEventListener('click', () => { detalle_mision.hidden = true; });
  document.getElementById('boton_cerrar_foto').addEventListener('click', () => dialogo_foto.close());

  cargar_alertas();
  cargar_misiones();
  // Se actualiza solo; en una pestaña oculta no se gastan peticiones
  setInterval(() => {
    if (document.hidden) return;
    cargar_alertas();
    cargar_misiones();
  }, intervalo_actualizacion_ms);
}
