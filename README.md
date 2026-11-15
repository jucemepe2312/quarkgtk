# Quark

Editor de texto ligero en **C** con **GTK4** y **GtkSourceView**. Parte del ecosistema de herramientas de sistema con temática de física de partículas: **Quark**, **Neutron**, **Photon**, **Tachyon**, **Gluon**, **Electron**, **Átomo**.

## Características

- Resaltado de sintaxis con **GtkSourceView** (detección automática por extensión).
- Números de línea y resaltado de línea actual.
- Pestañas (`GtkNotebook`) con botón de cierre por pestaña.
- Abrir y guardar archivos con `GtkFileDialog` (asíncrono, no bloquea la UI).
- CSS personalizado (tema oscuro estilo Catppuccin/Mocha).
- Apertura desde línea de comandos: `quark archivo1.c archivo2.h`.
- UI definida en `quarkgtk.ui` (GtkBuilder), no hardcodeada en C.

## 🛠️ Requisitos

- **GTK4**
- **GtkSourceView 5**
- **GCC** o **Clang**
- **pkg-config**
- **make** (opcional, si usas el Makefile)

### Instalación de dependencias

**Arch Linux / Manjaro:**
```bash
sudo pacman -S gtk4 gtksourceview5 base-devel pkg-config