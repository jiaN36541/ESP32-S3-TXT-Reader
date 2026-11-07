# Hardware Wiring Guide

## 1. Overview

This document describes the GPIO connections for the ESP32-S3 TXT Reader.

Target hardware:

- ESP32-S3 development board
- ST7735 TFT display
- Micro SD card module
- W25Q128 SPI Flash
- Five physical buttons

## 2. TFT Display (ST7735)

| TFT Pin | ESP32-S3 GPIO |
|---|---:|
| SCK | 12 (defined in source) |
| MOSI | 11 (defined in source) |
| CS | 10 |
| DC | 9 |
| RST | 8 |
| BLK | 7 |

Note: The current Adafruit_ST7735 constructor uses hardware SPI. The actual SPI pins depend on the ESP32-S3 board configuration and should be verified before wiring.

## 3. SD Card Module

| SD Pin | ESP32-S3 GPIO |
|---|---:|
| SCK | 41 |
| MISO | 42 |
| MOSI | 15 |
| CS | 16 |

The SD card uses a dedicated SPIClass instance.

SPI clock frequency: 4 MHz.

## 4. W25Q128 Flash

| Flash Pin | ESP32-S3 GPIO |
|---|---:|
| SCK | 18 |
| MISO | 21 |
| MOSI | 17 |
| CS | 40 |

The Flash chip uses software bit-banged SPI.

## 5. Physical Buttons

All buttons are configured with internal pull-up resistors.

Connect one side of each button to the corresponding GPIO and the other side to GND.

| Button | ESP32-S3 GPIO |
|---|---:|
| Previous Page | GPIO 1 |
| Next Page | GPIO 2 |
| Back / Exit | GPIO 3 |
| Menu | GPIO 4 |
| Save Progress | GPIO 6 |

Button pressed state: LOW.

## 6. LED

| Device | ESP32-S3 GPIO |
|---|---:|
| User LED | GPIO 48 |

## 7. Power and Safety

- Connect all module grounds to a common GND.
- Verify module operating voltage before connecting power.
- Check signal voltage compatibility.
- Do not connect 5V signals directly to ESP32-S3 GPIO pins.
- Verify wiring before powering on the device.

## 8. Verification Status

The GPIO assignments are extracted from the Arduino source code.

Physical wiring and hardware operation have not yet been independently verified.
