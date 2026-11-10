#include <gtk/gtk.h>
#include <gtksourceview/gtksource.h>

#define APPLICATION_ID "julio.github.com"
#define ATRIBUTOS_DIR \
    "standard::name,standard::display-name,standard::type,standard::symbolic-icon"

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

static Doc* crear_documento(void);

static void abrir_archivo(GFile* file);

static void guardar_doc(Doc* doc, gboolean como, ContinuarCb cb, gpointer data);

static char* ruta_recurso(const char* nombre)
{
    if (g_file_test(nombre, G_FILE_TEST_EXISTS))
    {
        return g_strdup(nombre);
    }

    char* exe = g_file_read_link("/proc/self/exe", NULL);
    if (exe != NULL)
    {
        char* dir = g_path_get_dirname(exe);
        char* ruta = g_build_filename(dir, nombre, NULL);
        g_free(dir);
        g_free(exe);
        return ruta;
    }

    return g_strdup(nombre);
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
    gtk_widget_set_tooltip_text(GTK_WIDGET(lbl_carpeta), NULL);

    char* ruta = g_file_get_parse_name(dir);
    gtk_widget_set_tooltip_text(GTK_WIDGET(lbl_carpeta), ruta);
    g_free(ruta);
    g_free(mayus);
    g_free(base);
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
    {
        "nuevo", act_nuevo, NULL, NULL, NULL
    },
    {"abrir", act_abrir, NULL, NULL, NULL},
    {"abrir-carpeta", act_abrir_carpeta, NULL, NULL, NULL},
    {"guardar", act_guardar, NULL, NULL, NULL},
    {"guardar-como", act_guardar_como, NULL, NULL, NULL},
    {"cerrar-pestana", act_cerrar_pestana, NULL, NULL, NULL},
    {"sidebar", NULL, NULL, "true", act_sidebar_estado},
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

    if (!g_file_get_contents(ruta, &texto, NULL, NULL))
    {
        g_warning("No se encontró quark.css (%s); se usará el tema por defecto", ruta);
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

static void inicio(GApplication* app, gpointer user_data)
{
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, NULL);
    cargar_css();

    char* ruta_ui = ruta_recurso("quarkgtk.ui");
    GtkBuilder* build = gtk_builder_new_from_file(ruta_ui);
    g_free(ruta_ui);

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

    g_signal_connect(ventana, "close-request", G_CALLBACK(on_close_request), NULL);
    g_signal_connect(nb, "notify::page", G_CALLBACK(on_pagina_cambiada), NULL);

    GtkDropTarget* drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(drop, "drop", G_CALLBACK(on_drop), NULL);
    gtk_widget_add_controller(GTK_WIDGET(ventana), GTK_EVENT_CONTROLLER(drop));

    g_object_unref(build);

    crear_documento();
}

static void on_activate(GApplication* application)
{
    gtk_window_present(ventana);
}

static void abierto(GApplication* application, GFile**files, gint n_files, const gchar* hint)
{
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

    g_clear_object(&carpeta_actual);
    g_object_unref(app);
    return stat;
}
