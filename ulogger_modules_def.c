/**
 * @file ulogger_modules_def.c
 * @brief Debug module table definition for HardFault test
 *
 * This file instantiates the debug module table required by the uLogger library.
 * The static library references these symbols from binary_log.c.
 */

#include "ulogger_config.h"
#include "ulogger_debug_modules.h"

// Instantiate the debug module table in flash
ULLOGGER_DEFINE_DEBUG_MODULE_TABLE();