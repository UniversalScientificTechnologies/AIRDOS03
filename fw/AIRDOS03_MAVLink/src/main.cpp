#include <Arduino.h>
#define TYPE    "AIRDOS03B"
#define ADCTYPE "USTSIPIN03C"

#define MAJOR 1
#define MINOR 3
#include "githash.h"

// ADC values below THRESHOLD go to the histogram.
// Values >= THRESHOLD are recorded as individual $E events.
#define THRESHOLD  64
#define MAX_EVENTS 300

#include <Wire.h>
#include <SPI.h>
#include "mavlink/common/mavlink.h"

#define CONV   0   // PB0  — ADC CONV signal   (PCINT8, PCINT1_vect)
#define PPS    12  // PD4  — GNSS 1PPS          (PCINT28, PCINT3_vect)
#define DRESET 18  // PC2  — ADC peak-det reset
#define DSET   15  // PD7  — ADC chip enable
#define LED1   PIN_LED_RED
#define LED2   PIN_LED_BLUE
#define LED3   PIN_LED_GREEN

// ---------------------------------------------------------------------------
// MAVLink tunnel sender  (payload_type = 4 for all AIRDOS03B packets)
// ---------------------------------------------------------------------------
HardwareSerial &hs = Serial;

static void SendTunnelData(uint8_t *data, uint8_t len,
                           uint8_t payload_type = 4,
                           uint8_t sysid = 1, uint8_t compid = 1)
{
  mavlink_message_t msg;
  uint8_t buf[MAVLINK_MAX_PACKET_LEN];
  mavlink_msg_tunnel_pack(1, 10, &msg, sysid, compid, payload_type, len, data);
  uint16_t n = mavlink_msg_to_send_buffer(buf, &msg);
  hs.write(buf, n);
}

// Broadcast variant: target_system = 0, target_component = 0
static void SendTunnelBroadcast(uint8_t *data, uint8_t len,
                                uint8_t payload_type = 4)
{
  mavlink_message_t msg;
  uint8_t buf[MAVLINK_MAX_PACKET_LEN];
  mavlink_msg_tunnel_pack(1, 10, &msg, 0, 0, payload_type, len, data);
  uint16_t n = mavlink_msg_to_send_buffer(buf, &msg);
  hs.write(buf, n);
}

// ---------------------------------------------------------------------------
// Measurement buffers  (written from ISR)
// ---------------------------------------------------------------------------
uint16_t          histogram[THRESHOLD];
volatile uint16_t event_time[MAX_EVENTS];
volatile uint16_t event_channel[MAX_EVENTS];
volatile uint16_t events_counter = 0;
volatile uint16_t startSystime   = 0;

// Cumulative diagnostic counters (NOT reset by 10 s cycle)
volatile uint32_t total_pulses     = 0;  // every CONV interrupt since boot
volatile uint32_t events_min_count = 0;  // events ≥ THRESHOLD since last ALIVE

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
uint8_t  serial_number[16];

unsigned long lastDataOutMs = 0;
unsigned long lastStatusMs  = 0;
unsigned long lastAliveMs   = 0;

char    nmea_buf[100];
uint8_t nmea_len = 0;

// ===========================================================================
// ISR: GNSS 1PPS on PD4 (PCINT28 → PCINT3_vect)
// ===========================================================================
ISR(PCINT3_vect)
{
  if (PIND & (1 << 4))
  {
    last_pps_tcnt1 = TCNT1;
    rtc_seconds++;
    PORTC |= (1 << 7);             // LED3 ON
  }
  else
  {
    PORTC &= ~(1 << 7);            // LED3 OFF
  }
}

// ===========================================================================
// ISR: ADC CONV on PB0 (PCINT8 → PCINT1_vect)
// ===========================================================================
ISR(PCINT1_vect)
{
  if (!(PINB & (1 << 0))) return;   // ignore falling edge

  total_pulses++;                    // cumulative since boot (alive heartbeat)

  uint16_t timestamp = TCNT1;

  PORTC &= ~(1 << 2);               // DRESET LOW

  SPDR = 0x00;
  while (!(SPSR & (1 << SPIF)));
  uint8_t hi = SPDR;
  SPDR = 0x00;
  while (!(SPSR & (1 << SPIF)));
  uint8_t lo = SPDR;

  PORTC |= (1 << 2);                // DRESET HIGH

  uint16_t adcVal = ((uint16_t)hi << 8) | lo;
  adcVal >>= 2;

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
    events_min_count++;             // counted every event (incl. > MAX_EVENTS)
  }
}

// ===========================================================================
// Time helpers
// ===========================================================================

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

static void getCurrentTime(uint32_t &tm, uint8_t &tm_s100)
{
  if (!gnss_synced)
  {
    tm      = 0;
    tm_s100 = 0;
    return;
  }
  cli();
  uint32_t rSec = rtc_seconds;
  uint16_t tc1  = TCNT1;
  uint16_t pps  = last_pps_tcnt1;
  sei();
  uint16_t sub = tc1 - pps;
  tm_s100 = (uint8_t)((uint32_t)sub * 128UL / 10000UL);
  if (tm_s100 > 99u) tm_s100 = 99u;
  tm = gnss_sync_unix + (rSec - sync_rtc_seconds);
}

// ===========================================================================
// MAVLink packet senders
// ===========================================================================

// 0x02 — TIMESYNC (18 bytes)
static void SendTimeSync()
{
  cli();
  uint32_t rSec  = rtc_seconds;
  uint32_t sUnix = gnss_sync_unix;
  uint32_t sRtc  = sync_rtc_seconds;
  sei();

  uint32_t cur_unix = sUnix + (rSec - sRtc);
  uint32_t age      = rSec - sRtc;

  uint8_t buf[18];
  buf[0]  = 0x02;
  buf[1]  = gnss_synced ? 1 : 0;
  memcpy(buf +  2, &rSec,     4);
  memcpy(buf +  6, &sUnix,    4);
  memcpy(buf + 10, &cur_unix, 4);
  memcpy(buf + 14, &age,      4);
  SendTunnelData(buf, sizeof(buf));
}

// 0x08 — ENV (17 bytes)
static void SendENV()
{
  uint32_t tm; uint8_t tm_s100;
  getCurrentTime(tm, tm_s100);

  Wire.beginTransmission(0x45);
  Wire.write((uint8_t)0x24);
  Wire.write((uint8_t)0x00);
  if (Wire.endTransmission() != 0) return;

  delay(15);
  Wire.requestFrom((uint8_t)0x45, (uint8_t)6);
  if (Wire.available() < 6) return;

  uint16_t t_raw  = ((uint16_t)Wire.read() << 8) | Wire.read(); Wire.read();
  uint16_t rh_raw = ((uint16_t)Wire.read() << 8) | Wire.read(); Wire.read();

  float tempC    = -45.0f + 175.0f * ((float)t_raw  / 65535.0f);
  float humidity = 100.0f * ((float)rh_raw / 65535.0f);

  uint8_t reserved = 0;
  uint8_t buf[17];
  buf[0] = 0x08;
  memcpy(buf + 1, &count,    2);
  memcpy(buf + 3, &tm,       4);
  buf[7] = tm_s100;
  buf[8] = reserved;
  memcpy(buf +  9, &tempC,    4);
  memcpy(buf + 13, &humidity, 4);
  SendTunnelData(buf, sizeof(buf));
}

// 0x09 — ALIVE (20 bytes), broadcast every 60 s
static void SendAlive()
{
  cli();
  uint32_t pulses  = total_pulses;
  uint32_t evt_min = events_min_count;
  events_min_count = 0;
  uint32_t rSec    = rtc_seconds;
  uint32_t sRtc    = sync_rtc_seconds;
  sei();

  uint32_t uptime_s = millis() / 1000UL;
  uint32_t age      = gnss_synced ? (rSec - sRtc) : 0UL;

  uint8_t buf[20];
  buf[0] = 0x09;
  memcpy(buf +  1, &uptime_s, 4);
  memcpy(buf +  5, &count,    2);
  memcpy(buf +  7, &pulses,   4);
  memcpy(buf + 11, &evt_min,  4);
  buf[15] = gnss_synced ? 1 : 0;
  memcpy(buf + 16, &age,      4);
  SendTunnelBroadcast(buf, sizeof(buf));
}

// ===========================================================================
// NMEA parser  (non-blocking, called from loop)
// ===========================================================================

static void processNMEA()
{
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

  if (first_sync || !was_synced)
    SendTimeSync();
}

static void readNMEA()
{
  while (Serial1.available())
  {
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
      nmea_len = 0;
    }
  }
}

// ===========================================================================
// DataOut — emits START, EVENTS chunks, STOP, HIST_LO, HIST_HI
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

  uint16_t evSent = (evCount < MAX_EVENTS) ? evCount : MAX_EVENTS;

  // 0x03 — START (5 bytes)
  {
    uint8_t buf[5];
    buf[0] = 0x03;
    memcpy(buf + 1, &count,     2);
    memcpy(buf + 3, &captStart, 2);
    SendTunnelData(buf, sizeof(buf));
  }

  // 0x07 — EVENTS (chunked, 29 events per packet)
  if (evSent > 0)
  {
    const uint8_t EVENTS_PER_PKT = 29;
    uint8_t chunk_total = (evSent + EVENTS_PER_PKT - 1) / EVENTS_PER_PKT;

    for (uint8_t chunk = 0; chunk < chunk_total; chunk++)
    {
      uint16_t base = (uint16_t)chunk * EVENTS_PER_PKT;
      uint8_t  n    = (uint8_t)((evSent - base < EVENTS_PER_PKT)
                                ? (evSent - base)
                                : EVENTS_PER_PKT);
      uint8_t buf[9 + EVENTS_PER_PKT * 4];
      buf[0] = 0x07;
      memcpy(buf + 1, &count, 2);
      buf[3] = chunk;
      buf[4] = chunk_total;
      buf[5] = n;
      buf[6] = 0; buf[7] = 0; buf[8] = 0;  // reserved

      for (uint8_t i = 0; i < n; i++)
      {
        uint16_t et = event_time[base + i];
        uint16_t ec = event_channel[base + i];
        memcpy(buf + 9 + i * 4,     &et, 2);
        memcpy(buf + 9 + i * 4 + 2, &ec, 2);
      }
      SendTunnelData(buf, (uint8_t)(9 + n * 4));
    }
  }

  // 0x04 — STOP (14 bytes)
  {
    uint8_t buf[14];
    buf[0] = 0x04;
    memcpy(buf +  1, &count,       2);
    memcpy(buf +  3, &tm,          4);
    buf[7] = tm_s100;
    memcpy(buf +  8, &stopSystime, 2);
    memcpy(buf + 10, &evCount,     2);
    memcpy(buf + 12, &evSent,      2);
    SendTunnelData(buf, sizeof(buf));
  }

  // 0x05 — HIST_LO (69 bytes): histogram[0..31]
  {
    uint8_t buf[69];
    buf[0] = 0x05;
    memcpy(buf + 1, &count, 2);
    for (uint8_t i = 0; i < 32; i++)
    {
      memcpy(buf + 3 + i * 2, &histogram[i], 2);
    }
    SendTunnelData(buf, sizeof(buf));
  }

  // 0x06 — HIST_HI (67 bytes): histogram[32..63]
  {
    uint8_t buf[67];
    buf[0] = 0x06;
    memcpy(buf + 1, &count, 2);
    for (uint8_t i = 0; i < 32; i++)
    {
      memcpy(buf + 3 + i * 2, &histogram[32 + i], 2);
    }
    SendTunnelData(buf, sizeof(buf));
  }

  count++;
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

  // PCINT1: PB0 (CONV)  (PB0 = PCINT8, bit 0 of PCMSK1)
  PCICR  |= (1 << PCIE1);
  PCMSK1 |= (1 << 0);

  // PCINT3: PD4 (1PPS)  (PD4 = PCINT28, bit 4 of PCMSK3)
  PCICR  |= (1 << PCIE3);
  PCMSK3 |= (1 << 4);

  // Read serial number from EEPROM 0x5B (offset 0x0800)
  Wire.beginTransmission(0x5B);
  Wire.write((int)0x08);
  Wire.write((int)0x00);
  Wire.endTransmission();
  Wire.requestFrom((uint8_t)0x5B, (uint8_t)16);
  for (uint8_t i = 0; i < 16u; i++)
    serial_number[i] = Wire.read();

  // Read ADC config from EEPROM 0x53 (offset 0x0000)
  Wire.beginTransmission(0x53);
  Wire.write((int)0x00);
  Wire.write((int)0x00);
  Wire.endTransmission();
  Wire.requestFrom((uint8_t)0x53, (uint8_t)2);
  ADCconf1 = Wire.read();
  ADCconf2 = Wire.read();

  // 0x01 — STARTUP (29 bytes)
  {
    uint8_t  threshold  = THRESHOLD;
    uint16_t max_events = MAX_EVENTS;
    uint8_t  fw_major   = MAJOR;
    uint8_t  fw_minor   = MINOR;

    uint8_t buf[29] = {0};
    buf[0] = 0x01;
    buf[1] = threshold;
    memcpy(buf +  2, &max_events,   2);
    memcpy(buf +  4, serial_number, 16);
    buf[20] = ADCconf1;
    buf[21] = ADCconf2;
    buf[22] = fw_major;
    buf[23] = fw_minor;
    // buf[24..28] = reserved (already zero)
    SendTunnelData(buf, sizeof(buf));
  }

  // Initial ENV reading
  SendENV();

  digitalWrite(LED1, LOW);
  digitalWrite(LED2, LOW);
  digitalWrite(LED3, LOW);

  memset(histogram,            0, sizeof(histogram));
  memset((void*)event_time,    0, sizeof(event_time));
  memset((void*)event_channel, 0, sizeof(event_channel));
  events_counter = 0;

  lastDataOutMs = millis();
  lastStatusMs  = millis();
  lastAliveMs   = millis();
  startSystime  = TCNT1;
}

// ===========================================================================
// loop
// ===========================================================================

void loop()
{
  readNMEA();

  unsigned long now = millis();

  if (now - lastDataOutMs >= 10000UL)
  {
    lastDataOutMs = now;
    digitalWrite(LED2, HIGH);

    DataOut();

    cli();
    memset(histogram,            0, sizeof(histogram));
    memset((void*)event_time,    0, sizeof(event_time));
    memset((void*)event_channel, 0, sizeof(event_channel));
    events_counter = 0;
    startSystime   = TCNT1;
    sei();

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
    SendENV();
  }

  if (now - lastAliveMs >= 60000UL)
  {
    lastAliveMs = now;
    SendAlive();
  }
}
