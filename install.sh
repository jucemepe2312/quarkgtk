#!/bin/sh
# install.sh - compila e instala Quark en /usr/local
set -e

SRCS="src/quarkgtk.c src/quark_formato.c"
BIN="bin/quark"
DEST_BIN="/usr/local/bin/quark"
DEST_SHARE="/usr/local/share/quark"

# Dependencias
command -v gcc        >/dev/null 2>&1 || { echo "falta gcc"; exit 1; }
command -v pkg-config >/dev/null 2>&1 || { echo "falta pkg-config"; exit 1; }
pkg-config --exists gtk4            || { echo "falta gtk4"; exit 1; }
pkg-config --exists gtksourceview-5 || { echo "falta gtksourceview-5"; exit 1; }
pkg-config --exists glib-2.0 gio-2.0 || { echo "falta glib/gio"; exit 1; }

# Compilar
echo "compilando $SRCS..."
mkdir -p bin
gcc -Wall -Wextra -O2 $SRCS -o "$BIN" \
    $(pkg-config --cflags --libs gtk4 gtksourceview-5 glib-2.0 gio-2.0)

# Instalar binario y recursos
echo "instalando binario en $DEST_BIN..."
install -Dm755 "$BIN" "$DEST_BIN"

echo "instalando recursos en $DEST_SHARE..."
install -Dm644 src/quarkgtk.ui "$DEST_SHARE/quarkgtk.ui"
install -Dm644 src/quark.css    "$DEST_SHARE/quark.css"

echo "listo. prueba con: quark <archivo>"