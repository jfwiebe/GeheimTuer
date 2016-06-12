#include <WS2812.h>

// LED stripe length
#define MAXPIX 20
// animation buffer, read windows moves back and forth EXTRAPIX pixels
#define EXTRAPIX 3

// WS2812 output
const int ledPin = 11;

cRGB leds[MAXPIX+EXTRAPIX];
cRGB tmp_leds[MAXPIX]; // temp array for fading and blinking
cRGB *ledptr; // read pointer for WS2812 library, points to start of window

// requires custom changes to accept uint8_t** argument
WS2812 LED(MAXPIX, &ledptr);

enum ledModes { ledSolid, ledFade, ledBlink, ledForward, ledBackward } ledMode;

// mode-dependent state
union ledState { 
  unsigned int ledidx;
  signed int fader;
  bool blink_on;
} ledState;

// pre-defined colors. GRB order, g++ doesn't implement out-of-order initializers
const struct cRGB traktor = {g: 0xB8/4, r: 0x67/4, b: 0xDC/4}; // https://wiki.attraktor.org/Corporate_Identity
const struct cRGB black = {g: 0x00, r: 0x00, b: 0x00};
const struct cRGB white = {g: 0x1F, r: 0x1F, b: 0x1F};
const struct cRGB red = {g: 0x00, r: 0x1F, b: 0x00};
const struct cRGB green = {g: 0x1F, r: 0x00, b: 0x00};
const struct cRGB blue = {g: 0x00, r: 0x00, b: 0x1F};

// macro instead of inline function. TODO: compare generated code again
// fill LED buffer with solid color, reset state, set mode
#define setLeds1(color1, mode) {\
  for (uint8_t i = 0; i < MAXPIX+EXTRAPIX; i++) {\
    leds[i] = (color1);\
  }\
  memset(&ledState, 0x00, sizeof(ledState));\
  ledMode = (mode);\
}

// fill LED buffer with alternating colors every $modulo pixels, reset state, set mode
#define setLeds2(color1, color2, modulo, mode) {\
  for (uint8_t i = 0; i < MAXPIX+EXTRAPIX; i++) {\
    leds[i] = (i % (modulo)) ? ((color2)) : ((color1));\
  }\
  memset(&ledState, 0x00, sizeof(ledState));\
  ledMode = (mode);\
}


// global motor direction variable, required for PWM-brake & coasting
// TODO: PWM-brake is never used, maybe refactor & remove
enum motorDir { forward, backward } motorDir;

// I/O connections to bridge driver module
// https://www.olimex.com/Products/RobotParts/MotorDrivers/BB-VNH3SP30/
// https://www.olimex.com/Products/RobotParts/MotorDrivers/BB-VNH3SP30/resources/BB-VNH3SP30_schematic.pdf
const int motorDiagAPin = 5;
const int motorDiagBPin = 6;
const int motorInAPin = 7;
const int motorInBPin = 8;
const int motorPWMPin = 9;

// ad-hoc acceleration profile
struct accelProfile {
  unsigned long maxMillis; // time until next step, 0 == end of profile
  unsigned long stepMillis; // modify PWM value every stepMillis ms
  double factor; // newValue = (oldValue + add) * factor
  signed int add;
  byte minPWM; // lower limit
  byte maxPWM; // upper limit
  byte startPWM; // initial value
  bool useStartPWM; // overwrite PWM value with startPWM, ignored on first element
  bool brake; // short motor if true, reset to motorDir if false
};

// slow seek to end-stop, used if door is in unknown position
const struct accelProfile accelProfileLow[] = {
  {
    maxMillis: 13000,
    stepMillis: 50,
    factor: 1,
    add: 4,
    minPWM: 0,
    maxPWM: 64,
    startPWM: 0,
    useStartPWM: true,
    brake: false
  },
  {
    maxMillis: 0
  }
};

// normal operation
// try accelerating as fast as possible without skipping belt teeth
// TODO: install less stiff spring between belt and door to dampen the initial pull
const struct accelProfile accelProfileHigh[] = {
{ // slow start
    maxMillis: 350,
    stepMillis: 50,
    factor: 1,
    add: 5,
    minPWM: 0,
    maxPWM: 30,
    startPWM: 0,
    useStartPWM: true, // first element always sets initial PWM value
    brake: false
  },
  { // accelerate to full-ish speed and keep running
    maxMillis: 3000,
    stepMillis: 50,
    factor: 1.05,
    add: 0,
    minPWM: 0,
    maxPWM: 180,
    startPWM: 0,
    useStartPWM: false,
    brake: false
  },
/*
  { // coast
    maxMillis: 1000,
    stepMillis: 50,
    factor: 1,
    add: 0,
    minPWM: 0,
    maxPWM: 0,
    startPWM: 0,
    useStartPWM: true,
    brake: true
  },
  { // PWM-brake
    maxMillis: 1000,
    stepMillis: 50,
    factor: 1,
    add: 0,
    minPWM: 1,
    maxPWM: 1,
    startPWM: 1,
    useStartPWM: true,
    brake: true
  },
*/
  { // slow down before end-stop
    maxMillis: 1000,
    stepMillis: 100,
    factor: 0.9,
    add: 0,
    minPWM: 64,
    maxPWM: 255,
    startPWM: 0,
    useStartPWM: false,
    brake: false
  },
  { // seek end-stop
    maxMillis: 5000,
    stepMillis: 100,
    factor: 1,
    add: -1,
    minPWM: 32,
    maxPWM: 64,
    startPWM: 64,
    useStartPWM: true,
    brake: false
  },
  {
    maxMillis: 0
  }
};

const struct accelProfile *accelProfile; // variable pointer to a const struct
int accelProfileIdx = 0;

// switch inputs, active-low
const int switchFront = 3; // front is at the closed position
const int switchFrontIRQ = 1; // TODO: maybe use interrupts & sleep

const int switchBack = 4;

// wire-ORed buttons, active-low
const int switchTrigger = 2;
const int switchTriggerIRQ = 0;


// unsorted variables below. TODO: refactor & cleanup

// buttons & switches do different things in different states
enum doorStates { doorClosed, doorOpening, doorOpen, doorClosing, doorBlocked, doorError } doorState;

double drivePWM = 0;
const double drivePWMscale = 1; // fiddling aid, constant factor applied to calculated PWM value. TODO: update profile & remove

// TODO: the timing system has some quirks. think again or document

unsigned long lastMillis = 0; // last loop's millis() result

unsigned long driveMillis = 0;
unsigned long driveStepMillis = 0;

const unsigned long openInterval = 3000; // ms before the door closes again, unless blocked by button
unsigned long openMillis = 0;

const unsigned long debounceInterval = 2; // shift inputs into debounce register every debounceInterval ms
unsigned long debounceMillis = 0;

// TODO: use structured profiles
const unsigned long ledSolidInterval = 250; // constantly update LED stripe in solid mode
const unsigned long ledFadeInterval = 20;
const unsigned long ledBlinkInterval = 1000;
const unsigned long ledMoveInterval = 250; // shift LED window every X ms
unsigned long ledMillis = 0;

bool swFront, swBack, swTrigger, motorDiagA, motorDiagB; // debounced inputs
uint16_t swFrontDebounce, swBackDebounce, swTriggerDebounce, motorDiagADebounce, motorDiagBDebounce; // debounce shift registers


void setup() {
  Serial.begin(115200);
  Serial.print(F("\nDoor control init\n"));

  // disable driver bridges until setup complete
  motorDisable();

  digitalWrite(motorInAPin, LOW);
  pinMode(motorInAPin, OUTPUT);

  digitalWrite(motorInBPin, LOW);
  pinMode(motorInBPin, OUTPUT);

  digitalWrite(motorPWMPin, LOW);
  pinMode(motorPWMPin, OUTPUT);

// TODO: measure real PWM frequeny. any value other than 1 makes the motor whine
// the code says base frequency is ~31kHz
// VNH3SP30 datasheet limits the input PWM frequency to 10kHz - and yet it works
setPwmFrequency(motorPWMPin, 1);

  pinMode(switchFront, INPUT_PULLUP);
  pinMode(switchBack, INPUT_PULLUP);

  pinMode(switchTrigger, INPUT_PULLUP);

  setLeds1(traktor, ledSolid);
  ledptr = &leds[0];
  LED.setOutput(ledPin);
  LED.sync();

  // initialize debounce registers, assume steady state
  swFrontDebounce = (swFront = digitalRead(switchFront)) * 0xFFFF;
  swBackDebounce = (swBack = digitalRead(switchBack)) * 0xFFFF;
  swTriggerDebounce = (swTrigger = digitalRead(switchTrigger)) * 0xFFFF;

  Serial.print(F("Door status: "));

  if (swTrigger == LOW || (swFront == LOW && swBack == LOW)) {
    doorState = doorError;
    Serial.print(F("ERROR\n"));
  } else if (swFront == LOW) {
    doorState = doorClosed;
    Serial.print(F("closed\n"));
  } else if (swBack == LOW) {
    doorState = doorOpen;
    Serial.print(F("open\n"));
  } else {
    doorState = doorBlocked;
    Serial.print(F("blocked\n"));
  }

  if (doorState != doorError) {
    // enable driver
    motorEnable();

    // diag pull-ups should have brought the lines up by now
    motorDiagADebounce = (motorDiagA = digitalRead(motorDiagAPin)) * 0xFFFF;
    motorDiagBDebounce = (motorDiagB = digitalRead(motorDiagBPin)) * 0xFFFF;

    if (doorState != doorBlocked) {
      motorBrake();
    } else {
      motorFree();
      setLeds1(red, ledSolid);
    }
  } else {
    setLeds1(red, ledBlink);
  }

// TODO: on-board status LEDs, independent of ws2812 stripe
digitalWrite(13, LOW);
pinMode(13, OUTPUT);

//  attachInterrupt(switchFrontIRQ, switchFrontInterrupt, CHANGE);
//  attachInterrupt(switchTriggerIRQ, switchTriggerInterrupt, FALLING);
}

void loop() {
  unsigned long currentMillis;
  unsigned long elapsedMillis;

  currentMillis = millis();
  // millis() will overflow after approximately 50 days.
  if (currentMillis < lastMillis) {
    elapsedMillis = (~0UL - lastMillis) + currentMillis;
  } else {
    elapsedMillis = currentMillis - lastMillis;
  }
  lastMillis = currentMillis;
  // assume that none of the following intervals overflow

/*
  Serial.print(F("Elapsed millis: "));
  Serial.println(elapsedMillis);
*/

  debounceMillis += elapsedMillis;
  if (debounceMillis >= debounceInterval) {
    debounceMillis = 0;

    debounce(switchTrigger, &swTrigger, &swTriggerDebounce);
    debounce(switchFront, &swFront, &swFrontDebounce);
    debounce(switchBack, &swBack, &swBackDebounce);
  }

  // fast debounce driver error signals to filter short glitches
  debounce(motorDiagAPin, &motorDiagA, &motorDiagADebounce);
  debounce(motorDiagBPin, &motorDiagB, &motorDiagBDebounce);

  // The WAM's overheating!
  if ((motorDiagA == LOW || motorDiagB == LOW) && doorState != doorError) {
    motorFree();
    Serial.print(F("Motor driver error condition - door disabled\n"));
    setLeds1(red, ledFade);
    doorState = doorError;
  }

  switch(doorState) {
    case doorClosed:
      // wait until a button is pressed or the front switch opens
      if (swFront == HIGH || swTrigger == LOW) {
        if (swFront == HIGH) {
          // maybe someone tried to pull the door open
          // TODO: or the door slams too fast into the end-stop and rebounds. align profile & inertial reality
          Serial.print(F("Front switch triggered - opening door\n"));
          initDrive(backward, accelProfileLow);
        } else {
          Serial.print(F("Button switch triggered - opening door\n"));
          initDrive(backward, accelProfileHigh);
// TODO: think again. re-arming swTrigger disables deglitching
swTrigger = HIGH;
        }
        setLeds2(white, black, 3, ledBackward);
        doorState = doorOpening;
      }
      break;

    case doorOpen:
      // hold the door open if button pressed during openInterval
      if (swTrigger == LOW) {
        Serial.print(F("Button switch triggered - door blocked\n"));
        motorFree();
        setLeds1(red, ledSolid);
        doorState = doorBlocked;
swTrigger = HIGH;
        break;
      }

      openMillis += elapsedMillis;
      if (openMillis >= openInterval || swBack == HIGH) {
        if (swBack == HIGH) {
          // maybe someone tried to pull the door close
          // TODO: or the door slams too fast into the end-stop and rebounds. align profile & inertial reality
          Serial.print(F("Back switch triggered - closing door\n"));
          initDrive(forward, accelProfileLow);
        } else {
          Serial.print(F("openInterval timeout - closing door\n"));
          initDrive(forward, accelProfileHigh);
        }
        setLeds2(white, black, 3, ledForward);
        doorState = doorClosing;
      }
      break;

    case doorOpening:
      // are we there yet?
      if (swBack == LOW) {
        motorBrake();
        Serial.print(F("Back switch triggered - door open\n"));
        openMillis = 0;
        setLeds1(green, ledFade);
        doorState = doorOpen;
        break;
      }
      // fall-through to next case

    case doorClosing:
      if (doorState == doorClosing && swFront == LOW) {
        motorBrake();
        Serial.print(F("Front switch triggered - door closed\n"));
        setLeds1(green, ledFade);
        doorState = doorClosed;
        break;
      }

      // stop if button pressed while in motion
      if (swTrigger == LOW) {
        motorFree(); // TODO: motorBrake() might be better for an emergency stop. doesn't make much different in our case, the belt skips and the door stops in both cases
        Serial.print(F("Button switch triggered - door blocked\n"));
        setLeds1(red, ledSolid);
        doorState = doorBlocked;
swTrigger = HIGH;
        break;
      }

      driveStepMillis += elapsedMillis;
      if (driveStepMillis >= accelProfile[accelProfileIdx].stepMillis) {
        driveMillis += driveStepMillis;
        driveStepMillis = 0;

        // switch to next profile step
        if (driveMillis >= accelProfile[accelProfileIdx].maxMillis) {
          driveMillis = 0;

          // end-stop not reached at end of profile, someone might be in the way
          if (accelProfile[++accelProfileIdx].maxMillis == 0) {
            motorFree();
            Serial.print(F("End of profile - door blocked\n"));
            setLeds1(red, ledBlink);
            doorState = doorBlocked;
            break;
          }

          if (accelProfile[accelProfileIdx].brake) {
            // short motor if PWM is HIGH, coast otherwise
            _motorFree();
          } else {
            switch(motorDir) {
              case forward:
                motorForward(); break;
              case backward:
                motorBackward(); break;
            }
          }

          // overwrite current PWM value
          if (accelProfile[accelProfileIdx].useStartPWM) {
            drivePWM = accelProfile[accelProfileIdx].startPWM;
          }

          Serial.print(F("accelProfileIdx: "));
          Serial.println(accelProfileIdx);
        }

        // update PWM value
        drivePWM = min(accelProfile[accelProfileIdx].maxPWM, max(accelProfile[accelProfileIdx].minPWM,
                        (drivePWM + accelProfile[accelProfileIdx].add) * accelProfile[accelProfileIdx].factor));

        Serial.print(F("Drive PWM: "));
        Serial.println(drivePWM);

        analogWrite(motorPWMPin, round(drivePWM*drivePWMscale));
      }
      break;

    case doorBlocked:
      // wait until a button is pressed
      if (swTrigger == LOW) {
        // close door only if end-stop is active
        if (swBack == LOW) {
          Serial.print(F("Button switch triggered - closing door\n"));
          initDrive(forward, accelProfileLow);
          ledMode = ledForward;
          doorState = doorClosing;
        } else {
          Serial.print(F("Button switch triggered - opening door\n"));
          initDrive(backward, accelProfileLow);
          ledMode = ledBackward;
          doorState = doorOpening;
        }
        setLeds2(white, black, 3, ledMode);
swTrigger = HIGH;
      }
      break;

    case doorError:
      // wait for reset by qualified service technician
      // TODO: make it user-resettable
/*
// wait until driver cools down
// TODO: this won't work now because motorDisable() pulls the pins low
if (motorDiagA == HIGH && motorDiagB == HIGH) {
  motorEnable();
  setLeds1(red, ledSolid);
  doorState = doorBlocked;
}
*/
digitalWrite(13, HIGH);
      break;
  }


  ledMillis += elapsedMillis;
  switch(ledMode) {
    case ledSolid:
      if (ledMillis >= ledSolidInterval) {
        ledMillis = 0;
        ledptr = &leds[0];
        LED.sync();
      }
      break;

    case ledFade: {
      if (ledMillis >= ledFadeInterval) {
        ledMillis = 0;

        // TODO: really scale the values. this is too cheap
        for (uint8_t i = 0; i < MAXPIX; i++) {
          tmp_leds[i].r = max(0, (int)leds[i].r - abs(ledState.fader));
          tmp_leds[i].g = max(0, (int)leds[i].g - abs(ledState.fader));
          tmp_leds[i].b = max(0, (int)leds[i].b - abs(ledState.fader));
        }

        if (ledState.fader++ >= 255) {
          ledState.fader = -254;
        }

        ledptr = &tmp_leds[0];
        LED.sync();
      }
      break;
    }

    case ledBlink:
      if (ledMillis >= ledBlinkInterval) {
        ledMillis = 0;

        if (!ledState.blink_on) {
  //          memset(&tmp_leds, 0x00, sizeof(tmp_leds));
          for (uint8_t i = 0; i < MAXPIX; i++) {
            tmp_leds[i] = black;
          }
          ledptr = &tmp_leds[0];
        } else {
          ledptr = &leds[0];
        }
  
        ledState.blink_on = !ledState.blink_on;

        LED.sync();
      }
      break;

    case ledBackward:
      if (ledMillis >= ledMoveInterval) {
        ledMillis = 0;

        if (ledState.ledidx >= EXTRAPIX) {
          ledState.ledidx = 0;
        }
  
        // move window forward
        ledptr = &leds[ledState.ledidx++];
        LED.sync();
      }
      break;

    case ledForward:
      if (ledMillis >= ledMoveInterval) {
        ledMillis = 0;

        if (ledState.ledidx == 0) {
          ledState.ledidx = EXTRAPIX;
        }
  
        // move window backwards
        ledptr = &leds[--ledState.ledidx];
        LED.sync();
      }
      break;
  }
}

//__attribute__((always_inline))
inline void debounce(const int swPin, bool *swVal, uint16_t *swDebounce) {
  // similiar to what digitalRead() does, assumes that swPin has no timer output assigned
  // this still leads to several indirect lookups per sample. direct PINx access might be faster
  debounce(portInputRegister(digitalPinToPort(swPin)), digitalPinToBitMask(swPin), swVal, swDebounce);
/*
  *swDebounce = ((*swDebounce << 1) | digitalRead(swPin)) & 0x1FFF; // 13-bit debounce
*/
}

//__attribute__((always_inline))
inline void debounce(const volatile uint8_t *port, const uint8_t pinMask, bool *swVal, uint16_t *swDebounce) {
  // NB: interference near multiples of 1/interval could cause spurious edges
  *swDebounce = ((*swDebounce << 1) | ((*port & pinMask)?(1):(0))) & 0x1FFF; // 13-bit debounce
  if (*swDebounce == 0x1000) { // edge HIGH -> 12xLOW
    *swVal = LOW;
  } else if (*swDebounce == 0x0FFF) { // edge LOW -> 12xHIGH
    *swVal = HIGH;
  }
}

// set profile pointer, reset counters, set driver inputs
// TODO: understand & fix enum namespace fnord
inline void initDrive(const int dir, const struct accelProfile *profile) {
  motorDir = (enum motorDir)dir;

  accelProfile = profile;
  accelProfileIdx = 0;
  drivePWM = accelProfile[0].startPWM;

  driveMillis = driveStepMillis = 0;

  switch(dir) {
    case forward:
      motorForward(); break;
    case backward:
      motorBackward(); break;
  }
}

// functions beginning with _motor don't output to Serial, used in early interrupt version
// TODO: either use them with interrupts or remove

inline void setMotorBits(const byte bits) {
  digitalWrite(motorPWMPin, LOW);
  digitalWrite(motorInAPin, (bits & 0x02) ? (HIGH) : (LOW));
  digitalWrite(motorInBPin, (bits & 0x01) ? (HIGH) : (LOW));
}

void motorDisable() {
  // actively pull diag pins low, disables driver bridges
  digitalWrite(motorDiagAPin, LOW);
  pinMode(motorDiagAPin, OUTPUT);

  digitalWrite(motorDiagBPin, LOW);
  pinMode(motorDiagBPin, OUTPUT);

  Serial.print(F("Motor driver disabled\n"));
}

void motorEnable() {
  // return control to pull-ups on the BB-VNH3SP30 board
  pinMode(motorDiagAPin, INPUT);
  pinMode(motorDiagBPin, INPUT);
  Serial.print(F("Motor driver enabled\n"));
}

void motorBrake() {
  _motorBrake();
  Serial.print(F("Brake engaged\n"));
}

void _motorBrake() {
  setMotorBits(0x00);
/*
  digitalWrite(motorInAPin, LOW);
  digitalWrite(motorInBPin, LOW);
*/
  digitalWrite(motorPWMPin, HIGH);
}

void motorFree() {
  _motorFree();
  Serial.print(F("Brake disengaged\n"));
}

void _motorFree() {
  setMotorBits(0x00);
/*
  digitalWrite(motorPWMPin, LOW);
  digitalWrite(motorInAPin, LOW);
  digitalWrite(motorInBPin, LOW);
*/
}

void motorForward() {
  _motorForward();
  Serial.print(F("Forward\n"));
}

void _motorForward() {
  setMotorBits(0x02);
/*
  digitalWrite(motorPWMPin, LOW);
  digitalWrite(motorInAPin, HIGH);
  digitalWrite(motorInBPin, LOW);
*/
}

void motorBackward() {
  _motorBackward();
  Serial.print(F("Backward\n"));
}

void _motorBackward() {
  setMotorBits(0x01);
/*
  digitalWrite(motorPWMPin, LOW);
  digitalWrite(motorInAPin, LOW);
  digitalWrite(motorInBPin, HIGH);
*/
}


// copy&pasted from http://playground.arduino.cc/Code/PwmFrequency

/**
 * Divides a given PWM pin frequency by a divisor.
 *
 * The resulting frequency is equal to the base frequency divided by
 * the given divisor:
 *   - Base frequencies:
 *      o The base frequency for pins 3, 9, 10, and 11 is 31250 Hz.
 *      o The base frequency for pins 5 and 6 is 62500 Hz.
 *   - Divisors:
 *      o The divisors available on pins 5, 6, 9 and 10 are: 1, 8, 64,
 *        256, and 1024.
 *      o The divisors available on pins 3 and 11 are: 1, 8, 32, 64,
 *        128, 256, and 1024.
 *
 * PWM frequencies are tied together in pairs of pins. If one in a
 * pair is changed, the other is also changed to match:
 *   - Pins 5 and 6 are paired on timer0
 *   - Pins 9 and 10 are paired on timer1
 *   - Pins 3 and 11 are paired on timer2
 *
 * Note that this function will have side effects on anything else
 * that uses timers:
 *   - Changes on pins 3, 5, 6, or 11 may cause the delay() and
 *     millis() functions to stop working. Other timing-related
 *     functions may also be affected.
 *   - Changes on pins 9 or 10 will cause the Servo library to function
 *     incorrectly.
 *
 * Thanks to macegr of the Arduino forums for his documentation of the
 * PWM frequency divisors. His post can be viewed at:
 *   http://forum.arduino.cc/index.php?topic=16612#msg121031
 */
void setPwmFrequency(int pin, int divisor) {
  byte mode;
  if(pin == 5 || pin == 6 || pin == 9 || pin == 10) {
    switch(divisor) {
      case 1: mode = 0x01; break;
      case 8: mode = 0x02; break;
      case 64: mode = 0x03; break;
      case 256: mode = 0x04; break;
      case 1024: mode = 0x05; break;
      default: return;
    }
    if(pin == 5 || pin == 6) {
      TCCR0B = TCCR0B & 0b11111000 | mode;
    } else {
      TCCR1B = TCCR1B & 0b11111000 | mode;
    }
  } else if(pin == 3 || pin == 11) {
    switch(divisor) {
      case 1: mode = 0x01; break;
      case 8: mode = 0x02; break;
      case 32: mode = 0x03; break;
      case 64: mode = 0x04; break;
      case 128: mode = 0x05; break;
      case 256: mode = 0x06; break;
      case 1024: mode = 0x7; break;
      default: return;
    }
    TCCR2B = TCCR2B & 0b11111000 | mode;
  }
}
