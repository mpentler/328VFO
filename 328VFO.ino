// Custom ATMega328p-powered simple VFO
// Mark Pentler 2021
//
// V0.1 - first release
// This is fully-working right now in terms of being able to tune any frequency, transmit, send a stored message in morse code, change the
// drive strength current of the clock generator, and has some basic menu implementation with a rotary encoder and buttons. If you use this
// and change any pins you will need to change any code accordingly for different PORT/PIN registers. I went for size/speed for a known
// config rather than going for portability

#include <avr/sleep.h>
#include <avr/interrupt.h>
#include "SSD1306AsciiAvrI2c.h"
#include "si5351.h"

// State tracking variables
boolean tx = false;
boolean sending_message = false;
boolean menu_displayed = false;
boolean enable_select = false;
volatile boolean input_received = false;

// Initial PORTD button states
uint8_t portdhistory = 0b00111111; // default is high because of the pull-up resistors

// Defining some buttons and doing the rotary encoder setup here
#define button_PTT PD0
#define button_step PD1
#define button_menu PD2
#define encoder_A PD3
#define encoder_B PD4
#define button_band PD5
byte encoder_seqA = 0;
byte encoder_seqB = 0;

// Menu system
char *menuMainList[] = {"Main Menu", " Drive Current", " Stored Message", " Reserved"};
char *menuOpt1List[] = {"Drive Current", " 2ma", " 4ma", " 8ma"};
char *menuOpt2List[] = {"Stored Message", " Send Message", " View Message"};
uint8_t menupage = 0;
uint8_t menuoption = 1;

// Frequency variables, band changing & step sizes setup, plus drive strength for UI
unsigned long frequency = 7000000ULL;
const unsigned long bandstarts[] = {3500000ULL, 7000000ULL, 10100000ULL, 14000000ULL, 18068000ULL, 21000000ULL, 24890000ULL, 28000000ULL};
const unsigned long freqstep[] = {100, 500, 1000, 5000, 10000, 100000};
#define band_arraylength (sizeof(bandstarts) / sizeof(bandstarts[0]))
#define step_arraylength (sizeof(freqstep) / sizeof(freqstep[0]))
uint8_t freqsteps = 1;
uint8_t currentband = 1;
uint8_t drive_strength = 8;

// Morse code arrays per-character, some durations, and a stored message
const char *letters[] = {
  // The letters A-Z in Morse code
  ".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..",
  ".---", "-.-", ".-..", "--", "-.", "---", ".--.", "--.-", ".-.",
  "...", "-", "..-", "...-", ".--", "-..-", "-.--", "--.."
};
const char *numbers[] = {
  // The numbers 0-9 in Morse code
  "-----", ".----", "..---", "...--", "....-", ".....", "-....",
  "--...", "---..", "----."
};
uint8_t dot_duration = 100;
uint8_t message_delay = 2000;
const char stored_message[] = "CQ TEST DE MM3IIG";

// define clockgen and 128x32 display
Si5351 clockgen;
SSD1306AsciiAvrI2c display;

// ******************************************************************************************************************************************
// The important functions first
// ************************************
void setup() {
  // Define I/O pins and set some registers
  DDRB = 0b00000010;                                                                                                               // Define PB1 as output for later use
  DDRD |= (1 << button_PTT) | (1 << button_step) | (1 << button_menu) | (1 << encoder_A) | (1 << encoder_B) | (1 << button_band);  // Define the buttons as inputs
  PORTD |= (1 << button_PTT) | (1 << button_step) | (1 << button_menu) | (1 << encoder_A) | (1 << encoder_B) | (1 << button_band); // Set pullup resistors
  PCICR |= (1 << PCIE2);                                                                                                           // Pin-change interrupts on PORTD
  PCMSK2 |= (1 << button_PTT) | (1 << button_step) | (1 << button_menu) | (1 << encoder_A) | (1 << encoder_B | (1 << button_band));// Set pin-change mask to read the 6 inputs
  ADCSRA = 0; // Disable the ADC to save power

  // Display setup
  display.begin(&Adafruit128x32, 0x3C); // Address 0x3C for 128x32 LCD
  display.setFont(System5x7);
  redraw_VFO_UI();

  // set clk0 output to the starting frequency
  clockgen.init(SI5351_CRYSTAL_LOAD_8PF, 0, 0);
  clockgen.drive_strength(SI5351_CLK0, SI5351_DRIVE_8MA);
  clockgen.set_freq(frequency * 100ULL, SI5351_CLK0);
  update_display();
  clockgen.output_enable(SI5351_CLK0, 0); // Turn off the clockgen once frequency set, as default behaviour seems to turn it on when frequency is changed
}

void loop() {
  if (input_received) {
    poll_inputs(); // What was the input?
  }
  sleep(); // The Si5351a and display will keep going without the Arduino after commanded, so we can save power until next input
}

void sleep() {
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);    // Set the correct sleep mode
  sleep_enable();                         // Sets the Sleep Enable bit in the MCUCR Register (SE BIT)
  sleep_cpu();                            // Sleep!
  // =========================================== sleeps here
  sleep_disable();                        // Clear SE bit
}

ISR (PCINT2_vect) { // Called on any input
  input_received = true;
}

// ******************************************************************************************************************************************
// Button/Rotary Encoder functions here
// ************************************
void poll_inputs() { // All of this from https://www.avrfreaks.net/forum/pin-change-interrupt-atmega328p - Debounce needs implementing, possibly?
  uint8_t changedbits;
  uint8_t intreading = PIND & PCMSK2;
  changedbits = PIND ^ portdhistory;
  portdhistory = intreading; // Save current input states

  switch (changedbits) {
    case 1: // PTT or Enter depending on if we're in menus or not
      if (!(PIND & (1 << button_PTT))) {
        if (!(menu_displayed)) {
          if (!(sending_message)) {           // Button going down, but we're not sending a message already
            tx = true;
            clockgen.output_enable(SI5351_CLK0, 1);
            display.clearField(0, 2, 3);
            display.print("Tx!");
          }
          else {                              // Button going down, but we are message-sending, so a button push cancels it
            sending_message = false;
          }
        }
        else {                                // Button going down, but we are in menus, so it's an enter button
          if (!(enable_select)) { // Are we in a main or sub-menu?
            menupage = menuoption;
            draw_menu();
            display.clearField(0, menuoption, 1);
            display.print(">");      // Draw cursor
          }
          else {
            menu_selectoption();
          }
        }
      }
      else {
        // Button going up - on PTT it cycles the clockgen and display, in menu mode we want to just ignore it and move on so it doesn't interfere with
        // any display generated by other parts of the code.
        if ((!(menu_displayed)) && (!(sending_message))) {
          tx = false;
          clockgen.output_enable(SI5351_CLK0, 0);
          display.clearField(0, 2, 3);
        }
      }
      break;

    case 2: // Change step size
      if (!(PIND & (1 << button_step))) {
        if ((!(menu_displayed)) && (!(sending_message))) {
          freqsteps += 1;
          if (freqsteps > step_arraylength - 1) {
            freqsteps = 0; // Reset to beginning of array
          }
          clockgen.set_freq(frequency * 100ULL, SI5351_CLK0);
          update_display();
        }
        else {
          sending_message = false;
        }
      }
      break;

    case 4: // Enter/Exit menu system
      if (!(PIND & (1 << button_menu))) {
        if (!(sending_message)) {
          // send_message();
          if (!(menu_displayed)) {
            draw_menu();
            display.clearField(0, menuoption, 1);
            display.print(">");
            menu_displayed = true;
          }
          else {
            menu_cancel();
          }
        }
        else {
          sending_message = false;
        }
      }

      break;

    case 32: // Change band
      if (!(PIND & (1 << button_band))) {
        if ((!(menu_displayed)) && (!(sending_message))) {
          currentband += 1;
          if (currentband > band_arraylength - 1) {
            currentband = 0; // Reset to beginning of array
          }
          frequency = bandstarts[currentband];
          update_display();
        }
        else {
          sending_message = false;
        }
      }
      break;

    default: // The rotary encoder is the only thing left...
      if (!(sending_message)) {
        poll_encoder();
      }
      else {
        sending_message = false;
      }
  }

  input_received = false; // Reset input tracker
}

void poll_encoder() { // All of this great code from https://www.allaboutcircuits.com/projects/how-to-use-a-rotary-encoder-in-a-mcu-based-project/
  boolean encoderA_val = PIND & (1 << encoder_A);
  boolean encoderB_val = PIND & (1 << encoder_B);

  encoder_seqA <<= 1;
  encoder_seqA |= encoderA_val;
  encoder_seqB <<= 1;
  encoder_seqB |= encoderB_val;

  encoder_seqA &= 0b00001111;
  encoder_seqB &= 0b00001111;

  // Need to implement some kind of frequency control here. Do I want to set limits? Free run?
  // Probably should lock to amateur bands only with an upper and lower limit, checked in here.
  if (encoder_seqA == 0b00001001 && encoder_seqB == 0b00000011) {
    if (!(menu_displayed)) {
      frequency -= freqstep[freqsteps];
      clockgen.set_freq(frequency * 100ULL, SI5351_CLK0);
      update_display();
    }
    else {
      display.clearField(0, menuoption, 1);
      menuoption--;
      if (menuoption == 0) {
        menuoption = 3;  // Reset to bottom
      }
      display.clearField(0, menuoption, 1);
      display.print(">");       // Draw cursor
    }
  }

  if (encoder_seqA == 0b00000011 && encoder_seqB == 0b00001001) {
    if (!(menu_displayed)) {
      frequency += freqstep[freqsteps];
      clockgen.set_freq(frequency * 100ULL, SI5351_CLK0);
      update_display();
    }
    else {
      display.clearField(0, menuoption, 1);
      menuoption++;
      if (menuoption == 4) {
        menuoption = 1;  // Reset to top
      }
      display.clearField(0, menuoption, 1);
      display.print(">");      // Draw cursor
    }
  }
}

// ******************************************************************************************************************************************
// Helper functions (screen, clockgen, menus)
// ***********************************************
void update_display() {
  // convert frequency to floating point, adding 0.0000001 fixes a rounding error (but not properly)
  double freq = (double) frequency / 1000000 + 0.0000001;

  display.clearField(32, 0, 7);
  display.print(freq, 4);
  display.clearField(32, 1, 7);
  display.print(freqsteps[freqstep]);
  display.clearField(65, 2, 1);
  display.print(drive_strength);
}

void redraw_VFO_UI() {
  display.clear();
  display.println("Freq: xxxxxx MHz");
  display.println("Step: xxxxxx KHz");
  display.println("xxx | Drv: x mA");
  display.clearField(0, 2, 3);
}

void draw_menu() {
  display.clear();
  switch (menupage) {
    case 0:
      display.println(menuMainList[0]);
      display.println(menuMainList[1]);
      display.println(menuMainList[2]);
      display.println(menuMainList[3]);
      break;

    case 1:
      display.println(menuOpt1List[0]);
      display.println(menuOpt1List[1]);
      display.println(menuOpt1List[2]);
      display.println(menuOpt1List[3]);
      enable_select = true;
      break;

    case 2:
      display.println(menuOpt2List[0]);
      display.println(menuOpt2List[1]);
      display.println(menuOpt2List[2]);
      enable_select = true;
      break;

    case 3:
      display.println("Reserved");
      enable_select = true;
      break;
  }
}

void menu_selectoption() {
  switch (menupage) {
    case 1:
      switch (menuoption) {
        case 1:
          clockgen.drive_strength(SI5351_CLK0, SI5351_DRIVE_2MA);
          drive_strength = 2;
          break;

        case 2:
          clockgen.drive_strength(SI5351_CLK0, SI5351_DRIVE_4MA);
          drive_strength = 4;
          break;

        case 3:
          clockgen.drive_strength(SI5351_CLK0, SI5351_DRIVE_8MA); // Clear the correct menu line
          drive_strength = 8;
          break;
      }
      display.clearField(0, menuoption, 17); // Clear the correct menu line
      display.println(">Selected");
      delay(1000);
      menu_cancel();
      break;

    case 2:
      switch (menuoption) {
        case 1:
          display.clearField(0, menuoption, 17);
          display.println(">Selected");
          delay(1000);
          menu_cancel();
          send_message();
          break;

        case 2:
          display.clearField(0, 3, 17);
          display.println(stored_message);
          delay(2000);
          display.clearField(0, 3, 17);
          break;
      }
      break;

    case 3:
      break;
  }
}

void menu_cancel() { // Reused a lot
  enable_select = false;
  menupage = 0;
  menuoption = 1;
  redraw_VFO_UI();
  update_display();
  menu_displayed = false;
}

// ******************************************************************************************************************************************
// All Morse code functions below here
// ***********************************************
void send_message() {
  tx = true;
  sending_message = true;
  display.clearField(0, 2, 3);
  display.print("Tx!");
  display.clearField(0, 3, 17);
  display.print(stored_message);
  while (sending_message) { // Assuming no input received...
    for (uint8_t i = 0; i < strlen(stored_message); i++) {
      char ch = stored_message[i];
      // Check for uppercase letters
      if (ch >= 'A' && ch <= 'Z') {
        flash_morse_code(letters[ch - 'A']);
      }
      // Check for lowercase letters
      else if (ch >= 'a' && ch <= 'z') {
        flash_morse_code(letters[ch - 'a']);
      }
      // Check for numbers
      else if (ch >= '0' && ch <= '9') {
        flash_morse_code(numbers[ch - '0']);
      }
      // Check for space between words
      else if (ch == ' ') {
        // Put space between two words in a message...equal to seven dots
        delay(dot_duration * 7);
      }
      if (input_received) {
        poll_inputs();
      }
      if (!(sending_message)) {
        break; // Break out of the for loop here
      }
    }
    delay(message_delay);
  }
  tx = false;
  display.clearField(0, 2, 3);
  display.clearField(0, 3, 17);
}

void flash_morse_code(char *morse_code) {
  uint8_t i = 0;

  // Read the dots and dashes and flash accordingly
  while (morse_code[i] != NULL) {
    flash_dot_or_dash(morse_code[i]);
    i++;
  }

  delay(dot_duration * 3); // Space between two letters is equal to three dots
}

void flash_dot_or_dash(char dot_or_dash) {
  clockgen.output_enable(SI5351_CLK0, 1); // Turn the clockgen on
  if (dot_or_dash == '.') { // If it is a dot
    delay(dot_duration);
  }
  else { // Has to be a dash...equal to three dots
    delay(dot_duration * 3);
  }

  clockgen.output_enable(SI5351_CLK0, 0); // Turn the clockgen off
  // Give space between parts of the same letter...equal to one dot
  delay(dot_duration);
}
