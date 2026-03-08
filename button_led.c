#include "app_timer.h"
#include "em_gpio.h"
#include "gpiointerrupt.h"
#include "app.h"
#include "ulogger.h"
#include "ulogger_config.h"
#include "ulogger_debug_modules.h"

// ULOGGER TODO: Define which board you have BRD4184A, BRD4184B, or EK4018A
#define BRD4184A

#if defined(BRD4184A)
#define LED_PORT     gpioPortB
#define LED_PIN      0

#define BUTTON_PORT  gpioPortB
#define BUTTON_PIN   1
#define BUTTON_INTNO 1   // interrupt number = pin number for EFR32

#elif defined(BRD4184B)
#define LED_PORT     gpioPortA
#define LED_PIN      4

#define BUTTON_PORT  gpioPortB
#define BUTTON_PIN   3
#define BUTTON_INTNO 3   // interrupt number = pin number for EFR32

#elif defined(EK4018A)
#define LED_PORT     gpioPortA
#define LED_PIN      4

#define BUTTON_PORT  gpioPortC
#define BUTTON_PIN   7
#define BUTTON_INTNO 7   // interrupt number = pin number for EFR32

#else
#error "ULOGGER TODO: Define your board (BRD4184A, BRD4184B, or EK4018A)"
#endif

#define DEBOUNCE_MS  20

void led_event_callback(app_timer_t *handle, void *data);
void debounce_callback(app_timer_t *handle, void *data);
void button_gpio_callback(uint8_t intNo);
void test_func_1(void);

static app_timer_t led_timer;
static app_timer_t debounce_timer;


void init_button_led(void) {
  // Initialize LED GPIO pin as push-pull output, initially off
  GPIO_PinModeSet(LED_PORT, LED_PIN, gpioModePushPull, 0);

  // Initialize button pin as input with pull-up (button pulls low when pressed)
  GPIO_PinModeSet(BUTTON_PORT, BUTTON_PIN, gpioModeInputPull, 1);

  // Register GPIO interrupt callback and configure falling-edge trigger
  GPIOINT_Init();
  GPIOINT_CallbackRegister(BUTTON_INTNO, button_gpio_callback);
  GPIO_ExtIntConfig(BUTTON_PORT, BUTTON_PIN, BUTTON_INTNO,
                    false,  // rising edge: disabled
                    true,   // falling edge: enabled (button press)
                    true);  // enable interrupt

  app_timer_start(&led_timer, 1000, led_event_callback, NULL, true);
}

void led_event_callback(app_timer_t *handle, void *data) {
  (void)handle;
  (void)data;
  GPIO_PinOutToggle(LED_PORT, LED_PIN);
}

// Called from GPIO ISR context – start the debounce one-shot timer
void button_gpio_callback(uint8_t intNo) {
  (void)intNo;
  // (Re-)start the debounce timer; any further bounces will simply restart it
  app_timer_start(&debounce_timer, DEBOUNCE_MS, debounce_callback, NULL, false);
}

// Called DEBOUNCE_MS after the last falling edge – confirm pin is still low
void debounce_callback(app_timer_t *handle, void *data) {
  (void)handle;
  (void)data;
  if (GPIO_PinInGet(BUTTON_PORT, BUTTON_PIN) == 0) {
    ulogger_log(MAIN_MODULE, ULOG_INFO, "button event");
    generate_test_logs();
    test_func_1();
  }
}

// ---------------------------------------------------------------------------
// Deliberately trigger a HardFault to demonstrate uLogger's crash monitoring.
//
// This function recursively calls itself a few times to build up a non-trivial
// call stack, then writes to an invalid memory address (0x00000000) to cause a
// HardFault.  uLogger's fault handler captures the crash dump (registers,
// stack trace, etc.) into NV memory so it can be transferred over BLE and
// published to the uLogger cloud for analysis.
//
// In a real application you would never do this intentionally — this exists
// solely to exercise and showcase the crash monitoring pipeline.
// ---------------------------------------------------------------------------
unsigned int g_index = 0;
void test_func_1(void) {
  unsigned int i = g_index++;
  while (i++ < 3) {
      test_func_1();
  }
  // Trigger a HardFault by writing to address 0x0
  *(volatile uint32_t *)0x000000000 = 0xDEADBEEF;
}
