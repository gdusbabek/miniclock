SAMD21 GPS Clock

I would like to build a GPS-disciplined clock for use during my portable ham radio activities. I will be using the following materials:
- Seeed Studio SAMD21 MCU.
- u-blox NEO-6M GPS board (four pins)
- A 1.54 inch e-paper module from Waveshare with a SPI interface (https://www.waveshare.com/1.54inch-e-paper-module.htm)

I would like for it to display the following:
- UTC time and date (updated once a minute)
- 6-digit Maidenhead grid location.
- Variable text depending on the maidenhead grid.
- Number of satellites.

Additional things if I have pin/header space:
- A DS18B20 temperature sensor.

On startup I would like:
- initialize pins.
- Initialize the main Serial device (which may or may not be attached).
- to initialize the epaper display.
- have it say “acquiring lock”
- Initialize the GPS. It will be attached the SAMD21 rx/tx pins as Serial1.
- once the GPS lock is achieved, set the system date/time.

The main loop will consist of the following:
- consume gps data off serial1.
- if available output a few sentences of NMEA to the serial device (this is so my computer can synchronize it’s time in the field)
- Once a minute synchronize the GPS time to the system clock (if we have a gps lock).
- Update the display.

What I need from you initially is a set of good pin assigments for the GPS and epaper display. 
I expect the GPS to be connected to:
- 5v, gnd, tx (d6), rx (d7)
I expect the epaper display to be connected to:
- DIN/MOSI → SPI MOSI
- CLK → SPI SCK
- CS → any GPIO (please suggest)
- DC → any GPIO (please suggest)
- RST → any GPIO (please suggest)
- BUSY → any GPIO (please suggest)
- VCC → 3.3 V
- GND → GND
