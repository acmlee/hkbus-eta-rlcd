/**
 * @file button.c
 * @brief KEY button (GPIO18) driver with falling-edge ISR and atomic press counter.
 *
 * Coexistence note: Page-toggle owns GPIO18 short-press.
 * The pending sleep plan (docs/plan-display-sleep-button-wake.md) must be
 * reworked to either use long-press discrimination or a different button.
 */

#include "button.h"
#include "driver/gpio.h"
#include "esp_attr.h"

static volatile uint32_t s_press_count = 0;

static void IRAM_ATTR button_isr(void *arg)
{
    (void)arg;
    __atomic_add_fetch(&s_press_count, 1, __ATOMIC_SEQ_CST);
}

void button_init(void)
{
    gpio_install_isr_service(0);

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io);
    gpio_isr_handler_add(BUTTON_GPIO, button_isr, NULL);
}

uint32_t button_consume_presses(void)
{
    return __atomic_exchange_n(&s_press_count, 0, __ATOMIC_SEQ_CST);
}

uint32_t button_get_press_count(void)
{
    return __atomic_load_n(&s_press_count, __ATOMIC_SEQ_CST);
}