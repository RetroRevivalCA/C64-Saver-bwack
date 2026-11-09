/**
 * @file ui.h
 * @brief User Interface Header.
 * @details Exposes the OLED user interface and menu state machine to the main application loop.
 */

#ifndef UI_H
#define UI_H

#include <stdint.h>

/**
 * @brief Processes button inputs, updates configuration menus, and draws to the OLED.
 * @details This function acts as the state machine for user interaction. It debounces the 
 * single hardware button, processes long/short presses, commits changed values to EEPROM, 
 * and controls the screen saver sleep states. Call this function periodically (e.g., every 
 * ~11 ms tick) from the main loop. It internally throttles OLED redraws to save CPU cycles.
 * 
 * @param dcvoltage Real-time DC voltage reading from the sensor (mV).
 * @param dccurrent Real-time DC current reading from the sensor (mA).
 */
void handle_ui_update(uint16_t dcvoltage, uint16_t dccurrent);

/**
 * @brief Draws a string stored in flash (PROGMEM) on the OLED, space-padded to a fixed width.
 * @details All fixed UI text lives in flash: on the ATtiny85 a plain string literal is copied
 * into the 512 bytes of SRAM at start-up, which used to consume most of the RAM. The text is
 * expanded into a small temporary stack buffer just long enough to draw it.
 * @param x       Pixel column (as for ssd1306_string_font8x16xy()).
 * @param row     OLED page row (0 = top line, 2 = bottom line).
 * @param s_P     Text in flash (declare it with PROGMEM).
 * @param width   Number of characters to draw (1..16); shorter text is padded with spaces,
 * longer text is truncated. Padding also erases whatever was previously on screen.
 */
void ui_draw_str_P(uint8_t x, uint8_t row, const char *s_P, uint8_t width);

#endif // UI_H
