/***************************************************************************//**
 * @file
 * @brief BLE chunked transfer protocol for logs and core dumps.
 *
 * Transfers uLogger binary logs and core dumps to the Python host over BLE
 * using a simple ACK-based chunked protocol.  See ble_transfer.h for the
 * packet layout description.
 ******************************************************************************/
#include "ble_transfer.h"
#include "app.h"
#include "app_timer.h"
#include "gatt_db.h"
#include "ulogger.h"
#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Protocol constants
// ---------------------------------------------------------------------------
#define BLE_CHUNK_HEADER   4u   // [offset_lo, offset_hi, total_lo, total_hi]
#define BLE_CHUNK_PAYLOAD 16u   // max data bytes per notification

// ---------------------------------------------------------------------------
// Connection state
// ---------------------------------------------------------------------------
static uint8_t active_connection_handle = 0xff;

// ---------------------------------------------------------------------------
// Log transfer state
// ---------------------------------------------------------------------------
static uint16_t transfer_offset      = 0;
static uint32_t transfer_total_len   = 0;
static bool     transfer_in_progress = false;

// ---------------------------------------------------------------------------
// Core dump transfer state (same protocol, different characteristic)
// ---------------------------------------------------------------------------
static uint16_t cd_transfer_offset      = 0;
static uint32_t cd_transfer_total_len   = 0;
static bool     cd_transfer_in_progress = false;

// One-shot timer used to kick the first core-dump chunk from a safe context
// (sending a notification directly from inside another BLE event callback
// can cause it to be silently dropped by the stack).
static app_timer_t cd_kick_timer;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void send_next_chunk(void);
static void send_next_cd_chunk(void);
static void start_log_transfer(void);

// ---------------------------------------------------------------------------
// Core-dump kick timer callback
// ---------------------------------------------------------------------------
static void cd_kick_callback(app_timer_t *handle, void *data) {
  (void)handle; (void)data;
  if (cd_transfer_in_progress) {
    send_next_cd_chunk();
  }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void ble_transfer_on_connection_opened(uint8_t connection_handle) {
  active_connection_handle = connection_handle;
  // Reset any in-progress transfer so the next timer tick starts fresh.
  transfer_in_progress     = false;
  transfer_offset          = 0;
  cd_transfer_in_progress  = false;
  cd_transfer_offset       = 0;
}

void ble_transfer_on_connection_closed(void) {
  active_connection_handle = 0xff;
  transfer_in_progress     = false;
  transfer_offset          = 0;
  cd_transfer_in_progress  = false;
  cd_transfer_offset       = 0;
}

void ble_transfer_handle_ack(uint16_t ack_offset) {
  if (!transfer_in_progress && !cd_transfer_in_progress && ack_offset == 0) {
    // Host is ready — send logs first, then core dump.
    start_log_transfer();
    if (!transfer_in_progress) {
      // No logs — check for core dump.
      cd_transfer_total_len = ulogger_get_core_dump_size();
      if (cd_transfer_total_len > 0) {
        cd_transfer_in_progress = true;
        cd_transfer_offset      = 0;
        send_next_cd_chunk();
      }
    }
  } else if (transfer_in_progress) {
    transfer_offset = ack_offset;
    send_next_chunk();
  } else if (cd_transfer_in_progress) {
    cd_transfer_offset = ack_offset;
    send_next_cd_chunk();
  }
}

void ble_transfer_timer_tick(void) {
  if (active_connection_handle == 0xff) {
    return;
  }

  if (transfer_in_progress) {
    // A log transfer is already being driven by ACKs — nothing to do.
    return;
  }

  if (cd_transfer_in_progress) {
    // Kick/retry the current core-dump chunk from the timer context where
    // the BLE stack is safe to accept a new notification.
    send_next_cd_chunk();
    return;
  }

  // Neither transfer is active — check for new data.
  ulogger_flush_pretrigger_to_nv();
  transfer_total_len = ulogger_get_nv_log_usage();
  log_local("binary log len %d", transfer_total_len);
  log_local("cd log size %d", ulogger_get_core_dump_size());
  if (transfer_total_len > 0) {
    transfer_in_progress = true;
    transfer_offset      = 0;
    send_next_chunk();
  }
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static void start_log_transfer(void) {
  ulogger_flush_pretrigger_to_nv();
  transfer_total_len   = ulogger_get_nv_log_usage();
  transfer_in_progress = (transfer_total_len > 0);
  transfer_offset      = 0;
  if (transfer_in_progress) {
    send_next_chunk();
  }
}

// ---------------------------------------------------------------------------
// Send the next log chunk at `transfer_offset`.
// ---------------------------------------------------------------------------
static void send_next_chunk(void) {
  if (!transfer_in_progress || active_connection_handle == 0xff) {
    return;
  }

  if (transfer_offset >= transfer_total_len) {
    // All data has been ACKed — transfer complete.
    transfer_in_progress = false;
    transfer_offset      = 0;
    // Check for a core dump to transfer next.
    cd_transfer_total_len = ulogger_get_core_dump_size();
    log_local("log xfer done, cd size=%lu", (unsigned long)cd_transfer_total_len);
    if (cd_transfer_total_len > 0) {
      // Schedule the first core-dump chunk via a one-shot timer so that
      // the notification is sent outside this BLE event callback context.
      // IMPORTANT: Do NOT clear logs yet — the log and core dump NV regions
      // are adjacent and clearing logs corrupts the core dump data.  Logs
      // will be cleared after the core dump transfer completes.
      cd_transfer_in_progress = true;
      cd_transfer_offset      = 0;
      app_timer_start(&cd_kick_timer, 100, cd_kick_callback, NULL, false);
    } else {
      // No core dump — safe to clear logs now.
      ulogger_clear_nv_logs();
    }
    return;
  }

  uint32_t remaining = transfer_total_len - transfer_offset;
  uint8_t  chunk_len = (remaining > BLE_CHUNK_PAYLOAD)
                        ? (uint8_t)BLE_CHUNK_PAYLOAD
                        : (uint8_t)remaining;

  uint8_t pkt[BLE_CHUNK_HEADER + BLE_CHUNK_PAYLOAD];
  pkt[0] = (uint8_t)(transfer_offset & 0xFFu);
  pkt[1] = (uint8_t)(transfer_offset >> 8u);
  pkt[2] = (uint8_t)(transfer_total_len & 0xFFu);
  pkt[3] = (uint8_t)(transfer_total_len >> 8u);

  uint32_t bytes_read = ulogger_read_nv_logs_with_header(
      pkt + BLE_CHUNK_HEADER,
      chunk_len,
      0u,
      transfer_offset);

  if (bytes_read == 0) {
    transfer_in_progress = false;
    transfer_offset      = 0;
    return;
  }

  sl_bt_gatt_server_send_notification(
      active_connection_handle,
      gattdb_log_data,
      (size_t)(BLE_CHUNK_HEADER + bytes_read),
      pkt);
}

// ---------------------------------------------------------------------------
// Send the next core-dump chunk at `cd_transfer_offset`.
// ---------------------------------------------------------------------------
static void send_next_cd_chunk(void) {
  if (!cd_transfer_in_progress || active_connection_handle == 0xff) {
    return;
  }

  if (cd_transfer_offset >= cd_transfer_total_len) {
    // Core dump transfer complete — erase both NV regions.
    cd_transfer_in_progress = false;
    cd_transfer_offset      = 0;
    ulogger_clear_nv_logs();
    Mem_erase_all(ULOGGER_MEM_TYPE_STACK_TRACE);
    log_local("cd xfer done, both regions cleared");
    return;
  }

  uint32_t remaining = cd_transfer_total_len - cd_transfer_offset;
  uint8_t  chunk_len = (remaining > BLE_CHUNK_PAYLOAD)
                        ? (uint8_t)BLE_CHUNK_PAYLOAD
                        : (uint8_t)remaining;

  uint8_t pkt[BLE_CHUNK_HEADER + BLE_CHUNK_PAYLOAD];
  pkt[0] = (uint8_t)(cd_transfer_offset & 0xFFu);
  pkt[1] = (uint8_t)(cd_transfer_offset >> 8u);
  pkt[2] = (uint8_t)(cd_transfer_total_len & 0xFFu);
  pkt[3] = (uint8_t)(cd_transfer_total_len >> 8u);

  bool ok = Mem_read(ULOGGER_MEM_TYPE_STACK_TRACE,
                     cd_transfer_offset,
                     pkt + BLE_CHUNK_HEADER,
                     chunk_len);
  if (!ok) {
    cd_transfer_in_progress = false;
    cd_transfer_offset      = 0;
    return;
  }

  sl_bt_gatt_server_send_notification(
      active_connection_handle,
      gattdb_core_dump_data,
      (size_t)(BLE_CHUNK_HEADER + chunk_len),
      pkt);
}
