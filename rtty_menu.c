/* Copyright (C) 2024
*
* RTTY menu for piHPSDR — GTK3 UI following cw_menu.c patterns.
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*/

#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>

#include "new_menu.h"
#include "rtty.h"
#include "rtty_menu.h"

static GtkWidget *parent_window = NULL;
static GtkWidget *dialog        = NULL;

// UI widgets we need to reference from callbacks
static GtkWidget *rx_textview   = NULL;
static GtkWidget *tx_entry      = NULL;

// ── cleanup ──────────────────────────────────────────────────────────────────

static void cleanup(void) {
    if (dialog != NULL) {
        rtty_set_rx_callback(NULL);   // disconnect callback
        gtk_widget_destroy(dialog);
        dialog      = NULL;
        sub_menu    = NULL;
    }
}

static gboolean close_cb(GtkWidget *widget, GdkEventButton *event, gpointer data) {
    cleanup();
    return TRUE;
}

static gboolean delete_event(GtkWidget *widget, GdkEvent *event, gpointer user_data) {
    cleanup();
    return FALSE;
}

// ── RX text callback (called from audio thread → g_idle_add) ─────────────────

typedef struct { char *text; int len; } rx_text_data_t;

static gboolean append_rx_text_idle(gpointer data) {
    rx_text_data_t *d = (rx_text_data_t *)data;

    if (rx_textview != NULL && GTK_IS_WIDGET(rx_textview)) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(rx_textview));
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(buf, &end);
        gtk_text_buffer_insert(buf, &end, d->text, d->len);

        // Auto-scroll to end
        gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(rx_textview),
            gtk_text_buffer_get_insert(buf),
            0.0, FALSE, 0.0, 0.0);
    }

    g_free(d->text);
    g_free(d);
    return G_SOURCE_REMOVE;
}

static void rx_text_cb(const char *text, int len) {
    rx_text_data_t *d = g_malloc(sizeof(rx_text_data_t));
    d->text = g_strndup(text, len);
    d->len  = len;
    g_idle_add(append_rx_text_idle, d);
}

// ── Callbacks ─────────────────────────────────────────────────────────────────

static void rtty_enabled_cb(GtkToggleButton *widget, gpointer data) {
    rtty_enabled = gtk_toggle_button_get_active(widget) ? 1 : 0;
    if (rtty_enabled) {
        rtty_rx_reconfigure();
    }
}

static void mark_freq_cb(GtkSpinButton *widget, gpointer data) {
    rtty_mark_freq = gtk_spin_button_get_value_as_int(widget);
    rtty_rx_reconfigure();
}

static void shift_cb(GtkSpinButton *widget, gpointer data) {
    rtty_shift = gtk_spin_button_get_value_as_int(widget);
    rtty_rx_reconfigure();
}

static void baud_combo_cb(GtkComboBox *widget, gpointer data) {
    int idx = gtk_combo_box_get_active(widget);
    static const double bauds[] = { 45.45, 50.0, 75.0, 100.0 };
    if (idx >= 0 && idx < (int)(sizeof(bauds) / sizeof(bauds[0]))) {
        rtty_baud_rate = bauds[idx];
        rtty_rx_reconfigure();
    }
}

static void invert_cb(GtkToggleButton *widget, gpointer data) {
    rtty_invert = gtk_toggle_button_get_active(widget) ? 1 : 0;
    rtty_rx_reconfigure();
}

static void send_cb(GtkButton *widget, gpointer data) {
    if (tx_entry == NULL) return;
    const char *text = gtk_entry_get_text(GTK_ENTRY(tx_entry));
    if (text && *text) {
        rtty_tx_send_text(text);
        gtk_entry_set_text(GTK_ENTRY(tx_entry), "");
    }
}

static void stop_tx_cb(GtkButton *widget, gpointer data) {
    rtty_tx_stop();
}

static void clear_tx_cb(GtkButton *widget, gpointer data) {
    if (tx_entry) gtk_entry_set_text(GTK_ENTRY(tx_entry), "");
}

static void clear_rx_cb(GtkButton *widget, gpointer data) {
    if (rx_textview == NULL) return;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(rx_textview));
    gtk_text_buffer_set_text(buf, "", -1);
}

// Enter key in TX entry sends text
static gboolean tx_entry_key_cb(GtkWidget *widget, GdkEventKey *event, gpointer data) {
    if (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter) {
        send_cb(NULL, NULL);
        return TRUE;
    }
    return FALSE;
}

// ── Main menu constructor ────────────────────────────────────────────────────

void rtty_menu(GtkWidget *parent) {
    parent_window = parent;

    dialog = gtk_dialog_new();
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(parent_window));
    gtk_window_set_title(GTK_WINDOW(dialog), "piHPSDR - RTTY");
    g_signal_connect(dialog, "delete_event", G_CALLBACK(delete_event), NULL);

    GdkRGBA color = { 1.0, 1.0, 1.0, 1.0 };
    gtk_widget_override_background_color(dialog, GTK_STATE_FLAG_NORMAL, &color);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 8);

    int row = 0;

    // ── Row 0: Enable checkbox ────────────────────────────────────────────
    GtkWidget *enabled_check = gtk_check_button_new_with_label("RTTY Enabled");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(enabled_check), rtty_enabled);
    g_signal_connect(enabled_check, "toggled", G_CALLBACK(rtty_enabled_cb), NULL);
    gtk_grid_attach(GTK_GRID(grid), enabled_check, 0, row, 4, 1);
    row++;

    // ── Row 1: Mark freq and Shift ───────────────────────────────────────
    GtkWidget *mark_label = gtk_label_new("Mark (Hz):");
    gtk_widget_set_halign(mark_label, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), mark_label, 0, row, 1, 1);

    GtkWidget *mark_spin = gtk_spin_button_new_with_range(500, 3000, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(mark_spin), (double)rtty_mark_freq);
    g_signal_connect(mark_spin, "value-changed", G_CALLBACK(mark_freq_cb), NULL);
    gtk_grid_attach(GTK_GRID(grid), mark_spin, 1, row, 1, 1);

    GtkWidget *shift_label = gtk_label_new("Shift (Hz):");
    gtk_widget_set_halign(shift_label, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), shift_label, 2, row, 1, 1);

    GtkWidget *shift_spin = gtk_spin_button_new_with_range(10, 850, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(shift_spin), (double)rtty_shift);
    g_signal_connect(shift_spin, "value-changed", G_CALLBACK(shift_cb), NULL);
    gtk_grid_attach(GTK_GRID(grid), shift_spin, 3, row, 1, 1);
    row++;

    // ── Row 2: Baud rate combo + Invert checkbox ──────────────────────────
    GtkWidget *baud_label = gtk_label_new("Baud:");
    gtk_widget_set_halign(baud_label, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), baud_label, 0, row, 1, 1);

    GtkWidget *baud_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(baud_combo), "45.45");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(baud_combo), "50");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(baud_combo), "75");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(baud_combo), "100");
    // Select closest preset
    if      (rtty_baud_rate <= 47.0)  gtk_combo_box_set_active(GTK_COMBO_BOX(baud_combo), 0);
    else if (rtty_baud_rate <= 60.0)  gtk_combo_box_set_active(GTK_COMBO_BOX(baud_combo), 1);
    else if (rtty_baud_rate <= 85.0)  gtk_combo_box_set_active(GTK_COMBO_BOX(baud_combo), 2);
    else                              gtk_combo_box_set_active(GTK_COMBO_BOX(baud_combo), 3);
    g_signal_connect(baud_combo, "changed", G_CALLBACK(baud_combo_cb), NULL);
    gtk_grid_attach(GTK_GRID(grid), baud_combo, 1, row, 1, 1);

    GtkWidget *invert_check = gtk_check_button_new_with_label("Invert (USB)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(invert_check), rtty_invert);
    g_signal_connect(invert_check, "toggled", G_CALLBACK(invert_cb), NULL);
    gtk_grid_attach(GTK_GRID(grid), invert_check, 2, row, 2, 1);
    row++;

    // ── Separator ─────────────────────────────────────────────────────────
    GtkWidget *sep1 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_grid_attach(GTK_GRID(grid), sep1, 0, row, 4, 1);
    row++;

    // ── Row: RX label ─────────────────────────────────────────────────────
    GtkWidget *rx_label = gtk_label_new("RX:");
    gtk_widget_set_halign(rx_label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), rx_label, 0, row, 4, 1);
    row++;

    // ── Row: RX text view ─────────────────────────────────────────────────
    rx_textview = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(rx_textview), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(rx_textview), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(rx_textview), GTK_WRAP_CHAR);

    GtkWidget *rx_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(rx_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(rx_scroll, 400, 120);
    gtk_container_add(GTK_CONTAINER(rx_scroll), rx_textview);
    gtk_grid_attach(GTK_GRID(grid), rx_scroll, 0, row, 4, 1);
    row++;

    // ── Row: Clear RX button ──────────────────────────────────────────────
    GtkWidget *clear_rx_btn = gtk_button_new_with_label("Clear RX");
    g_signal_connect(clear_rx_btn, "clicked", G_CALLBACK(clear_rx_cb), NULL);
    gtk_grid_attach(GTK_GRID(grid), clear_rx_btn, 0, row, 1, 1);
    row++;

    // ── Separator ─────────────────────────────────────────────────────────
    GtkWidget *sep2 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_grid_attach(GTK_GRID(grid), sep2, 0, row, 4, 1);
    row++;

    // ── Row: TX entry + Send button ───────────────────────────────────────
    GtkWidget *tx_label = gtk_label_new("TX:");
    gtk_widget_set_halign(tx_label, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), tx_label, 0, row, 1, 1);

    tx_entry = gtk_entry_new();
    gtk_widget_set_hexpand(tx_entry, TRUE);
    g_signal_connect(tx_entry, "key-press-event", G_CALLBACK(tx_entry_key_cb), NULL);
    gtk_grid_attach(GTK_GRID(grid), tx_entry, 1, row, 2, 1);

    GtkWidget *send_btn = gtk_button_new_with_label("Send");
    g_signal_connect(send_btn, "clicked", G_CALLBACK(send_cb), NULL);
    gtk_grid_attach(GTK_GRID(grid), send_btn, 3, row, 1, 1);
    row++;

    // ── Row: Stop TX + Clear TX buttons ──────────────────────────────────
    GtkWidget *stop_tx_btn = gtk_button_new_with_label("Stop TX");
    g_signal_connect(stop_tx_btn, "clicked", G_CALLBACK(stop_tx_cb), NULL);
    gtk_grid_attach(GTK_GRID(grid), stop_tx_btn, 0, row, 1, 1);

    GtkWidget *clear_tx_btn = gtk_button_new_with_label("Clear");
    g_signal_connect(clear_tx_btn, "clicked", G_CALLBACK(clear_tx_cb), NULL);
    gtk_grid_attach(GTK_GRID(grid), clear_tx_btn, 3, row, 1, 1);
    row++;

    // ── Separator ─────────────────────────────────────────────────────────
    GtkWidget *sep3 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_grid_attach(GTK_GRID(grid), sep3, 0, row, 4, 1);
    row++;

    // ── Row: Close button ─────────────────────────────────────────────────
    GtkWidget *close_btn = gtk_button_new_with_label("Close");
    g_signal_connect(close_btn, "button-press-event", G_CALLBACK(close_cb), NULL);
    gtk_grid_attach(GTK_GRID(grid), close_btn, 3, row, 1, 1);

    gtk_container_add(GTK_CONTAINER(content), grid);

    // Install RX callback
    rtty_set_rx_callback(rx_text_cb);

    gtk_widget_show_all(dialog);

    sub_menu = dialog;
}
