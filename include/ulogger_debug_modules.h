#pragma once
#include <stdint.h>

/**
 * @file ulogger_debug_modules.h
 * @brief Debug module table generation for uLogger
 *
 * This header generates both enum values and a flash-resident table
 * of debug modules that can be extracted from the ELF file.
 *
 * Users must define ULOGGER_DEBUG_MODULE_LIST(X) before including this header.
 */

typedef struct {
    const char *name;
    uint32_t    bit;
} ulogger_debug_module_t;

/*
User must define ULOGGER_DEBUG_MODULE_LIST(X) before including this header.
Example list:
  #define ULOGGER_DEBUG_MODULE_LIST(X) \
      X(MAIN,   0) \
      X(SENSOR, 3)
*/
#ifndef ULOGGER_DEBUG_MODULE_LIST
#error "Define ULOGGER_DEBUG_MODULE_LIST(X) before including ulogger_debug_modules.h"
#endif

// Cross-compiler attributes
#if defined(__clang__) || defined(__GNUC__)
  #define ULLOGGER_USED        __attribute__((used))
  #define ULLOGGER_SECTION(x)  __attribute__((section(x)))
#else
  #define ULLOGGER_USED
  #define ULLOGGER_SECTION(x)
#endif

// Enum mask values generated from the list
typedef enum {
#define X(name, bit)  name##_MODULE = (1u << (bit)),
  ULOGGER_DEBUG_MODULE_LIST(X)
#undef X
} ulogger_debug_module_mask_t;

// User defines these in exactly one .c file (using macro below)
extern const ulogger_debug_module_t ulogger_debug_modules[];
extern const uint32_t ulogger_debug_modules_count;

// Helper macro for stringification
#define ULLOGGER_STRINGIFY(x) #x
#define ULLOGGER_STRINGIFY_EXPAND(x) ULLOGGER_STRINGIFY(x)

/**
 * @brief Helper macro to emit the debug module table in one translation unit
 *
 * Place this macro in exactly one .c file in your application:
 * @code
 * #include "ulogger_config.h"
 * #include "ulogger_debug_modules.h"
 * 
 * ULLOGGER_DEFINE_DEBUG_MODULE_TABLE();
 * @endcode
 *
 * The table will be placed in the .ulogger.debug_modules section and can be
 * extracted from the ELF file for debugging and analysis tools.
 */
#define ULLOGGER_MODULE_ENTRY(name, bit) { ULLOGGER_STRINGIFY(name), (1u << (bit)) },

#define ULLOGGER_DEFINE_DEBUG_MODULE_TABLE()                               \
  ULLOGGER_USED ULLOGGER_SECTION(".ulogger.debug_modules")                 \
  const ulogger_debug_module_t ulogger_debug_modules[] = {                 \
    ULOGGER_DEBUG_MODULE_LIST(ULLOGGER_MODULE_ENTRY)                     \
  };                                                                       \
  ULLOGGER_USED ULLOGGER_SECTION(".ulogger.debug_modules")                 \
  const uint32_t ulogger_debug_modules_count =                              \
      (uint32_t)(sizeof(ulogger_debug_modules) / sizeof(ulogger_debug_modules[0]));
