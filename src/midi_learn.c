#include "midi_learn.h"

#include <errno.h>
#include <glib/gstdio.h>

struct midi_learn {
  const device_profile *profile;
  char *device_name;
  GKeyFile *bindings;
  char *bindings_path;
  char *bindings_error;
  char *learn_key;
  gboolean learning;
  gboolean learning_all;
  guint learned_count;
  char *learned_keys[KNOB_COUNT];
  GtkLabel *learn_status;
  GtkEntry *learn_name;
  GtkWidget *learn_save;
};

static void load_bindings(midi_learn *view) {
  view->bindings = g_key_file_new();
  view->bindings_path = g_build_filename(g_get_user_config_dir(),
                                         "cc-scout-gui", "knobs.ini", NULL);
  GError *error = NULL;
  if (!g_key_file_load_from_file(view->bindings, view->bindings_path,
                                 G_KEY_FILE_NONE, &error) &&
      !g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
    view->bindings_error =
        g_strdup_printf("Could not load knob names. %s", error->message);
  }
  g_clear_error(&error);
}

static char *binding_key(const char *client, const char *port,
                         const cc_scout_event *event) {
  char *client_key = g_uri_escape_string(client, NULL, FALSE);
  char *port_key = g_uri_escape_string(port, NULL, FALSE);
  char *key = g_strdup_printf("%s/%s/%u/%u", client_key, port_key,
                              event->channel, event->number);
  g_free(port_key);
  g_free(client_key);
  return key;
}

static void clear_learn(midi_learn *view) {
  view->learning = FALSE;
  view->learning_all = FALSE;
  for (guint i = 0; i < view->learned_count; ++i) {
    g_clear_pointer(&view->learned_keys[i], g_free);
  }
  view->learned_count = 0;
  g_clear_pointer(&view->learn_key, g_free);
  gtk_widget_set_visible(GTK_WIDGET(view->learn_name), TRUE);
  gtk_button_set_label(GTK_BUTTON(view->learn_save), "Save");
  gtk_editable_set_text(GTK_EDITABLE(view->learn_name), "");
  gtk_widget_set_sensitive(GTK_WIDGET(view->learn_name), FALSE);
  gtk_widget_set_sensitive(view->learn_save, FALSE);
}

static gboolean arm_learn(midi_learn *view) {
  clear_learn(view);
  if (view->bindings_error) {
    gtk_label_set_text(view->learn_status, view->bindings_error);
    return FALSE;
  }
  if (!view->device_name) {
    gtk_label_set_text(view->learn_status, "Connect a MIDI device first.");
    return FALSE;
  }
  view->learning = TRUE;
  return TRUE;
}

static void start_learn(GtkButton *button, gpointer userdata) {
  (void)button;
  midi_learn *view = userdata;
  if (arm_learn(view)) {
    gtk_label_set_text(view->learn_status, "Turn the knob you want to name.");
  }
}

static void prompt_next_knob(midi_learn *view) {
  guint index = view->learned_count;
  char *prompt = g_strdup_printf("Turn knob %u of %u: %s.", index + 1,
                                 (unsigned int)KNOB_COUNT,
                                 view->profile->knob_names[index]);
  gtk_label_set_text(view->learn_status, prompt);
  g_free(prompt);
}

static void start_learn_all(GtkButton *button, gpointer userdata) {
  (void)button;
  midi_learn *view = userdata;
  if (!arm_learn(view)) {
    return;
  }
  if (view->device_name &&
      !g_str_equal(view->device_name, view->profile->client)) {
    clear_learn(view);
    char *message = g_strdup_printf("Select %s first.", view->profile->client);
    gtk_label_set_text(view->learn_status, message);
    g_free(message);
    return;
  }
  view->learning_all = TRUE;
  gtk_widget_set_visible(GTK_WIDGET(view->learn_name), FALSE);
  gtk_button_set_label(GTK_BUTTON(view->learn_save), "Save All");
  prompt_next_knob(view);
}

static void capture_batch_knob(midi_learn *view, const char *key) {
  for (guint i = 0; i < view->learned_count; ++i) {
    if (g_str_equal(view->learned_keys[i], key)) {
      return;
    }
  }
  view->learned_keys[view->learned_count++] = g_strdup(key);
  if (view->learned_count < KNOB_COUNT) {
    prompt_next_knob(view);
    return;
  }
  view->learning = FALSE;
  gtk_label_set_text(view->learn_status,
                     "All eight knobs captured. Click Save All to apply.");
  gtk_widget_set_sensitive(view->learn_save, TRUE);
}

static void capture_knob(midi_learn *view, const char *key, const char *name,
                         const cc_scout_event *event) {
  view->learning = FALSE;
  view->learn_key = g_strdup(key);
  char *text = g_strdup_printf("Captured channel %u, CC %u. Name this knob.",
                               event->channel, event->number);
  gtk_label_set_text(view->learn_status, text);
  g_free(text);
  gtk_widget_set_sensitive(GTK_WIDGET(view->learn_name), TRUE);
  gtk_widget_set_sensitive(view->learn_save, TRUE);
  gtk_editable_set_text(GTK_EDITABLE(view->learn_name), name ? name : "");
  gtk_widget_grab_focus(GTK_WIDGET(view->learn_name));
}

static gboolean write_bindings(midi_learn *view, GKeyFile *bindings,
                               GError **error) {
  char *directory = g_path_get_dirname(view->bindings_path);
  int result = g_mkdir_with_parents(directory, 0700);
  int directory_error = errno;
  g_free(directory);
  if (result < 0) {
    g_set_error_literal(error, G_FILE_ERROR,
                        g_file_error_from_errno(directory_error),
                        g_strerror(directory_error));
    return FALSE;
  }
  gsize size;
  char *data = g_key_file_to_data(bindings, &size, NULL);
  gboolean saved =
      g_file_set_contents_full(view->bindings_path, data, size,
                               G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
  g_free(data);
  return saved;
}

static void save_learn(GtkButton *button, gpointer userdata) {
  (void)button;
  midi_learn *view = userdata;
  gboolean batch = view->learning_all;
  if (batch ? view->learned_count != KNOB_COUNT : !view->learn_key) {
    return;
  }
  char *name = NULL;
  if (!batch) {
    name = g_strdup(gtk_editable_get_text(GTK_EDITABLE(view->learn_name)));
    g_strstrip(name);
    if (!*name) {
      gtk_label_set_text(view->learn_status, "Enter a knob name first.");
      g_free(name);
      return;
    }
  }

  GKeyFile *updated = g_key_file_new();
  gsize size;
  char *data = g_key_file_to_data(view->bindings, &size, NULL);
  GError *error = NULL;
  gboolean ready = !size || g_key_file_load_from_data(updated, data, size,
                                                      G_KEY_FILE_NONE, &error);
  g_free(data);
  if (ready) {
    if (batch) {
      for (guint i = 0; i < KNOB_COUNT; ++i) {
        g_key_file_set_string(updated, "Knobs", view->learned_keys[i],
                              view->profile->knob_names[i]);
      }
    } else {
      g_key_file_set_string(updated, "Knobs", view->learn_key, name);
    }
    ready = write_bindings(view, updated, &error);
  }
  if (ready) {
    g_key_file_unref(view->bindings);
    view->bindings = g_key_file_ref(updated);
    char *text = batch ? g_strdup("Saved all eight knobs.")
                       : g_strdup_printf("Saved %s.", name);
    clear_learn(view);
    gtk_label_set_text(view->learn_status, text);
    g_free(text);
  } else {
    char *text =
        g_strdup_printf("Could not save knob names. %s", error->message);
    gtk_label_set_text(view->learn_status, text);
    g_free(text);
    g_error_free(error);
  }
  g_key_file_unref(updated);
  g_free(name);
}

void midi_learn_reset(midi_learn *view) {
  clear_learn(view);
  gtk_label_set_text(view->learn_status, "Name a knob for the live feed.");
}

static gboolean close_settings(GtkWindow *window, gpointer userdata) {
  (void)window;
  midi_learn_reset(userdata);
  return FALSE;
}

midi_learn *midi_learn_new(AdwPreferencesPage *page, GtkWindow *settings,
                           const device_profile *profile) {
  midi_learn *view = g_new0(midi_learn, 1);
  view->profile = profile;
  load_bindings(view);
  AdwPreferencesGroup *learn_group =
      ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(learn_group, "MIDI Learn");
  adw_preferences_page_add(page, learn_group);

  AdwActionRow *actions = ADW_ACTION_ROW(adw_action_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(actions), "Knob names");
  GtkWidget *learn_button = gtk_button_new_with_label("Learn");
  GtkWidget *learn_all_button = gtk_button_new_with_label("Learn All Knobs");
  gtk_widget_set_valign(learn_button, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(learn_all_button, GTK_ALIGN_CENTER);
  adw_action_row_add_suffix(actions, learn_button);
  adw_action_row_add_suffix(actions, learn_all_button);
  adw_preferences_group_add(learn_group, GTK_WIDGET(actions));

  GtkWidget *editor = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  adw_preferences_group_add(learn_group, editor);
  view->learn_status =
      GTK_LABEL(gtk_label_new("Name a knob for the live feed."));
  gtk_label_set_xalign(view->learn_status, 0.0);
  gtk_label_set_wrap(view->learn_status, TRUE);
  gtk_box_append(GTK_BOX(editor), GTK_WIDGET(view->learn_status));

  GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append(GTK_BOX(editor), controls);
  view->learn_name = GTK_ENTRY(gtk_entry_new());
  gtk_entry_set_placeholder_text(view->learn_name, "Knob name");
  gtk_entry_set_max_length(view->learn_name, 64);
  gtk_widget_set_hexpand(GTK_WIDGET(view->learn_name), TRUE);
  gtk_box_append(GTK_BOX(controls), GTK_WIDGET(view->learn_name));
  view->learn_save = gtk_button_new_with_label("Save");
  gtk_box_append(GTK_BOX(controls), view->learn_save);
  clear_learn(view);
  g_signal_connect(learn_button, "clicked", G_CALLBACK(start_learn), view);
  g_signal_connect(learn_all_button, "clicked", G_CALLBACK(start_learn_all),
                   view);
  g_signal_connect(view->learn_save, "clicked", G_CALLBACK(save_learn), view);
  g_signal_connect(settings, "close-request", G_CALLBACK(close_settings), view);
  return view;
}

void midi_learn_set_device(midi_learn *view, const char *name) {
  g_free(view->device_name);
  view->device_name = g_strdup(name);
}

char *midi_learn_event(midi_learn *view, const char *client, const char *port,
                       const cc_scout_event *event) {
  if (event->type != CC_SCOUT_CONTROL_CHANGE) {
    return NULL;
  }
  char *key = binding_key(client, port, event);
  char *name = g_key_file_get_string(view->bindings, "Knobs", key, NULL);
  if (view->learning) {
    if (!view->learning_all) {
      capture_knob(view, key, name, event);
    } else if (g_str_equal(client, view->profile->client)) {
      capture_batch_knob(view, key);
    }
  }
  g_free(key);
  return name;
}

void midi_learn_free(midi_learn *view) {
  g_free(view->device_name);
  g_key_file_unref(view->bindings);
  g_free(view->bindings_path);
  g_free(view->bindings_error);
  g_free(view->learn_key);
  for (guint i = 0; i < view->learned_count; ++i) {
    g_free(view->learned_keys[i]);
  }
  g_free(view);
}
