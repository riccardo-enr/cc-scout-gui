#ifndef CC_SCOUT_GUI_KNOB_GRID_H
#define CC_SCOUT_GUI_KNOB_GRID_H

#include <gtk/gtk.h>

#include "device_profile.h"

typedef struct {
  GtkWidget *widget;
  GtkLabel *names[KNOB_COUNT];
  GtkLabel *directions[KNOB_COUNT];
} knob_grid;

GtkWidget *knob_grid_new(knob_grid *knobs);
void knob_grid_reset(knob_grid *knobs);
void knob_grid_turn(knob_grid *knobs, unsigned int slot, int value);

#endif
