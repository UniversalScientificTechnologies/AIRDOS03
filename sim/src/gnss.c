/*
 * GNSS receiver model (u-blox like).
 *
 *  - 1PPS on PD4: rising edge on every whole simulated second while the
 *    receiver has a fix, pulse width pps_width.
 *  - NMEA on UART1 (38400 8N1): $GNRMC + $GNGGA shortly after each PPS
 *    edge, describing the time of that edge. Without a fix the sentences
 *    carry status 'V' and empty fields.
 *
 * Bytes are delivered with real baud-rate spacing; if the firmware does
 * not drain the UART in time, simavr reports an RX overrun.
 */
#include <string.h>
#include <time.h>
#include "board.h"
#include "avr_ioport.h"
#include "avr_uart.h"
#include "sim_time.h"

static avr_irq_t *pin_pps;

int gnss_has_fix(const gnss_t *g, double t)
{
	if (g->fix_at < 0 || t < g->fix_at)
		return 0;
	if (t >= g->loss_from && t < g->loss_to)
		return 0;
	return 1;
}

static void enqueue(gnss_t *g, const char *s)
{
	for (; *s; s++) {
		int next = (g->q_tail + 1) % (int)sizeof(g->queue);
		if (next == g->q_head)
			return;                 /* queue full, drop */
		g->queue[g->q_tail] = (uint8_t)*s;
		g->q_tail = next;
	}
}

static void nmea(gnss_t *g, const char *body)
{
	uint8_t ck = 0;
	for (const char *c = body; *c; c++)
		ck ^= (uint8_t)*c;
	char line[128];
	snprintf(line, sizeof(line), "$%s*%02X\r\n", body, ck);
	enqueue(g, line);
	g->sentences++;
}

static avr_cycle_count_t byte_timer(avr_t *avr, avr_cycle_count_t when, void *param)
{
	gnss_t *g = param;
	if (g->q_head == g->q_tail)
		return 0;
	uint8_t b = g->queue[g->q_head];
	g->q_head = (g->q_head + 1) % (int)sizeof(g->queue);
	avr_raise_irq(avr_io_getirq(avr, AVR_IOCTL_UART_GETIRQ(g->uart), UART_IRQ_INPUT), b);
	g->bytes_sent++;
	if (g->q_head == g->q_tail)
		return 0;
	return when + avr_hz_to_cycles(avr, g->baud / 10);     /* 10 bits per byte */
}

static avr_cycle_count_t nmea_timer(avr_t *avr, avr_cycle_count_t when, void *param)
{
	gnss_t *g = param;
	unsigned long sec = g->second;          /* second whose PPS edge just passed */
	int fix = gnss_has_fix(g, (double)sec);
	char body[100];

	if (fix) {
		time_t ut = (time_t)(g->utc_start + (int64_t)sec);
		struct tm tm;
		gmtime_r(&ut, &tm);
		snprintf(body, sizeof(body),
			"GNRMC,%02d%02d%02d.00,A,5005.12345,N,01426.54321,E,0.012,,%02d%02d%02d,,,A",
			tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_mday, tm.tm_mon + 1, tm.tm_year % 100);
		nmea(g, body);
		snprintf(body, sizeof(body),
			"GNGGA,%02d%02d%02d.00,5005.12345,N,01426.54321,E,1,12,0.80,300.0,M,45.0,M,,",
			tm.tm_hour, tm.tm_min, tm.tm_sec);
		nmea(g, body);
	} else {
		nmea(g, "GNRMC,,V,,,,,,,,,,N");
		nmea(g, "GNGGA,,,,,,0,00,99.99,,,,,,");
	}
	if (g->verbose)
		fprintf(stderr, "[gnss] t=%lu fix=%d NMEA queued\n", sec, fix);

	if (avr_cycle_timer_status(avr, byte_timer, g) == 0)
		avr_cycle_timer_register(avr, 1, byte_timer, g);
	return 0;
}

static avr_cycle_count_t pps_fall_timer(avr_t *avr, avr_cycle_count_t when, void *param)
{
	avr_raise_irq(pin_pps, 0);
	return 0;
}

static avr_cycle_count_t second_timer(avr_t *avr, avr_cycle_count_t when, void *param)
{
	gnss_t *g = param;
	g->second++;
	if (gnss_has_fix(g, (double)g->second)) {
		avr_raise_irq(pin_pps, 1);
		g->pps_pulses++;
		truth(avr, "pps", "%lu", g->second);
		avr_cycle_timer_register(avr, sec_to_cycles(avr, g->pps_width), pps_fall_timer, g);
	}
	avr_cycle_timer_register(avr, sec_to_cycles(avr, g->nmea_delay), nmea_timer, g);
	return when + sec_to_cycles(avr, 1.0);
}

void gnss_init(avr_t *avr, gnss_t *g)
{
	memset(g, 0, sizeof(*g));
	g->avr = avr;
	g->uart = '1';
	g->baud = 38400;
	g->fix_at = 5;
	g->loss_from = g->loss_to = -1;
	g->pps_width = 0.1;
	g->nmea_delay = 0.05;
	g->utc_start = 1789560000;      /* 2026-09-16 12:00:00 UTC */
}

void gnss_attach(avr_t *avr, gnss_t *g)
{
	pin_pps = avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('D'), IOPORT_IRQ_PIN4);
	avr_raise_irq(pin_pps, 0);
	/* first whole second is t = 1 s */
	avr_cycle_timer_register(avr, sec_to_cycles(avr, 1.0), second_timer, g);
}
