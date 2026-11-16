# Quark
 
Editor de texto ligero en **C + GTK4 + GtkSourceView 5**, con una terminal
integrada llamada **Fermion** (panel inferior, estilo VS Code). Un solo
binario, sin VTE ni librerías externas de terminal.
 
## Archivos
 
```
quark/
├── quarkgtk.c     Editor + terminal (todo el código C)
├── quarkgtk.ui    Interfaz GtkBuilder (incluye el panel de Fermion)
├── quark.css      Estilos (editor, explorador, pestañas y Fermion)
├── quark.desktop  Lanzador de escritorio (opcional)
├── Makefile       all / deps / run / install / uninstall / clean
└── README.md
```
 
## Dependencias
 
| Qué              | Versión mínima | Debian/Ubuntu                        | Fedora                | Arch            |
|------------------|----------------|--------------------------------------|-----------------------|-----------------|
| GTK 4            | 4.10           | `libgtk-4-dev`                       | `gtk4-devel`          | `gtk4`          |
| GtkSourceView 5  | 5.x            | `libgtksourceview-5-dev`             | `gtksourceview5-devel`| `gtksourceview5`|
| gcc, make, pkg-config | —         | `build-essential pkg-config`         | `gcc make pkgconf-pkg-config` | `base-devel` |
 
GTK 4.10 es el mínimo porque `GtkFileDialog` y `GtkAlertDialog` nacieron ahí.
Solo Linux/Unix (usa `fork`, `/proc/self/exe` y `glib-unix`).
 
## Compilar y ejecutar
 
```sh
make            # genera ./quark  (comprueba las dependencias antes)
make run        # compila y abre el editor
./quark archivo.c otra_carpeta/   # también acepta archivos y carpetas
```
 
Sin Makefile, el comando mínimo es:
 
```sh
gcc -Wall -Wextra -O2 -o quark quarkgtk.c \
    $(pkg-config --cflags --libs gtk4 gtksourceview-5)
```
 
El ejecutable busca `quarkgtk.ui` y `quark.css` en este orden:
`$QUARK_DATADIR`, la carpeta del ejecutable, `<PREFIX>/share/quark` y el
directorio actual. Por eso `./quark` funciona desde la carpeta del proyecto
y también una vez instalado.
 
### Instalar
 
```sh
sudo make install                  # en /usr/local
make install PREFIX=$HOME/.local   # solo para tu usuario
make install DESTDIR=/tmp/pkg      # para empaquetar
make uninstall
```
 
`make install` instala el binario, el `.ui`, el `.css` y `quark.desktop`
(como `io.github.julio.Quark.desktop`, igual que el ID de la aplicación).
 
## Atajos de teclado
 
| Atajo                    | Acción                              |
|--------------------------|-------------------------------------|
| `Ctrl+N`                 | Nueva pestaña                       |
| `Ctrl+O`                 | Abrir archivos                      |
| `Ctrl+Mayús+O`           | Abrir carpeta (explorador lateral)  |
| `Ctrl+S`                 | Guardar                             |
| `Ctrl+Mayús+S`           | Guardar como…                       |
| `Ctrl+W`                 | Cerrar pestaña                      |
| `Ctrl+B` / `F9`          | Mostrar/ocultar barra lateral       |
| `Ctrl+J` / `F12`         | Mostrar/ocultar la terminal Fermion |
 
También se pueden **arrastrar archivos o carpetas** a la ventana. Con
cambios sin guardar, cerrar una pestaña o la ventana pide confirmación.
 
### Dentro de Fermion (con el foco en la línea de órdenes)
 
| Tecla / orden   | Efecto                                                                 |
|-----------------|------------------------------------------------------------------------|
| `Enter`         | Ejecuta la orden                                                       |
| `↑` / `↓`       | Historial (máx. 200, sin duplicados consecutivos)                      |
| `Ctrl+C`        | Con un comando en marcha y sin texto seleccionado: `SIGINT` al grupo; los siguientes `Ctrl+C`: `SIGKILL`. Con texto seleccionado, copia. |
| `Ctrl+L`        | Limpia la salida                                                       |
| `Esc`           | Devuelve el foco al editor                                             |
| `cd [ruta]`     | Interna. Admite `~`, `~/x`, `-`, comillas y `\ ` para espacios        |
| `clear`         | Interna. Limpia la salida                                              |
| `exit`          | Interna. Oculta el panel                                               |
 
El directorio de trabajo de la terminal **sigue a la carpeta abierta** en el
explorador. El prompt muestra la carpeta actual (`~ $`, `tmp $`…) y la
cabecera la ruta completa (con `~` por el home).
 
## Arquitectura
 
### Editor
 
- `Doc` agrupa por pestaña: `GtkScrolledWindow` + `GtkSourceView` +
  `GtkSourceBuffer` + `GtkSourceFile` + etiqueta. Se cuelga de la página del
  notebook con `g_object_set_data_full`, así se libera con ella.
- Cargar/guardar usan `GtkSourceFileLoader/Saver` (asíncronos). Los cierres
  que dependen de un diálogo se encadenan con un contexto `Ctx` y un
  callback `ContinuarCb`.
- El explorador es un `GtkListView` sobre `GtkTreeListModel`, con un
  `GtkDirectoryList` por carpeta (filtrado de ocultos y orden carpetas primero).
- Todo se maneja con `GAction`: `nuevo`, `abrir`, `abrir-carpeta`, `guardar`,
  `guardar-como`, `cerrar-pestana`, `sidebar` y `terminal` (las dos últimas
  con estado booleano; los botones y atajos las activan igual).
### Diseño de la ventana
 
```
GtkApplicationWindow
└─ GtkPaned (horizontal)
   ├─ panel_izquierdo (explorador)
   └─ box_editor (GtkBox vertical)
      ├─ paned_term (GtkPaned vertical, vexpand)
      │  ├─ nb              ← editor (pestañas)
      │  └─ fermion_panel   ← terminal (oculta por defecto)
      └─ barra_estado       ← hermana del paned, nunca dentro
```
 
### Fermion
 
Cada orden se ejecuta con `fork` + `pipe` + `dup2` + `execl("/bin/sh","sh","-c",…)`.
 
```
hijo:   setpgid(0,0) → stdin=/dev/null, stdout=stderr=pipe → chdir(cwd) → sh -c
padre:  close(escritura) → O_NONBLOCK en la lectura
        g_unix_fd_add()      → fermion_leer_cb()       datos, sin bloquear la UI
        g_child_watch_add()  → fermion_hijo_termino()  última lectura, close(fd),
                                                       código de salida o señal
```
 
- **Sin hilos**: la lectura corre en el bucle principal de GLib, así que no
  hay `g_idle_add` ni condiciones de carrera con la UI.
- **UTF-8 parcial**: los bytes se acumulan en un `GString`; solo se vuelcan
  secuencias completas. Un byte inválido se sustituye por U+FFFD y el NUL se
  descarta; una secuencia cortada al final espera al siguiente trozo.
- **Límites**: 5000 líneas de salida (se recorta lo más antiguo) y 64 KiB
  leídos por vuelta del bucle, para que un comando muy verboso no congele la UI.
- **Etiquetas de texto**: `cmd` (eco de la orden), `meta` (avisos de Fermion) y
  `stderr` (errores propios de Fermion: `cd` inválido, `fork` fallido…).
- Al cerrar la aplicación, si queda un trabajo en curso se envía `SIGTERM`
  a su grupo de procesos.
### Limitaciones conocidas de Fermion
 
- No hay PTY: los programas interactivos (`vim`, `top`, `ssh` pidiendo
  contraseña…) no funcionan, porque `stdin` es `/dev/null`.
- `stdout` y `stderr` comparten el pipe, así que la salida de error de un
  comando externo no se distingue (se pinta igual que la normal).
- No interpreta secuencias de escape ANSI (colores, movimiento de cursor).
- Solo corre un comando a la vez.
## Ejemplo de sesión
 
1. `./quark` → se abre una ventana con una pestaña vacía.
2. `Ctrl+J` → aparece Fermion abajo; la barra de estado sigue debajo.
3. Prueba, una a una:
```text
ls
cd /tmp          (el prompt pasa a "tmp $")
pwd              (muestra /tmp)
ls /no/existe    (el error sale con código 2)
echo "ñandú 日本語 😀"
sleep 5          (pulsa Ctrl+C: se corta con "[terminado por la señal 2 (Interrupt)]")
seq 1 100000 | tail -n 2
cd "mi carpeta"  (si no existe: error de cd en rojo)
cd ~             (vuelve al home)
cd -             (vuelve a /tmp)
clear            (limpia)
exit             (oculta el panel)
```
 
4. `Ctrl+Mayús+O` y elige una carpeta → el explorador la muestra y, al abrir
   Fermion, el prompt y la cabecera apuntan a esa carpeta.
5. Escribe algo, `Ctrl+S` (elige nombre), `Ctrl+W`. Escribe de nuevo y
   cierra la ventana: pide confirmar los cambios.
## Licencia
 
GPL-3.0-o-posterior (ver cabecera de `quarkgtk.c`).
