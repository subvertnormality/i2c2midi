// January 30, 2022 
// v2_7
// https://github.com/attowatt/i2c2midi

// -----------------------------------------------------------------
// clock implementation via https://github.com/taubaland
// -----------------------------------------------------------------



#include <i2c_t3.h>
#include <MIDI.h>

// using https://github.com/rlogiacco/CircularBuffer
#include <CircularBuffer.h>

// using https://github.com/midilab/uClock
#include <uClock.h>

#define MIDI_CLOCK 0xF8
#define MIDI_START 0xFA
#define MIDI_STOP  0xFC

// DEBUG
// Uncomment this to see i2c messages etc. in the serial monitor:
 #define DEBUG      

// USB MIDI
// This is for your own modifications or hacks. Don't forget to select 'Tools' -> 'USB Type' -> 'MIDI + Serial' when uploading to the Teensy.
// uncomment this to use USB MIDI over Teensy's USB port:
// #define USB_MIDI

// I2C
#define MEM_LEN 256
uint8_t databuf[MEM_LEN];
volatile uint8_t received;
void receiveEvent(size_t count);

// MIDI
MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI);

// Values init
unsigned long notes[16][8][4];    // array to store the note information: pitch, start time, duration, currently on/off
int noteCount[16];
int currentNote[16];
int noteDuration = 300;           // default note duration
int maxNotes = 8;                 // polyphony
int numChannels = 16;
int lastChannel = 1;
int led1 = 2;
int led2 = 3;
unsigned long lastLEDMillis1 = 0;
unsigned long lastLEDMillis2 = 0;
int animationSpeed = 100;

#define NUMBER_OF_TAPS 3

CircularBuffer<float, NUMBER_OF_TAPS> tapBuffer;

float sum = 0;
float avg = 0;
int lastTap = 0;
int currentTap = millis();
float tempo = 120.0;

void setup() {

  pinMode(led1,OUTPUT); 
  pinMode(led2,OUTPUT); 

  // setup for Slave mode, address (= 66), pins 18/19, external pullups, 400kHz
  Wire.begin(I2C_SLAVE, 0x42, I2C_PINS_18_19, I2C_PULLUP_EXT, 400000);

  // data init
  received = 0;
  memset(databuf, 0, sizeof(databuf));
    
  // register events
  Wire.onReceive(receiveEvent);
  Serial.begin(115200);

  // Start MIDI
  MIDI.begin();
  
  // start up animation
  for (int i=0; i < 4; i++) {
    digitalWrite(led1,HIGH); delay(animationSpeed);
    digitalWrite(led2,HIGH); delay(animationSpeed);
    digitalWrite(led1,LOW); delay(animationSpeed);
    digitalWrite(led2,LOW); delay(animationSpeed);
  }
  
  Serial.println("started");

  // Inits the clock
  uClock.init();
  // Set the callback function for the clock output to send MIDI Sync message.
  uClock.setClock96PPQNOutput(ClockOut96PPQN);
  // Set the callback function for MIDI Start and Stop messages.
  uClock.setOnClockStartOutput(onClockStart);  
  uClock.setOnClockStopOutput(onClockStop);
  // Set the clock BPM to 120 BPM
  uClock.setTempo(tempo);

  // Starts the clock, tick-tac-tick-tac...
  uClock.start();
 
}

void loop() {
  
  if(received) {

    blinkLED(1);

    #ifdef DEBUG
      Serial.print("DATABUF 0: "); Serial.println(databuf[0]);
      Serial.print("DATABUF 1: "); Serial.println(databuf[1]);
      Serial.print("DATABUF 2: "); Serial.println(databuf[2]);
    #endif

    // Send Midi Notes and CC
    // This part is set up to react to I2C messages from Teletype using the Disting EX MIDI OPs. 
    // See https://github.com/scanner-darkly/teletype/wiki/DISTING-EX-I2C-SPECIFICATION or the Disting Ex manual. 
    // MIDI messages have the command 0x4F == 79. 
    // The messages also include a status. See https://www.midimountain.com/midi/midi_status.htm.
    
    if (databuf[0] == 79) {

      // NOTE ON messages have status 144-159 for MIDI channels 1-16
      // EX.M.N note velocity
      if (databuf[1] >= 144 && databuf[1] <= 159) {
        midiNoteOn(databuf[2], noteDuration, databuf[3], databuf[1]-144);
        lastChannel = databuf[1]-144;
      }  
      
      // CONTROL CHANGE messages have status 176-191 for MIDI channels 1-16.
      // EX.M.CC controller value
      if (databuf[1] >= 176 && databuf[1] <= 191) {
        sendMidiCc(databuf[2], databuf[3], databuf[1]-176);
        lastChannel = databuf[1]-176;
      }

      // PROGRAM CHANGE messages have status 192-207 for MIDI channels 1-16
      // EX.M.PRG program
      if (databuf[1] >= 192 && databuf[1] <= 207) {
        sendMidiProgramChange(databuf[2], databuf[1]-192);
        lastChannel = databuf[1]-192;
      }
      
      // PITCH BEND messages have status 224-239 for MIDI channels 1-16
      // EX.M.PB pitchbend (min: -8192, max: 8191)
      if (databuf[1] >= 224 && databuf[1] <= 239) {
        sendMidiPitchBend(databuf[2], databuf[3], databuf[1]-224);
        lastChannel = databuf[1]-224;
      }

      // CLOCK messages have the same status for all MIDI channels 1-16
      // EX.M.CLK
      if (databuf[2] == 248) {

        tap();
      }

      // EX.M.START
      if (databuf[2] == 250) {
        MIDI.sendRealTime(midi::Start);
        #ifdef USB_MIDI
          usbMIDI.sendRealTime(usbMIDI.Start);
        #endif
        blinkLED(2);
      }
      // EX.M.CONT
      if (databuf[2] == 251) {
        MIDI.sendRealTime(midi::Continue);
        #ifdef USB_MIDI
          usbMIDI.sendRealTime(usbMIDI.Continue);
        #endif
        blinkLED(2);
      }
      // EX.M.STOP
      if (databuf[2] == 252) {
        MIDI.sendRealTime(midi::Stop);
        #ifdef USB_MIDI
          usbMIDI.sendRealTime(usbMIDI.Stop);
        #endif
        blinkLED(2);
      }

    }

    // GENERIC parameters (0x46 == 70)
    if (databuf[0] == 70) {       
      int value = (int16_t)(databuf[2] << 8 | databuf[3]);  
      
      // set note duration
      // EX.P 1
      if (databuf[1] == 1) {   
        noteDuration = value;
        blinkLED(2);
      }
      
      // send aftertouch 
      // EX.P 2
      if (databuf[1] == 2) {                                
        MIDI.sendAfterTouch(constrain(value, 0, 127), lastChannel+1);   
        #ifdef USB_MIDI
          usbMIDI.sendAfterTouch(constrain(value, 0, 127), lastChannel+1);
        #endif
        blinkLED(2);                          
      }

      // set I2C address (99)
      if (databuf[1] == 99) {           
        if (value == 65) {
          Wire.begin(I2C_SLAVE, 0x41, I2C_PINS_18_19, I2C_PULLUP_EXT, 400000);    
        }
        if (value == 66) {
          Wire.begin(I2C_SLAVE, 0x42, I2C_PINS_18_19, I2C_PULLUP_EXT, 400000);    
        }
        if (value == 67) {
          Wire.begin(I2C_SLAVE, 0x43, I2C_PINS_18_19, I2C_PULLUP_EXT, 400000);    
        }
        if (value == 68) {
          Wire.begin(I2C_SLAVE, 0x44, I2C_PINS_18_19, I2C_PULLUP_EXT, 400000);    
        }
        blinkLED(2);                          
      }
    }

   received = false;
  
  }

  checkNoteDurations();       // check if there are notes to turn off
  checkLEDs();                // check if the LEDs should be turned off

}

void tap(){
  
  currentTap = millis() - lastTap;
  lastTap = millis();

  sum += currentTap;

  if (tapBuffer.isFull())
  {
    sum = sum - tapBuffer.first();
    tapBuffer.shift();  
  }

  tapBuffer.push(currentTap);
  avg = sum / NUMBER_OF_TAPS;

  tempo = 60000.0 / avg; // * 2 for M as 1/2 note, / 2 for M as 1/8 note

  Serial.println("Current Tap: ");
  Serial.println(currentTap);

  Serial.println("Average Tap: ");
  Serial.println(avg);

  Serial.println("Tempo: ");
  Serial.println(tempo);

  uClock.setTempo(tempo);
}


void ClockOut96PPQN(uint32_t * tick) {
  // Send MIDI_CLOCK to external gears
  sendClockPulse();
}

void onClockStart() {
  Serial.write(MIDI_START);
}

// The callback function wich will be called when clock stops by using Clock.stop() method.
void onClockStop() {
  Serial.write(MIDI_STOP);
}


//function for sending clock pulses
void sendClockPulse(){
        MIDI.sendRealTime(midi::Clock);
        #ifdef USB_MIDI
          usbMIDI.sendRealTime(usbMIDI.Clock);
        #endif
}

// function for receiving I2C messages
void receiveEvent(size_t count) {
  if(count<MEM_LEN) {
    // copy Rx data to databuf
    Wire.read(databuf, count);
    // set received flag to count, this triggers main loop  
    received += count;           
  }
}


// function for sending MIDI Note On
void midiNoteOn(int pitch, int noteDuration, int velocity, int channel) {

  // check if this note is already playing; if yes, send note off message and play again
  for (int i=0; i < maxNotes; i++) {
    if (notes[channel][i][0] == pitch && notes[channel][i][3] == 1) {
      #ifdef DEBUG
        Serial.println("Note is already playing"); 
      #endif
      MIDI.sendNoteOff(notes[channel][i][0], 0, channel+1);
      #ifdef USB_MIDI 
        usbMIDI.sendNoteOff(pitch, 0, channel+1);
      #endif
      digitalWrite(led2,LOW);
      notes[channel][i][3] = 0;
    }
  }
    
  noteCount[channel] += 1;                                  // count one note up
  currentNote[channel] = noteCount[channel] % maxNotes;     // determine the current note number
  
  // check if next note number is still playing; if yes, skip to next note number; 
  // if there's no more space available, replace the note
  for (int i=0; i < maxNotes; i++) {                        
    if (notes[channel][currentNote[channel]][3] == 1) {
      noteCount[channel] += 1; // count one note up
      currentNote[channel] = noteCount[channel] % maxNotes;
    }
    else {
      break;
    }
  }
  
  // store the values for the note in the notes array
  notes[channel][currentNote[channel]][0] = pitch;          // pitch
  notes[channel][currentNote[channel]][1] = millis();       // note start time
  notes[channel][currentNote[channel]][2] = noteDuration;   // note duration
  notes[channel][currentNote[channel]][3] = 1;              // note is on

  #ifdef DEBUG
    Serial.print(currentNote[channel]); 
    Serial.print("Note on: ");
    Serial.println(notes[channel][currentNote[channel]][0]);
  #endif
  
  MIDI.sendNoteOn(pitch, velocity, channel+1);
  #ifdef USB_MIDI 
    usbMIDI.sendNoteOn(pitch, velocity, channel+1);
  #endif
  blinkLED(2);
}


// function for sending MIDI Note Off 
void midiNoteOff(int pitch, int channel) {
    MIDI.sendNoteOff(pitch, 0, channel+1);
    #ifdef USB_MIDI 
      usbMIDI.sendNoteOff(pitch, 0, channel+1);
    #endif
    blinkLED(2);
}


// function for handling Note Offs
void checkNoteDurations() {
  unsigned long currentTime = millis();
  for (int j=0; j < numChannels; j++) {
    for (int i=0; i < maxNotes; i++) {
      if (notes[j][i][3] != 0) {
        if (currentTime - notes[j][i][1] > notes[j][i][2]) {
          midiNoteOff(notes[j][i][0], j);
          notes[j][i][3] = 0;  
        }  
      } 
    }
  }   
}


// function for sending MIDI CCs
void sendMidiCc(int controller, int value, int channel){
  MIDI.sendControlChange(controller, value, channel+1);
  #ifdef USB_MIDI 
    usbMIDI.sendControlChange(controller, value, channel+1);
  #endif
  blinkLED(2);
}


// function for sending MIDI Program Changes
void sendMidiProgramChange(int programNumber, int channel){
  MIDI.sendProgramChange(programNumber, channel+1);
  #ifdef USB_MIDI 
    usbMIDI.sendProgramChange(programNumber, channel+1);
  #endif
  blinkLED(2);
}


// function for sending MIDI Pitch Bend
void sendMidiPitchBend(int MSB, int LSB, int channel){
  int value = (int16_t)(LSB << 8 | MSB);    // it seems LSB and MSB are mixed-up?
  //int value2 = (8191./127.)*value;
  MIDI.sendPitchBend(value, channel+1);
  #ifdef USB_MIDI 
    usbMIDI.sendPitchBend(value, channel+1);
  #endif
  blinkLED(2);
}


// function for turning on the LEDs
void blinkLED(int led) {
  if (led == 1) {
    digitalWrite(led1,HIGH);
    lastLEDMillis1 = millis();
  }
  if (led == 2) {
    digitalWrite(led2,HIGH);
    lastLEDMillis2 = millis();
  }
}


// function for turning off the LEDs
void checkLEDs() {
  unsigned long currentMillis = millis();
  int LEDBlinkLength = 50;
  if (currentMillis - lastLEDMillis1 >= LEDBlinkLength) {
    digitalWrite(led1,LOW);
  }
  if (currentMillis - lastLEDMillis2 >= LEDBlinkLength) {
    digitalWrite(led2,LOW);
  }
}