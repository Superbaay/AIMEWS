// Panel de control. Depende de ../main.js (api_url, clave_token y el observador de .revelar),
// de sesion_panel.js (leer_sesion, salir_del_panel, mostrar_datos_sesion, formatear_hora y es_numero)
// y de enlace_ws.js (crear_enlace_ws)
// La API expone /ws y espera el token en el primer mensaje: {"token": "..."} (main.py)
const ws_url = `${api_url.replace(/^http/, 'ws')}/ws`;
const metros_por_segundo_a_km_h = 3.6;
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

  let ultimo_mensaje_dron_ms = 0;
  let estado_enlace_actual = '';
  let temporizador_autoprueba = null;

  // ===== Datos del usuario =====
  const nombre_usuario = sesion.nombre || 'Usuario';
  mostrar_datos_sesion(sesion);

  // ===== Utilidades =====
  function escribir_valor(id, valor, decimales) {
    document.getElementById(id).textContent = es_numero(valor) ? valor.toFixed(decimales) : '—';
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
    if (estado_socket === WebSocket.CONNECTING) return 'conectando';
    // null (sin socket), CLOSING o CLOSED: no hay camino hasta el dron
    if (estado_socket !== WebSocket.OPEN) return 'desconectado';
    // Socket abierto: la API responde, pero el dron solo cuenta si habló hace poco.
    // El dron manda telemetría 10 veces por segundo, así que 5 s de silencio es un corte real
    const dron_reciente = ultimo_mensaje_dron_ms > 0 && ahora_ms - ultimo_mensaje_dron_ms <= tiempo_max_sin_senal_ms;
    return dron_reciente ? 'en_linea' : 'sin_senal';
  }

  function refrescar_estado_enlace() {
    const estado = calcular_estado_enlace(enlace.estado, ultimo_mensaje_dron_ms, Date.now());
    if (estado === estado_enlace_actual) return;
    estado_enlace_actual = estado;
    estado_enlace.className = `estado_enlace estado_enlace_${estado}`;
    estado_enlace_texto.textContent = textos_estado_enlace[estado];
    const autoprueba_en_curso = boton_autoprueba.classList.contains('esta_cargando');
    boton_autoprueba.disabled = autoprueba_en_curso || estado !== 'en_linea';
  }

  // ===== WebSocket =====
  // enlace_ws.js manda el token en el primer mensaje, reconecta con espera creciente y sale del panel si la API rechaza la sesión
  const enlace = crear_enlace_ws({
    url: ws_url,
    token: sesion.token,
    al_conectar: refrescar_estado_enlace,
    al_abrir: () => {
      registrar_evento('Conectado a la API. Enviando sesión…');
      refrescar_estado_enlace();
    },
    al_mensaje: procesar_mensaje,
    al_mensaje_invalido: () => registrar_evento('Se recibió un mensaje con formato inválido.', true),
    al_cerrar: refrescar_estado_enlace,
    al_rechazo: () => registrar_evento('La API rechazó la sesión. Redirigiendo…', true),
    al_reintento: (espera_ms) => registrar_evento(`Conexión cerrada. Reintento en ${Math.round(espera_ms / 1000)} s.`, true),
  });

  // La API (drone_ws_rt) reenvía el comando al dron con manager.send_command y devuelve el mismo mensaje con api_response
  function enviar_comando(accion) {
    return enlace.enviar({ tipo: 'comando', action: accion });
  }

  // Mensajes del saludo inicial de la API: {"message": "..."} sin campo tipo
  function procesar_mensaje_api(texto) {
    if (texto === 'Unauthorized' || texto.startsWith('WebSocket error')) {
      enlace.marcar_rechazada();
      registrar_evento(`La API rechazó la sesión: ${texto}`, true);
      return;
    }
    registrar_evento(`API: ${texto}`);
  }

  function procesar_mensaje(mensaje) {
    if (!mensaje.tipo && typeof mensaje.message === 'string') {
      procesar_mensaje_api(mensaje.message);
      return;
    }
    switch (mensaje.tipo) {
      case 'estado_dron':
        ultimo_mensaje_dron_ms = Date.now();
        if (mensaje.version) titulo_dron.textContent = `Dron SAMATI ${mensaje.version}`;
        break;
      case 'telemetria':
        ultimo_mensaje_dron_ms = Date.now();
        pintar_telemetria(mensaje.datos ?? mensaje);
        break;
      case 'autoprueba':
        ultimo_mensaje_dron_ms = Date.now();
        // resultado tiene la forma de auto_test_machine: {gps: true, camera: false, ...}
        if (mensaje.resultado) pintar_autoprueba_completa(mensaje.resultado);
        else pintar_resultado_prueba(mensaje.componente, mensaje.estado);
        break;
      case 'comando':
        registrar_evento(`Comando ${mensaje.action}: ${mensaje.api_response?.message ?? 'sin respuesta'}`);
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
  // Los datos tienen la forma del BaseModel sensor_data de la API (base_model_bm.py)
  function pintar_telemetria({ batery = {}, gps = {}, lidar_distance = {}, magnetometer = {}, acelerometer_gyroscope = {}, barometer = {} }) {
    const celdas = [batery.c1, batery.c2, batery.c3, batery.c4, batery.c5, batery.c6];
    ultima_telemetria.textContent = formatear_hora(new Date());
    escribir_valor('bateria_voltaje', batery.voltage, 1);
    const carga = estimar_carga(celdas);
    escribir_valor('bateria_porcentaje', carga, 0);
    // TODO(human): llenar el anillo de carga (#anillo_carga) con `carga` (0 a 100, o null si no hay celdas válidas)
    escribir_valor('bateria_corriente', batery.current, 1);
    escribir_valor('bateria_temperatura', batery.temperature, 1);
    escribir_valor('imu_cabeceo', acelerometer_gyroscope.pitch_angle, 1);
    escribir_valor('imu_alabeo', acelerometer_gyroscope.roll_angle, 1);
    escribir_valor('imu_guinada', acelerometer_gyroscope.yaw_angle, 1);
    escribir_valor('imu_vel_cabeceo', acelerometer_gyroscope.pitch_velocity, 1);
    escribir_valor('imu_vel_alabeo', acelerometer_gyroscope.roll_velocity, 1);
    escribir_valor('imu_vel_guinada', acelerometer_gyroscope.yaw_velocity, 1);
    escribir_valor('barometro_presion', barometer.pressure, 1);
    escribir_valor('barometro_temperatura', barometer.temperature, 1);
    escribir_valor('barometro_altitud', barometer.altitude, 1);
    escribir_valor('magnetometro_x', magnetometer.x, 2);
    escribir_valor('magnetometro_y', magnetometer.y, 2);
    escribir_valor('magnetometro_z', magnetometer.z, 2);
    escribir_valor('telemetria_latitud', gps.latitude, 6);
    escribir_valor('telemetria_longitud', gps.longitude, 6);
    escribir_valor('telemetria_altitud', gps.altitude, 1);
    // La API manda gps.speed en m/s y el panel lo muestra en km/h
    const velocidad = typeof gps.speed === 'number' ? gps.speed * metros_por_segundo_a_km_h : null;
    escribir_valor('telemetria_velocidad', velocidad, 1);
    escribir_valor('gps_satelites', gps.satellites, 0);
    escribir_valor('gps_satelites_vista', gps.satellites_in_view, 0);
    escribir_valor('gps_hdop', gps.hdop, 1);
    document.getElementById('gps_fix').textContent = texto_fix_gps(gps);
    escribir_valor('lidar_distancia', lidar_distance.distance, 2);
    pintar_celdas(celdas);
  }

  // fix_type del GPS (NMEA GSA): 1 sin fix, 2 = 2D (sin altitud fiable), 3 = 3D
  function texto_fix_gps(gps) {
    if (typeof gps.fix !== 'boolean') return '—';
    if (!gps.fix) return 'Sin fix';
    return gps.fix_type === 3 ? '3D' : gps.fix_type === 2 ? '2D' : 'Fix';
  }

  // batery_6s no trae porcentaje: se estima con el promedio de las celdas entre el mínimo y el máximo de una LiPo.
  // Es lineal, así que es aproximado (la curva real de descarga es plana en el centro)
  function estimar_carga(voltajes) {
    const validos = voltajes.filter(es_numero);
    if (validos.length === 0) return null;
    const promedio = validos.reduce((suma, voltaje) => suma + voltaje, 0) / validos.length;
    const nivel = (promedio - voltaje_celda_minimo) / (voltaje_celda_maximo - voltaje_celda_minimo);
    return Math.min(1, Math.max(0, nivel)) * 100;
  }

  function pintar_celdas(voltajes) {
    celdas_bateria.forEach((celda, indice) => {
      const voltaje = voltajes[indice];
      const es_valido = es_numero(voltaje);
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

  // Cada data-componente del HTML coincide con un campo bool de auto_test_machine
  function pintar_autoprueba_completa(resultado) {
    filas_prueba.forEach((fila) => {
      const valor = resultado[fila.dataset.componente];
      if (typeof valor === 'boolean') pintar_resultado_prueba(fila.dataset.componente, valor ? 'correcto' : 'falla');
    });
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
  enlace.conectar();
  setInterval(refrescar_estado_enlace, 1000);
}
