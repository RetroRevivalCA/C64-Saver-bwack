/**
 * @file ina219.h
 * @brief INA219 Voltage and Current Sensor API.
 * @details Provides functions to initialize, read, and recover the INA219 sensor 
 * over a lightweight, bit-banged I2C interface optimized for the ATtiny85.
 */

#ifndef _INA219_H_
#define _INA219_H_

#include <stdint.h>

/**
 * @brief Initializes the INA219 sensor over I2C.
 * @details Transmits the configuration register (0b0011100110011111) to set 
 * 32V bus range, PGA gain of /8 (320mV), and fast continuous conversions. 
 * Also applies the calibration multiplier.
 * @param calValue The 16-bit calibration multiplier loaded from EEPROM.
 */
void initINA(uint16_t calValue); 

/**
 * @brief Forcefully resets a locked I2C bus.
 * @details Clocks SCL up to 9 times to free a hung slave holding SDA low,
 * then transmits a standard STOP condition.
 */
void I2C_reset(void);

/**
 * @brief Reads the C64's 5V DC bus voltage.
 * @return The bus voltage in millivolts (mV), or 0xFFFF if the sensor is unresponsive/disconnected.
 */
uint16_t ina219_get_bus_voltage_mv(void);

/**
 * @brief Reads the C64's DC current draw.
 * @return The current draw in milliamperes (mA). Returns as a signed integer to 
 * account for negative calibration flutters at zero-load. (Note: 0xFFFF is no longer 
 * a reliable indicator of sensor disconnect for this function).
 */
int16_t ina219_get_current_ma(void);

/**
 * @brief Bit mask to apply to a calibration value before comparing it with a read-back.
 * @details Bit 0 (FS0) of the INA219 calibration register is void and always reads 0,
 * so an odd value written to the sensor reads back with its LSB cleared.
 */
#define INA219_CAL_REG_MASK 0xFFFE

/**
 * @brief Reads the active calibration register from the sensor.
 * @details Used as a liveness/reset check: an INA219 that has been reset (static shock,
 * brownout) reverts to calibration 0 and silently reports 0 mA. Compare the result with
 * (expected & INA219_CAL_REG_MASK).
 * @return The raw 16-bit register value, or 0xFFFF if unresponsive (a stuck-low SDA reads 0x0000).
 */
uint16_t ina219_get_calibration(void);

#endif /* _INA219_H_ */
