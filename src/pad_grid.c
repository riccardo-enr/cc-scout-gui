#include "pad_grid.h"

#include <string.h>

static void set_tile(GtkToggleButton *tile, gboolean pressed) {
  if (gtk_toggle_button_get_active(tile) == pressed) {
    return;
  }
  gtk_toggle_button_set_active(tile, pressed);
  if (pressed) {
    gtk_widget_add_css_class(GTK_WIDGET(tile), "suggested-action");
  } else {
    gtk_widget_remove_css_class(GTK_WIDGET(tile), "suggested-action");
  }
}

GtkWidget *pad_grid_new(pad_grid *pads) {
  pads->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  GtkWidget *heading = gtk_label_new("Pads");
  gtk_label_set_xalign(GTK_LABEL(heading), 0.0);
  gtk_widget_add_css_class(heading, "heading");
  gtk_box_append(GTK_BOX(pads->widget), heading);

  GtkGrid *grid = GTK_GRID(gtk_grid_new());
  gtk_grid_set_column_homogeneous(grid, TRUE);
  gtk_grid_set_column_spacing(grid, 6);
  gtk_grid_set_row_spacing(grid, 6);
  gtk_box_append(GTK_BOX(pads->widget), GTK_WIDGET(grid));
  for (int i = 0; i < PAD_COUNT; ++i) {
    char *label = g_strdup_printf("Pad %d", i + 1);
    GtkWidget *tile = gtk_toggle_button_new_with_label(label);
    g_free(label);
    pads->tiles[i] = GTK_TOGGLE_BUTTON(tile);
    GtkLabel *text = GTK_LABEL(gtk_button_get_child(GTK_BUTTON(tile)));
    gtk_label_set_ellipsize(text, PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(text, 18);
    gtk_widget_set_can_target(tile, FALSE);
    gtk_widget_set_focusable(tile, FALSE);
    gtk_widget_set_size_request(tile, -1, 42);
    gtk_grid_attach(grid, tile, i % 4, 1 - i / 4, 1, 1);
  }
  return pads->widget;
}

void pad_grid_set_name(pad_grid *pads, unsigned int slot, const char *name) {
  GtkWidget *tile = GTK_WIDGET(pads->tiles[slot]);
  gtk_label_set_text(GTK_LABEL(gtk_button_get_child(GTK_BUTTON(tile))), name);
  gtk_widget_set_tooltip_text(tile, name);
}

void pad_grid_highlight(pad_grid *pads, unsigned int slot) {
  set_tile(pads->tiles[slot], TRUE);
}

void pad_grid_reset(pad_grid *pads) {
  memset(pads->held, 0, sizeof(pads->held));
  for (int i = 0; i < PAD_COUNT; ++i) {
    set_tile(pads->tiles[i], FALSE);
  }
}

int pad_grid_event(pad_grid *pads, const device_profile *profile,
                   const char *client, const char *port,
                   const cc_scout_event *event) {
  if (!g_str_equal(client, profile->client) ||
      event->channel != profile->pad_channel ||
      (event->type != CC_SCOUT_NOTE_ON && event->type != CC_SCOUT_NOTE_OFF)) {
    return -1;
  }
  unsigned int note = 0;
  while (note < G_N_ELEMENTS(profile->pad_notes) &&
         profile->pad_notes[note] != event->number) {
    ++note;
  }
  if (note == G_N_ELEMENTS(profile->pad_notes)) {
    return -1;
  }
  guint8 port_bit;
  if (g_str_equal(port, profile->midi_port)) {
    port_bit = 1;
  } else if (g_str_equal(port, profile->daw_port)) {
    port_bit = 2;
  } else {
    return -1;
  }
  unsigned int pad = note % PAD_COUNT;
  gboolean was_pressed = gtk_toggle_button_get_active(pads->tiles[pad]);
  if (event->type == CC_SCOUT_NOTE_ON) {
    pads->held[note] |= port_bit;
  } else {
    pads->held[note] &= ~port_bit;
  }
  gboolean pressed = pads->held[pad] || pads->held[pad + PAD_COUNT];
  set_tile(pads->tiles[pad], pressed);
  return pressed && !was_pressed ? (int)pad : -1;
}
