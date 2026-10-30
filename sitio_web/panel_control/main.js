// Panel de control. Depende de ../main.js (api_url, clave_token y el observador de .revelar)
// y de sesion_panel.js (leer_sesion, salir_del_panel y mostrar_datos_sesion)
const ws_url = `${api_url.replace(/^http/, 'ws')}/ws/panel`;
const tiempo_max_sin_senal_ms = 5000;
const tiempo_max_autoprueba_ms = 30000;
const voltaje_celda_minimo = 3.0;
const voltaje_celda_maximo = 4.2;
const voltaje_celda_bajo = 3.5;

// ===== Sesión =====
const sesion = leer_sesion();
if (!sesion) {
  // salir_del_panel();
} else {
  iniciar_panel();
}

function iniciar_panel() {
  const estado_enlace = document.getElementById('estado_enlace');
  const estado_enlace_texto = document.getElementById('estado_enlace_texto');
  const titulo_dron = document.getElementById('titulo_dron');
  const ultima_telemetria = document.getElementById('ultima_telemetria');
  const boton_autoprueba = document.getElementById('boton_autoprueba');
  const texto_boton_autoprueba = document.getElementById('texto_boton_autoprueba');
  const resumen_prueba = document.getElementById('resumen_prueba');
  const filas_prueba = document.querySelectorAll('#lista_prueba [data-componente]');
  const celdas_bateria = document.querySelectorAll('#celdas_bateria .celda_bateria');

  const textos_estado_enlace = {
    desconectado: 'Sin conexión con la API',
    conectando: 'Conectando…',
    sin_senal: 'API conectada · Dron sin señal',
    en_linea: 'Dron en línea',
  };
  const textos_estado_prueba = {
    pendiente: 'Pendiente',
    en_curso: 'En curso',
    correcto: 'Correcto',
    falla: 'Falla',
    sin_respuesta: 'Sin respuesta',
  };

  let socket = null;
  let intentos_reconexion = 0;
  let temporizador_reconexion = null;
  let ultimo_mensaje_dron_ms = 0;
  let estado_enlace_actual = '';
  let temporizador_autoprueba = null;

  // ===== Datos del usuario =====
  const nombre_usuario = sesion.nombre || 'Usuario';
  mostrar_datos_sesion(sesion);

  // ===== Utilidades =====
  function formatear_hora(fecha) {
    return fecha.toLocaleTimeString('es-MX', { hour12: false });
  }

  function escribir_valor(id, valor, decimales) {
    const elemento = document.getElementById(id);
    const es_numero = typeof valor === 'number' && Number.isFinite(valor);
    elemento.textContent = es_numero ? valor.toFixed(decimales) : '—';
  }

  // El panel ya no tiene tarjeta de registro: los eventos del enlace van a la consola del navegador
  function registrar_evento(texto, es_error = false) {
    const linea = `[${formatear_hora(new Date())}] ${texto}`;
    if (es_error) console.warn(linea);
    else console.info(linea);
  }

  // ===== Estado del enlace =====
  // Devuelve una clave de textos_estado_enlace: 'desconectado', 'conectando', 'sin_senal' o 'en_linea'.
  // estado_socket es socket.readyState (WebSocket.CONNECTING, OPEN, CLOSING o CLOSED) o null si no hay socket.
  // ultimo_mensaje_dron_ms es Date.now() del último mensaje del dron (0 si nunca llegó ninguno).
  function calcular_estado_enlace(estado_socket, ultimo_mensaje_dron_ms, ahora_ms) {
    // TODO(human): decide el estado del enlace a partir del socket y del tiempo desde el último mensaje del dron
    return 'desconectado';
  }

  function refrescar_estado_enlace() {
    const estado_socket = socket ? socket.readyState : null;
    const estado = calcular_estado_enlace(estado_socket, ultimo_mensaje_dron_ms, Date.now());
    if (estado === estado_enlace_actual) return;
    estado_enlace_actual = estado;
    estado_enlace.className = `estado_enlace estado_enlace_${estado}`;
    estado_enlace_texto.textContent = textos_estado_enlace[estado];
    const autoprueba_en_curso = boton_autoprueba.classList.contains('esta_cargando');
    boton_autoprueba.disabled = autoprueba_en_curso || estado !== 'en_linea';
  }

  // ===== WebSocket =====
  // El navegador no permite cabeceras en WebSocket, así que el JWT viaja como parámetro de la URL
  function conectar_socket() {
    clearTimeout(temporizador_reconexion);
    if (socket && socket.readyState <= WebSocket.OPEN) return;

    socket = new WebSocket(`${ws_url}?token=${encodeURIComponent(sesion.token)}`);
    refrescar_estado_enlace();

    socket.addEventListener('open', () => {
      intentos_reconexion = 0;
      registrar_evento('Conectado a la API.');
      refrescar_estado_enlace();
    });

    socket.addEventListener('message', (evento) => {
      try {
        procesar_mensaje(JSON.parse(evento.data));
      } catch (error) {
        registrar_evento('Se recibió un mensaje con formato inválido.', true);
      }
    });

    socket.addEventListener('close', (evento) => {
      refrescar_estado_enlace();
      // 1008 (policy violation): la API rechazó el token
      if (evento.code === 1008) {
        registrar_evento('La API rechazó la sesión. Redirigiendo…', true);
        setTimeout(salir_del_panel, 1200);
        return;
      }
      const espera_ms = Math.min(30000, 1000 * 2 ** intentos_reconexion);
      intentos_reconexion += 1;
      registrar_evento(`Conexión cerrada. Reintento en ${Math.round(espera_ms / 1000)} s.`, true);
      temporizador_reconexion = setTimeout(conectar_socket, espera_ms);
    });
  }

  function enviar_comando(accion) {
    if (!socket || socket.readyState !== WebSocket.OPEN) return false;
    socket.send(JSON.stringify({ tipo: 'comando', accion }));
    return true;
  }

  function procesar_mensaje(mensaje) {
    switch (mensaje.tipo) {
      case 'estado_dron':
        ultimo_mensaje_dron_ms = Date.now();
        if (mensaje.version) titulo_dron.textContent = `Dron SAMATI ${mensaje.version}`;
        break;
      case 'telemetria':
        ultimo_mensaje_dron_ms = Date.now();
        pintar_telemetria(mensaje);
        break;
      case 'autoprueba':
        ultimo_mensaje_dron_ms = Date.now();
        pintar_resultado_prueba(mensaje.componente, mensaje.estado);
        break;
      case 'error':
        registrar_evento(mensaje.detalle || 'La API reportó un error.', true);
        break;
      default:
        registrar_evento(`Mensaje no reconocido: ${mensaje.tipo}`, true);
    }
    refrescar_estado_enlace();
  }

  // ===== Telemetría =====
  // acelerometer_gyroscope y barometer llegan con los nombres de los BaseModel de la API (base_model_bm.py)
  function pintar_telemetria({ bateria = {}, gps = {}, altitud, velocidad, rumbo, acelerometer_gyroscope = {}, barometer = {} }) {
    ultima_telemetria.textContent = formatear_hora(new Date());
    escribir_valor('bateria_voltaje', bateria.voltaje, 1);
    escribir_valor('bateria_porcentaje', bateria.porcentaje, 0);
    escribir_valor('imu_cabeceo', acelerometer_gyroscope.pitch_angle, 1);
    escribir_valor('imu_alabeo', acelerometer_gyroscope.roll_angle, 1);
    escribir_valor('imu_guinada', acelerometer_gyroscope.yaw_angle, 1);
    escribir_valor('imu_vel_cabeceo', acelerometer_gyroscope.pitch_velocity, 1);
    escribir_valor('imu_vel_alabeo', acelerometer_gyroscope.roll_velocity, 1);
    escribir_valor('imu_vel_guinada', acelerometer_gyroscope.yaw_velocity, 1);
    escribir_valor('barometro_presion', barometer.pressure, 1);
    escribir_valor('barometro_temperatura', barometer.temperature, 1);
    escribir_valor('barometro_altitud', barometer.altitude, 1);
    escribir_valor('telemetria_latitud', gps.latitud, 6);
    escribir_valor('telemetria_longitud', gps.longitud, 6);
    escribir_valor('telemetria_satelites', gps.satelites, 0);
    escribir_valor('telemetria_altitud', altitud, 1);
    escribir_valor('telemetria_velocidad', velocidad, 1);
    escribir_valor('telemetria_rumbo', rumbo, 0);
    if (Array.isArray(bateria.celdas)) pintar_celdas(bateria.celdas);
  }

  function pintar_celdas(voltajes) {
    celdas_bateria.forEach((celda, indice) => {
      const voltaje = voltajes[indice];
      const es_valido = typeof voltaje === 'number' && Number.isFinite(voltaje);
      const rango = voltaje_celda_maximo - voltaje_celda_minimo;
      const nivel = es_valido ? Math.min(1, Math.max(0, (voltaje - voltaje_celda_minimo) / rango)) : 0;
      celda.style.setProperty('--nivel', nivel);
      celda.classList.toggle('celda_bateria_baja', es_valido && voltaje < voltaje_celda_bajo);
      celda.querySelector('.celda_bateria_valor').textContent = es_valido ? voltaje.toFixed(2) : '—';
    });
  }

  // ===== Autoprueba =====
  function pintar_resultado_prueba(componente, estado) {
    const fila = document.querySelector(`#lista_prueba [data-componente="${componente}"]`);
    if (!fila || !textos_estado_prueba[estado]) return;
    const etiqueta = fila.querySelector('.estado_prueba');
    etiqueta.className = `estado_prueba estado_prueba_${estado}`;
    etiqueta.textContent = textos_estado_prueba[estado];
    etiqueta.dataset.estado = estado;
    if (estado === 'falla') registrar_evento(`Autoprueba: falla en ${fila.firstElementChild.textContent}.`, true);
    revisar_fin_autoprueba();
  }

  function establecer_autoprueba_en_curso(en_curso) {
    boton_autoprueba.classList.toggle('esta_cargando', en_curso);
    texto_boton_autoprueba.textContent = en_curso ? 'Probando…' : 'Iniciar autoprueba';
    refrescar_estado_enlace();
    boton_autoprueba.disabled = en_curso || estado_enlace_actual !== 'en_linea';
  }

  function revisar_fin_autoprueba() {
    if (!boton_autoprueba.classList.contains('esta_cargando')) return;
    const estados = [...filas_prueba].map((fila) => fila.querySelector('.estado_prueba').dataset.estado);
    if (estados.includes('en_curso')) return;
    terminar_autoprueba(estados);
  }

  function terminar_autoprueba(estados) {
    clearTimeout(temporizador_autoprueba);
    const correctos = estados.filter((estado) => estado === 'correcto').length;
    resumen_prueba.textContent = `${correctos}/${estados.length} correctos · ${formatear_hora(new Date())}`;
    registrar_evento(`Autoprueba terminada: ${correctos} de ${estados.length} componentes correctos.`, correctos < estados.length);
    establecer_autoprueba_en_curso(false);
  }

  boton_autoprueba.addEventListener('click', () => {
    if (!enviar_comando('iniciar_autoprueba')) return;
    filas_prueba.forEach((fila) => pintar_resultado_prueba(fila.dataset.componente, 'en_curso'));
    resumen_prueba.textContent = 'En curso';
    registrar_evento('Autoprueba solicitada al dron.');
    establecer_autoprueba_en_curso(true);
    temporizador_autoprueba = setTimeout(() => {
      filas_prueba.forEach((fila) => {
        if (fila.querySelector('.estado_prueba').dataset.estado === 'en_curso') {
          pintar_resultado_prueba(fila.dataset.componente, 'sin_respuesta');
        }
      });
    }, tiempo_max_autoprueba_ms);
  });

  // ===== Arranque =====
  registrar_evento(`Sesión iniciada como ${nombre_usuario}.`);
  conectar_socket();
  setInterval(refrescar_estado_enlace, 1000);
}
