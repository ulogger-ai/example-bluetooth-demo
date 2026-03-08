#ifndef ULOGGER_H
#define ULOGGER_H

/**
 * @file ulogger.h
 * @brief Public API for uLogger library
 *
 * This header contains the public API, types, and function prototypes for uLogger.
 * Include this file in your application code. Do not modify this file.
 * 
 * For configuration settings, see ulogger_config.h
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include "ulogger_mem.h"
#include "ulogger_config.h"
#include "ulogger_debug_modules.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Debug Level Definitions
// ============================================================================

/**
 * Standard debug levels (ascending severity)
 */
enum ULOGGER_DEBUG_LEVEL {
    ULOG_DEBUG = 0,      // Detailed debug information
    ULOG_INFO,           // General information
    ULOG_WARNING,        // Warning conditions
    ULOG_ERROR,          // Error conditions
    ULOG_CRITICAL,       // Critical failures
    ULOG_INVALID = 5     // Invalid/unused level
};

// ============================================================================
// Public API Types
// ============================================================================

/**
 * @brief Configuration structure for flags and level
 */
typedef struct {
    uint32_t flags;   // Debug module flags (bitfield)
    uint8_t level;    // Minimum log level threshold
} ulogger_flags_level_t;

typedef enum {
    ULOGGER_STACK_TYPE_MSP = 0,
    ULOGGER_STACK_TYPE_PSP = 1
} ulogger_stack_type_t;

/**
 * @brief Configuration structure for uLogger system (for crash handler)
 */
typedef struct {
    void((*fault_reboot_cb)(void));  // callback to execute after a crash is captured. 
    const void*(*stack_top_address_cb)(ulogger_stack_type_t stack_type);  // Top of stack for crash dumps
    ulogger_flags_level_t flags_level; // Flags and level configuration
    const mem_ctl_block_t *mcb_param; // Memory control block array
    uint32_t mcb_len;           // Length of memory control block array
    uint16_t pretrigger_log_count; // Number of pretrigger logs to keep in buffer
    uint8_t *pretrigger_buffer;  // Pointer to pretrigger buffer (user-allocated)
    uint16_t pretrigger_buffer_size; // Size of pretrigger buffer in bytes
    
    // Crash dump header metadata
    uint32_t application_id;        // Application identifier
    const char *git_hash;           // Git commit hash (max 40 chars)
    const char *device_type;        // Device type string (max 64 chars)
    const char *device_serial;      // Device serial number (max 64 chars)
    const char *version_string;     // Firmware version string (max 64 chars)
} ulogger_config_t;

// ============================================================================
// Public API Functions
// ============================================================================

/**
 * @brief Initialize the uLogger system
 * @param config Pointer to uLogger configuration structure
 * 
 * This function must be called once during application initialization before
 * using any logging functions. It initializes the memory subsystem, pretrigger
 * buffer, and crash dump infrastructure.
 */
void ulogger_init(ulogger_config_t *config);

/**
 * @brief Main logging function
 * @param debug_module Bit mask indicating the module (0x01-0x80000000)
 * @param debug_level Log level (use ULOGGER_DEBUG_LEVEL enum values)
 * @param fmt Printf-style format string
 * @param ... Variable arguments matching the format string
 */
void ulogger_log(uint32_t debug_module, uint8_t debug_level, const char *fmt, ...);

/**
 * @brief Set the flags and level configuration
 * @param flags_level Pointer to flags/level configuration structure
 * 
 * @note Before calling this function, you must initialize the memory subsystem
 *       by calling Mem_init() with your memory control block configuration.
 *       Example:
 *       @code
 *       Mem_init(ulogger_mem_ctl_block, sizeof(ulogger_mem_ctl_block));
 *       @endcode
 */
void ulogger_set_flags_level(ulogger_flags_level_t *flags_level);

/**
 * @brief Clear all logs in non-volatile memory
 */
void ulogger_clear_nv_logs(void);

/**
 * @brief Get current non-volatile log buffer usage in bytes
 * @return Total bytes needed for transmission (13-byte header + log data), or 0 if no logs
 * 
 * @note This function returns the total size required for buffer allocation when using
 *       ulogger_read_nv_logs_with_header(). The returned size includes both the 13-byte
 *       header and the actual log data.
 */
uint32_t ulogger_get_nv_log_usage(void);

/**
 * @brief Get the size of the core dump stored in non-volatile memory
 * @return Size of the core dump in bytes, or 0 if no valid dump exists
 */
uint32_t ulogger_get_core_dump_size(void);

/**
 * @brief Read NV logs with 13-byte header prepended, with support for chunked reads
 * 
 * The complete output is a logical stream composed of a 13-byte header followed by
 * the raw NV log data. Applications that cannot hold the entire stream in RAM can
 * call this function repeatedly, advancing read_offset by the number of bytes
 * returned each time, until 0 is returned.
 * 
 * @param dest         Destination buffer to write into
 * @param max_bytes    Maximum bytes to write to dest
 * @param session_token Session identifier included in the header (only relevant when
 *                     read_offset < 13, i.e. the header is being read)
 * @param read_offset  Byte offset into the logical stream [header | log data] to start
 *                     reading from. Pass 0 on the first call.
 * @return Number of bytes written to dest, or 0 if no logs available or read_offset
 *         is at/past the end of the stream.
 * 
 * @note Call ulogger_get_nv_log_usage() to obtain the total stream length.
 */
uint32_t ulogger_read_nv_logs_with_header(void *dest, uint32_t max_bytes, uint32_t session_token, uint32_t read_offset);

/**
 * @brief Flush all pretrigger logs to NV memory
 * 
 * Call this function to ensure all buffered logs in the pretrigger buffer (RAM) are
 * written to non-volatile memory before reading or transmitting logs. This is typically
 * called before ulogger_get_nv_log_usage() and ulogger_read_nv_logs_with_header().
 * 
 * @note This function should be called before attempting to read logs for transmission
 *       to ensure all recent logs are persisted to NV memory.
 */
void ulogger_flush_pretrigger_to_nv(void);

// ============================================================================
// Application-Provided Functions
// ============================================================================

/**
 * @brief Read data from non-volatile memory
 * 
 * This function must be implemented by the user application to read data from
 * non-volatile memory (e.g., Flash, EEPROM, or persistent storage).
 * 
 * @param address Starting address in non-volatile memory to read from
 * @param data Pointer to buffer where read data will be stored
 * @param size Number of bytes to read
 * @return true on success, false on error
 */
bool ulogger_nv_mem_read(uint32_t address, uint8_t *data, uint32_t size);

/**
 * @brief Write data to non-volatile memory
 * 
 * This function must be implemented by the user application to write data to
 * non-volatile memory (e.g., Flash, EEPROM, or persistent storage).
 * 
 * @param address Starting address in non-volatile memory to write to
 * @param data Pointer to buffer containing data to write
 * @param size Number of bytes to write
 * @return true on success, false on error
 */
bool ulogger_nv_mem_write(uint32_t address, const uint8_t *data, uint32_t size);

/**
 * @brief Erase a region of non-volatile memory
 * 
 * This function must be implemented by the user application to erase a region
 * of non-volatile memory (e.g., Flash sector erase). The implementation should
 * erase all data in the specified address range, preparing it for new writes.
 * 
 * @param address Starting address of the region to erase
 * @param size Size of the region to erase in bytes
 * @return true on success, false on error
 */
bool ulogger_nv_mem_erase(uint32_t address, uint32_t size);

/**
 * @brief Register a local log callback
 * 
 * This function allows the user to register a callback function that will be
 * called whenever a log message is generated. The callback receives the format
 * string and the variable argument list, allowing the user to handle log
 * messages in a custom manner (e.g., printing to a console or storing in a
 * custom buffer).
 * 
 * @param callback Pointer to the callback function to register
 */
void register_local_log_callback(void (*callback)(uint32_t debug_module, uint8_t debug_level, const char *fmt, va_list args));

/**
 * @brief Assert failure handler — logs file/line and triggers a core dump
 *
 * This function is called by the ULOGGER_ASSERT() macro when the assertion
 * condition evaluates to false. It logs the file name and line number at
 * ULOG_CRITICAL level, then captures a full core dump.
 *
 * @param file Source file name (provided by __FILE__)
 * @param line Source line number (provided by __LINE__)
 *
 * @note This function does not return.
 */
void ulogger_assert_fail(const char *file, int line);

/**
 * @brief Assert macro that logs file/line and captures a core dump on failure
 *
 * Usage:
 * @code
 * ULOGGER_ASSERT(ptr != NULL);
 * ULOGGER_ASSERT(index < MAX_SIZE);
 * @endcode
 *
 * When the condition is false, the macro logs the assertion location and
 * triggers CrashCatcher to produce a core dump for post-mortem analysis.
 */
#define ULOGGER_ASSERT(cond) \
    do { if (!(cond)) { ulogger_assert_fail(__FILE__, __LINE__); } } while (0)

#ifdef __cplusplus
}
#endif

// Check definitions
/**
 * Calculated memory length based on address range
 */
#define ULOGGER_LOG_MEM_LEN (ULOGGER_LOG_NV_END_ADDRESS - ULOGGER_LOG_NV_START_ADDRESS + 1)

// Generate build error if NV memory size is insufficient
// Validate minimum log data length
#if ULOGGER_LOG_MEM_LEN < 200
#error "ULOGGER_LOG_MEM_LEN must be at least 200 bytes"
#endif

#endif // ULOGGER_H
