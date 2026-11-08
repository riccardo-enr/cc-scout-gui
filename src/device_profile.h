#ifndef CC_SCOUT_GUI_DEVICE_PROFILE_H
#define CC_SCOUT_GUI_DEVICE_PROFILE_H

#include <gio/gio.h>

enum {
  PAD_COUNT = 8,
  PAD_BANK_COUNT = 2,
  KNOB_COUNT = 8,
  MIDI_VALUE_COUNT = 128
};

typedef struct {
  char *client;
  char *midi_port;
  char *daw_port;
  unsigned int key_channel;
  unsigned int pad_channel;
  unsigned int knob_channel;
  unsigned int pad_notes[PAD_COUNT * PAD_BANK_COUNT];
  gboolean knob_ccs[MIDI_VALUE_COUNT];
  char *knob_names[KNOB_COUNT];
} device_profile;

device_profile *device_profile_load(GError **error);
void device_profile_free(device_profile *profile);

#endif
