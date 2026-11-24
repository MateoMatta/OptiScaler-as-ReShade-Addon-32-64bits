# Traspaso para segunda opinión: OptiScaler, ReShade y diagnóstico DX11

**Actualizado:** 24 de noviembre de 2026, después de la prueba F2. **Audiencia:** otra IA o una persona
con acceso autorizado a los repositorios y, si están disponibles, los logs
locales. Contrasta cada conclusión con el código y los archivos reales.

## 1. Objetivo del proyecto

Se investiga cómo ejecutar escalado basado en OptiScaler con un juego DirectX 9
de 32 bits. OptiScaler y el add-on actual son x64: un proceso x86 no puede
cargar directamente un módulo x64. La posibilidad futura es un componente x86
de captura, transporte de recursos GPU y un host x64. El proyecto
[DLSS5-Reshade-AIO](https://github.com/kibblerz/DLSS5-Reshade-AIO) es una
referencia para el carrier, pero su host no ejecuta OptiScaler automáticamente.
También se desea estudiar Vulkan y las rutas dgVoodoo2 y DXVK. Convertir una API
gráfica no convierte por sí solo el proceso del juego en x64.

**Estado real:** el add-on es una sonda ReShade DX11 x64. No invoca un backend
OptiScaler ni escala imágenes. No se ha implementado el puente x86→x64 ni se ha
demostrado compatibilidad general con juegos o Vulkan.

## 2. Orden acordado

1. Etapas **1–4**: add-on de diagnóstico y baseline DX11. Aún en progreso.
2. Etapas **5–12**: integrar y comprobar un add-on x64 funcional con un backend
   real. El usuario ya autorizó las etapas posteriores, pero aún no se han implementado.
3. Etapa **13**: puente IPC x86→x64, después de que 5–12 funcionen.
4. Etapas **14–15**: posteriores al puente.

No se conserva en el contexto disponible la lista original literal ni criterios
de aceptación exhaustivos para cada número. No los inventes: propón al usuario
criterios finitos, concretos y medibles, sin afirmar cobertura universal.

## 3. Rutas y código relevante

La raíz del espacio de trabajo no es un repositorio Git:

`D:\Descargas\Proyecto compatiblidad DirectX - Vulkan Optiscaler`

- Fork del add-on:
  `Extra\OptiScaler-as-ReShade-Addon-32-64bits`
  - `OptiScaler\dllmain.cpp`: callbacks ReShade, logger, recursos y swapchains.
  - `OptiScaler\AddonResourceSnapshot.h`: inspección de vistas.
  - `OptiScaler\AddonSwapchainEpochs.h`: asociación de épocas de swapchains.
  - `tests\addon_swapchain_epochs_test.cpp`: control sintético de esa lógica.
  - `README.md`: configuración y limitaciones.
- Demo:
  `Extra\Probe-Diagnostics-ngx_dlss_demo`
  - `ngx_dlss_demo\DemoMain.cpp`: frame loop y activación de diagnósticos.
  - `ngx_dlss_demo\TemporalDiagnostics.cpp` y `.h`: trazas, readback y pruebas.
  - `ngx_dlss_demo\IdentitySelfTest.cpp`: identidad sintética.
  - `dx11-diagnostic-harness.ps1`: orquestación y aserciones.
  - `README.md`: configuración, alcance y opciones.
  - Repositorio: https://github.com/MateoMatta/Probe-Diagnostics-ngx_dlss_demo
- `Extra\DLSS5-Reshade-AIO`: referencia para el carrier, no backend OptiScaler.

Revisar `git status` por separado y preservar cambios del usuario. Hay una
modificación preexistente en `external\FidelityFX-SDK-v2` del fork: no revertir.
No hacer commits ni push salvo solicitud expresa. No iniciar pruebas en juegos
ajenos: How to Fish se mantiene pospuesto.

## 4. Qué registran la sonda y la demo

- El add-on observa dispositivos, swapchains, recursos, vistas y bindings DX11
  comunicados por ReShade. Su logger independiente tiene cola acotada y un
  resumen de pérdidas, fallos de escritura y truncamientos al cierre normal.
- `OPTISCALER_PROBE_RESOURCE_LIFETIMES=1` habilita correlación optativa de
  generaciones para inicializaciones observadas. Si falta la inicialización, la
  observación queda `generation=0 status=unknown`. Las inicializaciones
  repetidas pueden ocurrir según la API de ReShade; el código actual registra
  como error un init de una generación activa y requiere más investigación.
- Una **época de buffer de swapchain** corresponde al intervalo observado entre
  callbacks de inicialización y destrucción/resize para un slot. Un binding
  solo se asocia si encuentra una coincidencia única; varias son ambiguas.
  Una época no es prueba de creación/destrucción COM ni sincronización GPU.
- `OPTISCALER_DEMO_TRACE=1` habilita `TemporalInputs_<pid>_<counter>.log`.
  El harness lo activa en todos los casos y restaura el entorno al salir.
  La demo instrumenta TAA/DLSS, dimensiones, jitter, resets, cámara, objetos,
  descriptores y readback optativo.
- Límite normal 240 FPS; modo de estrés 300 FPS mediante `-StressFps` en el
  harness. Logging exhaustivo y readback perturban el rendimiento.
- QPC, contadores de present y orden de callbacks son evidencia de CPU, no una
  fence GPU ni identificadores universales de frames.

## 5. Harness y reproducción

La suite predeterminada contiene **17 casos**: estático, cámara, objetos,
control coplanar, identidad sintética, stencil/reset, transiciones de modo,
resize TAA/DLSS, consultas GPU, dos swapchains adicionales, registro tardío de
una segunda sonda con un recurso preexistente, correlación de
frame y captura exhaustiva optativa de bindings de targets y shaders.

Desde la raíz de la demo, con ReShade `dxgi.dll`, el add-on y `OptiScaler.ini`
en la carpeta del ejecutable:

```powershell
$demo = (Get-Location).Path
.\dx11-diagnostic-harness.ps1 `
  -Executable (Join-Path $demo 'bin\ngx_dlss_demo\ngx_dlss_demo_diagnostics.exe') `
  -MediaPath (Join-Path $demo 'media') `
  -OutputPath (Join-Path $demo 'diagnostic-results\run-001') `
  -SecondsPerCase 12
```

La carpeta de salida no debe existir. Hay selectores como `-LateAddonOnly`, `-MultiSwapOnly`,
`-GpuPipelineOnly`, `-ResizeOnly` y `-ResizeDlssOnly`, documentados en el
README. El harness lanza y cierra sus propios procesos, archiva logs y falla si
faltan sus aserciones. No tratar logs detallados como benchmark.

El selector `-CombinedOnly` ejecuta adicionalmente los controles intrusivos de
color, exposición, salida DLSS, influencia en tone mapping, back buffer y
profundidad; **no forma parte de los 17 casos predeterminados**. Las
compilaciones A, A2, B, C, D, E, F y F2 se guardan por identificador en
`Extra\OptiScaler-as-ReShade-Addon-32-64bits\x64\Release\a\Compilations`.
Cada manifiesto indica hashes, reutilización del add-on y evidencias. No
sobrescribir ni eliminar versiones. F es un intento fallido que debe conservarse.

Compilación de la demo tras su renombrado: el antiguo `sample-build` apunta a
la ruta anterior y no debe reutilizarse sin reconfigurar. Esta sesión configuró
`...\files\sample-build-renamed` con CMake de Visual Studio, DX11 ON, DX12 y
Vulkan OFF, el SDK NGX ya existente y `__nvapi_install` apuntando a
`Extra\OptiScaler-as-ReShade-Addon-32-64bits\external\nvapi`. Se necesitó
`-DCMAKE_POLICY_VERSION_MINIMUM=3.5` y un `DXC_BIN_SPIRV` ya disponible. No se
instalaron dependencias nuevas. La demo compiló con advertencias de conversión
preexistentes.

## 6. Evidencias verificadas

### Identidad y bindings

- Control de identidad propio y aislado: 14 casos, 106.496 comparaciones sin
  discrepancias. No es un buffer de ID universal de Sponza o juegos. El
  seguimiento transparente cubre contribuyentes conocidos; no representa todos
  los fragmentos o su orden general. El hash de una región no es identidad.
- En una ruta TAA concreta se correlacionaron `CS t1=motion`,
  `CS t2=feedback input` y `CS u1=feedback output` en el intervalo CPU
  identificado. Esto no demuestra todos los accesos GPU ni todos los bindings
  posibles.
- La inspección anterior con RenderDoc Pixel History se hizo sobre un píxel
  real de la demo; no demuestra identidad general o solapamientos completos.

### Controles sintéticos de épocas

`tests\addon_swapchain_epochs_test.cpp` comprueba, con el mismo helper usado
por la sonda, recursos desconocidos, slots duplicados, dos dispositivos,
coincidencias ambiguas, eliminación y reutilización del mismo handle. Pasó
compilado con MSVC `/W4 /WX`. Es una prueba de lógica en memoria, no una
demostración de que DXGI reutilizó realmente una dirección.

### Dos swapchains DXGI reales — nuevo

Con `-MultiSwapOnly`, la demo creó **dos swapchains ocultas adicionales** en el
mismo dispositivo que la principal. Las dos existieron simultáneamente; se
redimensionó la primera de 64×64 a 96×64 mientras la otra permanecía 64×64.
El harness correlacionó sus HWND con callbacks ReShade distintos, verificó
épocas de inicio/fin, el resize únicamente de la primera y su destrucción.
Resultado: PASS; ReShade observó tres inicializaciones iniciales en total,
incluyendo la swapchain principal.

Alcance: no se llamó Present en las dos swapchains auxiliares, no se verificó
renderizado ni múltiples dispositivos, y la prueba no forzó reutilización
real de handles. Ventanas y swapchains se liberaron al terminar el control.

### Consultas GPU — nuevo

El caso `-GpuPipelineOnly` emite ocho `D3D11_QUERY_EVENT` después de la ruta
TAA. Ahora consulta inmediatamente cada una con `D3D11_ASYNC_GETDATA_DONOTFLUSH`,
y distingue `not_ready` de `complete` y de las consultas aún no retiradas de
la estructura CPU. Al final realiza un flush y espera acotada.

Último resultado: ocho consultas `not_ready` en el primer sondeo, ocho
completadas posteriormente, y dos elementos aún no retirados antes del primer
drenaje. **S_FALSE significa resultado no disponible**; sin flush, el trabajo
podría no haberse enviado aún. No demuestra que la GPU ya lo estuviera
ejecutando, simultaneidad de frames en GPU, ni sincronización entre procesos.

### Suite completa

Después de los cambios se ejecutó la suite F2: **17/17 casos PASS**, incluyendo
TAA y DLSS con resize y cierre limpio de logs. Evidencias:

`C:\Users\mateo\.copilot\session-state\b9da036c-d5c4-445d-a754-582e1d4d7ff7\files\harness-combined-F2-full`

Pruebas focalizadas adicionales: `harness-multiswap-v3` y
`harness-gpu-immediate-v3` en la misma carpeta `files`. Después de la suite
completa se reforzaron las aserciones de solapamiento temporal y recuentos, y
se ejecutaron de nuevo esos dos casos focalizados. Estas rutas son locales y
pueden no estar disponibles para otra IA; solicitar logs o acceso si faltan.

### Recurso real creado antes del registro de una segunda sonda

En `-LateAddonOnly`, la demo crea una textura DX11 32×32 y su RTV con el primer
add-on ya activo. A continuación carga manualmente una copia aislada de la
misma sonda, cambia su nombre exportado de prueba para evitar la prohibición de
nombres duplicados de ReShade y registra esa copia con el ReShade real. Luego
crea una segunda textura de control y hace binding de ambas; restaura los RTV y
DSV previos y desregistra la copia. La primera sonda observó la creación de la
primera textura. La segunda no recibió esa creación anterior y registró
`generation=0 status=unknown`, mientras que para el control posterior registró
una generación positiva y `status=live`. El harness comprueba orden QPC,
identidad, ausencia de creación inventada, control positivo y cierre limpio.
Resultado: PASS, también dentro de la suite completa.

Esta prueba es real para DX11 y el ReShade de la demo, pero **el registro
tardío fue explícito y manual**; no demuestra que ReShade recargue add-ons
automáticamente o que todos los recursos de juegos se puedan recuperar.
La destrucción del control no se exige a la sonda tardía: en una ejecución
quedó diferida hasta después de su desregistro, aunque la primera sonda sí la
observó. No atribuir el fin real de la vida al simple `Reset` de un `ComPtr`.

### Color, salida, profundidad y prueba combinada F2

Las versiones A2 y B leen el color RGBA16F y exposición R16F suministrados a
NGX, con controles de valores conocidos. C verifica que ocho imágenes DLSS
sobrescriben un centinela; D verifica influencia causal en el resultado del
tone mapping al sustituir temporalmente la salida por negro y restaurarla.
E comprueba los bindings del blit y contenido del back buffer **antes de
Present**, además de clears y vistas D24S8 y D32.

F añadió D16 y un clear solo de stencil que preserva profundidad. El primer
intento de comprobar sRGB mediante `ClearRenderTargetView` falló: tanto la
vista lineal como la sRGB devolvieron 64 para entrada 0.25. No extrapolar ese
resultado a un draw. F2 reemplazó esa prueba por un shader en un dispositivo
DX11 separado con el mismo adaptador, y comprueba bytes lineales y sRGB y alfa.
El caso `-CombinedOnly` F2 pasó. Este control de output merger no prueba
decodificación sRGB por SRV, la ruta de color de la demo, ni HDR de pantalla.
El control de presentación sigue siendo previo a Present: ni un callback ni
la secuencia CPU prueban el píxel finalmente presentado en el monitor.

Evidencias locales: `...\files\harness-combined-F-01`,
`...\files\harness-combined-F2-01` y `...\files\harness-combined-F2-full`.
La suite predeterminada y el caso combinado deben evaluarse por separado.
F y F2 reutilizan el add-on anterior; el manifiesto de F indica que no se
conservó una instantánea exacta de su fuente. F2 incluye archivos fuente y
diff. La pantalla HDR física está pospuesta por solicitud del usuario.

### Inventario de probes: probado y pendiente

Leyenda: ✅ funcional y probado **solo en el alcance indicado**; ⚠️ probado
parcialmente; ⏳ pendiente; ⏸️ pospuesto. Una marca ✅ no implica cobertura
universal ni significa que OptiScaler ya haga escalado.

| Estado | Recurso o mecanismo | Evidencia y alcance |
|---|---|---|
| ✅ | Registro tardío y recurso preexistente | Una textura/RTV real fue `unknown` para la copia registrada manualmente después; la textura de control posterior fue `live`. |
| ✅ | Profundidad | Readback y controles conocidos en la demo; F2 incluye D24S8, D32 y D16 en texturas de control. |
| ✅ | Vectores de movimiento | Muestras de cámara y objetos, reproyección y un control sintético de valores conocidos. |
| ✅ | Historial TAA y vistas SRV/UAV | Correlación de `CS t1`, `CS t2` y `CS u1` para una ruta concreta de la demo. |
| ✅ | IDs sintéticos por píxel | Controles de instancias, solapamiento, transparencia y MSAA en geometría propia; no es una identidad universal. |
| ✅ | Vidas útiles y resize | Eventos y generaciones observados durante cambios de resolución de TAA y DLSS. |
| ✅ | Swapchains auxiliares | Dos swapchains DXGI reales en el mismo dispositivo; resize de una sin cambiar la otra, sin presentarlas. |
| ✅ | Consultas GPU | Ocho consultas no listas inicialmente y completadas después; no demuestra simultaneidad de frames. |
| ✅ | Color DLSS de entrada | Lectura RGBA16F de la región activa, correlación con NGX y control sintético con offset y valores conocidos. |
| ✅ | Exposición de la demo | Lectura de la textura R16F 1×1 y correlación con NGX; control sintético de 0,5. |
| ✅ | Salida DLSS y tone mapping | Se sobrescribió un centinela; sustituir temporalmente la salida por negro cambió el resultado del tone mapping y restaurarla recuperó el resultado original. |
| ✅ | Codificación sRGB por RTV | Shader en dispositivo separado, mismo adaptador: RGB 0,25 produjo 64 lineal y 137 sRGB; alfa 0,5 produjo 127 en ambos. |
| ✅ | Harness y logs | Suite predeterminada F2 17/17; caso `-CombinedOnly` F2 aprobado aparte. Se comprueba cierre del logger en los casos aplicables. |
| ⚠️ | Depth/stencil y vistas | Faltan comparaciones, máscaras, operaciones de dibujo, formatos y estados adicionales; los clears no validan todos los estados. |
| ⚠️ | Back buffer y bindings | Se leyó y correlacionó el back buffer antes de `Present`; no se comprobó la imagen finalmente presentada. |
| ⚠️ | Color y exposición | No se ha validado la ruta completa de color de la demo, decodificación sRGB por SRV, preexposición ni semánticas de otros backends. |

| Estado | Cobertura de probe pendiente | Próxima evidencia requerida |
|---|---|---|
| ⏳ | Imagen ya presentada | Observación externa correlacionada y control causal después de Present; no sustituirla por callback CPU. |
| ⏳ | Preexposición | Entradas conocidas y convención específica para el backend integrado. |
| ⏳ | Recursos anteriores al registro | Variantes SRV, UAV, profundidad y destrucción diferida. |
| ⏳ | Identidad y vidas útiles | Reutilización real de handles, aliasing, referencias retenidas y eventos concurrentes/intercalados. |
| ⏳ | Dispositivos y contextos | Separación de identidades y eventos entre dispositivos y contextos inmediatos y diferidos. |
| ⏳ | Subrecursos y vistas | Mips, slices, arrays, cubemaps, texturas 3D y múltiples vistas de un recurso. |
| ⏳ | MSAA | Cobertura parcial de bordes y geometrías/estados adicionales. |
| ⏳ | Depth/stencil | Comparaciones, escrituras, máscaras y operaciones de stencil en dibujos; otros formatos. |
| ⏳ | Movimiento | Signo, píxeles/NDC, resoluciones, jitter, disoclusiones y más tipos de objetos. |
| ⏳ | Transparencia y partículas | Fragmentos y contribuciones que un solo ID por píxel no conserva. |
| ⏳ | Sincronización | Envío, finalización, frames en vuelo y resize/destrucción con trabajo GPU pendiente. |
| ⏳ | Resolución e historial | Más resoluciones dinámicas, regiones activas, recreaciones y resets. |
| ⏳ | Rendimiento | Medición separada de readbacks bloqueantes y pruebas intrusivas. |
| ⏸️ | Pantalla HDR física | Pospuesta por el usuario; controles sintéticos no validan presentación HDR real. |

| Estado | Etapa posterior autorizada | Falta |
|---|---|---|
| ⏳ | Backend OptiScaler | Integración real y prueba de escritura, consumo e historial propios. |
| ⏳ | Máscaras reactivas y de composición | Disponibilidad y semánticas concretas del backend. |
| ⏳ | Puente x86→x64 | Transporte de recursos, formatos, propiedad, sincronización y recuperación. |
| ⏳ | Vulkan | Imágenes, vistas, layouts y sincronización; no extrapolar desde DX11. |
| ⏳ | HUD/UI | Separación cuando la funcionalidad elegida lo requiera. |
| ⏳ | Frame generation y ray reconstruction | Recursos y metadatos específicos, si se integran. |

Jitter, resolución, matrices, escala de movimiento, tiempo de frame y reset
son parámetros o metadatos; queries y fences son mecanismos de sincronización.
Validarlos junto con las texturas sin clasificarlos como recursos equivalentes.

## 7. Pendientes y bloqueos honestos

1. Ampliar el caso de registro tardío a SRV, UAV, profundidad y destrucción
   diferida; evaluar carga tardía nativa o habilitación de add-ons según la
   versión real de ReShade, sin asumir que todos los dispositivos se enumeran.
2. Múltiples swapchains: ampliar a presentación y bindings reales, múltiples
   buffers, resize fallido, destrucciones o recreaciones, y múltiples
   dispositivos cuando proceda. No extrapolar del test de dos swapchains ocultas.
3. Investigar la semántica real de inicializaciones de recurso repetidas y
   destrucción bajo la versión de ReShade presente, incluidos callbacks
   concurrentes y reutilización de dirección. No atribuir una nueva generación
   sin evidencia.
4. Sincronización: diseñar controles de envío/completado/fallo que tengan en
   cuenta flush, ausencia de datos y perturbación de polling. Separar comandos
   no enviados, resultados no disponibles y trabajo GPU realmente completado.
   No prometer prueba de simultaneidad con `S_FALSE` solo.
5. El aviso de linker preexistente `LNK4098` permanece pospuesto por decisión
   del usuario. En el fork, el backend real no está integrado y no debe
   confundirse éxito de NGX con producción y consumo de una imagen.
6. La demo usa componentes NVIDIA/Donut antiguos sujetos a sus propios términos.
   La licencia MIT de Donut público no cambia automáticamente la licencia de
   esa copia. Revisar derechos antes de redistribuir; esto no es asesoría legal.

## 8. Instrucción para la IA que dará segunda opinión

Actúa como revisor senior de C++, Direct3D 11 y ReShade. Antes de editar,
inspecciona los archivos y logs disponibles, preserva cambios existentes y
verifica las firmas y garantías de la versión real de ReShade. Entrega:

1. Hallazgos priorizados por severidad con archivo, línea, mecanismo,
   condiciones y un método de reproducción. Diferencia hecho, hipótesis y
   limitación de evidencia.
2. Criterios de aceptación finitos para concluir 1–4 en esta demo, indicando
   explícitamente lo que no se podría garantizar para juegos arbitrarios.
3. Auditar el control de registro tardío real ya implementado y diseñar
   variantes para SRV/UAV, profundidad y destrucción diferida.
4. Revisión adversarial del control de dos swapchains y de sus aserciones:
   identidad, orden de eventos, errores, limpieza y posibles falsos positivos.
5. Plan de pruebas de sincronización que no confunda `GetData == S_FALSE`,
   flush, ejecución o frames en vuelo.
6. Mantén `TemporalInputs` activo en pruebas de la demo, los límites de FPS,
   los cierres limpios y los logs. El usuario autorizó etapas posteriores;
   no afirmar que están implementadas ni saltar sus prerrequisitos.

Fuentes de referencia:
[OptiScaler](https://github.com/optiscaler/OptiScaler/),
[ReShade](https://github.com/crosire/reshade),
[DLSS5-Reshade-AIO](https://github.com/kibblerz/DLSS5-Reshade-AIO),
[DXVK](https://github.com/doitsujin/dxvk),
[D3D11 GetData](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-getdata),
[DXGI conversión de color](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/converting-data-color-space),
[RenderDoc Pixel History](https://renderdoc.org/docs/how/how_inspect_pixel.html).
