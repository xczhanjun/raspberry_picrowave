#include <ruby.h>
#include <wiringPi.h>
#include <wiringShift.h>
#include <stdio.h>
#include <stdbool.h>

#define DEBUG false
#define SERIAL_TIMEOUT 1000
#define VOICE_COMMAND_TIMEOUT 10000   // Allow voice commands up to 10 seconds after the door closes

#define scanDataPin  0
#define scanLatchPin 1
#define scanClockPin 2

#define inputDataPin  3
#define inputLatchPin 4
#define inputClockPin 5

#define outputDataPin  6
#define outputLatchPin 7
#define outputClockPin 10

#define btn0 0
#define btn1 1
#define btn2 2
#define btn3 3
#define btn4 4
#define btn5 5
#define btn6 6
#define btn7 7
#define btn8 8
#define btn9 9
#define btnWeightDefrost 10
#define btnJetDefrost 11
#define btnPreset 12
#define btnTime 13
#define btnExpress 14
#define btnCancel 15
#define btnPower 16
#define btnMemory 17
#define btnClock 18
#define btnStart 19

#define newBtnHigh10s 1
#define newBtnHigh20s 2
#define newBtnHigh30s 3
#define newBtnHigh1m 4
#define newBtnHigh2m 5
#define newBtnMed10s 6
#define newBtnMed20s 7
#define newBtnMed30s 8
#define newBtnMed1m 9
#define newBtnMed2m 0
#define newBtnStart 10
#define newBtnStop 11
#define newBtn10s 12
#define newBtn10m 13
#define newBtnMed 14
#define newBtnDefrost 15
#define newBtn1s 16
#define newBtn1m 17
#define newBtnHigh 18
#define newBtnLow 19

#define doorSwitch 0x40 // B01000000
#define buttonMask 0x3F // B00111111

const int MAX_SECONDS   = 5999; // (99 * 60) + 59
const int POWER_HIGH    = 10;
const int POWER_MEDIUM  = 7;
const int POWER_LOW     = 5;
const int POWER_DEFROST = 3;
const int POWER_ZERO    = 0;

// btnMatrix[VCC Pin][GND Pin]
int btnMatrix[6][6] = {
  {-1, -1, -1, -1, -1, 19},
  {-1,  0,  1,  2,  3, -1},
  {-1,  4,  5,  6,  7, -1},
  {10,  8,  9, 12, 15, -1},
  {-1, 13, 18, 11, 16, -1},
  {17, -1, 14, -1, -1, -1}
};

char btnNames[][15] = {
  "0",
  "1",
  "2",
  "3",
  "4",
  "5",
  "6",
  "7",
  "8",
  "9",
  "Weight Defrost",
  "Jet Defrost",
  "Pre-set",
  "Time",
  "Express",
  "Cancel",
  "Power",
  "Memory",
  "Clock",
  "Start"
};

uint8_t inputByte, buttonByte, scanMask;
int currentButton = -1;
int lastButton = -1;
int lastPushedButton = -1;
bool on       = false;
bool paused   = false;
bool doorOpen = false;
bool pendingStart = false;
bool buttonLock = false;

VALUE commandFromVoice = Qfalse;
int  currentPower = 0;
int  currentTime  = 0;

char serCommand, serParams;

unsigned long serialTimer, buttonTimer, countdownTimer, doorTimer;


// Check voice commands
bool voiceCommandAllowed() {
  if (commandFromVoice != Qfalse) {
    return millis() - VOICE_COMMAND_TIMEOUT < doorTimer;
  }
  return true;
}

// If multiple buttons are pressed, this
// function just returns the first detected button.
// Stop pressing so many buttons at once.
int pressedButton(uint8_t vccByte, uint8_t gndByte) {
  int vcc, gnd, button;

  for (vcc = 0; vcc < 6; vcc++) {
    if (vccByte & (0x40 >> vcc)) {  // B01000000
      for (gnd = 0; gnd < 6; gnd++) {
        if (gndByte & (0x20 >> gnd)) {  // B00100000
          button = btnMatrix[vcc][gnd];
          if (button != -1) {
            return button;
          }
        }
      }
    }
  }

  return -1;
}

// Find button in matrix
bool findButton(int button, int pins[]) {
  int vcc, gnd;
  for (vcc = 0; vcc < 6; vcc++) {
    for (gnd = 0; gnd < 6; gnd++) {
      if (btnMatrix[vcc][gnd] == button) {
        pins[0] = vcc;
        pins[1] = gnd;
        return true;
      }
    }
  }
  return false;
}

void pushButton(int button) {
  int buttonPins[2];
  uint8_t vccByte, gndByte;

  if (DEBUG) {
    printf("Pressing: ");
    printf("%s", btnNames[button]);
  }

  if (findButton(button, buttonPins)) {
    // Ensure buttons aren't being pushed by multiple threads.
    while (buttonLock) { delay(25); }
    buttonLock = true;

    // VCC 1,2,3,4,5,6 = 7,6,5,4,3,2 (second register)
    // GND 1,2,3,4,5,6 = 7,6,5,4,3,2 (first register)
    vccByte = 0x40 >> buttonPins[0]; // B01000000
    gndByte = 0x40 >> buttonPins[1]; // B01000000

    // Only need a 'off' delay when pushing the same button twice
    if (lastPushedButton == button) { delay(25); }

    digitalWrite(outputLatchPin, LOW);
    shiftOut(outputDataPin, outputClockPin, MSBFIRST, vccByte);
    shiftOut(outputDataPin, outputClockPin, MSBFIRST, gndByte);
    digitalWrite(outputLatchPin, HIGH);

    // Start button needs a longer press
    delay(button == btnStart ? 64 : 25);

    digitalWrite(outputLatchPin, LOW);
    shiftOut(outputDataPin, outputClockPin, MSBFIRST, 0);
    shiftOut(outputDataPin, outputClockPin, MSBFIRST, 0);
    digitalWrite(outputLatchPin, HIGH);

    buttonLock = false;
    lastPushedButton = button;
  }
}

void setTime(int totalSeconds) {
  int minutes, seconds;

  if (totalSeconds > MAX_SECONDS) { return; }

  // Setting time will stop the microwave
  if (on) { on = false; }

  currentTime = totalSeconds;

  pushButton(btnTime);

  minutes = totalSeconds / 60;
  seconds = totalSeconds % 60;

  if (minutes != 0) {
    if (minutes / 10 != 0) {
      pushButton(minutes / 10);
    }
    pushButton(minutes % 10);
  }
  if (seconds >= 10 || minutes != 0) {
    pushButton(seconds / 10);
  }
  pushButton(seconds % 10);
}

void incrementTime(int extraSeconds) {
  if (currentTime + extraSeconds > MAX_SECONDS) { currentTime = 0; }
  setTime(currentTime + extraSeconds);
}

void setPower(int power) {
  if (power < 0 || power > 10) { return; }

  currentPower = power;

  pushButton(btnPower);

  if (power == 10) {
    pushButton(1);
    pushButton(0);
  } else {
    pushButton(power);
  }
}

// time is an int, of the form 'hhmm', without leading zeros.
// Times must be between 1:00 and 12:59
void setClock(unsigned int time) {
  if (time > 1259 || time < 100 || time % 100 > 59 ) { return; }

  pushButton(btnClock);
  pushButton(btnCancel);

  if (time >= 1000) { pushButton(1); }
  pushButton(time % 1000 / 100);
  pushButton(time % 100 / 10);
  pushButton(time % 10);

  pushButton(btnClock);
}

void start() {
  // Only start if door is closed
  if (!doorOpen) {
    if (currentTime > 0) {
      pushButton(btnStart);
      on = true;
      paused = false;
      pendingStart = false;
      // Set door timer to 0, so that no further commands are run
      doorTimer = 0;
      // Set countdown timer to decrement time remaining
      countdownTimer = millis();
    }
  } else {
    // Record the fact that start was pushed while the door was open.
    // Once door is closed, start the microwave.
    pendingStart = true;
  }
}

void stop() {
  pushButton(btnCancel);
  on = false;
  paused = false;
  pendingStart = false;
  currentTime = 0;
  currentPower = 0;
}

void pause() {
  // Can only pause when microwave is on
  if (on) {
    pushButton(btnTime);
    on = false;
    paused = true;
  }
}

void quickStart(int seconds, int power) {
  setTime(seconds);
  setPower(power);
  start();
}


static VALUE method_send_command(VALUE self, VALUE args) {
  VALUE command, param;

  long len = RARRAY_LEN(args);

  if (len > 2 || len == 0) {
      rb_raise(rb_eArgError, "wrong number of arguments");
  }

  command = rb_ary_entry(args, 0);

  if (len >= 2) { param = rb_ary_entry(args, 1); }

  // Voice - indicates that the command came from latent speech recognition (no keyword prefix).
  // The command will only be executed if the microwave door was recently closed.
  if (len >= 3) {
    commandFromVoice = rb_ary_entry(args, 2);
  } else {
    commandFromVoice = Qfalse;
  }

  /* Command API
   *
   * To start microwave for 2 minutes on high, send the following commands: plht120;s
   * (power level high, time 120s, start)
   *
   */
  switch (command) {
    // Button  |  bc  => Push Cancel button
    case 'b':
      switch (StringValue(param)) {
        case '0': pushButton(btn0); break;
        case '1': pushButton(btn1); break;
        case '2': pushButton(btn2); break;
        case '3': pushButton(btn3); break;
        case '4': pushButton(btn4); break;
        case '5': pushButton(btn5); break;
        case '6': pushButton(btn6); break;
        case '7': pushButton(btn7); break;
        case '8': pushButton(btn8); break;
        case '9': pushButton(btn9); break;
        case 'w': pushButton(btnWeightDefrost); break;
        case 'j': pushButton(btnJetDefrost); break;
        case 'r': pushButton(btnPreset); break;
        case 't': pushButton(btnTime); break;
        case 'e': pushButton(btnExpress); break;
        case 'c': pushButton(btnCancel); break;
        case 'p': pushButton(btnPower); break;
        case 'm': pushButton(btnMemory); break;
        case 'l': pushButton(btnClock); break;
        case 's': pushButton(btnStart); break;
      }
      break;

    // Clock  |  c1135  => Set clock to 11:35
    case 'c':
      setClock(NUM2INT(param));
      break;

    // Time  |  t90;  => Set time to 90 seconds
    case 't':
      if (voiceCommandAllowed()) {
        setTime(NUM2INT(param));
      }
      break;

    // Power (integer or level)
    // Integer: p6; => Set power to 6
    // Level:   ph  => Set power to high (10)
    case 'p':
      if (voiceCommandAllowed()) {
        switch (StringValue(param)) {
          case 'h': setPower(POWER_HIGH);    break;
          case 'm': setPower(POWER_MEDIUM);  break;
          case 'l': setPower(POWER_LOW);     break;
          case 'd': setPower(POWER_DEFROST); break;
          case 'o': setPower(POWER_ZERO);    break;
          default:  setPower(NUM2INT(param)); break;
        }
      }
      break;

    // Start
    case 's':
      if (voiceCommandAllowed()) {
        start();
      }
      break;

    // Stop
    case 'S':
      if (voiceCommandAllowed()) {
        stop();
      }
      break;

    // Pause
    case 'P':
      pause();
      break;
  }

  return self;
}


VALUE method_get_info(VALUE self) {
  VALUE info = rb_hash_new();

  rb_hash_aset(info, rb_str_new2("on"),            on       ? Qtrue : Qfalse);
  rb_hash_aset(info, rb_str_new2("paused"),        paused   ? Qtrue : Qfalse);
  rb_hash_aset(info, rb_str_new2("door_open"),     doorOpen ? Qtrue : Qfalse);
  rb_hash_aset(info, rb_str_new2("current_power"), INT2FIX(currentPower));
  rb_hash_aset(info, rb_str_new2("current_time"),  INT2FIX(currentTime));

  return info;
}


VALUE method_touchpad_loop(VALUE self) {
  for (;;) {
    inputByte = 0;
    //              B00000010              B01000000
    for (scanMask = 0x2; scanMask <= 0x40; scanMask <<= 1) {
      digitalWrite(scanLatchPin, LOW);
      shiftOut(scanDataPin, scanClockPin, MSBFIRST, scanMask);
      digitalWrite(scanLatchPin, HIGH);

      // Ignore first bit (floating)
      inputByte = shiftIn(inputDataPin, inputClockPin, MSBFIRST) & 0x7F; // B01111111

      // 1 = closed, 0 = open
      if (!(inputByte & doorSwitch)) {
        // Stop if door is opened
        if (on) { stop(); }
        if (!doorOpen) {
          if (DEBUG) { printf("Door is now open.\n"); }
          doorOpen = true;
        }
      } else {
        // Set door state if door is now closed
        if (doorOpen) {
          if (DEBUG) { printf("Door is now closed.\n"); }
          doorOpen = false;
          doorTimer = millis(); // Allow voice commands up to 7 seconds after door was closed

          // Check to see if start was pressed while door was open
          // If so, start the microwave now.
          if (pendingStart) { start(); }
        }
      }

      buttonByte = inputByte & buttonMask;

      if (buttonByte != 0) {
        currentButton = pressedButton(scanMask, buttonByte);

        if (currentButton != -1 && currentButton != lastButton) {
          switch (currentButton) {
            case newBtnHigh10s: quickStart(10,  POWER_HIGH); break;
            case newBtnHigh20s: quickStart(20,  POWER_HIGH); break;
            case newBtnHigh30s: quickStart(30,  POWER_HIGH); break;
            case newBtnHigh1m:  quickStart(60,  POWER_HIGH); break;
            case newBtnHigh2m:  quickStart(120, POWER_HIGH); break;
            case newBtnMed10s:  quickStart(10,  POWER_MEDIUM); break;
            case newBtnMed20s:  quickStart(20,  POWER_MEDIUM); break;
            case newBtnMed30s:  quickStart(30,  POWER_MEDIUM); break;
            case newBtnMed1m:   quickStart(60,  POWER_MEDIUM); break;
            case newBtnMed2m:   quickStart(120, POWER_MEDIUM); break;
            case newBtn10s:     incrementTime(10); break;
            case newBtn10m:     incrementTime(600); break;
            case newBtn1s:      incrementTime(1); break;
            case newBtn1m:      incrementTime(60); break;
            // Can't set power while microwave is on
            case newBtnHigh:    if (!on) { setPower(POWER_HIGH); }; break;
            case newBtnMed:     if (!on) { setPower(POWER_MEDIUM); }; break;
            case newBtnLow:     if (!on) { setPower(POWER_LOW); }; break;
            case newBtnDefrost: if (!on) { setPower(POWER_DEFROST); }; break;
            case newBtnStart:   start(); break;
            case newBtnStop:    stop(); break;
          }

          lastButton = currentButton;
        }

        buttonTimer = millis();
      } else {
        // Debounce buttons with 100ms delay
        if (lastButton != -1 && millis() - 100 > buttonTimer) {
          lastButton = -1;
          buttonTimer = 0;
        }
      }
    }

    if (on) {
      // Decrement time counter every 1000ms
      if (millis() - 1000 > countdownTimer) {
        countdownTimer = millis();
        currentTime -= 1;
        if (currentTime <= 0) {
          stop();
        }
      }
    }

    delay(5);
  }
}

// void printBinary(uint8_t var) {
//   for (uint8_t mask = 0x80; mask; mask >>= 1) {  // B10000000
//       printf(mask & var ? '1' : '0');
//   }
//   printf("\n");
// }


VALUE MicrowaveExt;

// The initialization method for this module
void Init_microwave() {
  // Set pin modes
  pinMode(scanDataPin,    OUTPUT);
  pinMode(scanLatchPin,   OUTPUT);
  pinMode(scanClockPin,   OUTPUT);

  pinMode(inputDataPin,   INPUT);
  pinMode(inputLatchPin,  OUTPUT);
  pinMode(inputClockPin,  OUTPUT);

  pinMode(outputDataPin,  OUTPUT);
  pinMode(outputLatchPin, OUTPUT);
  pinMode(outputClockPin, OUTPUT);

  // Setup class
  MicrowaveExt = rb_define_class("MicrowaveExt", rb_cObject);
  rb_define_method(MicrowaveExt, "touchpad_loop", method_touchpad_loop, 0);
  rb_define_method(MicrowaveExt, "send_command", method_send_command, -2);
  rb_define_method(MicrowaveExt, "get_info", method_get_info, 0);
}
