/**
 * @file board.h
 * @brief Hardware Abstraction Layer (HAL) and Global System Definitions.
 * @details Defines hardware pinout macros, safe operating limits for configuration,
 * and global state structures for the C64 Saver 2 firmware.
 *
 * CHANGELOG:
 * - 2026-11-07: FW_VERSION 1.00, first release of this fork. See CHANGES.md in this folder
 *   for the full list of changes from the original firmware.
 */

#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>
#include <stdbool.h>
#include <avr/io.h>

// ============================================================================
// BUILD-TIME SANITY CHECKS
// ============================================================================
/**
 * @brief CPU frequency this firmware's timing constants were tuned for.
 * @details The ~11 ms tick (Timer1 /8192, OCR1A=10), the ~340 ms overcurrent debounce,
 * the bit-banged I2C delays and the 250 ms watchdog all assume this clock. Build with
 * -DF_CPU=8000000UL and program the fuses for the 8 MHz internal oscillator with
 * CKDIV8 UNprogrammed. Change this macro only after re-deriving every timing constant.
 */
#define FW_EXPECTED_F_CPU   8000000UL

#ifndef F_CPU
#error "F_CPU is not defined. Build with -DF_CPU=8000000UL (see FW_EXPECTED_F_CPU in board.h)."
#elif (F_CPU != FW_EXPECTED_F_CPU)
#error "F_CPU does not match FW_EXPECTED_F_CPU: all firmware timing constants assume 8 MHz."
#endif

// ============================================================================
// HARDWARE ABSTRACTION LAYER (HAL)
// ============================================================================
/** @brief I2C Clock Pin (SCL) mapping on ATtiny85. */
#define SSD1306_SCL   PB2  
/** @brief I2C Data Pin (SDA) mapping on ATtiny85. */
#define SSD1306_SDA   PB0  
/** @brief Relay control pin, active low. */
#define HOLD_N        PB1  

/** @brief Macro to safely disconnect the relay (pull HOLD_N high). */
#define RELAY_DISCONNECT()  (PORTB |= (1 << HOLD_N))
/** @brief Macro to safely connect the relay (pull HOLD_N low). */
#define RELAY_CONNECT()     (PORTB &= ~(1 << HOLD_N))

/** @brief ADC channel for AC voltage sensing. */
#define ADC_CH_VAC    2    
/** @brief ADC channel for AC current sensing. */
#define ADC_CH_ACCUR  3    

/**
 * @brief Selects the AC Voltage sensing channel for the ADC.
 */
static inline void adc_select_channel_vac(void) {
    ADMUX = (ADMUX & 0xF0) | ADC_CH_VAC;
}

/**
 * @brief Selects the AC Current sensing channel for the ADC.
 */
static inline void adc_select_channel_accur(void) {
    ADMUX = (ADMUX & 0xF0) | ADC_CH_ACCUR;
}

// ============================================================================
// CONFIGURATION LIMITS (Software Bounds)
// ============================================================================

// DC overcurrent limits (Amperes * 10)
#define LIMIT_OC_MAX        20  ///< 2A Maximum allowed user setting
#define LIMIT_OC_MIN        5   ///< 0.5A Minimum allowed user setting
#define LIMIT_OC_DEFAULT    12  ///< 1.2A Factory default setting

// DC overvoltage limits (Volts * 10)
#define LIMIT_OV_MAX        55   ///< 5.5V Maximum allowed user setting  
#define LIMIT_OV_MIN        51   ///< 5.1V Minimum allowed user setting  
#define LIMIT_OV_DEFAULT    54   ///< 5.4V Factory default setting
#define ACCUR_SCALE_NUM     46    ///< AC current scale numerator (over 50). 33 is the ACS712-5A datasheet value; 46 matched the meter
#define ACCUR_NOISE_VAR     16    ///< Sensor noise variance (counts squared) removed from the AC current reading; tune so idle reads 0.00
#define OV_CONFIRM_TICKS    3     ///< Consecutive over-voltage readings (~11 ms each) needed to trip
#define LIMIT_OV_HARDWARE   55   ///< 5.5V Hardware limit of main board, determined by resistors.

// DC undervoltage limits (Volts * 10)
#define LIMIT_UV_MAX        49   ///< 4.9V Maximum allowed user setting
#define LIMIT_UV_MIN        40   ///< 4.0V Minimum allowed user setting
#define LIMIT_UV_DEFAULT    46   ///< 4.6V Factory default setting
#define LIMIT_UV_MINIMUM    1000 ///< 1.0V Minimum threshold to ignore powered-off state

// Fault recovery / retry tuning (all values in main-loop ticks).
// One tick is NOMINALLY about 11 ms (Timer1 /8192 at 8 MHz = 1.024 ms per count, compare at 10,
// so 11 counts), and it is longer whenever the tick body is slow, e.g. while the OLED is redrawn
// every tick because a fault is latched. The "seconds" quoted below are therefore approximate,
// and run somewhat long while a fault is displayed.
#define RECOVER_MIN_LATCH_TICKS  200  ///< A fault must stay latched >= 2 s before ANY clear is accepted (filters AC-sag glitches)
#define RECOVER_PRESS_EVENTS     2    ///< Long-press events needed to clear a latched fault (~0.9 s hold)
#define RECOVER_MSG_TICKS        200  ///< How long the "recover refused" message is shown (2 s)
#define SENSOR_RECOVER_TICKS     100  ///< Consecutive good sensor ticks before a sensor fault auto-clears, in ANY mode (1 s)
#define OC_AUTO_RETRY_MAX        3    ///< AUTO mode: overcurrent retries before the fault latches until manual recovery
#define OC_HEALTHY_TICKS         6000 ///< Connected, fault-free, below-limit time that refills the retry budget (60 s)

// DC calibration limits
#define LIMIT_CAL_MAX       800  ///< Maximum INA219 calibration multiplier
#define LIMIT_CAL_MIN       100  ///< Minimum INA219 calibration multiplier
#define LIMIT_CAL_DEFAULT   400  ///< Default INA219 calibration multiplier

// AC calibration limits
#define LIMIT_AC_CAL_MAX    50   ///< Maximum ATtiny85 ADC AC multiplier
#define LIMIT_AC_CAL_MIN    10   ///< Minimum ATtiny85 ADC AC multiplier
#define LIMIT_AC_CAL_DEFAULT 20  ///< Default ATtiny85 ADC AC multiplier

// ============================================================================
// GLOBAL STATE & DATA STRUCTURES
// ============================================================================
/** @brief Firmware version string for UI display. */
#define FW_VERSION "1.00"

/**
 * @struct config
 * @brief Holds user-configurable limits and calibration data.
 */
struct config {
    uint8_t oc;       ///< DC Overcurrent trip point in Amps
    uint8_t hold;     ///< Overvoltage/overcurrent recovery mode (0 = Auto-recover, 1 = Hold until manual clear); sensor faults always auto-clear
    uint8_t vtrip;    ///< DC Overvoltage trip point (Volts * 10)
    uint8_t uvtrip;   ///< DC Undervoltage warning point (Volts * 10)
    uint16_t cal;     ///< INA219 calibration multiplier
    uint16_t ac_cal;  ///< AC sensing ADC calibration multiplier
};
extern struct config conf; 

// EEPROM Variable Declarations
extern uint8_t EEMEM EEOvercurrent;
extern uint8_t EEMEM EEHold;
extern uint8_t EEMEM EEVtrip;
extern uint8_t EEMEM EEUVtrip; 
extern uint16_t EEMEM EECalibration; 
extern uint16_t EEMEM EEACCalibration; 
extern uint8_t EEMEM EEScreensaver;

/**
 * @union fault_status_t
 * @brief Bitfield for tracking active hardware faults.
 */
typedef union {
    struct {
        uint8_t overcurrent    : 1; ///< 1 if an overcurrent fault is active
        uint8_t overvoltage    : 1; ///< 1 if an overvoltage fault is active
        uint8_t sensor         : 1; ///< 1 if the INA219 is unresponsive or has lost its calibration
    } flags;
    uint8_t all; ///< Access to all flags simultaneously
} fault_status_t;

extern fault_status_t system_faults;

/**
 * @brief True when the most recent INA219 read passed validation.
 * @details Validation = bus voltage responded (not 0xFFFF) AND the calibration register
 * reads back the value we wrote. Written only by the main-loop tick, read by the UI.
 * Unlike the voltage passed to the UI (which is masked to 0 on a bad read), this flag
 * lets the UI tell "reading is 0 V" apart from "reading is invalid".
 */
extern bool sensor_valid;
extern uint16_t recorded_overcurrent;

/**
 * @struct isr_data_t
 * @brief Shared data populated by the ADC Interrupt Service Routine.
 */
typedef struct {
    uint16_t vac_peak;   ///< Peak-to-peak AC Voltage ADC reading
    uint16_t accur_var;  ///< Variance (counts squared) of the AC current ADC samples over one 256-sample window
    bool button_down;    ///< True if UI push-button is currently pressed
} isr_data_t;

/**
 * @brief ISR-shared sensor data. The object is volatile; the TYPE is not, so that the local
 * snapshot taken by the UI is an ordinary (register-friendly) variable and costs no extra flash.
 */
extern volatile isr_data_t sensor_data;

// ============================================================================
// TYPE-SAFE ENUMS (Strong Typing)
// ============================================================================
/**
 * @enum screen_state_t
 * @brief Enumerates the top-level UI states and edit menus.
 */
typedef enum { 
    MENUSCREEN=0,           
    RECOVER,                
    EDIT_DC_OVERCURRENT,    
    EDIT_OVERVOLTAGE_HOLD,  
    EDIT_DC_VOLT_TRIPPOINT, 
    EDIT_DC_UNDERVOLTAGE,   
    EDIT_DC_CALIBRATION,    
    EDIT_AC_CALIBRATION,    
    EDIT_SCREENSAVER,
    RECOVER_REFUSED         ///< Transient message shown when a manual fault clear is rejected
} screen_state_t;

/**
 * @enum display_cycle_t
 * @brief Enumerates the dashboard data cycles.
 */
typedef enum { 
    UI_SHOW_VAC=0, UI_SHOW_ACCUR=1, UI_SHOW_VDC=2, UI_SHOW_DCCUR=3
} display_cycle_t;

/**
 * @enum button_press_t
 * @brief Enumerates processed button events.
 */
typedef enum { 
    BUTTON_IDLE=0, BUTTON_SHORT_PRESS=1, BUTTON_LONG_PRESS=2 
} button_press_t;

#endif // BOARD_H
