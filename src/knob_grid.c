#include "knob_grid.h"

GtkWidget *knob_grid_new(knob_grid *knobs) {
  knobs->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  GtkWidget *heading = gtk_label_new("Knobs");
  gtk_label_set_xalign(GTK_LABEL(heading), 0.0);
  gtk_widget_add_css_class(heading, "heading");
  gtk_box_append(GTK_BOX(knobs->widget), heading);

  GtkGrid *grid = GTK_GRID(gtk_grid_new());
  gtk_grid_set_column_homogeneous(grid, TRUE);
  gtk_grid_set_column_spacing(grid, 6);
  gtk_grid_set_row_spacing(grid, 6);
  gtk_box_append(GTK_BOX(knobs->widget), GTK_WIDGET(grid));
  for (int i = 0; i < KNOB_COUNT; ++i) {
    GtkWidget *tile = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(tile, "card");
    gtk_widget_set_size_request(tile, 64, 44);
    gtk_widget_set_valign(tile, GTK_ALIGN_FILL);
    knobs->names[i] = GTK_LABEL(gtk_label_new(NULL));
    gtk_label_set_ellipsize(knobs->names[i], PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(knobs->names[i], 18);
    knobs->directions[i] = GTK_LABEL(gtk_label_new("-"));
    gtk_widget_add_css_class(GTK_WIDGET(knobs->directions[i]), "caption");
    gtk_box_append(GTK_BOX(tile), GTK_WIDGET(knobs->names[i]));
    gtk_box_append(GTK_BOX(tile), GTK_WIDGET(knobs->directions[i]));
    gtk_grid_attach(grid, tile, i % 4, i / 4, 1, 1);
  }
  return knobs->widget;
}

void knob_grid_reset(knob_grid *knobs) {
  for (int i = 0; i < KNOB_COUNT; ++i) {
    gtk_label_set_text(knobs->directions[i], "-");
  }
}

void knob_grid_turn(knob_grid *knobs, unsigned int slot, int value) {
  if (value <= 0 || value >= MIDI_VALUE_COUNT) {
    return;
  }
  int steps = value < MIDI_VALUE_COUNT / 2 ? value : value - MIDI_VALUE_COUNT;
  char text[16];
  g_snprintf(text, sizeof(text), "%s %+d", steps > 0 ? "CW" : "CCW", steps);
  gtk_label_set_text(knobs->directions[slot], text);
}
