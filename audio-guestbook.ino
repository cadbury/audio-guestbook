/**
 * Audio Guestbook, Copyright (c) 2022 Playful Technology
 * 
 * Tested using a Teensy 4.0 with Teensy Audio Shield, although should work 
 * with minor modifications on other similar hardware
 * 
 * When handset is lifted, a pre-recorded greeting message is played, followed by a tone.
 * Then, recording starts, and continues until the handset is replaced.
 * Playback button allows all messages currently saved on SD card through earpiece 
 * 
 * Files are saved on SD card as 44.1kHz, 16-bit, mono signed integer RAW audio format 
 * --> changed this to WAV recording, DD4WH 2022_07_31
 * --> added MTP support, which enables copying WAV files from the SD card via the USB connection, DD4WH 2022_08_01
 * 
 * 
 * Frank DD4WH, August 1st 2022 
 * for a DBP 611 telephone (closed contact when handheld is lifted) & with recording to WAV file
 * contact for switch button 0 is closed when handheld is lifted
 * 
 * GNU GPL v3.0 license
 * 
 */

#include <Bounce.h>
#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <TimeLib.h>
#include <MTP_Teensy.h>
#include "play_sd_wav.h"  // local copy with fixes
#include <Keypad.h>

// DEFINES
// Define pins used by Teensy Audio Shield
#define SDCARD_CS_PIN 10
#define SDCARD_MOSI_PIN 11
#define SDCARD_SCK_PIN 13
// And those used for inputs
#define HOOK_PIN 0
#define PLAYBACK_BUTTON_PIN 1

#define noINSTRUMENT_SD_WRITE

#define TIMEOUT 300000

// GLOBALS
// Audio initialisation code can be generated using the GUI interface at https://www.pjrc.com/teensy/gui/
// Inputs
AudioSynthWaveform waveform1;                         // To create the "beep" sfx
AudioSynthWaveform waveformMF1;                       // To create the "beep" sfx
AudioSynthWaveform waveformMF2;                       // To create the "beep" sfx
AudioInputI2S i2s2;                                   // I2S input from microphone on audio shield
AudioPlaySdWavX playWav1;                             // Play 44.1kHz 16-bit PCM greeting WAV file
AudioRecordQueue queue1;                              // Creating an audio buffer in memory before saving to SD
AudioMixer4 mixer1;                                   // Allows merging several inputs to same output
AudioMixer4 mixer2;                                   // Allows merging several inputs to same output
AudioMixer4 mixer3;                                   // Allows merging several inputs to same output
AudioOutputI2S i2s1;                                  // I2S interface to Speaker/Line Out on Audio shield
AudioConnection patchCord1(waveform1, 0, mixer1, 0);  // wave to mixer
AudioConnection patchCord2(playWav1, 0, mixer1, 1);   // wav file mixer

AudioConnection patchCord3(waveformMF1, 0, mixer2, 0);  // DTMF 1
AudioConnection patchCord4(waveformMF2, 0, mixer2, 1);  // DTMF 2

AudioConnection patchCord5(mixer2, 0, mixer1, 2);  //mixer2 into mixer
AudioConnection patchCord6(mixer2, 0, mixer3, 0);  //mixer2 into mixer

AudioConnection patchCord7(i2s2, 0, mixer3, 1);  // mic input to mixer2 (L)

AudioConnection patchCord8(mixer1, 0, i2s1, 0);     // mixer output to speaker (L)
AudioConnection patchCord9(mixer1, 0, i2s1, 1);     // mixer output to speaker (R)
AudioConnection patchCord10(mixer3, 0, queue1, 0);  // mixer2 input to queue (L)
AudioControlSGTL5000 sgtl5000_1;

// Filename to save audio recording on SD card
char filename[15];
// The file object itself
File frec;

// Use long 40ms debounce time on both switches
Bounce buttonRecord = Bounce(HOOK_PIN, 40);
Bounce buttonPlay = Bounce(PLAYBACK_BUTTON_PIN, 40);

// Keep track of current state of the device
enum Mode { Initialising,
            Ready,
            Prompting,
            Init_Recording,
            Recording,
            Playing,
            Playing_Greeting,
            Recording_Greeting,
            Waiting,
            Tone };
Mode mode = Mode::Initialising;
Mode waitMode = mode;

enum Tones { Beep,
             Two_Tone,
             Solid,
             None };

float beep_volume = 0.04f;  // not too loud :-)

uint32_t MTPcheckInterval;  // default value of device check interval [ms]

unsigned long startTime = 0;
unsigned long waitTime = 0;
unsigned long waitStartTime = 0;
unsigned long curMillis = 0;
unsigned long toneTime = 0;

// variables for writing to WAV file
unsigned long ChunkSize = 0L;
unsigned long Subchunk1Size = 16;
unsigned int AudioFormat = 1;
unsigned int numChannels = 1;
unsigned long sampleRate = 44100;
unsigned int bitsPerSample = 16;
unsigned long byteRate = sampleRate * numChannels * (bitsPerSample / 8);  // samplerate x channels x (bitspersample / 8)
unsigned int blockAlign = numChannels * bitsPerSample / 8;
unsigned long Subchunk2Size = 0L;
unsigned long recByteSaved = 0L;
unsigned long NumSamples = 0L;
byte byte1, byte2, byte3, byte4;


const byte ROWS = 4;  //four rows
const byte COLS = 4;  //four columns
//define the cymbols on the buttons of the keypads
char hexaKeys[ROWS][COLS] = {
  { '1', '2', 'x', '3' },
  { '4', '5', 'F', '6' },
  { '7', '8', 'x', '9' },
  { '*', '0', 'R', '#' }
};
byte rowPins[ROWS] = { 4, 5, 3, 2 };      //connect to the row pinouts of the keypad
byte colPins[COLS] = { 17, 16, 15, 14 };  //connect to the column pinouts of the keypad

Keypad customKeypad = Keypad(makeKeymap(hexaKeys), rowPins, colPins, ROWS, COLS);

int curTone = Tones::None;
int tonePos = 0;

float toneAmpVal[3][9] = {
  { 0, beep_volume, 0, beep_volume, 0, beep_volume, 0, beep_volume, 0 },
  { 0, beep_volume, 0, beep_volume, 0, beep_volume, 0, beep_volume, 0 },
  { 0, beep_volume, beep_volume, beep_volume, beep_volume, beep_volume, beep_volume, beep_volume, 0 }
};

float toneFreqVal[3][9] = {
  { 0, 523.25, 523.25, 523.25, 523.25, 523.25, 523.25, 523.25, 0 },
  { 0, 523.25, 375.0, 523.25, 375.0, 523.25, 375.0, 523.25, 0 },
  { 0, 440, 440, 440, 440, 440, 440, 440, 0 }
};

unsigned long toneWaitVal[3][9] = {
  { 0, 250, 250, 250, 250, 250, 250, 250, 0 },
  { 0, 250, 250, 250, 250, 250, 250, 250, 0 },
  { 0, 200, 200, 200, 200, 200, 200, 200, 0 }
};

void playTone(Tones play);

void setup() {

  Serial.begin(9600);
  while (!Serial && millis() < 5000) {
    // wait for serial port to connect.
  }
  Serial.println("Serial set up correctly");
  Serial.printf("Audio block set to %d samples\n", AUDIO_BLOCK_SAMPLES);
  changeMode(Mode::Initialising);
  // Configure the input pins
  pinMode(HOOK_PIN, INPUT_PULLUP);
  pinMode(PLAYBACK_BUTTON_PIN, INPUT_PULLUP);

  // Audio connections require memory, and the record queue
  // uses this memory to buffer incoming audio.
  AudioMemory(60);

  // Enable the audio shield, select input, and enable output
  sgtl5000_1.enable();
  // Define which input on the audio shield to use (AUDIO_INPUT_LINEIN / AUDIO_INPUT_MIC)
  sgtl5000_1.inputSelect(AUDIO_INPUT_MIC);
  //sgtl5000_1.adcHighPassFilterDisable(); //
  sgtl5000_1.volume(0.95);

  mixer1.gain(0, 1.0f);
  mixer1.gain(1, 1.0f);
  mixer1.gain(3, 1.0f);

  mixer2.gain(0, 0.8f);
  mixer2.gain(1, 0.8f);

  mixer3.gain(0, 1.0f);
  mixer3.gain(1, 1.0f);
  // Play a beep to indicate system is online
  waveform1.begin(beep_volume, 440, WAVEFORM_SINE);
  wait(1000);
  waveform1.amplitude(0);
  delay(1000);

  //
  // waveformMF1.begin(0, 440, WAVEFORM_SINE);
  // waveformMF2.begin(0, 440, WAVEFORM_SINE);

  // Initialize the SD card
  SPI.setMOSI(SDCARD_MOSI_PIN);
  SPI.setSCK(SDCARD_SCK_PIN);
  if (!(SD.begin(SDCARD_CS_PIN))) {
    // stop here if no SD card, but print a message
    while (1) {
      Serial.println("Unable to access the SD card");
      delay(500);
    }
  } else Serial.println("SD card correctly initialized");


  // mandatory to begin the MTP session.
  MTP.begin();

  // Add SD Card
  //    MTP.addFilesystem(SD, "SD Card");
  MTP.addFilesystem(SD, "Something Borrowed Audio Guestbook");  // choose a nice name for the SD card volume to appear in your file explorer
  Serial.println("Added SD card via MTP");
  MTPcheckInterval = MTP.storage()->get_DeltaDeviceCheckTimeMS();

  // Value in dB
  sgtl5000_1.micGain(16);  //used 10 for real mic, 16 for high def
  //sgtl5000_1.micGain(5); // much lower gain is required for the AOM5024 electret capsule

  // Synchronise the Time object used in the program code with the RTC time provider.
  // See https://github.com/PaulStoffregen/Time
  setSyncProvider(getTeensy3Time);

  // Define a callback that will assign the correct datetime for any file system operations
  // (i.e. saving a new audio recording onto the SD card)
  FsDateTime::setCallback(dateTime);

  changeMode(Mode::Ready);
}

void loop() {
  curMillis = millis();


  // Handle the keypad
  checkKeypad();

  // Read the buttons
  buttonRecord.update();
  buttonPlay.update();

  switch (mode) {
    case Mode::Ready:
      // Falling edge occurs when the handset is lifted --> 611 telephone
      if (buttonRecord.fallingEdge()) {
        Serial.println("Handset lifted");
        changeMode(Mode::Prompting);
      } else if (buttonPlay.fallingEdge()) {
        //playAllRecordings();
        playLastRecording();
      }
      break;

    case Mode::Prompting:
      // Wait a second for users to put the handset to their ear
      wait2(1000);
      if (!SD.exists("greeting.wav")) {
        changeMode(Mode::Recording_Greeting);
        playTone(Tones::Two_Tone);
        break;
      }

      // Play the greeting inviting them to record their message
      playWav1.play("greeting.wav");
      changeMode(Mode::Playing_Greeting);
      break;

    case Mode::Playing_Greeting:
      // Wait until the message has finished playing

      // hung up while playing greeting
      if (buttonRecord.risingEdge()) {
        playWav1.stop();
        changeMode(Mode::Ready);
        return;
      }

      // play recording button
      if (buttonPlay.fallingEdge()) {
        playWav1.stop();
        //playAllRecordings();
        playLastRecording();
        return;
      }

      if (playWav1.isStopped()) {
        Serial.println("Starting Recording");
        mode = Mode::Init_Recording;
        playTone(Tones::Solid);
      }
      break;

    case Mode::Init_Recording:
      startTime = curMillis;
      // Start the recording function
      startRecording();
      break;

    case Mode::Recording:
      // Handset is replaced
      if (buttonRecord.risingEdge()) {
        // Debug log
        Serial.println("Stopping Recording");
        // Stop recording
        stopRecording();
        // Play audio tone to confirm recording has ended
        playTone(Tones::Beep);
      } else if (millis() - startTime >= TIMEOUT) {
        // Debug log
        Serial.println("Stopping Recording because timelimit");
        // Stop recording
        stopRecording();
        // Play audio tone and message to confirm recording has ended
        playTone(Tones::Two_Tone);
      } else {
        continueRecording();
      }
      break;

    case Mode::Tone:  // to make compiler happy
      if (curMillis - toneTime >= toneWaitVal[curTone][tonePos]) {
        tonePos++;
        waveform1.frequency(toneFreqVal[curTone][tonePos]);
        waveform1.amplitude(toneAmpVal[curTone][tonePos]);
        toneTime = curMillis;
      }
      if (tonePos > 8) {
        mode = waitMode;
      }

      break;

    case Mode::Playing:  // to make compiler happy

      if (buttonPlay.fallingEdge() || buttonRecord.risingEdge()) {
        playWav1.stop();
        changeMode(Mode::Ready);
        return;
      }

      if (!playWav1.isStopped()) {
        return;
      }

      // file has been played
      changeMode(Mode::Ready);
      playTone(Tones::Beep);
      break;

    case Mode::Initialising:  // to make compiler happy
      break;

    case Mode::Recording_Greeting:  // to make compiler happy
      startRecordingGreeting();
      break;

    case Mode::Waiting:  // to make compiler happy
      if (curMillis - waitStartTime >= waitTime) {
        mode = waitMode;
        return;
      }

      if (buttonRecord.risingEdge()) {
        playWav1.stop();
        changeMode(Mode::Ready);
        return;
      }
      break;
  }

  MTP.loop();  // This is mandatory to be placed in the loop code.
}

void setMTPdeviceChecks(bool nable) {
  if (nable) {
    MTP.storage()->set_DeltaDeviceCheckTimeMS(MTPcheckInterval);
    Serial.print("En");
  } else {
    MTP.storage()->set_DeltaDeviceCheckTimeMS((uint32_t)-1);
    Serial.print("Dis");
  }
  Serial.println("abled MTP storage device checks");
}


#if defined(INSTRUMENT_SD_WRITE)
static uint32_t worstSDwrite, printNext;
#endif  // defined(INSTRUMENT_SD_WRITE)

void startRecordingGreeting() {
  if (SD.exists("greeting.wav")) {
    return;
  }

  frec = SD.open("greeting.wav", FILE_WRITE);
  Serial.println("Opened Greeting file !");
  if (frec) {
    Serial.print("Recording to greeting.wav");
    queue1.begin();
    changeMode(Mode::Recording);
    recByteSaved = 0L;
  } else {
    Serial.println("Couldn't open file to record!");
  }
}

void startRecording() {
  setMTPdeviceChecks(false);  // disable MTP device checks while recording
#if defined(INSTRUMENT_SD_WRITE)
  worstSDwrite = 0;
  printNext = 0;
#endif  // defined(INSTRUMENT_SD_WRITE)
  // Find the first available file number
  //  for (uint8_t i=0; i<9999; i++) { // BUGFIX uint8_t overflows if it reaches 255
  for (uint16_t i = 0; i < 9999; i++) {
    // Format the counter as a five-digit number with leading zeroes, followed by file extension
    snprintf(filename, 11, " %05d.wav", i);
    // Create if does not exist, do not open existing, write, sync after write
    if (!SD.exists(filename)) {
      break;
    }
  }
  frec = SD.open(filename, FILE_WRITE);
  Serial.println("Opened file !");
  if (frec) {
    Serial.print("Recording to ");
    Serial.println(filename);
    queue1.begin();
    changeMode(Mode::Recording);
    recByteSaved = 0L;
  } else {
    Serial.println("Couldn't open file to record!");
  }
}

void continueRecording() {
#if defined(INSTRUMENT_SD_WRITE)
  uint32_t started = micros();
#endif  // defined(INSTRUMENT_SD_WRITE)
#define NBLOX 16
  // Check if there is data in the queue
  if (queue1.available() >= NBLOX) {
    byte buffer[NBLOX * AUDIO_BLOCK_SAMPLES * sizeof(int16_t)];
    // Fetch 2 blocks from the audio library and copy
    // into a 512 byte buffer.  The Arduino SD library
    // is most efficient when full 512 byte sector size
    // writes are used.
    for (int i = 0; i < NBLOX; i++) {
      memcpy(buffer + i * AUDIO_BLOCK_SAMPLES * sizeof(int16_t), queue1.readBuffer(), AUDIO_BLOCK_SAMPLES * sizeof(int16_t));
      queue1.freeBuffer();
    }
    // Write all 512 bytes to the SD card
    frec.write(buffer, sizeof buffer);
    recByteSaved += sizeof buffer;
  }

#if defined(INSTRUMENT_SD_WRITE)
  started = micros() - started;
  if (started > worstSDwrite)
    worstSDwrite = started;

  if (millis() >= printNext) {
    Serial.printf("Worst write took %luus\n", worstSDwrite);
    worstSDwrite = 0;
    printNext = millis() + 250;
  }
#endif  // defined(INSTRUMENT_SD_WRITE)
}

void stopRecording() {
  // Stop adding any new data to the queue
  queue1.end();
  // Flush all existing remaining data from the queue
  while (queue1.available() > 0) {
    // Save to open file
    frec.write((byte*)queue1.readBuffer(), AUDIO_BLOCK_SAMPLES * sizeof(int16_t));
    queue1.freeBuffer();
    recByteSaved += AUDIO_BLOCK_SAMPLES * sizeof(int16_t);
  }
  writeOutHeader();
  // Close the file
  frec.close();
  Serial.println("Closed file");
  changeMode(Mode::Ready);
  setMTPdeviceChecks(true);  // enable MTP device checks, recording is finished
}

void playLastRecording() {
  // Find the first available file number
  uint16_t idx = 0;
  for (uint16_t i = 0; i < 9999; i++) {
    // Format the counter as a five-digit number with leading zeroes, followed by file extension
    snprintf(filename, 11, " %05d.wav", i);
    // check, if file with index i exists
    if (!SD.exists(filename)) {
      idx = i - 1;
      break;
    }
  }

  // now play file with index idx == last recorded file
  snprintf(filename, 11, " %05d.wav", idx);
  Serial.println(filename);
  playWav1.play(filename);
  changeMode(Mode::Playing);
}


// Retrieve the current time from Teensy built-in RTC
time_t getTeensy3Time() {
  return Teensy3Clock.get();
}

// Callback to assign timestamps for file system operations
void dateTime(uint16_t* date, uint16_t* time, uint8_t* ms10) {

  // Return date using FS_DATE macro to format fields.
  *date = FS_DATE(year(), month(), day());

  // Return time using FS_TIME macro to format fields.
  *time = FS_TIME(hour(), minute(), second());

  // Return low time bits in units of 10 ms.
  *ms10 = second() & 1 ? 100 : 0;
}

// Non-blocking delay, which pauses execution of main program logic,
// but while still listening for input
void wait(unsigned int milliseconds) {
  elapsedMillis msec = 0;

  while (msec <= milliseconds) {
    delay(1);
  }
}

void wait2(unsigned long milliseconds) {
  waitTime = milliseconds;
  waitStartTime = curMillis;
  mode = Mode::Waiting;
}

void writeOutHeader() {  // update WAV header with final filesize/datasize

  //  NumSamples = (recByteSaved*8)/bitsPerSample/numChannels;
  //  Subchunk2Size = NumSamples*numChannels*bitsPerSample/8; // number of samples x number of channels x number of bytes per sample
  Subchunk2Size = recByteSaved - 42;  // because we didn't make space for the header to start with! Lose 21 samples...
  ChunkSize = Subchunk2Size + 34;     // was 36;
  frec.seek(0);
  frec.write("RIFF");
  byte1 = ChunkSize & 0xff;
  byte2 = (ChunkSize >> 8) & 0xff;
  byte3 = (ChunkSize >> 16) & 0xff;
  byte4 = (ChunkSize >> 24) & 0xff;
  frec.write(byte1);
  frec.write(byte2);
  frec.write(byte3);
  frec.write(byte4);
  frec.write("WAVE");
  frec.write("fmt ");
  byte1 = Subchunk1Size & 0xff;
  byte2 = (Subchunk1Size >> 8) & 0xff;
  byte3 = (Subchunk1Size >> 16) & 0xff;
  byte4 = (Subchunk1Size >> 24) & 0xff;
  frec.write(byte1);
  frec.write(byte2);
  frec.write(byte3);
  frec.write(byte4);
  byte1 = AudioFormat & 0xff;
  byte2 = (AudioFormat >> 8) & 0xff;
  frec.write(byte1);
  frec.write(byte2);
  byte1 = numChannels & 0xff;
  byte2 = (numChannels >> 8) & 0xff;
  frec.write(byte1);
  frec.write(byte2);
  byte1 = sampleRate & 0xff;
  byte2 = (sampleRate >> 8) & 0xff;
  byte3 = (sampleRate >> 16) & 0xff;
  byte4 = (sampleRate >> 24) & 0xff;
  frec.write(byte1);
  frec.write(byte2);
  frec.write(byte3);
  frec.write(byte4);
  byte1 = byteRate & 0xff;
  byte2 = (byteRate >> 8) & 0xff;
  byte3 = (byteRate >> 16) & 0xff;
  byte4 = (byteRate >> 24) & 0xff;
  frec.write(byte1);
  frec.write(byte2);
  frec.write(byte3);
  frec.write(byte4);
  byte1 = blockAlign & 0xff;
  byte2 = (blockAlign >> 8) & 0xff;
  frec.write(byte1);
  frec.write(byte2);
  byte1 = bitsPerSample & 0xff;
  byte2 = (bitsPerSample >> 8) & 0xff;
  frec.write(byte1);
  frec.write(byte2);
  frec.write("data");
  byte1 = Subchunk2Size & 0xff;
  byte2 = (Subchunk2Size >> 8) & 0xff;
  byte3 = (Subchunk2Size >> 16) & 0xff;
  byte4 = (Subchunk2Size >> 24) & 0xff;
  frec.write(byte1);
  frec.write(byte2);
  frec.write(byte3);
  frec.write(byte4);
  frec.close();
  Serial.println("header written");
  Serial.print("Subchunk2: ");
  Serial.println(Subchunk2Size);
}



void changeMode(Mode newMode) {  // only for debugging
  mode = newMode;

  Serial.print("Mode switched to: ");
  // Initialising, Ready, Prompting, Recording, Playing
  if (mode == Mode::Ready) Serial.println(" Ready");
  else if (mode == Mode::Prompting) Serial.println(" Prompting");
  else if (mode == Mode::Recording) Serial.println(" Recording");
  else if (mode == Mode::Playing) Serial.println(" Playing");
  else if (mode == Mode::Initialising) Serial.println(" Initialising");
  else if (mode == Mode::Recording_Greeting) Serial.println(" Recording Greeting");
  else Serial.println(" Undefined");
}



void checkKeypad() {
  if (customKeypad.getState() == RELEASED || customKeypad.getState() == IDLE) {
    waveformMF1.amplitude(0);
    waveformMF2.amplitude(0);
  }

  if(buttonRecord.read()) {
    return;
  }

  char key = customKeypad.getKey();
  if (key != NO_KEY) {
    Serial.println(key);
    float tone1;
    float tone2;
    if (key == '1' || key == '2' || key == '3') {
      tone1 = 697;
    } else if (key == '4' || key == '5' || key == '6') {
      tone1 = 770;
    } else if (key == '7' || key == '8' || key == '9') {
      tone1 = 852;
    } else if (key == '*' || key == '0' || key == '#') {
      tone1 = 941;
    }

    if (key == '1' || key == '4' || key == '7' || key == '*') {
      tone2 = 1209;
    } else if (key == '2' || key == '5' || key == '8' || key == '0') {
      tone2 = 1336;
    } else if (key == '3' || key == '6' || key == '9' || key == '#') {
      tone2 = 1477;
    }

    waveformMF1.begin(beep_volume, tone1, WAVEFORM_SINE);
    waveformMF2.begin(beep_volume, tone2, WAVEFORM_SINE);
  }
}
