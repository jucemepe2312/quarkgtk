/*
 * quarkgtk.c - Editor de texto ligero en C con GTK4 y GtkSourceView,
 *              con terminal integrada (Fermion).
 * Copyright (C) 2026 Julio <jucemepe2312@gmail.com>
 *
 * Este programa es software libre: puedes redistribuirlo y/o modificarlo
 * bajo los términos de la GNU General Public License publicada por la
 * Free Software Foundation, ya sea la versión 3 de la Licencia, o
 * (a tu elección) cualquier versión posterior.
 *
 * Este programa se distribuye con la esperanza de que sea útil,
 * pero SIN NINGUNA GARANTÍA; ni siquiera la garantía implícita de
 * COMERCIABILIDAD o IDONEIDAD PARA UN PROPÓSITO PARTICULAR. Véase la
 * GNU General Public License para más detalles.
 *
 * Deberías haber recibido una copia de la GNU General Public License
 * junto con este programa. Si no, consulta <https://www.gnu.org/licenses/>.
 */
#include <gtk/gtk.h>
#include <gtksourceview/gtksource.h>
#include <glib-unix.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Casi todos los callbacks de GTK reciben parámetros que no usamos; así el
 * comando `gcc -Wall -Wextra` sale limpio sin necesidad de -Wno-unused-parameter. */
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

#define APPLICATION_ID "io.github.julio.Quark"
#define ATRIBUTOS_DIR \
    "standard::name,standard::display-name,standard::type,standard::symbolic-icon"

/* Carpeta donde `make install` deja quarkgtk.ui y quark.css (el Makefile la
 * sobrescribe con -DQUARK_DATADIR según PREFIX). */
#ifndef QUARK_DATADIR
#define QUARK_DATADIR "/usr/local/share/quark"
#endif

typedef struct
{
    GtkWidget* scr;
    GtkSourceView* view;
    GtkSourceBuffer* buffer;
    GtkSourceFile* sfile;
    GtkWidget* label;
} Doc;

typedef void(*ContinuarCb)(gboolean ok, gpointer data);

typedef struct
{
    GtkWidget* scr;
    GFile* target;
    ContinuarCb cb;
    gpointer data;
} Ctx;

static GtkWindow* ventana = NULL;
static GtkNotebook* nb = NULL;
static GtkWidget* panel_izq = NULL;
static GtkListView* arbol = NULL;
static GtkLabel* lbl_carpeta = NULL;
static GtkLabel* lbl_pos = NULL;
static GtkLabel* lbl_lang = NULL;
static GFile* carpeta_actual = NULL;
static gboolean fallo_inicio = FALSE;

static Doc* crear_documento(void);

static void abrir_archivo(GFile* file);

static void fermion_set_cwd(GFile* dir);

static void guardar_doc(Doc* doc, gboolean como, ContinuarCb cb, gpointer data);

/* Busca un archivo de recursos (.ui, .css). Orden:
 *   1. $QUARK_DATADIR (útil para pruebas)
 *   2. la carpeta del ejecutable (desarrollo: ./quark junto a sus recursos)
 *   3. la carpeta de instalación (QUARK_DATADIR, la que usa `make install`)
 *   4. el directorio actual
 * Devuelve NULL si no lo encuentra en ninguna. */
static char* ruta_recurso(const char* nombre)
{
    char* dir_exe = NULL;
    char* exe = g_file_read_link("/proc/self/exe", NULL);
    if (exe != NULL)
    {
        dir_exe = g_path_get_dirname(exe);
        g_free(exe);
    }

    /* ojo: getenv y dir_exe pueden ser NULL, así que se saltan en vez de cortar el bucle */
    const char* candidatos[] = {g_getenv("QUARK_DATADIR"), dir_exe, QUARK_DATADIR, "."};
    char* resultado = NULL;

    for (gsize i = 0; i < G_N_ELEMENTS(candidatos) && resultado == NULL; i++)
    {
        if (candidatos[i] == NULL)
        {
            continue;
        }

        char* ruta = g_build_filename(candidatos[i], nombre, NULL);
        if (g_file_test(ruta, G_FILE_TEST_IS_REGULAR))
        {
            resultado = ruta;
        }
        else
        {
            g_free(ruta);
        }
    }

    g_free(dir_exe);
    return resultado;
}

static void mostrar_error(const char* titulo, const char* detalle)
{
    GtkAlertDialog* d = gtk_alert_dialog_new("%s", titulo);
    gtk_alert_dialog_set_detail(d, detalle != NULL ? detalle : "");
    gtk_alert_dialog_show(d, ventana);
    g_object_unref(d);
}

static Ctx* ctx_nuevo(Doc* doc, ContinuarCb cb, gpointer data)
{
    Ctx* c = g_new0(Ctx, 1);
    c->scr = g_object_ref(doc->scr);
    c->cb = cb;
    c->data = data;
    return c;
}

static void ctx_liberar(Ctx* c)
{
    g_clear_object(&c->target);
    g_object_unref(c->scr);
    g_free(c);
}

static Doc* doc_de_pagina(GtkWidget* pagina)
{
    return g_object_get_data(G_OBJECT(pagina), "doc");
}

static Doc* doc_actual(void)
{
    int i = gtk_notebook_get_current_page(nb);
    if (i < 0)
    {
        return NULL;
    }

    return doc_de_pagina(gtk_notebook_get_nth_page(nb, i));
}

static gboolean doc_modificado(Doc* d)
{
    return gtk_text_buffer_get_modified(GTK_TEXT_BUFFER(d->buffer));
}

static char* doc_nombre(Doc* d)
{
    GFile* loc = gtk_source_file_get_location(d->sfile);
    if (loc != NULL)
    {
        char* base = g_file_get_basename(loc);
        if (base != NULL)
        {
            return base;
        }
    }

    return g_strdup("Sin título");
}

static gboolean doc_es_virgen(Doc* d)
{
    return d != NULL
    && gtk_source_file_get_location(d->sfile) == NULL
    && !doc_modificado(d)
    && gtk_text_buffer_get_char_count(GTK_TEXT_BUFFER(d->buffer)) == 0;
}

static void refrescar_ventana(void)
{
    if (ventana == NULL || nb == NULL)
    {
        return;
    }

    Doc* d = doc_actual();
    if (d == NULL)
    {
        gtk_window_set_title(ventana, "Quark");
        return;
    }

    char* nombre = doc_nombre(d);
    char* titulo = g_strdup_printf("%s%s — Quark", doc_modificado(d) ? "● " : "", nombre);
    gtk_window_set_title(ventana, titulo);
    g_free(titulo);
    g_free(nombre);
}

static void actualizar_estado(void)
{
    if (lbl_pos == NULL || lbl_lang == NULL || nb == NULL)
    {
        return;
    }

    Doc* d = doc_actual();
    if (d == NULL)
    {
        gtk_label_set_text(lbl_pos, "");
        gtk_label_set_text(lbl_lang, "");
        return;
    }

    GtkTextBuffer* tb = GTK_TEXT_BUFFER(d->buffer);
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(tb, &it, gtk_text_buffer_get_insert(tb));

    char* pos = g_strdup_printf("Ln %d, Col %d",
        gtk_text_iter_get_line(&it) + 1,
        gtk_source_view_get_visual_column(d->view, &it) + 1);
    gtk_label_set_text(lbl_pos, pos);
    g_free(pos);

    GtkSourceLanguage* lang = gtk_source_buffer_get_language(d->buffer);
    gtk_label_set_text(lbl_lang, lang != NULL ? gtk_source_language_get_name(lang) : "Texto plano");
}

static void doc_actualizar_etiqueta(Doc* d)
{
    char* nombre = doc_nombre(d);
    char* texto = g_strdup_printf("%s%s", doc_modificado(d) ? "● " : "", nombre);
    gtk_label_set_text(GTK_LABEL(d->label), texto);

    GFile* loc = gtk_source_file_get_location(d->sfile);
    if (loc != NULL)
    {
        char* ruta = g_file_get_parse_name(loc);
        gtk_widget_set_tooltip_text(d->label, ruta);
        g_free(ruta);
    }
    else
    {
        gtk_widget_set_tooltip_text(d->label, NULL);
    }

    g_free(texto);
    g_free(nombre);
    refrescar_ventana();
}

static void doc_aplicar_lenguaje(Doc* d)
{
    GFile* loc = gtk_source_file_get_location(d->sfile);
    GtkSourceLanguage* lang = NULL;

    if (loc != NULL)
    {
        char* base = g_file_get_basename(loc);
        GtkSourceLanguageManager* lm = gtk_source_language_manager_get_default();
        lang = gtk_source_language_manager_guess_language(lm, base, NULL);
        g_free(base);
    }

    gtk_source_buffer_set_language(d->buffer, lang);
    actualizar_estado();
}

static void doc_liberar(gpointer p)
{
    Doc* d = p;
    g_object_unref(d->sfile);
    g_free(d);
}

static void quitar_pagina(GtkWidget* scr)
{
    int idx = gtk_notebook_page_num(nb, scr);
    if (idx >= 0)
    {
        gtk_notebook_remove_page(nb, idx);
    }

    if (gtk_notebook_get_n_pages(nb) == 0)
    {
        crear_documento();
    }
}

static void confirmacion_lista(GObject* src, GAsyncResult* res, gpointer user_data)
{
    Ctx* c = user_data;
    Doc* doc = doc_de_pagina(c->scr);
    int r = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, NULL);

    if (r == 1)
    {

        gtk_text_buffer_set_modified(GTK_TEXT_BUFFER(doc->buffer), FALSE);
        c->cb(TRUE, c->data);
    }
    else if (r == 2)
    {

        guardar_doc(doc, FALSE, c->cb, c->data);
    }
    else
    {

        c->cb(FALSE, c->data);
    }

    ctx_liberar(c);
}

static void pedir_confirmacion(Doc* doc, ContinuarCb cb, gpointer data)
{
    char* nombre = doc_nombre(doc);
    GtkAlertDialog* d = gtk_alert_dialog_new("¿Guardar los cambios en «%s»?", nombre);
    gtk_alert_dialog_set_detail(d, "Si no los guardas, se perderán.");

    const char * botones[] = {"Cancelar", "Descartar", "Guardar", NULL};
    gtk_alert_dialog_set_buttons(d, botones);
    gtk_alert_dialog_set_cancel_button(d, 0);
    gtk_alert_dialog_set_default_button(d, 2);

    Ctx* c = ctx_nuevo(doc, cb, data);
    gtk_alert_dialog_choose(d, ventana, NULL, confirmacion_lista, c);

    g_object_unref(d);
    g_free(nombre);
}

static void tras_confirmar_pestana(gboolean ok, gpointer data)
{
    if (ok)
    {
        quitar_pagina(GTK_WIDGET(data));
    }
}

static void cerrar_doc(Doc* doc)
{
    if (!doc_modificado(doc))
    {
        quitar_pagina(doc->scr);
        return;
    }

    int idx = gtk_notebook_page_num(nb, doc->scr);
    if (idx >= 0)
    {
        gtk_notebook_set_current_page(nb, idx);
    }

    pedir_confirmacion(doc, tras_confirmar_pestana, doc->scr);
}

static void on_cerrar_clicked(GtkButton* btn, gpointer user_data)
{
    Doc* d = doc_de_pagina(GTK_WIDGET(user_data));
    if (d != NULL)
    {
        cerrar_doc(d);
    }
}

static void on_tab_clic_medio(
    GtkGestureClick* g,
    gint n_press,
    gdouble x,
    gdouble y,
    gpointer user_data)
{
    Doc* d = doc_de_pagina(GTK_WIDGET(user_data));
    if (d != NULL)
    {
        cerrar_doc(d);
    }
}

static void escritura_lista(GObject* src, GAsyncResult* res, gpointer user_data)
{
    Ctx* c = user_data;
    Doc* doc = doc_de_pagina(c->scr);
    GError* err = NULL;
    gboolean ok = gtk_source_file_saver_save_finish(GTK_SOURCE_FILE_SAVER(src), res, &err);

    if (!ok)
    {
        mostrar_error("No se pudo guardar el archivo", err != NULL ? err->message : NULL);
        g_clear_error(&err);
    }
    else
    {
        doc_aplicar_lenguaje(doc);
        doc_actualizar_etiqueta(doc);
    }

    if (c->cb != NULL)
    {
        c->cb(ok, c->data);
    }

    ctx_liberar(c);
}

static void escribir_doc(Ctx* c)
{
    Doc* doc = doc_de_pagina(c->scr);
    GtkSourceFileSaver* saver =
        gtk_source_file_saver_new_with_target(doc->buffer, doc->sfile, c->target);

    gtk_source_file_saver_set_flags(saver, GTK_SOURCE_FILE_SAVER_FLAGS_IGNORE_MODIFICATION_TIME);
    gtk_source_file_saver_save_async(saver, G_PRIORITY_DEFAULT, NULL,
                                     NULL, NULL, NULL, escritura_lista, c);
    g_object_unref(saver);
}

static void dialogo_guardar_listo(GObject* src, GAsyncResult* res, gpointer user_data)
{
    Ctx* c = user_data;
    GError* err = NULL;
    GFile* file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, &err);
    g_clear_error(&err);

    if (file == NULL)
    {

        if (c->cb != NULL)
        {
            c->cb(FALSE, c->data);
        }

        ctx_liberar(c);
        return;
    }

    c->target = file;
    escribir_doc(c);
}

static void guardar_doc(Doc* doc, gboolean como, ContinuarCb cb, gpointer data)
{
    Ctx* c = ctx_nuevo(doc, cb, data);
    GFile* loc = gtk_source_file_get_location(doc->sfile);

    if (!como && loc != NULL)
    {
        c->target = g_object_ref(loc);
        escribir_doc(c);
        return;
    }

    GtkFileDialog* dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, como ? "Guardar como" : "Guardar archivo");

    if (loc != NULL)
    {
        gtk_file_dialog_set_initial_file(dlg, loc);
    }
    else
    {
        gtk_file_dialog_set_initial_name(dlg, "sin_titulo.txt");
        if (carpeta_actual != NULL)
        {
            gtk_file_dialog_set_initial_folder(dlg, carpeta_actual);
        }
    }

    gtk_file_dialog_save(dlg, ventana, NULL, dialogo_guardar_listo, c);
    g_object_unref(dlg);
}

static void carga_lista(GObject* src, GAsyncResult* res, gpointer user_data)
{
    GtkWidget* scr = user_data;
    Doc* doc = doc_de_pagina(scr);
    GError* err = NULL;

    gboolean ok = gtk_source_file_loader_load_finish(GTK_SOURCE_FILE_LOADER(src), res, &err);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(doc->view), TRUE);

    if (!ok)
    {
        mostrar_error("No se pudo abrir el archivo", err != NULL ? err->message : NULL);
        g_clear_error(&err);

        gtk_source_file_set_location(doc->sfile, NULL);
        if (doc_es_virgen(doc) && gtk_notebook_get_n_pages(nb) > 1)
        {
            quitar_pagina(scr);
        }
        else
        {
            doc_actualizar_etiqueta(doc);
        }
    }
    else
    {
        GtkTextIter inicio;
        gtk_text_buffer_get_start_iter(GTK_TEXT_BUFFER(doc->buffer), &inicio);
        gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(doc->buffer), &inicio);

        doc_aplicar_lenguaje(doc);
        doc_actualizar_etiqueta(doc);
    }

    g_object_unref(scr);
}

static void cargar_doc(Doc* doc)
{
    GtkSourceFileLoader* loader = gtk_source_file_loader_new(doc->buffer, doc->sfile);

    gtk_text_view_set_editable(GTK_TEXT_VIEW(doc->view), FALSE);
    doc_actualizar_etiqueta(doc);

    gtk_source_file_loader_load_async(loader, G_PRIORITY_DEFAULT, NULL,
                                      NULL, NULL, NULL,
        carga_lista, g_object_ref(doc->scr));
    g_object_unref(loader);
}

static void establecer_carpeta(GFile* dir);

static void abrir_archivo(GFile* file)
{
    int n = gtk_notebook_get_n_pages(nb);

    for (int i = 0; i < n; i++)
    {
        Doc* d = doc_de_pagina(gtk_notebook_get_nth_page(nb, i));
        GFile* loc = gtk_source_file_get_location(d->sfile);
        if (loc != NULL && g_file_equal(loc, file))
        {
            gtk_notebook_set_current_page(nb, i);
            return;
        }
    }

    Doc* doc = doc_actual();
    if (!doc_es_virgen(doc))
    {
        doc = crear_documento();
    }

    gtk_source_file_set_location(doc->sfile, file);
    cargar_doc(doc);

    if (carpeta_actual == NULL)
    {
        GFile* padre = g_file_get_parent(file);
        if (padre != NULL)
        {
            establecer_carpeta(padre);
            g_object_unref(padre);
        }
    }
}

static void abrir_ruta(GFile* f)
{
    if (g_file_query_file_type(f, G_FILE_QUERY_INFO_NONE, NULL) == G_FILE_TYPE_DIRECTORY)
    {
        establecer_carpeta(f);
    }
    else
    {
        abrir_archivo(f);
    }
}

static void dialogo_abrir_listo(GObject* src, GAsyncResult* res, gpointer user_data)
{
    GError* err = NULL;
    GListModel* lista = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(src), res, &err);
    g_clear_error(&err);

    if (lista == NULL)
    {
        return;
    }

    guint n = g_list_model_get_n_items(lista);
    for (guint i = 0; i < n; i++)
    {
        GFile* f = g_list_model_get_item(lista, i);
        abrir_ruta(f);
        g_object_unref(f);
    }

    g_object_unref(lista);
}

static void dialogo_carpeta_listo(GObject* src, GAsyncResult* res, gpointer user_data)
{
    GError* err = NULL;
    GFile* dir = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, &err);
    g_clear_error(&err);

    if (dir != NULL)
    {
        establecer_carpeta(dir);
        g_object_unref(dir);
    }
}

static void on_modificado(GtkTextBuffer* tb, gpointer user_data)
{
    doc_actualizar_etiqueta(user_data);
}

static void on_cursor(GObject* obj, GParamSpec* pspec, gpointer user_data)
{
    actualizar_estado();
}

static Doc* crear_documento(void)
{
    Doc* doc = g_new0(Doc, 1);

    doc->scr = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(doc->scr, TRUE);

    doc->view = GTK_SOURCE_VIEW(gtk_source_view_new());
    doc->buffer = GTK_SOURCE_BUFFER(gtk_text_view_get_buffer(GTK_TEXT_VIEW(doc->view)));
    doc->sfile = gtk_source_file_new();

    GtkTextView* tv = GTK_TEXT_VIEW(doc->view);
    gtk_text_view_set_wrap_mode(tv, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_monospace(tv, TRUE);
    gtk_text_view_set_left_margin(tv, 8);
    gtk_text_view_set_right_margin(tv, 8);
    gtk_text_view_set_top_margin(tv, 8);
    gtk_text_view_set_bottom_margin(tv, 8);

    gtk_source_view_set_show_line_numbers(doc->view, TRUE);
    gtk_source_view_set_highlight_current_line(doc->view, TRUE);
    gtk_source_view_set_auto_indent(doc->view, TRUE);
    gtk_source_view_set_indent_on_tab(doc->view, TRUE);
    gtk_source_view_set_tab_width(doc->view, 4);
    gtk_source_view_set_insert_spaces_instead_of_tabs(doc->view, TRUE);
    gtk_source_view_set_smart_home_end(doc->view, GTK_SOURCE_SMART_HOME_END_BEFORE);
    gtk_source_buffer_set_highlight_matching_brackets(doc->buffer, TRUE);

    GtkSourceStyleSchemeManager* sm = gtk_source_style_scheme_manager_get_default();
    GtkSourceStyleScheme* esquema = gtk_source_style_scheme_manager_get_scheme(sm, "Adwaita-dark");
    if (esquema == NULL)
    {
        esquema = gtk_source_style_scheme_manager_get_scheme(sm, "classic-dark");
    }

    if (esquema != NULL)
    {
        gtk_source_buffer_set_style_scheme(doc->buffer, esquema);
    }

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(doc->scr), GTK_WIDGET(doc->view));

    doc->label = gtk_label_new(NULL);
    gtk_label_set_ellipsize(GTK_LABEL(doc->label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(doc->label), 28);

    GtkWidget* btn = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
    gtk_widget_set_focus_on_click(btn, FALSE);
    gtk_widget_set_tooltip_text(btn, "Cerrar (Ctrl+W)");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_cerrar_clicked), doc->scr);

    GtkWidget* tab = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(tab), doc->label);
    gtk_box_append(GTK_BOX(tab), btn);

    GtkGesture* medio = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(medio), GDK_BUTTON_MIDDLE);
    g_signal_connect(medio, "pressed", G_CALLBACK(on_tab_clic_medio), doc->scr);
    gtk_widget_add_controller(tab, GTK_EVENT_CONTROLLER(medio));

    g_object_set_data_full(G_OBJECT(doc->scr), "doc", doc, doc_liberar);

    g_signal_connect(doc->buffer, "modified-changed", G_CALLBACK(on_modificado), doc);
    g_signal_connect(doc->buffer, "notify::cursor-position", G_CALLBACK(on_cursor), NULL);

    int idx = gtk_notebook_append_page(nb, doc->scr, tab);
    gtk_notebook_set_tab_reorderable(nb, doc->scr, TRUE);
    gtk_notebook_set_current_page(nb, idx);

    doc_actualizar_etiqueta(doc);
    actualizar_estado();
    gtk_widget_grab_focus(GTK_WIDGET(doc->view));

    return doc;
}

static void on_pagina_cambiada(GObject* obj, GParamSpec* pspec, gpointer user_data)
{
    refrescar_ventana();
    actualizar_estado();

    Doc* d = doc_actual();
    if (d != NULL)
    {
        gtk_widget_grab_focus(GTK_WIDGET(d->view));
    }
}

static gint comparar_info(gconstpointer a, gconstpointer b, gpointer user_data)
{
    GFileInfo* ia = (GFileInfo*) a;
    GFileInfo* ib = (GFileInfo*) b;
    gboolean da = g_file_info_get_file_type(ia) == G_FILE_TYPE_DIRECTORY;
    gboolean db = g_file_info_get_file_type(ib) == G_FILE_TYPE_DIRECTORY;

    if (da != db)
    {
        return da ? -1 : 1;
    }

    return g_utf8_collate(g_file_info_get_display_name(ia), g_file_info_get_display_name(ib));
}

static gboolean filtro_visible(gpointer item, gpointer user_data)
{
    const char* n = g_file_info_get_name(G_FILE_INFO(item));
    if (n == NULL || n[0] == '\0')
    {
        return FALSE;
    }

    return n[0] != '.' && !g_str_has_suffix(n, "~");
}

static GListModel* crear_modelo_dir(GFile* dir)
{
    GtkDirectoryList* dl = gtk_directory_list_new(ATRIBUTOS_DIR, dir);
    gtk_directory_list_set_monitored(dl, TRUE);

    GtkFilterListModel* fm = gtk_filter_list_model_new(
        G_LIST_MODEL(dl), GTK_FILTER(gtk_custom_filter_new(filtro_visible, NULL, NULL)));

    GtkSortListModel* sm = gtk_sort_list_model_new(
        G_LIST_MODEL(fm), GTK_SORTER(gtk_custom_sorter_new(comparar_info, NULL, NULL)));

    return G_LIST_MODEL(sm);
}

static GListModel* hijos_cb(gpointer item, gpointer user_data)
{
    GFileInfo* info = G_FILE_INFO(item);
    if (g_file_info_get_file_type(info) != G_FILE_TYPE_DIRECTORY)
    {
        return NULL;
    }

    GObject* obj = g_file_info_get_attribute_object(info, "standard::file");
    if (obj == NULL)
    {
        return NULL;
    }

    return crear_modelo_dir(G_FILE(obj));
}

static void fila_setup(GtkListItemFactory* f, GtkListItem* item, gpointer user_data)
{
    GtkWidget* expander = gtk_tree_expander_new();
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* img = gtk_image_new();
    GtkWidget* lab = gtk_label_new(NULL);

    gtk_label_set_xalign(GTK_LABEL(lab), 0);
    gtk_label_set_ellipsize(GTK_LABEL(lab), PANGO_ELLIPSIZE_END);

    gtk_box_append(GTK_BOX(box), img);
    gtk_box_append(GTK_BOX(box), lab);
    gtk_tree_expander_set_child(GTK_TREE_EXPANDER(expander), box);
    gtk_list_item_set_child(item, expander);
}

static void fila_bind(GtkListItemFactory* f, GtkListItem* item, gpointer user_data)
{
    GtkTreeExpander* expander = GTK_TREE_EXPANDER(gtk_list_item_get_child(item));
    GtkTreeListRow* row = GTK_TREE_LIST_ROW(gtk_list_item_get_item(item));
    gtk_tree_expander_set_list_row(expander, row);

    GFileInfo* info = gtk_tree_list_row_get_item(row); /* transfer full */
    GtkWidget* box = gtk_tree_expander_get_child(expander);
    GtkWidget* img = gtk_widget_get_first_child(box);
    GtkWidget* lab = gtk_widget_get_last_child(box);

    GIcon* icono = g_file_info_get_symbolic_icon(info);
    if (icono != NULL)
    {
        gtk_image_set_from_gicon(GTK_IMAGE(img), icono);
    }

    gtk_label_set_text(GTK_LABEL(lab), g_file_info_get_display_name(info));

    g_object_unref(info);
}

static void arbol_activado(GtkListView* lv, guint pos, gpointer user_data)
{
    GListModel* modelo = G_LIST_MODEL(gtk_list_view_get_model(lv));
    GtkTreeListRow* row = g_list_model_get_item(modelo, pos);
    if (row == NULL)
    {
        return;
    }

    GFileInfo* info = gtk_tree_list_row_get_item(row);
    if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY)
    {
        gtk_tree_list_row_set_expanded(row, !gtk_tree_list_row_get_expanded(row));
    }
    else
    {
        GObject* obj = g_file_info_get_attribute_object(info, "standard::file");
        if (obj != NULL)
        {
            abrir_archivo(G_FILE(obj));
        }
    }

    g_object_unref(info);
    g_object_unref(row);
}

static void establecer_carpeta(GFile* dir)
{
    g_set_object(&carpeta_actual, dir);

    GListModel* raiz = crear_modelo_dir(dir);
    GtkTreeListModel* arbol_modelo = gtk_tree_list_model_new(raiz, FALSE, FALSE, hijos_cb, NULL, NULL);
    GtkSingleSelection* sel = gtk_single_selection_new(G_LIST_MODEL(arbol_modelo));
    gtk_single_selection_set_autoselect(sel, FALSE);
    gtk_single_selection_set_can_unselect(sel, TRUE);
    gtk_list_view_set_model(arbol, GTK_SELECTION_MODEL(sel));
    g_object_unref(sel);

    char* base = g_file_get_basename(dir);
    char* mayus = g_utf8_strup(base != NULL ? base : "", -1);
    gtk_label_set_text(lbl_carpeta, mayus);

    char* ruta = g_file_get_parse_name(dir);
    gtk_widget_set_tooltip_text(GTK_WIDGET(lbl_carpeta), ruta);
    g_free(ruta);
    g_free(mayus);
    g_free(base);

    fermion_set_cwd(dir);
}

/* ====================================================================
 * Fermion - terminal integrada en Quark
 *
 * Cada orden se ejecuta con fork + pipe + dup2 + `sh -c` (como en el
 * Fermion original), pero la salida se lee de forma ASÍNCRONA desde el
 * bucle principal de GLib con g_unix_fd_add(), así que la interfaz no se
 * congela mientras el comando corre y no hacen falta hilos:
 *
 *   hijo:   setpgid -> stdin=/dev/null, stdout=stderr=pipe -> chdir -> sh -c
 *   padre:  cierra la escritura, O_NONBLOCK en la lectura,
 *           g_unix_fd_add()  -> fermion_leer_cb()      (datos)
 *           g_child_watch_add() -> fermion_hijo_termino() (fin + cierre)
 *
 * Limitación consciente: no hay PTY, así que los programas interactivos
 * (vim, top, ssh con contraseña...) no funcionan; stdin es /dev/null.
 * ==================================================================== */

#define FERMION_MAX_LINEAS 5000
#define FERMION_HIST_MAX 200
#define FERMION_LECTURA_MAX 65536 /* bytes máximos por vuelta del bucle */

typedef struct
{
    pid_t pid;
    int fd;             /* extremo de lectura del pipe */
    guint fuente;       /* id de g_unix_fd_add (0 = ya eliminada) */
    GString* pend;      /* bytes pendientes (UTF-8 incompleto) */
    int cancelaciones;  /* veces que se pulsó Ctrl+C */
} Trabajo;

static GtkWidget* fermion_paned = NULL;
static GtkWidget* fermion_panel = NULL;
static GtkTextView* fermion_salida = NULL;
static GtkTextBuffer* fermion_buf = NULL;
static GtkEntry* fermion_entrada = NULL;
static GtkLabel* fermion_lbl_cwd = NULL;
static GtkLabel* fermion_prompt = NULL;
static char* fermion_cwd = NULL;
static char* fermion_cwd_previo = NULL; /* para `cd -` */
static Trabajo* fermion_trabajo = NULL;
static GPtrArray* fermion_hist = NULL;
static guint fermion_hist_pos = 0;
static gboolean fermion_pos_inicial = FALSE;

/* ---------- Funciones puras (sin GTK), fáciles de probar ---------- */

/* "/home/ana/src" -> "~/src" */
static char* fermion_ruta_corta(const char* cwd, const char* home)
{
    size_t lh = strlen(home);

    if (lh > 0 && g_str_has_prefix(cwd, home) && (cwd[lh] == '\0' || cwd[lh] == '/'))
    {
        return g_strdup_printf("~%s", cwd + lh);
    }

    return g_strdup(cwd);
}

/* Nombre que se muestra en el prompt: "~", "/" o la última carpeta */
static char* fermion_nombre_prompt(const char* cwd, const char* home)
{
    if (strcmp(cwd, home) == 0)
    {
        return g_strdup("~");
    }

    if (strcmp(cwd, "/") == 0)
    {
        return g_strdup("/");
    }

    return g_path_get_basename(cwd);
}

/* Resuelve el argumento de `cd` a una ruta absoluta y canónica.
 * Admite comillas simples/dobles y barras (g_shell_unquote), `~`, `~/x`
 * y `-` (carpeta anterior). Devuelve NULL si las comillas no cierran.
 * No comprueba que la ruta exista: eso lo hace el que llama. */
static char* fermion_resolver_destino(
    const char* arg,
    const char* cwd,
    const char* home,
    const char* previo)
{
    char* a = g_strstrip(g_strdup(arg));
    char* crudo;

    if (a[0] == '\0')
    {
        crudo = g_strdup(home);
    }
    else
    {
        GError* err = NULL;
        crudo = g_shell_unquote(a, &err);
        g_clear_error(&err);
        if (crudo == NULL)
        {
            g_free(a);
            return NULL;
        }
    }

    char* destino;
    if (strcmp(crudo, "~") == 0)
    {
        destino = g_strdup(home);
    }
    else if (g_str_has_prefix(crudo, "~/"))
    {
        destino = g_build_filename(home, crudo + 2, NULL);
    }
    else if (strcmp(crudo, "-") == 0 && previo != NULL)
    {
        destino = g_strdup(previo);
    }
    else
    {
        destino = g_strdup(crudo);
    }

    char* absoluta = g_canonicalize_filename(destino, cwd);

    g_free(destino);
    g_free(crudo);
    g_free(a);
    return absoluta;
}

/* ---------- Estado visible: cwd y prompt ---------- */

static const char* fermion_directorio(void)
{
    if (fermion_cwd == NULL)
    {
        fermion_cwd = g_get_current_dir();
    }

    return fermion_cwd;
}

static void fermion_refrescar_cwd(void)
{
    if (fermion_lbl_cwd == NULL || fermion_prompt == NULL)
    {
        return;
    }

    const char* cwd = fermion_directorio();
    const char* home = g_get_home_dir();

    char* corta = fermion_ruta_corta(cwd, home);
    gtk_label_set_text(fermion_lbl_cwd, corta);
    gtk_widget_set_tooltip_text(GTK_WIDGET(fermion_lbl_cwd), cwd);
    g_free(corta);

    char* nombre = fermion_nombre_prompt(cwd, home);
    char* prompt = g_strdup_printf("%s $", nombre);
    gtk_label_set_text(fermion_prompt, prompt);
    g_free(prompt);
    g_free(nombre);
}

/* Lo llama establecer_carpeta(): la terminal sigue a la carpeta abierta */
static void fermion_set_cwd(GFile* dir)
{
    char* ruta = g_file_get_path(dir);
    if (ruta == NULL)
    {
        return; /* carpeta remota sin ruta local */
    }

    if (g_strcmp0(ruta, fermion_cwd) != 0)
    {
        g_free(fermion_cwd_previo);
        fermion_cwd_previo = fermion_cwd;
    }
    else
    {
        g_free(fermion_cwd);
    }

    fermion_cwd = ruta;
    fermion_refrescar_cwd();
}

/* ---------- Salida ---------- */

static void fermion_escribir(const char* texto, gssize len, const char* tag)
{
    GtkTextIter fin;
    gtk_text_buffer_get_end_iter(fermion_buf, &fin);

    if (tag != NULL)
    {
        gtk_text_buffer_insert_with_tags_by_name(fermion_buf, &fin, texto, len, tag, NULL);
    }
    else
    {
        gtk_text_buffer_insert(fermion_buf, &fin, texto, len);
    }

    /* límite de líneas: se descarta lo más antiguo */
    int lineas = gtk_text_buffer_get_line_count(fermion_buf);
    if (lineas > FERMION_MAX_LINEAS)
    {
        GtkTextIter ini, corte;
        gtk_text_buffer_get_start_iter(fermion_buf, &ini);
        gtk_text_buffer_get_iter_at_line(fermion_buf, &corte, lineas - FERMION_MAX_LINEAS);
        gtk_text_buffer_delete(fermion_buf, &ini, &corte);
    }

    gtk_text_view_scroll_mark_onscreen(fermion_salida,
                                       gtk_text_buffer_get_mark(fermion_buf, "fin"));
}

static void fermion_vmensaje(const char* tag, const char* fmt, va_list ap) G_GNUC_PRINTF(2, 0);

static void fermion_vmensaje(const char* tag, const char* fmt, va_list ap)
{
    char* txt = g_strdup_vprintf(fmt, ap);
    fermion_escribir(txt, -1, tag);
    g_free(txt);
}

/* Avisos de Fermion (código de salida, señales, ayuda) */
static void fermion_mensaje(const char* fmt, ...) G_GNUC_PRINTF(1, 2);

static void fermion_mensaje(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fermion_vmensaje("meta", fmt, ap);
    va_end(ap);
}

/* Errores propios de Fermion (cd inválido, fork fallido...). Los del comando
 * externo no se pueden distinguir: stdout y stderr comparten el pipe. */
static void fermion_error(const char* fmt, ...) G_GNUC_PRINTF(1, 2);

static void fermion_error(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fermion_vmensaje("stderr", fmt, ap);
    va_end(ap);
}

/* Vuelca al buffer lo acumulado sin partir caracteres UTF-8:
 *  - secuencia válida completa  -> se escribe
 *  - incompleta al final        -> se espera al siguiente trozo (o, con
 *                                  `forzar`, se sustituye por U+FFFD)
 *  - byte inválido              -> U+FFFD y se sigue con el siguiente byte
 *  - NUL                        -> se descarta (GtkTextBuffer no lo admite) */
static void fermion_volcar(Trabajo* t, gboolean forzar)
{
    GString* p = t->pend;
    GString* limpio = g_string_sized_new(p->len);
    gsize i = 0;

    while (i < p->len)
    {
        const char* ini = p->str + i;
        guchar c = (guchar) *ini;

        if (c == '\0')
        {
            i++;
            continue;
        }

        if (c < 0x80)
        {
            g_string_append_c(limpio, (char) c);
            i++;
            continue;
        }

        gunichar u = g_utf8_get_char_validated(ini, p->len - i);

        if (u == (gunichar) - 2)
        {
            if (!forzar)
            {
                break; /* los bytes restantes se conservan en `pend` */
            }

            g_string_append(limpio, "\xEF\xBF\xBD");
            i = p->len;
        }
        else if (u == (gunichar) - 1)
        {
            g_string_append(limpio, "\xEF\xBF\xBD");
            i++;
        }
        else
        {
            gsize l = (gsize) (g_utf8_next_char(ini) - ini);
            g_string_append_len(limpio, ini, (gssize) l);
            i += l;
        }
    }

    g_string_erase(p, 0, (gssize) i);

    if (limpio->len > 0)
    {
        fermion_escribir(limpio->str, (gssize) limpio->len, NULL);
    }

    g_string_free(limpio, TRUE);
}

/* ---------- Ejecución de órdenes ---------- */

/* g_unix_fd_add: se llama desde el bucle principal cuando hay datos */
static gboolean fermion_leer_cb(gint fd, GIOCondition cond, gpointer data)
{
    Trabajo* t = data;
    char buf[4096];
    ssize_t n = 0;
    gsize total = 0;

    /* tope por vuelta para que un comando muy verboso no bloquee la UI */
    while (total < FERMION_LECTURA_MAX && (n = read(fd, buf, sizeof(buf))) > 0)
    {
        g_string_append_len(t->pend, buf, n);
        total += (gsize) n;
    }

    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))
    {
        /* EOF o error: nadie más escribe; cerramos la fuente (el fd lo
         * cierra fermion_hijo_termino) */
        fermion_volcar(t, TRUE);
        t->fuente = 0;
        return G_SOURCE_REMOVE;
    }

    fermion_volcar(t, FALSE);
    return G_SOURCE_CONTINUE;
}

/* g_child_watch_add: el hijo terminó */
static void fermion_hijo_termino(GPid pid, gint status, gpointer data)
{
    Trabajo* t = data;

    if (t->fuente != 0)
    {
        /* última lectura de lo que quedara en el pipe */
        char buf[4096];
        ssize_t n;
        while ((n = read(t->fd, buf, sizeof(buf))) > 0)
        {
            g_string_append_len(t->pend, buf, n);
        }

        g_source_remove(t->fuente);
        t->fuente = 0;
    }

    fermion_volcar(t, TRUE);
    close(t->fd);

    if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
    {
        fermion_mensaje("[terminó con código %d]\n", WEXITSTATUS(status));
    }
    else if (WIFSIGNALED(status))
    {
        fermion_mensaje("[terminado por la señal %d (%s)]\n",
                        WTERMSIG(status), g_strsignal(WTERMSIG(status)));
    }

    g_string_free(t->pend, TRUE);
    g_free(t);
    fermion_trabajo = NULL;
}

static void fermion_lanzar(const char* linea)
{
    int fds[2];
    GError* err = NULL;

    /* FD_CLOEXEC: ni este pipe ni sus extremos se filtran a otros hijos;
     * dup2 limpia el flag en el descriptor destino (0/1/2). */
    if (!g_unix_open_pipe(fds, FD_CLOEXEC, &err))
    {
        fermion_error("pipe: %s\n", err->message);
        g_clear_error(&err);
        return;
    }

    const char* cwd = fermion_directorio();
    pid_t pid = fork();

    if (pid < 0)
    {
        fermion_error("fork: %s\n", g_strerror(errno));
        close(fds[0]);
        close(fds[1]);
        return;
    }

    if (pid == 0)
    {
        /* HIJO: tras fork() solo funciones seguras y luego exec. */
        setpgid(0, 0); /* grupo propio: kill(-pid, ...) alcanza a todo el comando */

        int nul = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (nul >= 0)
        {
            dup2(nul, STDIN_FILENO); /* sin teclado: nada se queda esperando entrada */
        }

        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);

        if (chdir(cwd) != 0)
        {
            const char msg[] = "fermion: no se pudo entrar en el directorio de trabajo\n";
            ssize_t r = write(STDERR_FILENO, msg, sizeof(msg) - 1);
            (void) r;
            _exit(126);
        }

        execl("/bin/sh", "sh", "-c", linea, (char*) NULL);

        const char msg[] = "fermion: no se pudo ejecutar /bin/sh\n";
        ssize_t r = write(STDERR_FILENO, msg, sizeof(msg) - 1);
        (void) r;
        _exit(127); /* _exit: no ejecutar los atexit de GTK en el hijo */
    }

    /* PADRE */
    close(fds[1]);
    setpgid(pid, pid); /* evita la carrera con el setpgid del hijo */
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);

    Trabajo* t = g_new0(Trabajo, 1);
    t->pid = pid;
    t->fd = fds[0];
    t->pend = g_string_new(NULL);
    t->fuente = g_unix_fd_add(fds[0], G_IO_IN | G_IO_HUP | G_IO_ERR, fermion_leer_cb, t);
    g_child_watch_add(pid, fermion_hijo_termino, t);
    fermion_trabajo = t;
}

/* ---------- Órdenes internas ---------- */

/* Devuelve el argumento si la línea es `cd` o `cd <algo>`; si no, NULL */
static const char* fermion_arg_cd(const char* linea)
{
    if (strcmp(linea, "cd") == 0)
    {
        return "";
    }

    if (g_str_has_prefix(linea, "cd") && g_ascii_isspace(linea[2]))
    {
        const char* a = linea + 2;
        while (g_ascii_isspace(*a))
        {
            a++;
        }

        return a;
    }

    return NULL;
}

/* `cd` tiene que ser interno: cada orden corre en su propio sh y un cd allí
 * se perdería al terminar. */
static void fermion_cd(const char* arg)
{
    char* destino = fermion_resolver_destino(arg, fermion_directorio(),
                                             g_get_home_dir(), fermion_cwd_previo);

    if (destino == NULL)
    {
        fermion_error("cd: comillas sin cerrar\n");
        return;
    }

    if (g_file_test(destino, G_FILE_TEST_IS_DIR))
    {
        g_free(fermion_cwd_previo);
        fermion_cwd_previo = fermion_cwd;
        fermion_cwd = destino;
        fermion_refrescar_cwd();
    }
    else
    {
        fermion_error("cd: %s: no existe o no es un directorio\n", arg);
        g_free(destino);
    }
}

static void fermion_limpiar(void)
{
    gtk_text_buffer_set_text(fermion_buf, "", 0);
}

static void fermion_ocultar(void)
{
    gtk_widget_activate_action(GTK_WIDGET(ventana), "win.terminal", NULL);
}

static void fermion_historial_agregar(const char* linea)
{
    if (fermion_hist->len == 0
        || g_strcmp0(g_ptr_array_index(fermion_hist, fermion_hist->len - 1), linea) != 0)
    {
        g_ptr_array_add(fermion_hist, g_strdup(linea));
        if (fermion_hist->len > FERMION_HIST_MAX)
        {
            g_ptr_array_remove_index(fermion_hist, 0);
        }
    }

    fermion_hist_pos = fermion_hist->len;
}

static void fermion_ejecutar(const char* orden)
{
    char* linea = g_strstrip(g_strdup(orden));

    if (linea[0] == '\0')
    {
        g_free(linea);
        return;
    }

    if (fermion_trabajo != NULL)
    {
        fermion_error("Ya hay un comando en ejecución (Ctrl+C para cancelarlo)\n");
        g_free(linea);
        return;
    }

    fermion_historial_agregar(linea);

    char* eco = g_strdup_printf("$ %s\n", linea);
    fermion_escribir(eco, -1, "cmd");
    g_free(eco);

    const char* arg_cd = fermion_arg_cd(linea);

    if (strcmp(linea, "exit") == 0)
    {
        fermion_ocultar();
    }
    else if (strcmp(linea, "clear") == 0)
    {
        fermion_limpiar();
    }
    else if (arg_cd != NULL)
    {
        fermion_cd(arg_cd);
    }
    else
    {
        fermion_lanzar(linea);
    }

    g_free(linea);
}

/* ---------- Eventos del panel ---------- */

static void fermion_activar(GtkEntry* e, gpointer user_data)
{
    char* texto = g_strdup(gtk_editable_get_text(GTK_EDITABLE(e)));
    gtk_editable_set_text(GTK_EDITABLE(e), "");
    fermion_ejecutar(texto);
    g_free(texto);
}

static void fermion_historial_navegar(gboolean arriba)
{
    GtkEditable* ed = GTK_EDITABLE(fermion_entrada);

    if (fermion_hist->len == 0)
    {
        return;
    }

    if (arriba)
    {
        if (fermion_hist_pos > 0)
        {
            fermion_hist_pos--;
        }
    }
    else if (fermion_hist_pos < fermion_hist->len)
    {
        fermion_hist_pos++;
    }

    const char* t = fermion_hist_pos < fermion_hist->len
        ? g_ptr_array_index(fermion_hist, fermion_hist_pos)
        : "";
    gtk_editable_set_text(ed, t);
    gtk_editable_set_position(ed, -1);
}

static gboolean fermion_tecla(
    GtkEventControllerKey* c,
    guint keyval,
    guint keycode,
    GdkModifierType state,
    gpointer user_data)
{
    GtkEditable* ed = GTK_EDITABLE(fermion_entrada);
    gboolean ctrl = (state & GDK_CONTROL_MASK) != 0;

    if (keyval == GDK_KEY_Up || keyval == GDK_KEY_Down)
    {
        fermion_historial_navegar(keyval == GDK_KEY_Up);
        return TRUE;
    }

    /* Ctrl+C con un comando en marcha y sin texto seleccionado: cancelar.
     * Con selección, se deja pasar para que copie como siempre. */
    if (ctrl && (keyval == GDK_KEY_c || keyval == GDK_KEY_C)
        && fermion_trabajo != NULL
        && !gtk_editable_get_selection_bounds(ed, NULL, NULL))
    {
        /* 1.er Ctrl+C: SIGINT al grupo; los siguientes: SIGKILL */
        int sig = fermion_trabajo->cancelaciones++ == 0 ? SIGINT : SIGKILL;
        kill(-fermion_trabajo->pid, sig);
        fermion_mensaje("^C\n");
        return TRUE;
    }

    if (ctrl && (keyval == GDK_KEY_l || keyval == GDK_KEY_L))
    {
        fermion_limpiar();
        return TRUE;
    }

    if (keyval == GDK_KEY_Escape)
    {
        Doc* d = doc_actual();
        if (d != NULL)
        {
            gtk_widget_grab_focus(GTK_WIDGET(d->view));
        }

        return TRUE;
    }

    return FALSE;
}

static void fermion_btn_limpiar(GtkButton* b, gpointer user_data)
{
    fermion_limpiar();
    gtk_widget_grab_focus(GTK_WIDGET(fermion_entrada));
}

/* El panel y sus widgets ya vienen de quarkgtk.ui (el paned vertical envuelve
 * al notebook allí mismo, sin reparentar nada en tiempo de ejecución); aquí
 * solo se enganchan referencias, etiquetas de texto y señales. */
static void fermion_montar(GtkBuilder* b)
{
    fermion_paned = GTK_WIDGET(gtk_builder_get_object(b, "paned_term"));
    fermion_panel = GTK_WIDGET(gtk_builder_get_object(b, "fermion_panel"));
    fermion_salida = GTK_TEXT_VIEW(gtk_builder_get_object(b, "fermion_salida"));
    fermion_entrada = GTK_ENTRY(gtk_builder_get_object(b, "fermion_entrada"));
    fermion_lbl_cwd = GTK_LABEL(gtk_builder_get_object(b, "fermion_lbl_cwd"));
    fermion_prompt = GTK_LABEL(gtk_builder_get_object(b, "fermion_prompt"));
    GtkWidget* b_limpiar = GTK_WIDGET(gtk_builder_get_object(b, "fermion_btn_limpiar"));

    fermion_hist = g_ptr_array_new_with_free_func(g_free);

    fermion_buf = gtk_text_view_get_buffer(fermion_salida);
    gtk_text_buffer_create_tag(fermion_buf, "cmd",
                               "weight", PANGO_WEIGHT_BOLD,
                               "foreground", "#7aa2f7", NULL);
    gtk_text_buffer_create_tag(fermion_buf, "meta",
                               "style", PANGO_STYLE_ITALIC,
                               "foreground", "#9aa0a6", NULL);
    gtk_text_buffer_create_tag(fermion_buf, "stderr",
                               "foreground", "#f7768e", NULL);

    GtkTextIter fin;
    gtk_text_buffer_get_end_iter(fermion_buf, &fin);
    gtk_text_buffer_create_mark(fermion_buf, "fin", &fin, FALSE); /* gravedad derecha */

    g_signal_connect(b_limpiar, "clicked", G_CALLBACK(fermion_btn_limpiar), NULL);
    g_signal_connect(fermion_entrada, "activate", G_CALLBACK(fermion_activar), NULL);

    GtkEventController* teclas = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(teclas, GTK_PHASE_CAPTURE);
    g_signal_connect(teclas, "key-pressed", G_CALLBACK(fermion_tecla), NULL);
    gtk_widget_add_controller(GTK_WIDGET(fermion_entrada), teclas);

    fermion_refrescar_cwd();
    fermion_mensaje("Fermion v0.2 - Terminal integrada\n"
                    "Ctrl+C cancela el comando, Ctrl+L limpia, 'exit' oculta el panel.\n\n");
}

/* Acción win.terminal (con estado booleano): muestra/oculta el panel */
static void act_terminal_estado(GSimpleAction* a, GVariant* valor, gpointer ud)
{
    g_simple_action_set_state(a, valor);

    if (fermion_panel == NULL)
    {
        return;
    }

    gboolean ver = g_variant_get_boolean(valor);
    gtk_widget_set_visible(fermion_panel, ver);

    if (ver)
    {
        /* primera vez: la terminal ocupa ~240 px (o la mitad si la ventana es baja);
         * después GtkPaned recuerda la posición que haya dejado el usuario */
        if (!fermion_pos_inicial)
        {
            int alto = gtk_widget_get_height(fermion_paned);
            if (alto > 0)
            {
                gtk_paned_set_position(GTK_PANED(fermion_paned),
                                       alto > 400 ? alto - 240 : alto / 2);
                fermion_pos_inicial = TRUE;
            }
        }

        gtk_widget_grab_focus(GTK_WIDGET(fermion_entrada));
    }
    else
    {
        Doc* d = doc_actual();
        if (d != NULL)
        {
            gtk_widget_grab_focus(GTK_WIDGET(d->view));
        }
    }
}

/* Al salir: si queda un trabajo en curso, SIGTERM a todo su grupo */
static void fermion_liberar(void)
{
    if (fermion_trabajo != NULL)
    {
        kill(-fermion_trabajo->pid, SIGTERM);
    }

    g_clear_pointer(&fermion_cwd, g_free);
    g_clear_pointer(&fermion_cwd_previo, g_free);
    g_clear_pointer(&fermion_hist, g_ptr_array_unref);
}

static void act_nuevo(GSimpleAction* a, GVariant* p, gpointer ud)
{
    crear_documento();
}

static void act_abrir(GSimpleAction* a, GVariant* p, gpointer ud)
{
    GtkFileDialog* dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Abrir archivos");
    if (carpeta_actual != NULL)
    {
        gtk_file_dialog_set_initial_folder(dlg, carpeta_actual);
    }

    gtk_file_dialog_open_multiple(dlg, ventana, NULL, dialogo_abrir_listo, NULL);
    g_object_unref(dlg);
}

static void act_abrir_carpeta(GSimpleAction* a, GVariant* p, gpointer ud)
{
    GtkFileDialog* dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Abrir carpeta");
    if (carpeta_actual != NULL)
    {
        gtk_file_dialog_set_initial_folder(dlg, carpeta_actual);
    }

    gtk_file_dialog_select_folder(dlg, ventana, NULL, dialogo_carpeta_listo, NULL);
    g_object_unref(dlg);
}

static void act_guardar(GSimpleAction* a, GVariant* p, gpointer ud)
{
    Doc* d = doc_actual();
    if (d != NULL)
    {
        guardar_doc(d, FALSE, NULL, NULL);
    }
}

static void act_guardar_como(GSimpleAction* a, GVariant* p, gpointer ud)
{
    Doc* d = doc_actual();
    if (d != NULL)
    {
        guardar_doc(d, TRUE, NULL, NULL);
    }
}

static void act_cerrar_pestana(GSimpleAction* a, GVariant* p, gpointer ud)
{
    Doc* d = doc_actual();
    if (d != NULL)
    {
        cerrar_doc(d);
    }
}

static void act_sidebar_estado(GSimpleAction* a, GVariant* valor, gpointer ud)
{
    g_simple_action_set_state(a, valor);
    gtk_widget_set_visible(panel_izq, g_variant_get_boolean(valor));
}

static const GActionEntry acciones[] = {
    {.name = "nuevo", .activate = act_nuevo},
    {.name = "abrir", .activate = act_abrir},
    {.name = "abrir-carpeta", .activate = act_abrir_carpeta},
    {.name = "guardar", .activate = act_guardar},
    {.name = "guardar-como", .activate = act_guardar_como},
    {.name = "cerrar-pestana", .activate = act_cerrar_pestana},
    {.name = "sidebar", .state = "true", .change_state = act_sidebar_estado},
    {.name = "terminal", .state = "false", .change_state = act_terminal_estado},
};

static void atajo(GtkApplication* app, const char* accion, const char* a1, const char* a2)
{
    const char * lista[] = {a1, a2, NULL};
    gtk_application_set_accels_for_action(app, accion, lista);
}

static void tras_confirmar_ventana(gboolean ok, gpointer data)
{
    if (ok)
    {
        gtk_window_close(ventana);
    }
}

static gboolean on_close_request(GtkWindow* w, gpointer user_data)
{
    int n = gtk_notebook_get_n_pages(nb);

    for (int i = 0; i < n; i++)
    {
        Doc* d = doc_de_pagina(gtk_notebook_get_nth_page(nb, i));
        if (doc_modificado(d))
        {
            gtk_notebook_set_current_page(nb, i);
            pedir_confirmacion(d, tras_confirmar_ventana, NULL);
            return TRUE; /* bloquea el cierre por ahora */
        }
    }

    return FALSE;
}

static gboolean on_drop(
    GtkDropTarget* t,
    const GValue* v,
    gdouble x,
    gdouble y,
    gpointer user_data)
{
    GdkFileList* fl = g_value_get_boxed(v);
    if (fl == NULL)
    {
        return FALSE;
    }

    for (GSList * l = gdk_file_list_get_files(fl); l != NULL; l = l->next)
    {
        abrir_ruta(G_FILE(l->data));
    }

    return TRUE;
}

static void cargar_css(void)
{
    char* ruta = ruta_recurso("quark.css");
    char* texto = NULL;

    if (ruta == NULL || !g_file_get_contents(ruta, &texto, NULL, NULL))
    {
        g_warning("No se encontró quark.css; se usará el tema por defecto");
        g_free(ruta);
        return;
    }

    GtkCssProvider* provider = gtk_css_provider_new();
#if GTK_CHECK_VERSION(4, 12, 0)
    gtk_css_provider_load_from_string(provider, texto);
#else
    gtk_css_provider_load_from_data(provider, texto, -1);
#endif
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
    g_free(texto);
    g_free(ruta);
}

/* Comprueba que el .ui trae todos los objetos que el código necesita; así un
 * .ui desactualizado da un mensaje claro en vez de un fallo dentro de GTK. */
static gboolean ui_completa(GtkBuilder* b)
{
    const char * ids[] = {
        "win", "nb", "panel_izquierdo", "arbol", "lbl_carpeta", "lbl_pos", "lbl_lang",
        "paned_term", "fermion_panel", "fermion_salida", "fermion_entrada",
        "fermion_prompt", "fermion_lbl_cwd", "fermion_btn_limpiar", NULL
    };

    for (int i = 0; ids[i] != NULL; i++)
    {
        if (gtk_builder_get_object(b, ids[i]) == NULL)
        {
            g_printerr("quarkgtk.ui: falta el objeto «%s»\n", ids[i]);
            return FALSE;
        }
    }

    return TRUE;
}

static void inicio(GApplication* app, gpointer user_data)
{
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, NULL);
    cargar_css();

    char* ruta_ui = ruta_recurso("quarkgtk.ui");
    if (ruta_ui == NULL)
    {
        g_printerr("No se encontró quarkgtk.ui (buscado junto al ejecutable, en %s y en el "
                   "directorio actual)\n", QUARK_DATADIR);
        fallo_inicio = TRUE;
        return;
    }

    GtkBuilder* build = gtk_builder_new();
    GError* err = NULL;

    if (!gtk_builder_add_from_file(build, ruta_ui, &err))
    {
        g_printerr("Error al cargar %s: %s\n", ruta_ui, err->message);
        g_clear_error(&err);
        g_free(ruta_ui);
        g_object_unref(build);
        fallo_inicio = TRUE;
        return;
    }

    g_free(ruta_ui);

    if (!ui_completa(build))
    {
        g_object_unref(build);
        fallo_inicio = TRUE;
        return;
    }

    ventana = GTK_WINDOW(gtk_builder_get_object(build, "win"));
    nb = GTK_NOTEBOOK(gtk_builder_get_object(build, "nb"));
    panel_izq = GTK_WIDGET(gtk_builder_get_object(build, "panel_izquierdo"));
    arbol = GTK_LIST_VIEW(gtk_builder_get_object(build, "arbol"));
    lbl_carpeta = GTK_LABEL(gtk_builder_get_object(build, "lbl_carpeta"));
    lbl_pos = GTK_LABEL(gtk_builder_get_object(build, "lbl_pos"));
    lbl_lang = GTK_LABEL(gtk_builder_get_object(build, "lbl_lang"));

    gtk_window_set_application(ventana, GTK_APPLICATION(app));

    GtkListItemFactory* fabrica = gtk_signal_list_item_factory_new();
    g_signal_connect(fabrica, "setup", G_CALLBACK(fila_setup), NULL);
    g_signal_connect(fabrica, "bind", G_CALLBACK(fila_bind), NULL);
    gtk_list_view_set_factory(arbol, fabrica);
    g_object_unref(fabrica);
    g_signal_connect(arbol, "activate", G_CALLBACK(arbol_activado), NULL);

    g_action_map_add_action_entries(G_ACTION_MAP(ventana), acciones, G_N_ELEMENTS(acciones), NULL);
    atajo(GTK_APPLICATION(app), "win.nuevo", "<Control>n", NULL);
    atajo(GTK_APPLICATION(app), "win.abrir", "<Control>o", NULL);
    atajo(GTK_APPLICATION(app), "win.abrir-carpeta", "<Control><Shift>o", NULL);
    atajo(GTK_APPLICATION(app), "win.guardar", "<Control>s", NULL);
    atajo(GTK_APPLICATION(app), "win.guardar-como", "<Control><Shift>s", NULL);
    atajo(GTK_APPLICATION(app), "win.cerrar-pestana", "<Control>w", NULL);
    atajo(GTK_APPLICATION(app), "win.sidebar", "<Control>b", "F9");
    atajo(GTK_APPLICATION(app), "win.terminal", "<Control>j", "F12");

    g_signal_connect(ventana, "close-request", G_CALLBACK(on_close_request), NULL);
    g_signal_connect(nb, "notify::page", G_CALLBACK(on_pagina_cambiada), NULL);

    GtkDropTarget* drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(drop, "drop", G_CALLBACK(on_drop), NULL);
    gtk_widget_add_controller(GTK_WIDGET(ventana), GTK_EVENT_CONTROLLER(drop));

    fermion_montar(build);

    g_object_unref(build);

    crear_documento();
}

static void on_activate(GApplication* application)
{
    if (ventana != NULL)
    {
        gtk_window_present(ventana);
    }
}

static void abierto(GApplication* application, GFile**files, gint n_files, const gchar* hint)
{
    if (ventana == NULL)
    {
        return;
    }

    for (int i = 0; i < n_files; i++)
    {
        abrir_ruta(files[i]);
    }

    gtk_window_present(ventana);
}

int main(int argc, char**argv)
{
    GtkApplication* app;
    int stat;

    app = gtk_application_new(APPLICATION_ID, G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(app, "startup", G_CALLBACK(inicio), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(abierto), NULL);
    stat = g_application_run(G_APPLICATION(app), argc, argv);

    fermion_liberar();
    g_clear_object(&carpeta_actual);
    g_object_unref(app);
    return fallo_inicio ? 1 : stat;
}