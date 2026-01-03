/**
 * @file glib_stubs.h
 * @brief Minimal GLib stubs for WASM builds
 *
 * Provides stub implementations of GLib functions used by proto2state
 * when building for WebAssembly (Emscripten) where GLib is not available.
 */

#ifndef GLIB_STUBS_H
#define GLIB_STUBS_H

#ifdef EMSCRIPTEN

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* ==================== Type Definitions ==================== */

/**
 * GLib basic types - map to standard C types for WASM compatibility
 */
typedef int gboolean; /* Boolean type (0=FALSE, 1=TRUE) */
typedef int gint;     /* Integer type (alias for int) */
typedef char gchar;   /* Character type (alias for char) */

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

/* ==================== Logging Functions ==================== */

/**
 * GLib logging macros - map to fprintf for WASM
 * All output goes to stderr with appropriate level prefixes
 * g_error() calls abort() to match GLib behavior
 */
#define g_critical(fmt, ...) \
  fprintf(stderr, "[CRITICAL] " fmt "\n", ##__VA_ARGS__)

#define g_error(fmt, ...)                                  \
  do                                                       \
    {                                                      \
      fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__); \
      abort();                                             \
    }                                                      \
  while (0)

#define g_warning(fmt, ...) \
  fprintf(stderr, "[WARNING] " fmt "\n", ##__VA_ARGS__)

#define g_message(fmt, ...) \
  fprintf(stderr, "[MESSAGE] " fmt "\n", ##__VA_ARGS__)

#define g_debug(fmt, ...) fprintf(stderr, "[DEBUG] " fmt "\n", ##__VA_ARGS__)

/* ==================== Memory Allocation ==================== */

/**
 * GLib memory allocation - map to standard C functions
 *
 * Note: Unlike real GLib, these do NOT abort on allocation failure.
 * Caller must check return values for NULL.
 */
#define g_malloc malloc                 /* Basic malloc */
#define g_malloc0(size) calloc(1, size) /* Zero-initialized malloc */
#define g_free free                     /* Free memory */
#define g_new(type, count) \
  ((type *)malloc(sizeof(type) * (count))) /* Type-safe malloc */
#define g_new0(type, count) \
  ((type *)calloc((count), sizeof(type))) /* Type-safe zero malloc */

#endif /* EMSCRIPTEN */

#endif /* GLIB_STUBS_H */
