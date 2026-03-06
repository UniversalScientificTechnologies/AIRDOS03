#include <Arduino.h>
#define TYPE "AIRDOS03B"
#define ADCTYPE "USTSIPIN03C"

#define MAJOR 1
#define MINOR 2
#include "githash.h"

#define XSTR(s) STR(s)
#define STR(s) #s

String FWversion = XSTR(MAJOR)"."XSTR(MINOR)"."XSTR(GHRELEASE)"-"XSTR(GHBUILD)"-"XSTR(GHBUILDTYPE);

#define CHANNELS 1024

#include <Wire.h>
#include <SPI.h>

// PC0=SCL PC1=SDA 
#define CONV0        0     // PB0=0, ADC CONV signal
#define CONV1        1     // PB1=1, ADC CONV signal
#define DRESET      18    // PC2, ADC CONV command
#define DSET        15    // PD7, ADC chip enable
#define MUX0        24    // PA0=24 
#define MUX1        25    // PA1=25 
#define LED1        PIN_LED_RED   // red PC5
#define LED2        PIN_LED_BLUE  // blue PC6
#define LED3        PIN_LED_GREEN // green PC7
#define POWER5V     26   // PA2
#define POWER3V3    2    // PB2
#define TP1         20   // PC3
#define TP2         20   // PC4
#define ACONNECT    27   // PA3

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
uint8_t histogram0[CHANNELS];
uint8_t histogram1[CHANNELS];
uint8_t ADCconf1;
uint8_t ADCconf2;

unsigned long lastDataOutMs = 0;
unsigned long lastStatusMs = 0;

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
  // Optional humidity/temperature readout from SHT4x on external I2C (0x44)
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
      Serial.print(tempC, 2);
      Serial.print(",");
      Serial.println(humidity, 2);
    }
  }
}

void StatusOut()
{
  printOptionalEnv();
}

void DataOut()
{
  Serial.print("$HIST0,");
  Serial.print(count);

  for (uint16_t n = 0; n < CHANNELS; n++)
  {
    Serial.print(",");
    Serial.print(histogram0[n]);
  }
  Serial.println();

  Serial.print("$HIST1,");
  Serial.print(count);

  for (uint16_t n = 0; n < CHANNELS; n++)
  {
    Serial.print(",");
    Serial.print(histogram1[n]);
  }
  Serial.println();

  count++;
}

void setup()
{
  Serial.begin(115200);
  Wire.setClock(100000);
  SPI.begin();
  SPI.beginTransaction(SPISettings(20000000, MSBFIRST, SPI_MODE0));

  pinMode(ACONNECT, INPUT);
  pinMode(CONV0, INPUT);
  pinMode(CONV1, INPUT);
  pinMode(DRESET, OUTPUT);
  pinMode(DSET, OUTPUT);
  pinMode(LED1, OUTPUT);
  pinMode(LED2, OUTPUT);
  pinMode(LED3, OUTPUT);
  pinMode(MUX0, OUTPUT);
  pinMode(MUX1, OUTPUT);
  
  digitalWrite(DSET, HIGH);
  digitalWrite(DRESET, HIGH);
  digitalWrite(MUX0, HIGH);
  digitalWrite(MUX1, HIGH);
  
  Serial.println("#Cvak...");

  String dataString = "$DOS," TYPE "," + FWversion + ",0," + githash + ",";
  Serial.print(dataString);
  printHexSN(0x5B); // analog board SN


  Serial.print("\r\n$ADC," ADCTYPE ",");
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
  digitalWrite(LED1, HIGH);
  delay(500);
  digitalWrite(LED2, HIGH);
  delay(500);
  digitalWrite(LED3, HIGH);
  delay(500);
  digitalWrite(LED1, LOW);
  delay(500);
  digitalWrite(LED2, LOW);
  delay(500);
  digitalWrite(LED3, LOW);

  memset(histogram0, 0, sizeof(histogram0));
  memset(histogram1, 0, sizeof(histogram1));
  lastDataOutMs = millis();
  lastStatusMs = millis();
}

void loop()
{
  uint16_t adcVal; 

  digitalWrite(MUX0, HIGH);
  digitalWrite(MUX1, LOW);
  digitalWrite(DSET, HIGH);
  digitalWrite(DRESET, HIGH);

  while(true)
  {
    //while ((PINB & 0x10) == 0);
    if ((PINB & 0x10)==0x10)
    {
      delayMicroseconds(20);
      digitalWrite(DRESET, LOW);
      digitalWrite(DRESET, HIGH);

    }

  }

  while ((PINB & 0x11) == 0);
  {
    digitalWrite(MUX0, LOW);
    digitalWrite(MUX1, HIGH);
    //digitalWrite(DSET, HIGH);
    //digitalWrite(DRESET, LOW);
    PORTC=0b00011011;
    delayMicroseconds(5);
    adcVal = SPI.transfer16(0x0000);
    adcVal >>= 4;
    adcVal &= 0x3FF;
    if (histogram0[adcVal] < 255) histogram0[adcVal]++;
    //delayMicroseconds(100);
    //digitalWrite(DRESET, HIGH);
    PORTC=0b00011111;
    //Serial.print("*");
    //Serial.print(adcVal);

    digitalWrite(MUX0, HIGH);
    digitalWrite(MUX1, LOW);
    PORTC=0b00011011;
    delayMicroseconds(5);
    adcVal = SPI.transfer16(0x0000);
    adcVal >>= 4;
    adcVal &= 0x3FF;
    if (histogram1[adcVal] < 255) histogram1[adcVal]++;
    PORTC=0b00011111;
  }

  unsigned long now = millis();
  if (now - lastDataOutMs >= 3000)
  {
    lastDataOutMs = now;
    digitalWrite(LED2, HIGH);
    DataOut();
    memset(histogram0, 0, sizeof(histogram0));
    memset(histogram1, 0, sizeof(histogram1));

    digitalWrite(DSET, HIGH);
    digitalWrite(DRESET, LOW);
    SPI.transfer16(0x0000);
    digitalWrite(DRESET, HIGH);
    digitalWrite(LED2, LOW);
  }

  /*
  if (now - lastStatusMs >= 60000)
  {
    lastStatusMs = now;
    StatusOut();
  }
  */
}
