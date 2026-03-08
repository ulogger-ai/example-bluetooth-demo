/***************************************************************************//**
 * @file
 * @brief Core application logic.
 *******************************************************************************
 * # License
 * <b>Copyright 2020 Silicon Laboratories Inc. www.silabs.com</b>
 *******************************************************************************
 *
 * SPDX-License-Identifier: Zlib
 *
 * The licensor of this software is Silicon Laboratories Inc.
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 ******************************************************************************/
#include "em_common.h"
#include "app_assert.h"
#include "sl_bluetooth.h"
#include "app.h"
#include "app_timer.h"
#include "gatt_db.h"
#include "ulogger.h"
#include "git_config.h"
#include "ble_transfer.h"
#include <string.h>
#include <stdio.h>

#if APPLICATION_ID == 12345
#error "APPLICATION_ID has not been configured"
#endif

void my_timer_callback(app_timer_t *handle, void *data);
static void populate_config_characteristic(const bd_addr *address);

const void *get_stack_top(ulogger_stack_type_t stack_type);
void fault_reboot(void);
void init_flash(void);

// Timer handle for periodic transfer polling
static app_timer_t my_timer;

// The advertising set handle allocated from Bluetooth stack.
static uint8_t advertising_set_handle = 0xff;

// ============================================================================
// Memory Driver for NV Logs
// ============================================================================

extern uint8_t __StackTop;
static const uint8_t *LINKER_STACK_TOP = (uint8_t *)&__StackTop;

static const mem_drv_t nv_log_mem_driver = {
    .read = ulogger_nv_mem_read,
    .write = ulogger_nv_mem_write,
    .erase = ulogger_nv_mem_erase,
};

// Memory control blocks for ulogger
static mem_ctl_block_t ulogger_mem_ctl_blocks[] = {
    {
        .type = ULOGGER_MEM_TYPE_DEBUG_LOG,
        .start_addr = ULOGGER_LOG_NV_START_ADDRESS,
        .end_addr = ULOGGER_LOG_NV_END_ADDRESS,
        .mem_drv = &nv_log_mem_driver
    },
    {
        .type = ULOGGER_MEM_TYPE_STACK_TRACE,
        .start_addr = ULOGGER_EXCEPTION_NV_START_ADDRESS,
        .end_addr = ULOGGER_EXCEPTION_NV_END_ADDRESS,
        .mem_drv = &nv_log_mem_driver
    }
};

// ============================================================================
// ULogger Configuration
// ============================================================================

#define PRETRIG_BUF_SIZE 300
static uint8_t pretrigger_buf[PRETRIG_BUF_SIZE];

static ulogger_config_t config = {
    .fault_reboot_cb = fault_reboot,
    .stack_top_address_cb = get_stack_top,
    .flags_level = {
        .flags = 0xFFFFFFFF,  // Enable all modules initially
        .level = ULOG_ERROR,//ULOG_DEBUG,  // Log all levels
    },
    .mcb_param = ulogger_mem_ctl_blocks,
    .mcb_len = sizeof(ulogger_mem_ctl_blocks),
    .pretrigger_log_count = 5,
    .pretrigger_buffer = pretrigger_buf,
    .pretrigger_buffer_size = PRETRIG_BUF_SIZE,

    // Crash dump header metadata
    .application_id = APPLICATION_ID,
    .git_hash = GIT_HASH,
    .device_type = ULOGGER_DEVICE_TYPE,
    .device_serial = "1001",
    .version_string = GIT_VERSION,
};

char device_serial[13]; // "AABBCCDDEEFF" + null

/**************************************************************************//**
 * Application Init.
 *****************************************************************************/
SL_WEAK void app_init(void)
{
  bd_addr address;
  uint8_t address_type;

  init_flash();


  init_local_logging();
  sl_bt_system_get_identity_address(&address, &address_type);

  // You can then print this using app_log or a similar print function
  log_local("Bluetooth Address: %02X:%02X:%02X:%02X:%02X:%02X\n",
            address.addr[5], address.addr[4], address.addr[3],
            address.addr[2], address.addr[1], address.addr[0]);

  snprintf(device_serial, sizeof(device_serial), "%02X%02X%02X%02X%02X%02X",
           address.addr[5], address.addr[4], address.addr[3],
           address.addr[2], address.addr[1], address.addr[0]);
  config.device_serial = (const char *)&device_serial;
  ulogger_init(&config);

  init_button_led();
  /////////////////////////////////////////////////////////////////////////////
  // Put your additional application init code here!                         //
  // This is called once during start-up.                                    //
  /////////////////////////////////////////////////////////////////////////////
}

/**************************************************************************//**
 * Application Process Action.
 *****************************************************************************/
SL_WEAK void app_process_action(void)
{
  /////////////////////////////////////////////////////////////////////////////
  // Put your additional application code here!                              //
  // This is called infinitely.                                              //
  // Do not call blocking functions from here!                               //
  /////////////////////////////////////////////////////////////////////////////
}

/**************************************************************************//**
 * Bluetooth stack event handler.
 * This overrides the dummy weak implementation.
 *
 * @param[in] evt Event coming from the Bluetooth stack.
 *****************************************************************************/
void sl_bt_on_event(sl_bt_msg_t *evt)
{
  sl_status_t sc;

  switch (SL_BT_MSG_ID(evt->header)) {
    // -------------------------------
    // This event indicates the device has started and the radio is ready.
    // Do not call any stack command before receiving this boot event!
    case sl_bt_evt_system_boot_id:
      bd_addr address;
      uint8_t address_type;

      // Get the public Bluetooth address
      sl_bt_system_get_identity_address(&address, &address_type);

      // Populate the config characteristic with device identification
      // so the Python host can read app_id, serial, type and firmware version.
      populate_config_characteristic(&address);

      generate_init_logs_local();


      // Create an advertising set.
      sc = sl_bt_advertiser_create_set(&advertising_set_handle);
      app_assert_status(sc);
      app_timer_start(&my_timer, 2000, my_timer_callback, NULL, true);

      // Generate data for advertising
      sc = sl_bt_legacy_advertiser_generate_data(advertising_set_handle,
                                                 sl_bt_advertiser_general_discoverable);
      app_assert_status(sc);

      // Set advertising interval to 100ms.
      sc = sl_bt_advertiser_set_timing(
        advertising_set_handle,
        160, // min. adv. interval (milliseconds * 1.6)
        160, // max. adv. interval (milliseconds * 1.6)
        0,   // adv. duration
        0);  // max. num. adv. events
      app_assert_status(sc);
      // Start advertising and enable connections.
      sc = sl_bt_legacy_advertiser_start(advertising_set_handle,
                                         sl_bt_legacy_advertiser_connectable);
      app_assert_status(sc);
      break;

    // -------------------------------
    // This event indicates that a new connection was opened.
      case sl_bt_evt_connection_opened_id:
        ble_transfer_on_connection_opened(evt->data.evt_connection_opened.connection);
        break;

    // -------------------------------
    // This event indicates that a connection was closed.
    case sl_bt_evt_connection_closed_id:
      ble_transfer_on_connection_closed();
      // Generate data for advertising
      sc = sl_bt_legacy_advertiser_generate_data(advertising_set_handle,
                                                 sl_bt_advertiser_general_discoverable);
      app_assert_status(sc);

      // Restart advertising after client has disconnected.
      sc = sl_bt_legacy_advertiser_start(advertising_set_handle,
                                         sl_bt_legacy_advertiser_connectable);
      app_assert_status(sc);
      break;

    ///////////////////////////////////////////////////////////////////////////
    // Add additional event handlers here as your application requires!      //
    ///////////////////////////////////////////////////////////////////////////

    // ACK received from the Python host — delegate to the transfer module.
    case sl_bt_evt_gatt_server_attribute_value_id: {
      sl_bt_evt_gatt_server_attribute_value_t *av =
          &evt->data.evt_gatt_server_attribute_value;
      if (av->attribute == gattdb_log_ack && av->value.len >= 2) {
        uint16_t ack_offset = (uint16_t)av->value.data[0]
                            | ((uint16_t)av->value.data[1] << 8);
        ble_transfer_handle_ack(ack_offset);
      }
      break;
    }

    // -------------------------------
    // Default event handler.
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Build and write the config characteristic payload.
//
// Payload layout (mirrors what bluetooth.py expects):
//   Bytes 0-3 : APP_ID (uint32_t little-endian)
//   Bytes 4+  : null-terminated strings in order:
//               device_serial, device_type, git_version, git_hash
// ---------------------------------------------------------------------------
static void populate_config_characteristic(const bd_addr *address)
{
    // device_serial is encoded directly from the 6-byte BLE address.
    char serial[13]; // "AABBCCDDEEFF" + null
    snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X",
             address->addr[5], address->addr[4], address->addr[3],
             address->addr[2], address->addr[1], address->addr[0]);

    const char *device_type   = ULOGGER_DEVICE_TYPE;
    const char *git_version   = GIT_VERSION;
    const char *git_hash      = GIT_HASH;

    // Build the flat byte buffer: [app_id LE][serial\0][type\0][ver\0][hash\0]
    uint8_t buf[128];
    size_t  pos = 0;

    buf[pos++] = (uint8_t)( APPLICATION_ID        & 0xFFu);
    buf[pos++] = (uint8_t)((APPLICATION_ID >>  8) & 0xFFu);
    buf[pos++] = (uint8_t)((APPLICATION_ID >> 16) & 0xFFu);
    buf[pos++] = (uint8_t)((APPLICATION_ID >> 24) & 0xFFu);

    // Helper: append a null-terminated string (including the null byte)
    #define APPEND_STR(s) do { \
        size_t _n = strlen(s) + 1; \
        if (pos + _n <= sizeof(buf)) { memcpy(buf + pos, (s), _n); pos += _n; } \
    } while (0)

    APPEND_STR(serial);
    APPEND_STR(device_type);
    APPEND_STR(git_version);
    APPEND_STR(git_hash);

    #undef APPEND_STR

    sl_bt_gatt_server_write_attribute_value(gattdb_config, 0, pos, buf);
}

// Timer callback — delegates to the BLE transfer module.
void my_timer_callback(app_timer_t *handle, void *data) {
  (void)handle;
  (void)data;
  ble_transfer_timer_tick();
}

const void *get_stack_top(ulogger_stack_type_t stack_type) {
  (void) stack_type; // unused parameter
  return LINKER_STACK_TOP;
}

void fault_reboot(void) {
  NVIC_SystemReset();
}

