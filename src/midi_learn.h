#ifndef CC_SCOUT_GUI_MIDI_LEARN_H
#define CC_SCOUT_GUI_MIDI_LEARN_H

#include <adwaita.h>

#include "cc_scout.h"
#include "device_profile.h"

typedef struct midi_learn midi_learn;

midi_learn *midi_learn_new(AdwPreferencesPage *page, GtkWindow *settings,
                           const device_profile *profile,
                           void (*changed)(gpointer), gpointer changed_data);
void midi_learn_free(midi_learn *view);
void midi_learn_set_device(midi_learn *view, const char *name);
void midi_learn_reset(midi_learn *view);
char *midi_learn_knob_name(midi_learn *view, unsigned int slot);
char *midi_learn_event(midi_learn *view, const char *client, const char *port,
                       const cc_scout_event *event, int *slot);

#endif
