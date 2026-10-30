1. Seguridad (urgente)
La secret_key del JWT está escrita en segurity_bm.py:10 y subida a GitHub. Con ella, cualquiera puede firmar un token con rol O5-1 y tener acceso total. Hay que pasarla a una variable de entorno (un archivo .env que esté en .gitignore) y generar una clave nueva, porque la actual ya hay que darla por expuesta.
Las contraseñas de las máquinas se guardan en texto plano. new_machine_rt en def_bm.py:61 hace model_dump() sin pasar por encode_password.
Un rol insuficiente responde 401 en vez de 403. El panel interpreta el 401 como "sesión expirada" y cierra la sesión del usuario.
El rol viaja dentro del token, que dura 24 h. Si bajas de rol a alguien, sigue teniendo el rol anterior hasta que su token expire. Para un sistema que controla un dron, conviene volver a leer el rol en la base de datos en las rutas críticas, o acortar la duración del token.
2. Cosas que hoy están rotas
Todas las rutas de machine_rt.py tienen el error de Depends: el parámetro recibe el token, no los datos. Es el mismo error que ya corregí en usuarios.
test_all_sensors llama a search_machine_rt_exception(data.name) sin el argumento number, así que lanza TypeError. Además es un GET que recibe cuerpo; debería ser POST.
El router de la bitácora no está registrado en main.py y tiene los errores que te listé antes.
La API y el panel no coinciden en el WebSocket. El panel se conecta a /ws/panel?token=..., pero la API expone /ws y espera el token en el primer mensaje. Además, si el mensaje no trae token, se lanza AttributeError, que no está en tu except.
3. Lo que falta para el objetivo del proyecto
Esto es lo más importante a mediano plazo. La API todavía no modela el núcleo de SAMATI:

El dron no tiene cómo autenticarse. /token solo busca en la colección de usuarios. Las máquinas tienen contraseña, pero no existe un endpoint que les entregue un token. Hace falta algo como POST /machine_rt/token.
El WebSocket solo devuelve lo que recibe (eco); no reenvía nada. Hace falta un ConnectionManager que mantenga dos grupos de conexiones, drones y paneles. La telemetría del dron se reenvía a todos los paneles, y los comandos de un panel se envían al dron que corresponda. Los mensajes se validan con tus BaseModel (sensor_data, batery_6s).
Faltan las misiones. No hay modelos ni rutas para la misión de referencia, los waypoints, las lecturas LiDAR por waypoint, la comparación entre la misión de referencia y la misión posterior a un evento climático, ni las alertas a las dependencias. Es justo lo que describe el sitio en "Funcionamiento". Te propongo estas colecciones: missions, waypoints, readings y alerts.
La telemetría no se guarda. Para comparar misiones necesitas el historial. MongoDB tiene time series collections, pensadas exactamente para datos de sensores.
Falta subir evidencia (fotos y videos). python-multipart ya está en tus requirements. Falta el endpoint de subida con límite de tamaño y tipo de archivo, y una ruta para servir esos archivos solo con sesión iniciada.
4. Base de datos y estructura
Estás usando la base local de MongoDB, que está reservada para uso interno del servidor (datos de replicación). No se replica y no está pensada para datos de la aplicación. Conviene crear una base samati.
Hay 4 MongoClient(), uno por archivo. Lo mejor es un solo cliente en un módulo propio (por ejemplo db_bm.py), con la URL de conexión en una variable de entorno.
Los números se renumeran al borrar (máquinas y bitácora). log_005 puede pasar a llamarse log_004, y cualquier referencia guardada a esos números queda apuntando al registro equivocado. Usa un identificador que nunca se reutilice. count_documents() + 1 también puede generar duplicados si llegan dos altas al mismo tiempo.
Faltan índices únicos en user.name y machine.name. Hoy evitas los nombres repetidos con un find_one previo, y eso falla si llegan dos peticiones simultáneas.
Usas pymongo síncrono dentro de async def. Cada consulta bloquea el servidor completo, incluidos los WebSockets de telemetría. Hay dos salidas: declarar esas rutas con def normal (FastAPI las ejecuta en otro hilo) o usar AsyncMongoClient, que ya viene en tu versión de PyMongo.
Conviene corregir los nombres con errores ahora, mientras hay pocos datos: onwer, hipothesis, segurity, shetings. Después, renombrar campos obliga a migrar todos los documentos guardados. phone_number debería ser str.
5. Calidad
Usa response_model= en las rutas, para que FastAPI garantice que nunca se devuelve un campo password, aunque alguien lo olvide al escribir la ruta.
Agrega una carpeta tests/ con pruebas como las 22 que corrí para usuarios (con TestClient y una base de datos simulada). Así compruebas los permisos cada vez que cambies algo.
Agrega la configuración de Ruff que te comenté, para quitar los falsos positivos de Depends.
✶ Insight ─────────────────────────────────────

El cuello de botella de SAMATI va a ser el tiempo real, no el CRUD. Un dron enviando telemetría varias veces por segundo a través de un servidor que se bloquea en cada consulta a MongoDB (punto 4) va a meter retraso justo en el canal que controla el vuelo. Por eso separar el WebSocket de las consultas bloqueantes importa más que los demás detalles.
El error de Depends se repite en cada router porque se copió el mismo patrón. Vale la pena entender la regla de fondo: un parámetro con = Depends(f) recibe lo que devuelve f, no el cuerpo de la petición. El cuerpo va en un parámetro tipado sin valor por defecto.