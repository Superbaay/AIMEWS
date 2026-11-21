// Firmware OTA. Depende de ../../main.js (api_url, clave_token y el observador de .revelar),
// ../sesion_panel.js (sesión, pedir_api, enviar_json, fechas, utilidades) y ../enlace_ws.js (crear_enlace_ws).
// API: API/Routers_BM/ota_rt.py (todo solo O5-1) y GET /ws/drones (drones conectados y sus acciones).
// El avance llega por el WebSocket del panel como {"tipo": "ota", "estado": ..., "machine": ...}.
const rol_ota = 'O5-1';
const tamano_maximo_firmware = 4 * 1024 * 1024;   // partición ota_0/ota_1 del firmware
const ws_url = `${api_url.replace(/^http/, 'ws')}/ws`;
const textos_estado_ota = {
  enviado: 'Orden enviada al dron',
  descargando: 'Descargando e instalando',
  instalado: 'Instalado, reiniciando',
  confirmado: 'Versión nueva confirmada',
  error: 'Falló la actualización',
};

// La página solo existe para O5-1: sin sesión se va al login y con otro rol al Home del panel
// (la barra lateral tampoco muestra "OTA" a otros roles, y la API responde 403 de todos modos)
const sesion = leer_sesion();
if (!sesion) {
  salir_del_panel();
} else if (sesion.rol !== rol_ota) {
  window.location.replace(new URL('../index.html', location.href).href);
} else {
  iniciar_ota();
}

function iniciar_ota() {
  const aviso_permisos = document.getElementById('aviso_permisos');
  const gestion_ota = document.getElementById('gestion_ota');
  const total_drones = document.getElementById('total_drones');
  // Subir
  const formulario_subir = document.getElementById('formulario_subir');
  const zona_archivo = document.getElementById('zona_archivo');
  const zona_archivo_titulo = document.getElementById('zona_archivo_titulo');
  const campo_archivo = document.getElementById('campo_archivo');
  const campo_notas = document.getElementById('campo_notas');
  const mensaje_error_subir = document.getElementById('mensaje_error_subir');
  const mensaje_error_subir_texto = document.getElementById('mensaje_error_subir_texto');
  const boton_subir = document.getElementById('boton_subir');
  const nota_subir = document.getElementById('nota_subir');
  // Desplegar
  const formulario_desplegar = document.getElementById('formulario_desplegar');
  const campo_firmware = document.getElementById('campo_firmware');
  const campo_dron = document.getElementById('campo_dron');
  const boton_desplegar = document.getElementById('boton_desplegar');
  const nota_desplegar = document.getElementById('nota_desplegar');
  const progreso_ota = document.getElementById('progreso_ota');
  const progreso_ota_estado = document.getElementById('progreso_ota_estado');
  const progreso_ota_porcentaje = document.getElementById('progreso_ota_porcentaje');
  const progreso_ota_barra = document.getElementById('progreso_ota_barra');
  const progreso_ota_relleno = document.getElementById('progreso_ota_relleno');
  const registro_ota = document.getElementById('registro_ota');
  // Lista
  const estado_lista = document.getElementById('estado_lista');
  const lista_firmware = document.getElementById('lista_firmware');
  const plantilla_firmware = document.getElementById('plantilla_firmware');

  let firmwares = [];
  let drones = [];
  let archivo_elegido = null;
  let dron_en_actualizacion = null;

  mostrar_datos_sesion(sesion);
  aviso_permisos.hidden = true;
  gestion_ota.hidden = false;

  // ===== Utilidades =====
  function formatear_tamano(bytes) {
    return bytes >= 1024 * 1024 ? `${(bytes / 1024 / 1024).toFixed(2)} MB` : `${Math.round(bytes / 1024)} KB`;
  }

  function opcion(valor, texto) {
    const elemento = document.createElement('option');
    elemento.value = valor;
    elemento.textContent = texto;
    return elemento;
  }

  function registrar(texto, es_error = false) {
    const linea = document.createElement('li');
    linea.textContent = `[${formatear_hora(new Date())}] ${texto}`;
    linea.classList.toggle('registro_ota_error', es_error);
    registro_ota.prepend(linea);
  }

  // ===== Archivo =====
  function elegir_archivo(archivo) {
    mensaje_error_subir.hidden = true;
    mostrar_resultado(nota_subir, '');
    archivo_elegido = null;
    zona_archivo.classList.remove('tiene_archivo');
    zona_archivo_titulo.textContent = 'Arrastra el .bin aquí o haz clic para elegirlo';
    if (!archivo) return;
    if (!archivo.name.toLowerCase().endsWith('.bin')) {
      mostrar_error_subir('Elige el archivo .bin que genera la compilación (build/Dron_telemetria_ws.bin).');
      return;
    }
    if (archivo.size > tamano_maximo_firmware) {
      mostrar_error_subir('El firmware pasa de 4 MB: no cabe en la partición OTA.');
      return;
    }
    archivo_elegido = archivo;
    zona_archivo.classList.add('tiene_archivo');
    zona_archivo_titulo.textContent = `${archivo.name} · ${formatear_tamano(archivo.size)}`;
  }

  function mostrar_error_subir(texto) {
    mensaje_error_subir_texto.textContent = texto;
    mensaje_error_subir.hidden = false;
  }

  campo_archivo.addEventListener('change', () => elegir_archivo(campo_archivo.files[0]));
  ['dragenter', 'dragover'].forEach((evento) => zona_archivo.addEventListener(evento, (e) => {
    e.preventDefault();
    zona_archivo.classList.add('esta_arrastrando');
  }));
  ['dragleave', 'drop'].forEach((evento) => zona_archivo.addEventListener(evento, () => zona_archivo.classList.remove('esta_arrastrando')));
  zona_archivo.addEventListener('drop', (e) => {
    e.preventDefault();
    elegir_archivo(e.dataTransfer.files[0]);
  });

  formulario_subir.addEventListener('submit', (evento) => {
    evento.preventDefault();
    if (!archivo_elegido) {
      mostrar_error_subir('Primero elige el archivo .bin.');
      return;
    }
    con_boton_ocupado(boton_subir, async () => {
      // multipart/form-data: el navegador pone el Content-Type con su boundary, por eso no se fija aquí
      const datos = new FormData();
      datos.append('file', archivo_elegido);
      const notas = texto_opcional(campo_notas);
      if (notas) datos.append('notes', notas);
      try {
        const subido = await pedir_api('/ota/upload', { method: 'POST', body: datos });
        mostrar_resultado(nota_subir, `Versión ${subido.version} subida (${formatear_tamano(subido.size)}).`);
        campo_archivo.value = '';
        campo_notas.value = '';
        elegir_archivo(null);
        await cargar_firmwares(subido.number);
      } catch (error) {
        if (error.message === 'sesion_invalida') return;
        const textos = {
          413: 'El firmware pasa de 4 MB: no cabe en la partición OTA.',
          415: 'El archivo no es un firmware de ESP32-S3 válido (usa build/<proyecto>.bin, no el bootloader).',
        };
        mostrar_error_subir(textos[error.estado] ?? error.message);
      }
    });
  });

  // ===== Firmwares =====
  async function cargar_firmwares(numero_a_elegir = null) {
    estado_lista.hidden = false;
    estado_lista.textContent = 'Cargando firmwares…';
    try {
      firmwares = await pedir_api('/ota/all');
      pintar_firmwares();
      pintar_selector_firmware(numero_a_elegir);
    } catch (error) {
      if (error.message === 'sesion_invalida') return;
      estado_lista.textContent = error.message;
    }
  }

  function pintar_firmwares() {
    lista_firmware.replaceChildren();
    estado_lista.hidden = firmwares.length > 0;
    estado_lista.textContent = 'Aún no hay firmwares subidos.';
    firmwares.forEach((firmware) => {
      const fila = plantilla_firmware.content.firstElementChild.cloneNode(true);
      fila.classList.toggle('tarjeta_firmware_activa', String(firmware.number) === campo_firmware.value);
      fila.querySelector('.insignia_version').textContent = `v${firmware.version}`;
      fila.querySelector('.tarjeta_firmware_nombre').textContent = `#${firmware.number} · ${firmware.project_name}`;
      fila.querySelector('.tarjeta_firmware_detalle').textContent =
        `${formatear_tamano(firmware.size)} · ${formatear_fecha(firmware.date)} · ${firmware.onwer ?? '—'} · sha256 ${firmware.sha256.slice(0, 12)}`;
      fila.querySelector('.tarjeta_firmware_notas').textContent = firmware.notes ?? '';
      const boton_borrar = fila.querySelector('.tarjeta_firmware_borrar');
      boton_borrar.setAttribute('aria-label', `Eliminar firmware ${firmware.version}`);
      boton_borrar.addEventListener('click', () => borrar_firmware(firmware, boton_borrar));
      lista_firmware.append(fila);
    });
  }

  function pintar_selector_firmware(numero_a_elegir) {
    const anterior = numero_a_elegir ?? campo_firmware.value;
    campo_firmware.replaceChildren(...(firmwares.length
      ? firmwares.map((firmware) => opcion(firmware.number, `v${firmware.version} · #${firmware.number}`))
      : [opcion('', 'Sin firmwares')]));
    if (firmwares.some((firmware) => String(firmware.number) === String(anterior))) campo_firmware.value = String(anterior);
    refrescar_boton_desplegar();
  }

  async function borrar_firmware(firmware, boton) {
    if (!confirm(`¿Eliminar el firmware v${firmware.version} (#${firmware.number})? El archivo se borra del servidor.`)) return;
    await con_boton_ocupado(boton, async () => {
      try {
        await pedir_api(`/ota/number?number=${firmware.number}`, { method: 'DELETE' });
        await cargar_firmwares();
      } catch (error) {
        if (error.message !== 'sesion_invalida') estado_lista.textContent = error.message;
      }
    });
  }

  campo_firmware.addEventListener('change', () => {
    pintar_firmwares();
    refrescar_boton_desplegar();
  });

  // ===== Drones =====
  // Solo se ofrecen los que declaran "ota" en estado_dron.acciones (firmware 0.3.0 o posterior)
  async function cargar_drones() {
    try {
      const conectados = await pedir_api('/ws/drones');
      drones = conectados.filter((dron) => (dron.acciones ?? []).includes('ota'));
      total_drones.textContent = drones.length;
      const anterior = campo_dron.value;
      campo_dron.replaceChildren(...(drones.length
        ? drones.map((dron) => opcion(dron.machine, `${dron.machine} · v${dron.version ?? '?'}`))
        : [opcion('', 'Ningún dron conectado con OTA')]));
      if (drones.some((dron) => dron.machine === anterior)) campo_dron.value = anterior;
    } catch (error) {
      if (error.message === 'sesion_invalida') return;
      campo_dron.replaceChildren(opcion('', 'No se pudo leer la lista'));
    }
    refrescar_boton_desplegar();
  }

  function refrescar_boton_desplegar() {
    boton_desplegar.disabled = !campo_firmware.value || !campo_dron.value || dron_en_actualizacion !== null;
  }

  formulario_desplegar.addEventListener('submit', (evento) => {
    evento.preventDefault();
    const firmware = firmwares.find((candidato) => String(candidato.number) === campo_firmware.value);
    const maquina = campo_dron.value;
    if (!firmware || !maquina) return;
    if (!confirm(`¿Actualizar ${maquina} a la versión ${firmware.version}? El dron se reiniciará al terminar.`)) return;
    con_boton_ocupado(boton_desplegar, async () => {
      try {
        await enviar_json('/ota/deploy', 'POST', { number: firmware.number, machine: maquina });
        dron_en_actualizacion = maquina;
        mostrar_resultado(nota_desplegar, '');
        registro_ota.replaceChildren();
        pintar_progreso('enviado', 0);
      } catch (error) {
        if (error.message === 'sesion_invalida') return;
        mostrar_resultado(nota_desplegar, error.estado === 409
          ? `${maquina} se desconectó o su firmware no admite OTA.`
          : error.message, true);
      }
    }, refrescar_boton_desplegar);
  });

  function pintar_progreso(estado, porcentaje = null) {
    progreso_ota.hidden = false;
    progreso_ota.classList.toggle('esta_error', estado === 'error');
    progreso_ota_estado.textContent = textos_estado_ota[estado] ?? estado;
    if (es_numero(porcentaje)) {
      progreso_ota_porcentaje.textContent = `${porcentaje} %`;
      progreso_ota_relleno.style.width = `${porcentaje}%`;
      progreso_ota_barra.setAttribute('aria-valuenow', String(porcentaje));
    }
  }

  // ===== WebSocket: avance en vivo =====
  function procesar_ota(mensaje) {
    if (dron_en_actualizacion && mensaje.machine !== dron_en_actualizacion) return;
    const { estado, porcentaje, version, detalle } = mensaje;
    if (estado === 'descargando') {
      pintar_progreso(estado, porcentaje ?? 0);
      registrar(`${mensaje.machine}: descargando v${version} · ${porcentaje ?? 0} %`);
    } else if (estado === 'instalado') {
      pintar_progreso(estado, 100);
      registrar(`${mensaje.machine}: v${version} instalada, reiniciando. Esperando a que vuelva…`);
    } else if (estado === 'confirmado') {
      pintar_progreso(estado, 100);
      registrar(`${mensaje.machine}: arrancó con v${version} y se conectó a la API. Listo.`);
      dron_en_actualizacion = null;
      cargar_drones();
    } else if (estado === 'error') {
      pintar_progreso(estado);
      registrar(`${mensaje.machine}: ${detalle ?? 'error'}`, true);
      dron_en_actualizacion = null;
      refrescar_boton_desplegar();
    } else if (estado === 'enviado') {
      registrar(`${mensaje.from ?? 'Panel'} mandó v${version} a ${mensaje.machine}`);
    }
  }

  const enlace = crear_enlace_ws({
    url: ws_url,
    token: sesion.token,
    al_mensaje(mensaje) {
      if (!mensaje.tipo && typeof mensaje.message === 'string') {
        if (mensaje.message === 'Unauthorized' || mensaje.message.startsWith('WebSocket error')) enlace.marcar_rechazada();
        return;
      }
      if (mensaje.tipo === 'ota') procesar_ota(mensaje);
      // Un dron que se conecta (o vuelve tras reiniciar) manda estado_dron con su versión
      if (mensaje.tipo === 'estado_dron') {
        cargar_drones();
        if (mensaje.machine === dron_en_actualizacion) registrar(`${mensaje.machine} reconectado con v${mensaje.version}`);
      }
      if (mensaje.tipo === 'error' && mensaje.machine) cargar_drones();
    },
  });
  enlace.conectar();

  cargar_firmwares();
  cargar_drones();
}
