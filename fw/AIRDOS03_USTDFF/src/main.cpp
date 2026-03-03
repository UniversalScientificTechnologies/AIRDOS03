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

#define CONV        0    // PB0, ADC CONV signal
#define DRESET      22   // PC6, ADC CONV command
#define DSET        23   // PC7, ADC chip enable
#define LED1        PIN_LED_RED   // red
#define LED2        PIN_LED_BLUE  // blue
#define LED3        PIN_LED_GREEN // green
#define BUZZER      15            // PD7
#define POWER5V     26   // PA2
#define POWER3V3    2    // PB2
#define EXT_I2C_EN  20   // PC4
#define ACONNECT    27   // PA3

uint16_t count = 0;
uint8_t histogram[CHANNELS];
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
  uint16_t noise = 4;
  uint32_t flux = 0;

  for (uint16_t n = noise; n < CHANNELS; n++)
  {
    flux += histogram[n];
  }

  Serial.print("$HIST,");
  Serial.print(count);
  Serial.print(",");
  Serial.print(flux);

  for (uint16_t n = 0; n < CHANNELS; n++)
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
  SPI.begin();
  SPI.beginTransaction(SPISettings(500000, MSBFIRST, SPI_MODE0));

  pinMode(ACONNECT, INPUT);
  pinMode(CONV, INPUT);
  pinMode(DRESET, OUTPUT);
  pinMode(DSET, OUTPUT);
  pinMode(LED1, OUTPUT);
  pinMode(LED2, OUTPUT);
  pinMode(LED3, OUTPUT);
  pinMode(BUZZER, OUTPUT);

  digitalWrite(DSET, HIGH);
  digitalWrite(DRESET, HIGH);

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

  memset(histogram, 0, sizeof(histogram));
  lastDataOutMs = millis();
  lastStatusMs = millis();
}

void loop()
{
  if ((PINB & 1) != 0)
  {
    digitalWrite(DRESET, LOW);
    uint16_t adcVal = SPI.transfer16(0x0000);
    adcVal >>= 6;
    if (adcVal < CHANNELS && histogram[adcVal] < 255) histogram[adcVal]++;
    digitalWrite(DRESET, HIGH);
  }

  unsigned long now = millis();
  if (now - lastDataOutMs >= 10000)
  {
    lastDataOutMs = now;
    digitalWrite(LED2, HIGH);
    DataOut();
    memset(histogram, 0, sizeof(histogram));

    digitalWrite(DSET, HIGH);
    digitalWrite(DRESET, LOW);
    SPI.transfer16(0x0000);
    digitalWrite(DRESET, HIGH);
    digitalWrite(LED2, LOW);
  }

  if (now - lastStatusMs >= 60000)
  {
    lastStatusMs = now;
    StatusOut();
  }
}
