"""uLogger BLE demo -- connect to a uLogger device, download the binary log,
and publish it to the uLogger cloud platform via MQTT.

Install the companion library first::

    pip install ./ulogger_cloud

Then run::

    python bluetooth.py
"""

import sys
import asyncio
import logging
import struct
import threading
import time
import json
from pathlib import Path
from typing import Optional

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakBluetoothNotAvailableError
from ulogger_cloud import (
    DeviceInfo,
    LogConfig,
    MqttConfig,
    SessionStore,
    upload_log,
    publish_core_dump,
    publish_metrics,
    subscribe_log_config_loop,
)

log = logging.getLogger(__name__)
logging.basicConfig(level=logging.INFO, format="%(asctime)s.%(msecs)03d %(levelname)s %(name)s: %(message)s", datefmt="%H:%M:%S")

# ---------------------------------------------------------------------------
# Paths -- adjust if your layout differs
# ---------------------------------------------------------------------------
_SCRIPT_DIR = Path(__file__).parent
OUTPUT_BIN  = _SCRIPT_DIR / "output.bin"
# ---------------------------------------------------------------------------
# Load configuration from JSON
# ---------------------------------------------------------------------------
CONFIG_FILE = _SCRIPT_DIR / "bt_example.json"
_customer_id = 12345  # default fallback

try:
    with open(CONFIG_FILE) as f:
        config_data = json.load(f)
        _customer_id = config_data.get("customer_id", 12345)
        if _customer_id == 12345:
            log.warning("customer_id not set in config file %s", CONFIG_FILE)
            sys.exit(1)
except Exception as exc:
    log.warning("Could not load config from %s: %s. Using default customer_id.", CONFIG_FILE, exc)
# ---------------------------------------------------------------------------
# Device / characteristic configuration
# ---------------------------------------------------------------------------
DEVICE_ADDR  = ""  # optional: set to the target device's BLE address to filter the scan results
NOTIFY_UUID  = "1828e769-1420-4363-ab28-04037d9ff755"  # log_data (notify)
ACK_UUID     = "3828e769-1420-4363-ab28-04037d9ff755"  # log_ack  (write)
COREDUMP_UUID = "15b0fba5-59f9-4f25-bbd4-dd70ba04c6da" # core_dump_data (notify)
CONFIG_UUID  = "3ad8119d-8f9d-4297-9302-aa56f2283a72"  # config   (read)
LOG_CONFIG_UUID = "5828e769-1420-4363-ab28-04037d9ff755"  # log_config (write)
RSSI_UUID    = "7828e769-1420-4363-ab28-04037d9ff755"  # rssi      (read, 1 signed byte dBm)

_LOG_LEVEL_TO_BYTE = {"DEBUG": 0, "INFO": 1, "WARNING": 2, "ERROR": 3, "CRITICAL": 4}

TARGET_DEVICE_NAME = "uLogger Example"
SCAN_DURATION = 2.0  # seconds

# ---------------------------------------------------------------------------
# MQTT configuration -- passed to the ulogger_cloud library
# ---------------------------------------------------------------------------
MQTT_CFG = MqttConfig(
    broker="mqtt.ulogger.ai",
    port=8883,
    cert_file=_SCRIPT_DIR / "certificate.pem.crt",
    key_file=_SCRIPT_DIR / "private.pem.key",
    customer_id=_customer_id,
    token_timeout=15.0,
)

# ---------------------------------------------------------------------------
# Session token store persisted to ~/.ulogger/session_tokens.json
# Tokens are automatically re-fetched when a device's firmware is upgraded.
# ---------------------------------------------------------------------------
SESSION_STORE = SessionStore()


async def read_device_info(client: BleakClient) -> Optional[DeviceInfo]:
    """Read and parse the device identification payload from the config characteristic.

    Raises OSError if the read fails due to a connection-level problem (the
    caller should treat this as a lost connection and retry).
    """
    try:
        data = await client.read_gatt_char(CONFIG_UUID)
    except OSError:
        raise  # connection-level error -- let caller handle reconnect
    except Exception as exc:
        log.warning("Could not read config characteristic: %s", exc)
        return None

    if len(data) < 5:  # 4-byte app_id + at least one null
        log.warning("Config characteristic too short (%d bytes)", len(data))
        return None

    app_id = struct.unpack_from("<I", data, 0)[0]
    # Split the remaining null-terminated strings
    parts = bytes(data[4:]).split(b"\x00")
    # Strip any trailing empty entries from the final null
    while parts and parts[-1] == b"":
        parts.pop()

    def _str(idx: int) -> str:
        return parts[idx].decode("utf-8", errors="replace") if idx < len(parts) else ""

    return DeviceInfo(
        application_id = app_id,
        device_serial  = _str(0),
        device_type    = _str(1),
        git_version    = _str(2),
        git_hash       = _str(3),
    )


# ---------------------------------------------------------------------------
# Chunked-transfer reassembly
#
# Notification packet layout (firmware -> Python):
#   Byte 0-1 : current offset  (uint16_t little-endian)
#   Byte 2-3 : total length    (uint16_t little-endian)
#   Byte 4+  : payload bytes
#
# ACK packet layout (Python -> firmware):
#   Byte 0-1 : next expected offset (uint16_t little-endian)
# ---------------------------------------------------------------------------

class TransferState:
    """Tracks reassembly progress for a single chunked BLE transfer."""

    def __init__(self, label: str = ""):
        self.label = label
        self.reset()

    def reset(self):
        self.total_len        = None        # declared total length from firmware
        self.buffer           = bytearray() # reassembly buffer
        self.expected         = 0           # next expected offset
        self.complete         = False
        self.last_packet_time = time.monotonic()

    def is_done(self):
        return self.complete


class BleSession:
    """Encapsulates all mutable state for one BLE connection cycle.

    This avoids module-level globals and makes the data flow explicit.
    """

    def __init__(self):
        self.transfer    = TransferState("log")
        self.cd_transfer = TransferState("core dump")
        self.client: Optional[BleakClient] = None
        self.loop: Optional[asyncio.AbstractEventLoop] = None
        self.packet_event: Optional[asyncio.Event] = None
        self.disconnected_event: Optional[asyncio.Event] = None

    # -- ACK helper --------------------------------------------------------

    async def _send_ack(self, ack_value: bytes) -> None:
        """Write the ACK value to the firmware; errors are logged."""
        if self.client is None:
            return
        try:
            await self.client.write_gatt_char(ACK_UUID, ack_value, response=False)
        except Exception as exc:
            log.warning("ACK write failed: %s", exc)

    # -- Unified notification handler --------------------------------------

    def _make_notification_handler(self, state: TransferState):
        """Return a BLE notification callback bound to *state*.

        NOTE: On Windows the WinRT backend may call callbacks from a thread
        pool thread, so we use ``call_soon_threadsafe`` /
        ``run_coroutine_threadsafe`` for any asyncio work.
        """

        def handler(sender, data: bytearray):
            if len(data) < 4:
                log.warning("Short %s packet (%d bytes) -- ignored", state.label, len(data))
                return

            offset    = struct.unpack_from("<H", data, 0)[0]
            total_len = struct.unpack_from("<H", data, 2)[0]
            payload   = data[4:]

            # Initialise / validate total length
            if state.total_len is None:
                state.total_len = total_len
                state.buffer    = bytearray(total_len)
                log.info("%s transfer started -- total %d bytes, %d chunk(s) expected",
                         state.label.capitalize(), total_len, (total_len + 15) // 16)
            elif state.total_len != total_len:
                log.warning("%s total_len mismatch (%d vs %d) -- ignored",
                            state.label, total_len, state.total_len)
                return

            # Discard out-of-order / duplicate chunks
            if offset != state.expected:
                log.warning("%s unexpected offset %d (expected %d) -- ignored",
                            state.label, offset, state.expected)
                return

            # Copy payload into the reassembly buffer
            end = offset + len(payload)
            state.buffer[offset:end] = payload
            state.expected = end

            ack_value = struct.pack("<H", state.expected)
            state.last_packet_time = time.monotonic()
            log.info("%s RECV offset=%4d  payload=%2d B  progress=%d/%d",
                     state.label.upper(), offset, len(payload), state.expected, total_len)

            # Schedule ACK write on the event loop -- safe from any thread
            if self.loop is not None:
                asyncio.run_coroutine_threadsafe(self._send_ack(ack_value), self.loop)
                if self.packet_event is not None:
                    self.loop.call_soon_threadsafe(self.packet_event.set)

            # Check for completion
            if state.expected >= total_len:
                state.complete = True

        return handler


# Log config relay
# ---------------------------------------------------------------------------

async def _config_listener_coro(device_info: DeviceInfo, session: "BleSession") -> None:
    """MQTT log-config subscriber — stays connected and calls
    _send_config_to_device for every config message that arrives.
    Runs until cancelled (e.g. on BLE disconnect).
    """
    stop_event = threading.Event()
    loop = asyncio.get_running_loop()

    def _on_config(log_config: LogConfig) -> None:
        log.info("Cloud log-config received: level=%s flags=0x%08X modules=%s timeout=%ds",
                 log_config.log_level, log_config.module_flags,
                 log_config.log_modules, log_config.timeout_seconds)
        asyncio.run_coroutine_threadsafe(_send_config_to_device(session, log_config), loop)

    log.info("MQTT log-config listener started — topic: config/v0/%s/%s/%s",
             MQTT_CFG.customer_id, device_info.application_id, device_info.device_serial)
    try:
        await loop.run_in_executor(
            None,
            lambda: subscribe_log_config_loop(device_info, MQTT_CFG, _on_config, stop_event),
        )
    except asyncio.CancelledError:
        log.info("Log-config listener cancelled (BLE session ended).")
        raise
    except Exception as exc:
        log.warning("Log config subscription error: %s", exc)
    finally:
        stop_event.set()  # unblock the background thread if still waiting


async def _send_config_to_device(session: "BleSession", log_config: LogConfig) -> None:
    """Write the 9-byte binary config header to the log_config BLE characteristic."""
    if session.client is None or not session.client.is_connected:
        log.warning("BLE disconnected before log-config could be sent")
        return
    level_byte = _LOG_LEVEL_TO_BYTE.get(log_config.log_level.upper(), 1)
    payload = struct.pack("<BII", level_byte,
                          log_config.module_flags & 0xFFFFFFFF,
                          log_config.timeout_seconds & 0xFFFFFFFF)
    try:
        await session.client.write_gatt_char(LOG_CONFIG_UUID, payload, response=False)
        log.info("Log config sent to device: level=%s flags=0x%08X timeout=%ds",
                 log_config.log_level, log_config.module_flags, log_config.timeout_seconds)
    except Exception as exc:
        log.warning("Failed to write log config to device: %s", exc)


# ---------------------------------------------------------------------------
# Scan helpers
# ---------------------------------------------------------------------------

async def scan_for_devices():
    """Scan for nearby BLE devices.

    Returns (target_found, target_addr, target_rssi). target_rssi is the
    advertisement RSSI in dBm captured during the scan, or None if no target
    was found. BLE advertisements always carry RSSI, so this is reliable
    across backends (unlike connected-state client.read_rssi()).
    """
    log.info("Scanning for BLE devices for %.1f seconds...", SCAN_DURATION)
    try:
        devices = await BleakScanner.discover(timeout=SCAN_DURATION, return_adv=True)
    except BleakBluetoothNotAvailableError:
        log.warning("Bluetooth adapter not available. "
                    "Try toggling Bluetooth off/on in Windows Settings "
                    "or re-enabling the adapter in Device Manager.")
        return False, "", None

    if not devices:
        log.warning("No BLE devices found. Check that Bluetooth is enabled.")
        return False, "", None

    log.info("Found %d device(s):", len(devices))
    target_found = False
    target_addr = ""
    target_rssi = None
    target_label = DEVICE_ADDR if DEVICE_ADDR else TARGET_DEVICE_NAME
    for addr, (device, adv) in devices.items():
        name      = device.name or "(unknown)"
        rssi      = adv.rssi
        is_target = device.name == TARGET_DEVICE_NAME or (DEVICE_ADDR and addr.upper() == DEVICE_ADDR.upper())
        marker    = " <-- TARGET" if is_target else ""
        log.info("  %s  RSSI: %4d dBm  Name: %s%s", addr, rssi, name, marker)
        if is_target:
            target_found = True
            target_addr = addr
            target_rssi = rssi

    if not target_found:
        log.warning("Target device %r was NOT found in the scan.", target_label)
    else:
        log.info("Target device %r found, connecting...", target_label)
    return target_found, target_addr, target_rssi


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

async def _scan_until_found():
    """Keep scanning until the target device is discovered.

    Returns (addr, rssi) — RSSI is the advertisement RSSI from the scan that
    found the device, used to publish a fresh signal-strength metric per poll.
    """
    while True:
        target_found, device_addr, target_rssi = await scan_for_devices()
        if target_found:
            return device_addr, target_rssi
        log.info("Retrying scan in %.0f seconds...", SCAN_DURATION)
        await asyncio.sleep(SCAN_DURATION)


async def _connect_and_transfer(device_addr: str, scan_rssi: Optional[int] = None) -> bool:
    """Connect to *device_addr*, transfer logs and core dump, upload to cloud.

    *scan_rssi* is the advertisement RSSI (dBm) from the scan that found the
    device. When present, it's published as a `rssi` metric alongside the
    heartbeat, giving the platform a per-poll signal-strength reading.

    Returns True if the transfers completed successfully, False if the
    connection was lost before completion (caller should retry).
    """
    session = BleSession()
    session.loop = asyncio.get_running_loop()
    session.packet_event = asyncio.Event()
    session.disconnected_event = asyncio.Event()
    config_task: Optional[asyncio.Task] = None

    def _on_disconnect(client: BleakClient) -> None:
        log.warning("Device disconnected")
        if session.loop is not None:
            session.loop.call_soon_threadsafe(session.disconnected_event.set)
            if session.packet_event is not None:
                session.loop.call_soon_threadsafe(session.packet_event.set)

    try:
        async with BleakClient(
            device_addr,
            disconnected_callback=_on_disconnect,
            # Force a fresh GATT service discovery on Windows — the OS caches
            # the service/characteristic list and won't show characteristics
            # added in newer firmware without this flag.
            winrt={"use_cached_services": False},
        ) as client:
            session.client = client
            log.info("Connected: %s", client.is_connected)

            # Read device identification before starting the log transfer.
            # A successful CONFIG read is the "poll" that proves the device is
            # alive and responsive — publish a heartbeat metric on its behalf
            # so the platform sees it as active. BLE devices don't talk to MQTT
            # directly, so the gateway publishes here (avoids burning device NV
            # on a per-minute on-device heartbeat). When a scan-time RSSI is
            # available, it rides along in the same publish for free.
            device_info = await read_device_info(client)
            if device_info:
                log.info("Device info: %s", device_info)
                metrics_payload = [{"name": "heartbeat", "value": 1}]
                if scan_rssi is not None:
                    metrics_payload.append({"name": "rssi", "value": scan_rssi})
                publish_metrics(
                    device_serial=device_info.device_serial,
                    app_id=str(device_info.application_id),
                    metrics=metrics_payload,
                    cfg=MQTT_CFG,
                )
                # Start MQTT config listener immediately — runs concurrently with transfer
                config_task = asyncio.create_task(_config_listener_coro(device_info, session))
            else:
                log.warning("Device info unavailable, continuing anyway")

            # Give the BLE stack a moment to settle before writing the CCCD.
            await asyncio.sleep(0.5)

            # Retry start_notify a few times
            for attempt in range(1, 4):
                try:
                    await client.start_notify(
                        NOTIFY_UUID,
                        session._make_notification_handler(session.transfer),
                    )
                    await client.start_notify(
                        COREDUMP_UUID,
                        session._make_notification_handler(session.cd_transfer),
                    )
                    break
                except OSError as exc:
                    log.warning("start_notify attempt %d failed: %s", attempt, exc)
                    if attempt == 3:
                        log.error("Could not subscribe to notifications. "
                                  "Try removing the device from Windows Bluetooth "
                                  "settings and reconnecting.")
                        return False
                    await asyncio.sleep(1.0)

            # Send offset-0 ACK as a "ready" trigger
            log.info("Subscribed, sending start trigger to firmware...")
            await session._send_ack(struct.pack("<H", 0))

            # Wait for transfers to complete. A NO_DATA_TIMEOUT-second silence
            # is treated as "no more data coming" — this covers both the
            # "device had nothing to push" case (no packets ever arrive after
            # the start trigger) and end-of-stream (firmware stops sending
            # after the last chunk). The previous design blocked here forever
            # waiting for the first packet, which prevented the persistent
            # monitoring loop from ever starting on a device with no pending
            # logs.
            NO_DATA_TIMEOUT = 5.0
            while not (session.transfer.is_done() and session.cd_transfer.is_done()):
                if session.disconnected_event.is_set():
                    log.warning("Connection lost during transfer")
                    return False

                try:
                    await asyncio.wait_for(
                        session.packet_event.wait(), timeout=NO_DATA_TIMEOUT)
                except asyncio.TimeoutError:
                    log.info("No transfer activity for %.0fs - assuming complete",
                             NO_DATA_TIMEOUT)
                    break
                session.packet_event.clear()

                if session.disconnected_event.is_set():
                    log.warning("Connection lost during transfer")
                    return False

            if client.is_connected:
                await client.stop_notify(NOTIFY_UUID)
                await client.stop_notify(COREDUMP_UUID)

            # --- Handle core dump ---
            if session.cd_transfer.is_done():
                log.info("Core dump transfer complete, %d bytes received.",
                         len(session.cd_transfer.buffer))
                safe_addr = device_addr.replace(":", "-")
                coredump_bin = _SCRIPT_DIR / f"coredump_{safe_addr}.bin"
                coredump_bin.write_bytes(bytes(session.cd_transfer.buffer))
                log.info("Core dump saved to %s", coredump_bin)
                publish_core_dump(bytes(session.cd_transfer.buffer), MQTT_CFG)
            elif session.cd_transfer.total_len is not None:
                log.warning("Core dump transfer incomplete (%d/%d bytes)",
                            session.cd_transfer.expected, session.cd_transfer.total_len)
            else:
                log.info("No core dump present on device")

            if session.transfer.is_done():
                log.info("Transfer complete, %d bytes received.",
                         len(session.transfer.buffer))
                OUTPUT_BIN.write_bytes(bytes(session.transfer.buffer))
                log.info("Binary log saved to %s", OUTPUT_BIN)
                upload_log(device_info, session.transfer.buffer, MQTT_CFG, SESSION_STORE)
            elif session.transfer.total_len is None:
                # Firmware sent no log data after the start trigger — nothing
                # pending on the device. Not a failure; proceed to monitoring.
                log.info("No log data pending on device")
            else:
                # Transfer started but stalled before completing.
                log.warning("Transfer incomplete (%d/%s bytes received) - skipping upload",
                            session.transfer.expected, session.transfer.total_len)

            # The config listener task runs in the background and sends configs
            # to the device as they arrive.  It is cancelled in the finally block
            # when the BLE session ends.

            # --- Persistent monitoring loop ---
            # Hold the BLE connection open and publish a heartbeat every 60s.
            # RSSI comes from a dedicated GATT characteristic that the firmware
            # refreshes from its own end of the link (the firmware stops
            # advertising once connected, so scan-based RSSI doesn't work
            # mid-session). Reading the characteristic is a sub-100ms GATT op.
            HEARTBEAT_INTERVAL_SECONDS = 60.0
            log.info("Entering persistent monitoring (heartbeat every %.0fs)...",
                     HEARTBEAT_INTERVAL_SECONDS)
            while client.is_connected and not session.disconnected_event.is_set():
                try:
                    # Wait one interval, or wake immediately on disconnect.
                    await asyncio.wait_for(
                        session.disconnected_event.wait(),
                        timeout=HEARTBEAT_INTERVAL_SECONDS,
                    )
                    break  # disconnect_event fired - exit monitoring
                except asyncio.TimeoutError:
                    pass  # normal interval tick

                rssi: Optional[int] = None
                try:
                    rssi_bytes = await client.read_gatt_char(RSSI_UUID)
                    if len(rssi_bytes) >= 1:
                        rssi = struct.unpack("<b", bytes(rssi_bytes[:1]))[0]
                except Exception as exc:
                    log.warning("RSSI read failed (publishing heartbeat only): %s", exc)

                if device_info is not None:
                    metrics_payload = [{"name": "heartbeat", "value": 1}]
                    if rssi is not None:
                        metrics_payload.append({"name": "rssi", "value": rssi})
                    publish_metrics(
                        device_serial=device_info.device_serial,
                        app_id=str(device_info.application_id),
                        metrics=metrics_payload,
                        cfg=MQTT_CFG,
                    )
                    log.info("Heartbeat published (rssi=%s dBm)",
                             rssi if rssi is not None else "n/a")

            log.info("Device disconnected, ending session.")
            return True

    except (OSError, asyncio.TimeoutError) as exc:
        log.warning("Connection error: %s", exc)
        return False
    finally:
        if config_task is not None and not config_task.done():
            config_task.cancel()
        session.client = None
        session.loop = None
        session.packet_event = None


async def run():
    """Maintain a persistent BLE session with the target device.

    Each cycle: scan to locate the device, open a long-lived BLE connection
    that transfers any pending logs and then publishes a heartbeat metric
    (plus connected-state RSSI) every minute until the device disconnects.
    On disconnect, the loop scans + reconnects. Runs forever — Ctrl-C to stop.
    """
    while True:
        try:
            device_addr, scan_rssi = await _scan_until_found()
            await _connect_and_transfer(device_addr, scan_rssi)
        except Exception as exc:
            log.warning("Session failed: %s", exc)
        log.info("Reconnecting in %.0fs...", SCAN_DURATION)
        await asyncio.sleep(SCAN_DURATION)


asyncio.run(run())
