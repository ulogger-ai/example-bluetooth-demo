/***************************************************************************//**
 * @file
 * @brief BLE chunked transfer protocol for logs and core dumps.
 *
 * This module implements a simple chunked-transfer protocol over BLE
 * notifications.  The Python host ACKs each chunk by writing the next
 * expected offset to the log_ack characteristic, and the firmware sends
 * the next chunk in response.
 *
 * Packet layout (firmware -> host):
 *   Byte 0-1 : current offset   (uint16_t little-endian)
 *   Byte 2-3 : total length     (uint16_t little-endian)
 *   Byte 4+  : payload bytes
 *
 * ACK layout (host -> firmware):
 *   Byte 0-1 : next expected offset (uint16_t little-endian)
 ******************************************************************************/

#ifndef BLE_TRANSFER_H
#define BLE_TRANSFER_H

#include <stdint.h>
#include <stdbool.h>
#include "sl_bt_api.h"

/**************************************************************************//**
 * Called when a BLE connection is opened.  Resets transfer state.
 *
 * @param[in] connection_handle  Handle for the new connection.
 *****************************************************************************/
void ble_transfer_on_connection_opened(uint8_t connection_handle);

/**************************************************************************//**
 * Called when the BLE connection is closed.  Cancels any in-progress transfer.
 *****************************************************************************/
void ble_transfer_on_connection_closed(void);

/**************************************************************************//**
 * Called when the host writes to the log_ack characteristic.
 *
 * An ack_offset of 0 with no transfer in progress acts as a "start" trigger.
 *
 * @param[in] ack_offset  Next expected byte offset (from the host).
 *****************************************************************************/
void ble_transfer_handle_ack(uint16_t ack_offset);

/**************************************************************************//**
 * Periodic timer tick — checks for new log data and kicks stalled transfers.
 *****************************************************************************/
void ble_transfer_timer_tick(void);

#endif // BLE_TRANSFER_H
