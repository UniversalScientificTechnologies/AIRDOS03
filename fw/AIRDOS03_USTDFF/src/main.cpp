#include <Arduino.h>
#define TYPE    "AIRDOS03B"
#define ADCTYPE "USTSIPIN03C"

#define MAJOR 1
#define MINOR 3
#include "githash.h"

#define XSTR(s) STR(s)
#define STR(s) #s

String FWversion = XSTR(MAJOR)"."XSTR(MINOR)"."XSTR(GHRELEASE)"-"XSTR(GHBUILD)"-"XSTR(GHBUILDTYPE);

// ADC values below THRESHOLD go to the histogram.
// Values >= THRESHOLD are recorded as individual $E events.
#define THRESHOLD  64
#define MAX_EVENTS 300

#include <Wire.h>
#include <SPI.h>

#define CONV   0   // PB0  — ADC CONV signal   (PCINT0,  PCINT0_vect)
#define PPS    12  // PD4  — GNSS 1PPS          (PCINT12, PCINT1_vect)
#define DRESET 18  // PC2  — ADC peak-det reset
#define DSET   15  // PD7  — ADC chip enable
#define LED1   PIN_LED_RED
#define LED2   PIN_LED_BLUE
#define LED3   PIN_LED_GREEN

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

// ---------------------------------------------------------------------------
// Measurement buffers  (written from main-loop ADC polling)
// ---------------------------------------------------------------------------
uint16_t          histogram[THRESHOLD];
volatile uint16_t event_time[MAX_EVENTS];
volatile uint16_t event_channel[MAX_EVENTS];
volatile uint16_t events_counter = 0;
volatile uint16_t startSystime   = 0;

// ---------------------------------------------------------------------------
// Time-keeping
// ---------------------------------------------------------------------------
volatile uint32_t rtc_seconds    = 0;   // incremented on every 1PPS pulse
volatile uint16_t last_pps_tcnt1 = 0;   // TCNT1 value captured at last 1PPS

uint32_t gnss_sync_unix   = 0;          // Unix time received from GNSS
uint32_t sync_rtc_seconds = 0;          // rtc_seconds at moment of GNSS sync
bool     gnss_synced      = false;      // at least one valid fix received
bool     gnss_had_fix     = false;      // status flag from previous $GPRMC

// ---------------------------------------------------------------------------
// Other globals
// ---------------------------------------------------------------------------
uint16_t count    = 0;
uint8_t  ADCconf1 = 0;
uint8_t  ADCconf2 = 0;

unsigned long lastDataOutMs = 0;
unsigned long lastStatusMs  = 0;

char    nmea_buf[100];
uint8_t nmea_len = 0;

static void readNMEA();

// ===========================================================================
// ISR: GNSS 1PPS on PD4 (PCINT28 → PCINT3_vect)
// Rising edge: start of a new UTC second.
// ===========================================================================
ISR(PCINT3_vect)
{
  if (PIND & (1 << 4))              // rising edge — start of UTC second
  {
    last_pps_tcnt1 = TCNT1;
    rtc_seconds++;
    PORTC |= (1 << 7);             // LED3 ON  (PC7 = PIN_LED_GREEN = D23)
  }
  else                              // falling edge — end of 1PPS pulse
  {
    PORTC &= ~(1 << 7);            // LED3 OFF
  }
}

// ===========================================================================
// ADC CONV polling on PB0
// High CONV level means ADC conversion ready — read value via raw SPI.
// ===========================================================================
static inline void serviceADC()
{
  if (!(PINB & (1 << 0))) return;   // ignore falling edge

  uint16_t timestamp = TCNT1;

  PORTC &= ~(1 << 2);               // DRESET LOW  (PC2)

  // Raw SPI transfer (SPI library already configured in setup)
  SPDR = 0x00;
  while (!(SPSR & (1 << SPIF)));
  uint8_t hi = SPDR;
  SPDR = 0x00;
  while (!(SPSR & (1 << SPIF)));
  uint8_t lo = SPDR;

  PORTC |= (1 << 2);                // DRESET HIGH

  uint16_t adcVal = ((uint16_t)hi << 8) | lo;
  adcVal >>= 2;                     // 12-bit SPI word → 10-bit ADC value

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

static void serviceADCAndNMEAFor(uint16_t waitMs)
{
  unsigned long start = millis();
  while ((unsigned long)(millis() - start) < waitMs)
  {
    serviceADC();
    readNMEA();
  }
}

// ===========================================================================
// Time helpers
// ===========================================================================

// Convert GNSS DDMMYY + HHMMSS to Unix timestamp (UTC).
static uint32_t nmeaToUnix(uint8_t day, uint8_t month, uint8_t year2,
                            uint8_t hour, uint8_t min, uint8_t sec)
{
  uint16_t year = (year2 >= 70u) ? (1900u + year2) : (2000u + year2);

  uint32_t days = 0;
  for (uint16_t y = 1970; y < year; y++)
    days += ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) ? 366u : 365u;

  const uint8_t dim[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
  uint8_t feb = ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)) ? 29u : 28u;
  for (uint8_t m = 0; m < month - 1u; m++)
    days += (m == 1) ? feb : dim[m];
  days += day - 1u;

  return days * 86400UL
       + (uint32_t)hour * 3600UL
       + (uint32_t)min  * 60UL
       + sec;
}

// Convert Unix timestamp to calendar components.
static void unixToDateTime(uint32_t t, uint16_t &year,
                            uint8_t &month, uint8_t &day,
                            uint8_t &hour,  uint8_t &minute, uint8_t &second)
{
  second = t % 60; t /= 60;
  minute = t % 60; t /= 60;
  hour   = t % 24; t /= 24;
  year = 1970;
  while (true)
  {
    uint16_t diy = ((year%4==0 && year%100!=0)||(year%400==0)) ? 366u : 365u;
    if (t < diy) break;
    t -= diy;
    year++;
  }
  const uint8_t dim[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
  uint8_t feb = ((year%4==0 && year%100!=0)||(year%400==0)) ? 29u : 28u;
  month = 1;
  while (true)
  {
    uint8_t d = (month == 2) ? feb : dim[month-1];
    if (t < d) break;
    t -= d;
    month++;
  }
  day = t + 1u;
}

// Snapshot current time (safe across ISR boundary).
// tm      = current Unix time (or rtc_seconds if not yet synced)
// tm_s100 = hundredths of second derived from TCNT1 since last PPS
static void getCurrentTime(uint32_t &tm, uint8_t &tm_s100)
{
  if (!gnss_synced)
  {
    // No GNSS fix yet — output 0.0 as a clear unsynchronised indicator
    tm      = 0;
    tm_s100 = 0;
    return;
  }

  cli();
  uint32_t rSec = rtc_seconds;
  uint16_t tc1  = TCNT1;
  uint16_t pps  = last_pps_tcnt1;
  sei();

  // Ticks since last PPS (unsigned wrap-around is intentional)
  uint16_t sub = tc1 - pps;
  // 128 µs/tick → hundredths: sub * 128 / 10000
  tm_s100 = (uint8_t)((uint32_t)sub * 128UL / 10000UL);
  if (tm_s100 > 99u) tm_s100 = 99u;

  tm = gnss_sync_unix + (rSec - sync_rtc_seconds);
}

static void print2d(uint8_t v)
{
  if (v < 10u) Serial.print('0');
  Serial.print(v);
}

// Emit $TIME record (AIRDOS04-compatible format).
static void printTime()
{
  cli();
  uint32_t rSec  = rtc_seconds;
  uint32_t sUnix = gnss_sync_unix;
  uint32_t sRtc  = sync_rtc_seconds;
  sei();

  uint32_t cur_unix = sUnix + (rSec - sRtc);
  uint32_t age      = rSec - sRtc;

  uint16_t yr; uint8_t mo, dy, hh, mm, ss;
  unixToDateTime(cur_unix, yr, mo, dy, hh, mm, ss);

  Serial.print("$TIME,");
  Serial.print(rSec);
  Serial.print(",");
  Serial.print(sUnix);
  Serial.print(",");
  Serial.print(cur_unix);
  Serial.print(",");
  Serial.print(age);
  Serial.print(",");
  Serial.print(yr);   Serial.print('-');
  print2d(mo);        Serial.print('-');
  print2d(dy);        Serial.print(' ');
  print2d(hh);        Serial.print(':');
  print2d(mm);        Serial.print(':');
  print2d(ss);
  Serial.println();
}

// ===========================================================================
// NMEA parser  (non-blocking, called from loop)
// ===========================================================================

static void processNMEA()
{
  // Accept $GPRMC (GPS-only) and $GNRMC (multi-constellation)
  if (strncmp(nmea_buf, "$GPRMC,", 7) != 0 &&
      strncmp(nmea_buf, "$GNRMC,", 7) != 0) return;

  char time_str[12] = {0};
  char status_ch    = 0;
  char date_str[8]  = {0};

  uint8_t field  = 0;
  uint8_t fstart = 0;

  for (uint8_t i = 0; i <= nmea_len; i++)
  {
    char c = nmea_buf[i];
    if (c == ',' || c == '*' || c == '\r' || c == '\n' || c == '\0')
    {
      uint8_t flen = i - fstart;
      if (field == 1 && flen >= 6u)
        memcpy(time_str, nmea_buf + fstart, flen < 10u ? flen : 10u);
      if (field == 2 && flen >= 1u)
        status_ch = nmea_buf[fstart];
      if (field == 9 && flen >= 6u)
        memcpy(date_str, nmea_buf + fstart, flen < 6u ? flen : 6u);
      field++;
      fstart = i + 1u;
      if (field > 9u) break;
    }
  }

  bool valid = (status_ch == 'A') && (time_str[0] != 0) && (date_str[0] != 0);

  bool was_synced = gnss_had_fix;
  gnss_had_fix = valid;

  if (!valid) return;

  uint8_t hh = (time_str[0]-'0')*10u + (time_str[1]-'0');
  uint8_t mm = (time_str[2]-'0')*10u + (time_str[3]-'0');
  uint8_t ss = (time_str[4]-'0')*10u + (time_str[5]-'0');
  uint8_t dd = (date_str[0]-'0')*10u + (date_str[1]-'0');
  uint8_t mo = (date_str[2]-'0')*10u + (date_str[3]-'0');
  uint8_t yy = (date_str[4]-'0')*10u + (date_str[5]-'0');

  uint32_t unix_time = nmeaToUnix(dd, mo, yy, hh, mm, ss);

  bool first_sync = !gnss_synced;

  cli();
  gnss_sync_unix    = unix_time;
  sync_rtc_seconds  = rtc_seconds;
  gnss_synced       = true;
  sei();

  // Emit $TIME on first fix or after fix was lost
  if (first_sync || !was_synced)
    printTime();
}

static void readNMEA()
{
  while (Serial1.available())
  {
    serviceADC();
    char c = (char)Serial1.read();
    if (c == '$')
    {
      nmea_buf[0] = '$';
      nmea_len = 1;
    }
    else if (nmea_len > 0u && nmea_len < (uint8_t)(sizeof(nmea_buf) - 1u))
    {
      nmea_buf[nmea_len++] = c;
      if (c == '\n')
      {
        nmea_buf[nmea_len] = '\0';
        processNMEA();
        nmea_len = 0;
      }
    }
    else
    {
      nmea_len = 0;   // overlong line — discard
    }
  }
}

// ===========================================================================
// EEPROM serial-number helper
// ===========================================================================

static void printHexSN(uint8_t eepromAddr)
{
  Wire.beginTransmission(eepromAddr);
  Wire.write((int)0x08);
  Wire.write((int)0x00);
  Wire.endTransmission();
  Wire.requestFrom(eepromAddr, (uint8_t)16);
  for (uint8_t i = 0; i < 16u; i++)
  {
    uint8_t b = Wire.read();
    if (b < 0x10u) Serial.print('0');
    Serial.print(b, HEX);
  }
}

// ===========================================================================
// Data output
// ===========================================================================

void DataOut()
{
  uint32_t tm; uint8_t tm_s100;
  getCurrentTime(tm, tm_s100);

  cli();
  uint16_t stopSystime = TCNT1;
  uint16_t evCount     = events_counter;
  uint16_t captStart   = startSystime;
  sei();

  // $START
  Serial.print("$START,");
  Serial.print(count);
  Serial.print(",");
  Serial.println(captStart);

  // $E — individual above-threshold events
  for (uint16_t n = 0; n < evCount && n < MAX_EVENTS; n++)
  {
    serviceADC();
    Serial.print("\r\n$E,");
    Serial.print(event_time[n]);
    Serial.print(",");
    Serial.print(event_channel[n]);
  }

  // $STOP
  Serial.print("\r\n$STOP,");
  Serial.print(count);
  Serial.print(",");
  Serial.print(tm);
  Serial.print(".");
  Serial.print(tm_s100);
  Serial.print(",");
  Serial.print(stopSystime);
  Serial.print(",");
  Serial.print(evCount);

  for (uint16_t n = 0; n < THRESHOLD; n++)
  {
    serviceADC();
    Serial.print(",");
    Serial.print(histogram[n]);
  }
  Serial.println();

  count++;
}

void StatusOut()
{
  uint32_t tm; uint8_t tm_s100;
  getCurrentTime(tm, tm_s100);

  // SHT31-DIS, ADDR pin HIGH → I2C address 0x45
  // Command: single shot, high repeatability, no clock stretching (0x24 0x00)
  Wire.beginTransmission(0x45);
  Wire.write((uint8_t)0x24);
  Wire.write((uint8_t)0x00);
  if (Wire.endTransmission() == 0)
  {
    serviceADCAndNMEAFor(15);
    Wire.requestFrom((uint8_t)0x45, (uint8_t)6);
    if (Wire.available() >= 6)
    {
      uint16_t t_raw  = ((uint16_t)Wire.read() << 8) | Wire.read(); Wire.read();
      uint16_t rh_raw = ((uint16_t)Wire.read() << 8) | Wire.read(); Wire.read();

      float tempC    = -45.0f + 175.0f * ((float)t_raw  / 65535.0f);
      float humidity = 100.0f * ((float)rh_raw / 65535.0f);

      Serial.print("$ENV,");
      Serial.print(count);
      Serial.print(",");
      Serial.print(tm);
      Serial.print(".");
      Serial.print(tm_s100);
      Serial.print(",");
      Serial.print(tempC,    1);
      Serial.print(",");
      Serial.println(humidity, 1);
    }
  }
}

// ===========================================================================
// setup
// ===========================================================================

void setup()
{
  Serial.begin(115200);
  Serial1.begin(38400);     // GNSS NMEA output
  Wire.setClock(100000);
  SPI.begin();
  SPI.beginTransaction(SPISettings(500000, MSBFIRST, SPI_MODE0));

  // Timer1: normal mode, prescaler 1024 → 128 µs/tick at 8 MHz
  TCCR1A = 0;
  TCCR1B = (1 << CS12) | (1 << CS10);
  TCNT1  = 0;

  pinMode(CONV,   INPUT);
  pinMode(PPS,    INPUT);
  pinMode(DRESET, OUTPUT);
  pinMode(DSET,   OUTPUT);
  pinMode(LED1,   OUTPUT);
  pinMode(LED2,   OUTPUT);
  pinMode(LED3,   OUTPUT);

  digitalWrite(DSET,   HIGH);
  digitalWrite(DRESET, HIGH);

  // PCINT3: PD4 (1PPS) — GNSS seconds tick  (PD4 = PCINT28, bit 4 of PCMSK3)
  PCICR  |= (1 << PCIE3);
  PCMSK3 |= (1 << 4);

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

  StatusOut();

  digitalWrite(LED1, LOW);
  digitalWrite(LED2, LOW);
  digitalWrite(LED3, LOW);

  memset(histogram,            0, sizeof(histogram));
  memset((void*)event_time,    0, sizeof(event_time));
  memset((void*)event_channel, 0, sizeof(event_channel));
  events_counter = 0;

  lastDataOutMs = millis();
  lastStatusMs  = millis();
  startSystime  = TCNT1;
}

// ===========================================================================
// loop
// ===========================================================================

void loop()
{
  serviceADC();
  readNMEA();

  unsigned long now = millis();

  if (now - lastDataOutMs >= 10000UL)
  {
    lastDataOutMs = now;
    digitalWrite(LED2, HIGH);

    DataOut();

    memset(histogram,            0, sizeof(histogram));
    memset((void*)event_time,    0, sizeof(event_time));
    memset((void*)event_channel, 0, sizeof(event_channel));
    events_counter = 0;
    startSystime   = TCNT1;

    // Re-arm peak detector
    digitalWrite(DSET,   HIGH);
    digitalWrite(DRESET, LOW);
    SPI.transfer16(0x0000);
    digitalWrite(DRESET, HIGH);

    digitalWrite(LED2, LOW);
  }

  if (now - lastStatusMs >= 30000UL)
  {
    lastStatusMs = now;
    StatusOut();
  }
}
