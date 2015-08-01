#include "application.h"
#include "ac_display_reader.h"
#include "ac_display_reader_p.h"

// Global config
int clockPin;
int inputPin;

// State tracking
AcModels acModel = V1_2;
char statusJson[sizeof(STATUS_TEMPLATE) * 2];
char registerData[(BUFFER_LEN * 3) + 1];

// Variables used by the ISR to track the shift register
unsigned int cycleStart = 0;
volatile uint8_t byteBuffer[BUFFER_LEN];
volatile uint8_t* currentByte = byteBuffer;


void initAcDisplayReader(int cp, int ip, String statusVar, String dataVar, String setAcModelFuncName) {
  clockPin = cp;
  inputPin = ip;

  pinMode(clockPin, INPUT);
  pinMode(inputPin, INPUT);

  // Register display status variables
  Spark.variable(statusVar, &statusJson, STRING);
  Spark.variable(dataVar, &registerData, STRING);

  // Register control functions
  Spark.function(setAcModelFuncName, setAcModel);

  // Setup interrupt handler on rising edge of the register clock
  attachInterrupt(clockPin, clock_Interrupt_Handler, RISING);
}

/**
 * Spark Function letting me switch AC Models while running
 * maybe read/write this in EEPROM
 *
 * http://docs.particle.io/core/firmware/#other-functions-eeprom
 */
int setAcModel(String acModelName) {
  if (acModelName == "V1_2") {
    acModel = V1_2;
    return 12;
  } else {
    // Default to V1_4
    acModel = V1_4;
    return 14;
  }
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
  /*if ((GPIOB->IDR) & 0b01000000) {*/
  // Using digitalRead for core/photon cross compatibility
  if (digitalRead(inputPin) == HIGH) {
    *currentByte |= 1;
  }
}
 
