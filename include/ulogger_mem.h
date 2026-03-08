#ifndef _MEM_H_
#define _MEM_H_

/**
 * @file ulogger_mem.h
 * @brief Public memory management API for uLogger library
 *
 * This header provides the memory abstraction layer for uLogger, allowing the
 * library to work with different memory types (debug log storage, stack traces)
 * and different memory devices (flash, EEPROM, etc.). Do not modify this file.
 * 
 * Users must provide memory driver implementations and configure memory control
 * blocks to define memory regions for different log types.
 * 
 * Include this file in your application code when using uLogger memory functions.
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Memory Types
// ============================================================================

/**
 * @brief Memory type identifiers for different log storage purposes
 */
typedef enum {
  ULOGGER_MEM_TYPE_DEBUG_LOG = 0,  /**< Memory region for debug log storage */
  ULOGGER_MEM_TYPE_STACK_TRACE,    /**< Memory region for stack trace/crash dump storage */
  ULOGGER_MEM_TYPE_END             /**< Sentinel value for memory type bounds checking */
} mem_type_t;

// ============================================================================
// Memory Driver Interface
// ============================================================================

/**
 * @brief Memory driver interface structure
 *
 * User-provided memory driver implementation for accessing physical memory.
 * All function pointers must be implemented for the target memory device.
 */
typedef struct {
  bool (*read)(uint32_t address, uint8_t *data, uint32_t len);      /**< Read data from memory */
  bool (*write)(uint32_t address, const uint8_t *data, uint32_t len); /**< Write data to memory */
  bool (*erase)(uint32_t address, uint32_t len);                     /**< Erase memory region */
} mem_drv_t;

/**
 * @brief Memory control block structure
 *
 * Defines a memory region associated with a specific memory type and driver.
 * An array of these blocks is passed to Mem_init() to configure the memory
 * subsystem.
 */
typedef struct {
  mem_type_t type;         /**< Type of data stored in this memory region */
  uint32_t start_addr;     /**< Starting address of memory region */
  uint32_t end_addr;       /**< Ending address of memory region (inclusive) */
  const mem_drv_t *mem_drv; /**< Pointer to memory driver implementation */
} mem_ctl_block_t;

// ============================================================================
// Public API Functions
// ============================================================================

/**
 * @brief Initialize the memory subsystem
 * @param mcb Pointer to array of memory control blocks
 * @param len Number of memory control blocks in the array
 *
 * This function must be called once during initialization before using any
 * other memory functions. It configures the memory regions and drivers for
 * different log types.
 */
void Mem_init(const mem_ctl_block_t *mcb, uint32_t len);

/**
 * @brief Read data from memory
 * @param type Memory type to read from (debug log or stack trace)
 * @param offset Offset from start of memory region (in bytes)
 * @param buf Buffer to store read data
 * @param len Number of bytes to read
 * @return true if read succeeded, false otherwise
 */
bool Mem_read(mem_type_t type, uint32_t offset, uint8_t *buf, uint32_t len);

/**
 * @brief Write data to memory
 * @param type Memory type to write to (debug log or stack trace)
 * @param offset Offset from start of memory region (in bytes)
 * @param buf Buffer containing data to write
 * @param len Number of bytes to write
 * @return true if write succeeded, false otherwise
 */
bool Mem_write(mem_type_t type, uint32_t offset, uint8_t *buf, uint32_t len);

/**
 * @brief Erase a region of memory
 * @param type Memory type to erase (debug log or stack trace)
 * @param offset Offset from start of memory region (in bytes)
 * @param len Number of bytes to erase
 * @return true if erase succeeded, false otherwise
 *
 * @note The erase operation may be device-specific (e.g., flash requires
 *       sector/block erase). Consult your memory driver implementation.
 */
bool Mem_erase(mem_type_t type, uint32_t offset, uint32_t len);

/**
 * @brief Erase entire memory region for a given type
 * @param type Memory type to erase (debug log or stack trace)
 * @return true if erase succeeded, false otherwise
 *
 * This is a convenience function that erases the entire memory region
 * associated with the specified memory type.
 */
bool Mem_erase_all(mem_type_t type);

#ifdef __cplusplus
}
#endif

#endif  // _MEM_H_
