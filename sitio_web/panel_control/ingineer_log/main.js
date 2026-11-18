// Bitácora de ingeniería. Depende de ../../main.js (api_url, clave_token y el observador de .revelar)
// y de ../sesion_panel.js (sesión, pedir_api, enviar_json, fechas y utilidades de formulario)
const rol_puede_eliminar = 'O5-1'; // DELETE /engineer_log/number exige verify_role_Depends_O5

// Etiquetas de los campos del BaseModel engineer_log, en el orden en que se muestran
const campos_detalle = [
  ['problem', 'Problema'],
  ['hipothesis', 'Hipótesis'],
  ['solution', 'Solución'],
  ['result', 'Resultado'],
  ['next_steps', 'Siguientes pasos'],
  ['log', 'Registro'],
];

// Igual que allowed_types y max_size de enginer_log_rt.py. La API vuelve a comprobarlo con los bytes reales,
// esto solo avisa antes de subir. archivo.type viene vacío si el navegador no reconoce la extensión
const tipos_evidencia = ['image/jpeg', 'image/png', 'image/webp', 'video/mp4', 'video/quicktime', 'application/pdf'];
const tamano_maximo_evidencia = 200 * 1024 * 1024;

// Devuelve el motivo por el que el archivo no se puede subir (se muestra en rojo en la lista) o null si es válido
// Extensión -> tipo, para cuando archivo.type llega vacío (mismo mapa que allowed_types de la API, al revés)
const tipo_por_extension = { jpg: 'image/jpeg', jpeg: 'image/jpeg', png: 'image/png', webp: 'image/webp', mp4: 'video/mp4', mov: 'video/quicktime', pdf: 'application/pdf' };

function validar_archivo(archivo) {
  if (archivo.size === 0) return 'archivo vacío';
  // Mismos textos que motivo_rechazo(): el aviso previo y la respuesta de la API dicen lo mismo
  if (archivo.size > tamano_maximo_evidencia) return 'supera el tamaño máximo';
  const extension = archivo.name.includes('.') ? archivo.name.split('.').pop().toLowerCase() : '';
  const tipo = archivo.type || tipo_por_extension[extension] || '';
  if (!tipos_evidencia.includes(tipo)) return 'tipo no permitido';
  return null;
}

const sesion = leer_sesion();
if (!sesion) {
  // salir_del_panel();
} else {
  iniciar_bitacora();
}

function iniciar_bitacora() {
  const formulario_registro = document.getElementById('formulario_registro');
  const boton_guardar_registro = document.getElementById('boton_guardar_registro');
  const texto_boton_guardar = document.getElementById('texto_boton_guardar');
  const mensaje_error_registro = document.getElementById('mensaje_error_registro');
  const mensaje_error_registro_texto = document.getElementById('mensaje_error_registro_texto');
  const vista_registro = document.getElementById('vista_registro');
  const detalle_numero = document.getElementById('detalle_numero');
  const detalle_nombre = document.getElementById('detalle_nombre');
  const detalle_meta = document.getElementById('detalle_meta');
  const detalle_campos = document.getElementById('detalle_campos');
  const detalle_evidencia = document.getElementById('detalle_evidencia');
  const boton_volver = document.getElementById('boton_volver');
  const boton_eliminar_registro = document.getElementById('boton_eliminar_registro');
  const total_registros = document.getElementById('total_registros');
  const estado_lista = document.getElementById('estado_lista');
  const lista_registros = document.getElementById('lista_registros');
  const plantilla_registro = document.getElementById('plantilla_registro');
  const zona_evidencia = document.getElementById('zona_evidencia');
  const campo_evidencia = document.getElementById('campo_evidencia');
  const lista_adjuntos = document.getElementById('lista_adjuntos');
  const plantilla_adjunto = document.getElementById('plantilla_adjunto');
  const visor_evidencia = document.getElementById('visor_evidencia');
  const visor_contador = document.getElementById('visor_contador');
  const visor_nombre = document.getElementById('visor_nombre');
  const visor_marco = document.getElementById('visor_marco');
  const visor_anterior = document.getElementById('visor_anterior');
  const visor_siguiente = document.getElementById('visor_siguiente');

  let registros = [];
  let numero_seleccionado = null;
  let adjuntos = []; // { archivo, url_vista, error }: elegidos y pendientes de subir
  let urls_evidencia = []; // URLs blob de la evidencia mostrada; se liberan al cambiar de registro
  let medios_visor = []; // { tipo, url, nombre } por posición en la galería; solo imágenes y videos ya descargados
  let posicion_visor = 0;

  mostrar_datos_sesion(sesion);

  // ===== Utilidades =====
  function formatear_numero(numero) {
    return `log_${String(numero).padStart(3, '0')}`;
  }

  function mostrar_error(texto) {
    mensaje_error_registro_texto.textContent = texto;
    mensaje_error_registro.hidden = false;
  }

  function ocultar_error() {
    mensaje_error_registro.hidden = true;
    limpiar_invalidos(formulario_registro);
  }

  function formatear_tamano(bytes) {
    return bytes < 1024 * 1024 ? `${Math.max(1, Math.round(bytes / 1024))} KB` : `${(bytes / 1024 / 1024).toFixed(1)} MB`;
  }

  // ===== Adjuntos: evidencia elegida que se sube después de crear el registro =====
  function agregar_adjuntos(archivos) {
    [...archivos].forEach((archivo) => {
      const repetido = adjuntos.some(({ archivo: otro }) => otro.name === archivo.name && otro.size === archivo.size && otro.lastModified === archivo.lastModified);
      if (repetido) return;
      const url_vista = archivo.type.startsWith('image/') ? URL.createObjectURL(archivo) : null;
      adjuntos.push({ archivo, url_vista, error: validar_archivo(archivo) });
    });
    pintar_adjuntos();
  }

  function quitar_adjunto(adjunto) {
    if (adjunto.url_vista) URL.revokeObjectURL(adjunto.url_vista);
    adjuntos = adjuntos.filter((otro) => otro !== adjunto);
    pintar_adjuntos();
  }

  function limpiar_adjuntos() {
    adjuntos.forEach(({ url_vista }) => url_vista && URL.revokeObjectURL(url_vista));
    adjuntos = [];
    pintar_adjuntos();
  }

  function pintar_adjuntos() {
    lista_adjuntos.replaceChildren();
    adjuntos.forEach((adjunto) => {
      const fila = plantilla_adjunto.content.firstElementChild.cloneNode(true);
      const vista = fila.querySelector('.adjunto_vista');
      if (adjunto.url_vista) {
        const imagen = document.createElement('img');
        imagen.src = adjunto.url_vista;
        imagen.alt = '';
        vista.append(imagen);
      } else {
        vista.textContent = adjunto.archivo.name.split('.').pop().slice(0, 4).toUpperCase();
      }
      fila.classList.toggle('adjunto_invalido', Boolean(adjunto.error));
      fila.querySelector('.adjunto_nombre').textContent = adjunto.archivo.name;
      fila.querySelector('.adjunto_meta').textContent = adjunto.error || formatear_tamano(adjunto.archivo.size);
      fila.querySelector('.adjunto_quitar').addEventListener('click', () => quitar_adjunto(adjunto));
      lista_adjuntos.append(fila);
    });
  }

  function motivo_rechazo(error) {
    if (error.estado === 415) return 'tipo no permitido';
    if (error.estado === 413) return 'supera el tamaño máximo';
    return error.message;
  }

  // De uno en uno para mostrar el progreso; devuelve los archivos que la API rechazó
  async function subir_adjuntos(numero) {
    const fallidos = [];
    const validos = adjuntos.filter(({ error }) => !error);
    for (const [indice, { archivo }] of validos.entries()) {
      texto_boton_guardar.textContent = `Subiendo evidencia ${indice + 1}/${validos.length}…`;
      const datos = new FormData();
      datos.append('file', archivo);
      try {
        // Sin Content-Type: el navegador lo pone con el separador del multipart
        await pedir_api(`/engineer_log/evidence?number=${numero}`, { method: 'POST', body: datos });
      } catch (error) {
        if (error.message === 'sesion_invalida') throw error;
        fallidos.push(`${archivo.name}: ${motivo_rechazo(error)}`);
      }
    }
    return fallidos;
  }

  // ===== Lista =====
  async function cargar_registros() {
    estado_lista.hidden = false;
    estado_lista.textContent = 'Cargando registros…';
    try {
      registros = await pedir_api('/engineer_log/all');
      registros.sort((a, b) => b.number - a.number);
      pintar_lista();
    } catch (error) {
      if (error.message === 'sesion_invalida') return;
      // La bitácora exige nivel 4 (verify_role_Depends_4); un rol menor recibe 403
      estado_lista.textContent = error.estado === 403 ? 'Tu rol no tiene acceso a la bitácora (se requiere nivel 4).' : error.message;
    }
  }

  function pintar_lista() {
    total_registros.textContent = registros.length;
    lista_registros.replaceChildren();
    estado_lista.hidden = registros.length > 0;
    estado_lista.textContent = 'Aún no hay registros.';

    registros.forEach((registro) => {
      const fila = plantilla_registro.content.firstElementChild.cloneNode(true);
      fila.dataset.numero = registro.number;
      fila.classList.toggle('tarjeta_registro_activa', registro.number === numero_seleccionado);
      fila.querySelector('.tarjeta_registro_numero').textContent = formatear_numero(registro.number);
      fila.querySelector('.titulo_registro').textContent = registro.name;
      fila.querySelector('.tarjeta_registro_resumen').textContent = registro.problem;
      fila.querySelector('.tarjeta_registro_meta').textContent = `${formatear_fecha(registro.date_creation)} · ${registro.onwer}`;
      fila.querySelector('.tarjeta_registro_boton').addEventListener('click', () => mostrar_registro(registro));
      lista_registros.append(fila);
    });
  }

  function marcar_seleccionado(numero) {
    numero_seleccionado = numero;
    lista_registros.querySelectorAll('.tarjeta_registro').forEach((fila) => {
      fila.classList.toggle('tarjeta_registro_activa', Number(fila.dataset.numero) === numero);
    });
  }

  // ===== Detalle =====
  function mostrar_registro(registro) {
    marcar_seleccionado(registro.number);
    detalle_numero.textContent = formatear_numero(registro.number);
    detalle_nombre.textContent = registro.name;
    const actualizado = registro.date_update ? ` · Actualizado ${formatear_fecha(registro.date_update)}` : '';
    detalle_meta.textContent = `${registro.onwer} · ${formatear_fecha(registro.date_creation)}${actualizado}`;

    detalle_campos.replaceChildren();
    campos_detalle.forEach(([clave, etiqueta]) => {
      if (!registro[clave]) return;
      const grupo = document.createElement('div');
      const titulo = document.createElement('dt');
      const valor = document.createElement('dd');
      titulo.textContent = etiqueta;
      valor.textContent = registro[clave];
      grupo.append(titulo, valor);
      detalle_campos.append(grupo);
    });

    pintar_evidencia(registro.evidence);
    boton_eliminar_registro.hidden = sesion.rol !== rol_puede_eliminar;
    formulario_registro.hidden = true;
    vista_registro.hidden = false;
  }

  function mostrar_formulario() {
    marcar_seleccionado(null);
    vista_registro.hidden = true;
    formulario_registro.hidden = false;
  }

  // La evidencia queda plegada; los archivos solo se crean (y descargan) al desplegarla
  function pintar_evidencia(evidencia) {
    urls_evidencia.forEach((url) => URL.revokeObjectURL(url));
    urls_evidencia = [];
    medios_visor = [];
    detalle_evidencia.replaceChildren();
    if (!Array.isArray(evidencia) || evidencia.length === 0) {
      const vacia = document.createElement('p');
      vacia.className = 'evidencia_vacia';
      vacia.textContent = 'Sin evidencia adjunta.';
      detalle_evidencia.append(vacia);
      return;
    }

    const desplegable = document.createElement('details');
    desplegable.className = 'evidencia';
    const resumen = document.createElement('summary');
    resumen.className = 'evidencia_resumen';
    resumen.textContent = `Evidencia · ${evidencia.length} ${evidencia.length === 1 ? 'archivo' : 'archivos'}`;
    const galeria = document.createElement('div');
    galeria.className = 'evidencia_galeria';
    desplegable.append(resumen, galeria);

    desplegable.addEventListener('toggle', () => {
      if (!desplegable.open || galeria.childElementCount > 0) return;
      evidencia.forEach((archivo, indice) => galeria.append(crear_archivo_evidencia(archivo, indice)));
    });
    detalle_evidencia.append(desplegable);
  }

  // GET /engineer_log/files exige token y un <img src> no puede enviarlo: se descarga con fetch y se muestra como blob
  async function descargar_evidencia(url) {
    const respuesta = await fetch(url, { headers: { Authorization: `Bearer ${leer_token()}` } });
    if (respuesta.status === 401) salir_del_panel();
    if (!respuesta.ok) throw new Error(`no se pudo cargar (${respuesta.status})`);
    const url_blob = URL.createObjectURL(await respuesta.blob());
    urls_evidencia.push(url_blob);
    return url_blob;
  }

  // archivo sigue el BaseModel media: filename, content_type, url_path, onwer
  // indice es la posición en la galería: el visor recorre los archivos en ese mismo orden
  function crear_archivo_evidencia({ filename = 'archivo', content_type = '', url_path = '' }, indice) {
    const figura = document.createElement('figure');
    figura.className = 'evidencia_archivo';
    const url = new URL(url_path, api_url);
    // Solo la propia API: el token no debe enviarse a otro servidor ni a una url_path con "javascript:"
    if (url.origin !== new URL(api_url).origin) return figura;

    const tipo = content_type.startsWith('image/') ? 'imagen' : content_type.startsWith('video/') ? 'video' : 'otro';
    let vista;
    let miniatura = null;
    if (tipo === 'otro') {
      vista = document.createElement('a');
      vista.target = '_blank';
      vista.rel = 'noopener';
      vista.textContent = 'Abrir archivo';
    } else {
      // La miniatura solo muestra; se reproduce y se ve completa en el visor
      vista = document.createElement(tipo === 'imagen' ? 'img' : 'video');
      if (tipo === 'imagen') vista.alt = filename;
      else Object.assign(vista, { muted: true, playsInline: true, preload: 'metadata' });
      miniatura = document.createElement('button');
      miniatura.type = 'button';
      miniatura.className = `evidencia_miniatura evidencia_miniatura_${tipo}`;
      miniatura.disabled = true;
      miniatura.setAttribute('aria-label', `Ampliar ${filename}`);
      miniatura.append(vista);
      miniatura.addEventListener('click', () => abrir_visor(indice));
    }
    const pie = document.createElement('figcaption');
    pie.textContent = `${filename} · cargando…`;
    figura.append(miniatura || vista, pie);

    descargar_evidencia(url.href)
      .then((url_blob) => {
        if (tipo === 'otro') {
          vista.href = url_blob;
        } else {
          vista.src = url_blob;
          medios_visor[indice] = { tipo, url: url_blob, nombre: filename };
          miniatura.disabled = false;
        }
        pie.textContent = filename;
      })
      .catch((error) => { pie.textContent = `${filename} · ${error.message}`; });
    return figura;
  }

  // ===== Visor de evidencia =====
  // medios_visor puede tener huecos (PDF o archivos que aún no bajan): se navega solo entre los disponibles
  function posiciones_disponibles() {
    return medios_visor.flatMap((medio, indice) => (medio ? [indice] : []));
  }

  function abrir_visor(indice) {
    if (!medios_visor[indice]) return;
    mostrar_en_visor(indice);
    if (!visor_evidencia.open) visor_evidencia.showModal();
  }

  function mostrar_en_visor(indice) {
    posicion_visor = indice;
    const { tipo, url, nombre } = medios_visor[indice];
    const medio = document.createElement(tipo === 'imagen' ? 'img' : 'video');
    medio.className = 'visor_evidencia_medio';
    medio.src = url;
    if (tipo === 'imagen') {
      medio.alt = nombre;
    } else {
      Object.assign(medio, { controls: true, autoplay: true, playsInline: true });
    }
    // Reemplazar el contenido detiene el video anterior
    visor_marco.replaceChildren(medio);
    visor_nombre.textContent = nombre;

    const disponibles = posiciones_disponibles();
    visor_contador.textContent = `${disponibles.indexOf(indice) + 1} / ${disponibles.length}`;
    visor_anterior.hidden = disponibles.length < 2;
    visor_siguiente.hidden = disponibles.length < 2;
  }

  // paso: -1 anterior, 1 siguiente. Da la vuelta al llegar a un extremo
  function mover_visor(paso) {
    const disponibles = posiciones_disponibles();
    if (disponibles.length < 2) return;
    const actual = disponibles.indexOf(posicion_visor);
    mostrar_en_visor(disponibles[(actual + paso + disponibles.length) % disponibles.length]);
  }

  function cerrar_visor() {
    visor_evidencia.close();
  }

  // ===== Eventos =====
  document.getElementById('visor_cerrar').addEventListener('click', cerrar_visor);
  visor_anterior.addEventListener('click', () => mover_visor(-1));
  visor_siguiente.addEventListener('click', () => mover_visor(1));
  // Clic en el fondo (fuera de la imagen o el video) cierra el visor
  visor_evidencia.addEventListener('click', (evento) => {
    if (evento.target === visor_evidencia || evento.target === visor_marco) cerrar_visor();
  });
  visor_evidencia.addEventListener('keydown', (evento) => {
    // Con el video enfocado, las flechas adelantan o retroceden el video
    if (evento.target.tagName === 'VIDEO') return;
    if (evento.key === 'ArrowLeft') mover_visor(-1);
    if (evento.key === 'ArrowRight') mover_visor(1);
  });
  // Esc, el botón o el clic fuera pasan por aquí: vaciar el marco detiene el video
  visor_evidencia.addEventListener('close', () => visor_marco.replaceChildren());

  formulario_registro.addEventListener('input', ocultar_error);
  boton_volver.addEventListener('click', mostrar_formulario);

  campo_evidencia.addEventListener('change', () => {
    agregar_adjuntos(campo_evidencia.files);
    campo_evidencia.value = ''; // permite volver a elegir el mismo archivo después de quitarlo
  });

  // Arrastrar y soltar (PC). El contador evita que el resaltado parpadee al pasar sobre los hijos de la zona
  let profundidad_arrastre = 0;
  zona_evidencia.addEventListener('dragenter', (evento) => {
    evento.preventDefault();
    profundidad_arrastre += 1;
    zona_evidencia.classList.add('esta_arrastrando');
  });
  zona_evidencia.addEventListener('dragover', (evento) => {
    evento.preventDefault();
    evento.dataTransfer.dropEffect = 'copy';
  });
  zona_evidencia.addEventListener('dragleave', () => {
    profundidad_arrastre = Math.max(0, profundidad_arrastre - 1);
    if (profundidad_arrastre === 0) zona_evidencia.classList.remove('esta_arrastrando');
  });
  zona_evidencia.addEventListener('drop', (evento) => {
    evento.preventDefault();
    profundidad_arrastre = 0;
    zona_evidencia.classList.remove('esta_arrastrando');
    agregar_adjuntos(evento.dataTransfer.files);
  });
  // Un archivo soltado fuera de la zona abriría el archivo en la pestaña y se perdería el formulario
  window.addEventListener('dragover', (evento) => evento.preventDefault());
  window.addEventListener('drop', (evento) => evento.preventDefault());

  formulario_registro.addEventListener('submit', async (evento) => {
    evento.preventDefault();
    const vacios = [...formulario_registro.querySelectorAll('[required]')].filter((campo) => !campo.value.trim());
    if (vacios.length > 0) {
      vacios.forEach((campo) => campo.classList.add('campo_entrada_invalido'));
      vacios[0].focus();
      mostrar_error('Completa los campos obligatorios.');
      return;
    }
    if (adjuntos.some(({ error }) => error)) {
      mostrar_error('Quita los archivos marcados en rojo antes de guardar.');
      return;
    }

    // Los atributos name del formulario coinciden con las claves del BaseModel engineer_log
    const datos = Object.fromEntries(new FormData(formulario_registro));
    const cuerpo = {
      ...datos,
      next_steps: datos.next_steps.trim() || null,
      onwer: sesion.nombre,
      number: 0, // la API asigna el número consecutivo
      evidence: [],
      date_creation: new Date().toISOString(),
    };

    ocultar_error();
    texto_boton_guardar.textContent = 'Guardando…';
    await con_boton_ocupado(boton_guardar_registro, async () => {
      try {
        // Dos pasos: el registro va en JSON y la evidencia en multipart, cada archivo a POST /engineer_log/evidence
        const { number } = await enviar_json('/engineer_log/new', 'POST', cuerpo);
        const fallidos = await subir_adjuntos(number);
        formulario_registro.reset();
        limpiar_adjuntos();
        await cargar_registros();
        const creado = registros.find((registro) => registro.number === number) || registros[0];
        if (creado) mostrar_registro(creado);
        if (fallidos.length > 0) {
          window.alert(`El registro ${formatear_numero(number)} se guardó, pero esta evidencia no se subió:\n\n${fallidos.join('\n')}`);
        }
      } catch (error) {
        if (error.message === 'sesion_invalida') return;
        mostrar_error(error.message);
      }
    }, () => { texto_boton_guardar.textContent = 'Guardar registro'; });
  });

  boton_eliminar_registro.addEventListener('click', async () => {
    const numero = numero_seleccionado;
    // La API renumera los registros al borrar, así que los posteriores cambian de número
    const confirmado = window.confirm(`¿Eliminar ${formatear_numero(numero)}? Los registros posteriores cambiarán de número.`);
    if (!confirmado) return;
    boton_eliminar_registro.disabled = true;
    try {
      await pedir_api(`/engineer_log/number?number=${numero}`, { method: 'DELETE' });
      mostrar_formulario();
      await cargar_registros();
    } catch (error) {
      if (error.message === 'sesion_invalida') return;
      window.alert(error.message);
    } finally {
      boton_eliminar_registro.disabled = false;
    }
  });

  // ===== Arranque =====
  cargar_registros();
}
