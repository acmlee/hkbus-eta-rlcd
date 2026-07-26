#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

/* ------------------------------------------------------------------
 * Display constants
 * ----------------------------------------------------------------*/
#define DISP_WIDTH      400
#define DISP_HEIGHT     300

/* ── Layout zone Y-offsets ── */
#define ZONE_HEADER_Y       0
#define ZONE_HEADER_H       36
#define ZONE_ROW1_Y         36
#define ZONE_ROW_H          81
#define ZONE_ROW_GAP        1           /* 1px divider between rows */
#define ZONE_ROW_COUNT      3
#define ZONE_FOOTER_Y       282
#define ZONE_FOOTER_H       18

/* Column layout within each route row */
#define COL_ROUTE_X         0
#define COL_ROUTE_W         60          /* fixed width for route number */
#define COL_INFO_X          92          /* starts after route + 32px gap */
#define COL_ETA_W           120         /* fixed width for ETA group */

/* ------------------------------------------------------------------
 * Initialisation
 * ----------------------------------------------------------------*/

/**
 * @brief Initialise the ST7305 display via the u8g2_st7305 driver.
 *        SPI3_HOST, 24 MHz, pins MOSI=12/SCLK=11/CS=40/DC=5/RST=41.
 *        Includes boot flash (all-pixels-on 500 ms) and priming draw.
 */
void display_init(void);

/* ------------------------------------------------------------------
 * Render functions
 * ----------------------------------------------------------------*/

typedef struct {
    const char *route_num;
    const char *dest_zh;   /** zh-HK destination name (UTF-8 CJK) */
    const char *stop_zh;   /** zh-HK bus-stop name (UTF-8 CJK) */
    time_t eta1;           /** Raw epoch timestamp; (time_t)-1 = no data / "--" */
    time_t eta2;
    time_t eta3;
} route_data_t;

#include "u8g2_st7305.h"

void render_header(const char *time_str, const char *temp_str);
void render_divider(int y);
void render_route_row(int row_index, const char *route_num,
                      const char *dest_zh, const char *stop_zh,
                      time_t eta1, time_t eta2, time_t eta3);
void render_footer(const char *updated_str, int battery_pct);
void render_dashboard(const char *time_str, const char *temp_str,
                      const char *updated_str, int battery_pct,
                      const route_data_t routes[3]);

/**
 * @brief Flush the U8g2 buffer to the display.
 *        Called automatically by render_dashboard(), or separately
 *        for priming draws.
 */
void render_flush(void);