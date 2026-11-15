#!/bin/sh
# install.sh - compila e instala Quark en /usr/local
set -e

SRC="quarkgtk.c"
BIN="bin/quarkgtk"
DEST_BIN="/usr/local/bin/quark"
DEST_SHARE="/usr/local/share/quark"

# Dependencias
command -v gcc        >/dev/null 2>&1 || { echo "falta gcc"; exit 1; }
command -v pkg-config >/dev/null 2>&1 || { echo "falta pkg-config"; exit 1; }
pkg-config --exists gtk4            || { echo "falta gtk4"; exit 1; }
pkg-config --exists gtksourceview-5 || { echo "falta gtksourceview-5"; exit 1; }

# Compilar
echo "compilando $SRC..."
mkdir -p bin
gcc -Wall -Wextra -O2 "$SRC" -o "$BIN" \
    $(pkg-config --cflags --libs gtk4 gtksourceview-5)

# Instalar binario y recursos
echo "instalando binario en $DEST_BIN..."
install -Dm755 "$BIN" "$DEST_BIN"

echo "instalando recursos en $DEST_SHARE..."
install -Dm644 quarkgtk.ui "$DEST_SHARE/quarkgtk.ui"
install -Dm644 quark.css    "$DEST_SHARE/quark.css"

echo "listo. prueba con: quark <archivo>"
echo "nota: si Quark no encuentra quarkgtk.ui o quark.css, ejecútalo desde la carpeta del repo,"
echo "      o ajusta las rutas en quarkgtk.c a $DEST_SHARE/"