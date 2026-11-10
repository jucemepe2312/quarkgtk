#!/bin/bash

# ==========================================
# Quark GTK - Instalador
# ==========================================

set -e # Detiene el script si hay un error

APP_NAME="Quark"
BIN_NAME="quark"
SRC_FILE="quarkgtk.c"
UI_FILE="quarkgtk.ui"
CSS_FILE="quark.css"
LINK_NAME="quarks"

# Colores para la terminal
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # Sin color

echo -e "${YELLOW}=== Instalador de Quark GTK ===${NC}\n"

# 1. Verificar que estamos en la raíz del repo
if [ ! -f "$SRC_FILE" ] || [ ! -f "$UI_FILE" ] || [ ! -f "$CSS_FILE" ]; then
    echo -e "${RED}Error: No se encontraron los archivos fuente ($SRC_FILE, $UI_FILE, $CSS_FILE).${NC}"
    echo "Asegúrate de ejecutar este script desde la raíz del repositorio."
    exit 1
fi

# 2. Verificar dependencias
echo -e "${YELLOW}[1/5] Verificando dependencias...${NC}"
if ! command -v gcc &> /dev/null; then
    echo -e "${RED}Error: gcc no está instalado.${NC}"
    exit 1
fi

if ! pkg-config --exists gtk4; then
    echo -e "${RED}Error: GTK4 no está instalado.${NC}"
    echo "En Arch/Manjaro: sudo pacman -S gtk4"
    exit 1
fi

if ! pkg-config --exists gtksourceview-5; then
    echo -e "${RED}Error: GtkSourceView 5 no está instalado.${NC}"
    echo "En Arch/Manjaro: sudo pacman -S gtksourceview5"
    exit 1
fi

# 3. Compilar
echo -e "${YELLOW}[2/5] Compilando Quark...${NC}"
gcc "$SRC_FILE" -o "$BIN_NAME" $(pkg-config --cflags --libs gtk4 gtksourceview-5)
echo -e "${GREEN}Compilación exitosa.${NC}"

# 4. Instalar binario y recursos
echo -e "${YELLOW}[3/5] Instalando binario y recursos...${NC}"
sudo mkdir -p /usr/local/bin
sudo mkdir -p /usr/local/share/quark

sudo cp "$BIN_NAME" /usr/local/bin/
sudo cp "$UI_FILE" /usr/local/share/quark/
sudo cp "$CSS_FILE" /usr/local/share/quark/

echo -e "${GREEN}Archivos copiados a /usr/local/.${NC}"

# 5. Crear script lanzador
echo -e "${YELLOW}[4/5] Creando lanzador...${NC}"
cat << 'EOF' | sudo tee /usr/local/bin/quarks > /dev/null
#!/bin/bash
# Cambia al directorio de recursos y ejecuta Quark
cd /usr/local/share/quark || exit 1
exec /usr/local/bin/quark "$@"
EOF

sudo chmod +x /usr/local/bin/quarks
echo -e "${GREEN}Lanzador creado en /usr/local/bin/quarks${NC}"

# 6. Crear archivo .desktop
echo -e "${YELLOW}[5/5] Creando acceso en el menú de aplicaciones...${NC}"
mkdir -p ~/.local/share/applications
cat << EOF > ~/.local/share/applications/quark.desktop
[Desktop Entry]
Version=1.0
Name=$APP_NAME
Comment=Editor de texto minimalista en GTK4
Exec=/usr/local/bin/$LINK_NAME
Icon=accessories-text-editor
Terminal=false
Type=Application
Categories=Utility;TextEditor;
StartupNotify=true
EOF

# Actualizar la base de datos de aplicaciones
update-desktop-database ~/.local/share/applications 2>/dev/null || true

echo -e "\n${GREEN}=== ¡Instalación completada! ===${NC}"
echo "Puedes abrir Quark desde el menú de aplicaciones o ejecutando: quarks"
echo "Para desinstalar, ejecuta: sudo rm /usr/local/bin/quarks /usr/local/bin/quark && sudo rm -rf /usr/local/share/quark ~/.local/share/applications/quark.desktop"