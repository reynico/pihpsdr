/* Copyright (C) 2024
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*/

#ifndef _RTTY_H
#define _RTTY_H

// Configurable parameters (globals, with defaults)
extern int    rtty_enabled;
extern int    rtty_mark_freq;   // default 2125 Hz
extern int    rtty_shift;       // default 170 Hz (space = mark + shift)
extern double rtty_baud_rate;   // default 45.45
extern int    rtty_invert;      // 0=normal, 1=swap mark/space (USB)

// Public API
void  rtty_init(int sample_rate);
void  rtty_rx_sample(float sample);
void  rtty_rx_reconfigure(void);
void  rtty_tx_init(void);
float rtty_tx_next_sample(void);
void  rtty_tx_send_text(const char *text);
int   rtty_tx_is_idle(void);
void  rtty_tx_stop(void);

// Callback for decoded RX text
typedef void (*rtty_rx_text_cb_t)(const char *text, int len);
void  rtty_set_rx_callback(rtty_rx_text_cb_t cb);

#endif // _RTTY_H
