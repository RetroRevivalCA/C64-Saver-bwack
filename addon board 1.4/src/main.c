/**
 * @file main.c
 * @brief Main Application Loop, Interrupts, and Hardware Protection Logic.
 * @details Implements the core safety constraints, background ADC peak sampling,
 * EEPROM state recovery, and relay triggering logic for the C64 Saver 2.
 * 
 * CHANGELOG:
 * - 2026-11-07: FW_VERSION 1.00, first release of this fork. See CHANGES.md in this folder
 *   for the full list of changes from the original firmware.
 */

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/eeprom.h>
#include <avr/wdt.h>
#include <avr/boot.h> // Software read of the fuse bytes (CKDIV8 guard)
#include <avr/pgmspace.h> // PROGMEM for the fixed screen text
#include <util/delay.h>
#include <util/atomic.h>

#include "board.h"
#include "ui.h"
#include "font8x16.h"
#include "ssd1306xled.h"
#include "ina219.h"

// Fixed screen text lives in flash, not RAM (16-character lines; ui_draw_str_P pads the rest).
static const char txt_clock_err1[] PROGMEM = " CLOCK ERROR!";
static const char txt_clock_err2[] PROGMEM = " CKDIV8 ACTIVE";
static const char txt_splash1[]    PROGMEM = "   C64 Saver";
static const char txt_splash2[]    PROGMEM = " Firmware v" FW_VERSION;

/** @brief Bit position of CKDIV8 in the ATtiny85 low fuse byte (0 = programmed = clock divided by 8). */
#define LFUSE_CKDIV8_BIT 7

// System Configuration state variables
struct config conf;

// EEPROM Allocations
uint8_t EEMEM EEOvercurrent;
uint8_t EEMEM EEHold;
uint8_t EEMEM EEVtrip;
uint8_t EEMEM EEUVtrip; 
uint16_t EEMEM EECalibration; 
uint16_t EEMEM EEACCalibration; 
uint8_t EEMEM EEScreensaver;

// Fault tracking and sensor data struct allocations
fault_status_t system_faults = {0}; 
uint16_t recorded_overcurrent = 0;
volatile isr_data_t sensor_data = {0, 0, false};
bool sensor_valid = false; // See board.h: result of the latest INA219 validation

/**
 * @brief ADC Interrupt Service Routine.
 * @details Fires continuously to read analog channels. Samples peak-to-peak 
 * AC voltage and current by tracking highs and lows over 200 samples. 
 * Also detects UI push-button states.
 */
ISR(ADC_vect) {
    static uint16_t sample_count = 0;
    static uint16_t adchigh = 0x0000, adclow = 0xFFFF;
    static uint8_t discard_count = 0; 
    static int32_t acc_sum = 0;   // Sum of (sample - 512) for the AC current channel
    static uint32_t acc_sq = 0;   // Sum of squares of (sample - 512)
    static uint16_t vac_f = 0;    // AC voltage peak-to-peak, smoothed: holds 8x the running average

    uint16_t adc_val = ADC; 
    uint8_t current_channel = ADMUX & 0x0F;

    if (discard_count > 0) {
        discard_count--; // Discard samples immediately following channel switch
    } else {
        // The push-button shares the VAC sense pin: a reading below 100 counts as "pressed".
        // Limitation: an AC sag or a shorted sense line is indistinguishable from a press.
        // The UI therefore requires a sustained hold, a minimum latch time and a valid
        // sensor before it will clear a latched fault (see handle_ui_update()).
        if (current_channel == ADC_CH_VAC && adc_val < 100) {
            sensor_data.button_down = true;
        } else {
            if (current_channel == ADC_CH_VAC) sensor_data.button_down = false;
            
            // Track waveform peak-to-peak
            if (adc_val > adchigh) adchigh = adc_val;
            if (adc_val < adclow)  adclow = adc_val;

            if (current_channel != ADC_CH_VAC) {
                // True RMS needs every sample, not just the extremes. Centre on mid-scale (512)
                // so the sums stay small: 256 samples of at most 512 squared fit in 32 bits.
                int16_t d = (int16_t)adc_val - 512;
                acc_sum += d;
                acc_sq  += (uint32_t)((int32_t)d * d);
            }
        }

        // Calculate peak envelope after 200 samples, then swap channels
        // The current channel uses 256 samples (a power of two, so the maths is shifts only)
        if (++sample_count > (current_channel == ADC_CH_VAC ? 200 : 255)) {
            if (current_channel == ADC_CH_VAC) {
                // Smooth the reading (f = f - f/8 + new) so the display does not jump with every
                // window. Seeded on the first window so it does not creep up from 0 at power-up.
                uint16_t pp = adchigh - adclow;
                if (vac_f == 0) vac_f = pp << 3;
                vac_f = vac_f - (vac_f >> 3) + pp;
                sensor_data.vac_peak = vac_f >> 3;
                adc_select_channel_accur();
            } else {
                // Variance = mean(d^2) - mean(d)^2. The mean is kept in quarter-counts
                // (sum >> 6) so truncating it costs little accuracy.
                int16_t m4 = (int16_t)(acc_sum >> 6);
                int32_t v = (int32_t)(acc_sq >> 8) - (((int32_t)m4 * m4) >> 4);
                if (v < 0) v = 0;
                if (v > 65535) v = 65535;
                sensor_data.accur_var = (uint16_t)v;
                adc_select_channel_vac();
            }
            sample_count = 0;
            acc_sum = 0;
            acc_sq = 0;
            adchigh = 0;
            adclow = 0xFFFF;
            discard_count = 10; 
        }
    }
    
    // Start next conversion
    ADCSRA |= (1 << ADSC); 
}

/**
 * @brief Master system initialization sequence.
 * @details Loads user configurations from EEPROM, sanitizes variables within safe bounds,
 * initiates I2C communications, pre-sets timers, and enables the watchdog.
 */
void init(void) {
    // 1. Read stored settings from EEPROM
    uint8_t ee_oc = eeprom_read_byte(&EEOvercurrent);
    uint8_t ee_hold = eeprom_read_byte(&EEHold);
    uint8_t ee_vtrip = eeprom_read_byte(&EEVtrip);
    uint8_t ee_uvtrip = eeprom_read_byte(&EEUVtrip);
    uint16_t ee_cal = eeprom_read_word(&EECalibration); 
    uint16_t ee_ac_cal = eeprom_read_word(&EEACCalibration); 
    
    // 2. Bound-check constraints to prevent erratic behavior from corrupt memory
    conf.oc =     (ee_oc >= LIMIT_OC_MIN && ee_oc <= LIMIT_OC_MAX) ? ee_oc : LIMIT_OC_DEFAULT; 
    conf.hold =   (ee_hold <= 1) ? ee_hold : 1;  
    conf.vtrip =  (ee_vtrip >= LIMIT_OV_MIN && ee_vtrip <= LIMIT_OV_MAX) ? ee_vtrip : LIMIT_OV_DEFAULT; 
    conf.uvtrip = (ee_uvtrip >= LIMIT_UV_MIN && ee_uvtrip <= LIMIT_UV_MAX) ? ee_uvtrip : LIMIT_UV_DEFAULT;
    conf.cal =    (ee_cal >= LIMIT_CAL_MIN && ee_cal <= LIMIT_CAL_MAX) ? ee_cal : LIMIT_CAL_DEFAULT;
    conf.ac_cal = (ee_ac_cal >= LIMIT_AC_CAL_MIN && ee_ac_cal <= LIMIT_AC_CAL_MAX) ? ee_ac_cal : LIMIT_AC_CAL_DEFAULT;

    // The INA219 calibration register ignores bit 0. Force an even value so the runtime
    // read-back comparison can never mismatch because of a corrupt-but-in-range odd EEPROM value.
    conf.cal &= INA219_CAL_REG_MASK;
  
    cli(); // Disable interrupts during hard setup
    
    // 3. Hardware pin states setup
    MCUCR &= ~(1 << PUD); // Global pull-up enable
    DDRB &= ~((1 << PB0) | (1 << PB2));
    PORTB &= ~((1 << PB0) | (1 << PB2));
    
    _delay_ms(40); // Boot stabilization delay

    // 4. Peripherals setup
    ssd1306xled_font8x16 = ssd1306xled_font8x16data;
    ssd1306_init();
    
    // Drive the relay pin to its DISCONNECT level (PORTB high) BEFORE making it an output.
    // Making it an output first would drive the reset-default low level, i.e. CONNECT.
    // init() never connects the relay: the main loop does that after the first valid sensor read.
    RELAY_DISCONNECT(); 
    DDRB |= (1 << HOLD_N);
    
    initINA(conf.cal); 

    // 5. Timer setup (Used for UI rendering frequency)
    TCCR1 = (0b1110 << CS10);
    OCR1A = 10;	
    TCNT1 = 0;
    TIFR |= (1 << OCF1A);

    // 6. ADC setup
    ADMUX = ADC_CH_VAC;
    ADCSRA = (1 << ADEN) | (1 << ADIE) | (1 << ADPS2) | (1 << ADPS1) | (1 << ADPS0);
    ADCSRA |= (1 << ADSC); 

    wdt_enable(WDTO_250MS); // Enable Watchdog timer (250ms timeout)
    sei(); // Enable global interrupts
}

/**
 * @brief Halts the system, with the relay open, if the CKDIV8 fuse is programmed.
 * @details With CKDIV8 programmed the CPU runs at 1 MHz instead of 8 MHz, which makes every
 * timing constant 8x too slow (including the overcurrent debounce and bit-banged I2C).
 * Must be called first in main(), while the watchdog is disabled (so the halt loop cannot
 * trigger a reset loop). The relay is opened BEFORE anything else so a bad clock can never
 * leave power connected.
 * @note Verify on real hardware that the fuse read behaves as expected on the ATtiny85.
 */
static void clock_fuse_guard(void) {
    RELAY_DISCONNECT();            // Level first, then direction (see init())
    DDRB |= (1 << HOLD_N);

    uint8_t low_fuse = boot_lock_fuse_bits_get(GET_LOW_FUSE_BITS);
    if (low_fuse & (1 << LFUSE_CKDIV8_BIT)) {
        return;                    // Bit set = CKDIV8 unprogrammed = full-speed clock: OK
    }

    // Best-effort on-screen message. The relay is already open, so a failure to draw is harmless.
    DDRB &= ~((1 << PB0) | (1 << PB2));
    PORTB &= ~((1 << PB0) | (1 << PB2));
    _delay_ms(40);
    ssd1306xled_font8x16 = ssd1306xled_font8x16data;
    ssd1306_init();
    ssd1306_clear();
    ui_draw_str_P(0, 0, txt_clock_err1, 16);
    ui_draw_str_P(0, 2, txt_clock_err2, 16);

    for (;;) { }                   // Halt. Power-cycle after reprogramming the fuses.
}

/**
 * @brief Non-blocking UI timer check.
 * @return True if a UI rendering tick has elapsed, false otherwise.
 */
bool ui_timer_tick(void) {
    if ((TIFR & (1 << OCF1A)) != 0) {
        TIFR |= (1 << OCF1A); 
        TCNT1 = 0; 
        return true;
    }
    return false;
}

/**
 * @brief Application Main Loop.
 * @details Executes continuous DC safety checks, updates UI state machines, and resets WDT.
 */
int main(void) {
    // Clear WDRF immediately: while it is set the watchdog is forced on at its shortest
    // timeout, which would reset the chip again during init() (boot loop).
    MCUSR = 0;
    wdt_disable();

    // Must run before anything that can connect the relay or arm the watchdog.
    clock_fuse_guard();

    I2C_reset(); // Force clear bus in case of static shock on reboot
    init();
    ssd1306_clear();
    _delay_ms(50);
    wdt_reset(); 
    
    // Draw initial splash screen
    ui_draw_str_P(0, 0, txt_splash1, 16);
    ui_draw_str_P(0, 2, txt_splash2, 16);
    
    // System timers and state trackers
    uint16_t splash_timer = 0; 
    uint16_t dcvoltage = 0;
    uint16_t dccurrent = 0;
    
    uint8_t dc_oc_counter = 0;
    uint8_t dc_ov_counter = 0; // Consecutive over-voltage readings
    uint8_t i2c_err_counter = 0; 
    bool link_established = false; // Set after the first valid INA219 read; relay stays open until then
    bool dc_oc_active = false;

    while (1) {
        wdt_reset(); // Feed watchdog

        // Run non-blocking UI and Sensor update sequence (one tick, nominally ~11 ms)
        if (ui_timer_tick()) {
            
            // Read sensors
            dcvoltage = ina219_get_bus_voltage_mv();
            int16_t raw_current = ina219_get_current_ma();
            uint16_t active_cal = ina219_get_calibration();

            // Validate the sensor. A read is bad if the bus is dead (0xFFFF) or the INA219 has
            // silently reset (calibration reverted, so current would read 0 mA while voltage
            // still looks fine). A stuck-low SDA returns 0x0000 for the calibration register,
            // which also fails the comparison. Bit 0 of the register is void, hence the mask.
            const uint16_t expected_cal = conf.cal & INA219_CAL_REG_MASK;
            sensor_valid = (dcvoltage != 0xFFFF) && (active_cal == expected_cal);

            if (!sensor_valid) {
                if (i2c_err_counter < 255) i2c_err_counter++; // Saturate instead of wrapping
                if (i2c_err_counter > 50) { // 500 ms grace period for sensor boot/recovery
                    system_faults.flags.sensor = 1; // Latch: protection is blind without the sensor
                }
                I2C_reset();
                wdt_reset(); 
                initINA(conf.cal); // Also restores calibration after a sensor reset
                dcvoltage = 0; // Mask invalid readings
                dccurrent = 0;
            } else {
                i2c_err_counter = 0;
                link_established = true;
                // Clamp negative zero-load calibration flutter to 0 mA to prevent wrap-around overcurrent faults
                dccurrent = (raw_current < 0) ? 0 : (uint16_t)raw_current;
            }

            // Flag overcurrent threshold exceed
            dc_oc_active = (dccurrent > (conf.oc * 100));

            // Flag overvoltage limit exceed
            // It must be seen on OV_CONFIRM_TICKS consecutive ticks (3 ticks, about 34 ms), so the
            // sub-millisecond spike from switching the C64 off does not latch a fault.
            if (dcvoltage >= (conf.vtrip * 100)) {
                if (dc_ov_counter < OV_CONFIRM_TICKS) dc_ov_counter++;
            } else {
                dc_ov_counter = 0;
            }
            if (dc_ov_counter >= OV_CONFIRM_TICKS && !system_faults.flags.overvoltage) {
                system_faults.flags.overvoltage = 1;
            }

            // Debounce overcurrent trips (condition must persist for 30 ticks, about 340 ms at ~11 ms per tick)
            if (dc_oc_active) {
                if (dc_oc_counter < 30) dc_oc_counter++; 
            } else {
                // Decay gradually, and hold on masked reads, so a single I2C glitch cannot reset the debounce
                if (dc_oc_counter > 0 && i2c_err_counter == 0) dc_oc_counter--; 
            }

            // Commit overcurrent trip if debounce passes
            if (dc_oc_counter >= 30 && !system_faults.flags.overcurrent) {
                recorded_overcurrent = dccurrent; // Save value that caused the trip
                system_faults.flags.overcurrent = 1;
            }

            // The main tick is the ONLY place that drives the relay. It runs before the slow
            // bit-banged OLED redraw so a fault opens the relay without waiting on the UI.
            // The relay also stays open until the sensor has been read successfully once.
            if (system_faults.all != 0 || !link_established) { 
                RELAY_DISCONNECT(); 
            } else { 
                RELAY_CONNECT(); 
            }

            // Splash screen clear logic
            if (splash_timer < 266) { 
                splash_timer++;
                if (splash_timer == 266) {
                    ssd1306_clear();
                }
            } else {
                // Handoff to UI logic handler
                handle_ui_update(dcvoltage, dccurrent); 
            }
        }
    }
    return 0; // Should never be reached
}
