/**
 * Code to read the data being sent to a shift register and decode it. In this case decoding the
 * display status of a Frigidaire AC unit.
 *
 * See for context:
 * https://community.particle.io/t/reading-digits-from-dual-7-segment-10-pin-display/12989
 */

#define CLOCK_PIN D0
#define INPUT_PIN D1

// The shift register sees 6 bytes repeatedly
// Keep the last 4 status codes
#define BUFFER_LEN 6 * 4
#define UPDATE_TIME_MAX 500 // max time in micros the AC controller spends pushing data into the register

unsigned int cycleStart = 0;
volatile uint8_t byteBuffer[BUFFER_LEN];
volatile uint8_t* currentByte = byteBuffer;

long lastUpdate = 0;
long lastMessage = 0;
double tempDisplay = 0;
char fanSpeed[2];
char acMode[2];

void setup() {
  Serial.begin(115200); //change BAUD rate as required

  pinMode(CLOCK_PIN, INPUT);
  pinMode(INPUT_PIN, INPUT);

  // Register display status variables
  Spark.variable("temp", &tempDisplay, DOUBLE);
  Spark.variable("fan", &fanSpeed, STRING);
  Spark.variable("mode", &acMode, STRING);

  attachInterrupt(CLOCK_PIN, clock_Interrupt_Handler, RISING); //set up ISR for receiving shift register clock signal
}

/**
 * Decode the number bits that control the 8 segment display
 */
int decodeDigit(uint8_t digitBits) {
  // Ensure the top bit is true to make the switch work no matter the state of the decimal place
  uint8_t decimalMasked = digitBits | 0x80;
  //Serial.print("masked: ");
  //Serial.print(digitBits, HEX);
  //Serial.print(" to ");
  //Serial.print(decimalMasked, HEX);
  //Serial.println();
  switch (decimalMasked) {
    case 0x90:
      return 9;
    case 0x80:
      return 8;
    case 0xF8:
      return 7;
    case 0x82:
      return 6;
    case 0x92:
      return 5;
    case 0x99:
      return 4;
    case 0xB0:
      return 3;
    case 0xA4:
      return 2;
    case 0xF9:
      return 1;
    case 0xC0:
      return 0;
    default:
      return -1;
  }
}

double decodeDisplayNumber(uint8_t tensBits, uint8_t onesBits) {
  int tens = decodeDigit(tensBits);
  //Serial.print("Decoded: ");
  //Serial.print(tensBits, HEX);
  //Serial.print(" to ");
  //Serial.print(tens);
  //Serial.println();

  int ones = decodeDigit(onesBits);
  //Serial.print("Decoded: ");
  //Serial.print(onesBits, HEX);
  //Serial.print(" to ");
  //Serial.print(ones);
  //Serial.println();

  // One of the digits must be invalid, return -1
  if (tens == -1 || ones == -1) {
    return -1;
  }

  if ((tensBits & 0x80) == 0x80) {
    // If the top bit of the tens digit is set the display is an integer between 00 and 99
    return (tens * 10) + ones;
  } else {
    // If the top bit of the tens digit is not set the display is a decimal between 0.0 and 9.9
    return tens + (ones / 10.0);
  }
}

String decodeFanSpeed(uint8_t modeFanBits) {
  Serial.print("dc_fs: ");
  Serial.print(modeFanBits, HEX);

  uint8_t fanBitsMasked = modeFanBits & 0x0F;
  Serial.print(", mb: ");
  Serial.print(fanBitsMasked, HEX);

  switch (fanBitsMasked) {
    case 0x07:
      Serial.println(" L");
      return "L";
    case 0x0B:
      Serial.println(" H");
      return "H";
    case 0x0D:
      Serial.println(" A");
      return "A";
    default:
      Serial.println(" DEFAULT");
      return "";
  }
}

String decodeAcMode(uint8_t modeFanBits) {
  Serial.print("dc_am: ");
  Serial.print(modeFanBits, HEX);

  uint8_t modeBitsMasked = modeFanBits & 0xF0;
  Serial.print(", mb: ");
  Serial.print(modeBitsMasked, HEX);

  switch (modeBitsMasked) {
    case 0xB0:
      Serial.println(" L");
      return "F";
    case 0xD0:
      Serial.println(" E");
      return "E";
    case 0xE0:
      Serial.println(" C");
      return "C";
    default:
      Serial.println(" DEFAULT");
      return "";
  }
}

void updateState(double temp, String fan, String mode) {
  Serial.println("Updating State");

  String fanSpeedStr(fanSpeed);
  String acModeStr(acMode);

  // Record if any of the data changed, used to decide if an event should be published
  bool changed = temp != tempDisplay || fan != fanSpeedStr || mode != acModeStr;

  // All the data is good, update the published variables
  tempDisplay = temp;
  fan.toCharArray(fanSpeed, fan.length() + 1);
  mode.toCharArray(acMode, mode.length() + 1);
  lastUpdate = millis();

  if (changed || lastUpdate - lastMessage > 300000) {
    Serial.println("State Changed");
    char message[80];

    sprintf(message, "{\"temp\":%f,\"fan\":\"%s\",\"mode\":\"%s\"}", tempDisplay, fanSpeed, acMode);
    Serial.println(message);
    Spark.publish("STATE_CHANGED", message);
    lastMessage = lastUpdate;
  }
}

void loop() {
  uint8_t readBuffer[BUFFER_LEN];

  // Copy the volatile byteBuffer to a local buffer to minimize the time that interrupts are off
  noInterrupts();
  memcpy(readBuffer, (const uint8_t*) byteBuffer, BUFFER_LEN);
  interrupts();


  // Debugging that dumps the whole buffer out via an event
  /*String state;
  state.reserve(BUFFER_LEN * 3);
  for (int i = 0; i < BUFFER_LEN; i++) {
    String byteHex(readBuffer[i], HEX);
    byteHex.toUpperCase();
    state += byteHex;
    state += " ";
  }
  Spark.publish("BUFFER_READ", state);*/

  // Dump the input data
  for (int i = 0; i < BUFFER_LEN - 6; i++) {
    // Look for the header bytes
    if (readBuffer[i] == 0x7F && readBuffer[i+1] == 0x7F) {
      if (readBuffer[i+2] == 0xFF && readBuffer[i+3] == 0xFF && readBuffer[i+4] == 0xFF && readBuffer[i+5] == 0xFF) {
        // If the next 4 bytes are FF the unit is off
        updateState(0, "X", "X");
        break; // Read a successfull data frame, stop looping
      } else if (readBuffer[i+5] == 0xFD) {
        // If the last byte is FD we should have a 6 byte status
        double td = decodeDisplayNumber(readBuffer[i+2], readBuffer[i+3]);
        Serial.println(td);
        if (td == -1) {
          // If decoded number is -1 then the data must be bad, give up and let the loop keep iterating
          continue;
        }

        String fs = decodeFanSpeed(readBuffer[i+4]);
        Serial.println(fs);
        if (fs == "") {
          // If decoded String is "" then the data must be bad, give up and let the loop keep iterating
          continue;
        }

        String am = decodeAcMode(readBuffer[i+4]);
        Serial.println(am);
        if (am == "") {
          // If decoded String is "" then the data must be bad, give up and let the loop keep iterating
          continue;
        }

        updateState(td, fs, am);
        break; // Read a successfull data frame, stop looping
      }
    }
  }

  // If no update for 2 minutes complain
  if (millis() - lastUpdate > 120000) {
    Spark.publish("STATE_STALE", "TODO");
    lastUpdate += 30000; // wait 30s before complaining again
  }
  /*//Serial.println();*/


  delay(5000); // pause 5s
}

void clock_Interrupt_Handler() {
  // Track the start of each cycle
  // zero out the currentByte to start accumulating data
  int now = micros();
  if (cycleStart == 0 || (now - cycleStart) > UPDATE_TIME_MAX) {
    if (cycleStart != 0) {
      // New cycle means go to the next byte
      currentByte++;

      // Roll over to the start of the buffer once BUFFER_LEN is reached
      if (currentByte == (byteBuffer + BUFFER_LEN)) {
        currentByte = byteBuffer;
      }
    }

    // Record the update cycle start time for the current byte
    cycleStart = now;

    // Zero the current byte, some cycles don't push a full 8 bits into the register
    *currentByte = 0;
  }

  // On clock rise shift the data in the register by one
  *currentByte <<= 1;

  // If the input is high set the new bit to 1 otherwise leave it zero
  // faster way of reading pin D1: https://community.particle.io/t/reading-digits-from-dual-7-segment-10-pin-display/12989/6
  if ((GPIOB->IDR) & 0b01000000) {
    *currentByte |= 1;
  }
}
 
