// Bitácora de ingeniería. Depende de ../../main.js (api_url, clave_token y el observador de .revelar)
// y de ../sesion_panel.js (sesión, pedir_api, enviar_json y fechas)
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

  let registros = [];
  let numero_seleccionado = null;

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
    formulario_registro.querySelectorAll('.campo_entrada_invalido').forEach((campo) => campo.classList.remove('campo_entrada_invalido'));
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
      estado_lista.textContent = error.message;
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
      evidencia.forEach((archivo) => galeria.append(crear_archivo_evidencia(archivo)));
    });
    detalle_evidencia.append(desplegable);
  }

  // archivo sigue el BaseModel media: filename, content_type, url_path, onwer
  function crear_archivo_evidencia({ filename = 'archivo', content_type = '', url_path = '' }) {
    const figura = document.createElement('figure');
    figura.className = 'evidencia_archivo';
    const url = new URL(url_path, api_url);
    // Solo http(s): una url_path con "javascript:" no debe convertirse en un enlace ejecutable
    if (!['http:', 'https:'].includes(url.protocol)) return figura;

    let vista;
    if (content_type.startsWith('image/')) {
      vista = document.createElement('img');
      vista.src = url.href;
      vista.alt = filename;
      vista.loading = 'lazy';
    } else if (content_type.startsWith('video/')) {
      vista = document.createElement('video');
      vista.src = url.href;
      vista.controls = true;
      vista.preload = 'metadata';
    } else {
      vista = document.createElement('a');
      vista.href = url.href;
      vista.target = '_blank';
      vista.rel = 'noopener';
      vista.textContent = 'Abrir archivo';
    }
    const pie = document.createElement('figcaption');
    pie.textContent = filename;
    figura.append(vista, pie);
    return figura;
  }

  // ===== Eventos =====
  formulario_registro.addEventListener('input', ocultar_error);
  boton_volver.addEventListener('click', mostrar_formulario);

  formulario_registro.addEventListener('submit', async (evento) => {
    evento.preventDefault();
    const vacios = [...formulario_registro.querySelectorAll('[required]')].filter((campo) => !campo.value.trim());
    if (vacios.length > 0) {
      vacios.forEach((campo) => campo.classList.add('campo_entrada_invalido'));
      vacios[0].focus();
      mostrar_error('Completa los campos obligatorios.');
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
    boton_guardar_registro.disabled = true;
    boton_guardar_registro.classList.add('esta_cargando');
    texto_boton_guardar.textContent = 'Guardando…';
    try {
      await enviar_json('/engineer_log/new', 'POST', cuerpo);
      formulario_registro.reset();
      await cargar_registros();
      if (registros.length > 0) mostrar_registro(registros[0]);
    } catch (error) {
      if (error.message === 'sesion_invalida') return;
      mostrar_error(error.message);
    } finally {
      boton_guardar_registro.disabled = false;
      boton_guardar_registro.classList.remove('esta_cargando');
      texto_boton_guardar.textContent = 'Guardar registro';
    }
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
