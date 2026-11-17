/*
 * quark_formato.h - Formateador de código C/C++ para Quark.
 *
 * Estilo: Allman, 4 espacios, siempre llaves, sin reducir líneas.
 * Todo lo de este módulo es independiente de GTK (solo usa GLib/GIO), así que
 * se puede probar desde la línea de comandos (ver test_formato.c).
 */
#ifndef QUARK_FORMATO_H
#define QUARK_FORMATO_H

#include <glib.h>

G_BEGIN_DECLS

#define QUARK_FORMATO_ERROR (quark_formato_error_quark())

typedef enum
{
    QUARK_FORMATO_ERROR_LEXICO,  /* cadena/comentario sin cerrar */
    QUARK_FORMATO_ERROR_LLAVES,  /* llaves, paréntesis o corchetes desbalanceados */
    QUARK_FORMATO_ERROR_CLANG    /* clang-format ausente o con error */
} QuarkFormatoError;

GQuark quark_formato_error_quark(void);

typedef enum
{
    QUARK_FORMATO_AUTO,     /* interno, salvo QUARK_FORMATEADOR=clang-format */
    QUARK_FORMATO_INTERNO,  /* tokenizador propio */
    QUARK_FORMATO_CLANG     /* clang-format con el estilo embebido */
} QuarkFormatoMotor;

/* Formatea con el motor por defecto (ver QUARK_FORMATO_AUTO). Devuelve una
 * cadena nueva (g_free) o NULL con `err` relleno. */
char* quark_formatear_c(const char* codigo, GError** err);

/* Igual, eligiendo motor. Si se pidió clang-format y no está disponible (o
 * falla), se usa el interno y `*uso_alternativa` queda en TRUE (puede ser NULL). */
char* quark_formatear_c_con(const char* codigo, QuarkFormatoMotor motor,
                            gboolean* uso_alternativa, GError** err);

/* Solo el formateador propio (determinista, sin programas externos). */
char* quark_formatear_c_interno(const char* codigo, GError** err);

/* Solo clang-format (error QUARK_FORMATO_ERROR_CLANG si no está en PATH). */
char* quark_formatear_c_clang(const char* codigo, GError** err);

gboolean quark_clang_format_disponible(void);

/* Traduce una posición (en caracteres, como GtkTextIter) del texto original
 * a la posición equivalente del texto formateado. Se ancla en los caracteres
 * significativos (ni espacios ni llaves), de modo que sigue siendo válida
 * cuando el formateador añade o mueve líneas y llaves. */
glong quark_formato_mapear_posicion(const char* original, const char* formateado, glong pos);

G_END_DECLS

#endif /* QUARK_FORMATO_H */
