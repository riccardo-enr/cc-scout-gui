#ifndef CC_SCOUT_GUI_PAD_GRID_H
#define CC_SCOUT_GUI_PAD_GRID_H

#include <gtk/gtk.h>

#include "cc_scout.h"
#include "device_profile.h"

typedef struct {
  GtkWidget *widget;
  GtkToggleButton *tiles[PAD_COUNT];
  guint8 held[PAD_COUNT * PAD_BANK_COUNT];
} pad_grid;

GtkWidget *pad_grid_new(pad_grid *pads);
void pad_grid_reset(pad_grid *pads);
void pad_grid_event(pad_grid *pads, const device_profile *profile,
                    const char *client, const char *port,
                    const cc_scout_event *event);

#endif
