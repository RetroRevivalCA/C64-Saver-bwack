/**
 * @file ina219.c
 * @brief INA219 I2C Driver Implementation.
 * @details Handles bit-banged I2C communication to the INA219 power monitor to 
 * conserve flash memory on the ATtiny85 by avoiding full TWI libraries.
 */

#include <stdint.h>
#include <avr/io.h>
#include <util/delay.h>
#include "ina219.h"

// Hardware pin mappings for I2C
#define SCL          PB2
#define SDA          PB0
#define TICK         5            ///< Microseconds per I2C clock cycle tick
#define HALF_TICK    (TICK / 2.0) ///< Microseconds for half clock cycle

// Internal INA219 Register Map
enum {
  REG_ADDR_CONFIG,
  REG_ADDR_SHUNT_VOLTAGE,
  REG_ADDR_BUS_VOLTAGE,
  REG_ADDR_POWER,
  REG_ADDR_CURRENT,
  REG_ADDR_CALIBRATION
};

// --- Low-Level Bit-Banged I2C Macros ---
#define DIGITAL_WRITE_HIGH(PORT, PAUSE)    { PORTB |= (1 << PORT); if (PAUSE) { _delay_us(TICK); } }
#define DIGITAL_WRITE_LOW(PORT, PAUSE)     { PORTB &= ~(1 << PORT); if (PAUSE) { _delay_us(TICK); } }

// Input state must be sampled from PINB; PORTB only holds the output latch / pull-up setting.
#define SET_SDA_INPUT()     { DDRB &= ~(1 << SDA); PORTB |= (1 << SDA); }
#define SET_SCL_INPUT()     { DDRB &= ~(1 << SCL); PORTB |= (1 << SCL); }
#define SET_SDA_OUTPUT()    { DDRB |= (1 << SDA); }
#define SET_SCL_OUTPUT()    { DDRB |= (1 << SCL); }
#define IS_INPUT(PORT)      (0 == (DDRB & (1 << PORT)))
#define IS_OUTPUT(PORT)     (0 != (DDRB & (1 << PORT)))

uint8_t slave_address; ///< Stores the I2C 7-bit slave address

// ============================================================================
// BIT-BANGED I2C CORE
// ============================================================================

/**
 * @brief Generates an I2C Start Condition.
 */
void xfer_start(void) {
  SET_SCL_OUTPUT();
  DIGITAL_WRITE_LOW(SCL, 1);
  SET_SDA_OUTPUT();
  DIGITAL_WRITE_HIGH(SDA, 1);
  DIGITAL_WRITE_HIGH(SCL, 1);
  DIGITAL_WRITE_LOW(SDA, 1);
  DIGITAL_WRITE_LOW(SCL, 1);
}

/**
 * @brief Generates an I2C Stop Condition.
 */
void xfer_stop(void) {
  SET_SDA_OUTPUT();
  DIGITAL_WRITE_LOW(SDA, 1);
  SET_SCL_OUTPUT();
  DIGITAL_WRITE_HIGH(SCL, 1);
  DIGITAL_WRITE_HIGH(SDA, 1);
}

/**
 * @brief Transmits a single byte over the bit-banged I2C bus.
 * @param byte The 8-bit data to send.
 * @return 0 if ACK received, non-zero if NACK.
 */
int send_byte(uint8_t byte) {
  uint8_t i;

  if (IS_INPUT(SDA)) {
      SET_SDA_OUTPUT();
  }
  if (IS_INPUT(SCL)) {
      SET_SCL_OUTPUT();
  }
  
  // Transmit 8 bits, MSB first
  for (i = 0; i < 8; i++) {
    if ((byte << i) & 0x80) {
        DIGITAL_WRITE_HIGH(SDA, 0);
    } else {
        DIGITAL_WRITE_LOW(SDA, 0);
    }
    
    _delay_us(1);
    DIGITAL_WRITE_HIGH(SCL, 1);
    DIGITAL_WRITE_LOW(SCL, 0);
    _delay_us(4);
  }
  
  // Wait for ACK after byte transmission
  _delay_us(TICK);
  SET_SDA_INPUT();
  _delay_us(1);
  DIGITAL_WRITE_HIGH(SCL, 0);
  _delay_us(HALF_TICK);
  
  // Sample SDA from PINB (not PORTB): the slave ACKs by pulling SDA low
  int result = PINB & (1 << SDA);
  
  _delay_us(HALF_TICK);
  DIGITAL_WRITE_LOW(SCL, 0);
  SET_SDA_OUTPUT();
  DIGITAL_WRITE_LOW(SDA, 1);

  return result;
}

/**
 * @brief Receives a single byte from the bit-banged I2C bus.
 * @param last Set to 1 if this is the final byte (sends NACK), 0 otherwise (sends ACK).
 * @return The received 8-bit data.
 */
uint8_t receive_byte(uint8_t last) {
  uint8_t i, val = 0;

  SET_SDA_INPUT();
  // Read 8 bits, MSB first
  for (i = 0; i < 8; i++) {
    DIGITAL_WRITE_HIGH(SCL, 1);
    val <<= 1;
    val |= (PINB & (1 << SDA)) ? 0x01 : 0x00;
    DIGITAL_WRITE_LOW(SCL, 1);
  }
  SET_SDA_OUTPUT();
  
  // Transmit ACK/NACK
  if (last) {
      DIGITAL_WRITE_HIGH(SDA, 1); // Send NACK
  } else {
      DIGITAL_WRITE_LOW(SDA, 1);  // Send ACK
  }
  
  DIGITAL_WRITE_HIGH(SCL, 1);
  DIGITAL_WRITE_LOW(SCL, 1);
  return val;
}

/**
 * @brief Writes a 16-bit value to a specific INA219 register.
 * @param reg Register address.
 * @param val 16-bit value to write.
 */
void write_register(uint8_t reg, uint16_t val) {
  xfer_start();
  if (send_byte((uint8_t)((slave_address << 1) & 0xfe))) { xfer_stop(); return; } // Check ACK
  if (send_byte(reg)) { xfer_stop(); return; }                                    // Check ACK
  if (send_byte((uint8_t)((val >> 8) & 0xff))) { xfer_stop(); return; }           // Check ACK
  if (send_byte((uint8_t)(val & 0xff))) { xfer_stop(); return; }                  // Check ACK
  xfer_stop();
}

/**
 * @brief Reads a 16-bit value from a specific INA219 register.
 * @param reg Register address.
 * @param buf Pointer to buffer storing the read 16-bit value.
 */
void read_register(uint8_t reg, uint16_t *buf) {
  xfer_start();
  if (send_byte((slave_address << 1))) { xfer_stop(); *buf = 0xFFFF; return; } // Write addr
  if (send_byte(reg)) { xfer_stop(); *buf = 0xFFFF; return; }                  // Reg pointer
  xfer_stop();
  
  xfer_start();
  if (send_byte((uint8_t)((slave_address << 1) | 0x01))) { xfer_stop(); *buf = 0xFFFF; return; } // Read addr
  
  // Read MSB then LSB in separate statements: C leaves operand evaluation order
  // unspecified, and the master's ACK/NACK sequence depends on this order.
  uint8_t msb = receive_byte(0);
  uint8_t lsb = receive_byte(1);
  *buf = ((uint16_t)msb << 8) | lsb;
  
  xfer_stop();
}

/**
 * @brief Forcefully resets a locked I2C bus.
 */
void I2C_reset(void) {
  SET_SDA_INPUT();
  SET_SCL_OUTPUT();
  
  // Clock the bus up to 9 times to free a hung slave holding SDA low
  for (uint8_t i = 0; i < 9; i++) {
      if (PINB & (1 << SDA)) break; // SDA released high (sampled from PINB, not PORTB)
      DIGITAL_WRITE_HIGH(SCL, 1);
      DIGITAL_WRITE_LOW(SCL, 1);
  }
  
  xfer_start(); // Generate a standard STOP condition to reset the slave's state machine
  xfer_stop();
}

// ============================================================================
// INA219 SENSOR API
// ============================================================================

void initINA(uint16_t calValue) {
  slave_address = 0b1000000; // Default INA219 I2C address (0x40)

  // Apply the dynamic calibration value loaded from EEPROM
  write_register(REG_ADDR_CALIBRATION, calValue);
  
  // Configuration register breakdown (0x399F, also the INA219 power-on default):
  // 0b0011100110011111 sets the INA219 to:
  // - 32V Bus Voltage Range
  // - PGA gain /8 (320mV range, allowing up to 3.2A on a 0.1R shunt)
  // - 12-bit ADC resolution (fast conversions, no averaging lag)
  // - Continuous Shunt and Bus voltage operating mode
  uint16_t config = 0b0011100110011111; 
  write_register(REG_ADDR_CONFIG, config);
}

uint16_t ina219_get_bus_voltage_mv() {
  uint16_t busvoltage;
  
  read_register(REG_ADDR_BUS_VOLTAGE, &busvoltage);
  
  if (busvoltage == 0xFFFF) return 0xFFFF; // Catch dead sensor before shifting
  
  // Bus voltage is in bits 15:3 with 4 mV per LSB. Bits 1 (CNVR) and 0 (OVF) are
  // status flags and bit 2 is unused: shift all three out, then scale to millivolts.
  // Note: the OVF flag is intentionally not evaluated here.
  return (busvoltage >> 3) * 4; 
}

int16_t ina219_get_current_ma(void) {
  uint16_t current;
  read_register(REG_ADDR_CURRENT, &current);
  // Current register is two's complement. Return as signed integer.
  return (int16_t)current; 
}

/**
 * @brief Implementation note: returns the raw register; bit 0 always reads 0 (see INA219_CAL_REG_MASK).
 */
uint16_t ina219_get_calibration(void) {
  uint16_t cal;
  read_register(REG_ADDR_CALIBRATION, &cal);
  return cal;
}
