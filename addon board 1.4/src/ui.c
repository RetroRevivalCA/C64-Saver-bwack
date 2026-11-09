/**
 * @file ui.c
 * @brief User Interface Implementation and OLED Rendering.
 * @details Manages UI navigation, screensaver mechanics, display cycling, and 
 * editing parameters directly on the SSD1306 OLED without using floating point math.
 *
 * CHANGELOG:
 * - 2026-11-07: FW_VERSION 1.00, first release of this fork. See CHANGES.md in this folder
 *   for the full list of changes from the original firmware.
 */

#include <string.h>
#include <avr/eeprom.h>
#include <avr/pgmspace.h>
#include <util/atomic.h>

#include "ui.h"
#include "board.h"
#include "ssd1306xled.h"
#include "ina219.h" 

#define UI_COL_LEFT   0
#define UI_COL_RIGHT  64 
#define UI_ROW_TOP    0
#define UI_ROW_BOTTOM 2

/** @brief Internal cursor tracking states for the edit menus. */
typedef enum { FOCUS_PLUS=0, FOCUS_MINUS, FOCUS_SAVE, FOCUS_CANCEL } cursor_focus_t;

/** @brief Outcome of a manual/auto fault-clear request (see recover_check()). */
typedef enum {
    RECOVER_OK = 0,        ///< Safe to clear faults
    RECOVER_TOO_SOON,      ///< Fault has not been latched for RECOVER_MIN_LATCH_TICKS yet
    RECOVER_SENSOR_BAD,    ///< INA219 reading is not currently valid
    RECOVER_VOLTAGE_HIGH   ///< DC voltage is still at/above the trip point
} recover_reason_t;

// Refusal messages live in flash (PROGMEM): the ATtiny85 has only 512 bytes of SRAM.
static const char msg_refused[]  PROGMEM = "Recover refused";
static const char msg_too_soon[] PROGMEM = "Wait 2 seconds";
static const char msg_sensor[]   PROGMEM = "Sensor not OK";
static const char msg_voltage[]  PROGMEM = "Voltage too high";

// All other fixed text is also in flash. Padding is added at draw time (ui_draw_str_P), so
// the stored strings are not padded.
static const char txt_dc[]       PROGMEM = "DC";
static const char txt_ac[]       PROGMEM = "AC";
static const char txt_empty[]    PROGMEM = "";
static const char txt_sensor[]   PROGMEM = "SENSOR";
static const char txt_lowv[]     PROGMEM = "LOW V";
static const char txt_hold[]     PROGMEM = "HOLD";
static const char txt_auto[]     PROGMEM = "AUTO";
static const char txt_hw[]       PROGMEM = "5.5HW";
static const char txt_menu_sym[] PROGMEM = "+-SC";   // edit-menu items: plus, minus, save, cancel
static const char ttl_oc[]       PROGMEM = "DC Current Limit";
static const char ttl_recover[]  PROGMEM = "Fault Recovery";
static const char ttl_vtrip[]    PROGMEM = "DC Volt Trip";
static const char ttl_uvtrip[]   PROGMEM = "DC Under Volt";
static const char ttl_dccal[]    PROGMEM = "DC Calibration";
static const char ttl_accal[]    PROGMEM = "AC Calibration";
static const char ttl_saver[]    PROGMEM = "Screen Saver";
/** @brief Screen-saver timeout labels, 6 bytes each, indexed by saver_temp (0..3). */
static const char saver_labels[4][6] PROGMEM = { "1m", "5m", "10m", "DIS" };

static bool menu_labels_drawn = false;

/**
 * @brief Copies a flash string into RAM, space-padded/truncated to exactly `width` characters.
 * @param dst   Destination, at least width + 1 bytes (a NUL terminator is added).
 * @param s_P   Source string in flash.
 * @param width Characters to produce.
 */
static void fill_line_P(char *dst, const char *s_P, uint8_t width) {
    uint8_t i = 0;
    char c;
    while (i < width && (c = (char)pgm_read_byte(s_P + i)) != '\0') {
        dst[i++] = c;
    }
    while (i < width) dst[i++] = ' ';
    dst[i] = '\0';
}

void ui_draw_str_P(uint8_t x, uint8_t row, const char *s_P, uint8_t width) {
    char line[17];
    if (width > 16) width = 16; // Never overrun the buffer
    fill_line_P(line, s_P, width);
    ssd1306_string_font8x16xy(x, row, line);
}

/**
 * @brief Flushes the OLED display and resets draw flags.
 */
static void ui_clear_screen(void) {
    ssd1306_clear();
    menu_labels_drawn = false;
}

/**
 * @brief Advanced button debouncer tracking short and long presses.
 * @param is_pressed Raw digital state of the button pin.
 * @return BUTTON_IDLE, BUTTON_SHORT_PRESS, or BUTTON_LONG_PRESS.
 */
static button_press_t read_button_debounced(bool is_pressed) {
    static uint8_t history = 0;
    static uint16_t press_duration = 0; 
    static bool longpress_triggered = false;
    
    // Shift history state
    history = (history << 1) | (is_pressed ? 1 : 0);
    
    if ((history & 0x07) == 0x07) { // Pin held logic
        press_duration++;
        
        if (press_duration == 50) { 
            longpress_triggered = true;
            return BUTTON_LONG_PRESS;
        } 
        else if (press_duration > 50 && ((press_duration - 50) % 40) == 0) {
            return BUTTON_LONG_PRESS; // Auto-repeat firing
        }
    } 
    else if ((history & 0x07) == 0x00) { // Pin released logic
        if (press_duration > 0 && !longpress_triggered) {
            press_duration = 0;
            return BUTTON_SHORT_PRESS; 
        }
        press_duration = 0;
        longpress_triggered = false;
    }
    
    return BUTTON_IDLE;
}

/**
 * @brief Fast integer-to-string formatter with decimal placement, avoiding heavy float libs.
 * @param value Raw integer value to convert.
 * @param buf Destination character buffer.
 * @param maxlen Buffer safety length.
 * @param total_digits Total string length required.
 * @param decimal_pos Number of digits to pad after the decimal dot.
 * @param unit Measurement unit character (e.g. 'V', 'A'), or 0 for none.
 */
static void formatNumber(uint16_t value, char *buf, uint8_t maxlen, uint8_t total_digits, uint8_t decimal_pos, char unit) {
    if (total_digits > 6) total_digits = 6; // Prevent buffer overflow

    char temp[6]; 
    for (int i = total_digits - 1; i >= 0; i--) {
        temp[i] = (value % 10) + '0';
        value /= 10;
    }
    // Strip leading zeros
    for (int i = 0; i < total_digits - decimal_pos - 1; i++) {
        if (temp[i] == '0') temp[i] = ' ';
        else break;
    }
    
    uint8_t idx = 0;
    for (int i = 0; i < total_digits; i++) {
        if (i == total_digits - decimal_pos && decimal_pos > 0) buf[idx++] = '.';
        buf[idx++] = temp[i];
    }
    if (unit && idx < maxlen - 1) buf[idx++] = unit;
    
    // Pad trailing spaces
    while (idx < maxlen - 1) {
        buf[idx++] = ' ';
    }
    buf[idx] = '\0';
}

/**
 * @brief Integer square root of a 32-bit value (result fits 16 bits), bitwise method.
 */
static uint16_t isqrt32(uint32_t n) {
    uint32_t res = 0, bit = 1UL << 30;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= res + bit) { n -= res + bit; res = (res >> 1) + bit; }
        else res >>= 1;
        bit >>= 2;
    }
    return (uint16_t)res;
}

/**
 * @brief Renders the primary dashboard data loop depending on current display cycle.
 */
static void draw_dashboard(isr_data_t *safe_data, uint16_t dcvoltage, uint16_t dccurrent, display_cycle_t cycle) {
    char buf[8]; 

    if (cycle == UI_SHOW_VAC) {
        formatNumber((safe_data->vac_peak * conf.ac_cal) / 10, buf, 8, 4, 2, 'V'); 
        ssd1306_string_font8x16xy(32, UI_ROW_BOTTOM, buf);
        
    } else if (cycle == UI_SHOW_ACCUR) {
        // True RMS current. accur_var is the variance of the ADC samples in counts squared.
        // Remove the sensor's noise variance, take the square root of 16x the variance (so
        // the result is RMS in quarter-counts), and scale: one count is 0.0264 A (ACS712-5A,
        // 185 mV/A on a 5 V, 10-bit ADC), so amps x100 = rms4 * 33 / 50. ACCUR_SCALE_NUM (board.h) is that 33, trimmed against a true-RMS meter.
        uint16_t v = safe_data->accur_var;
        v = (v > ACCUR_NOISE_VAR) ? (v - ACCUR_NOISE_VAR) : 0;
        uint16_t rms4 = isqrt32((uint32_t)v << 4);
        uint16_t amps_x100 = (rms4 * ACCUR_SCALE_NUM) / 50;
        // Light smoothing: amps_f holds 4x the running average (f = f - f/4 + new), so the
        // display value is amps_f >> 2. This settles exactly on the input and decays to 0.00.
        static uint16_t amps_f = 0;
        amps_f = (uint16_t)(amps_f - (amps_f >> 2) + amps_x100);
        amps_x100 = amps_f >> 2;
        formatNumber(amps_x100, buf, 6, 3, 2, 'A'); 
        ssd1306_string_font8x16xy(88, UI_ROW_BOTTOM, buf);
        
    } else if (cycle == UI_SHOW_VDC) {
        static uint8_t v_flash = 0;
        v_flash++;
        
        // Blink logic for faults and warnings
        if ((system_faults.flags.overvoltage || dcvoltage > (conf.vtrip * 100)) && v_flash > 10) {
            ui_draw_str_P(32, UI_ROW_TOP, txt_empty, 7); 
        } 
        else if (system_faults.flags.sensor && v_flash > 10) {
            ui_draw_str_P(32, UI_ROW_TOP, txt_sensor, 7); // INA219 fault (voltage reading is masked)
        }
        else if (dcvoltage > LIMIT_UV_MINIMUM && dcvoltage < (conf.uvtrip * 100) && v_flash > 10) {
            ui_draw_str_P(32, UI_ROW_TOP, txt_lowv, 7); // Brownout warning
        } 
        else {
            formatNumber(dcvoltage / 10, buf, 8, 4, 2, 'V'); 
            ssd1306_string_font8x16xy(32, UI_ROW_TOP, buf);
        }
        
        if (v_flash > 20) v_flash = 0;
        
    } else if (cycle == UI_SHOW_DCCUR) {
        static uint8_t c_flash = 0;
        if (system_faults.flags.overcurrent && ++c_flash > 10) {
            ui_draw_str_P(88, UI_ROW_TOP, txt_empty, 5); 
        } else {
            uint16_t disp_cur = system_faults.flags.overcurrent ? recorded_overcurrent : dccurrent;
            formatNumber(disp_cur / 10, buf, 6, 3, 2, 'A'); 
            ssd1306_string_font8x16xy(88, UI_ROW_TOP, buf);
        }
        if (c_flash > 12) c_flash = 0;
    }
}

/**
 * @brief Decides whether latched faults may be cleared right now.
 * @details Shared by the manual (long-press) and auto-recover paths. A clear is only
 * accepted when (a) nothing is latched, or (b) the fault has been latched long enough to
 * filter glitches, the INA219 is currently readable, and the DC voltage is below the trip
 * point. The latch flags are deliberately NOT part of the test, otherwise a HOLD-mode
 * fault could never be cleared.
 * @param dcvoltage Current DC voltage in mV (already masked to 0 when the sensor is invalid).
 * @param latched_ticks Number of ~11 ms ticks the current fault set has been latched.
 * @return RECOVER_OK, or the reason the request is refused.
 */
static recover_reason_t recover_check(uint16_t dcvoltage, uint16_t latched_ticks) {
    if (system_faults.all == 0)                    return RECOVER_OK;
    if (latched_ticks < RECOVER_MIN_LATCH_TICKS)   return RECOVER_TOO_SOON;
    if (!sensor_valid)                             return RECOVER_SENSOR_BAD;
    if (dcvoltage >= (uint16_t)(conf.vtrip * 100)) return RECOVER_VOLTAGE_HIGH;
    return RECOVER_OK;
}

/**
 * @brief Renders an individual parameter edit menu on screen.
 * @details Uses one 16-character line buffer for both lines (stack is scarce). The bottom
 * line is the value in columns 0-4 and the "+ - S C" action bar from column 6, with square
 * brackets around the focused item, e.g. focus on minus gives "+ [-] S C".
 * @param title_P Parameter name, in flash (PROGMEM).
 * @param val_str Formatted value (RAM string, at most 5 characters are shown).
 * @param focus   The currently highlighted action bracket (e.g. [+]).
 */
static void draw_edit_menu(const char *title_P, const char *val_str, cursor_focus_t focus) {
    char line[17];

    fill_line_P(line, title_P, 16);
    ssd1306_string_font8x16xy(0, UI_ROW_TOP, line);

    fill_line_P(line, txt_empty, 16); // 16 spaces
    for (uint8_t i = 0; val_str[i] != '\0' && i < 5; i++) {
        line[i] = val_str[i];
    }
    uint8_t p = 6; // Action bar occupies columns 6..14
    for (uint8_t k = 0; k < 4; k++) {
        if (k) line[p++] = ' ';
        if (k == (uint8_t)focus) line[p++] = '[';
        line[p++] = (char)pgm_read_byte(txt_menu_sym + k);
        if (k == (uint8_t)focus) line[p++] = ']';
    }
    ssd1306_string_font8x16xy(0, UI_ROW_BOTTOM, line);
}

void handle_ui_update(uint16_t dcvoltage, uint16_t dccurrent) {
    static screen_state_t ui_state = MENUSCREEN;
    static display_cycle_t ui_display_cycle = UI_SHOW_VAC;
    
    // Staging variables for parameter editing menus
    static uint8_t oc_temp = 0, hold_temp = 0, ov_temp = 0, uv_temp = 0, saver_temp = 0;
    static uint16_t cal_temp = 0, ac_cal_temp = 0; 
    static cursor_focus_t current_focus = FOCUS_PLUS;
    
    static uint8_t ui_refresh_divider = 0;
    static uint16_t screensaver_ticks = 0;
    static bool screen_is_off = false;
    static bool require_release = false;

    // Fault-recovery bookkeeping. These MUST live at function scope: they are shared by the
    // overcurrent auto-recover logic and the RECOVER state below.
    static uint8_t  oc_retries = 0;            // AUTO-mode overcurrent retries used since last healthy period
    static uint16_t healthy_cnt = 0;           // Ticks spent connected, fault-free and below the current limit
    static uint16_t auto_recover_cnt = 0;      // Ticks the current overcurrent fault has been latched (AUTO retry timer)
    static uint16_t latched_ticks = 0;         // Ticks any fault has been continuously latched
    static uint16_t sensor_ok_ticks = 0;       // Consecutive ticks with a valid INA219 reading
    static uint8_t  recover_press_events = 0;  // Long-press events seen during the current button hold
    static uint8_t  recover_msg_ticks = 0;     // Remaining display time of the "recover refused" message
    static recover_reason_t recover_reason = RECOVER_OK;

    // Fetch screensaver initial state from EEPROM if needed
    static uint8_t active_saver_setting = 255; 
    if (active_saver_setting == 255) {
        active_saver_setting = eeprom_read_byte(&EEScreensaver);
        if (active_saver_setting > 3) active_saver_setting = 1; 
    }

    // Safely snapshot ADC ISR data explicitly (Prevent tearing)
    isr_data_t safe_data;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        safe_data.vac_peak    = sensor_data.vac_peak;
        safe_data.accur_var   = sensor_data.accur_var;
        safe_data.button_down = sensor_data.button_down;
    }

    button_press_t btn = read_button_debounced(safe_data.button_down);
    char buf[8]; // Longest value shown is 5 characters (+ NUL); formatNumber pads to buf size - 1

    // Menu exit safety lock: requires letting go of the button
    if (!safe_data.button_down) require_release = false;
    if (require_release) btn = BUTTON_IDLE;
    
    // ---- Per-tick bookkeeping (runs every tick, about 11 ms; independent of the redraw throttle) ----
    if (system_faults.all != 0) { if (latched_ticks < 0xFFFF) latched_ticks++; }
    else                        { latched_ticks = 0; }

    if (sensor_valid) { if (sensor_ok_ticks < 0xFFFF) sensor_ok_ticks++; }
    else              { sensor_ok_ticks = 0; }

    if (!safe_data.button_down) recover_press_events = 0; // New hold starts from zero
    if (recover_msg_ticks > 0)  recover_msg_ticks--;

    // Refill the AUTO-mode overcurrent retry budget only after a sustained healthy period.
    // "Healthy" is measured while the relay is closed (no fault latched); measuring it while
    // latched would be meaningless because the load is disconnected and reads 0 mA.
    if (system_faults.all == 0 && dccurrent < (uint16_t)(conf.oc * 100)) {
        if (++healthy_cnt >= OC_HEALTHY_TICKS) { oc_retries = 0; healthy_cnt = 0; }
    } else {
        healthy_cnt = 0;
    }


    // Timeout mappings
    uint16_t saver_limit = 30000; 
    if (active_saver_setting == 0) saver_limit = 6000;       
    else if (active_saver_setting == 1) saver_limit = 30000; 
    else if (active_saver_setting == 2) saver_limit = 60000; 

    // Screensaver wake-up logic
    if (btn != BUTTON_IDLE || system_faults.all != 0) {
        screensaver_ticks = 0;
        if (screen_is_off) {
            ui_clear_screen();
            screen_is_off = false;
            btn = BUTTON_IDLE; 
            require_release = true; 
        }
    } else {
        if (active_saver_setting != 3) { 
            if (screensaver_ticks < saver_limit) screensaver_ticks++;
            else {
                if (!screen_is_off) { ui_clear_screen(); screen_is_off = true; }
            }
        } else {
            screensaver_ticks = 0;
        }
    }

    // Overcurrent auto-recover (AUTO mode only): retry after ~5 s, at most OC_AUTO_RETRY_MAX times.
    // Once the budget is spent the fault stays latched until a manual recover (which refills it)
    // or until OC_HEALTHY_TICKS of fault-free operation, so a dead short is not hammered forever.
    if (system_faults.flags.overcurrent && !system_faults.flags.overvoltage) {
        if (!conf.hold && oc_retries < OC_AUTO_RETRY_MAX) { 
            if (++auto_recover_cnt > 500) {
                auto_recover_cnt = 0;
                oc_retries++;
                system_faults.flags.overcurrent = 0; // Relay re-closes on the next main tick
            }
        }
    } else {
        auto_recover_cnt = 0; // Timer only runs while an overcurrent fault is latched
    }

    // Sensor-fault auto-recover, in BOTH HOLD and AUTO modes: needs a sustained run of valid
    // readings (SENSOR_RECOVER_TICKS). The INA219 is powered from the DC rail, so any loss of
    // that rail (e.g. a supply power-cycle) looks like a dead sensor; requiring a button press
    // to recover from that would be a nuisance. This is safe because the relay stays open for the
    // whole outage, and the overvoltage/overcurrent checks run again as soon as readings are
    // valid. Overvoltage and overcurrent faults still follow the HOLD/AUTO setting.
    if (system_faults.flags.sensor && sensor_ok_ticks >= SENSOR_RECOVER_TICKS) {
        system_faults.flags.sensor = 0;
    }
    
    // Overvoltage auto-recover (AUTO mode only). Requires a VALID sensor, a plausible (>1 V)
    // reading so an open relay on a load-side sensor is not mistaken for "voltage is fine",
    // a 0.1 V hysteresis below the trip point, and the minimum latch time.
    if (system_faults.flags.overvoltage && sensor_valid && !conf.hold &&
        dcvoltage > LIMIT_UV_MINIMUM && dcvoltage < ((conf.vtrip * 100) - 100) &&
        latched_ticks >= RECOVER_MIN_LATCH_TICKS) {
        ui_state = RECOVER;
    }

    bool force_redraw = (btn != BUTTON_IDLE || system_faults.all != 0);

    // Render Throttle (Every ~20 ticks unless action taken)
    if (++ui_refresh_divider >= 20 || force_redraw) {
        ui_refresh_divider = 0;

        // Draw active screensaver bouncing text
        if (screen_is_off) {
            static uint8_t ss_jump_counter = 0;
            static uint8_t ss_pos_idx = 0;
            
            if (++ss_jump_counter >= 25) { 
                ss_jump_counter = 0;
                ss_pos_idx = (ss_pos_idx + 1) % 3; 
                ui_clear_screen(); 
            }
            
            char ss_buf[12];
            // Bouncing x positions 0, 28, 56 (computed instead of stored in a RAM array)
            
            ss_buf[0] = 'D'; ss_buf[1] = 'C'; ss_buf[2] = ' ';
            formatNumber(dcvoltage / 10, ss_buf + 3, 7, 4, 2, 'V');
            ssd1306_string_font8x16xy(ss_pos_idx * 28, UI_ROW_TOP, ss_buf);
            
            uint16_t safe_vac;
            ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
                safe_vac = sensor_data.vac_peak;
            }
            ss_buf[0] = 'A'; ss_buf[1] = 'C'; ss_buf[2] = ' ';
            formatNumber((safe_vac * conf.ac_cal) / 10, ss_buf + 3, 7, 4, 2, 'V');
            ssd1306_string_font8x16xy(((ss_pos_idx + 1) % 3) * 28, UI_ROW_BOTTOM, ss_buf);
            
            // Cycle through the four dashboard items (VAC, AC current, VDC, DC current)
            ui_display_cycle = (ui_display_cycle + 1) % 4; 
            return; 
        }

        // Main State Machine Router
        switch(ui_state) {
            case MENUSCREEN:
                if (!menu_labels_drawn || ui_display_cycle == UI_SHOW_VAC) { // also redrawn every cycle to repair display glitches
                    ui_draw_str_P(0, UI_ROW_TOP, txt_dc, 4); 
                    ui_draw_str_P(0, UI_ROW_BOTTOM, txt_ac, 4); 
                    menu_labels_drawn = true;
                }
            
                draw_dashboard(&safe_data, dcvoltage, dccurrent, ui_display_cycle);
                
                // Route user interactions
                if (btn == BUTTON_SHORT_PRESS) { 
                    ui_state = EDIT_DC_OVERCURRENT; 
                    current_focus = FOCUS_PLUS;
                    oc_temp = conf.oc; hold_temp = conf.hold; ov_temp = conf.vtrip; 
                    uv_temp = conf.uvtrip; cal_temp = conf.cal; ac_cal_temp = conf.ac_cal;
                    require_release = true;
                    ui_clear_screen(); 
                } else if (btn == BUTTON_LONG_PRESS) {
                    // The button shares the VAC sense pin, so a brief AC sag can mimic a press.
                    // While faults are latched require a sustained hold (RECOVER_PRESS_EVENTS
                    // consecutive long-press events) before even attempting a manual clear.
                    if (system_faults.all == 0) {
                        ui_state = RECOVER; // Nothing latched: harmless
                    } else if (++recover_press_events >= RECOVER_PRESS_EVENTS) {
                        recover_press_events = 0;
                        ui_state = RECOVER;
                    }
                }
                break;

            case EDIT_DC_OVERCURRENT:
                if (btn == BUTTON_SHORT_PRESS) {
                    current_focus = (current_focus == FOCUS_CANCEL) ? FOCUS_PLUS : current_focus + 1;
                } else if (btn == BUTTON_LONG_PRESS) {
                    if (current_focus == FOCUS_PLUS) oc_temp = (oc_temp >= LIMIT_OC_MAX) ? LIMIT_OC_MAX : oc_temp + 1;
                    else if (current_focus == FOCUS_MINUS) oc_temp = (oc_temp <= LIMIT_OC_MIN) ? LIMIT_OC_MIN : oc_temp - 1;
                    else if (current_focus == FOCUS_SAVE) { 
                        conf.oc = oc_temp; eeprom_update_byte(&EEOvercurrent, conf.oc); 
                        ui_state = EDIT_OVERVOLTAGE_HOLD; current_focus = FOCUS_PLUS; require_release = true; ui_clear_screen(); 
                    }
                    else if (current_focus == FOCUS_CANCEL) { ui_state = MENUSCREEN; require_release = true; ui_clear_screen(); }
                }
                if (ui_state == EDIT_DC_OVERCURRENT) {
                    formatNumber(oc_temp, buf, sizeof(buf), 2, 1, 'A');
                    draw_edit_menu(ttl_oc, buf, current_focus);
                }
                break;

            case EDIT_OVERVOLTAGE_HOLD:
                if (btn == BUTTON_SHORT_PRESS) {
                    current_focus = (current_focus == FOCUS_CANCEL) ? FOCUS_PLUS : current_focus + 1;
                } else if (btn == BUTTON_LONG_PRESS) {
                    if (current_focus == FOCUS_PLUS || current_focus == FOCUS_MINUS) hold_temp = hold_temp ? 0 : 1;
                    else if (current_focus == FOCUS_SAVE) { 
                        conf.hold = hold_temp; eeprom_update_byte(&EEHold, conf.hold); 
                        ui_state = EDIT_DC_VOLT_TRIPPOINT; current_focus = FOCUS_PLUS; require_release = true; ui_clear_screen(); 
                    }
                    else if (current_focus == FOCUS_CANCEL) { ui_state = MENUSCREEN; require_release = true; ui_clear_screen(); }
                }
                if (ui_state == EDIT_OVERVOLTAGE_HOLD) {
                    strcpy_P(buf, hold_temp ? txt_hold : txt_auto);
                    draw_edit_menu(ttl_recover, buf, current_focus); // Applies to all fault types
                }
                break;

            case EDIT_DC_VOLT_TRIPPOINT:
                if (btn == BUTTON_SHORT_PRESS) {
                    current_focus = (current_focus == FOCUS_CANCEL) ? FOCUS_PLUS : current_focus + 1;
                } else if (btn == BUTTON_LONG_PRESS) {
                    if (current_focus == FOCUS_PLUS) ov_temp = (ov_temp >= LIMIT_OV_MAX) ? LIMIT_OV_MAX : ov_temp + 1;
                    else if (current_focus == FOCUS_MINUS) ov_temp = (ov_temp <= LIMIT_OV_MIN) ? LIMIT_OV_MIN : ov_temp - 1;
                    else if (current_focus == FOCUS_SAVE) { 
                        conf.vtrip = ov_temp; eeprom_update_byte(&EEVtrip, conf.vtrip); 
                        ui_state = EDIT_DC_UNDERVOLTAGE; current_focus = FOCUS_PLUS; require_release = true; ui_clear_screen(); 
                    }
                    else if (current_focus == FOCUS_CANCEL) { ui_state = MENUSCREEN; require_release = true; ui_clear_screen(); }
                }
                if (ui_state == EDIT_DC_VOLT_TRIPPOINT) {
                    if (ov_temp == LIMIT_OV_HARDWARE) strcpy_P(buf, txt_hw);
                    else formatNumber(ov_temp, buf, sizeof(buf), 2, 1, 'V');
                    draw_edit_menu(ttl_vtrip, buf, current_focus);
                }
                break;

            case EDIT_DC_UNDERVOLTAGE:
                if (btn == BUTTON_SHORT_PRESS) {
                    current_focus = (current_focus == FOCUS_CANCEL) ? FOCUS_PLUS : current_focus + 1;
                } else if (btn == BUTTON_LONG_PRESS) {
                    if (current_focus == FOCUS_PLUS) uv_temp = (uv_temp >= LIMIT_UV_MAX) ? LIMIT_UV_MAX : uv_temp + 1;
                    else if (current_focus == FOCUS_MINUS) uv_temp = (uv_temp <= LIMIT_UV_MIN) ? LIMIT_UV_MIN : uv_temp - 1;
                    else if (current_focus == FOCUS_SAVE) { 
                        conf.uvtrip = uv_temp; eeprom_update_byte(&EEUVtrip, conf.uvtrip); 
                        ui_state = EDIT_DC_CALIBRATION; current_focus = FOCUS_PLUS; require_release = true; ui_clear_screen(); 
                    }
                    else if (current_focus == FOCUS_CANCEL) { ui_state = MENUSCREEN; require_release = true; ui_clear_screen(); }
                }
                if (ui_state == EDIT_DC_UNDERVOLTAGE) {
                    formatNumber(uv_temp, buf, sizeof(buf), 2, 1, 'V');
                    draw_edit_menu(ttl_uvtrip, buf, current_focus);
                }
                break;

            case EDIT_DC_CALIBRATION:
                if (btn == BUTTON_SHORT_PRESS) {
                    current_focus = (current_focus == FOCUS_CANCEL) ? FOCUS_PLUS : current_focus + 1;
                } else if (btn == BUTTON_LONG_PRESS) {
                    if (current_focus == FOCUS_PLUS) cal_temp = (cal_temp >= LIMIT_CAL_MAX) ? LIMIT_CAL_MAX : cal_temp + 10;
                    else if (current_focus == FOCUS_MINUS) cal_temp = (cal_temp <= LIMIT_CAL_MIN) ? LIMIT_CAL_MIN : cal_temp - 10;
                    else if (current_focus == FOCUS_SAVE) { 
                        conf.cal = cal_temp; eeprom_update_word(&EECalibration, conf.cal); initINA(conf.cal);
                        ui_state = EDIT_AC_CALIBRATION; current_focus = FOCUS_PLUS; require_release = true; ui_clear_screen(); 
                    }
                    else if (current_focus == FOCUS_CANCEL) { ui_state = MENUSCREEN; require_release = true; ui_clear_screen(); }
                }
                if (ui_state == EDIT_DC_CALIBRATION) {
                    formatNumber(cal_temp, buf, sizeof(buf), 3, 0, 0);
                    draw_edit_menu(ttl_dccal, buf, current_focus); 
                }
                break;

            case EDIT_AC_CALIBRATION:
                if (btn == BUTTON_SHORT_PRESS) {
                    current_focus = (current_focus == FOCUS_CANCEL) ? FOCUS_PLUS : current_focus + 1;
                } else if (btn == BUTTON_LONG_PRESS) {
                    if (current_focus == FOCUS_PLUS) ac_cal_temp = (ac_cal_temp >= LIMIT_AC_CAL_MAX) ? LIMIT_AC_CAL_MAX : ac_cal_temp + 1;
                    else if (current_focus == FOCUS_MINUS) ac_cal_temp = (ac_cal_temp <= LIMIT_AC_CAL_MIN) ? LIMIT_AC_CAL_MIN : ac_cal_temp - 1;
                    else if (current_focus == FOCUS_SAVE) { 
                        conf.ac_cal = ac_cal_temp; eeprom_update_word(&EEACCalibration, conf.ac_cal); 
                        ui_state = EDIT_SCREENSAVER; current_focus = FOCUS_PLUS; saver_temp = active_saver_setting; require_release = true; ui_clear_screen(); 
                    }
                    else if (current_focus == FOCUS_CANCEL) { ui_state = MENUSCREEN; require_release = true; ui_clear_screen(); }
                }
                if (ui_state == EDIT_AC_CALIBRATION) {
                    formatNumber(ac_cal_temp, buf, sizeof(buf), 2, 0, 0);
                    draw_edit_menu(ttl_accal, buf, current_focus);
                }
                break;

            case EDIT_SCREENSAVER:
                if (btn == BUTTON_SHORT_PRESS) {
                    current_focus = (current_focus == FOCUS_CANCEL) ? FOCUS_PLUS : current_focus + 1;
                } else if (btn == BUTTON_LONG_PRESS) {
                    if (current_focus == FOCUS_PLUS) saver_temp = (saver_temp >= 3) ? 3 : saver_temp + 1;
                    else if (current_focus == FOCUS_MINUS) saver_temp = (saver_temp == 0) ? 0 : saver_temp - 1;
                    else if (current_focus == FOCUS_SAVE) { 
                        active_saver_setting = saver_temp; 
                        eeprom_update_byte(&EEScreensaver, active_saver_setting); 
                        ui_state = MENUSCREEN; require_release = true; ui_clear_screen(); 
                    }
                    else if (current_focus == FOCUS_CANCEL) { ui_state = MENUSCREEN; require_release = true; ui_clear_screen(); }
                }
                if (ui_state == EDIT_SCREENSAVER) {
                    strcpy_P(buf, saver_labels[saver_temp & 3]); // saver_temp is clamped to 0..3 anyway
                    draw_edit_menu(ttl_saver, buf, current_focus);
                }
                break;

            case RECOVER:
                // The relay is driven only by the main tick: clearing the flags here is enough,
                // the relay closes on the next ~11 ms tick once main re-evaluates the faults.
                // Never block here (no delay loops): the main tick must keep monitoring the rails.
                recover_reason = recover_check(dcvoltage, latched_ticks);
                if (recover_reason == RECOVER_OK) {
                    system_faults.all = 0;
                    oc_retries = 0;        // Manual intervention refills the AUTO retry budget
                    auto_recover_cnt = 0;
                    latched_ticks = 0;     // A re-trip must serve the full minimum latch time again
                    ui_state = MENUSCREEN; 
                    ui_clear_screen();
                } else {
                    ui_state = RECOVER_REFUSED;
                    recover_msg_ticks = RECOVER_MSG_TICKS;
                    require_release = true; // Ignore the still-held button
                    ui_clear_screen();
                }
                break;

            case RECOVER_REFUSED:
                // Non-blocking denial screen: shows why the clear was rejected until the
                // timeout expires or the user presses the button again.
                if (btn != BUTTON_IDLE || recover_msg_ticks == 0) {
                    ui_state = MENUSCREEN;
                    require_release = true;
                    ui_clear_screen();
                } else {
                    ui_draw_str_P(0, UI_ROW_TOP, msg_refused, 16);
                    ui_draw_str_P(0, UI_ROW_BOTTOM,
                                  recover_reason == RECOVER_TOO_SOON   ? msg_too_soon :
                                  recover_reason == RECOVER_SENSOR_BAD ? msg_sensor   : msg_voltage, 16);
                }
                break;
        }

        ui_display_cycle = (ui_display_cycle + 1) % 4; 
    }
}
