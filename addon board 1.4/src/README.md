# Firmware for the addon board

This folder holds the firmware for the C64 Saver 2 addon board 1.4 (ATtiny85). It started as bwack's original firmware and has been rewritten in this fork. **This folder now contains firmware 1.00.**

- What changed, how it was tested, the fuse settings and the known limitations: [CHANGES.md](CHANGES.md). Read it before building or flashing.
- The firmware changes were written with AI assistance and tested on real hardware, but have not had an independent human review. See the note at the top of CHANGES.md.
- A ready-to-flash `main.hex` is attached to the [v1.00 release](https://github.com/RetroRevivalCA/C64-Saver-bwack/releases/tag/v1.00). Its SHA-256 is in CHANGES.md.

## Files

| File | Purpose |
|------|---------|
| `board.h` | Pin assignments, timing and limits, firmware version (`FW_VERSION`) and the change log |
| `main.c` | Protection logic, ADC sampling and the main tick |
| `ui.c`, `ui.h` | OLED display, button handling and the settings menu |
| `ina219.c`, `ina219.h` | INA219 current and voltage sensor driver |
| `libs/` | SSD1306 OLED library (tinusaur) |
| `Makefile` | Build and programming settings (see the notes at the top of the file) |

## Programming

Program the addon board using a USBasp or other AVR programming ISP device. Arduino as ISP does also work. The ISP header has 2 mm pitch, and the pinout is the standard 2x3 way ISP pinout. The Makefile is set up for a `usbtiny` programmer by default, so change `AVRDUDE_PROGRAMMER` if you use another one.

Use the `main.hex` file from the releases tab, or build it yourself (below). **The ATtiny85 fuses must be set for 8 MHz with CKDIV8 off.** The values, and a warning about the one setting that can lock the chip out, are in CHANGES.md under "Fuses". Flashing wipes the EEPROM settings, so back them up first if you want to keep them (the command is in CHANGES.md).

### Alternative: flash through the OLED header

The ISP header is a small 2 mm pitch footprint, and some people may not have a matching header or cable lying around. If you do not want to solder or improvise connections to all six small pads, you can use a mix of points instead. This only needs two of the six ISP pads, which are much easier to reach, and the other four connections come from the larger OLED header. This is how the boards were flashed while developing firmware 1.00.

1. Disconnect the board from the C64 and leave the 9 V AC unplugged.
2. Unplug the OLED display from its header.
3. Connect the programmer to these points:

   | Programmer line | Take it from |
   |-----------------|--------------|
   | GND | OLED header, GND |
   | VCC | OLED header, VCC |
   | SCK | OLED header, SCL (this is the ATtiny85's SCK line) |
   | MOSI | OLED header, SDA (this is the ATtiny85's MOSI line) |
   | MISO | ISP footprint, MISO pad |
   | RESET | ISP footprint, RST pad |

4. The programmer powers the board through the VCC connection (a USBtiny was used). Flash the hex or run `make install` as usual.
5. Remove the programming wires and plug the display back in before powering the board normally.

Check the pin positions against the schematic and the board silkscreen before connecting anything.

## Developing

To build the project you need to download this repository from GitHub. From there you will get the sources for the addon board.
Install WinAVR (Windows), or avr-gcc (Linux, Mac). The avrdude version might not be compatible with your USBasp. If so, download Arduino also, and copy avrdude from it into your WinAVR bin folder.

Firmware 1.00 was built with the Microchip AVR8 GNU toolchain (avr-gcc 15.1). The program uses 99.5% of the ATtiny85's flash (8,147 of 8,192 bytes), and a different compiler version changes the size, so check the size output after building.

In the src folder you can clean, build and program:
- make clean
- make
- make install (this programs the flash and the fuses as set in the Makefile)

## Contributing to the GitHub repository

Fork the project and make your changes. Make sure you "make clean" before push to the repo. We only want source code in the repo. Have fun!
