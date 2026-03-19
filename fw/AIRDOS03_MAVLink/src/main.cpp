#include <Arduino.h>
String FWversion = "UAV00"; // 8 MHz crystal

#define MAJOR 0
#define MINOR 1

#include "wiring_private.h"
#include <Wire.h>
#include <SPI.h>
#include "githash.h"
#include "mavlink/common/mavlink.h"

#define CONV        0     // PB0=0, PB1=1, ADC CONV signal
#define DRESET      18    // PC2, ADC CONV command
#define DSET        15    // PD7, ADC chip enable
#define LED1        PIN_LED_RED   // red
#define LED2        PIN_LED_BLUE  // blue
#define LED3        PIN_LED_GREEN // green


#define CHANNELS 1024
#define HIST_OUT_START 4
#define HIST_OUT_RANGE 252

uint8_t mavlink_info[25];
uint16_t count = 0;
uint32_t serialhash = 0;
uint16_t base_offset = HIST_OUT_START;

HardwareSerial &hs = Serial;

void SendTunnelData(uint8_t *payload_data, uint8_t payload_length, uint8_t payload_type = 0, uint8_t sysid = 0, uint8_t compid = 0)
{
  mavlink_message_t msgtn;
  uint8_t buftn[MAVLINK_MAX_PACKET_LEN];
  mavlink_msg_tunnel_pack(1, 10, &msgtn, sysid, compid, payload_type, payload_length, payload_data);
  uint16_t lentn = mavlink_msg_to_send_buffer(buftn, &msgtn);
  hs.write(buftn, lentn);
}

void setup()
{
  memset(mavlink_info, 0, sizeof(mavlink_info));

  Serial.begin(115200);
  SPI.begin();
  SPI.beginTransaction(SPISettings(500000, MSBFIRST, SPI_MODE0));
  Wire.setClock(100000);

  pinMode(CONV, INPUT);
  pinMode(DRESET, OUTPUT);
  pinMode(DSET, OUTPUT);
  digitalWrite(DSET, HIGH);
  digitalWrite(DRESET, HIGH);

  pinMode(LED1, OUTPUT);
  pinMode(LED2, OUTPUT);
  pinMode(LED3, OUTPUT);

  digitalWrite(LED1, HIGH);
  delay(100);
  digitalWrite(LED2, HIGH);
  delay(100);
  digitalWrite(LED3, HIGH);
  delay(100);

  String dataString = "$DOS,LABDOS01A," + FWversion + "," + String(base_offset) + "," + githash + ",";
  Wire.beginTransmission(0x58);
  Wire.write((int)0x08);
  Wire.write((int)0x00);
  Wire.endTransmission();
  Wire.requestFrom((uint8_t)0x58, (uint8_t)16);
  for (int8_t reg = 0; reg < 16; reg++)
  {
    uint8_t serialbyte = Wire.read();
    if (serialbyte < 0x10) dataString += "0";
    dataString += String(serialbyte, HEX);
    serialhash += serialbyte;
  }
  Serial.println(dataString);

  digitalWrite(LED1, LOW);
  digitalWrite(LED2, LOW);
  digitalWrite(LED3, LOW);
}

void loop()
{
  uint16_t histogram[CHANNELS];
  uint8_t mavlink_buffer_1[12] = {0};
  uint8_t mavlink_buffer_2[128] = {0};
  uint8_t mavlink_buffer_3[128] = {0};

  memset(histogram, 0, sizeof(histogram));

  digitalWrite(DSET, HIGH);
  digitalWrite(DRESET, LOW);
  SPI.transfer16(0x0000);
  digitalWrite(DRESET, HIGH);

  uint16_t suppress = 0;
  uint16_t maximum = 0;

  for (uint16_t i = 0; i < (4600 * 2); i++)
  {
    while ((PINB & 1) == 0) {}

    digitalWrite(DRESET, LOW);
    uint16_t adcVal = SPI.transfer16(0x0000);
    adcVal >>= 6;
    digitalWrite(DRESET, HIGH);

    if (adcVal > maximum)
    {
      maximum = adcVal;
      suppress++;
    }
    else
    {
      if (maximum < CHANNELS) histogram[maximum]++;
      maximum = 0;
    }
  }

  uint32_t dose = 0;
  for (uint16_t n = HIST_OUT_START; n < (HIST_OUT_START + HIST_OUT_RANGE); n++)
  {
    dose += histogram[n];
  }

  digitalWrite(LED3, HIGH);

  mavlink_buffer_1[0] = (count & 0xff00) >> 8;
  mavlink_buffer_1[1] = (count & 0x00ff);
  mavlink_buffer_1[2] = (dose & 0xff000000) >> 24;
  mavlink_buffer_1[3] = (dose & 0x00ff0000) >> 16;
  mavlink_buffer_1[4] = (dose & 0x0000ff00) >> 8;
  mavlink_buffer_1[5] = (dose & 0x000000ff);
  mavlink_buffer_1[6] = (suppress & 0xff00) >> 8;
  mavlink_buffer_1[7] = (suppress & 0x00ff);
  mavlink_buffer_1[8] = (base_offset & 0xff00) >> 8;
  mavlink_buffer_1[9] = (base_offset & 0x00ff);

  for (uint16_t n = 0; n < 128; n++)
  {
    uint16_t value = histogram[HIST_OUT_START + n];
    mavlink_buffer_2[n] = value > 255 ? 255 : value;
  }
  for (uint16_t n = 0; n < 124; n++)
  {
    uint16_t value = histogram[HIST_OUT_START + 128 + n];
    mavlink_buffer_3[n] = value > 255 ? 255 : value;
  }

  SendTunnelData(mavlink_buffer_1, sizeof(mavlink_buffer_1), 11, 0, 0);
  SendTunnelData(mavlink_buffer_2, sizeof(mavlink_buffer_2), 12, 0, 0);
  SendTunnelData(mavlink_buffer_3, sizeof(mavlink_buffer_3), 13, 0, 0);

  count++;
  digitalWrite(LED3, LOW);
}
