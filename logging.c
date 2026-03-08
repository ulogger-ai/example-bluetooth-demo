#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include "errno.h"

//! Binary logging system
#include "ulogger.h"
#include "ulogger_debug_modules.h"


const char *DEBUG_ERROR_STR[] = {
    "DEBUG",
    "INFO",
    "WARNING",
    "ERROR",
    "CRITICAL"
};

void debug_putstr(const char *buf);
void debug_flush(void);
void log_vlocal(uint32_t module, uint8_t level, const char *format, va_list args);
void debug_init_ITM(void);


void init_local_logging(void) {
  // Register local logging callback
  register_local_log_callback(log_vlocal);

  // Setup ITM data interface
  debug_init_ITM();
}


// Local variadic log function
void log_vlocal(uint32_t module, uint8_t level, const char *format, va_list args) {
  char buffer[128];

  vsnprintf(buffer, sizeof(buffer), format, args);

  if (level < ULOG_INVALID) {
    debug_putstr("(");
    debug_putstr(ulogger_debug_modules[__builtin_ctz(module)].name);
    debug_putstr(") - ");
    debug_putstr(DEBUG_ERROR_STR[level]);
    debug_putstr(" - ");
  }
  debug_putstr(buffer);
  debug_putstr("\n");
  debug_flush();
}

// Write log to console only
void log_local(const char *format, ...) {
  va_list args;
  va_start(args, format);
  log_vlocal(0, ULOG_INVALID, format, args);
  va_end(args);
}

void generate_init_logs_local(void) {
  log_local("Welcome to the uLogger Bluetooth Demo!");
  log_local("Launch the bluetooth.py script to connect to the BG22 and transfer the logs\n");
  log_local("Press the button on the dev board to trigger writing logs and trigger a core-dump");
}

void generate_test_logs(void)
{
  // Welcome
  ulogger_log(SYSTEM_MODULE, ULOG_INFO, "Log messages will be sent to both the console channel");
  ulogger_log(SYSTEM_MODULE, ULOG_INFO, "and to the uLogger log handler.\n");

  // Formatting configuration
  ulogger_log(SYSTEM_MODULE, ULOG_INFO, "You can format numbers like this %d, %d, %d", 1, 2, 3);
  ulogger_log(SYSTEM_MODULE, ULOG_INFO, "You can format strings like %s\n", "this");

  // Module & Debug level configuration
  ulogger_log(SYSTEM_MODULE, ULOG_DEBUG, "You can change the log level");
  ulogger_log(COMM_MODULE, ULOG_DEBUG, "and the log module.");
  ulogger_log(COMM_MODULE, ULOG_WARNING, "Only the logs that match the configured debug level and module");
  ulogger_log(COMM_MODULE, ULOG_DEBUG, "will be stored and sent to uLogger cloud.\n");


  // Pretrigger logging
  ulogger_log(SYSTEM_MODULE, ULOG_DEBUG, "When troubleshooting errors, you generally need to know more");
  ulogger_log(SYSTEM_MODULE, ULOG_DEBUG, "context than the just the error message itself.\n");

  ulogger_log(SYSTEM_MODULE, ULOG_DEBUG, "If you were to configure debug-level logging, you might run");
  ulogger_log(SYSTEM_MODULE, ULOG_DEBUG, "out of storage to store that many logs, or simply consume too");
  ulogger_log(SYSTEM_MODULE, ULOG_DEBUG, "bandwidth and power transferring it.\n");

  ulogger_log(SYSTEM_MODULE, ULOG_DEBUG, "This is where pretrigger logs come in. Like a car dash-cam");
  ulogger_log(SYSTEM_MODULE, ULOG_DEBUG, "Upon encountering an error condition, it will look back through");
  ulogger_log(SYSTEM_MODULE, ULOG_DEBUG, "the previous logs and captures them. The number of historical");
  ulogger_log(SYSTEM_MODULE, ULOG_DEBUG, "log messages captured is configured by the setting");
  ulogger_log(SYSTEM_MODULE, ULOG_DEBUG, "\"pretrigger_log_count\"\n");

  ulogger_log(SYSTEM_MODULE, ULOG_DEBUG, "Let's create an error-level log and observe this\n");

  ulogger_log(POWER_MODULE, ULOG_ERROR, "Super-dee-duper bad error");
}
