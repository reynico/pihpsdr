/* Copyright (C) 2024
*
* RTTY encoder/decoder for piHPSDR
* Operates as a flag (rtty_enabled) over the existing DIGL mode.
*
* RX pipeline: audio tap → biquad BPF mark/space → envelope detector
*              → PLL bit sync → Baudot decoder → text callback
*
* TX pipeline: replaces mic samples with AFSK tones (mark/space)
*              following Baudot encoding of queued text
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "rtty.h"

// ---------------------------------------------------------------------------
// Global parameters (defaults)
// ---------------------------------------------------------------------------
int    rtty_enabled   = 0;
int    rtty_mark_freq = 2125;
int    rtty_shift     = 170;
double rtty_baud_rate = 45.45;
int    rtty_invert    = 0;

// ---------------------------------------------------------------------------
// ITA2 Baudot tables
// ---------------------------------------------------------------------------
// Index = Baudot code (0-31), value = {LTRS char, FIGS char}
static const char baudot_ltrs[32] = {
    0,    'E',  '\n', 'A',  ' ',  'S',  'I',  'U',
    '\r', 'D',  'R',  'J',  'N',  'F',  'C',  'K',
    'T',  'Z',  'L',  'W',  'H',  'Y',  'P',  'Q',
    'O',  'B',  'G',  0,    'M',  'X',  'V',  0
};

static const char baudot_figs[32] = {
    0,    '3',  '\n', '-',  ' ',  '\'', '8',  '7',
    '\r', 5,    '4',  '\a', ',',  '!',  ':',  '(',
    '5',  '"',  ')',  '2',  '#',  '6',  '0',  '1',
    '9',  '?',  '&',  0,    '.',  '/',  ';',  0
};

// Special Baudot control codes
#define BAUDOT_LTRS 31
#define BAUDOT_FIGS 27
#define BAUDOT_SP   4
#define BAUDOT_CR   8
#define BAUDOT_LF   2

// ---------------------------------------------------------------------------
// Biquad filter (Direct Form II transposed)
// ---------------------------------------------------------------------------
typedef struct {
    double b0, b1, b2, a1, a2;
    double w1, w2;  // state
} biquad_t;

static void biquad_bandpass(biquad_t *f, double fc, double Q, int sr) {
    double w0 = 2.0 * M_PI * fc / sr;
    double alpha = sin(w0) / (2.0 * Q);
    double cos_w0 = cos(w0);

    double a0 = 1.0 + alpha;
    f->b0 =  alpha / a0;
    f->b1 =  0.0;
    f->b2 = -alpha / a0;
    f->a1 = -2.0 * cos_w0 / a0;
    f->a2 = (1.0 - alpha) / a0;
    f->w1 = 0.0;
    f->w2 = 0.0;
}

static inline double biquad_process(biquad_t *f, double x) {
    double y = f->b0 * x + f->w1;
    f->w1 = f->b1 * x - f->a1 * y + f->w2;
    f->w2 = f->b2 * x - f->a2 * y;
    return y;
}

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------
#define TX_QUEUE_SIZE 4096

typedef enum { STATE_IDLE, STATE_START, STATE_DATA, STATE_STOP } rx_state_t;
typedef enum { TX_IDLE, TX_START, TX_DATA, TX_STOP } tx_state_t;

static struct {
    // sample rate
    int sample_rate;

    // RX filter bank
    biquad_t mark_f1, mark_f2;
    biquad_t space_f1, space_f2;

    // RX envelope detectors
    double mark_env, space_env;
    double env_alpha;

    // RX bit timing (PLL)
    double bit_phase;
    double bit_phase_inc;
    int    last_sym;        // 1=mark 0=space
    int    cur_sym;

    // RX state machine
    rx_state_t rx_state;
    int rx_bits;
    int rx_bit_count;
    int rx_shift_mode;   // 0=LTRS, 1=FIGS

    // TX oscillator
    double tx_phase;
    double tx_freq;
    double tx_mark_freq;
    double tx_space_freq;

    // TX bit timing
    double tx_samples_per_bit;
    double tx_sample_accum;

    // TX state machine
    tx_state_t tx_state;
    int tx_bits;
    int tx_bit_idx;
    int tx_bit_count;     // total bits to send for current symbol
    int tx_shift_mode;    // 0=LTRS, 1=FIGS

    // TX text queue
    char tx_queue[TX_QUEUE_SIZE];
    int  tx_read;
    int  tx_write;

    // RX callback
    rtty_rx_text_cb_t rx_cb;
} R;

// ---------------------------------------------------------------------------
// Baudot helpers
// ---------------------------------------------------------------------------
static char baudot_to_char(int code, int shift_mode) {
    if (code < 0 || code > 31) return 0;
    return shift_mode ? baudot_figs[code] : baudot_ltrs[code];
}

// Returns Baudot code for char c. Sets *need_shift_change and *new_shift.
// Returns -1 if char is not encodable.
static int char_to_baudot(char c, int current_shift, int *new_shift, int *shift_code) {
    *new_shift = current_shift;
    *shift_code = -1;

    // Search LTRS table
    for (int i = 1; i < 32; i++) {
        if (baudot_ltrs[i] == c) {
            if (current_shift != 0) {
                *new_shift = 0;
                *shift_code = BAUDOT_LTRS;
            }
            return i;
        }
    }
    // Search FIGS table
    for (int i = 1; i < 32; i++) {
        if (baudot_figs[i] == c) {
            if (current_shift != 1) {
                *new_shift = 1;
                *shift_code = BAUDOT_FIGS;
            }
            return i;
        }
    }
    // Space is in both tables
    if (c == ' ') {
        return BAUDOT_SP;
    }
    return -1;  // not encodable
}

// ---------------------------------------------------------------------------
// RX reconfigure (called when parameters change)
// ---------------------------------------------------------------------------
void rtty_rx_reconfigure(void) {
    double mark_hz  = (double)(rtty_invert ? (rtty_mark_freq + rtty_shift) : rtty_mark_freq);
    double space_hz = (double)(rtty_invert ? rtty_mark_freq : (rtty_mark_freq + rtty_shift));

    // Q chosen so BW ≈ baud_rate (3dB BW = fc/Q)
    double Q = mark_hz / (rtty_baud_rate * 1.5);
    if (Q < 1.0) Q = 1.0;

    biquad_bandpass(&R.mark_f1,  mark_hz,  Q, R.sample_rate);
    biquad_bandpass(&R.mark_f2,  mark_hz,  Q, R.sample_rate);
    biquad_bandpass(&R.space_f1, space_hz, Q, R.sample_rate);
    biquad_bandpass(&R.space_f2, space_hz, Q, R.sample_rate);

    // Envelope LP time constant: ~1 bit period
    R.env_alpha = exp(-2.0 * M_PI * rtty_baud_rate / R.sample_rate);

    R.bit_phase_inc = rtty_baud_rate / R.sample_rate;

    R.tx_mark_freq  = mark_hz;
    R.tx_space_freq = space_hz;
    R.tx_samples_per_bit = R.sample_rate / rtty_baud_rate;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void rtty_init(int sample_rate) {
    memset(&R, 0, sizeof(R));
    R.sample_rate = sample_rate;
    R.rx_state    = STATE_IDLE;
    R.tx_state    = TX_IDLE;
    R.tx_shift_mode = 0;
    R.rx_shift_mode = 0;
    R.last_sym    = 1;  // mark = idle
    R.cur_sym     = 1;
    rtty_rx_reconfigure();
    R.tx_freq = R.tx_mark_freq;
}

void rtty_tx_init(void) {
    R.tx_state     = TX_IDLE;
    R.tx_read      = 0;
    R.tx_write     = 0;
    R.tx_shift_mode = 0;
    R.tx_phase     = 0.0;
    R.tx_freq      = R.tx_mark_freq;
    R.tx_sample_accum = 0.0;
}

// ---------------------------------------------------------------------------
// RX callback
// ---------------------------------------------------------------------------
void rtty_set_rx_callback(rtty_rx_text_cb_t cb) {
    R.rx_cb = cb;
}

// ---------------------------------------------------------------------------
// RX sample processing
// ---------------------------------------------------------------------------
static void rx_emit_char(int code) {
    char buf[2];

    // Handle shift codes
    if (code == BAUDOT_LTRS) { R.rx_shift_mode = 0; return; }
    if (code == BAUDOT_FIGS) { R.rx_shift_mode = 1; return; }

    char c = baudot_to_char(code, R.rx_shift_mode);
    if (c == 0) return;

    buf[0] = c;
    buf[1] = '\0';
    if (R.rx_cb) R.rx_cb(buf, 1);
}

void rtty_rx_sample(float sample) {
    double x = (double)sample;

    // 2-stage bandpass
    double mout = biquad_process(&R.mark_f2,  biquad_process(&R.mark_f1,  x));
    double sout = biquad_process(&R.space_f2, biquad_process(&R.space_f1, x));

    // Envelope detection (half-wave rectify + LP)
    double ma = fabs(mout);
    double sa = fabs(sout);
    R.mark_env  = R.mark_env  * R.env_alpha + ma * (1.0 - R.env_alpha);
    R.space_env = R.space_env * R.env_alpha + sa * (1.0 - R.env_alpha);

    int sym = (R.mark_env >= R.space_env) ? 1 : 0;

    // PLL: on symbol transition, nudge bit_phase toward 0 (centre of bit)
    if (sym != R.last_sym) {
        // push phase toward 0.5 (mid-point of new bit)
        if (R.bit_phase > 0.5)
            R.bit_phase += (1.0 - R.bit_phase) * 0.3;
        else
            R.bit_phase -= R.bit_phase * 0.3;
        R.last_sym = sym;
    }
    R.cur_sym = sym;

    R.bit_phase += R.bit_phase_inc;

    if (R.bit_phase >= 1.0) {
        R.bit_phase -= 1.0;

        // Sample the current symbol at the bit boundary
        int bit = R.cur_sym;  // 1=mark, 0=space

        switch (R.rx_state) {
        case STATE_IDLE:
            // Start bit is SPACE (0)
            if (bit == 0) {
                R.rx_state    = STATE_DATA;
                R.rx_bits     = 0;
                R.rx_bit_count = 0;
            }
            break;

        case STATE_DATA:
            // 5 data bits, LSB first; mark=1, space=0
            if (bit) R.rx_bits |= (1 << R.rx_bit_count);
            R.rx_bit_count++;
            if (R.rx_bit_count >= 5) {
                R.rx_state = STATE_STOP;
            }
            break;

        case STATE_STOP:
            // Stop bit must be MARK (1) — 1.5 bits, we just check 1
            if (bit == 1) {
                rx_emit_char(R.rx_bits);
            }
            R.rx_state = STATE_IDLE;
            R.rx_bits  = 0;
            break;

        default:
            R.rx_state = STATE_IDLE;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// TX queue helpers
// ---------------------------------------------------------------------------
static int tx_queue_empty(void) {
    return R.tx_read == R.tx_write;
}

static char tx_queue_pop(void) {
    char c = R.tx_queue[R.tx_read];
    R.tx_read = (R.tx_read + 1) % TX_QUEUE_SIZE;
    return c;
}

static void tx_queue_push(char c) {
    int next = (R.tx_write + 1) % TX_QUEUE_SIZE;
    if (next != R.tx_read) {   // drop if full
        R.tx_queue[R.tx_write] = c;
        R.tx_write = next;
    }
}

void rtty_tx_send_text(const char *text) {
    while (*text) {
        tx_queue_push(*text++);
    }
}

int rtty_tx_is_idle(void) {
    return R.tx_state == TX_IDLE && tx_queue_empty();
}

void rtty_tx_stop(void) {
    R.tx_read  = 0;
    R.tx_write = 0;
    R.tx_state = TX_IDLE;
    R.tx_freq  = R.tx_mark_freq;
}

// ---------------------------------------------------------------------------
// TX sample generation
// ---------------------------------------------------------------------------
// Loads the next character from queue into tx_bits/tx_bit_count, handling
// shift transitions. Returns 0 if nothing to send.
static int tx_load_next_char(void) {
    while (!tx_queue_empty()) {
        char c = tx_queue_pop();
        int new_shift, shift_code;
        int code = char_to_baudot(c, R.tx_shift_mode, &new_shift, &shift_code);

        if (code < 0) continue;    // skip unencodable chars

        if (shift_code >= 0) {
            // Need to send a shift code first — push the char back and
            // send the shift code now.
            // Actually: we re-push the original char and encode the shift code
            // by temporarily inserting it at the front (simplest: re-push c,
            // then set bits to shift code).
            // But we can't easily insert at front of queue.
            // Instead: send the shift code this call, then next call sends c.
            // Push c back so it is processed after the shift.

            // push c back to front — rotate queue by using a small trick:
            // We insert at read-1 position.
            R.tx_read = (R.tx_read - 1 + TX_QUEUE_SIZE) % TX_QUEUE_SIZE;
            R.tx_queue[R.tx_read] = c;

            // Send shift code
            R.tx_bits       = shift_code;
            R.tx_bit_count  = 5;
            R.tx_bit_idx    = 0;
            R.tx_shift_mode = new_shift;
            return 1;
        }

        R.tx_bits      = code;
        R.tx_bit_count = 5;
        R.tx_bit_idx   = 0;
        return 1;
    }
    return 0;
}

float rtty_tx_next_sample(void) {
    // Generate oscillator sample
    double two_pi = 2.0 * M_PI;
    double sample = sin(two_pi * R.tx_freq * R.tx_phase / R.sample_rate);
    R.tx_phase += 1.0;
    if (R.tx_phase >= R.sample_rate) R.tx_phase -= R.sample_rate;

    // Advance bit timing
    R.tx_sample_accum += 1.0;

    double samples_this_bit = R.tx_samples_per_bit;

    if (R.tx_state == TX_STOP) {
        // Stop bit is 1.5 bits long
        samples_this_bit = R.tx_samples_per_bit * 1.5;
    }

    if (R.tx_sample_accum < samples_this_bit) {
        return (float)sample;
    }

    // Time to move to next bit
    R.tx_sample_accum -= samples_this_bit;

    switch (R.tx_state) {
    case TX_IDLE:
        if (!tx_queue_empty()) {
            // Send start bit (SPACE)
            R.tx_freq  = R.tx_space_freq;
            R.tx_state = TX_START;
            // Pre-load the character for DATA phase
            tx_load_next_char();
        }
        // else stay IDLE sending mark (idle tone)
        R.tx_freq = R.tx_mark_freq;
        break;

    case TX_START:
        // Start bit just finished, begin DATA
        R.tx_state  = TX_DATA;
        // Set first data bit
        {
            int bit = (R.tx_bits >> R.tx_bit_idx) & 1;
            R.tx_freq = bit ? R.tx_mark_freq : R.tx_space_freq;
            R.tx_bit_idx++;
        }
        break;

    case TX_DATA:
        if (R.tx_bit_idx < R.tx_bit_count) {
            int bit = (R.tx_bits >> R.tx_bit_idx) & 1;
            R.tx_freq = bit ? R.tx_mark_freq : R.tx_space_freq;
            R.tx_bit_idx++;
        } else {
            // All data bits sent, send stop bit (MARK)
            R.tx_freq  = R.tx_mark_freq;
            R.tx_state = TX_STOP;
        }
        break;

    case TX_STOP:
        // Stop bit done — try next char
        if (!tx_queue_empty()) {
            // Send start bit for next char
            R.tx_freq  = R.tx_space_freq;
            R.tx_state = TX_START;
            tx_load_next_char();
        } else {
            R.tx_state = TX_IDLE;
            R.tx_freq  = R.tx_mark_freq;
        }
        break;
    }

    return (float)sample;
}
