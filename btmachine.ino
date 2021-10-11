#include <EEPROM.h>

#define PIN_DIN 2
#define PIN_CS 3
#define PIN_CLK 4
#define PIN_JGND A0
#define PIN_JVCC A1
#define PIN_JVRX A2
#define PIN_JVRY A3
#define PIN_JSW A4

#define BAUDRATE 57600
#define KEYREPEAT 500
#define MAXPROGLEN 1024
#define MAXIOBUFLEN 96
#define MAXFRAMES 75

unsigned char progData[MAXPROGLEN];
unsigned char numFrames = 0;
unsigned char pattern[8];
unsigned char transfers[6];
unsigned char defaultTransfers[6];
unsigned char currentFrame;
unsigned char playbackSpeed;
unsigned short frameDelay;
unsigned short patternIndex;
unsigned short progLen;
unsigned char isPaused;
unsigned long lastHoriz = 0;
unsigned long lastVert = 0;
unsigned long lastButton = 0;
unsigned long lastTimer = 0;
unsigned long frameProgress = 0;
unsigned char ioBuffer[MAXIOBUFLEN];
unsigned char ioBufLen = 0;

#define HEADERMAGIC "73743031"
const unsigned char headerMagic[4] = {0x73, 0x74, 0x30, 0x31};
const unsigned char errorPattern[8] = {0x81, 0xbd, 0xa1, 0xbd, 0xa1, 0xa1, 0x3c, 0x81};

#define DELAYS 12
const unsigned short delayList[DELAYS] = {2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 0};

#define MULTIPLIERS 4
const unsigned char multiplierList[MULTIPLIERS] = {1, 2, 4, 0};

#define DELAYLCM 8192

#define DEMOLEN 65
const unsigned char demoData[DEMOLEN] = {
  0x73, 0x74, 0x30, 0x31, 0x04, 0xf3, 0xe7, 0xcf, 0x9f, 0x9f, 0x87, 0xfe, 0x07, 0xf8, 0x3f, 0xff, 0xd0, 0x1f, 0xff, 0x82, 0xff, 0xfd, 0x07, 0xfc,
  0x0f, 0xff, 0xff, 0xdf, 0xe0, 0x80, 0x7f, 0xff, 0xfd, 0xd8, 0xd8, 0x18, 0xff, 0xff, 0x18, 0x18, 0x18, 0x1b, 0x1b, 0x18, 0xff, 0xff, 0x18, 0x18,
  0x18, 0x18, 0x18, 0x18, 0xff, 0xff, 0x18, 0x1b, 0x1b, 0x18, 0x18, 0x18, 0xff, 0xff, 0x18, 0xd8, 0xd8
};

void spi_send(unsigned char addr, unsigned char dat) {
  unsigned char i;
  digitalWrite(PIN_CS, LOW);
  for(i=0;i<8;++i) {
    digitalWrite(PIN_CLK, LOW);
    digitalWrite(PIN_DIN, addr & (0x80 >> i));
    digitalWrite(PIN_CLK, HIGH);
  }
  for(i=0;i<8;++i) {
    digitalWrite(PIN_CLK, LOW);
    digitalWrite(PIN_DIN, dat & (0x80 >> i));
    digitalWrite(PIN_CLK, HIGH);
  }
  digitalWrite(PIN_CS, HIGH);
}
 
void display_init() {
  // from max7219 datasheet
  spi_send(0x09, 0x00);  // decode mode (individual leds, not numbers for 7-segment display)
  spi_send(0x0a, 0x03);  // intensity (7/32 duty cycle, ~20% brightness)
  spi_send(0x0b, 0x07);  // scan limit (use all lines)
  spi_send(0x0c, 0x01);  // shutdown register (normal operation)
  spi_send(0x0f, 0x00);  // display test mode (off)
}

void display_update() {
  for(int i=0; i<8; ++i) spi_send(i+1, pattern[i]);  
}

void setup() {
  Serial.begin(BAUDRATE);
  
  pinMode(PIN_CLK, OUTPUT);
  pinMode(PIN_CS, OUTPUT);
  pinMode(PIN_DIN, OUTPUT);

  pinMode(PIN_JGND, OUTPUT);
  digitalWrite(PIN_JGND, LOW);
  pinMode(PIN_JVCC, OUTPUT);
  digitalWrite(PIN_JVCC, HIGH);
  pinMode(PIN_JVRX, INPUT);
  pinMode(PIN_JVRY, INPUT);
  pinMode(PIN_JSW, INPUT_PULLUP);
  
  delay(50);
  display_init();

  persist_load();
  if(!numFrames) {
    load_demo_content();
  }
}

void persist_save() {
  EEPROM.put(0, progData);
}

void persist_load() {
  EEPROM.get(0, progData);
  load_program();
}

void load_demo_content() {
  memcpy(progData, demoData, DEMOLEN);
  memset(progData+DEMOLEN, 0, MAXPROGLEN-DEMOLEN);
  load_program();
}

void load_program() {
  if(memcmp(progData, headerMagic, 4)) {
    numFrames = 0; 
    return; 
  }
  numFrames = progData[4];
  if(numFrames > MAXFRAMES) {
    numFrames = 0;
    return;
  }
  defaultTransfers[0] = (progData[5] >> 1) & 0x7f;
  defaultTransfers[1] = (progData[5] << 6 | progData[6] >> 2) & 0x7f;
  defaultTransfers[2] = (progData[6] << 5 | progData[7] >> 3) & 0x7f;
  defaultTransfers[3] = (progData[7] << 4 | progData[8] >> 4) & 0x7f;
  defaultTransfers[4] = (progData[8] << 3 | progData[9] >> 5) & 0x7f;
  defaultTransfers[5] = (progData[9] << 2 | progData[10] >> 6) & 0x7f;
  playbackSpeed = progData[10] & 0x3f;
  patternIndex = (numFrames * 11 + 23)/ 2;  
  progLen = patternIndex + numFrames * 8;
  currentFrame = 0;
  isPaused = 0;
  lastTimer = 0;
  frameProgress = 0;
  load_frame();
}

void load_frame() {
  if(currentFrame < numFrames) {
    int p = patternIndex + currentFrame * 8;
    memcpy(pattern, progData+p, 8);
    p = (currentFrame * 11) / 2 + 11;
    int frameSpeed = 0;
    if(currentFrame % 2 == 0) {
      transfers[0] = (progData[p] >> 1) & 0x7f;
      transfers[1] = (progData[p] << 6 | progData[p+1] >> 2) & 0x7f;
      transfers[2] = (progData[p+1] << 5 | progData[p+2] >> 3) & 0x7f;
      transfers[3] = (progData[p+2] << 4 | progData[p+3] >> 4) & 0x7f;
      transfers[4] = (progData[p+3] << 3 | progData[p+4] >> 5) & 0x7f;
      transfers[5] = (progData[p+4] << 2 | progData[p+5] >> 6) & 0x7f;
      frameSpeed = (progData[p+5] >> 4) & 0x03;  
    } else {
      transfers[0] = (progData[p] << 3 | progData[p+1] >> 5) & 0x7f;
      transfers[1] = (progData[p+1] << 2 | progData[p+2] >> 6) & 0x7f;
      transfers[2] = (progData[p+2] << 1 | progData[p+3] >> 7) & 0x7f;
      transfers[3] = progData[p+3] & 0x7f;
      transfers[4] = (progData[p+4] >> 1) & 0x7f;
      transfers[5] = (progData[p+4] << 6 | progData[p+5] >> 2) & 0x7f;
      frameSpeed = progData[p+5] & 0x03;    
    }
    if(playbackSpeed < DELAYS && frameSpeed < MULTIPLIERS) {
      frameDelay = delayList[playbackSpeed] * multiplierList[frameSpeed];
    } else {
      frameDelay = 0;
    }
  } else {  // error frame
    memcpy(pattern, errorPattern, 8);
    memset(transfers, 0x78, 6);  // OP_ERROR
    frameDelay = 0;    
  }
}

void process_key(unsigned char key) {
  if(key >= 6) return;
  unsigned char op = transfers[key];
  if(op == 0x7f) op = defaultTransfers[key];  // OP_INHERIT
  switch(op) {
    case 0x7e:  // OP_NEXT
      currentFrame = (currentFrame + 1) % numFrames;
      break;
    case 0x7d:  // OP_PREV
      currentFrame = (currentFrame + numFrames - 1) % numFrames;
      break;
    case 0x7c:  // OP_PAUSE
      isPaused = !isPaused;
      break;
    case 0x7b:  // OP_FASTER
      if(playbackSpeed > 0) --playbackSpeed;
      break;
    case 0x7a:  // OP_SLOWER
      if(playbackSpeed < DELAYS-1) ++playbackSpeed;
      break;
    case 0x79:  // OP_NONE
      break;
    default:  // includes 0x78 = OP_ERROR
      if(op < numFrames) {
        currentFrame = op;
      } else {
        currentFrame = 0xff;  // error frame
      }
  }
  load_frame();
}

void process_timer() {
  if(isPaused || frameDelay == 0) {
    lastTimer = 0;
    return;
  }
  unsigned long timer = millis();
  if(lastTimer != 0) {
    frameProgress += (timer - lastTimer) * DELAYLCM / frameDelay;
    if(frameProgress > DELAYLCM) {
      int advance = frameProgress / DELAYLCM;
      frameProgress %= DELAYLCM;
      for(int i=0; i<advance; ++i) {
        process_key(5);  // TX_AUTO
      }
    }
  }
  lastTimer = timer;
}

void process_input() {
  unsigned long timer = millis();
  int xpos = analogRead(PIN_JVRX);
  int left = xpos < 256;
  int right = xpos >= 768;
  if(left || right) {
    if((lastHoriz == 0) || (timer > lastHoriz + KEYREPEAT)) {
      lastHoriz = timer;
      if(left) process_key(0);  // TX_LEFT
      if(right) process_key(1);  // TX_RIGHT
    }    
  } else {
    lastHoriz = 0;
  }

  int ypos = analogRead(PIN_JVRY);
  int up = ypos < 256;
  int down = ypos >= 768;
  if(up || down) {
    if((lastVert == 0) || (timer > lastVert + KEYREPEAT)) {
      lastVert = timer;
      if(up) process_key(2);  // TX_UP
      if(down) process_key(3);  // TX_DOWN
    }
  } else {
    lastVert = 0;
  }
  
  int button = !digitalRead(PIN_JSW);
  if(button) {
    if((lastButton == 0) || (timer > lastButton + KEYREPEAT)) {
      lastButton = timer;
      process_key(4);  // TX_CLICK
    }
  } else {
    lastButton = 0;
  }
}

unsigned char from_hex(unsigned char i) {
  if(48 <= i && i < 58) return i-48;
  if(65 <= i && i < 71) return i-55;
  if(97 <= i && i < 103) return i-87;
  return '!';
}

unsigned char to_hex(unsigned char i) {
  if(i < 10) return i+48;
  if(i < 16) return i+87;
  return '!';  
}

void handle_load_error() {
  consume_serial_line();
  Serial.write("error:load\n");
  load_demo_content();
}

void load_from_serial() {
  int bufPtr;
  int progPtr = 0;
  int retries = 0;
  unsigned char preNumFrames = from_hex(ioBuffer[8])*16 + from_hex(ioBuffer[9]);
  unsigned short preProgLen = ((unsigned short)preNumFrames * 27 + 23)/ 2;
  if(preNumFrames < 1 || preNumFrames > MAXFRAMES || preProgLen > MAXPROGLEN) {
    handle_load_error();
    return;
  }
  while(true) {
    for(bufPtr=1; bufPtr<ioBufLen; bufPtr+=2) {
      unsigned char c1 = from_hex(ioBuffer[bufPtr-1]);
      unsigned char c2 = from_hex(ioBuffer[bufPtr]);
      if(c1 == '!' || c2 == '!' || progPtr >= MAXPROGLEN) {
        handle_load_error();
        return;
      }
      progData[progPtr++] = c1*16 + c2;
    }
    if(ioBufLen < MAXIOBUFLEN) break;
    if(bufPtr == ioBufLen) {
      ioBuffer[0] = ioBuffer[bufPtr-1];
      ioBufLen = Serial.readBytesUntil('\n', ioBuffer+1, MAXIOBUFLEN-1) + 1;
    } else {
      ioBufLen = Serial.readBytesUntil('\n', ioBuffer, MAXIOBUFLEN);
    }
    if(ioBuffer[ioBufLen-1] == '\r') --ioBufLen;   // support dos-style line endings
  }
  if(bufPtr == ioBufLen) {
    handle_load_error();
    return;
  }
  memset(progData+progPtr, 0, MAXPROGLEN-progPtr);
  load_program();
  if(numFrames != preNumFrames || progLen != preProgLen || progLen != progPtr) {
    handle_load_error();
    return;
  }
  persist_save();
  Serial.write("ok\n");
}

void save_to_serial() {
  if(numFrames == 0) {
    Serial.write("error:save\n");
    return;
  }
  int ptr = 0;
  for(int i=0; i<progLen; ++i) {
    unsigned char c = progData[i];
    ioBuffer[ptr++] = to_hex(c/16);
    ioBuffer[ptr++] = to_hex(c%16);
    if(ptr >= MAXIOBUFLEN-1) {
      Serial.write(ioBuffer, ptr);
      ptr = 0;
    }
  }
  ioBuffer[ptr++] = '\n';
  Serial.write(ioBuffer, ptr);
}

void consume_serial_line() {
  while(ioBufLen == MAXIOBUFLEN) {
    ioBufLen = Serial.readBytesUntil('\n', ioBuffer, MAXIOBUFLEN);
  }
}

void process_serial() {
  if(Serial.available()) {
    ioBufLen = Serial.readBytesUntil('\n', ioBuffer, MAXIOBUFLEN);
    //char test[16]; Serial.write(": "); sprintf(test, "%d ", ioBufLen); Serial.write(test); Serial.write(ioBuffer, ioBufLen);
    if(ioBufLen < 4) return;
    if(ioBuffer[ioBufLen-1] == '\r') --ioBufLen;   // support dos-style line endings
    if((ioBufLen == 4) && !strncmp(ioBuffer, "save", 4)) {
      save_to_serial();
    }
    else if((ioBufLen >= 10) && !strncmp(ioBuffer, HEADERMAGIC, 8)) {
      load_from_serial();
    }
    else if((ioBufLen == 4) && !strncmp(ioBuffer, "ping", 4)) {
      Serial.write("pong:" HEADERMAGIC "\n");
    }
    else {
      consume_serial_line();
      Serial.write("error:parse\n");      
    }    
  }
}
 
void loop() {
  process_timer();
  process_input();
  process_serial();
  display_update();
  delay(5);
}
