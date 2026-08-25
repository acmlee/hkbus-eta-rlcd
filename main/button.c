/**
 * @file button.c
 * @brief KEY button (GPIO18) driver for light-sleep operation.
 *
 * Coexistence note: Page-toggle owns GPIO18 short-press.
 * The pending sleep plan (docs/plan-display-sleep-button-wake.md) must be
 * reworked to either use long-press discrimination or a different button.
 *
 * Light-sleep interaction (plan-battery-optimizations.md Phase 1): while the
 * SoC is in light sleep the GPIO peripheral is powered down, so an
 * edge-triggered ISR would silently miss presses.  The pin is therefore
 * configured with a LOW_LEVEL trigger, which doubles as a light-sleep wakeup
 * source: a press (pin → low) wakes the SoC immediately and latches
 * s_press_pending.  One physical press-and-release counts as exactly one
 * press — the consumer's release watchdog dedupes the level-triggered ISR
 * firing continuously while the button is held.
 */

#include "button.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_sleep.h"

/* Latched by the ISR while the pin is low.  Consumed (cleared) by
 * button_consume_presses().  Written from ISR context (atomic store),
 * read/cleared from task context. */
static volatile bool s_press_pending = false;

/* Release watchdog (consumer side only — button_consume_presses runs in a
 * single task, no concurrency): once a press is counted, further ISR
 * latches are ignored until the pin is observed high again, so holding the
 * button still counts as one press. */
static bool s_waiting_release = false;

static void IRAM_ATTR button_isr(void *arg)
{
    (void)arg;
    /* Level-triggered: fires repeatedly while the button is held.  The
     * consumer's release watchdog dedupes that; here we just latch. */
    __atomic_store_n(&s_press_pending, true, __ATOMIC_SEQ_CST);
}

void button_init(void)
{
    gpio_install_isr_service(0);

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        /* LOW_LEVEL (not NEGEDGE): the same trigger doubles as the
         * light-sleep wakeup source, which only supports level wakeup. */
        .intr_type    = GPIO_INTR_LOW_LEVEL,
    };
    gpio_config(&io);
    gpio_isr_handler_add(BUTTON_GPIO, button_isr, NULL);

    /* Wake the SoC from light sleep on a button press (plan-battery-
     * optimizations.md Phase 1).  Without this, presses occurring entirely
     * inside a light-sleep window would be missed by the ISR. */
    gpio_wakeup_enable(BUTTON_GPIO, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
}

uint32_t button_consume_presses(void)
{
    uint32_t count = 0;

    if (!s_waiting_release) {
        if (__atomic_exchange_n(&s_press_pending, false, __ATOMIC_SEQ_CST)) {
            count = 1;
            s_waiting_release = true;   /* ignore further latches until release */
        }
    }

    /* Released (pin high, active-low button)?  Arm for the next press. */
    if (s_waiting_release && gpio_get_level(BUTTON_GPIO) == 1) {
        s_waiting_release = false;
    }

    return count;
}

uint32_t button_get_press_count(void)
{
    return __atomic_load_n(&s_press_pending, __ATOMIC_SEQ_CST) ? 1 : 0;
}
