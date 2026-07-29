/**
 * @file display.c
 * @brief ST7305 display driver using the u8g2_st7305 component.
 *
 * Uses the official Waveshare u8g2_st7305 component which handles
 * init sequence, SPI, pixel-layout remapping (12×4-pixel-group LUT),
 * and the DRAW_TILE callback for the ST7305's non-standard addressing.
 *
 * Hardware:
 *   - SPI3_HOST, 24 MHz, CS/DC manually via GPIO
 *   - Pins: MOSI=GPIO12, SCLK=GPIO11, CS=GPIO40, DC=GPIO5, RST=GPIO41
 *   - Resolution: 400 × 300 (U8G2_R1 rotation, 1-bit monochrome)
 */

#include "display.h"
#include "fonts.h"
#include "u8g2_st7305.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "display";

/* Global u8g2_st7305 device instance — non-static for access from main.c */
u8g2_st7305_t g_lcd;

/** Convenience: get the U8g2 context from our device instance. */
static inline u8g2_t *u8g2(void)
{
    return u8g2_st7305_get_u8g2(&g_lcd);
}

/* ------------------------------------------------------------------
 * Row y-offset helper
 * ----------------------------------------------------------------*/
static inline int row_y(int idx)
{
    return ZONE_ROW1_Y + idx * (ZONE_ROW_H + ZONE_ROW_GAP);
}

/* ------------------------------------------------------------------
 * Initialisation
 * ----------------------------------------------------------------*/
void display_init(void)
{
    /* Use the default config — pins match our board exactly:
     *   MOSI=GPIO12, SCLK=GPIO11, CS=GPIO40, DC=GPIO5, RST=GPIO41
     *   SPI3_HOST, 24 MHz, U8G2_R1 rotation, full tile buffer */
    u8g2_st7305_config_t config = u8g2_st7305_default_config();

    /* Diagnostic: log GPIO values from config before init */
    ESP_LOGI(TAG, "config: mosi=%d sclk=%d dc=%d cs=%d rst=%d spi_host=%d clock=%d rotation=%p prefer_psram=%d",
             config.mosi_io, config.sclk_io, config.dc_io, config.cs_io,
             config.reset_io, config.spi_host, config.clock_hz,
             (void *)config.rotation, config.prefer_psram);

    ESP_ERROR_CHECK(u8g2_st7305_init(&g_lcd, &config));

    u8g2_t *u = u8g2();

    /* ---- Boot flash: all-pixels-on for 500 ms (PRD FR-1) ---- */
    u8g2_ClearBuffer(u);
    u8g2_SetDrawColor(u, 1);
    u8g2_DrawBox(u, 0, 0, DISP_WIDTH, DISP_HEIGHT);
    u8g2_SendBuffer(u);
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Clear to white for first real frame */
    u8g2_ClearBuffer(u);
    u8g2_SendBuffer(u);

    /* ---- Priming draw: exercise the render pipeline once ---- */
    render_flush();

    ESP_LOGI(TAG, "ST7305 display initialised via u8g2_st7305 (400×300, U8G2_R1)");
}

/* ------------------------------------------------------------------
 * Render helpers
 * ----------------------------------------------------------------*/

void render_header(const char *time_str, const char *temp_str)
{
    u8g2_t *u = u8g2();

    /* Solid black band */
    u8g2_SetDrawColor(u, 1);
    u8g2_DrawBox(u, 0, ZONE_HEADER_Y, DISP_WIDTH, ZONE_HEADER_H);

    /* "HK Bus ETA" left, white-on-black */
    u8g2_SetFont(u, u8g2_font_helvR10_tr);
    u8g2_SetDrawColor(u, 0);
    u8g2_DrawStr(u, 14, 24, "HK Bus ETA");

    /* Temperature (22px bold), 16px right of title, same baseline */
    if (temp_str) {
        /* Measure title width in its own font */
        u8g2_SetFont(u, u8g2_font_helvR10_tr);
        int w_title = u8g2_GetStrWidth(u, "HK Bus ETA");

        u8g2_SetFont(u, u8g2_font_profont22_mf);
        int w_temp = u8g2_GetUTF8Width(u, temp_str);
        int x_temp = 14 + w_title + 16;

        /* Time left edge (time is right-anchored at DISP_WIDTH - 14) */
        u8g2_SetFont(u, u8g2_font_logisoso28_tf);
        int w_time = u8g2_GetStrWidth(u, time_str);
        int x_time_left = DISP_WIDTH - 14 - w_time;

        if (x_temp + w_temp + 8 <= x_time_left) {
            u8g2_SetFont(u, u8g2_font_profont22_mf);
            u8g2_SetDrawColor(u, 0);  /* white-on-black */
            u8g2_DrawUTF8(u, x_temp, 24, temp_str);
        }
        /* If overlap would occur, skip — temperature omitted for this frame */
    }

    /* HH:MM right, white-on-black, 28px bold */
    u8g2_SetFont(u, u8g2_font_logisoso28_tf);
    int tw = u8g2_GetStrWidth(u, time_str);
    u8g2_DrawStr(u, DISP_WIDTH - 14 - tw, 32, time_str);

    u8g2_SetDrawColor(u, 1);   /* restore black */
}

void render_divider(int y)
{
    u8g2_SetDrawColor(u8g2(), 1);
    u8g2_DrawHLine(u8g2(), 0, y, DISP_WIDTH);
}

/* Available width for Col 2 text between route number and ETA columns */
#define COL2_AVAIL_W (DISP_WIDTH - COL_ETA_W - COL_INFO_X - 8)

/* Font shrink chain for destination line: step down if string overflows */
static const uint8_t *dest_font_chain[] = {
    u8g2_font_zhhk_dest_24,   /* primary ~24px CJK + ASCII */
    u8g2_font_helvB14_tr,     /* fallback 1 ~14px ASCII only */
    u8g2_font_helvB12_tr,     /* fallback 2 ~12px ASCII only */
    u8g2_font_helvB10_tr,     /* fallback 3 ~10px ASCII only */
    u8g2_font_helvB08_tr,     /* minimum ~8px ASCII only */
};
#define DEST_FONT_COUNT (sizeof(dest_font_chain) / sizeof(dest_font_chain[0]))

/* Font shrink chain for stop-name line: step down if string overflows */
static const uint8_t *stop_font_chain[] = {
    u8g2_font_zhhk_stop_20,   /* primary ~20px CJK + ASCII */
    u8g2_font_helvR10_tr,    /* fallback 1 ~10px ASCII only */
    u8g2_font_helvR08_tr,    /* fallback 2 ~8px ASCII only */
};
#define STOP_FONT_COUNT (sizeof(stop_font_chain) / sizeof(stop_font_chain[0]))

void render_route_row(int row_index, const char *route_num,
                      const char *dest_zh, const char *stop_zh,
                      time_t eta1, time_t eta2, time_t eta3)
{
    u8g2_t *u = u8g2();
    int y_off = row_y(row_index);

    /* ---- Col 1: route number (60px fixed, left-aligned, centred) ---- */
    u8g2_SetDrawColor(u, 1);
    u8g2_SetFont(u, u8g2_font_profont29_mf);
    {
        int ascent = u8g2_GetFontAscent(u);
        int descent = u8g2_GetFontDescent(u);
        int text_h = ascent - descent;
        int baseline = y_off + (ZONE_ROW_H - text_h) / 2 + ascent;
        u8g2_DrawStr(u, COL_ROUTE_X + 6, baseline, route_num);
    }

    /* ---- Col 2: destination + bus-stop (elastic, shrink-to-fit) ---- */
    int col2_x = COL_INFO_X;

    /* Pick the largest font in the shrink chain that fits */
    const uint8_t *dest_font = dest_font_chain[0];
    for (int i = 0; i < DEST_FONT_COUNT; i++) {
        u8g2_SetFont(u, dest_font_chain[i]);
        int w = u8g2_GetUTF8Width(u, dest_zh);
        if (w <= COL2_AVAIL_W) {
            dest_font = dest_font_chain[i];
            break;
        }
    }
    const uint8_t *stop_font = stop_font_chain[0];
    for (int i = 0; i < STOP_FONT_COUNT; i++) {
        u8g2_SetFont(u, stop_font_chain[i]);
        int w = u8g2_GetUTF8Width(u, stop_zh);
        if (w <= COL2_AVAIL_W) {
            stop_font = stop_font_chain[i];
            break;
        }
    }

    /* Measure destination font metrics */
    u8g2_SetFont(u, dest_font);
    int dest_ascent = u8g2_GetFontAscent(u);
    int dest_descent = u8g2_GetFontDescent(u);
    int dest_h = dest_ascent - dest_descent;

    /* Measure stop font metrics */
    u8g2_SetFont(u, stop_font);
    int stop_ascent = u8g2_GetFontAscent(u);
    int stop_descent = u8g2_GetFontDescent(u);
    int stop_h = stop_ascent - stop_descent;

    int gap = 4;
    int combined_h = dest_h + gap + stop_h;
    int block_top = y_off + (ZONE_ROW_H - combined_h) / 2;

    /* Diagnostic: log bytes and coordinates for dest/stop */
    size_t dest_len = strlen(dest_zh);
    size_t stop_len = strlen(stop_zh);
    ESP_LOGI(TAG, "dest_zh='%s' (%zu bytes) at (%d,%d) font=%s",
             dest_zh, dest_len, col2_x, block_top + dest_ascent,
             dest_font == u8g2_font_zhhk_dest_24 ? "zhhk_dest_24" :
             dest_font == u8g2_font_helvB14_tr ? "helvB14" :
             dest_font == u8g2_font_helvB12_tr ? "helvB12" :
             dest_font == u8g2_font_helvB10_tr ? "helvB10" : "helvB08");
    ESP_LOGI(TAG, "stop_zh='%s' (%zu bytes) at (%d,%d) font=%s",
             stop_zh, stop_len, col2_x, block_top + dest_h + gap + stop_ascent,
             stop_font == u8g2_font_zhhk_stop_20 ? "zhhk_stop_20" :
             stop_font == u8g2_font_helvR10_tr ? "helvR10" : "helvR08");

    /* Draw "往" prefix (stop font size) then destination */
    u8g2_SetDrawColor(u, 1);
    int dest_bl = block_top + dest_ascent;
    int stop_bl = block_top + dest_h + gap + stop_ascent;

    u8g2_SetFont(u, stop_font);
    int prefix_w = u8g2_GetUTF8Width(u, "往");
    u8g2_DrawUTF8(u, col2_x, dest_bl, "往");

    u8g2_SetFont(u, dest_font);
    u8g2_DrawUTF8(u, col2_x + prefix_w + 2, dest_bl, dest_zh);

    /* Draw bus-stop */
    u8g2_SetFont(u, stop_font);
    // log_glyph_coverage(u, "stop", stop_zh);  // kept for CJK re-entry
    u8g2_DrawUTF8(u, col2_x, stop_bl, stop_zh);

    /* ---- Col 3: ETA values (120px fixed, right-aligned) ---- */
    int eta_right_edge = DISP_WIDTH - 10;
    int eta_col_left   = DISP_WIDTH - COL_ETA_W;

    /* Compute minutes remaining from raw epoch timestamps at render time */
    int m1 = -1, m2 = -1, m3 = -1;
    time_t now;
    time(&now);
    if (eta1 != (time_t)-1) {
        double secs = difftime(eta1, now);
        m1 = (int)(secs / 60.0);    /* truncate = floor for positive values */
        if (m1 < 0) m1 = -1;
    }
    if (eta2 != (time_t)-1) {
        double secs = difftime(eta2, now);
        m2 = (int)(secs / 60.0);    /* truncate = floor for positive values */
        if (m2 < 0) m2 = -1;
    }
    if (eta3 != (time_t)-1) {
        double secs = difftime(eta3, now);
        m3 = (int)(secs / 60.0);    /* truncate = floor for positive values */
        if (m3 < 0) m3 = -1;
    }

    char e1[12], e2[12], e3[12];
    if (m1 < 0) snprintf(e1, sizeof(e1), "--");
    else        snprintf(e1, sizeof(e1), "%d", m1);
    if (m2 < 0) snprintf(e2, sizeof(e2), "--");
    else        snprintf(e2, sizeof(e2), "%d", m2);
    if (m3 < 0) snprintf(e3, sizeof(e3), "--");
    else        snprintf(e3, sizeof(e3), "%d", m3);

    /* Measure widths */
    u8g2_SetFont(u, u8g2_font_profont22_mf);
    int w3 = u8g2_GetStrWidth(u, e3);
    int w2 = u8g2_GetStrWidth(u, e2);
    u8g2_SetFont(u, u8g2_font_profont29_mf);
    int w1 = u8g2_GetStrWidth(u, e1);

    int gap_eta = 12;
    int x3 = eta_right_edge - w3;
    int x2 = x3 - gap_eta - w2;
    int x1 = x2 - gap_eta - w1;

    if (x1 < eta_col_left) {
        int overflow = eta_col_left - x1;
        x1 += overflow;
        x2 += overflow;
        x3 += overflow;
    }

    /* Draw eta1 (primary) — profont29_mf, bold */
    u8g2_SetDrawColor(u, 1);
    u8g2_SetFont(u, u8g2_font_profont29_mf);
    int e1_ascent = u8g2_GetFontAscent(u);
    int e1_descent = u8g2_GetFontDescent(u);
    int e1_h = e1_ascent - e1_descent;
    int e1_baseline = y_off + (ZONE_ROW_H - e1_h) / 2 + e1_ascent;
    u8g2_DrawStr(u, x1, e1_baseline, e1);

    /* Draw eta2, eta3 (secondary) — profont22_mf */
    u8g2_SetFont(u, u8g2_font_profont22_mf);
    int e2_ascent = u8g2_GetFontAscent(u);
    int e2_descent = u8g2_GetFontDescent(u);
    int e2_h = e2_ascent - e2_descent;
    int e2_baseline = y_off + (ZONE_ROW_H - e2_h) / 2 + e2_ascent;
    u8g2_DrawStr(u, x2, e2_baseline, e2);
    u8g2_DrawStr(u, x3, e2_baseline, e3);

    /* "min" suffix */
    u8g2_SetFont(u, u8g2_font_profont12_mf);
    int min_bl = y_off + ZONE_ROW_H - 6;
    u8g2_DrawStr(u, eta_right_edge - u8g2_GetStrWidth(u, "min"), min_bl, "min");
}

void render_footer(const char *updated_str, int battery_pct,
                   const char *page_indicator_str)
{
    u8g2_t *u = u8g2();

    /* Solid black band */
    u8g2_SetDrawColor(u, 1);
    u8g2_DrawBox(u, 0, ZONE_FOOTER_Y, DISP_WIDTH, ZONE_FOOTER_H);

    char pct_buf[16];
    if (battery_pct == 255) {
        snprintf(pct_buf, sizeof(pct_buf), "Battery:  --%%");
    } else {
        snprintf(pct_buf, sizeof(pct_buf), "Battery: %3d%%", battery_pct);
    }

    u8g2_SetFont(u, u8g2_font_profont12_mf);
    u8g2_SetDrawColor(u, 0);  /* white-on-black */

    /* Left: "Updated HH:MM:SS" */
    u8g2_DrawStr(u, 14, ZONE_FOOTER_Y + 14, updated_str);

    /* Page indicator 10 px after Updated text */
    if (page_indicator_str != NULL) {
        int w_updated = u8g2_GetStrWidth(u, updated_str);
        int x_page = 14 + w_updated + 10;
        u8g2_DrawStr(u, x_page, ZONE_FOOTER_Y + 14, page_indicator_str);
    }

    /* Right: "Battery: XX%" */
    int tw = u8g2_GetStrWidth(u, pct_buf);
    u8g2_DrawStr(u, DISP_WIDTH - 14 - tw, ZONE_FOOTER_Y + 14, pct_buf);

    u8g2_SetDrawColor(u, 1);  /* restore black */
}

/* ------------------------------------------------------------------
 * Frame send: just flush the U8g2 buffer to the display.
 * The u8g2_st7305 DRAW_TILE callback handles all SPI transfers
 * and ST7305 pixel-layout remapping (4×4 LUT, 12×4-pixel-groups).
 * ----------------------------------------------------------------*/
void render_flush(void)
{
    u8g2_SendBuffer(u8g2());
}

/* ------------------------------------------------------------------
 * Full dashboard render — always writes full buffer every cycle.
 * ----------------------------------------------------------------*/
void render_dashboard(const char *time_str, const char *temp_str,
                      const char *updated_str, int battery_pct,
                      const char *page_indicator_str,
                      const route_data_t routes[3], int route_count)
{
    u8g2_t *u = u8g2();

    /* Clear the U8g2 buffer (set to white/0) */
    u8g2_ClearBuffer(u);

    /* Draw all content */
    render_header(time_str, temp_str);

    for (int i = 0; i < route_count && i < 3; i++) {
        if (i > 0) {
            render_divider(row_y(i) - 1);
        }
        render_route_row(i, routes[i].route_num, routes[i].dest_zh,
                         routes[i].stop_zh, routes[i].eta1,
                         routes[i].eta2, routes[i].eta3);
    }

    render_footer(updated_str, battery_pct, page_indicator_str);

    /* Diagnostic */
    for (int i = 0; i < 3; i++) {
        ESP_LOGI(TAG, "Drew route %s → %s / %s, ETAs: %ld/%ld/%ld (epoch)",
                 routes[i].route_num, routes[i].dest_zh, routes[i].stop_zh,
                 (long)routes[i].eta1, (long)routes[i].eta2, (long)routes[i].eta3);
    }

    /* Send the full buffer to display */
    render_flush();
}

/* ------------------------------------------------------------------
 * DISPLAY_TEST — self-contained test with 3 sample routes
 * Compile with: idf.py -DCMAKE_C_FLAGS="-DDISPLAY_TEST=1" build
 * ----------------------------------------------------------------*/
#ifdef DISPLAY_TEST
void display_test(void)
{
    ESP_LOGI(TAG, "Running DISPLAY_TEST with 3 sample routes");

    route_data_t test_routes[3] = {
        { .route_num = "1A",  .dest_zh = "尖沙咀碼頭",
          .stop_zh  = "廣東道",  .eta1 = (time_t)-1,  .eta2 = (time_t)-1, .eta3 = (time_t)-1 },
        { .route_num = "6",   .dest_zh = "中環碼頭",
          .stop_zh  = "統一碼頭",       .eta1 = (time_t)-1,  .eta2 = (time_t)-1, .eta3 = (time_t)-1 },
        { .route_num = "296X", .dest_zh = "康城站",
          .stop_zh  = "將軍澳工業邨",   .eta1 = (time_t)-1, .eta2 = (time_t)-1, .eta3 = (time_t)-1 },
    };

    render_dashboard("14:32", "28°C", "Updated 14:32:00", 255, NULL, test_routes, 3);

    ESP_LOGI(TAG, "DISPLAY_TEST completed");
}
#endif /* DISPLAY_TEST */