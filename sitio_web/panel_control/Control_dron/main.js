// Control del dron. Depende de ../../main.js (api_url, clave_token y el observador de .revelar)
// y de ../sesion_panel.js (leer_sesion, salir_del_panel, mostrar_datos_sesion y pedir_api).
//
// Usa dos WebSockets independientes, así un corte del vídeo no tumba los mandos ni al revés:
//   /camera_rt/ws/view (camera_rt.py): frames JPEG binarios y ajustes de la ESP32-CAM.
//       Primer mensaje {"token", "machine"}; comandos {"set": nombre, "value": n} y {"flash": 0..255}.
//   /ws (main.py): mandos, movimiento y PID con el mismo formato que el Home del panel:
//       {"tipo": "comando", "action": "armar"}                         acciones puntuales
//       {"tipo": "control", "action": "movimiento", "ejes": {...}}      palancas a 10 Hz, valores de -1 a 1
//       {"tipo": "comando", "action": "pid", "pid": {alabeo: {kp, ki, kd}, ...}}
//   La API devuelve cada mensaje con api_response (api_bm.api_function); el dron responde con
//   "telemetria", "estado_dron" ({armado, modo}) y "pid" ({pid: {...}}).
const ws_base_url = api_url.replace(/^http/, 'ws');
const ws_api_url = `${ws_base_url}/ws`;
const ws_camara_url = `${ws_base_url}/camera_rt/ws/view`;
const roles_con_acceso = ['4', 'O5-1'];
const clave_maquina = 'samati_dron_maquina';
const clave_pid = 'samati_dron_pid';
const periodo_movimiento_ms = 100;
const tiempo_max_sin_video_ms = 3000;
const tiempo_max_sin_senal_ms = 5000;
const tiempo_paro_ms = 600;
const maximo_registro = 80;
const metros_por_segundo_a_km_h = 3.6;
const pixeles_por_grado_cabeceo = 2.4;
const voltaje_celda_minimo = 3.0;
const voltaje_celda_maximo = 4.2;

const ejes_pid = ['alabeo', 'cabeceo', 'guinada', 'altitud'];
const ganancias_pid = ['kp', 'ki', 'kd'];
const nombres_ejes_pid = { alabeo: 'Alabeo', cabeceo: 'Cabeceo', guinada: 'Guiñada', altitud: 'Altitud' };
// Punto de partida para ajustar, no los valores del dron: "Leer del dron" trae los reales
const valores_pid_base = {
  alabeo: { kp: 1.2, ki: 0.02, kd: 0.35 },
  cabeceo: { kp: 1.2, ki: 0.02, kd: 0.35 },
  guinada: { kp: 2.0, ki: 0.05, kd: 0 },
  altitud: { kp: 1.5, ki: 0.1, kd: 0.4 },
};
const nombres_modo = { estabilizado: 'Estabilizado', altitud: 'Altitud', posicion: 'Posición' };
const textos_accion = {
  armar: 'Armar motores',
  desarmar: 'Desarmar motores',
  despegar: 'Despegar',
  aterrizar: 'Aterrizar',
  mantener_posicion: 'Mantener posición',
  regresar_casa: 'Regresar a casa',
  paro_emergencia: 'Paro de emergencia',
  modo: 'Modo de vuelo',
  pid: 'PID',
  pid_leer: 'Solicitud de PID',
};

// localStorage puede fallar (modo privado o almacenamiento bloqueado): solo guarda preferencias
function leer_preferencia(clave) {
  try { return JSON.parse(localStorage.getItem(clave)); } catch (error) { return null; }
}
function guardar_preferencia(clave, valor) {
  try { localStorage.setItem(clave, JSON.stringify(valor)); } catch (error) { /* sin almacenamiento */ }
}

// ===== Pestañas del panel: Vuelo · PID · Registro =====
// Son solo de interfaz: se conectan siempre, aunque no haya sesión o falle algo en iniciar_control()
function iniciar_pestanas_control() {
  const pestanas = [...document.querySelectorAll('.pestana_control')];

  function mostrar_pestana(pestana) {
    for (const otra of pestanas) {
      const activa = otra === pestana;
      otra.setAttribute('aria-selected', String(activa));
      otra.tabIndex = activa ? 0 : -1;
      document.getElementById(otra.getAttribute('aria-controls')).hidden = !activa;
    }
    if (pestana.id === 'pestana_registro') document.getElementById('aviso_registro').hidden = true;
    guardar_preferencia('pestana_control', pestana.id);
  }

  pestanas.forEach((pestana, indice) => {
    pestana.addEventListener('click', () => mostrar_pestana(pestana));
    // Flechas izquierda y derecha entre pestañas; stopPropagation evita que muevan el dron
    pestana.addEventListener('keydown', (evento) => {
      if (evento.key !== 'ArrowRight' && evento.key !== 'ArrowLeft') return;
      evento.preventDefault();
      evento.stopPropagation();
      const siguiente = pestanas[(indice + (evento.key === 'ArrowRight' ? 1 : -1) + pestanas.length) % pestanas.length];
      mostrar_pestana(siguiente);
      siguiente.focus();
    });
  });

  // Vuelve a la última pestaña usada
  const guardada = pestanas.find((pestana) => pestana.id === leer_preferencia('pestana_control'));
  if (guardada) mostrar_pestana(guardada);
}
iniciar_pestanas_control();

const sesion = leer_sesion();
if (!sesion) {
  // salir_del_panel();
} else {
  iniciar_control();
}

function iniciar_control() {
  // ===== Elementos =====
  const aviso_permisos = document.getElementById('aviso_permisos');
  const cuadricula_control = document.getElementById('cuadricula_control');
  const estado_enlace = document.getElementById('estado_enlace');
  const estado_enlace_texto = document.getElementById('estado_enlace_texto');
  const selector_maquina = document.getElementById('selector_maquina');
  // Vídeo
  const estado_camara = document.getElementById('estado_camara');
  const estado_camara_texto = document.getElementById('estado_camara_texto');
  const marco_video = document.getElementById('marco_video');
  const imagen_video = document.getElementById('imagen_video');
  const video_espera = document.getElementById('video_espera');
  const video_espera_texto = document.getElementById('video_espera_texto');
  const hud_en_vivo = document.getElementById('hud_en_vivo');
  const hud_estadisticas = document.getElementById('hud_estadisticas');
  const hud_horizonte = document.getElementById('hud_horizonte');
  const hud_armado = document.getElementById('hud_armado');
  const hud_modo = document.getElementById('hud_modo');
  const texto_resolucion = document.getElementById('texto_resolucion');
  const boton_captura = document.getElementById('boton_captura');
  const boton_hud = document.getElementById('boton_hud');
  const boton_pantalla_completa = document.getElementById('boton_pantalla_completa');
  const texto_pantalla_completa = document.getElementById('texto_pantalla_completa');
  const boton_salir_pantalla = document.getElementById('boton_salir_pantalla');
  const control_flash = document.getElementById('control_flash');
  const controles_ajuste = document.querySelectorAll('[data-ajuste]');
  // Mandos
  const insignia_armado = document.getElementById('insignia_armado');
  const boton_armar = document.getElementById('boton_armar');
  const texto_boton_armar = document.getElementById('texto_boton_armar');
  const botones_mando = document.querySelectorAll('.boton_mando');
  const selector_modo = document.getElementById('selector_modo');
  const boton_paro = document.getElementById('boton_paro');
  // Movimiento
  const palancas = document.querySelectorAll('.palanca');
  const control_teclado = document.getElementById('control_teclado');
  const control_limite = document.getElementById('control_limite');
  const valor_limite = document.getElementById('valor_limite');
  const teclas_guia = document.querySelectorAll('.guia_teclas kbd');
  // PID
  const estado_pid = document.getElementById('estado_pid');
  const pestanas_pid = document.querySelectorAll('.pestana_pid');
  const boton_enviar_pid = document.getElementById('boton_enviar_pid');
  const boton_leer_pid = document.getElementById('boton_leer_pid');
  const boton_restablecer_pid = document.getElementById('boton_restablecer_pid');
  const tabla_pid_cuerpo = document.getElementById('tabla_pid_cuerpo');
  // Registro
  const lista_registro = document.getElementById('lista_registro');
  const boton_limpiar_registro = document.getElementById('boton_limpiar_registro');
  // Punto de aviso de la pestaña Registro (las pestañas se manejan en iniciar_pestanas_control)
  const pestana_registro = document.getElementById('pestana_registro');
  const aviso_registro = document.getElementById('aviso_registro');

  mostrar_datos_sesion(sesion);

  if (!roles_con_acceso.includes(sesion.rol)) {
    aviso_permisos.hidden = false;
    cuadricula_control.hidden = true;
    selector_maquina.disabled = true;
    return;
  }

  // ===== Utilidades =====
  function formatear_hora(fecha) {
    return fecha.toLocaleTimeString('es-MX', { hour12: false });
  }

  function es_numero(valor) {
    return typeof valor === 'number' && Number.isFinite(valor);
  }

  function limitar(valor, minimo, maximo) {
    return Math.min(maximo, Math.max(minimo, valor));
  }

  function escribir_numero(id, valor, decimales) {
    document.getElementById(id).textContent = es_numero(valor) ? valor.toFixed(decimales) : '—';
  }

  // tipo: 'info', 'envio' o 'error'
  function registrar(texto, tipo = 'info') {
    lista_registro.querySelector('.registro_vacio')?.remove();
    const fila = document.createElement('li');
    fila.className = `registro_${tipo}`;
    const hora = document.createElement('time');
    hora.textContent = formatear_hora(new Date());
    const mensaje = document.createElement('span');
    mensaje.textContent = texto;
    fila.append(hora, mensaje);
    lista_registro.prepend(fila);
    while (lista_registro.children.length > maximo_registro) lista_registro.lastElementChild.remove();
    // Un error con el registro oculto enciende el punto de la pestaña
    if (tipo === 'error' && pestana_registro.getAttribute('aria-selected') !== 'true') aviso_registro.hidden = false;
  }

  function vaciar_registro() {
    const vacio = document.createElement('li');
    vacio.className = 'registro_vacio';
    vacio.textContent = 'Sin eventos todavía.';
    lista_registro.replaceChildren(vacio);
  }

  // Los deslizadores pintan su parte llena con --relleno
  function pintar_relleno(deslizador) {
    const minimo = Number(deslizador.min);
    const maximo = Number(deslizador.max);
    const nivel = (Number(deslizador.value) - minimo) / (maximo - minimo);
    deslizador.style.setProperty('--relleno', `${limitar(nivel, 0, 1) * 100}%`);
  }

  // ===== Máquinas =====
  let maquina_actual = '';

  async function cargar_maquinas() {
    try {
      const maquinas = await pedir_api('/machine_rt/search_all_machine_rt_not');
      maquinas.sort((a, b) => a.number - b.number);
      selector_maquina.replaceChildren();
      if (maquinas.length === 0) {
        selector_maquina.append(new Option('Sin máquinas registradas', ''));
        selector_maquina.disabled = true;
        cambiar_estado_camara('sin_maquina', 'No hay máquinas registradas en la API.');
        return;
      }
      maquinas.forEach((maquina) => {
        const numero = `#${String(maquina.number).padStart(2, '0')}`;
        selector_maquina.append(new Option(`${numero} · ${maquina.name}`, maquina.name));
      });
      const guardada = leer_preferencia(clave_maquina);
      const inicial = maquinas.some((maquina) => maquina.name === guardada) ? guardada : maquinas[0].name;
      selector_maquina.value = inicial;
      seleccionar_maquina(inicial);
    } catch (error) {
      if (error.message === 'sesion_invalida') return;
      selector_maquina.replaceChildren(new Option('No disponible', ''));
      cambiar_estado_camara('error', error.message);
      registrar(`Máquinas: ${error.message}`, 'error');
    }
  }

  function seleccionar_maquina(nombre) {
    maquina_actual = nombre;
    guardar_preferencia(clave_maquina, nombre);
    registrar(`Máquina seleccionada: ${nombre}.`);
    conectar_camara();
  }

  selector_maquina.addEventListener('change', () => seleccionar_maquina(selector_maquina.value));

  // ===== Cámara =====
  const textos_estado_camara = {
    sin_maquina: 'Sin máquina',
    conectando: 'Conectando',
    sin_senal: 'Sin señal',
    en_vivo: 'En vivo',
    error: 'Error',
  };
  let socket_camara = null;
  let intentos_camara = 0;
  let temporizador_camara = null;
  let estado_camara_actual = '';
  let ultimo_frame_ms = 0;
  let ultimo_frame_blob = null;
  let url_frame_actual = null;
  let frame_pendiente = null;
  let decodificando = false;
  let motivo_rechazo = '';

  function cambiar_estado_camara(estado, texto_espera = null) {
    if (estado !== estado_camara_actual) {
      estado_camara_actual = estado;
      estado_camara.className = `estado_camara estado_camara_${estado}`;
      estado_camara_texto.textContent = textos_estado_camara[estado];
      hud_en_vivo.textContent = estado === 'en_vivo' ? 'En vivo' : textos_estado_camara[estado];
      hud_en_vivo.classList.toggle('esta_en_vivo', estado === 'en_vivo');
    }
    const en_vivo = estado === 'en_vivo';
    video_espera.hidden = en_vivo;
    imagen_video.hidden = !en_vivo && !ultimo_frame_blob;
    boton_captura.disabled = !ultimo_frame_blob;
    if (texto_espera) video_espera_texto.textContent = texto_espera;
  }

  function cerrar_camara() {
    clearTimeout(temporizador_camara);
    if (socket_camara) {
      socket_camara.cerrado_a_proposito = true;
      socket_camara.close(1000);
      socket_camara = null;
    }
    ultimo_frame_blob = null;
    frame_pendiente = null;
    imagen_video.hidden = true;
    hud_estadisticas.textContent = '— fps · — kB/s';
  }

  function conectar_camara() {
    cerrar_camara();
    if (!maquina_actual) {
      cambiar_estado_camara('sin_maquina', 'Selecciona una máquina para ver su cámara.');
      return;
    }
    motivo_rechazo = '';
    cambiar_estado_camara('conectando', `Conectando con la cámara de ${maquina_actual}…`);
    const socket = new WebSocket(ws_camara_url);
    socket.binaryType = 'blob';
    socket_camara = socket;

    socket.addEventListener('open', () => {
      socket.send(JSON.stringify({ token: sesion.token, machine: maquina_actual }));
    });

    socket.addEventListener('message', (evento) => {
      if (socket !== socket_camara) return;
      if (evento.data instanceof Blob) {
        mostrar_frame(evento.data);
        return;
      }
      try {
        procesar_evento_camara(JSON.parse(evento.data));
      } catch (error) {
        registrar('Cámara: mensaje con formato inválido.', 'error');
      }
    });

    socket.addEventListener('close', (evento) => {
      if (socket.cerrado_a_proposito || socket !== socket_camara) return;
      socket_camara = null;
      // 1008: la API rechazó la conexión (token, rol o máquina); reintentar no sirve
      if (evento.code === 1008) {
        const motivo = motivo_rechazo || evento.reason || 'Conexión rechazada';
        if (motivo === 'Unauthorized') {
          registrar('La API rechazó la sesión. Redirigiendo…', 'error');
          setTimeout(salir_del_panel, 1200);
          return;
        }
        const textos = { 'Machine not found': 'La máquina ya no existe en la API.', 'Role insuficiente': 'Tu rol no permite ver la cámara.' };
        cambiar_estado_camara('error', textos[motivo] || motivo);
        registrar(`Cámara rechazada: ${motivo}.`, 'error');
        return;
      }
      const espera_ms = Math.min(30000, 1000 * 2 ** intentos_camara);
      intentos_camara += 1;
      cambiar_estado_camara('error', `Se perdió la conexión con el servidor de vídeo. Reintento en ${Math.round(espera_ms / 1000)} s.`);
      temporizador_camara = setTimeout(conectar_camara, espera_ms);
    });
  }

  function procesar_evento_camara(evento) {
    switch (evento.type) {
      case 'auth_ok':
        intentos_camara = 0;
        registrar(`Vídeo autorizado para ${evento.machine} (${evento.fps} fps objetivo).`);
        if (evento.status) sincronizar_ajustes(evento.status);
        if (evento.online) cambiar_estado_camara('sin_senal', 'La cámara está en línea. Esperando el primer frame…');
        else cambiar_estado_camara('sin_senal', 'La cámara no está transmitiendo. Enciende la ESP32-CAM o revisa su WiFi.');
        break;
      case 'stats':
        hud_estadisticas.textContent = `${evento.fps.toFixed(1)} fps · ${evento.kbps} kB/s${evento.dropped ? ` · ${evento.dropped} desc.` : ''}`;
        break;
      case 'status':
        sincronizar_ajustes(evento);
        break;
      case 'camera_online':
        registrar('La cámara se conectó.');
        cambiar_estado_camara('sin_senal', 'La cámara está en línea. Esperando el primer frame…');
        break;
      case 'camera_offline':
        registrar('La cámara se desconectó.', 'error');
        hud_estadisticas.textContent = '— fps · — kB/s';
        cambiar_estado_camara('sin_senal', 'La cámara dejó de transmitir. Se mostrará en cuanto vuelva.');
        break;
      case 'error':
        motivo_rechazo = evento.message;
        if (evento.message === 'Camera offline') registrar('El ajuste no se aplicó: la cámara está desconectada.', 'error');
        else if (evento.message === 'Invalid command') registrar('La API rechazó el ajuste de cámara.', 'error');
        break;
      default:
        break;
    }
  }

  // Si llega un frame mientras el anterior se decodifica, solo se guarda el más reciente
  function mostrar_frame(blob) {
    ultimo_frame_ms = Date.now();
    ultimo_frame_blob = blob;
    if (estado_camara_actual !== 'en_vivo') cambiar_estado_camara('en_vivo');
    if (decodificando) {
      frame_pendiente = blob;
      return;
    }
    decodificando = true;
    const url = URL.createObjectURL(blob);
    imagen_video.src = url;
    imagen_video.decode()
      .catch(() => { /* un JPEG cortado: se espera al siguiente */ })
      .finally(() => {
        if (url_frame_actual) URL.revokeObjectURL(url_frame_actual);
        url_frame_actual = url;
        decodificando = false;
        if (frame_pendiente) {
          const siguiente = frame_pendiente;
          frame_pendiente = null;
          mostrar_frame(siguiente);
        }
      });
  }

  // El socket sigue abierto pero no llegan frames (WiFi de la cámara degradado)
  function revisar_video() {
    if (estado_camara_actual === 'en_vivo' && Date.now() - ultimo_frame_ms > tiempo_max_sin_video_ms) {
      cambiar_estado_camara('sin_senal', 'La imagen se congeló: la cámara no envía frames.');
    }
  }

  // Ajusta los controles al estado real que manda esp32cam_status_json()
  function sincronizar_ajustes(estado) {
    if (es_numero(estado.width) && es_numero(estado.height)) {
      texto_resolucion.textContent = `${estado.framesize ?? ''} · ${estado.width}×${estado.height}`;
    }
    controles_ajuste.forEach((control) => {
      const nombre = control.dataset.ajuste;
      const valor = estado[nombre];
      if (valor === undefined || control === document.activeElement) return;
      if (control.type === 'checkbox') control.checked = Boolean(valor);
      else if (nombre === 'framesize') {
        const opcion = [...control.options].find((candidata) => candidata.dataset.nombre === valor);
        if (opcion) control.value = opcion.value;
      } else if (es_numero(valor)) {
        if (control.tagName === 'SELECT' && ![...control.options].some((opcion) => Number(opcion.value) === valor)) {
          control.append(new Option(`Actual (${valor})`, valor));
        }
        control.value = valor;
        if (control.type === 'range') actualizar_salida_rango(control);
      }
    });
  }

  function actualizar_salida_rango(control) {
    pintar_relleno(control);
    const salida = document.querySelector(`output[for="${control.id}"]`);
    if (salida) salida.textContent = control.value;
  }

  function enviar_a_camara(comando) {
    if (!socket_camara || socket_camara.readyState !== WebSocket.OPEN) {
      registrar('Sin conexión con el servidor de vídeo.', 'error');
      return false;
    }
    socket_camara.send(JSON.stringify(comando));
    return true;
  }

  controles_ajuste.forEach((control) => {
    if (control.type === 'range') control.addEventListener('input', () => actualizar_salida_rango(control));
    control.addEventListener('change', () => {
      const valor = control.type === 'checkbox' ? Number(control.checked) : Number(control.value);
      if (enviar_a_camara({ set: control.dataset.ajuste, value: valor })) {
        registrar(`Cámara: ${control.dataset.ajuste} = ${valor}`, 'envio');
      }
    });
  });

  control_flash.addEventListener('input', () => actualizar_salida_rango(control_flash));
  control_flash.addEventListener('change', () => {
    const valor = Number(control_flash.value);
    if (enviar_a_camara({ flash: valor })) registrar(`Cámara: flash = ${valor}`, 'envio');
  });

  boton_captura.addEventListener('click', () => {
    if (!ultimo_frame_blob) return;
    const fecha = new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19);
    const enlace = document.createElement('a');
    const url = URL.createObjectURL(ultimo_frame_blob);
    enlace.href = url;
    enlace.download = `captura_${maquina_actual}_${fecha}.jpg`;
    enlace.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
    registrar('Captura guardada.');
  });

  boton_hud.addEventListener('click', () => {
    const visible = boton_hud.getAttribute('aria-pressed') !== 'true';
    boton_hud.setAttribute('aria-pressed', String(visible));
    marco_video.classList.toggle('hud_oculto', !visible);
  });

  function alternar_pantalla_completa() {
    if (document.fullscreenElement) {
      document.exitFullscreen();
      return;
    }
    marco_video.requestFullscreen().catch(() => registrar('El navegador no permitió la pantalla completa.', 'error'));
  }

  // Safari en iPhone no permite pantalla completa de un div: mejor ocultar el botón que dejarlo sin efecto
  if (!document.fullscreenEnabled) boton_pantalla_completa.hidden = true;
  boton_pantalla_completa.addEventListener('click', alternar_pantalla_completa);
  boton_salir_pantalla.addEventListener('click', () => document.exitFullscreen());
  imagen_video.addEventListener('dblclick', alternar_pantalla_completa);

  // También se sale con Esc o con el gesto del sistema: el estado se lee del evento, no del clic
  document.addEventListener('fullscreenchange', () => {
    const activa = document.fullscreenElement === marco_video;
    boton_pantalla_completa.setAttribute('aria-pressed', String(activa));
    texto_pantalla_completa.textContent = activa ? 'Salir de pantalla completa' : 'Pantalla completa';
    if (activa) boton_salir_pantalla.focus({ preventScroll: true });
    else boton_pantalla_completa.focus({ preventScroll: true });
  });

  // ===== Enlace con la API (/ws) =====
  const textos_estado_enlace = {
    desconectado: 'Sin conexión con la API',
    conectando: 'Conectando…',
    sin_senal: 'API conectada · Dron sin señal',
    en_linea: 'Dron en línea',
  };
  let socket_api = null;
  let api_autenticada = false;
  let sesion_rechazada = false;
  let intentos_api = 0;
  let temporizador_api = null;
  let ultimo_mensaje_dron_ms = 0;
  let estado_enlace_actual = '';

  function calcular_estado_enlace() {
    if (!socket_api || socket_api.readyState === WebSocket.CLOSED || socket_api.readyState === WebSocket.CLOSING) return 'desconectado';
    if (socket_api.readyState === WebSocket.CONNECTING || !api_autenticada) return 'conectando';
    return Date.now() - ultimo_mensaje_dron_ms <= tiempo_max_sin_senal_ms ? 'en_linea' : 'sin_senal';
  }

  function refrescar_estado_enlace() {
    const estado = calcular_estado_enlace();
    if (estado !== estado_enlace_actual) {
      estado_enlace_actual = estado;
      estado_enlace.className = `estado_enlace estado_enlace_${estado}`;
      estado_enlace_texto.textContent = textos_estado_enlace[estado];
    }
    refrescar_mandos();
  }

  function conectar_api() {
    clearTimeout(temporizador_api);
    if (sesion_rechazada) return;
    if (socket_api && socket_api.readyState <= WebSocket.OPEN) return;
    api_autenticada = false;
    socket_api = new WebSocket(ws_api_url);
    refrescar_estado_enlace();

    socket_api.addEventListener('open', () => {
      intentos_api = 0;
      socket_api.send(JSON.stringify({ token: sesion.token }));
      refrescar_estado_enlace();
    });

    socket_api.addEventListener('message', (evento) => {
      try {
        procesar_mensaje_api(JSON.parse(evento.data));
      } catch (error) {
        registrar('API: mensaje con formato inválido.', 'error');
      }
    });

    socket_api.addEventListener('close', (evento) => {
      api_autenticada = false;
      // Sin enlace no hay mandos: se sueltan las palancas para no reanudar con un valor viejo
      soltar_mandos();
      refrescar_estado_enlace();
      if (sesion_rechazada || evento.code === 1008) {
        sesion_rechazada = true;
        registrar('La API rechazó la sesión. Redirigiendo…', 'error');
        setTimeout(salir_del_panel, 1200);
        return;
      }
      const espera_ms = Math.min(30000, 1000 * 2 ** intentos_api);
      intentos_api += 1;
      registrar(`Enlace de mandos cerrado. Reintento en ${Math.round(espera_ms / 1000)} s.`, 'error');
      temporizador_api = setTimeout(conectar_api, espera_ms);
    });
  }

  function enviar_api(mensaje) {
    if (!socket_api || socket_api.readyState !== WebSocket.OPEN || !api_autenticada) return false;
    socket_api.send(JSON.stringify(mensaje));
    return true;
  }

  function enviar_comando(accion, datos = {}) {
    const enviado = enviar_api({ tipo: 'comando', action: accion, maquina: maquina_actual, ...datos });
    if (enviado) registrar(`${textos_accion[accion] ?? accion} enviado.`, 'envio');
    else registrar(`${textos_accion[accion] ?? accion}: sin enlace con la API.`, 'error');
    return enviado;
  }

  function procesar_mensaje_api(mensaje) {
    // Saludo inicial de main.py: {"message": "..."} sin campo tipo
    if (!mensaje.tipo && typeof mensaje.message === 'string') {
      const texto = mensaje.message;
      if (texto === 'Unauthorized' || texto.startsWith('WebSocket error')) {
        sesion_rechazada = true;
        registrar(`La API rechazó la sesión: ${texto}`, 'error');
      } else if (texto === 'Password received') {
        api_autenticada = true;
        registrar('Enlace de mandos listo.');
      }
      refrescar_estado_enlace();
      return;
    }
    switch (mensaje.tipo) {
      case 'telemetria':
        ultimo_mensaje_dron_ms = Date.now();
        pintar_telemetria(mensaje.datos ?? mensaje);
        break;
      case 'estado_dron':
        ultimo_mensaje_dron_ms = Date.now();
        if (typeof mensaje.armado === 'boolean') establecer_armado(mensaje.armado);
        if (nombres_modo[mensaje.modo]) establecer_modo(mensaje.modo);
        break;
      case 'pid':
        ultimo_mensaje_dron_ms = Date.now();
        if (mensaje.pid) recibir_pid(mensaje.pid);
        break;
      case 'comando':
        registrar(`API · ${textos_accion[mensaje.action] ?? mensaje.action}: ${mensaje.api_response?.message ?? 'sin respuesta'}`);
        break;
      case 'control':
        // Eco de cada paquete de movimiento: no se registra para no saturar la lista
        break;
      case 'error':
        registrar(mensaje.detalle || 'La API reportó un error.', 'error');
        break;
      default:
        break;
    }
    refrescar_estado_enlace();
  }

  // ===== Telemetría en el HUD =====
  // Misma forma que sensor_data de la API (base_model_bm.py)
  function pintar_telemetria({ batery = {}, gps = {}, lidar_distance = {}, acelerometer_gyroscope = {} }) {
    const alabeo = acelerometer_gyroscope.roll_angle;
    const cabeceo = acelerometer_gyroscope.pitch_angle;
    if (es_numero(alabeo)) hud_horizonte.style.setProperty('--giro', `${-alabeo}deg`);
    if (es_numero(cabeceo)) hud_horizonte.style.setProperty('--desplazamiento', `${limitar(cabeceo, -30, 30) * pixeles_por_grado_cabeceo}px`);
    const guinada = acelerometer_gyroscope.yaw_angle;
    document.getElementById('hud_rumbo').textContent = es_numero(guinada) ? String(Math.round(((guinada % 360) + 360) % 360)).padStart(3, '0') : '—';
    escribir_numero('hud_altitud', gps.altitude, 1);
    escribir_numero('hud_velocidad', es_numero(gps.speed) ? gps.speed * metros_por_segundo_a_km_h : null, 1);
    escribir_numero('hud_lidar', lidar_distance.distance, 2);
    const celdas = [batery.c1, batery.c2, batery.c3, batery.c4, batery.c5, batery.c6].filter(es_numero);
    const promedio = celdas.length ? celdas.reduce((suma, voltaje) => suma + voltaje, 0) / celdas.length : null;
    const carga = es_numero(promedio) ? limitar((promedio - voltaje_celda_minimo) / (voltaje_celda_maximo - voltaje_celda_minimo), 0, 1) * 100 : null;
    escribir_numero('hud_bateria', carga, 0);
  }

  // ===== Mandos =====
  let armado = false;
  let modo_actual = 'estabilizado';
  let temporizador_paro = null;

  function refrescar_mandos() {
    const listo = api_autenticada;
    boton_armar.disabled = !listo;
    selector_modo.disabled = !listo;
    boton_paro.disabled = !listo;
    botones_mando.forEach((boton) => {
      boton.disabled = !listo || (boton.hasAttribute('data-requiere_armado') && !armado);
    });
    boton_enviar_pid.disabled = !listo;
    boton_leer_pid.disabled = !listo;
    palancas.forEach((palanca) => palanca.classList.toggle('esta_deshabilitada', !listo || !armado));
  }

  function establecer_armado(valor) {
    armado = valor;
    boton_armar.setAttribute('aria-pressed', String(valor));
    texto_boton_armar.textContent = valor ? 'Desarmar motores' : 'Armar motores';
    insignia_armado.textContent = valor ? 'Armado' : 'Desarmado';
    insignia_armado.classList.toggle('esta_armado', valor);
    hud_armado.textContent = valor ? 'Armado' : 'Desarmado';
    hud_armado.parentElement.classList.toggle('esta_armado', valor);
    if (!valor) soltar_mandos();
    refrescar_mandos();
  }

  function establecer_modo(modo) {
    modo_actual = modo;
    hud_modo.textContent = nombres_modo[modo];
    const opcion = selector_modo.querySelector(`input[value="${modo}"]`);
    if (opcion) opcion.checked = true;
  }

  // El estado se actualiza al enviar; si el dron manda "estado_dron", ese valor manda
  boton_armar.addEventListener('click', () => {
    const accion = armado ? 'desarmar' : 'armar';
    if (enviar_comando(accion)) establecer_armado(!armado);
  });

  botones_mando.forEach((boton) => {
    boton.addEventListener('click', () => {
      if (!enviar_comando(boton.dataset.accion)) return;
      boton.classList.add('esta_enviado');
      setTimeout(() => boton.classList.remove('esta_enviado'), 900);
    });
  });

  selector_modo.addEventListener('change', (evento) => {
    const modo = evento.target.value;
    if (enviar_comando('modo', { modo })) establecer_modo(modo);
    else establecer_modo(modo_actual);
  });

  // Paro de emergencia: se dispara si se mantiene tiempo_paro_ms; soltar antes lo cancela
  boton_paro.style.setProperty('--tiempo_paro', `${tiempo_paro_ms}ms`);
  function iniciar_paro(evento) {
    if (boton_paro.disabled) return;
    evento.preventDefault();
    // Si la captura falla (puntero ya inactivo) el paro debe seguir funcionando
    try { if (evento.pointerId !== undefined) boton_paro.setPointerCapture(evento.pointerId); } catch (error) { /* sin captura */ }
    boton_paro.classList.add('esta_presionado');
    temporizador_paro = setTimeout(() => {
      boton_paro.classList.remove('esta_presionado');
      if (enviar_comando('paro_emergencia')) establecer_armado(false);
    }, tiempo_paro_ms);
  }
  function cancelar_paro() {
    clearTimeout(temporizador_paro);
    boton_paro.classList.remove('esta_presionado');
  }
  boton_paro.addEventListener('pointerdown', iniciar_paro);
  boton_paro.addEventListener('pointerup', cancelar_paro);
  boton_paro.addEventListener('pointercancel', cancelar_paro);
  boton_paro.addEventListener('lostpointercapture', cancelar_paro);
  // Con teclado: Enter o Espacio mantenidos sobre el botón
  boton_paro.addEventListener('keydown', (evento) => {
    if ((evento.key === 'Enter' || evento.key === ' ') && !evento.repeat) {
      evento.stopPropagation();
      iniciar_paro(evento);
    }
  });
  boton_paro.addEventListener('keyup', cancelar_paro);

  // ===== Movimiento =====
  // Cada eje va de -1 a 1. Potencia es velocidad vertical: al centro, el dron mantiene altura
  const ejes_palanca = { potencia: 0, guinada: 0, cabeceo: 0, alabeo: 0 };
  const ejes_teclado = { potencia: 0, guinada: 0, cabeceo: 0, alabeo: 0 };
  const teclas_mando = {
    w: ['potencia', 1], s: ['potencia', -1],
    d: ['guinada', 1], a: ['guinada', -1],
    arrowup: ['cabeceo', 1], arrowdown: ['cabeceo', -1],
    arrowright: ['alabeo', 1], arrowleft: ['alabeo', -1],
  };
  const teclas_pulsadas = new Set();
  let ultimo_envio_era_cero = true;

  function calcular_ejes() {
    const limite = Number(control_limite.value) / 100;
    const ejes = {};
    Object.keys(ejes_palanca).forEach((eje) => {
      const valor = limitar(ejes_palanca[eje] + ejes_teclado[eje], -1, 1) * limite;
      ejes[eje] = Math.round(valor * 100) / 100;
    });
    return ejes;
  }

  function pintar_ejes(ejes) {
    Object.entries(ejes).forEach(([eje, valor]) => {
      const relleno = document.getElementById(`barra_${eje}`);
      relleno.style.left = `${valor < 0 ? 50 + valor * 50 : 50}%`;
      relleno.style.width = `${Math.abs(valor) * 50}%`;
      document.getElementById(`valor_${eje}`).textContent = Math.round(valor * 100);
    });
  }

  // Envía a 10 Hz mientras haya mando, y un último paquete en cero al soltar
  function ciclo_movimiento() {
    const ejes = calcular_ejes();
    pintar_ejes(ejes);
    const es_cero = Object.values(ejes).every((valor) => valor === 0);
    if (!armado || (es_cero && ultimo_envio_era_cero)) return;
    if (enviar_api({ tipo: 'control', action: 'movimiento', maquina: maquina_actual, ejes })) {
      ultimo_envio_era_cero = es_cero;
    }
  }

  function mover_perilla(palanca, x, y) {
    const radio = palanca.clientWidth * 0.32;
    palanca.style.setProperty('--perilla_x', `${x * radio}px`);
    palanca.style.setProperty('--perilla_y', `${-y * radio}px`);
  }

  palancas.forEach((palanca) => {
    const eje_x = palanca.dataset.eje_x;
    const eje_y = palanca.dataset.eje_y;
    let puntero_activo = null;

    function leer_puntero(evento) {
      const caja = palanca.getBoundingClientRect();
      const radio = caja.width * 0.32;
      let dx = (evento.clientX - (caja.left + caja.width / 2)) / radio;
      let dy = (evento.clientY - (caja.top + caja.height / 2)) / radio;
      const distancia = Math.hypot(dx, dy);
      if (distancia > 1) { dx /= distancia; dy /= distancia; }
      ejes_palanca[eje_x] = dx;
      ejes_palanca[eje_y] = -dy;
      mover_perilla(palanca, dx, -dy);
    }

    function soltar() {
      puntero_activo = null;
      palanca.classList.remove('esta_activa');
      ejes_palanca[eje_x] = 0;
      ejes_palanca[eje_y] = 0;
      mover_perilla(palanca, 0, 0);
    }
    palanca.soltar = soltar;

    palanca.addEventListener('pointerdown', (evento) => {
      if (!armado || !api_autenticada) {
        registrar('Arma los motores para usar las palancas.', 'error');
        return;
      }
      puntero_activo = evento.pointerId;
      palanca.setPointerCapture(evento.pointerId);
      palanca.classList.add('esta_activa');
      leer_puntero(evento);
    });
    palanca.addEventListener('pointermove', (evento) => {
      if (evento.pointerId === puntero_activo) leer_puntero(evento);
    });
    palanca.addEventListener('pointerup', soltar);
    palanca.addEventListener('pointercancel', soltar);
    palanca.addEventListener('lostpointercapture', () => { if (puntero_activo !== null) soltar(); });
  });

  // Las perillas también muestran lo que se pulsa con el teclado
  function pintar_teclado() {
    palancas.forEach((palanca) => {
      if (palanca.classList.contains('esta_activa')) return;
      const x = ejes_teclado[palanca.dataset.eje_x];
      const y = ejes_teclado[palanca.dataset.eje_y];
      palanca.classList.toggle('esta_teclado', x !== 0 || y !== 0);
      mover_perilla(palanca, x, y);
    });
    teclas_guia.forEach((tecla) => {
      const nombre = { '↑': 'arrowup', '↓': 'arrowdown', '←': 'arrowleft', '→': 'arrowright' }[tecla.textContent] ?? tecla.textContent.toLowerCase();
      tecla.classList.toggle('esta_pulsada', teclas_pulsadas.has(nombre));
    });
  }

  function recalcular_teclado() {
    Object.keys(ejes_teclado).forEach((eje) => { ejes_teclado[eje] = 0; });
    teclas_pulsadas.forEach((tecla) => {
      const mando = teclas_mando[tecla];
      if (mando) ejes_teclado[mando[0]] += mando[1];
    });
    pintar_teclado();
  }

  function soltar_mandos() {
    teclas_pulsadas.clear();
    recalcular_teclado();
    palancas.forEach((palanca) => palanca.soltar?.());
  }

  function es_campo_de_texto(elemento) {
    if (!elemento) return false;
    if (elemento.isContentEditable || elemento.tagName === 'TEXTAREA' || elemento.tagName === 'SELECT') return true;
    return elemento.tagName === 'INPUT' && !['range', 'checkbox', 'radio'].includes(elemento.type);
  }

  document.addEventListener('keydown', (evento) => {
    if (!control_teclado.checked || es_campo_de_texto(evento.target) || evento.ctrlKey || evento.metaKey || evento.altKey) return;
    const tecla = evento.key.toLowerCase();
    if (tecla === ' ') {
      // Sobre un botón o control, Espacio conserva su función normal (activarlo)
      if (evento.target.closest?.('button, summary, a, input, label')) return;
      evento.preventDefault();
      soltar_mandos();
      return;
    }
    // Desarmado, las teclas hacen lo de siempre (desplazar la página, mover deslizadores)
    if (!teclas_mando[tecla] || !armado || !api_autenticada) return;
    // Armado, las flechas son mandos: no deben cambiar el modo de vuelo ni un deslizador enfocado
    evento.preventDefault();
    teclas_pulsadas.add(tecla);
    recalcular_teclado();
  });

  document.addEventListener('keyup', (evento) => {
    const tecla = evento.key.toLowerCase();
    if (teclas_pulsadas.delete(tecla)) recalcular_teclado();
  });

  // Hombre muerto: si la pestaña pierde el foco, el keyup nunca llega
  window.addEventListener('blur', soltar_mandos);
  document.addEventListener('visibilitychange', () => { if (document.hidden) soltar_mandos(); });
  control_teclado.addEventListener('change', () => { if (!control_teclado.checked) soltar_mandos(); });

  control_limite.addEventListener('input', () => {
    pintar_relleno(control_limite);
    valor_limite.textContent = `${control_limite.value} %`;
  });

  // ===== PID =====
  const valores_pid = structuredClone(valores_pid_base);
  let valores_pid_enviados = null;
  let eje_pid_actual = 'alabeo';
  const borrador_pid = leer_preferencia(clave_pid);
  if (borrador_pid) copiar_pid(borrador_pid, valores_pid);

  // Solo copia ejes y ganancias conocidos con valores numéricos (el borrador o el dron pueden traer otra cosa)
  function copiar_pid(origen, destino) {
    ejes_pid.forEach((eje) => {
      ganancias_pid.forEach((ganancia) => {
        const valor = origen?.[eje]?.[ganancia];
        if (es_numero(valor)) destino[eje][ganancia] = valor;
      });
    });
  }

  function eje_modificado(eje) {
    if (!valores_pid_enviados) return true;
    return ganancias_pid.some((ganancia) => valores_pid[eje][ganancia] !== valores_pid_enviados[eje][ganancia]);
  }

  function pintar_pid() {
    ganancias_pid.forEach((ganancia) => {
      const rango = document.getElementById(`rango_${ganancia}`);
      const numero = document.getElementById(`numero_${ganancia}`);
      rango.value = valores_pid[eje_pid_actual][ganancia];
      if (numero !== document.activeElement) numero.value = valores_pid[eje_pid_actual][ganancia];
      pintar_relleno(rango);
    });
    pestanas_pid.forEach((pestana) => {
      const activa = pestana.dataset.eje === eje_pid_actual;
      pestana.classList.toggle('pestana_pid_activa', activa);
      pestana.setAttribute('aria-selected', String(activa));
      pestana.classList.toggle('esta_modificada', valores_pid_enviados !== null && eje_modificado(pestana.dataset.eje));
    });
    tabla_pid_cuerpo.replaceChildren(...ejes_pid.map((eje) => {
      const fila = document.createElement('tr');
      fila.classList.toggle('esta_activa', eje === eje_pid_actual);
      fila.classList.toggle('esta_modificada', valores_pid_enviados !== null && eje_modificado(eje));
      const nombre = document.createElement('td');
      nombre.textContent = nombres_ejes_pid[eje];
      fila.append(nombre, ...ganancias_pid.map((ganancia) => {
        const celda = document.createElement('td');
        celda.textContent = valores_pid[eje][ganancia].toFixed(3);
        return celda;
      }));
      return fila;
    }));
    const sincronizado = valores_pid_enviados !== null && !ejes_pid.some(eje_modificado);
    estado_pid.textContent = sincronizado ? 'Sincronizado' : valores_pid_enviados ? 'Cambios sin enviar' : 'Sin enviar';
    estado_pid.classList.toggle('esta_sincronizado', sincronizado);
  }

  function cambiar_ganancia(ganancia, valor, minimo, maximo) {
    if (!es_numero(valor)) return;
    valores_pid[eje_pid_actual][ganancia] = Math.round(limitar(valor, minimo, maximo) * 1000) / 1000;
    guardar_preferencia(clave_pid, valores_pid);
    pintar_pid();
  }

  ganancias_pid.forEach((ganancia) => {
    const rango = document.getElementById(`rango_${ganancia}`);
    const numero = document.getElementById(`numero_${ganancia}`);
    const minimo = Number(rango.min);
    const maximo = Number(rango.max);
    rango.addEventListener('input', () => cambiar_ganancia(ganancia, Number(rango.value), minimo, maximo));
    numero.addEventListener('input', () => {
      if (numero.value !== '') cambiar_ganancia(ganancia, Number(numero.value), minimo, maximo);
    });
    // Al salir del campo se muestra el valor ya limitado al rango
    numero.addEventListener('blur', () => { numero.value = valores_pid[eje_pid_actual][ganancia]; });
  });

  pestanas_pid.forEach((pestana) => {
    pestana.addEventListener('click', () => {
      eje_pid_actual = pestana.dataset.eje;
      pintar_pid();
    });
    // Flechas izquierda y derecha entre pestañas (patrón ARIA de tablist)
    pestana.addEventListener('keydown', (evento) => {
      if (evento.key !== 'ArrowRight' && evento.key !== 'ArrowLeft') return;
      evento.preventDefault();
      evento.stopPropagation();
      const indice = ejes_pid.indexOf(eje_pid_actual);
      const siguiente = (indice + (evento.key === 'ArrowRight' ? 1 : -1) + ejes_pid.length) % ejes_pid.length;
      eje_pid_actual = ejes_pid[siguiente];
      pintar_pid();
      pestanas_pid[siguiente].focus();
    });
  });

  boton_enviar_pid.addEventListener('click', () => {
    if (!enviar_comando('pid', { pid: structuredClone(valores_pid) })) return;
    valores_pid_enviados = structuredClone(valores_pid);
    pintar_pid();
  });

  boton_leer_pid.addEventListener('click', () => enviar_comando('pid_leer'));

  boton_restablecer_pid.addEventListener('click', () => {
    valores_pid[eje_pid_actual] = { ...valores_pid_base[eje_pid_actual] };
    guardar_preferencia(clave_pid, valores_pid);
    pintar_pid();
    registrar(`PID de ${nombres_ejes_pid[eje_pid_actual].toLowerCase()} restablecido a los valores de referencia.`);
  });

  // Lo que manda el dron es lo que tiene aplicado: queda como referencia de "enviado"
  function recibir_pid(pid) {
    copiar_pid(pid, valores_pid);
    valores_pid_enviados = structuredClone(valores_pid);
    guardar_preferencia(clave_pid, valores_pid);
    pintar_pid();
    registrar('PID recibido del dron.');
  }

  boton_limpiar_registro.addEventListener('click', vaciar_registro);

  // ===== Arranque =====
  vaciar_registro();
  document.querySelectorAll('.deslizador').forEach(pintar_relleno);
  establecer_modo(modo_actual);
  establecer_armado(false);
  pintar_pid();
  pintar_ejes(calcular_ejes());
  registrar(`Sesión iniciada como ${sesion.nombre || 'Usuario'}.`);
  cargar_maquinas();
  conectar_api();
  setInterval(ciclo_movimiento, periodo_movimiento_ms);
  setInterval(() => {
    refrescar_estado_enlace();
    revisar_video();
  }, 1000);
}
