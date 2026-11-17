/*
 * quark_formato.c - Formateador de código C/C++ para Quark.
 *
 * Funcionamiento del motor interno, en cuatro pasos:
 *   1. lexer:           texto -> lista de tokens (con saltos de línea y espacios originales)
 *   2. insertar_llaves: añade { } a los cuerpos de if/else/for/while/do sin llaves
 *   3. anotar:          clasifica llaves, casts, operadores unarios y etiquetas
 *   4. emitir:          escribe los tokens aplicando Allman, sangrado y espaciado
 *
 * Criterios de diseño:
 *   - Nunca se elimina ni se fusiona nada: salvo las llaves insertadas, la sucesión de
 *     tokens es idéntica a la original (los comentarios se conservan, reindentados).
 *   - Los saltos de línea del original se respetan, salvo los que el estilo obliga a
 *     poner o a quitar (llaves, `;`, `else`, etiquetas...). Una línea en blanco o más
 *     se reduce a una.
 *   - Idempotencia: cada decisión depende solo de la estructura de tokens y de lo ya
 *     emitido, de modo que formatear la salida otra vez no cambia nada.
 *   - Cuando no se puede distinguir una declaración de puntero de una multiplicación
 *     (`GtkBuilder* b`, `const char * ids`), el espaciado de `*` y `&` se deja como estaba.
 */
#include "quark_formato.h"

#include <gio/gio.h>
#include <string.h>

#define COL_LIMITE 100       /* columna máxima aproximada */
#define COL_LIMITE_INIT 80   /* ancho máximo de un inicializador que se deja en una línea */
#define SANGRIA 4
#define ANCHO_TAB 4

G_DEFINE_QUARK(quark-formato-error, quark_formato_error)

/* ------------------------------------------------------------------------- */
/* Tokens                                                                    */
/* ------------------------------------------------------------------------- */

typedef enum
{
    K_PAL,   /* identificador o palabra clave */
    K_NUM,
    K_CAD,
    K_CHR,
    K_CBLQ,  /* comentario de bloque */
    K_CLIN,  /* comentario de línea */
    K_PP,    /* directiva del preprocesador (línea lógica completa) */
    K_PUN
} Clase;

enum
{
    ROL_NINGUNO = 0,
    ROL_BLOQUE,  /* función, if, for, while, switch, bloque suelto */
    ROL_INIT,    /* inicializador o literal compuesto */
    ROL_STRUCT,  /* cuerpo de struct/union */
    ROL_ENUM
};

typedef struct
{
    Clase clase;
    char* txt;
    int nl;       /* saltos de línea antes del token en el original */
    int sp;       /* espacios antes, en la misma línea del original */
    int col;      /* columna original donde empieza */
    int linea;    /* línea original (para los mensajes de error) */
    int par;      /* token pareja de ( [ { ) ] } */
    gboolean sint;

    /* anotaciones */
    int rol;      /* de { y }: ROL_* */
    gboolean ctrl;         /* { } de un bloque de if/for/while/switch/else/do */
    gboolean cola_do;      /* `while` que cierra un do { } while (...) */
    gboolean unario;
    gboolean postfijo;
    gboolean amb;          /* * o & ambiguo (declaración o multiplicación): se respeta su espaciado */
    gboolean cast_cierre;  /* ) que cierra un cast */
    int etiqueta;          /* de ':' -> 1 case/default, 2 goto */
} Tok;

typedef struct
{
    Tok* v;
    int n;
    int cap;
} Lista;

static gboolean es_p(const Tok* t, const char* s)
{
    return t->clase == K_PUN && strcmp(t->txt, s) == 0;
}

static gboolean es_w(const Tok* t, const char* s)
{
    return t->clase == K_PAL && strcmp(t->txt, s) == 0;
}

static gboolean es_coment(const Tok* t)
{
    return t->clase == K_CBLQ || t->clase == K_CLIN;
}

static gboolean es_ruido(const Tok* t)
{
    return es_coment(t) || t->clase == K_PP;
}

static int ancho_txt(const char* s)
{
    return (int) g_utf8_strlen(s, -1);
}

static Lista* lista_nueva(void)
{
    Lista* L = g_new0(Lista, 1);
    L->cap = 256;
    L->v = g_new0(Tok, L->cap);
    return L;
}

static Tok* lista_anadir(Lista* L)
{
    if (L->n == L->cap)
    {
        L->cap *= 2;
        L->v = g_renew(Tok, L->v, L->cap);
    }

    Tok* t = &L->v[L->n++];
    memset(t, 0, sizeof *t);
    t->par = -1;
    return t;
}

static void lista_liberar(Lista* L)
{
    if (L == NULL)
    {
        return;
    }

    for (int i = 0; i < L->n; i++)
    {
        g_free(L->v[i].txt);
    }

    g_free(L->v);
    g_free(L);
}

/* Primer token >= i que no sea comentario ni directiva. */
static int sig_desde(const Lista* L, int i)
{
    while (i < L->n && es_ruido(&L->v[i]))
    {
        i++;
    }

    return i;
}

/* Último token <= i que no sea comentario ni directiva (-1 si no hay). */
static int ant_sig(const Lista* L, int i)
{
    while (i >= 0 && es_ruido(&L->v[i]))
    {
        i--;
    }

    return i;
}

/* ------------------------------------------------------------------------- */
/* Paso 1: lexer                                                             */
/* ------------------------------------------------------------------------- */

static gboolean es_ini_pal(guchar c)
{
    return g_ascii_isalpha(c) || c == '_' || c == '$' || c >= 0x80;
}

static gboolean es_car_pal(guchar c)
{
    return g_ascii_isalnum(c) || c == '_' || c == '$' || c >= 0x80;
}

static void avanzar_col(const char* s, gsize len, int* col)
{
    for (gsize k = 0; k < len; k++)
    {
        guchar ch = (guchar) s[k];

        if (ch == '\n')
        {
            *col = 0;
        }
        else if (ch == '\t')
        {
            *col = (*col / ANCHO_TAB + 1) * ANCHO_TAB;
        }
        else if ((ch & 0xC0) != 0x80)
        {
            (*col)++;
        }
    }
}

static const char* const PUNT3[] = {"<<=", ">>=", "...", NULL};
static const char* const PUNT2[] = {
    "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
    "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "##", NULL
};

static int largo_punt(const char* s)
{
    for (int k = 0; PUNT3[k] != NULL; k++)
    {
        if (strncmp(s, PUNT3[k], 3) == 0)
        {
            return 3;
        }
    }

    for (int k = 0; PUNT2[k] != NULL; k++)
    {
        if (strncmp(s, PUNT2[k], 2) == 0)
        {
            return 2;
        }
    }

    return 1;
}

/* Devuelve el índice posterior a la comilla de cierre, o 0 si no se cierra. */
static gsize saltar_literal(const char* s, gsize i, char comilla)
{
    i++;

    while (s[i] != '\0')
    {
        if (s[i] == '\\' && s[i + 1] != '\0')
        {
            i += 2;
        }
        else if (s[i] == '\n')
        {
            return 0;
        }
        else if (s[i] == comilla)
        {
            return i + 1;
        }
        else
        {
            i++;
        }
    }

    return 0;
}

static gboolean prefijo_literal(const char* s, gsize ini, gsize fin)
{
    gsize n = fin - ini;

    if (n == 1)
    {
        return s[ini] == 'L' || s[ini] == 'u' || s[ini] == 'U';
    }

    return n == 2 && s[ini] == 'u' && s[ini + 1] == '8';
}

static gboolean emparejar(Lista* L, GError** err)
{
    GArray* pila = g_array_new(FALSE, FALSE, sizeof(int));
    gboolean ok = TRUE;

    for (int i = 0; i < L->n && ok; i++)
    {
        Tok* t = &L->v[i];
        t->par = -1;

        if (t->clase != K_PUN || t->txt[1] != '\0')
        {
            continue;
        }

        char x = t->txt[0];

        if (x == '(' || x == '[' || x == '{')
        {
            g_array_append_val(pila, i);
        }
        else if (x == ')' || x == ']' || x == '}')
        {
            if (pila->len == 0)
            {
                g_set_error(err, QUARK_FORMATO_ERROR, QUARK_FORMATO_ERROR_LLAVES,
                            "«%c» sin pareja de apertura (línea %d)", x, t->linea);
                ok = FALSE;
                break;
            }

            int j = g_array_index(pila, int, pila->len - 1);
            char a = L->v[j].txt[0];
            gboolean casan = (a == '(' && x == ')') || (a == '[' && x == ']') || (a == '{' && x == '}');

            if (!casan)
            {
                g_set_error(err, QUARK_FORMATO_ERROR, QUARK_FORMATO_ERROR_LLAVES,
                            "«%c» (línea %d) no corresponde con «%c» de la línea %d",
                            x, t->linea, a, L->v[j].linea);
                ok = FALSE;
                break;
            }

            g_array_set_size(pila, pila->len - 1);
            t->par = j;
            L->v[j].par = i;
        }
    }

    if (ok && pila->len > 0)
    {
        int j = g_array_index(pila, int, pila->len - 1);
        g_set_error(err, QUARK_FORMATO_ERROR, QUARK_FORMATO_ERROR_LLAVES,
                    "«%c» sin cerrar (línea %d)", L->v[j].txt[0], L->v[j].linea);
        ok = FALSE;
    }

    g_array_free(pila, TRUE);
    return ok;
}

static Lista* lexer(const char* s, GError** err)
{
    Lista* L = lista_nueva();
    const gsize len = strlen(s);
    gsize i = 0;
    int linea = 1;
    int col = 0;
    int col_fin = 0;
    int nl = 0;
    gboolean ini_linea = TRUE;

    while (i < len)
    {
        char ch = s[i];

        if (ch == '\n')
        {
            nl++;
            linea++;
            col = 0;
            ini_linea = TRUE;
            i++;
            continue;
        }

        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\f' || ch == '\v')
        {
            avanzar_col(&s[i], 1, &col);
            i++;
            continue;
        }

        if (ch == '\\' && s[i + 1] == '\n')
        {
            i += 2;
            linea++;
            col = 0;
            continue;
        }

        const gsize ini = i;
        const int col_ini = col;
        const int linea_ini = linea;
        Clase clase;

        if (ch == '/' && s[i + 1] == '*')
        {
            const char* fin = strstr(s + i + 2, "*/");

            if (fin == NULL)
            {
                g_set_error(err, QUARK_FORMATO_ERROR, QUARK_FORMATO_ERROR_LEXICO,
                            "Comentario sin cerrar (línea %d)", linea_ini);
                lista_liberar(L);
                return NULL;
            }

            i = (gsize) (fin - s) + 2;
            clase = K_CBLQ;
        }
        else if (ch == '/' && s[i + 1] == '/')
        {
            while (i < len && s[i] != '\n')
            {
                if (s[i] == '\\' && s[i + 1] == '\n')
                {
                    i++;
                }

                i++;
            }

            clase = K_CLIN;
        }
        else if (ch == '#' && ini_linea)
        {
            while (i < len && s[i] != '\n')
            {
                if (s[i] == '\\' && s[i + 1] == '\n')
                {
                    i += 2;
                }
                else if (s[i] == '/' && s[i + 1] == '*')
                {
                    const char* fin = strstr(s + i + 2, "*/");

                    if (fin == NULL)
                    {
                        g_set_error(err, QUARK_FORMATO_ERROR, QUARK_FORMATO_ERROR_LEXICO,
                                    "Comentario sin cerrar (línea %d)", linea);
                        lista_liberar(L);
                        return NULL;
                    }

                    i = (gsize) (fin - s) + 2;
                }
                else
                {
                    i++;
                }
            }

            clase = K_PP;
        }
        else if (ch == '"' || ch == '\'')
        {
            gsize fin = saltar_literal(s, i, ch);

            if (fin == 0)
            {
                g_set_error(err, QUARK_FORMATO_ERROR, QUARK_FORMATO_ERROR_LEXICO,
                            ch == '"' ? "Cadena sin cerrar (línea %d)" : "Carácter sin cerrar (línea %d)",
                            linea_ini);
                lista_liberar(L);
                return NULL;
            }

            i = fin;
            clase = ch == '"' ? K_CAD : K_CHR;
        }
        else if (g_ascii_isdigit((guchar) ch) || (ch == '.' && g_ascii_isdigit((guchar) s[i + 1])))
        {
            i++;

            while (s[i] != '\0')
            {
                if (es_car_pal((guchar) s[i]) || s[i] == '.')
                {
                    i++;
                }
                else if ((s[i] == '+' || s[i] == '-')
                         && (s[i - 1] == 'e' || s[i - 1] == 'E' || s[i - 1] == 'p' || s[i - 1] == 'P'))
                {
                    i++;
                }
                else
                {
                    break;
                }
            }

            clase = K_NUM;
        }
        else if (es_ini_pal((guchar) ch))
        {
            while (es_car_pal((guchar) s[i]))
            {
                i++;
            }

            clase = K_PAL;

            if ((s[i] == '"' || s[i] == '\'') && prefijo_literal(s, ini, i))
            {
                char comilla = s[i];
                gsize fin = saltar_literal(s, i, comilla);

                if (fin == 0)
                {
                    g_set_error(err, QUARK_FORMATO_ERROR, QUARK_FORMATO_ERROR_LEXICO,
                                "Cadena sin cerrar (línea %d)", linea_ini);
                    lista_liberar(L);
                    return NULL;
                }

                i = fin;
                clase = comilla == '"' ? K_CAD : K_CHR;
            }
        }
        else
        {
            i += (gsize) largo_punt(s + i);
            clase = K_PUN;
        }

        gsize tam = i - ini;

        if (clase == K_CBLQ || clase == K_CLIN || clase == K_PP)
        {
            while (tam > 0 && (s[ini + tam - 1] == ' ' || s[ini + tam - 1] == '\t' || s[ini + tam - 1] == '\r'))
            {
                tam--;
            }
        }

        Tok* t = lista_anadir(L);
        t->clase = clase;
        t->txt = g_strndup(s + ini, tam);
        t->nl = nl;
        t->sp = nl > 0 ? 0 : MAX(0, col_ini - col_fin);
        t->col = col_ini;
        t->linea = linea_ini;

        avanzar_col(s + ini, i - ini, &col);

        for (gsize k = ini; k < i; k++)
        {
            if (s[k] == '\n')
            {
                linea++;
            }
        }

        col_fin = col;
        nl = 0;
        ini_linea = FALSE;
    }

    if (!emparejar(L, err))
    {
        lista_liberar(L);
        return NULL;
    }

    return L;
}

/* ------------------------------------------------------------------------- */
/* Paso 2: llaves obligatorias                                               */
/* ------------------------------------------------------------------------- */

/* Índice del último token de la sentencia que empieza en i. */
static int fin_sentencia(const Lista* L, int i)
{
    i = sig_desde(L, i);

    if (i >= L->n)
    {
        return L->n - 1;
    }

    const Tok* t = &L->v[i];

    if (es_p(t, "{"))
    {
        return t->par;
    }

    if (es_w(t, "if") || es_w(t, "for") || es_w(t, "while") || es_w(t, "switch"))
    {
        int p = sig_desde(L, i + 1);
        int fin_cab = (p < L->n && es_p(&L->v[p], "(")) ? L->v[p].par : p;
        int e = fin_sentencia(L, sig_desde(L, fin_cab + 1));

        if (es_w(t, "if"))
        {
            int s = sig_desde(L, e + 1);

            if (s < L->n && es_w(&L->v[s], "else"))
            {
                return fin_sentencia(L, s + 1);
            }
        }

        return e;
    }

    if (es_w(t, "do"))
    {
        int e = fin_sentencia(L, sig_desde(L, i + 1));
        int w = sig_desde(L, e + 1);
        int p = sig_desde(L, w + 1);
        int c = (p < L->n && es_p(&L->v[p], "(")) ? L->v[p].par : p;
        int s = sig_desde(L, c + 1);
        return s < L->n ? s : L->n - 1;
    }

    if (es_p(t, ";"))
    {
        return i;
    }

    for (int k = i; k < L->n; k++)
    {
        const Tok* u = &L->v[k];

        if (es_p(u, "(") || es_p(u, "[") || es_p(u, "{"))
        {
            k = u->par;
        }
        else if (es_p(u, ";"))
        {
            return k;
        }
    }

    return L->n - 1;
}

static void marcar_cuerpo(const Lista* L, int fin_cab, int* abre, int* cierra)
{
    int b = sig_desde(L, fin_cab + 1);

    if (b >= L->n || es_p(&L->v[b], "{") || es_p(&L->v[b], ";"))
    {
        return;
    }

    int e = fin_sentencia(L, b);

    /* los comentarios de final de línea se quedan con su sentencia */
    while (e + 1 < L->n && es_coment(&L->v[e + 1]) && L->v[e + 1].nl == 0)
    {
        e++;
    }

    abre[fin_cab]++;
    cierra[e]++;
}

static void anadir_sintetico(Lista* M, const char* txt)
{
    Tok* t = lista_anadir(M);
    t->clase = K_PUN;
    t->txt = g_strdup(txt);
    t->sint = TRUE;
}

static Lista* insertar_llaves(Lista* L, GError** err)
{
    const int n = L->n;
    int* abre = g_new0(int, n);
    int* cierra = g_new0(int, n);

    for (int i = 0; i < n; i++)
    {
        const Tok* t = &L->v[i];

        if (t->clase != K_PAL)
        {
            continue;
        }

        if (es_w(t, "if") || es_w(t, "for") || (es_w(t, "while") && !t->cola_do))
        {
            int p = sig_desde(L, i + 1);

            if (p < n && es_p(&L->v[p], "("))
            {
                marcar_cuerpo(L, L->v[p].par, abre, cierra);
            }
        }
        else if (es_w(t, "else"))
        {
            int p = sig_desde(L, i + 1);

            if (p < n && !es_w(&L->v[p], "if"))
            {
                marcar_cuerpo(L, i, abre, cierra);
            }
        }
        else if (es_w(t, "do"))
        {
            marcar_cuerpo(L, i, abre, cierra);

            int e = fin_sentencia(L, sig_desde(L, i + 1));
            int w = sig_desde(L, e + 1);

            if (w < n && es_w(&L->v[w], "while"))
            {
                L->v[w].cola_do = TRUE;
            }
        }
    }

    Lista* M = lista_nueva();

    for (int i = 0; i < n; i++)
    {
        Tok* d = lista_anadir(M);
        *d = L->v[i];
        d->txt = g_strdup(L->v[i].txt);

        for (int k = 0; k < cierra[i]; k++)
        {
            anadir_sintetico(M, "}");
        }

        if (abre[i] > 0)
        {
            anadir_sintetico(M, "{");
        }
    }

    g_free(abre);
    g_free(cierra);
    lista_liberar(L);

    if (!emparejar(M, err))
    {
        lista_liberar(M);
        return NULL;
    }

    return M;
}

/* ------------------------------------------------------------------------- */
/* Paso 3: anotaciones                                                       */
/* ------------------------------------------------------------------------- */

static gboolean tipo_conocido(const char* w)
{
    static const char* const claves[] = {
        "void", "char", "short", "int", "long", "float", "double", "signed", "unsigned",
        "const", "volatile", "struct", "union", "enum", "_Bool", "bool", "size_t", "ssize_t",
        "gboolean", "gchar", "guchar", "gint", "guint", "glong", "gulong", "gshort", "gushort",
        "gfloat", "gdouble", "gpointer", "gconstpointer", "gsize", "gssize", "goffset", NULL
    };

    for (int k = 0; claves[k] != NULL; k++)
    {
        if (strcmp(w, claves[k]) == 0)
        {
            return TRUE;
        }
    }

    gsize n = strlen(w);

    if (n > 2 && strcmp(w + n - 2, "_t") == 0)
    {
        return TRUE;
    }

    if (g_ascii_isupper(w[0]))
    {
        for (gsize k = 1; k < n; k++)
        {
            if (g_ascii_islower(w[k]))
            {
                return TRUE;
            }
        }
    }

    return FALSE;
}

/* Palabras tras las que viene un operando (para distinguir unario de binario y casts). */
static gboolean pal_antes_operando(const char* w)
{
    return strcmp(w, "return") == 0 || strcmp(w, "case") == 0 || strcmp(w, "else") == 0
           || strcmp(w, "do") == 0 || strcmp(w, "goto") == 0;
}

/* Palabras que llevan un espacio antes del paréntesis: if (, for (, while (... */
static gboolean pal_espacio_paren(const char* w)
{
    return strcmp(w, "if") == 0 || strcmp(w, "for") == 0 || strcmp(w, "while") == 0
           || strcmp(w, "switch") == 0 || strcmp(w, "return") == 0 || strcmp(w, "case") == 0;
}

static gboolean es_llamada_o_decl(const Tok* p)
{
    if (p->clase != K_PAL || pal_espacio_paren(p->txt))
    {
        return FALSE;
    }

    return strcmp(p->txt, "sizeof") != 0 && strcmp(p->txt, "defined") != 0
           && strcmp(p->txt, "typeof") != 0 && strcmp(p->txt, "else") != 0 && strcmp(p->txt, "do") != 0;
}

static void clasificar_llaves(Lista* L)
{
    GArray* pila = g_array_new(FALSE, FALSE, sizeof(int));  /* roles de las { abiertas */

    for (int i = 0; i < L->n; i++)
    {
        Tok* t = &L->v[i];

        if (es_p(t, "}"))
        {
            if (pila->len > 0)
            {
                g_array_set_size(pila, pila->len - 1);
            }

            continue;
        }

        if (!es_p(t, "{"))
        {
            continue;
        }

        int rol = ROL_BLOQUE;
        gboolean ctrl = FALSE;
        int padre = pila->len > 0 ? g_array_index(pila, int, pila->len - 1) : ROL_NINGUNO;
        int p = ant_sig(L, i - 1);

        if (p >= 0)
        {
            const Tok* tp = &L->v[p];

            if (es_p(tp, "="))
            {
                rol = ROL_INIT;
            }
            else if (es_p(tp, ",") || es_p(tp, "("))
            {
                rol = ROL_INIT;
            }
            else if (es_p(tp, "{"))
            {
                rol = (padre == ROL_INIT || padre == ROL_ENUM) ? ROL_INIT : ROL_BLOQUE;
            }
            else if (es_p(tp, ")"))
            {
                int q = tp->par;
                int r = q > 0 ? ant_sig(L, q - 1) : -1;

                if (r >= 0 && L->v[r].clase == K_PAL)
                {
                    if (es_w(&L->v[r], "return"))
                    {
                        rol = ROL_INIT;
                    }
                    else
                    {
                        ctrl = es_w(&L->v[r], "if") || es_w(&L->v[r], "for")
                               || es_w(&L->v[r], "while") || es_w(&L->v[r], "switch");
                    }
                }
                else if (r >= 0 && !es_p(&L->v[r], ")") && !es_p(&L->v[r], "]"))
                {
                    rol = ROL_INIT;  /* literal compuesto: (Tipo){ ... } */
                }
            }
            else if (es_p(tp, "]"))
            {
                rol = ROL_INIT;
            }
            else if (tp->clase == K_PAL)
            {
                if (es_w(tp, "else") || es_w(tp, "do"))
                {
                    ctrl = TRUE;
                }
                else
                {
                    for (int k = p; k >= 0; k = ant_sig(L, k - 1))
                    {
                        const Tok* u = &L->v[k];

                        if (es_p(u, ";") || es_p(u, "{") || es_p(u, "}"))
                        {
                            break;
                        }

                        if (es_w(u, "enum"))
                        {
                            rol = ROL_ENUM;
                            break;
                        }

                        if (es_w(u, "struct") || es_w(u, "union"))
                        {
                            rol = ROL_STRUCT;
                            break;
                        }
                    }
                }
            }
        }

        t->rol = rol;
        t->ctrl = ctrl;

        if (t->par > 0)
        {
            L->v[t->par].rol = rol;
            L->v[t->par].ctrl = ctrl;
        }

        g_array_append_val(pila, rol);
    }

    g_array_free(pila, TRUE);
}

static gboolean es_cast(const Lista* L, int a)
{
    int c = L->v[a].par;

    if (c <= a + 1)
    {
        return FALSE;
    }

    int p = ant_sig(L, a - 1);

    if (p >= 0)
    {
        const Tok* tp = &L->v[p];

        if (tp->clase == K_PAL && !pal_antes_operando(tp->txt))
        {
            return FALSE;
        }

        if (tp->clase == K_NUM || tp->clase == K_CAD || tp->clase == K_CHR
            || es_p(tp, ")") || es_p(tp, "]"))
        {
            return FALSE;
        }
    }

    gboolean hay_pal = FALSE;
    gboolean hay_tipo = FALSE;

    for (int k = a + 1; k < c; k++)
    {
        const Tok* t = &L->v[k];

        if (t->clase == K_PAL)
        {
            hay_pal = TRUE;
            hay_tipo = hay_tipo || tipo_conocido(t->txt);
        }
        else if (es_p(t, "*"))
        {
            hay_tipo = TRUE;
        }
        else
        {
            return FALSE;
        }
    }

    int s = sig_desde(L, c + 1);

    if (!hay_pal || !hay_tipo || s >= L->n)
    {
        return FALSE;
    }

    const Tok* ts = &L->v[s];

    return ts->clase == K_PAL || ts->clase == K_NUM || ts->clase == K_CAD || ts->clase == K_CHR
           || es_p(ts, "(") || es_p(ts, "{") || es_p(ts, "!") || es_p(ts, "~") || es_p(ts, "-")
           || es_p(ts, "+") || es_p(ts, "*") || es_p(ts, "&") || es_p(ts, "++") || es_p(ts, "--");
}

static gboolean es_op_unario_posible(const Tok* t)
{
    return es_p(t, "+") || es_p(t, "-") || es_p(t, "*") || es_p(t, "&") || es_p(t, "!")
           || es_p(t, "~") || es_p(t, "++") || es_p(t, "--");
}

static void marcar_operadores(Lista* L)
{
    for (int i = 0; i < L->n; i++)
    {
        if (es_p(&L->v[i], "(") && L->v[i].par > 0 && es_cast(L, i))
        {
            L->v[L->v[i].par].cast_cierre = TRUE;
        }
    }

    for (int i = 0; i < L->n; i++)
    {
        Tok* t = &L->v[i];

        if (t->clase != K_PUN || !es_op_unario_posible(t))
        {
            continue;
        }

        int p = ant_sig(L, i - 1);
        gboolean prev_operando = FALSE;
        gboolean prev_pal = FALSE;
        gboolean prev_ptr = FALSE;

        if (p >= 0)
        {
            const Tok* tp = &L->v[p];

            if (tp->clase == K_NUM || tp->clase == K_CAD || tp->clase == K_CHR)
            {
                prev_operando = TRUE;
            }
            else if (tp->clase == K_PAL)
            {
                prev_operando = !pal_antes_operando(tp->txt);
                prev_pal = prev_operando;
            }
            else if (es_p(tp, ")"))
            {
                prev_operando = !tp->cast_cierre;
            }
            else if (es_p(tp, "]"))
            {
                prev_operando = TRUE;
            }
            else if (es_p(tp, "}"))
            {
                prev_operando = tp->rol == ROL_INIT;
            }
            else if ((es_p(tp, "++") || es_p(tp, "--")) && tp->postfijo)
            {
                prev_operando = TRUE;
            }
            else if (es_p(tp, "*") && tp->amb)
            {
                prev_ptr = TRUE;
            }
        }

        if (es_p(t, "!") || es_p(t, "~"))
        {
            t->unario = TRUE;
        }
        else if (es_p(t, "++") || es_p(t, "--"))
        {
            t->postfijo = prev_operando;
            t->unario = !prev_operando;
        }
        else if (es_p(t, "*") && prev_ptr)
        {
            t->amb = TRUE;  /* ** en una declaración */
        }
        else
        {
            t->unario = !prev_operando;

            if (!t->unario && (es_p(t, "*") || es_p(t, "&")) && prev_pal)
            {
                t->amb = TRUE;
            }
        }
    }
}

static void marcar_etiquetas(Lista* L)
{
    for (int i = 0; i < L->n; i++)
    {
        const Tok* t = &L->v[i];

        if (t->clase != K_PAL)
        {
            continue;
        }

        if (es_w(t, "case"))
        {
            int q = 0;

            for (int k = i + 1; k < L->n; k++)
            {
                const Tok* u = &L->v[k];

                if ((es_p(u, "(") || es_p(u, "[") || es_p(u, "{")) && u->par > k)
                {
                    k = u->par;
                }
                else if (es_p(u, "?"))
                {
                    q++;
                }
                else if (es_p(u, ":"))
                {
                    if (q == 0)
                    {
                        L->v[k].etiqueta = 1;
                        break;
                    }

                    q--;
                }
                else if (es_p(u, ";"))
                {
                    break;
                }
            }
        }
        else if (es_w(t, "default"))
        {
            int s = sig_desde(L, i + 1);

            if (s < L->n && es_p(&L->v[s], ":"))
            {
                L->v[s].etiqueta = 1;
            }
        }
        else
        {
            int s = sig_desde(L, i + 1);
            int s2 = s < L->n ? sig_desde(L, s + 1) : L->n;

            if (s >= L->n || !es_p(&L->v[s], ":") || (s2 < L->n && es_p(&L->v[s2], ":")))
            {
                continue;
            }

            int p = ant_sig(L, i - 1);
            gboolean ini = p < 0 || es_p(&L->v[p], ";") || es_p(&L->v[p], "{")
                           || (es_p(&L->v[p], "}") && L->v[p].rol != ROL_INIT)
                           || (es_p(&L->v[p], ":") && L->v[p].etiqueta > 0);

            if (ini && !es_w(t, "else") && !es_w(t, "do") && !es_w(t, "return"))
            {
                L->v[s].etiqueta = 2;
            }
        }
    }
}

static void anotar(Lista* L)
{
    clasificar_llaves(L);
    marcar_operadores(L);
    marcar_etiquetas(L);
}

/* ------------------------------------------------------------------------- */
/* Espaciado entre dos tokens contiguos de una misma línea                   */
/* ------------------------------------------------------------------------- */

static int espacio_antes(const Lista* L, int i)
{
    if (i == 0)
    {
        return 0;
    }

    const Tok* c = &L->v[i];
    const Tok* p = &L->v[i - 1];

    if (es_coment(p))
    {
        return (es_p(c, ",") || es_p(c, ";") || es_p(c, ")") || es_p(c, "]")) ? 0 : 1;
    }

    if (p->clase == K_PP || c->clase == K_PP)
    {
        return 0;
    }

    const gboolean cp = c->clase == K_PUN;
    const gboolean pp = p->clase == K_PUN;

    if ((pp && p->amb) || (cp && c->amb))
    {
        return c->sp > 0 ? 1 : 0;
    }

    if (cp && es_p(c, "}") && c->rol == ROL_INIT)
    {
        return 0;
    }

    if (pp && es_p(p, "{") && p->rol == ROL_INIT)
    {
        return 0;
    }

    if (pp && (es_p(p, "(") || es_p(p, "[")))
    {
        return 0;
    }

    if (cp && (es_p(c, ")") || es_p(c, "]") || es_p(c, ",") || es_p(c, ";")))
    {
        return 0;
    }

    if (pp && (es_p(p, ",") || es_p(p, ";")))
    {
        return 1;
    }

    if (cp && es_p(c, "{") && c->rol == ROL_INIT)
    {
        return (pp && es_p(p, ")")) ? 0 : 1;
    }

    if (cp && (es_p(c, ".") || es_p(c, "->")))
    {
        return 0;
    }

    if (pp && (es_p(p, ".") || es_p(p, "->")))
    {
        return 0;
    }

    if (cp && c->postfijo)
    {
        return 0;
    }

    if (pp && p->postfijo)
    {
        return 1;
    }

    if (pp && p->unario)
    {
        return 0;
    }

    if (cp && es_p(c, "("))
    {
        if (p->clase == K_PAL)
        {
            return pal_espacio_paren(p->txt) ? 1 : 0;
        }

        if (pp && es_p(p, ")"))
        {
            return p->cast_cierre ? 1 : 0;
        }

        if (pp && es_p(p, "]"))
        {
            return 0;
        }

        return 1;
    }

    if (cp && es_p(c, "["))
    {
        return (p->clase == K_PAL || (pp && (es_p(p, ")") || es_p(p, "]")))) ? 0 : 1;
    }

    if (cp && es_p(c, ":") && c->etiqueta > 0)
    {
        return 0;
    }

    return 1;
}

/* Ancho de un grupo ( ... ), [ ... ] o { ... } escrito en una sola línea. */
static int ancho_grupo(const Lista* L, int a)
{
    int w = 1;
    int c = L->v[a].par;

    for (int k = a + 1; k <= c; k++)
    {
        w += espacio_antes(L, k) + ancho_txt(L->v[k].txt);
    }

    return w;
}

/* Ancho de lo que sigue al cierre de un grupo, hasta el fin de la sentencia o del elemento. */
static int ancho_cola(const Lista* L, int c)
{
    int w = 0;

    for (int k = c + 1; k < L->n; k++)
    {
        const Tok* t = &L->v[k];

        if (t->nl > 0 || es_ruido(t) || (es_p(t, "{") && t->rol != ROL_INIT))
        {
            break;
        }

        w += espacio_antes(L, k) + ancho_txt(t->txt);

        if (es_p(t, ";") || es_p(t, ","))
        {
            break;
        }
    }

    return w;
}

/* ------------------------------------------------------------------------- */
/* Paso 4: emisión                                                           */
/* ------------------------------------------------------------------------- */

enum
{
    F_BLOQUE,
    F_INIT,
    F_PAREN
};

enum
{
    INIT_INLINE,
    INIT_UNO,
    INIT_MULTI
};

typedef struct
{
    int tipo;
    int inner;       /* sangría de los elementos o sentencias */
    int close;       /* sangría del cierre */
    int cont;        /* (F_PAREN) columna de las líneas de continuación */
    int case_extra;  /* (F_BLOQUE) sangría adicional dentro de un case */
    gboolean multi;     /* { y } en líneas propias */
    gboolean explotar;  /* salto tras cada coma de primer nivel */
    gboolean ctrl;
    gboolean estruct;   /* struct/union/enum: tras } puede seguir un declarador */
} Frame;

typedef struct
{
    const Lista* L;
    GString* out;
    GArray* pila;
    int col;
    int ind_linea;  /* sangría de la línea actual */
    int ind_stmt;   /* sangría de la primera línea de la sentencia en curso */
    gboolean primero;
    gboolean linea_vacia;
    gboolean forzar_nl;
    gboolean inicio_stmt;
    gboolean tras_etiqueta;
    gboolean cierra_ctrl_prev;
} Est;

static Frame* tope(Est* e)
{
    return e->pila->len > 0 ? &g_array_index(e->pila, Frame, e->pila->len - 1) : NULL;
}

static void emitir_txt(Est* e, const char* s)
{
    g_string_append(e->out, s);
    avanzar_col(s, strlen(s), &e->col);
    e->linea_vacia = FALSE;
}

static void emitir_espacios(Est* e, int n)
{
    for (int k = 0; k < n; k++)
    {
        g_string_append_c(e->out, ' ');
    }

    e->col += n;
}

static void nueva_linea(Est* e, int blancas, int sangria)
{
    for (int k = 0; k < 1 + blancas; k++)
    {
        g_string_append_c(e->out, '\n');
    }

    e->col = 0;
    emitir_espacios(e, sangria);
    e->ind_linea = sangria;
    e->linea_vacia = TRUE;
}

static gboolean es_etiqueta_case(const Tok* c)
{
    return es_w(c, "case") || es_w(c, "default");
}

/* Sangría de un token que abre línea, sin los casos especiales de llaves. */
static int sangria_base(Est* e, gboolean es_label, gboolean era_inicio)
{
    Frame* f = tope(e);

    if (f == NULL)
    {
        return era_inicio ? 0 : e->ind_stmt + SANGRIA;
    }

    switch (f->tipo)
    {
        case F_PAREN:
            return f->cont;

        case F_INIT:
            return f->inner;

        default:
            if (era_inicio)
            {
                return es_label ? f->inner : f->inner + f->case_extra;
            }

            return e->ind_stmt + SANGRIA;
    }
}

static int calcular_sangria(Est* e, const Tok* c, gboolean era_inicio, gboolean tras_et)
{
    Frame* f = tope(e);

    if (es_p(c, "}") || es_p(c, ")") || es_p(c, "]"))
    {
        return f != NULL ? f->close : 0;
    }

    if (es_p(c, "{") && c->rol != ROL_INIT)
    {
        if (era_inicio)
        {
            if (f == NULL)
            {
                return 0;
            }

            return (f->tipo == F_BLOQUE && !tras_et) ? f->inner + f->case_extra : f->inner;
        }

        return e->ind_stmt;
    }

    return sangria_base(e, es_etiqueta_case(c), era_inicio);
}

static gboolean forzado_antes(Est* e, const Tok* c)
{
    if (es_p(c, "{"))
    {
        return c->rol != ROL_INIT;
    }

    if (es_p(c, "}"))
    {
        Frame* f = tope(e);
        return f != NULL && (f->tipo == F_BLOQUE || f->multi);
    }

    return es_w(c, "else");
}

static gboolean suprimir_salto(const Lista* L, int i)
{
    const Tok* c = &L->v[i];

    if (i == 0)
    {
        return FALSE;
    }

    if (es_p(c, "{") && c->rol == ROL_INIT && es_p(&L->v[i - 1], "="))
    {
        return TRUE;
    }

    return es_w(c, "if") && es_w(&L->v[i - 1], "else");
}

/* Ancho del operador y de su operando derecho (para decidir si se parte la línea). */
static int ancho_operando(const Lista* L, int i, int esp)
{
    int w = esp + ancho_txt(L->v[i].txt);

    if (L->v[i].clase == K_CAD)
    {
        const Tok* s = i + 1 < L->n ? &L->v[i + 1] : NULL;
        return (s != NULL && (es_p(s, ")") || es_p(s, ";") || es_p(s, ","))) ? w + 1 : w;
    }

    int prof = 0;

    for (int k = i + 1; k < L->n; k++)
    {
        const Tok* t = &L->v[k];

        if (es_ruido(t))
        {
            break;
        }

        if (prof == 0
            && (es_p(t, "&&") || es_p(t, "||") || es_p(t, ";") || es_p(t, ",") || es_p(t, "?")
                || es_p(t, ":") || es_p(t, ")") || es_p(t, "]") || es_p(t, "}") || es_p(t, "{")))
        {
            if (es_p(t, ")") || es_p(t, ";"))
            {
                w += 1;
            }

            break;
        }

        if (es_p(t, "(") || es_p(t, "["))
        {
            prof++;
        }
        else if (es_p(t, ")") || es_p(t, "]"))
        {
            prof--;
        }

        w += espacio_antes(L, k) + ancho_txt(t->txt);
    }

    return w;
}

static gboolean necesita_envolver(Est* e, int i, int esp)
{
    const Lista* L = e->L;
    const Tok* c = &L->v[i];

    if (e->linea_vacia)
    {
        return FALSE;
    }

    if (es_p(c, "&&") || es_p(c, "||") || (c->clase == K_CAD && i > 0 && L->v[i - 1].clase == K_CAD))
    {
        return e->col + ancho_operando(L, i, esp) > COL_LIMITE;
    }

    return FALSE;
}

/* Un grupo de argumentos se parte (un argumento por línea) si no cabe en la línea. */
static gboolean decidir_explotar(Est* e, int a)
{
    const Lista* L = e->L;
    int cierre = L->v[a].par;
    int p = ant_sig(L, a - 1);

    if (p < 0 || !es_llamada_o_decl(&L->v[p]))
    {
        return FALSE;
    }

    int comas = 0;
    int prof = 0;

    /* un grupo que el autor ya partió en varias líneas se respeta tal cual */
    if (L->v[cierre].nl > 0)
    {
        return FALSE;
    }

    for (int k = a + 1; k < cierre; k++)
    {
        const Tok* t = &L->v[k];

        if (es_ruido(t) || t->nl > 0)
        {
            return FALSE;
        }

        if (es_p(t, "(") || es_p(t, "[") || es_p(t, "{"))
        {
            prof++;
        }
        else if (es_p(t, ")") || es_p(t, "]") || es_p(t, "}"))
        {
            prof--;
        }
        else if (prof == 0 && es_p(t, ","))
        {
            comas++;
        }
    }

    if (comas == 0)
    {
        return FALSE;
    }

    return (e->col - 1) + ancho_grupo(L, a) + ancho_cola(L, cierre) > COL_LIMITE;
}

static int analizar_init(Est* e, int a)
{
    const Lista* L = e->L;
    int cierre = L->v[a].par;
    gboolean multi = L->v[cierre].nl > 0;
    gboolean com = FALSE;
    gboolean en_elem = FALSE;
    int elementos = 0;
    int prof = 0;

    for (int k = a + 1; k < cierre; k++)
    {
        const Tok* t = &L->v[k];

        if (t->nl > 0)
        {
            multi = TRUE;
        }

        if (es_ruido(t))
        {
            com = TRUE;
            continue;
        }

        if (prof == 0 && es_p(t, ","))
        {
            if (en_elem)
            {
                elementos++;
            }

            en_elem = FALSE;
            continue;
        }

        en_elem = TRUE;

        if (es_p(t, "(") || es_p(t, "[") || es_p(t, "{"))
        {
            prof++;
        }
        else if (es_p(t, ")") || es_p(t, "]") || es_p(t, "}"))
        {
            prof--;
        }
    }

    if (en_elem)
    {
        elementos++;
    }

    if (elementos == 0 && !com)
    {
        return INIT_INLINE;
    }

    if (multi || com)
    {
        return INIT_MULTI;
    }

    if (elementos <= 3 && (e->col - 1) + ancho_grupo(L, a) + ancho_cola(L, cierre) <= COL_LIMITE_INIT)
    {
        return INIT_INLINE;
    }

    return INIT_UNO;
}

static gboolean es_macro_mayusculas(const char* w)
{
    gboolean mayus = FALSE;

    for (const char* q = w; *q != '\0'; q++)
    {
        if (g_ascii_isupper(*q))
        {
            mayus = TRUE;
        }
        else if (!g_ascii_isdigit(*q) && *q != '_')
        {
            return FALSE;
        }
    }

    return mayus && strlen(w) >= 2;
}

static void emitir_pp(Est* e, const char* txt)
{
    gchar** lineas = g_strsplit(txt, "\n", -1);

    for (int k = 0; lineas[k] != NULL; k++)
    {
        char* l = g_strdup(lineas[k]);
        g_strchomp(l);

        if (k == 0)
        {
            g_strchug(l);
        }
        else
        {
            g_string_append_c(e->out, '\n');
            e->col = 0;
        }

        emitir_txt(e, l);
        g_free(l);
    }

    g_strfreev(lineas);
}

static void emitir_comentario(Est* e, const Tok* c, int col_ini)
{
    if (c->clase == K_CLIN || strchr(c->txt, '\n') == NULL)
    {
        emitir_txt(e, c->txt);
        return;
    }

    gchar** lineas = g_strsplit(c->txt, "\n", -1);
    const int delta = col_ini - c->col;

    for (int k = 0; lineas[k] != NULL; k++)
    {
        const char* l = lineas[k];
        int w = 0;

        while (*l == ' ' || *l == '\t')
        {
            w = (*l == '\t') ? (w / ANCHO_TAB + 1) * ANCHO_TAB : w + 1;
            l++;
        }

        char* limpio = g_strchomp(g_strdup(l));

        if (k == 0)
        {
            emitir_txt(e, limpio);
        }
        else if (*limpio == '\0')
        {
            g_string_append_c(e->out, '\n');
            e->col = 0;
        }
        else
        {
            g_string_append_c(e->out, '\n');
            e->col = 0;
            emitir_espacios(e, MAX(0, w + delta));
            emitir_txt(e, limpio);
        }

        g_free(limpio);
    }

    g_strfreev(lineas);
}

static int sangria_comentario(Est* e, int i)
{
    const Lista* L = e->L;
    int s = sig_desde(L, i + 1);
    gboolean antes_de_label = s < L->n && es_etiqueta_case(&L->v[s]);
    return sangria_base(e, antes_de_label, e->inicio_stmt);
}

static void procesar_comentario(Est* e, int i)
{
    const Tok* c = &e->L->v[i];
    const gboolean misma_linea = !e->primero && c->nl == 0;

    e->cierra_ctrl_prev = FALSE;

    if (misma_linea)
    {
        emitir_espacios(e, MAX(1, c->sp));
    }
    else
    {
        int sang = sangria_comentario(e, i);

        if (e->primero)
        {
            emitir_espacios(e, sang);
            e->ind_linea = sang;
        }
        else
        {
            nueva_linea(e, c->nl >= 2 ? 1 : 0, sang);
        }

        e->forzar_nl = FALSE;
    }

    emitir_comentario(e, c, e->col);

    if (c->clase == K_CLIN)
    {
        e->forzar_nl = TRUE;
    }

    e->primero = FALSE;
}

static void procesar_directiva(Est* e, int i)
{
    const Tok* c = &e->L->v[i];

    if (!e->primero)
    {
        nueva_linea(e, c->nl >= 2 ? 1 : 0, 0);
    }

    emitir_pp(e, c->txt);
    e->forzar_nl = TRUE;
    e->primero = FALSE;
    e->cierra_ctrl_prev = FALSE;
}

static gboolean es_inicio_control(const Tok* c)
{
    return (es_w(c, "if") || es_w(c, "for") || es_w(c, "while") || es_w(c, "switch") || es_w(c, "do"))
           && !c->cola_do;
}

static void procesar_codigo(Est* e, int i)
{
    const Lista* L = e->L;
    const Tok* c = &L->v[i];
    const gboolean era_inicio = e->inicio_stmt;
    const gboolean tras_et = e->tras_etiqueta;
    const gboolean cierra_ctrl = e->cierra_ctrl_prev;

    e->inicio_stmt = FALSE;
    e->tras_etiqueta = FALSE;
    e->cierra_ctrl_prev = FALSE;

    gboolean romper = FALSE;

    if (!e->primero)
    {
        romper = c->nl > 0 || e->forzar_nl || forzado_antes(e, c);

        if (romper && suprimir_salto(L, i))
        {
            romper = FALSE;
        }
    }

    int esp = romper ? 0 : espacio_antes(L, i);

    if (!romper && !e->primero && necesita_envolver(e, i, esp))
    {
        romper = TRUE;
        esp = 0;
    }

    if (e->primero)
    {
        int sang = calcular_sangria(e, c, era_inicio, tras_et);
        emitir_espacios(e, sang);
        e->ind_linea = sang;
    }
    else if (romper)
    {
        int blancas = c->nl >= 2 ? 1 : 0;

        if (cierra_ctrl && es_inicio_control(c))
        {
            blancas = 1;
        }

        if (es_w(c, "else") || (es_p(c, "{") && c->rol != ROL_INIT))
        {
            blancas = 0;
        }

        nueva_linea(e, blancas, calcular_sangria(e, c, era_inicio, tras_et));
        e->forzar_nl = FALSE;
    }
    else
    {
        emitir_espacios(e, esp);
    }

    if (era_inicio)
    {
        e->ind_stmt = e->ind_linea;
    }

    emitir_txt(e, c->txt);
    e->primero = FALSE;

    if (c->clase != K_PUN)
    {
        return;
    }

    const char* x = c->txt;
    Frame* f = tope(e);

    if (strcmp(x, "(") == 0 || strcmp(x, "[") == 0)
    {
        Frame nf = {0};
        nf.tipo = F_PAREN;
        nf.close = e->ind_linea;

        gboolean alineado = i + 1 < L->n && L->v[i + 1].nl == 0 && L->v[i + 1].clase != K_CLIN
                            && !es_p(&L->v[i + 1], ")") && !es_p(&L->v[i + 1], "]");

        nf.cont = alineado ? e->col : e->ind_linea + SANGRIA;

        if (x[0] == '(')
        {
            nf.explotar = decidir_explotar(e, i);
        }

        g_array_append_val(e->pila, nf);
    }
    else if (strcmp(x, ")") == 0 || strcmp(x, "]") == 0)
    {
        if (e->pila->len > 0)
        {
            g_array_set_size(e->pila, e->pila->len - 1);
        }

        Frame* g = tope(e);
        int ap = c->par;

        if ((g == NULL || g->tipo == F_BLOQUE) && ap > 0)
        {
            int p = ant_sig(L, ap - 1);
            int s = sig_desde(L, i + 1);

            if (p >= 0 && L->v[p].clase == K_PAL && es_macro_mayusculas(L->v[p].txt)
                && s < L->n && L->v[s].nl > 0 && L->v[s].clase == K_PAL)
            {
                e->inicio_stmt = TRUE;  /* invocación de macro sin ; */
            }
        }
    }
    else if (strcmp(x, "{") == 0)
    {
        Frame nf = {0};

        if (c->rol == ROL_INIT)
        {
            int modo = analizar_init(e, i);
            nf.tipo = F_INIT;
            nf.inner = e->ind_linea + SANGRIA;
            nf.close = e->ind_linea;
            nf.multi = modo != INIT_INLINE;
            nf.explotar = modo == INIT_UNO;
            e->forzar_nl = nf.multi;
        }
        else if (c->rol == ROL_ENUM)
        {
            nf.tipo = F_INIT;
            nf.inner = e->ind_linea + SANGRIA;
            nf.close = e->ind_linea;
            nf.multi = TRUE;
            nf.explotar = TRUE;
            nf.estruct = TRUE;
            e->forzar_nl = TRUE;
        }
        else
        {
            nf.tipo = F_BLOQUE;
            nf.inner = e->ind_linea + SANGRIA;
            nf.close = e->ind_linea;
            nf.ctrl = c->ctrl;
            nf.estruct = c->rol == ROL_STRUCT;
            e->forzar_nl = TRUE;
            e->inicio_stmt = TRUE;
        }

        g_array_append_val(e->pila, nf);
    }
    else if (strcmp(x, "}") == 0)
    {
        Frame fr = {0};

        if (f != NULL)
        {
            fr = *f;
            g_array_set_size(e->pila, e->pila->len - 1);
        }

        if (fr.tipo == F_BLOQUE)
        {
            int s = sig_desde(L, i + 1);
            gboolean sigue = s < L->n
                             && (es_p(&L->v[s], ";") || es_p(&L->v[s], ",") || es_p(&L->v[s], ")")
                                 || (fr.estruct && (L->v[s].clase == K_PAL || es_p(&L->v[s], "*"))));

            e->forzar_nl = !sigue;
            e->inicio_stmt = !sigue;
            e->cierra_ctrl_prev = fr.ctrl && !sigue;
        }
    }
    else if (strcmp(x, ";") == 0)
    {
        if (f == NULL || f->tipo == F_BLOQUE)
        {
            e->forzar_nl = TRUE;
            e->inicio_stmt = TRUE;
        }
    }
    else if (strcmp(x, ",") == 0)
    {
        if (f != NULL && f->explotar)
        {
            e->forzar_nl = TRUE;
        }
    }
    else if (strcmp(x, ":") == 0 && c->etiqueta > 0)
    {
        e->forzar_nl = TRUE;
        e->inicio_stmt = TRUE;
        e->tras_etiqueta = TRUE;

        if (c->etiqueta == 1 && f != NULL && f->tipo == F_BLOQUE)
        {
            f->case_extra = SANGRIA;
        }
    }
}

static GString* emitir(const Lista* L)
{
    Est e = {0};
    e.L = L;
    e.out = g_string_new(NULL);
    e.pila = g_array_new(FALSE, FALSE, sizeof(Frame));
    e.inicio_stmt = TRUE;
    e.primero = TRUE;

    for (int i = 0; i < L->n; i++)
    {
        const Tok* c = &L->v[i];

        if (c->clase == K_PP)
        {
            procesar_directiva(&e, i);
        }
        else if (es_coment(c))
        {
            procesar_comentario(&e, i);
        }
        else
        {
            procesar_codigo(&e, i);
        }
    }

    g_array_free(e.pila, TRUE);
    return e.out;
}

/* ------------------------------------------------------------------------- */
/* API                                                                       */
/* ------------------------------------------------------------------------- */

static char* normalizar_saltos(const char* s)
{
    GString* r = g_string_sized_new(strlen(s));

    for (const char* p = s; *p != '\0'; p++)
    {
        if (*p == '\r')
        {
            if (p[1] != '\n')
            {
                g_string_append_c(r, '\n');
            }
        }
        else
        {
            g_string_append_c(r, *p);
        }
    }

    return g_string_free(r, FALSE);
}

char* quark_formatear_c_interno(const char* codigo, GError** err)
{
    g_return_val_if_fail(codigo != NULL, NULL);

    char* fuente = normalizar_saltos(codigo);
    const gboolean termina_nl = g_str_has_suffix(fuente, "\n");
    Lista* L = lexer(fuente, err);
    g_free(fuente);

    if (L == NULL)
    {
        return NULL;
    }

    if (L->n == 0)
    {
        lista_liberar(L);
        return g_strdup(codigo);  /* vacío o solo espacios */
    }

    Lista* M = insertar_llaves(L, err);

    if (M == NULL)
    {
        return NULL;
    }

    anotar(M);

    GString* out = emitir(M);
    lista_liberar(M);

    while (out->len > 0 && g_ascii_isspace(out->str[out->len - 1]))
    {
        g_string_truncate(out, out->len - 1);
    }

    if (termina_nl)
    {
        g_string_append_c(out, '\n');
    }

    return g_string_free(out, FALSE);
}

/* ---- clang-format (opcional, en tiempo de ejecución) ---- */

#define ESTILO_CLANG_BASE                                                                      \
    "BasedOnStyle: LLVM, IndentWidth: 4, UseTab: Never, BreakBeforeBraces: Allman, "           \
    "AllowShortIfStatementsOnASingleLine: false, AllowShortLoopsOnASingleLine: false, "        \
    "AllowShortFunctionsOnASingleLine: None, BinPackArguments: false, "                        \
    "BinPackParameters: false, ColumnLimit: 100, PointerAlignment: Left, "                     \
    "SpaceAfterCStyleCast: true, Cpp11BracedListStyle: false, SortIncludes: false"

gboolean quark_clang_format_disponible(void)
{
    char* ruta = g_find_program_in_path("clang-format");
    gboolean si = ruta != NULL;
    g_free(ruta);
    return si;
}

static char* ejecutar_clang(const char* exe, const char* estilo, const char* codigo, GError** err)
{
    char* arg_estilo = g_strconcat("--style={", estilo, "}", NULL);
    const char* argv[] = {exe, "--assume-filename=quark.c", arg_estilo, NULL};
    GError* e = NULL;
    GSubprocess* proc = g_subprocess_newv(argv,
                                          G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE
                                              | G_SUBPROCESS_FLAGS_STDERR_PIPE,
                                          &e);
    g_free(arg_estilo);

    if (proc == NULL)
    {
        g_set_error(err, QUARK_FORMATO_ERROR, QUARK_FORMATO_ERROR_CLANG,
                    "No se pudo ejecutar clang-format: %s", e->message);
        g_clear_error(&e);
        return NULL;
    }

    char* salida = NULL;
    char* errores = NULL;

    if (!g_subprocess_communicate_utf8(proc, codigo, NULL, &salida, &errores, &e))
    {
        g_set_error(err, QUARK_FORMATO_ERROR, QUARK_FORMATO_ERROR_CLANG,
                    "Fallo al comunicarse con clang-format: %s", e->message);
        g_clear_error(&e);
        g_object_unref(proc);
        return NULL;
    }

    gboolean ok = g_subprocess_get_successful(proc);
    g_object_unref(proc);

    if (!ok)
    {
        char* primera = errores != NULL ? g_strstrip(g_strdup(errores)) : g_strdup("");
        char* salto = strchr(primera, '\n');

        if (salto != NULL)
        {
            *salto = '\0';
        }

        g_set_error(err, QUARK_FORMATO_ERROR, QUARK_FORMATO_ERROR_CLANG,
                    "clang-format terminó con error: %s", primera);
        g_free(primera);
        g_free(salida);
        g_free(errores);
        return NULL;
    }

    g_free(errores);
    return salida;
}

char* quark_formatear_c_clang(const char* codigo, GError** err)
{
    g_return_val_if_fail(codigo != NULL, NULL);

    char* exe = g_find_program_in_path("clang-format");

    if (exe == NULL)
    {
        g_set_error_literal(err, QUARK_FORMATO_ERROR, QUARK_FORMATO_ERROR_CLANG,
                            "clang-format no está instalado");
        return NULL;
    }

    /* InsertBraces (clang-format >= 15) añade las llaves obligatorias; si la versión
     * instalada no la conoce se reintenta sin ella. */
    GError* e1 = NULL;
    char* r = ejecutar_clang(exe, ESTILO_CLANG_BASE ", InsertBraces: true", codigo, &e1);

    if (r == NULL)
    {
        r = ejecutar_clang(exe, ESTILO_CLANG_BASE, codigo, err);
    }

    g_clear_error(&e1);
    g_free(exe);
    return r;
}

char* quark_formatear_c_con(const char* codigo, QuarkFormatoMotor motor,
                            gboolean* uso_alternativa, GError** err)
{
    if (uso_alternativa != NULL)
    {
        *uso_alternativa = FALSE;
    }

    if (motor == QUARK_FORMATO_AUTO)
    {
        const char* pref = g_getenv("QUARK_FORMATEADOR");
        motor = (pref != NULL && g_ascii_strcasecmp(pref, "clang-format") == 0) ? QUARK_FORMATO_CLANG
                                                                                : QUARK_FORMATO_INTERNO;
    }

    if (motor == QUARK_FORMATO_CLANG)
    {
        GError* e = NULL;
        char* r = quark_formatear_c_clang(codigo, &e);

        if (r != NULL)
        {
            return r;
        }

        g_clear_error(&e);

        if (uso_alternativa != NULL)
        {
            *uso_alternativa = TRUE;
        }
    }

    return quark_formatear_c_interno(codigo, err);
}

char* quark_formatear_c(const char* codigo, GError** err)
{
    return quark_formatear_c_con(codigo, QUARK_FORMATO_AUTO, NULL, err);
}

/* ---- correspondencia de posiciones ---- */

static gboolean car_significativo(gunichar ch)
{
    return !g_unichar_isspace(ch) && ch != '{' && ch != '}';
}

glong quark_formato_mapear_posicion(const char* original, const char* formateado, glong pos)
{
    const glong total = g_utf8_strlen(original, -1);
    pos = CLAMP(pos, 0, total);

    /* caracteres significativos antes de la posición */
    glong n = 0;
    const char* p = original;

    for (glong k = 0; k < pos; k++, p = g_utf8_next_char(p))
    {
        if (car_significativo(g_utf8_get_char(p)))
        {
            n++;
        }
    }

    /* ¿el cursor está pegado (misma línea) a un carácter significativo posterior? */
    const char* q = p;

    while (*q == ' ' || *q == '\t')
    {
        q++;
    }

    const gboolean antes_de_sig = *q != '\0' && *q != '\n' && car_significativo(g_utf8_get_char(q));

    if (n == 0 && !antes_de_sig)
    {
        return 0;
    }

    glong idx = 0;
    glong cuenta = 0;

    for (const char* r = formateado; *r != '\0'; r = g_utf8_next_char(r), idx++)
    {
        if (!car_significativo(g_utf8_get_char(r)))
        {
            continue;
        }

        if (antes_de_sig && cuenta == n)
        {
            return idx;
        }

        cuenta++;

        if (!antes_de_sig && cuenta == n)
        {
            return idx + 1;
        }
    }

    return idx;
}
