#include <Arduino.h>
#define TYPE "AIRDOS03B"
#define ADCTYPE "USTSIPIN03C"

#define MAJOR 1
#define MINOR 3
#include "githash.h"

#define XSTR(s) STR(s)
#define STR(s) #s

String FWversion = XSTR(MAJOR)"."XSTR(MINOR)"."XSTR(GHRELEASE)"-"XSTR(GHBUILD)"-"XSTR(GHBUILDTYPE);

// Channels below this threshold go to histogram; at or above are logged as $E events
#define THRESHOLD   64
// Total ADC range (10-bit after >>2 from 12-bit SPI word)
#define CHANNELS    1024
// Maximum number of above-threshold events stored per integration
#define MAX_EVENTS  300

#include <Wire.h>
#include <SPI.h>

#define CONV        0     // PB0, ADC CONV signal — uses PCINT0
#define DRESET      18    // PC2, ADC peak-detector reset
#define DSET        15    // PD7, ADC chip enable
#define LED1        PIN_LED_RED   // red
#define LED2        PIN_LED_BLUE  // blue
#define LED3        PIN_LED_GREEN // green

/*

                     Mighty 1284p
                     +---\/---+
           (D 0) PB0 |        | PA0 (AI 0 / D24)
           (D 1) PB1 |        | PA1 (AI 1 / D25)
      INT2 (D 2) PB2 |        | PA2 (AI 2 / D26)
       PWM (D 3) PB3 |        | PA3 (AI 3 / D27)
    PWM/SS (D 4) PB4 |        | PA4 (AI 4 / D28)
      MOSI (D 5) PB5 |        | PA5 (AI 5 / D29)
  PWM/MISO (D 6) PB6 |        | PA6 (AI 6 / D30)
   PWM/SCK (D 7) PB7 |        | PA7 (AI 7 / D31)
                 RST |        | AREF
                VCC  |        | GND
                GND  |        | AVCC
              XTAL2  |        | PC7 (D 23)
              XTAL1  |        | PC6 (D 22)
      RX0 (D 8) PD0  |        | PC5 (D 21) TDI
      TX0 (D 9) PD1  |        | PC4 (D 20) TDO
RX1/INT0 (D 10) PD2  |        | PC3 (D 19) TMS
TX1/INT1 (D 11) PD3  |        | PC2 (D 18) TCK
     PWM (D 12) PD4  |        | PC1 (D 17) SDA
     PWM (D 13) PD5  |        | PC0 (D 16) SCL
     PWM (D 14) PD6  |        | PD7 (D 15) PWM
                     +--------+
*/

uint16_t count = 0;
uint16_t histogram[THRESHOLD];   // counts for ADC values 0 .. THRESHOLD-1

volatile uint16_t event_time[MAX_EVENTS];
volatile uint16_t event_channel[MAX_EVENTS];
volatile uint16_t events_counter = 0;

uint8_t ADCconf1;
uint8_t ADCconf2;

volatile uint16_t startSystime = 0;

unsigned long lastDataOutMs = 0;
unsigned long lastStatusMs = 0;

// ---------------------------------------------------------------------------
// Pin Change Interrupt on PB0 (PCINT0) — fires on every edge of CONV
// Rising edge: CONV went HIGH → ADC conversion ready
// ---------------------------------------------------------------------------
ISR(PCINT0_vect)
{
  if (!(PINB & (1 << 0))) return; // ignore falling edge

  uint16_t timestamp = TCNT1;

  // Assert DRESET (PC2) LOW to latch the ADC output
  PORTC &= ~(1 << 2);

  // Raw SPI transfer (SPI already configured in setup)
  SPDR = 0x00;
  while (!(SPSR & (1 << SPIF)));
  uint8_t hi = SPDR;
  SPDR = 0x00;
  while (!(SPSR & (1 << SPIF)));
  uint8_t lo = SPDR;

  // Release DRESET HIGH
  PORTC |= (1 << 2);

  uint16_t adcVal = ((uint16_t)hi << 8) | lo;
  adcVal >>= 2; // 12-bit SPI word → 10-bit ADC value

  if (adcVal < THRESHOLD)
  {
    if (histogram[adcVal] < 65535) histogram[adcVal]++;
  }
  else
  {
    if (events_counter < MAX_EVENTS)
    {
      event_time[events_counter]    = timestamp;
      event_channel[events_counter] = adcVal;
    }
    events_counter++;
  }
}

// ---------------------------------------------------------------------------

void printHexSN(uint8_t eepromAddr)
{
  Wire.beginTransmission(eepromAddr);
  Wire.write((int)0x08);
  Wire.write((int)0x00);
  Wire.endTransmission();
  Wire.requestFrom(eepromAddr, (uint8_t)16);
  for (int8_t reg = 0; reg < 16; reg++)
  {
    uint8_t serialbyte = Wire.read();
    if (serialbyte < 0x10) Serial.print("0");
    Serial.print(serialbyte, HEX);
  }
}

void printOptionalEnv()
{
  // Humidity/temperature from SHT4x on external I2C (0x44)
  Wire.beginTransmission(0x44);
  Wire.write((uint8_t)0xFD); // high precision measurement
  if (Wire.endTransmission() == 0)
  {
    delay(10);
    Wire.requestFrom((uint8_t)0x44, (uint8_t)6);
    if (Wire.available() >= 6)
    {
      uint16_t t_raw = ((uint16_t)Wire.read() << 8) | Wire.read();
      Wire.read();
      uint16_t rh_raw = ((uint16_t)Wire.read() << 8) | Wire.read();
      Wire.read();

      float tempC = -45.0f + 175.0f * ((float)t_raw / 65535.0f);
      float humidity = -6.0f + 125.0f * ((float)rh_raw / 65535.0f);

      Serial.print("$ENV,");
      Serial.print(count);
      Serial.print(",0.0,");
      Serial.print(tempC, 1);
      Serial.print(",");
      Serial.println(humidity, 1);
    }
  }
}

void StatusOut()
{
  printOptionalEnv();
}

void DataOut()
{
  // Snapshot volatile counters with interrupts disabled
  cli();
  uint16_t stopSystime    = TCNT1;
  uint16_t evCount        = events_counter;
  uint16_t captStart      = startSystime;
  sei();

  // $START — beginning of integration window
  Serial.print("$START,");
  Serial.print(count);
  Serial.print(",");
  Serial.println(captStart);

  // $E — individual above-threshold events
  for (uint16_t n = 0; n < evCount && n < MAX_EVENTS; n++)
  {
    Serial.print("\r\n$E,");
    Serial.print(event_time[n]);
    Serial.print(",");
    Serial.print(event_channel[n]);
  }

  // $STOP — end of integration window with histogram
  Serial.print("\r\n$STOP,");
  Serial.print(count);
  Serial.print(",0.0,");
  Serial.print(stopSystime);
  Serial.print(",");
  Serial.print(evCount);

  for (uint16_t n = 0; n < THRESHOLD; n++)
  {
    Serial.print(",");
    Serial.print(histogram[n]);
  }
  Serial.println();

  count++;
}

void setup()
{
  Serial.begin(115200);
  Wire.setClock(100000);

  // SPI: 500 kHz, MSB first, mode 0
  SPI.begin();
  SPI.beginTransaction(SPISettings(500000, MSBFIRST, SPI_MODE0));

  // Timer1: normal mode, prescaler 1024 → 128 µs/tick at 8 MHz
  TCCR1A = 0;
  TCCR1B = (1 << CS12) | (1 << CS10);
  TCNT1  = 0;

  pinMode(CONV,   INPUT);
  pinMode(DRESET, OUTPUT);
  pinMode(DSET,   OUTPUT);
  pinMode(LED1,   OUTPUT);
  pinMode(LED2,   OUTPUT);
  pinMode(LED3,   OUTPUT);

  digitalWrite(DSET,   HIGH);
  digitalWrite(DRESET, HIGH);

  // Enable Pin Change Interrupt on PB0 (PCINT0) for CONV signal
  PCICR  |= (1 << PCIE0);  // enable PCINT for port B
  PCMSK0 |= (1 << PCINT0); // enable PCINT0 (PB0)

  Serial.println("#Cvak...");

  String dataString = "$DOS," TYPE "," + FWversion + ",0," + githash + ",";
  Serial.print(dataString);
  printHexSN(0x5B);
  Serial.println();

  Serial.print("$ADC," ADCTYPE ",");
  printHexSN(0x5B);
  Serial.print(",");
  Wire.beginTransmission(0x53);
  Wire.write((int)0x00);
  Wire.write((int)0x00);
  Wire.endTransmission();
  Wire.requestFrom((uint8_t)0x53, (uint8_t)2);
  ADCconf1 = Wire.read();
  ADCconf2 = Wire.read();
  Serial.print(ADCconf1, HEX);
  Serial.println(ADCconf2, HEX);

  Serial.println("#Hmmm...");

  digitalWrite(LED1, LOW);
  digitalWrite(LED2, LOW);
  digitalWrite(LED3, LOW);

  memset(histogram,    0, sizeof(histogram));
  memset((void*)event_time,    0, sizeof(event_time));
  memset((void*)event_channel, 0, sizeof(event_channel));
  events_counter = 0;

  lastDataOutMs = millis();
  lastStatusMs  = millis();
  startSystime  = TCNT1;
}

void loop()
{
  unsigned long now = millis();

  if (now - lastDataOutMs >= 3000)
  {
    lastDataOutMs = now;
    digitalWrite(LED2, HIGH);

    DataOut();

    // Reset buffers for next integration window
    cli();
    memset(histogram,    0, sizeof(histogram));
    memset((void*)event_time,    0, sizeof(event_time));
    memset((void*)event_channel, 0, sizeof(event_channel));
    events_counter = 0;
    startSystime   = TCNT1;
    sei();

    // Pulse DSET/DRESET to re-arm peak detector
    digitalWrite(DSET,   HIGH);
    digitalWrite(DRESET, LOW);
    SPI.transfer16(0x0000);
    digitalWrite(DRESET, HIGH);

    digitalWrite(LED2, LOW);
  }

  if (now - lastStatusMs >= 300000)
  {
    lastStatusMs = now;
    StatusOut();
  }
}
