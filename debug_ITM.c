#include <sl_udelay.h>
#include <em_cmu.h>
#include <em_eusart.h>
#include <em_dbg.h>
#include "sl_debug_swo_config.h"


void debug_init_ITM(void) {
  if (DBG_Connected()) {
      uint32_t freq;
      uint32_t div;

      CMU_ClockEnable(cmuClock_GPIO, true);

      /* Enable Serial wire output pin */
      GPIO->TRACEROUTEPEN |= GPIO_TRACEROUTEPEN_SWVPEN;

      /* Enable output on correct pin. */
      #if defined(_GPIO_ROUTE_SWOPEN_MASK)
      // Series 0
      location = SL_DEBUG_ROUTE_LOC;
      GPIO_PinModeSet(SL_DEBUG_SWO_PORT, SL_DEBUG_SWO_PIN, gpioModePushPull, 1);
    #elif defined(_GPIO_ROUTEPEN_SWVPEN_MASK)
      // Series 1
      location = SL_DEBUG_SWV_LOC;
      GPIO_PinModeSet(SL_DEBUG_SWV_PORT, SL_DEBUG_SWV_PIN, gpioModePushPull, 1);
    #elif defined(GPIO_SWV_PORT)
      // Series 2
      // SWO location is not configurable
      GPIO_PinModeSet((GPIO_Port_TypeDef)GPIO_SWV_PORT, GPIO_SWV_PIN, gpioModePushPull, 1);
    #endif

      /* Enable trace in core debug */
      CoreDebug->DHCSR |= CoreDebug_DHCSR_C_DEBUGEN_Msk;
      CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;

      /* Enable PC and IRQ sampling output */
      DWT->CTRL = 0x400113FF;

      /* Set TPIU prescaler for the current debug clock frequency. Target frequency
         is 875 kHz so we choose a divider that gives us the closest match.
         Actual divider is TPI->ACPR + 1. */

      freq = CMU_ClockFreqGet(cmuClock_TRACECLK) + (875000 / 2);

      div  = freq / 875000;
      TPI->ACPR = div - 1;

      /* Set protocol to NRZ */
      TPI->SPPR = 2;

      /* Disable continuous formatting */
      TPI->FFCR = 0x100;

      /* Unlock ITM and output data */
      ITM->LAR = 0xC5ACCE55;
      ITM->TCR = 0x10009;

      /* ITM Channel 0 is used for UART output */
      ITM->TER |= (1UL << 0);
    }
}

/* Send debug string to console
 * NOTE: assumes null termination
 *
 * @param   buf character string
 */
void debug_putstr(const char *buf) {
    if (DBG_Connected()) {
        while(*buf) {
            ITM_SendChar(*buf++);
        }
    }
}

void debug_flush(void) {
  if (DBG_Connected()) {
    while (ITM->TCR & ITM_TCR_BUSY_Msk);
    sl_udelay_wait(50);
  }
}
