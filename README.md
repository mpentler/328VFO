# 328VFO
An ATMega328p-powered Si5351a VFO controller and CW keyer. This could be used in VFO only mode but also can also send a key signal for use in an actual transmitter/amplifier circuit. This is my intended end goal, but you could do something different, or swap out the Si5351a code for another device, etc..

# Setup

Standard breadboarded 328p with 16MHz crystal and reset button, buttons connected to PORTD as below, shared i2c bus with pull-up resistors for screen/clock.

* PD0 PTT/Enter
* PD1 Step Size
* PD2 Rotary button, enter/exit menu
* PD3 Rotary A, left
* PD4 Rotary B, right
* PD5 Band change

Some issues with debouncing or button reads being missed have occurred - help me if you can! Maybe I need a timer instead of sleeping?

An output is configured on PORTB in the code to trigger on Tx but isn't implemented yet. This could drive a IC gate with the clock but I'm killing the clock anyway on key up so maybe doing it this way isn't needed. We'll see when my MOSFETs arrive...

# Automatic message sender

In menus, self-explanatory, can be cancelled after the current character has been sent with any key press or knob rotation
