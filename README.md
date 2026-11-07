# ESP32-S3 TXT Reader

An ESP32-S3 based TXT e-book reader project.

## Features

- TXT file reading
- TFT color display
- SD card storage support
- UTF-8 text support
- Physical button control
- Reading progress management

## Hardware

- ESP32-S3 development board
- TFT display
- SD card module
- W25Q128 Flash memory
- Physical buttons

## Software

- Arduino IDE
- Arduino framework
- C/C++

## Project Structure

```text
ESP32-S3-TXT-Reader/
├── ESP32S3-TXT-Reader.ino
└── README.md
```

## Development Status

This project is under development.

## Author

GitHub: [@jiaN36541](https://github.com/jiaN36541)

## Dependencies

### Arduino Libraries

Install the following libraries using Arduino IDE Library Manager:

- [Adafruit GFX Library](https://github.com/adafruit/Adafruit-GFX-Library)
- [Adafruit ST7735 and ST7789 Library](https://github.com/adafruit/Adafruit-ST7735-Library)
- [U8g2_for_Adafruit_GFX](https://github.com/olikraus/U8g2_for_Adafruit_GFX)

### Development Environment

- Arduino IDE
- ESP32 Arduino Core
- Target board: ESP32-S3

### Built-in Libraries

The following libraries are included with the ESP32 Arduino Core:

- SPI
- SD
- FS
- ESP-IDF heap capabilities

## Pin Configuration

See the source code for the complete GPIO configuration.

## License

No license has been specified yet.
