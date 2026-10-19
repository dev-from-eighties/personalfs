# PersonalFS v2 — Especificación técnica y plan de validación

**Estado:** borrador implementable v0.1 · **Fecha:** 2026-10-19  
**Propósito:** primera iteración *specification-driven development* y prototipo medible.  
**Compatibilidad:** PersonalFS v1 continúa como producto/backend independiente. Ningún soporte de lectura o conversión automática de imágenes v1 en la primera iteración.

> **Convenciones:** **DEBE**, **NO DEBE**, **DEBERÍA** y **PUEDE** tienen significado normativo. Las decisiones de ingeniería que se fijan aquí para que el prototipo sea implementable se identifican como **decisión v0.1**, y pueden revisarse en una futura revisión del formato. No confundir objetivos de rendimiento con garantías.

## 1. Problema, contexto y objetivos

PersonalFS v1 usa una imagen dinámica montada vía FUSE. Su gestión de crecimiento y compactación permite superar los problemas de longitud de rutas físicas introducidos por una capa de cifrado exterior, pero bajo SMB y HDD las expansiones largas pueden bloquear solicitudes y producir timeouts. La compactación adaptativa puede contrarrestar el crecimiento logrado tras reintentos de copias parciales. v1 sigue siendo utilizable con incrementos estáticos generosos, `adaptative_shrink=false` y compactación al desmontar.

**PersonalFS v2** sustituye el contenedor monolítico por objetos físicos de nombres cortos almacenados en un árbol de **profundidad física fija de un solo nivel de paginación**. Las rutas visibles por FUSE son un espacio de nombres virtual. Tanto los archivos como los directorios son objetos. Un directorio es un objeto cuyo contenido codifica únicamente sus hijos inmediatos y su padre, no las rutas completas de descendientes.

### 1.1 Requisitos obligatorios

- No SQLite ni otro motor de base de datos; no archivos de imagen, loop devices, NBD, redimensionamiento Fibonacci, staging de datos ni compactación global de bloques.
- La longitud de rutas y nombres virtuales **no determina** la longitud de los nombres físicos de objetos. Navegación sin materializar la ruta virtual completa en el anfitrión.
- Estructura física con raíz de almacenamiento y, como máximo, **una capa** de directorios-página; la profundidad nunca crece con ObjectID ni con la ruta virtual.
- IDs de objeto estables de 64 bits; inicialmente monotónicos y no reutilizables.
- FUSE como interfaz; bajo él se reutilizan archivos ordinarios y operaciones POSIX del filesystem anfitrión.
- Escritura de contenido de archivos por descriptor (`pwrite`/`pread`) directamente sobre objeto de datos; no consultar ni reescribir directorio por cada bloque.
- Directorios persistidos como registro **append-only de operaciones** de longitud variable, con lectura por reconstrucción de estado; compactación local eventual por reemplazo atómico.
- Caché de directorios opcional, limitada por **presupuesto de bytes**, no por número de directorios/entradas. Persistencia siempre autoritativa.
- Journaling mínimo para operaciones de metadatos multiobjeto; sin *group commits* en v0.1.
- Recuperación tras apagón o terminación abrupta sin exponer entradas parcialmente escritas como válidas ni perder objetos por una operación a medio aplicar.
- Cifrado externo: almacenamiento, índices, objetos y journal se alojan **todos** dentro de la misma ubicación ya protegida. PersonalFS no implementa cifrado.

### 1.2 Fuera de alcance en la primera iteración

- Convertidor de imágenes v1; snapshots; cuotas por usuario; cifrado interno; deduplicación; compresión; clusters virtuales.
- Extensión de los límites del VFS Linux (`NAME_MAX`, máximos de rutas aceptadas por las syscalls, valores UTF-8 específicos del cliente).
- Concurrencia multiproceso de *writers* sobre el mismo repositorio: **un solo daemon dueño del almacén**. SMB/NFS pueden acceder *al montaje FUSE*, no directamente al almacén físico.
- Durabilidad ante fallo de hardware que mienta sobre `fsync`, sectores defectuosos no recuperables o corrupción arbitraria del filesystem anfitrión.
- Optimización anticipada mediante hashes persistidos, árboles B o modificación in-place de registros de directorio.

## 2. Modelo lógico

### 2.1 Objetos

- **ObjectID 0:** directorio raíz virtual, identificador permanente. Existe siempre.
- **Archivo regular:** contenido binario nativo en un archivo físico; metadatos de tipo y atributos asociados en un registro de control o *sidecar*.
- **Directorio:** objeto cuyo payload es una secuencia de registros de alta/baja de miembros inmediatos; contiene `parent_object_id` en metadatos persistentes. El índice activo se reconstruye por reproducción de sus registros válidos.
- **Enlace simbólico:** objeto de contenido que representa los bytes de destino; incluido como objetivo de compatibilidad POSIX, no requerido en la prueba inicial de copia de ZIP, pero v1 sí lo implementa por lo que sería bueno incluirlo.
- **Entradas de directorio:** `(nombre_bytes, child_object_id, tipo_hint)`; los nombres pertenecen al directorio padre y **no** al objeto hijo.
- Hard links para archivos regulares se contemplan en la semántica final mediante conteo de enlaces; la primera fase puede rechazarlos con `EOPNOTSUPP` **documentado**, nunca fingir que se implementaron.

Los nombres virtuales son secuencias de bytes POSIX válidas (sin `/` ni NUL), **no necesariamente UTF-8**. Se conservan intactos y se comparan byte a byte; no se hace ordenamiento natural, Unicode normalization, case-folding ni localización. La visualización textual de herramientas de depuración puede escaparlos.

### 2.2 Resolución de rutas

Una ruta `/a/b/c` se resuelve desde ObjectID 0: `lookup(0,"a") → A`, `lookup(A,"b") → B`, `lookup(B,"c") → C`. Cada búsqueda visita sólo el directorio padre correspondiente. Nunca se construye `ruta_fisica_cifrada/a/b/c`.

`rename` de un directorio modifica entradas de sus padres y, si cambia de padre, el `parent_object_id` del propio directorio, **sin recorrer descendientes**. DEBE prohibir ciclos (mover un directorio dentro de sí mismo o sus descendientes).

Los objetos no cambian de ID después de renombrar. Descriptores abiertos mantienen referencia al ObjectID, aun cuando cambie el nombre visible.

## 3. Layout físico y paginación

### 3.1 Layout de referencia (decisión v0.1)

```text
<store>/
  superblock                 # metadatos del almacén (no es un objeto virtual)
  0.obj                      # objeto directorio raíz
  00000000/                  # página 0
    1.obj
    1.meta
    2.obj
    2.meta
    ...
  00000001/                  # página 1
    ...
  journal                    # journal global de metadatos (único archivo)
  lock                       # exclusión de daemon
```

Cada directorio físico de página tiene nombre de ancho fijo y **no alberga subdirectorios de paginación**. `0.obj` está fuera de las páginas. Si un tipo necesita sidecar `.meta`, se aloja junto a su `.obj`; cada ObjectID consume como máximo una entrada `.obj` y una `.meta`, además de temporales transitorios. Una prueba deberá contar **entradas físicas por página**, no sólo objetos: una página de 4096 objetos puede contener más de 8192 entradas durante una transacción.

**Decisión v0.1:** `OBJECTS_PER_PAGE = 4096` y primer ID asignable = 1. `page=(id-1)//4096`. Nombres de página en decimal de ocho dígitos para la primera versión; al superar su rango, la versión de formato debe especificar un ancho mayor o mecanismo de extensión **antes** de continuar asignando páginas. Alternativamente, puede fijarse desde el principio un campo hexadecimal de 16 dígitos; la implementación DEBE escoger exactamente una codificación y documentarla en `superblock`. Se recomienda **hexadecimal de 16 dígitos** para evitar migraciones por anchura. Ejemplo: `0000000000000000/1.obj`. Los ejemplos de esta especificación no autorizan mezclar codificaciones.

Los ID se asignan y reservan de forma durable: el contador `next_object_id` DEBE persistirse antes de que un ID pueda aparecer en una entrada confirmada. Saltar IDs tras una caída es aceptable; **reutilizar un ID anterior nunca lo es**. Un objeto creado pero no enlazado puede ser eliminado de forma segura mediante recuperación/GC cuando no exista referencia ni transacción pendiente.

El tamaño de página es inmutable para un almacén existente. `superblock` incluye `magic`, versión mayor/menor de formato, `objects_per_page`, codificación de página, ID de raíz, `next_object_id`, flags y checksum. Diferencias incompatibles DEBEN rechazar montaje de escritura. **No se permite inferir el tamaño de página de los defaults actuales del binario.**

### 3.2 Límites de la capa exterior

El instalador DEBE medir la longitud de la ruta física del almacén, el máximo de componente y las restricciones observables del backend cifrado. El peor caso `<store>/<page>/<object_id>.meta` y variantes `.tmp` DEBE permanecer dentro de esos límites. No existe garantía de que un único nivel funcione si el usuario instala el almacén demasiado profundo: fallar durante `init` con diagnóstico accionable. Páginas fijas reducen presión sobre directorios enormes, pero el tamaño 4096 es **hipótesis a medir**, especialmente con eCryptfs, gocryptfs, HDD y ext4.

## 4. Formatos persistentes

### 4.1 Principios

- Formato autocontenido, versionado y con endianess fija (**little-endian**, decisión v0.1).
- Para datos del usuario, `.obj` de archivo regular son **bytes exactos**, sin encabezado que modifique sus offsets. Su tipo y atributos viven en `.meta`.
- Para directorios, `.obj` contiene un encabezado identificable y registros incrementales; no es un archivo de texto plano aunque los nombres sean bytes UTF-8 frecuentes.
- Cada registro incluye `length`, `opcode`, `sequence/generation`, `name_length`, `child_id` y checksum (CRC32C o equivalente especificado por versión). No aceptar registros fuera de límites ni con longitud/CRC incoherentes.
- El checksum identifica corrupción accidental o escritura parcial, **no reemplaza** la sincronización de durabilidad ni provee autenticación.
- El final válido de un log se determina por registros consecutivos íntegros. Una cola incompleta se ignora durante recuperación **sólo si existe evidencia de que no se confirmó como durable**; los errores de checksum en medio de registros confirmados son error de integridad, no borrado silencioso.

### 4.2 Registros de directorio

Opcodes mínimos: `ADD(name, child_id, type_hint)`, `REMOVE(name, expected_child_id)` y, para gestión de versiones, `CHECKPOINT(generation)` si se adopta. Para cada `(parent_id,name)` el último registro **confirmado y válido** define la entrada activa. `REMOVE` exige que el ID previo coincida: si no, es error o recuperación de journal, nunca borrar otra entrada con igual nombre por accidente.

El archivo no requiere orden lexicográfico. La caché puede construir `hashmap` y/o vector ordenado por bytes para búsquedas y enumeración. `readdir` NO garantiza orden humano ni natural.

**Advertencia de diseño:** el log por sí solo no hace atómica una operación de varios objetos; para esas operaciones manda el protocolo del journal (sección 6). Tampoco garantiza automáticamente la atomicidad del *append* bajo caída: la implementación debe delimitar qué registros fueron durables/confirmados mediante protocolo de `fsync` y journal.

### 4.3 Metadatos

`<id>.meta` incluye al menos: `magic`, versión, `object_id`, `type`, modo POSIX, uid/gid, nlink o referencias necesarias, timestamps (incluyendo nanosegundos cuando corresponda), `parent_id` para directorios, generación, checksum. El tamaño de archivo regular puede obtenerse de `stat(.obj)`; no duplicarlo sin un protocolo de coherencia. Valores no representables en capa exterior (ACL/xattrs) requieren implementación propia o rechazo explícito en v0.1.

Actualizaciones de `.meta` usan escritura a archivo temporal de **nombre único** (por transacción/hilo), `fsync(temp)`, `rename(temp, meta)`, `fsync(page_dir)`. Nunca borrar primero el original.

## 5. Operaciones FUSE y concurrencia

### 5.1 Camino rápido

1. `open(path)` resuelve componentes hasta ObjectID; abre descriptor físico de `<id>.obj`.
2. `read` usa `pread` sobre el descriptor; `write` usa `pwrite` sin visitar el índice por bloque.
3. `flush` **no** se interpreta como garantía de persistencia; `fsync`/`fdatasync` honran la semántica Linux y sincronizan datos más metadatos de recuperación necesarios.
4. El daemon DEBE propagar `ENOSPC`, `EIO`, `EDQUOT`, `EACCES` y otros errores reales; nunca prometer éxito incondicional.
5. Un archivo desvinculado pero abierto sigue disponible por su descriptor hasta el último `release`. El barrido de huérfanos debe considerar referencias abiertas y reinicio del daemon.

### 5.2 Modificaciones de directorios

- Altas/bajas: append de registro delimitado, comprobable y persistido según política; **no** reescribir el objeto completo en el camino habitual.
- Un bloqueo por directorio protege su secuencia y su caché; **evitar un mutex global durante I/O prolongado**.
- Transacciones que tocan varios directorios toman bloqueos en orden creciente por ObjectID para prevenir deadlocks.
- `readdir` necesita cookies estables durante la vida del handle: **decisión v0.1:** snapshot lógico por `opendir` o vector inmovilizado con referencia compartida. Documentar su coste y liberar en `releasedir`. Las modificaciones posteriores pueden ser visibles en un nuevo handle.
- `rmdir` debe verificar vaciedad lógica bajo bloqueo y estado consistente. `rename` maneja reemplazo de destino, cruces de directorios, enlaces y ciclos conforme al soporte POSIX declarado.

### 5.3 No soportado temporalmente

Para la primera demostración se permite una lista explícita y acotada de operaciones `EOPNOTSUPP` (`link`, ACL/xattr, etc.) siempre que no se anuncien como compatibles. **No** se permite silenciosamente devolver éxito por semánticas ausentes. La aceptación final POSIX se decide por matriz de pruebas (sección 11).

## 6. Durabilidad, journal y recuperación

### 6.1 Política de consistencia

La coherencia observable entre hilos es responsabilidad de PersonalFS; `rename` virtual debe parecer atómico al cliente. Los datos escritos en archivo regular siguen reglas normales de `write`/`fsync` del FS Linux; un `write` exitoso no equivale a persistencia garantizada tras apagón, pero un `fsync` exitoso sí debe persistir la información requerida en el backend dentro de sus garantías reales.

**Sin group commit.** Cada transacción de metadatos que deba quedar confirmada se sincroniza individualmente (coste aceptado para v0.1). La implementación deberá instrumentar número y latencia de `fsync`.

### 6.2 Journal global

El journal registra **intenciones de operaciones multiobjeto**, no el contenido de cada bloque de un ZIP. Registros binarios `BEGIN(txid, tipo, objetos, versiones originales, resultado objetivo, datos de undo/redo suficientes)`, `COMMIT(txid)` y, si procede, `DONE(txid)`, con longitud y checksum. `BEGIN` **debe estar persistido** antes de modificar estructuras publicadas relacionadas con la transacción. No iniciar una nueva modificación que contradiga una transacción activa sin ordenarla y coordinarla.

**Decisión v0.1: recuperación por REDO idempotente.** Una vez `BEGIN` es durable, la operación se ejecutará hasta estado final deseado o se dejará el almacén sin montaje de escritura con error diagnóstico si la recuperación no puede completarse. Por tanto `BEGIN` DEBE contener suficientes bytes/identidades para reproducir el resultado incluso si alguna modificación intermedia ya se aplicó. El recovery compara ObjectIDs, generaciones y nombres, y nunca interpreta “destino existe” como éxito sin comprobar **qué objeto** existe.

Secuencia de referencia:

1. Bloquear objetos afectados y comprobar precondiciones.
2. Escribir `BEGIN` completo y `fsync(journal)`.
3. Aplicar cada cambio de forma idempotente; `fsync` de archivos/objetos y directorios físicos involucrados.
4. Escribir `COMMIT` y `fsync(journal)`; responder éxito una vez que la semántica de operación esté satisfecha.
5. Marcar `DONE`/limpiar journal **sin permitir pérdida de información necesaria para una recuperación posterior**. Compactación del journal independiente, con reemplazo seguro.

Si el proceso cae **después de BEGIN y antes de COMMIT**, se rehace hasta completar según esta política. Esto significa que una operación que aún no había devuelto éxito podría aparecer aplicada tras la recuperación: comportamiento de recuperación aceptable para la versión de prueba, que DEBE quedar documentado. No se admite una aplicación a medias visible tras remontar.

La implementación DEBE diseñar de forma exacta la interacción entre registros `ADD/REMOVE` append-only y REDO para evitar aplicar dos veces un `ADD` o `REMOVE`; utilizar `txid`/generación persistidos en cada modificación o una tabla de resultados derivable del journal. Es un **criterio de bloqueo de implementación**, no una optimización postergable.

### 6.3 Compactación de un directorio

Cuando el log crece demasiado respecto de entradas vivas, worker en background puede crear `<id>.<unique>.tmp`, representar su estado actual validado y publicar por `rename()` atómico sobre `<id>.obj` tras `fsync(temp)`, seguido de `fsync(page_dir)`. No debe borrar el original antes del `rename`.

Durante construcción del temporal, cambios nuevos continúan registrándose en el original. Para publicar, bloquear brevemente ese directorio y verificar la generación capturada; si cambió, reintentar o incorporar el delta **antes de publicar**. Jamás reemplazar por una fotografía obsoleta. El temporal abandonado puede borrarse tras verificación; no es evidencia autoritativa de un cambio confirmado.

**v0.1 admite posponer completamente la compactación automática** mientras los benchmarks iniciales sean pequeños; debe proveer compactación manual segura para probarla. No se permite que visitar/cargar un directorio dispare compactación bloqueante.

### 6.4 Arranque y caída

Antes de ofrecer un montaje RW:

1. Adquirir lock exclusivo del almacén (con `flock` sobre archivo `lock` y/o descriptor retenido), verificar ausencia de otro escritor.
2. Validar superblock y layout/versión.
3. Recorrer journal hasta último registro válido y verificar integridad; un CRC inválido en medio de la zona durable es error, no truncado silencioso.
4. Rehacer transacciones pendientes según orden, `fsync` necesario.
5. Recuperar/eliminar temporales no publicados solo después de comprobar que no se necesitan para redo.
6. Validar invariantes mínimos (raíz, IDs, tipos, inexistencia de referencias colgantes para transacciones completadas).
7. Montar y permitir operaciones.

`SIGKILL`/apagón deben poder reproducirse en cualquier punto. No reparar silenciosamente corrupción no recuperable. Proveer utilidad `personalfs fsck --check` sin escritura y `--repair` separada, al menos en un subconjunto inicial documentado.

## 7. Caché adaptativa por bytes

- `directory_cache_max_bytes`: presupuesto configurable; **valor inicial tentativo** 64 MiB, sujeto a benchmarks. Medir coste de nombres, vector, hashmap, punteros, asignador y objetos de caché, no sólo bytes de `.obj`.
- LRU con `pin_count` para entradas activas. Un directorio puede ser servido **sin cachearlo** si supera el presupuesto. Nunca rechazar acceso por tamaño de directorio; usar lectura progresiva/índice temporal acotado si corresponde.
- Directorios pequeños: vector de nombres/IDs; búsqueda binaria sólo si se ordenó en RAM. Directorios calientes/grandes: hashmap opcional de clave bytes y valor ObjectID. El caché jamás es fuente de verdad.
- Si el directorio recibe modificación por la instancia única, actualizar/invalidad caché bajo el lock correspondiente. No leer periódicamente el árbol físico para “descubrir” cambios externos: modificaciones externas al store son **no soportadas**.
- Medir cache hits/misses, bytes actuales, expulsiones, tiempos de carga, tamaño de log leído y entradas activas. Políticas/umbrales se calibrarán por medición.

## 8. Seguridad y fallos operativos

- Repositorio físico dentro del directorio protegido por gocryptfs/eCryptfs/u otra capa exterior. No crear temporales, journal, logs de depuración con nombres originales ni snapshots fuera del límite cifrado sin consentimiento explícito.
- Verificar permisos y ownership del store. Evitar traversal y symlinks no confiables en los **nombres físicos**; operar con dirfd + `openat`/`renameat` y flags adecuados (`O_NOFOLLOW` donde corresponda). Nunca concatenar rutas virtuales como rutas del anfitrión.
- No asegurar disponibilidad por el mero hecho de que `umount` no forzado espere: desmontaje normal debe parar nuevas operaciones, vaciar/sincronizar estructuras relevantes y completar o devolver error. Las conexiones SMB son responsabilidad de Samba, no un candado de durabilidad.
- No confiar en un contador de archivos de directorio sin comprobar consistencia cuando `rmdir` o recuperación lo requieran.
- El FS anfitrión puede quedarse sin inodos, bytes o cuotas aun si PersonalFS tiene IDs disponibles; propagar errores.
- Crear y montar almacenes sólo con permisos apropiados: la pérdida del directorio `superblock`/`journal`/páginas puede significar pérdida de namespace aunque sobrevivan bytes de objetos.

## 9. Configuración y CLI propuesta

```ini
# conceptual, nombres exactos de CLI a estabilizar en implementación
format_version = 2
objects_per_page = 4096              # inmutable y persistido; no override posterior
storage_backend = indexed_objects
cache_max_bytes = 67108864
directory_compaction = manual         # v0.1; future: background
journal_group_commits = false         # no implementarlo v0.1
```

Comandos propuestos:

```text
personalfs v2 init <store>
personalfs v2 mount <store> <mountpoint> [--foreground]
personalfs v2 inspect <store> [--object ID]
personalfs v2 fsck <store> --check
personalfs v2 fsck <store> --repair
personalfs v2 compact-dir <store> <OBJECT_ID>
personalfs v2 stats <mountpoint>
```

El prefijo `v2` es ilustrativo: se puede mantener una sola CLI con selección por `superblock`. Un almacén v1 **no** puede abrirse como v2 ni viceversa por autodetección permisiva. La producción v1 no se modifica en esta rama.

## 10. Instrumentación obligatoria

Logs estructurados con timestamp de pared para correlación y duración monotónica para medir latencia. Objetos relevantes por ID, sin filtrar nombres virtuales sensibles fuera del store. Contadores y histogramas, como mínimo:

- Latencia `lookup`, `getattr`, `readdir`, `create`, `rename`, `unlink`, `read`, `write`, `fsync` (p50/p95/p99).
- Throughput secuencial MiB/s y operaciones/s; tamaños de syscalls FUSE y del anfitrión.
- `openat`, `pread`, `pwrite`, `fdatasync/fsync`, `rename`, locks y tiempo en cola.
- Cantidad de objetos y páginas, entradas físicas por página y descriptores abiertos.
- Tamaño de logs de directorio vs entradas vivas; bytes de basura recuperable; tiempo de compactación local.
- Bytes/cache hits/misses; coste de inicializar directorios grandes.
- Journal: bytes, transacciones, latencia de sync, tiempo de recovery y errores.

Ninguna solicitud FUSE independiente debe bloquearse globalmente esperando un worker de compactación. Establecer alertas de bloqueo >1s y stack trace opcional de depuración.

## 11. Plan de pruebas y criterios de aceptación

### 11.1 Prueba de concepto funcional (P0)

1. Inicializar store cifrado exterior, montar FUSE local, crear carpetas anidadas virtualmente y archivos de nombres largos válidos; comprobar que longitud/profundidad físicas no crecen con profundidad virtual.
2. Crear, escribir, cerrar, leer y verificar hash SHA-256 de un archivo grande (30 GiB o datos equivalentes no dispersos cuando se busque medir I/O real).
3. Copiar desde Nautilus por SMB hacia el montaje FUSE del servidor con HDD; verificar ausencia de timeouts repetitivos debidos a redimensionamiento/compactación, tamaño final y checksum.
4. Listar directorios grandes, navegar rutas profundas y renombrar un directorio con miles de descendientes; verificar que el número de operaciones físicas no crece con el número de descendientes.
5. Apagar/matar proceso y remontar; verificar contenido y estructura, incluyendo journal pendiente.

### 11.2 Cargas de metadatos (P1)

- Crear/copiar/eliminar **100.000 archivos pequeños**, distribuidos y luego concentrados en un único directorio; medir ops/s y latencias.
- Simular **1 millón de objetos** físicos sobre filesystem real (y su cifrado exterior): tiempos de acceso `stat/open`, creación y listado por página, distribución de tamaños, inodos y espacio, no sólo número teórico de carpetas.
- Comparar páginas de 1024/2048/4096/8192/16384 objetos en almacenes de prueba nuevos, en HDD y NVMe. Esto mide elección de constante para versión 2; no cambiar el tamaño de página de un store ya creado.
- Renombrado entre directorios concurrente y reemplazo de destino, `rmdir` con directorio no vacío, archivos abiertos mientras se hace `unlink`.
- Directorio gigante que supere `cache_max_bytes`: acceso correcto sin OOM, cache bypass o lectura incremental demostrada.

### 11.3 Inyección de fallos (P1 bloqueante para producción)

Inyectar caída después de **cada frontera durable**: reservar ID; crear objeto; escribir temporal; sincronizar temporal; publicar temporal; `BEGIN`; primera y segunda modificación de `rename` multiobjeto; `COMMIT`; limpieza de journal; compactación de directorio. Tras recuperación, exigir exactamente un estado lógico coherente y ninguna entrada apuntando al ID equivocado.

Casos separados de `ENOSPC`, falta de inodos, `EIO`, `fsync` fallido y permisos. Probar `SIGKILL` y, para durabilidad real, reinicio/retirada controlada de alimentación en entorno descartable: `SIGKILL` **no simula por completo** un corte eléctrico.

### 11.4 Rendimiento (no confundir objetivos con garantías)

Medir frente a: (A) filesystem anfitrión directo, (B) PersonalFS v1, (C) PersonalFS v2. Mismo HDD/cifrado/datos/cliente SMB; repetir en condiciones fría y caliente, medir medianas y colas (p95/p99), no un solo throughput promedio. La referencia observada de v1 es aproximadamente 25–50 MiB/s en el escenario mencionado; la meta de v2 es **estar cerca del throughput directo**, manteniendo integridad. No fijar por ahora un SLA numérico de ops/s sin datos.

### 11.5 Matriz de operaciones POSIX/FUSE

| Operación | P0 demostración | Producción v2 |
|---|---|---|
| `lookup/getattr/opendir/readdir/releasedir` | obligatorio | completo |
| `create/open/read/write/flush/release/fsync` | obligatorio | completo |
| `mkdir/rmdir/unlink` | obligatorio | completo |
| `rename` mismo padre | obligatorio | completo |
| `rename` distinto padre y reemplazo | prueba guiada | completo |
| `truncate/ftruncate` | obligatorio | completo |
| `chmod/chown/utimens` | especificar/rechazar | completo según política |
| `symlink/readlink` | opcional explícito | completo |
| `link` (hard link) | opcional explícito | completo |
| `statfs` | estimación delegada | coherente |
| xattrs/ACL | opcional explícito | declarar soporte real |

“Completo” significa fidelidad a la semántica compatible con el backend y pruebas; no basta con implementar una callback que devuelve 0.

## 12. Orden de implementación recomendado

**Fase 0 — formato:** superblock, paginación determinista, allocator durable de IDs, inspect y `fsck --check` básico. Pruebas unitarias de encode/decode y truncamientos.

**Fase 1 — namespace:** raíz ObjectID 0, directorios append-only y replay, caché RAM limitada, `lookup/readdir/mkdir/create`, pruebas de rutas profundas, muchos nombres, y directorios grandes.

**Fase 2 — datos:** archivo regular directo, `open/read/write/truncate/fsync`, permisos mínimos, prueba ZIP grande local y luego SMB, instrumentación.

**Fase 3 — mutaciones robustas:** `unlink/rmdir/rename`, journal redo durable, generaciones, locks ordenados, recovery idempotente, `SIGKILL` sistemático.

**Fase 4 — mantenimiento y hardening:** compactación local de logs de directorio, fsck/repair restringido y auditable, invalidaciones de caché, symlinks/hardlinks y metadatos extendidos según requisitos, benchmarks comparativos.

**Regla de salida:** P0 permite comparar velocidad pero **no autoriza migrar proyectos únicos sin copia verificada**. Para datos importantes se exige completar las pruebas de fallos de P1, en particular atomicidad `rename`, durabilidad `fsync`, y recuperación de journal.

## 13. Decisiones cerradas y cuestiones que deben verificarse

### Cerradas para el prototipo

- Backend nuevo basado en objetos; PersonalFS v1 continúa.
- Un solo nivel de paginación física; tamaño fijo registrado en superblock.
- ObjectID 0 raíz; ObjectIDs permanentes, monotónicos sin reciclaje.
- Directorios como objetos con hijos inmediatos; renombres sin recorrer descendientes.
- `append-only` para cambios de directorio, caché opcional en RAM, tamaño de caché medido por bytes.
- Sin orden ASCII obligatorio en disco; comparación de nombres por bytes; sin natural sort.
- Sin SQL, sin índices de ruta completa, sin group commits, sin compactación global de contenido.
- Journal de intenciones multiobjeto y recuperación idempotente; sincronización individual.
- Datos de archivos regulares directos, sin staging.

### Decisiones v0.1 revisables antes de congelar formato

1. Codificación final de nombres de página: se recomienda hex de 16 dígitos; elegir una antes del primer store real.
2. Separación `.obj`/`.meta` frente a un esquema simplificado para directorios; especificar serialización binaria concreta, tamaños máximos y checksums.
3. Mecanismo exacto de persistencia de `next_object_id` (superblock de copia doble o temporales + rename, sin ambigüedad bajo crash).
4. Codificación del journal e interacción `txid`/generaciones/records ADD-REMOVE durante redo.
5. Política de recuperaciones de escrituras/atributos de archivos regulares tras `fsync` y reinicios; paridad POSIX.
6. Umbral de compactación de directorios, cache y número de objetos por página: calibrar en HDD real.
7. Alcance real de POSIX sobre symlinks, hard links y xattrs antes de producción.

**No son cuestiones opcionales para una versión productiva.** Deben quedar resueltas mediante pruebas y documentación antes de declarar estable el formato v2.

---

**Criterio arquitectónico rector:** una ruta virtual larga nunca provoca una ruta física larga, ninguna escritura de datos grande necesita ampliar ni compactar una imagen, y ninguna operación confirmada queda irrecuperable por una interrupción del proceso entre cambios de metadatos.
