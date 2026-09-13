# Skematik & Wiring Diagram
### Ringkasan Pinout Hardware:

```text
+-------------------+----------------+--------------------------------------+
| Komponen          | Pin Komponen   | Pin ESP32-S3 (WROOM / EYE)           |
+-------------------+----------------+--------------------------------------+
| LCD 16x2 I2C      | SDA            | GPIO 1                               |
|                   | SCL            | GPIO 2                               |
|                   | VCC            | 5V                                   |
|                   | GND            | GND                                  |
+-------------------+----------------+--------------------------------------+
| NFC PN532 (HSU)   | TX (SDA)       | GPIO 47 (RX1 ESP32-S3)               |
|                   | RX (SCL)       | GPIO 48 (TX1 ESP32-S3)               |
|                   | RST (RST0)     | GPIO 21                              |
|                   | VCC            | 5V / 3.3V                            |
|                   | GND            | GND                                  |
|                   | Switch HSU     | Posisi [0 0] (I0=0, I1=0)            |
+-------------------+----------------+--------------------------------------+
| PIR (HC-SR501)    | OUT / Data     | GPIO 38                              |
|                   | VCC            | 5V                                   |
|                   | GND            | GND                                  |
+-------------------+----------------+--------------------------------------+
| Kamera (OV5640)   | D0 - D7        | Y2-Y9 (Lihat board_config.h)         |
| (5 Megapixel DVP) | XCLK, PCLK     | Pin S3 Standar                       |
|                   | VSYNC, HREF    | Pin S3 Standar                       |
+-------------------+----------------+--------------------------------------+
```
