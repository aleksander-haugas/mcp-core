# MCP Core FreeBSD — Estado global y arquitectura del proyecto

> **Proyecto:** MCP Core FreeBSD
> **Repositorio:** https://github.com/aleksander-haugas/mcp-core
> **Plataforma objetivo:** FreeBSD 15.1
> **Lenguaje principal:** C++17, con ABI de plugins compatible con C
> **Versión que informa el daemon:** `0.1.0`
> **Estado:** base Core operativa; plugins System, Hardware y Nakama integrados; siguiente hito: mensajería asíncrona Nakama ↔ Core ↔ AgentBus.
> **Actualizado:** 20 de noviembre de 2026

Este documento describe **el proyecto MCP Core completo**, no solamente el repositorio de código que ya existe. Separa la infraestructura implementada y verificada de los subsistemas planificados, para que la arquitectura objetivo no se confunda con funcionalidades que todavía no están terminadas.

---

## 1. Visión general

MCP Core es una plataforma modular nativa para FreeBSD. Un daemon central administra los plugins, publica herramientas MCP, aplica autorización y proporciona una vía local de comunicación mediante Unix IPC. Los plugins encapsulan las capacidades del sistema y las integraciones externas.

La arquitectura objetivo amplía ese núcleo con:

- Nakama como infraestructura realtime para comunicación entre agentes.
- AgentBus como capa de mensajería y enrutamiento dentro de MCP Core.
- PostgreSQL y pgvector para persistencia y recuperación de conocimiento.
- llama.cpp para inferencia local.
- Agentes especializados ejecutándose en jails separadas.
- Plugins nativos para administración, observabilidad, seguridad, red, servicios, base de datos y RAG.
- Herramientas de administración como `mcpctl` y Control Center.

La regla de diseño es mantener un **Core pequeño, autónomo y seguro**, con capacidades añadidas mediante plugins. Los agentes no deben acceder directamente a funciones privilegiadas del host: deben hacerlo a través de herramientas y APIs autorizadas del Core.

### Estado ejecutivo

| Área | Estado actual |
|---|---|
| MCP Core daemon | Implementado y en producción en el host |
| Plugin ABI 1.0 | Implementado; base de compatibilidad |
| Plugin Manager | Implementado; carga y ejecución de plugins |
| JSON-RPC / MCP | Implementado para el flujo operativo actual |
| STDIO ↔ Unix IPC | Implementado y probado |
| Servicio FreeBSD rc.d | Instalado y activo |
| System Plugin | Integrado y cargado |
| Hardware Plugin | Integrado y cargado |
| Authorization | Código implementado; verificación de integración/política en curso |
| Nakama Plugin | Implementado, cargado y operativo en el daemon |
| Nakama WebSocket | Conexión autenticada, estado y ping/pong verificados |
| Persistencia Nakama entre clientes STDIO | Verificada |
| Prueba sostenida de estabilidad Nakama | Completada: 10/10 rondas; status CONNECTED y ping pong:true |
| ABI de mensajes asíncronos | Declaraciones y soporte host-side presentes; integración end-to-end pendiente |
| AgentBus | Componentes Core presentes; integración con Nakama pendiente |
| Agentes Monitor / Guardian / Orchestrator | Arquitectura definida; integración pendiente |
| PostgreSQL / pgvector / RAG | Planificado |
| Plugins Network / Security / Services | Planificados como plugins completos |
| `mcpctl` / Control Center | Planificados |
| Build reproducible de Core y compilación de plugins | Makefiles BSD implementados; despliegue reproducible sigue pendiente |
| Auditoría y release final | Pendiente |

No se asigna un porcentaje global artificial: el avance se mide por subsistema y por pruebas verificables.

---

## 2. Arquitectura física y distribución de servicios

La topología acordada utiliza el host FreeBSD y jails con responsabilidades separadas. Las direcciones siguientes reflejan el esquema de red del proyecto.

| Dirección | Sistema / servicio | Responsabilidad |
|---|---|---|
| `192.168.100.1` | FreeBSD Host — MCP Core | Daemon central, plugins nativos y servicios locales |
| `192.168.100.1:8080` | llama.cpp | Inferencia local para los agentes |
| `192.168.100.2` | PostgreSQL Jail | Base de datos, persistencia y pgvector |
| `192.168.100.3` | Nakama Jail | Backend realtime: conexiones, presencia, eventos y mensajes |
| `192.168.100.4` | Monitor Jail | Supervisión del host y de los servicios autorizados |
| `192.168.100.5` | Guardian Jail | Seguridad, detección y respuesta controlada |
| `192.168.100.6` | Orchestrator Jail | Coordinación de agentes y planificación |

### Vista lógica

```text
                         OPERADOR / CLIENTES MCP
                                  |
                         STDIO / IPC / mcpctl
                                  |
                                  v
+------------------------------------------------------------------+
|                    FREEBSD HOST — 192.168.100.1                   |
|                                                                  |
|  +--------------------------- MCP CORE ------------------------+ |
|  | Daemon | JSON-RPC | RequestHandler | Authorization          | |
|  | IPC    | PluginManager | Plugin ABI | Audit (planned)        | |
|  +-------------------------------+------------------------------+ |
|                                  |                                 |
|       +--------------------------+---------------------------+     |
|       |                          |                           |     |
|  System Plugin             Hardware Plugin             Nakama Plugin|
|  Network (planned)         Security (planned)           WebSocket   |
|  Services (planned)        Database (planned)           Auth/Tools  |
|  RAG (planned)                                           Async ABI  |
|       |                          |                           |     |
|       +--------------------------+---------------------------+     |
|                                  |                                 |
|                         AgentBus (Core)                            |
|                     [integración pendiente]                       |
|                                  |                                 |
|                         llama.cpp :8080                            |
+----------------------------------+---------------------------------+
                                   |
                     red interna / comunicación realtime
                                   |
          +------------------------+------------------------+
          |                        |                        |
          v                        v                        v
  +---------------+        +---------------+        +---------------+
  | Nakama Jail   |        | PostgreSQL    |        | Agent Jails   |
  | .3            |        | Jail .2        |        | .4 / .5 / .6  |
  | Realtime      |        | SQL + pgvector|        | Monitor       |
  | Presence      |        | Persistencia  |        | Guardian      |
  | Events / RPC  |        | RAG data      |        | Orchestrator  |
  +---------------+        +---------------+        +---------------+
```

**Separación de responsabilidades:**

- MCP Core es el plano de control local: identidad, autorización, plugins y ejecución de herramientas.
- Nakama es el backend realtime de los agentes; no sustituye al Plugin Manager ni al IPC local.
- AgentBus es la abstracción de mensajería de Core; debe utilizar el transporte Nakama ya existente a través del plugin.
- PostgreSQL conserva estado y datos duraderos; Nakama no debe tratarse como sustituto de la base de datos.
- llama.cpp ofrece inferencia; no es el bus de mensajes ni el almacén de estado.
- Cada agente se ejecuta en su jail y recibe solamente las capacidades que necesita.

---

## 3. Arquitectura interna de MCP Core

### 3.1 Flujo de una petición MCP

```text
Cliente MCP
    |
    | JSON-RPC por STDIO
    v
mcp_core --stdio
    |
    | Unix Domain Socket
    v
MCP Core daemon
    |
    +--> IPC transport
    |
    +--> JSON-RPC / RequestHandler
    |
    +--> Identidad del peer / Authorization
    |
    +--> PluginManager / Tool Registry
              |
              +--> System Plugin
              +--> Hardware Plugin
              +--> Nakama Plugin
              +--> futuros plugins
```

El proceso `--stdio` es un puente cliente. No carga plugins ni es propietario de las conexiones persistentes. El daemon es quien mantiene el Plugin Manager, las instancias de plugins y el transporte WebSocket de Nakama.

### 3.2 Componentes del Core

| Componente | Función |
|---|---|
| `main.cpp` | Modos daemon y STDIO, argumentos, señales y arranque |
| `json_rpc` | Mensajes, solicitudes y respuestas JSON-RPC |
| `request_handler` | Procesamiento MCP y despacho de operaciones |
| `stdio_bridge` | Puente entre stdin/stdout y el cliente IPC |
| `stdio_transport` | Transporte de entrada/salida STDIO |
| `ipc_client` | Conexión de clientes con el daemon |
| `ipc_transport` | Servidor IPC sobre Unix domain socket |
| `authorization` | Identidad, capacidades y autorización |
| `plugin_abi.h` | Contrato C entre Core y bibliotecas de plugins |
| `plugin_manager` | Carga dinámica, validación ABI, registro y ejecución de herramientas |
| `agent_protocol` | Tipos y validación de mensajes de agentes |
| `agent_registry` | Registro e identidad de agentes |
| `agent_transport` | Interfaz abstracta para transportes de agentes |
| `agent_bus` | Enrutamiento de mensajes entre transportes y handlers |
| `agent_rpc` | Operaciones RPC sobre las abstracciones de agentes |

El código de AgentBus y sus interfaces ya está en el repositorio. Su presencia no significa que el recorrido completo desde Nakama hasta AgentBus esté terminado.

---

## 4. Plugin ABI y Plugin Manager

### Plugin ABI

El contrato está definido en `core/include/mcp/plugin_abi.h`.

- ABI base: `MCP_PLUGIN_ABI_VERSION = 0x00010000` (1.0).
- Interfaz externa en C para evitar depender de layouts C++ o excepciones entre bibliotecas.
- Punto de entrada obligatorio: `mcp_plugin_query()`.
- Descriptor con identificador, versión, descripción y callbacks de ciclo de vida y herramientas.
- Host API para logging y descubrimiento de extensiones.

Los plugins exponen `initialize`, `shutdown`, `list_tools` y `execute_tool`. Las herramientas declaran nombre, descripción, JSON Schema y capacidades requeridas.

**Compatibilidad obligatoria:** ABI 1.0 debe seguir funcionando. Las extensiones nuevas son opcionales y no deben obligar a recompilar ni romper plugins antiguos.

### Plugin Manager

El Plugin Manager:

1. Descubre bibliotecas compartidas en el directorio configurado.
2. Carga cada biblioteca dinámicamente.
3. Resuelve `mcp_plugin_query`.
4. Comprueba ABI y tamaño del descriptor.
5. Inicializa el plugin.
6. Registra las herramientas declaradas.
7. Despacha las llamadas a la función de ejecución del plugin.
8. Proporciona las extensiones opcionales del host.

El apagado completo y seguro del Plugin Manager está pendiente de una fase posterior. No se debe descargar una biblioteca mientras pueda existir una llamada o callback ejecutándose dentro de ella.

---

## 5. Plugins nativos

### System Plugin — integrado

Implementación C++ nativa para consultar información del sistema FreeBSD. La superficie documentada durante las pruebas contiene seis herramientas:

- Información general del sistema.
- Hostname.
- Uptime.
- CPU.
- Memoria.
- Load average.

Está cargado por el daemon actual. Las consultas se implementan mediante APIs nativas, no como envoltorios generales de comandos shell.

### Hardware Plugin — integrado

Implementación C++ nativa para inventario y sensores de hardware. La superficie documentada contiene cinco herramientas:

- Temperaturas.
- Inventario PCI.
- GPU.
- USB.
- Almacenamiento.

Está cargado por el daemon actual. Utiliza interfaces y bibliotecas del sistema FreeBSD.

### Nakama Plugin — integrado y operativo

Ubicación: `plugins/nakama/`.

Componentes:

- `nakama_plugin.cpp`: descriptor ABI, configuración, ciclo de vida y herramientas MCP.
- `websocket_transport.cpp`: conexión WebSocket, colas, envío/recepción y estados.
- `receive_dispatcher.cpp`: despacho de mensajes y correlación de respuestas por CID.
- `http_auth.cpp`: autenticación HTTP y soporte asociado.
- Headers en `plugins/nakama/include/nakama/`.

La configuración se carga desde `/var/db/mcp/nakama.json`. El loader valida la estructura, los permisos y la propiedad del archivo, el formato de URL, TLS y los límites de timeout. Los secretos no deben incluirse en documentación, commits ni logs.

**Estado observado en el daemon:**

- `configuration_state`: `VALID`.
- `transport_state`: `CONNECTED` después de establecer la conexión.
- `connected`: `true`.
- `nakama_ping`: `pong:true` y CID devuelto por Nakama.
- Una segunda sesión STDIO independiente pudo reutilizar la conexión mantenida por el daemon después de cerrar la primera sesión.

La persistencia entre clientes STDIO está verificada. La prueba sostenida de cinco minutos está completada: 10/10 rondas con `nakama_status` en CONNECTED y `nakama_ping` respondiendo `pong:true` en todas ellas.

### Plugins previstos

| Plugin | Responsabilidad | Estado |
|---|---|---|
| Network | Interfaces, direcciones, rutas, sockets y diagnóstico de red | Planificado |
| Security | Auditoría, controles, eventos y operaciones de seguridad | Planificado |
| Services | Estado y administración controlada de servicios FreeBSD | Planificado |
| Database | Operaciones autorizadas con PostgreSQL | Planificado |
| RAG | Recuperación de conocimiento y consultas contextuales | Planificado |

Los plugins de administración deben aplicar autorización explícita. Las operaciones destructivas o de alto impacto requerirán aprobación humana y registro de auditoría.

---

## 6. Seguridad y autorización

La autorización forma parte del diseño del Core y se implementa en el camino de ejecución, no como una comprobación confiada al cliente.

### Identidad local

El daemon se ejecuta como usuario dedicado `mcp` (UID 1003). Durante el desarrollo, `warbotz` (UID 1001) conserva acceso administrativo a través del cliente MCP. Ese acceso no se retirará hasta que la autonomía del daemon esté terminada y validada.

La identificación del cliente local se obtiene del peer de Unix IPC mediante `getpeereid()`. No se debe confiar en un UID, rol o capability declarado dentro de los parámetros JSON enviados por el cliente.

Política inicial de capacidades definida para las herramientas actuales:

| Identidad | UID | Capacidades previstas |
|---|---:|---|
| `warbotz` | 1001 | `system.read`, `hardware.read` y administración autorizada durante desarrollo |
| `mcp` | 1003 | `system.read`, `hardware.read` y capacidades internas necesarias para el daemon |
| Otros usuarios | Según peer | Denegado por defecto, salvo concesión explícita |

La matriz exacta de permisos debe mantenerse alineada con la política instalada y con los tests; no se debe inferir que todo el acceso administrativo está ya cerrado o finalizado.

### Controles implementados en el diseño/código

- Identidad obtenida del peer IPC.
- Comprobación de capacidades antes de ejecutar una herramienta.
- Filtrado de herramientas no autorizadas en `tools/list`.
- Denegación de llamadas directas no autorizadas.
- Separación entre identidad del peer y parámetros JSON.
- Capacidades declaradas por las herramientas de plugins.

### Pendiente de cerrar

- Completar la validación de integración de Authorization con todos los plugins y métodos MCP.
- Confirmar la matriz efectiva de UID/capabilities en el despliegue final.
- Auditoría de operaciones sensibles.
- Aprobación humana para acciones peligrosas.
- Pruebas negativas de suplantación de identidad y acceso directo.
- Revisión de permisos del socket, directorios y archivos de configuración.

La política es **deny by default**. El acceso de desarrollo de `warbotz` se conserva hasta validar la autonomía; no se elimina prematuramente.

---

## 7. Nakama y mensajería asíncrona

### 7.1 Transporte WebSocket

El transporte Nakama se implementa con libcurl WebSocket. Entre sus capacidades de código se encuentran:

- Conexión en modo `CONNECT_ONLY`.
- Envío y recepción TEXT/BINARY.
- Fragmentación y operaciones parciales.
- Gestión de `CURLE_AGAIN`.
- Colas acotadas de entrada y salida.
- Tamaño máximo de mensaje configurable.
- Polling y mecanismo de interrupción.
- Estados `STOPPED`, `CONNECTING`, `CONNECTED`, `CLOSING`, `ERROR`.
- Verificación TLS configurable.

Las pruebas previas documentadas incluyen 21 pruebas unitarias del transporte y una integración real con autenticación JWT, ping/pong con CID y desconexión limpia. Además, las pruebas operativas posteriores confirmaron estado conectado, ping correcto y persistencia de la conexión entre clientes STDIO.

### 7.2 Extensión ABI de mensajes asíncronos

El header ABI declara `MCP_PLUGIN_EXT_ASYNC_MESSAGES` y la tabla `mcp_async_messages_v1`. El código de `plugin_manager.cpp` incluye descubrimiento de la extensión y callbacks host-side para:

- `register_receiver`: el plugin registra un callback para mensajes Core → plugin.
- `unregister_receiver`: impide nuevas entregas y espera a callbacks activos.
- `publish`: el plugin publica mensajes hacia Core; el host copia los bytes antes de retornar.

El PluginManager también expone APIs para instalar/eliminar el handler Core y enviar mensajes a un plugin.

**Estado real:** la infraestructura de ABI y host-side está presente en el código. Falta completar la adopción del contrato desde Nakama, unirlo al AgentBus y verificar el flujo completo con tests de integración y concurrencia.

### 7.3 Arquitectura de integración deseada

```text
                    NAKAMA SERVER (.3)
                           |
                    WebSocket existente
                           |
                           v
                 Nakama WebSocketTransport
                           |
                 ReceiveDispatcher / CID
                           |
                           v
                  Nakama Plugin (ABI)
                           |
             Async Messages Extension v1
                           |
                           v
                 MCP Core PluginManager
                           |
                           v
                        AgentBus
                    /      |      \
                   /       |       \
             Registry  Protocol  AgentRPC
                   \       |       /
                    Agent transports
                           |
                  Monitor / Guardian /
                     Orchestrator
```

El sentido inverso utiliza el mismo canal: AgentBus → PluginManager → callback de Nakama → WebSocket ya conectado → Nakama server.

### Restricciones de diseño

1. No abrir una segunda conexión WebSocket para AgentBus.
2. AgentBus no debe depender del registro ni de la ejecución de herramientas MCP.
3. Mantener ABI 1.0 operativo para plugins existentes.
4. La extensión asíncrona debe ser opcional y descubrirse mediante `get_extension`.
5. Definir propiedad y vida útil de callbacks, subscriptions y buffers.
6. Definir seguridad de hilos, reentrancia y reglas de unregister.
7. Limitar tamaño de mensajes y capacidad de colas.
8. Definir backpressure, errores, cancelación y garantías de entrega.
9. Evitar que una entrega asíncrona bloquee llamadas normales de herramientas.
10. Probar mensajes entrantes/salientes, correlación, orden, desconexión y concurrencia.

---

## 8. AgentBus y protocolo de agentes

El Core ya contiene las interfaces y componentes `AgentBus`, `AgentRegistry`, `AgentProtocol`, `AgentTransport` y `AgentRPC`.

### Responsabilidades previstas

| Componente | Función |
|---|---|
| AgentProtocol | Formato, validación y semántica de mensajes |
| AgentRegistry | Identidad, registro y descubrimiento de agentes |
| AgentTransport | Contrato abstracto de transporte |
| AgentBus | Enrutamiento, entrega y despacho de mensajes |
| AgentRPC | Solicitudes/respuestas entre agentes y correlación |

El protocolo debe contemplar, como mínimo:

- Identidad del emisor y destinatario.
- `message_id`.
- `correlation_id` para request/response.
- Tipo de mensaje y versión del protocolo.
- Payload validado.
- Timestamp y metadatos necesarios.
- Errores y expiración de solicitudes.
- Mensajes dirigidos y eventos broadcast cuando estén autorizados.

La identidad autenticada del agente debe derivarse de la sesión/registro confiable, no aceptarse sin validar desde el payload.

**Estado:** componentes Core presentes; integración con Nakama pendiente. No declarar AgentBus operativo hasta probar el camino completo desde Nakama hasta el handler del Core y en sentido inverso.

---

## 9. Agentes especializados

Los agentes se ejecutarán en jails separadas y utilizarán MCP Core como interfaz controlada para las capacidades del host.

### Orden acordado

#### 1. Orchestrator — `192.168.100.6`

Responsabilidad:

- Coordinar tareas y agentes.
- Descomponer objetivos en operaciones.
- Decidir qué agente debe recibir cada tarea.
- Recoger resultados y correlacionar respuestas.
- Consultar llama.cpp en `192.168.100.1:8080` para inferencia local.
- Aplicar políticas de delegación y límites.

Orchestrator no debe saltarse la autorización del Core ni ejecutar operaciones privilegiadas directamente.

#### 2. Monitor — `192.168.100.4`

Responsabilidad:

- Supervisar recursos y estado del host.
- Consultar System y Hardware plugins.
- Supervisar servicios autorizados.
- Detectar anomalías operativas.
- Enviar eventos y métricas al sistema de agentes.
- Reportar al Orchestrator.

#### 3. Guardian — `192.168.100.5`

Responsabilidad:

- Analizar eventos de seguridad.
- Correlacionar señales y anomalías.
- Proponer respuestas y medidas de contención.
- Aplicar únicamente acciones expresamente autorizadas.
- Requerir aprobación humana para acciones peligrosas o disruptivas.
- Registrar decisiones y resultados.

Guardian se incorpora después de validar Orchestrator y Monitor. No adelantar su despliegue antes de tener identidad, permisos, auditoría y AgentBus probados.

**Estado de los tres agentes:** arquitectura y roles definidos; integración funcional pendiente.

---

## 10. PostgreSQL, persistencia y RAG

PostgreSQL se ejecutará en `192.168.100.2`.

### Función de PostgreSQL

- Persistencia duradera de datos de agentes.
- Registro de agentes, sesiones y metadatos.
- Historial de mensajes y operaciones relevantes.
- Estado de tareas y resultados.
- Auditoría y trazabilidad.
- Datos de conocimiento para RAG.

### pgvector y RAG

- Incorporar pgvector.
- Almacenar embeddings y metadatos.
- Indexar documentación y conocimiento autorizado.
- Recuperar contexto relevante para agentes.
- Aplicar filtros por identidad, ámbito y permisos.
- Mantener separación entre memoria operacional y conocimiento documental.

Nakama se utiliza para realtime y presencia; PostgreSQL para persistencia duradera. No duplicar responsabilidades ni usar el historial efímero de mensajes como sustituto de almacenamiento transaccional.

**Estado:** arquitectura planificada; plugin Database, esquema persistente, pgvector y pipeline RAG pendientes.

---

## 11. llama.cpp e inferencia local

Servicio previsto en el host:

`192.168.100.1:8080`

Responsabilidad:

- Inferencia local para Orchestrator y agentes autorizados.
- Procesamiento de contexto recuperado mediante RAG.
- Generación de planes y análisis.
- Evitar depender de un proveedor remoto para la inferencia principal.

La inferencia no otorga permisos. Toda herramienta solicitada por un agente debe pasar por autorización MCP. La salida del modelo se trata como entrada no confiable y se valida antes de ejecutar acciones.

**Estado:** endpoint y ubicación definidos en la arquitectura; integración de agentes con el servicio pendiente.

---

## 12. Plugins y servicios adicionales

| Plugin | Funciones previstas | Estado |
|---|---|---|
| Network | Interfaces, rutas, direcciones, sockets y diagnóstico | Planificado |
| Security | Auditoría, controles, eventos y revisión de seguridad | Planificado |
| Services | Supervisión y administración controlada de servicios rc.d | Planificado |
| Database | Operaciones autorizadas con PostgreSQL | Planificado |
| RAG | Indexación y recuperación de conocimiento | Planificado |

La implementación debe seguir el modelo de plugins nativos, con schemas, capacidades y límites claros. No introducir un plugin monolítico que concentre todas las funciones.

---

## 13. Clientes y administración

### STDIO Bridge e IPC Client — implementados

Son la vía actual de conexión de clientes MCP al daemon:

- El cliente inicia `mcp_core --stdio`.
- El bridge conecta con el Unix socket.
- Las solicitudes JSON-RPC se reenvían al daemon.
- La respuesta vuelve al cliente.
- Cerrar el bridge no debe descargar plugins ni cerrar conexiones propiedad del daemon.

### mcpctl — planificado

Cliente administrativo de terminal para:

- Consultar estado del daemon.
- Listar plugins y herramientas.
- Consultar salud y diagnóstico.
- Administrar operaciones autorizadas.
- Consultar estado de agentes y transportes.
- Facilitar tareas de operación sin depender de un cliente MCP genérico.

### Control Center — planificado

Interfaz centralizada para:

- Estado del Core y plugins.
- Estado de Nakama y AgentBus.
- Agentes conectados y presencia.
- Eventos y registros.
- Supervisión de servicios.
- Administración de permisos.
- Operaciones aprobadas y trazabilidad.

No se considera implementado hasta existir código, pruebas y despliegue verificables.

---

## 14. Despliegue actual en FreeBSD

### Daemon

Estado observado en el host:

- Ejecutable: `/usr/local/sbin/mcp_core`
- Modo: `--daemon`
- Plugin directory: `/usr/local/lib/mcp/plugins`
- Socket: `/var/run/mcp/mcp.sock`
- Usuario de ejecución: `mcp` (UID 1003)
- Grupo de acceso: grupo `mcp`
- `warbotz` (UID 1001) conserva acceso durante desarrollo y validación de autonomía.

### Configuración Nakama

- Archivo: `/var/db/mcp/nakama.json`
- Directorio protegido, propiedad de `mcp`, modo `0700`.
- Archivo propiedad de `mcp`, modo `0600`.
- El loader aplica validaciones estrictas de tamaño, tipo, propiedad, permisos, estructura JSON y URL/TLS.
- No mostrar ni versionar el contenido secreto del archivo.

### Build de MCP Core en FreeBSD

El repositorio dispone de Makefiles nativos BSD make en la raíz y en `core/`.
El Makefile raíz construye Core y delega los plugins a sus Makefiles propios,
sin sustituirlos ni modificar sus reglas.

Comandos desde la raíz:

- `make`: compila `build/bin/mcp-core`.
- `make test`: compila y ejecuta los tests de Core.
- `make test-agent`: ejecuta las pruebas del Orchestrator, Planner y Validator.
- `make test-nakama`: ejecuta la integración local de `NakamaAgentTransport`
  mediante un plugin fixture aislado.
- `make test-plugins`: ejecuta pruebas ABI y PluginManager.
- `make plugins`: compila hardware, system y Nakama.
- `make clean`: elimina únicamente el árbol generado `build/`.

Los objetos, biblioteca estática, ejecutable, binarios de test y fixtures
se generan bajo `build/`. Los fixtures se aíslan del directorio
`core/tests/plugins` del código fuente. La compilación no implica instalación
ni despliegue; reemplazar bibliotecas instaladas o reiniciar servicios
requiere un procedimiento separado y autorizado.

### Nota sobre versiones desplegadas

Se ha observado anteriormente una diferencia entre la biblioteca Nakama compilada localmente y la biblioteca instalada en `/usr/local/lib/mcp/plugins`. Por ello, compilar no equivale a desplegar. Antes de reemplazar una biblioteca hay que comprobar revisión, hash, dependencias y proceso que la tiene cargada.

La instalación futura debe utilizar staging, verificación y un procedimiento de reinicio/rollback explícito. No reemplazar una biblioteca que esté cargada por el daemon.

---

## 15. Estructura real del repositorio publicado

El repositorio `main` contiene actualmente:

```text
mcp-core/
├── .gitignore
├── README.md
├── TODO.md
├── core/
│   ├── include/mcp/
│   │   ├── agent_bus.hpp
│   │   ├── agent_protocol.hpp
│   │   ├── agent_registry.hpp
│   │   ├── agent_rpc.hpp
│   │   ├── agent_transport.hpp
│   │   ├── authorization.hpp
│   │   ├── ipc_client.hpp
│   │   ├── ipc_transport.hpp
│   │   ├── json_rpc.hpp
│   │   ├── plugin_abi.h
│   │   ├── plugin_manager.hpp
│   │   ├── request_handler.hpp
│   │   ├── stdio_bridge.hpp
│   │   └── stdio_transport.hpp
│   ├── src/
│   │   ├── agent_bus.cpp
│   │   ├── agent_protocol.cpp
│   │   ├── agent_registry.cpp
│   │   ├── agent_rpc.cpp
│   │   ├── authorization.cpp
│   │   ├── ipc_client.cpp
│   │   ├── ipc_transport.cpp
│   │   ├── json_rpc.cpp
│   │   ├── main.cpp
│   │   ├── plugin_manager.cpp
│   │   ├── request_handler.cpp
│   │   ├── stdio_bridge.cpp
│   │   └── stdio_transport.cpp
│   └── tests/
│       ├── authorization_test.cpp
│       ├── core_integration_test.cpp
│       ├── ipc_client_test.cpp
│       ├── ipc_transport_test.cpp
│       ├── json_rpc_test.cpp
│       ├── plugin_manager_test.cpp
│       ├── request_handler_test.cpp
│       ├── stdio_bridge_test.cpp
│       ├── stdio_transport_test.cpp
│       └── test_plugin.cpp
└── plugins/
    ├── hardware/
    │   ├── Makefile
    │   └── hardware_plugin.cpp
    ├── system/
    │   ├── Makefile
    │   └── system_plugin.cpp
    └── nakama/
        ├── Makefile
        ├── include/nakama/
        │   ├── http_auth.hpp
        │   ├── receive_dispatcher.hpp
        │   └── websocket_transport.hpp
        ├── src/
        │   ├── http_auth.cpp
        │   ├── nakama_plugin.cpp
        │   ├── receive_dispatcher.cpp
        │   └── websocket_transport.cpp
        └── tests/
            ├── http_auth_test.cpp
            ├── mock_websocket_server.cpp
            ├── mock_websocket_server.hpp
            ├── nakama_abi_test.cpp
            ├── nakama_config_loader_test.cpp
            ├── nakama_config_validation_test.cpp
            ├── nakama_live_integration.cpp
            ├── nakama_plugin_manager_test.cpp
            ├── websocket_transport_test.cpp
```

Esta es la estructura **real publicada**. Network, Security, Services, Database, RAG, agentes, mcpctl y Control Center son partes de la arquitectura global, pero todavía no aparecen como módulos implementados en este árbol.

---

## 16. Orden de trabajo acordado

El orden se actualiza según el estado real actual: el Nakama Plugin ya existe y está integrado; no se debe volver a tratar como si todavía faltara crearlo.

### Paso 1 — Cerrar estabilidad de Nakama

**Estado: comprobaciones básicas correctas; test sostenido pendiente.**

- Confirmar las diez rondas de status/ping durante cinco minutos.
- Verificar que el transporte permanezca `CONNECTED`.
- Confirmar `pong:true` y CID en cada ronda.
- Investigar cualquier silencio, bloqueo, error o desconexión.
- Probar después desconexión remota, reconexión y llamadas repetidas sin conexiones duplicadas.

### Paso 2 — Completar ABI de mensajería asíncrona

**Estado: infraestructura parcial en Core.**

- Revisar y cerrar el contrato `MCP_PLUGIN_EXT_ASYNC_MESSAGES`.
- Confirmar compatibilidad con plugins ABI 1.0 sin extensión.
- Verificar registro, entrega, unregister y publicación.
- Definir límites de payload, colas, backpressure y errores.
- Añadir tests de callbacks activos, carreras y fallos.

### Paso 3 — Conectar Nakama Plugin con AgentBus

**Estado: pendiente de integración end-to-end.**

- Implementar el adaptador `NakamaAgentTransport`.
- Reutilizar el WebSocket existente del plugin.
- Conectar mensajes entrantes al AgentBus.
- Enviar mensajes de AgentBus por el canal Core → plugin.
- Mantener AgentBus desacoplado de MCP tools.
- Probar ambos sentidos, correlación y concurrencia.

### Paso 4 — Validar AgentBus y Runtime de agentes

**Estado: interfaces Core presentes; runtime integrado pendiente.**

- Registro e identidad.
- Descubrimiento.
- Protocolo y validación de mensajes.
- RPC y correlación.
- Eventos y presencia.
- Permisos por agente.
- Reconexión y recuperación.

### Paso 5 — Orchestrator y Monitor

**Estado: pendiente.**

- Orchestrator primero, integrado con llama.cpp.
- Monitor después, con acceso de solo lectura inicialmente.
- Validar comunicación entre ambos a través de AgentBus.

### Paso 6 — Guardian

**Estado: pendiente.**

- Integrar después de Orchestrator y Monitor.
- Añadir análisis de seguridad, respuesta controlada y auditoría.
- Exigir aprobación humana para acciones peligrosas.

### Paso 7 — PostgreSQL, pgvector y RAG

**Estado: pendiente.**

- Plugin Database.
- Esquema persistente.
- Datos de agentes, tareas, mensajes y auditoría.
- pgvector, embeddings e indexación.
- Pipeline RAG con filtros de autorización.

### Paso 8 — Plugins Network, Security y Services

**Estado: pendiente.**

Desarrollar cada plugin de forma independiente y con capacidades explícitas.

### Paso 9 — mcpctl y Control Center

**Estado: pendiente.**

Herramientas de operación, visualización, diagnóstico y administración autorizada.

### Paso 10 — Auditoría, despliegue reproducible y entrega

**Estado: pendiente.**

- Pruebas de carga y concurrencia.
- Pruebas de reconexión y recuperación.
- Auditoría de permisos y límites.
- Verificación de servicios rc.d.
- Build e instalación reproducibles.
- Prueba de autonomía del daemon.
- Limpieza de artefactos temporales al final.
- Documentación y release.

---

## 17. Ciclo de vida y apagado seguro

Existe un camino básico de terminación del daemon y llamadas a `shutdown` de plugins. Sin embargo, el apagado ordenado completo del PluginManager está deliberadamente aplazado.

No implementarlo antes de estabilizar el canal de mensajería asíncrona. La solución final deberá:

1. Dejar de aceptar nuevas operaciones.
2. Detener nuevas entregas de mensajes.
3. Esperar llamadas y callbacks activos con timeout definido.
4. Drenar o cancelar colas según política explícita.
5. Ejecutar `shutdown` de plugins en orden seguro.
6. Descargar bibliotecas solo cuando no quede código activo dentro de ellas.
7. Liberar IPC y recursos del daemon.
8. Cubrir carreras, timeout y errores en tests.

---

## 18. Pruebas y criterios de aceptación

Los tests forman parte del desarrollo y no deben eliminarse antes de tiempo.

### Cobertura existente en el repositorio

- Core: autorización, integración, IPC client/server, JSON-RPC, Plugin Manager, RequestHandler, STDIO bridge y STDIO transport.
- Nakama: HTTP auth, WebSocket transport, ABI, carga/configuración, validación de configuración, integración con PluginManager y prueba live.
- Mock WebSocket server para pruebas controladas.

### Nuevas pruebas requeridas

- Plugins ABI 1.0 antiguos sin extensión async.
- Extensión ausente, incompatible o malformada.
- Mensajes Core → plugin y plugin → Core.
- Registro y baja de callbacks con entregas activas.
- Payloads vacíos, grandes y malformados.
- Backpressure y saturación de colas.
- Desconexión Nakama y recuperación.
- Entrega concurrente con llamadas MCP normales.
- Correlación y orden de mensajes.
- Fallo de plugin y aislamiento.
- Autorización negativa y suplantación de identidad.
- Shutdown seguro una vez que esa fase sea autorizada.

Los números históricos de tests (incluidos los 21 tests del transporte y las comprobaciones globales previas) son resultados de ejecuciones anteriores; no deben presentarse como resultado de la última revisión completa hasta repetir la suite sobre la revisión candidata.

---

## 19. Reglas permanentes del proyecto

- FreeBSD es la plataforma principal; mantener implementaciones nativas.
- ABI 1.0 debe seguir funcionando.
- No crear una segunda conexión WebSocket de Nakama para AgentBus.
- AgentBus no debe depender del registro ni de la ejecución de herramientas MCP.
- El daemon se ejecuta como `mcp`.
- `warbotz` mantiene acceso administrativo hasta validar la autonomía completa.
- No implementar todavía el graceful shutdown completo.
- No retirar tests ni plugins de prueba antes del cierre del desarrollo.
- No desplegar una biblioteca nueva ni reiniciar el daemon sin un plan validado.
- Revisar los diffs antes de hacer commits.
- No publicar secretos, JWT, claves, configuración privada ni logs sensibles.
- Mantener separados los hechos verificados, el código existente y la arquitectura futura.
- Seguir el orden de fases acordado; no saltar directamente a agentes o RAG antes de cerrar Nakama/AgentBus.

---

## 20. Evaluación global

MCP Core ya superó la fase de un servidor MCP experimental: existe un daemon nativo desplegado, un sistema de plugins, transporte local por Unix IPC, clientes STDIO, plugins System y Hardware, autorización en desarrollo/integración y un plugin Nakama operativo con conexión WebSocket persistente.

El siguiente cuello de botella ya no es construir el plugin Nakama desde cero. Es **cerrar la estabilidad del transporte y completar el puente de mensajería asíncrona entre Nakama, PluginManager y AgentBus**.

Los tres objetivos técnicos prioritarios son:

1. Cerrar las pruebas de estabilidad y ciclo de vida de Nakama.
2. Completar y probar la extensión ABI asíncrona sin romper ABI 1.0.
3. Integrar Nakama con AgentBus usando la conexión WebSocket existente.

Después se podrá construir el Runtime de agentes en el orden acordado: Orchestrator, Monitor y finalmente Guardian. PostgreSQL/pgvector/RAG, plugins adicionales y herramientas de administración vendrán después.

**Punto exacto de trabajo actual:** Nakama está integrado y operativo. La prueba sostenida de cinco minutos se ha superado con 10/10 rondas correctas. El siguiente hito es auditar y completar la integración de la mensajería asíncrona entre Nakama, PluginManager y AgentBus.
