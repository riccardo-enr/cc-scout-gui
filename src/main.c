#include <adwaita.h>
#include <errno.h>
#include <glib/gstdio.h>

#include "cc_scout.h"

static const char *const knob_names[] = {
    "Division", "Swing", "Mode", "Oct", "Latch", "Sync", "Gate", "BPM",
};

typedef struct {
  GtkWindow *settings;
  GtkListBox *devices;
  GtkLabel *status;
  GtkTextView *feed;
  GVariant *sources;
  GVariant *ports;
  cc_scout *monitor;
  char *device_name;
  guint8 device_id;
  guint scan_timer;
  GSource *read_source;
  GKeyFile *bindings;
  char *bindings_path;
  char *bindings_error;
  char *learn_key;
  gboolean learning;
  gboolean learning_all;
  guint learned_count;
  char *learned_keys[G_N_ELEMENTS(knob_names)];
  GtkLabel *learn_status;
  GtkEntry *learn_name;
  GtkWidget *learn_save;
} monitor_view;

static void collect_source(cc_scout_address address, const char *client_name,
                           const char *port_name, void *userdata) {
  GVariantBuilder *sources = userdata;
  char *client = g_utf8_make_valid(client_name, -1);
  char *port = g_utf8_make_valid(port_name, -1);
  g_variant_builder_add(sources, "(yyss)", address.client, address.port, client,
                        port);
  g_free(port);
  g_free(client);
}

static void close_monitor(monitor_view *view) {
  if (view->read_source) {
    g_source_destroy(view->read_source);
    g_clear_pointer(&view->read_source, g_source_unref);
  }
  if (view->monitor) {
    cc_scout_close(view->monitor);
    view->monitor = NULL;
  }
}

static void report_error(monitor_view *view, const char *message, int result) {
  char *text = g_strdup_printf("%s %s", message, cc_scout_error(result));
  gtk_label_set_text(view->status, text);
  g_free(text);
}

static gboolean find_source(GVariant *sources, cc_scout_address address,
                            const char **client, const char **port) {
  GVariantIter iter;
  g_variant_iter_init(&iter, sources);
  guint8 client_id, port_id;
  while (g_variant_iter_next(&iter, "(yy&s&s)", &client_id, &port_id, client,
                             port)) {
    if (client_id == address.client && port_id == address.port) {
      return TRUE;
    }
  }
  return FALSE;
}

static const char *control_label(const char *client, const char *port,
                                 const cc_scout_event *event) {
  if (!g_str_equal(client, "MPK mini IV")) {
    return NULL;
  }

  gboolean midi = g_str_equal(port, "MPK mini IV MIDI Port");
  gboolean daw = g_str_equal(port, "MPK mini IV DAW Port");
  if (event->type == CC_SCOUT_CONTROL_CHANGE) {
    return daw && event->channel == 1 &&
                   (event->number == 24 || event->number == 25)
               ? "Knob"
               : NULL;
  }
  if (event->channel == 10 && (midi || daw)) {
    return "Pad";
  }
  return event->channel == 1 && midi ? "Key" : NULL;
}

static void load_bindings(monitor_view *view) {
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

static void clear_learn(monitor_view *view) {
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

static gboolean arm_learn(monitor_view *view) {
  clear_learn(view);
  if (view->bindings_error) {
    gtk_label_set_text(view->learn_status, view->bindings_error);
    return FALSE;
  }
  if (!view->monitor) {
    gtk_label_set_text(view->learn_status, "Connect a MIDI device first.");
    return FALSE;
  }
  view->learning = TRUE;
  return TRUE;
}

static void start_learn(GtkButton *button, gpointer userdata) {
  (void)button;
  monitor_view *view = userdata;
  if (arm_learn(view)) {
    gtk_label_set_text(view->learn_status, "Turn the knob you want to name.");
  }
}

static void prompt_next_knob(monitor_view *view) {
  guint index = view->learned_count;
  char *prompt = g_strdup_printf("Turn knob %u of %u: %s.", index + 1,
                                 (unsigned int)G_N_ELEMENTS(knob_names),
                                 knob_names[index]);
  gtk_label_set_text(view->learn_status, prompt);
  g_free(prompt);
}

static void start_learn_all(GtkButton *button, gpointer userdata) {
  (void)button;
  monitor_view *view = userdata;
  if (!arm_learn(view)) {
    return;
  }
  if (view->device_name && !g_str_equal(view->device_name, "MPK mini IV")) {
    clear_learn(view);
    gtk_label_set_text(view->learn_status, "Select the MPK mini IV first.");
    return;
  }
  view->learning_all = TRUE;
  gtk_widget_set_visible(GTK_WIDGET(view->learn_name), FALSE);
  gtk_button_set_label(GTK_BUTTON(view->learn_save), "Save All");
  prompt_next_knob(view);
}

static void capture_batch_knob(monitor_view *view, const char *key) {
  for (guint i = 0; i < view->learned_count; ++i) {
    if (g_str_equal(view->learned_keys[i], key)) {
      return;
    }
  }
  view->learned_keys[view->learned_count++] = g_strdup(key);
  if (view->learned_count < G_N_ELEMENTS(knob_names)) {
    prompt_next_knob(view);
    return;
  }
  view->learning = FALSE;
  gtk_label_set_text(view->learn_status,
                     "All eight knobs captured. Click Save All to apply.");
  gtk_widget_set_sensitive(view->learn_save, TRUE);
}

static void capture_knob(monitor_view *view, const char *key, const char *name,
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

static gboolean write_bindings(monitor_view *view, GKeyFile *bindings,
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
  monitor_view *view = userdata;
  gboolean batch = view->learning_all;
  if (batch ? view->learned_count != G_N_ELEMENTS(knob_names)
            : !view->learn_key) {
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
      for (guint i = 0; i < G_N_ELEMENTS(knob_names); ++i) {
        g_key_file_set_string(updated, "Knobs", view->learned_keys[i],
                              knob_names[i]);
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

static void reset_learn(monitor_view *view) {
  clear_learn(view);
  gtk_label_set_text(view->learn_status, "Name a knob for the live feed.");
}

static gboolean close_settings(GtkWindow *window, gpointer userdata) {
  (void)window;
  reset_learn(userdata);
  return FALSE;
}

static gboolean read_events(monitor_view *view) {
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(view->feed);
  GString *lines = g_string_new(NULL);
  int i;
  for (i = 0; i < 64; ++i) {
    cc_scout_event event;
    int result = cc_scout_read_event(view->monitor, &event);
    if (result == -EINTR) {
      continue;
    }
    if (result == -EAGAIN) {
      break;
    }
    if (result < 0) {
      report_error(view, "Could not read MIDI events.", result);
      close_monitor(view);
      break;
    }
    g_string_append_printf(lines, "%u:%u  ", (unsigned int)event.source.client,
                           (unsigned int)event.source.port);
    const char *client, *port;
    if (find_source(view->sources, event.source, &client, &port)) {
      char *name = NULL;
      if (event.type == CC_SCOUT_CONTROL_CHANGE) {
        char *key = binding_key(client, port, &event);
        name = g_key_file_get_string(view->bindings, "Knobs", key, NULL);
        if (view->learning) {
          if (!view->learning_all) {
            capture_knob(view, key, name, &event);
          } else if (g_str_equal(client, "MPK mini IV")) {
            capture_batch_knob(view, key);
          }
        }
        g_free(key);
      }
      const char *label = name ? name : control_label(client, port, &event);
      if (label) {
        g_string_append_printf(lines, "%s  ", label);
      }
      g_free(name);
    }
    if (event.type == CC_SCOUT_CONTROL_CHANGE) {
      g_string_append_printf(lines, "cc ch=%u controller=%u value=%d\n",
                             event.channel, event.number, event.value);
    } else {
      const char *type =
          event.type == CC_SCOUT_NOTE_ON ? "note-on" : "note-off";
      g_string_append_printf(lines, "%s ch=%u note=%u velocity=%d\n", type,
                             event.channel, event.number, event.value);
    }
  }

  if (lines->len) {
    GtkTextIter start, end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end, lines->str, (int)lines->len);
    int line_count = gtk_text_buffer_get_line_count(buffer);
    if (line_count > 101) {
      gtk_text_buffer_get_start_iter(buffer, &start);
      gtk_text_buffer_get_iter_at_line(buffer, &end, line_count - 101);
      gtk_text_buffer_delete(buffer, &start, &end);
    }
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_view_scroll_to_iter(view->feed, &end, 0.0, FALSE, 0.0, 0.0);
  }
  g_string_free(lines, TRUE);
  return i == 64;
}

typedef struct {
  GSource source;
  monitor_view *view;
  struct pollfd *fds;
  gpointer *tags;
  unsigned int count;
} midi_watch;

static gboolean midi_watch_check(GSource *source) {
  midi_watch *watch = (midi_watch *)source;
  for (unsigned int i = 0; i < watch->count; ++i) {
    if (g_source_query_unix_fd(source, watch->tags[i])) {
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean midi_watch_dispatch(GSource *source, GSourceFunc callback,
                                    gpointer userdata) {
  (void)callback;
  (void)userdata;
  midi_watch *watch = (midi_watch *)source;
  monitor_view *view = watch->view;
  gboolean buffered = g_source_get_ready_time(source) == 0;
  g_source_set_ready_time(source, -1);
  for (unsigned int i = 0; i < watch->count; ++i) {
    watch->fds[i].revents = g_source_query_unix_fd(source, watch->tags[i]);
  }
  int flags = cc_scout_poll_revents(view->monitor, watch->fds, watch->count);
  if (flags < 0 || (flags & (POLLERR | POLLHUP | POLLNVAL))) {
    report_error(view, "Could not watch MIDI events.",
                 flags < 0 ? flags : -EIO);
    close_monitor(view);
    return G_SOURCE_REMOVE;
  }
  if ((buffered || (flags & POLLIN)) && read_events(view)) {
    g_source_set_ready_time(source, 0);
  }
  return view->monitor ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static void midi_watch_finalize(GSource *source) {
  midi_watch *watch = (midi_watch *)source;
  g_free(watch->tags);
  g_free(watch->fds);
}

static int start_midi_watch(monitor_view *view) {
  static GSourceFuncs funcs = {
      .check = midi_watch_check,
      .dispatch = midi_watch_dispatch,
      .finalize = midi_watch_finalize,
  };
  int count = cc_scout_poll_descriptor_count(view->monitor);
  if (count <= 0) {
    return count < 0 ? count : -EIO;
  }
  GSource *source = g_source_new(&funcs, sizeof(midi_watch));
  midi_watch *watch = (midi_watch *)source;
  watch->view = view;
  watch->fds = g_new0(struct pollfd, count);
  int filled = cc_scout_poll_descriptors(view->monitor, watch->fds, count);
  if (filled <= 0 || filled > count) {
    g_source_unref(source);
    return filled < 0 ? filled : -EIO;
  }
  watch->count = filled;
  watch->tags = g_new(gpointer, filled);
  for (unsigned int i = 0; i < watch->count; ++i) {
    watch->tags[i] =
        g_source_add_unix_fd(source, watch->fds[i].fd, watch->fds[i].events);
  }
  view->read_source = source;
  g_source_attach(source, NULL);
  return 0;
}

static void update_monitor(monitor_view *view) {
  const char *name = view->device_name ? view->device_name : "MPK mini IV";
  GVariantIter iter;
  guint8 id, port_id;
  const char *client, *port;
  int selected_id = -1;
  g_variant_iter_init(&iter, view->sources);
  while (
      g_variant_iter_next(&iter, "(yy&s&s)", &id, &port_id, &client, &port)) {
    if (g_str_equal(client, name)) {
      if (selected_id < 0) {
        selected_id = id;
      }
      if (view->device_name && id == view->device_id) {
        selected_id = id;
        break;
      }
    }
  }

  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("a(yy)"));
  g_variant_iter_init(&iter, view->sources);
  while (
      g_variant_iter_next(&iter, "(yy&s&s)", &id, &port_id, &client, &port)) {
    if (id != selected_id || !g_str_equal(client, name)) {
      continue;
    }
    if (g_str_equal(name, "MPK mini IV") &&
        !g_str_equal(port, "MPK mini IV MIDI Port") &&
        !g_str_equal(port, "MPK mini IV DAW Port")) {
      continue;
    }
    g_variant_builder_add(&builder, "(yy)", id, port_id);
  }

  GVariant *ports = g_variant_ref_sink(g_variant_builder_end(&builder));
  gboolean changed = !view->ports || !g_variant_equal(view->ports, ports);
  if (!changed && view->monitor) {
    g_variant_unref(ports);
    return;
  }
  close_monitor(view);
  g_clear_pointer(&view->ports, g_variant_unref);
  view->ports = ports;
  if (changed) {
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(view->feed), "", 0);
  }
  if (!g_variant_n_children(ports)) {
    char *text =
        g_strdup_printf("Waiting for %s. Change device in Settings.", name);
    gtk_label_set_text(view->status, text);
    g_free(text);
    return;
  }

  int result = cc_scout_open(&view->monitor);
  if (result >= 0) {
    result = cc_scout_prepare_monitor(view->monitor);
  }
  if (result >= 0) {
    result = cc_scout_set_nonblocking(view->monitor);
  }
  g_variant_iter_init(&iter, ports);
  while (result >= 0 && g_variant_iter_next(&iter, "(yy)", &id, &port_id)) {
    result = cc_scout_subscribe(view->monitor, (cc_scout_address){id, port_id});
  }
  if (result >= 0) {
    result = start_midi_watch(view);
  }
  if (result < 0) {
    report_error(view, "Could not start MIDI monitoring.", result);
    close_monitor(view);
    return;
  }
  char *text = g_strdup_printf("Monitoring %s", name);
  gtk_label_set_text(view->status, text);
  g_free(text);
}

static void choose_device(GtkListBox *list, GtkListBoxRow *row,
                          gpointer userdata) {
  (void)list;
  monitor_view *view = userdata;
  g_free(view->device_name);
  view->device_name = g_strdup(g_object_get_data(G_OBJECT(row), "device-name"));
  view->device_id =
      GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "device-id"));
  update_monitor(view);
  reset_learn(view);
  gtk_widget_set_visible(GTK_WIDGET(view->settings), FALSE);
}

static void add_device(GtkListBox *list, const char *name, guint8 id) {
  AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                name ? name : "Automatic (MPK mini IV)");
  if (name) {
    char *subtitle = g_strdup_printf("ALSA client %u", (unsigned int)id);
    adw_action_row_set_subtitle(row, subtitle);
    g_free(subtitle);
    g_object_set_data_full(G_OBJECT(row), "device-name", g_strdup(name),
                           g_free);
    g_object_set_data(G_OBJECT(row), "device-id", GUINT_TO_POINTER(id));
  }
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
  gtk_list_box_append(list, GTK_WIDGET(row));
}

static gboolean refresh_sources(gpointer userdata) {
  monitor_view *view = userdata;
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("a(yyss)"));
  cc_scout *scout;
  int result = cc_scout_open(&scout);
  if (result >= 0) {
    result = cc_scout_list_sources(scout, collect_source, &builder);
    cc_scout_close(scout);
  }

  if (result < 0) {
    g_variant_builder_clear(&builder);
    if (!view->monitor) {
      report_error(view, "Could not list MIDI sources.", result);
    }
    return G_SOURCE_CONTINUE;
  }

  GVariant *sources = g_variant_ref_sink(g_variant_builder_end(&builder));
  if (!view->sources || !g_variant_equal(view->sources, sources)) {
    g_clear_pointer(&view->sources, g_variant_unref);
    view->sources = g_variant_ref(sources);
    GtkListBoxRow *row;
    while ((row = gtk_list_box_get_row_at_index(view->devices, 0))) {
      gtk_list_box_remove(view->devices, GTK_WIDGET(row));
    }
    add_device(view->devices, NULL, 0);
    gboolean seen[256] = {FALSE};
    GVariantIter iter;
    guint8 id, port_id;
    const char *client, *port;
    g_variant_iter_init(&iter, sources);
    while (
        g_variant_iter_next(&iter, "(yy&s&s)", &id, &port_id, &client, &port)) {
      if (!seen[id]) {
        seen[id] = TRUE;
        add_device(view->devices, client, id);
      }
    }
  }
  g_variant_unref(sources);
  update_monitor(view);
  return G_SOURCE_CONTINUE;
}

static void free_monitor_view(gpointer userdata) {
  monitor_view *view = userdata;
  g_source_remove(view->scan_timer);
  close_monitor(view);
  g_clear_pointer(&view->ports, g_variant_unref);
  g_clear_pointer(&view->sources, g_variant_unref);
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

static void open_settings(GtkButton *button, gpointer userdata) {
  (void)button;
  gtk_window_present(GTK_WINDOW(userdata));
}

static void build_settings(monitor_view *monitor, GtkWindow *parent) {
  monitor->settings = GTK_WINDOW(adw_window_new());
  gtk_window_set_title(monitor->settings, "Settings");
  gtk_window_set_default_size(monitor->settings, 460, 500);
  gtk_window_set_transient_for(monitor->settings, parent);
  gtk_window_set_modal(monitor->settings, TRUE);
  gtk_window_set_destroy_with_parent(monitor->settings, TRUE);
  gtk_window_set_hide_on_close(monitor->settings, TRUE);

  AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
  adw_toolbar_view_add_top_bar(toolbar, adw_header_bar_new());
  adw_window_set_content(ADW_WINDOW(monitor->settings), GTK_WIDGET(toolbar));
  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
  adw_toolbar_view_set_content(toolbar, GTK_WIDGET(page));

  AdwPreferencesGroup *device_group =
      ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(device_group, "MIDI Device");
  adw_preferences_page_add(page, device_group);
  monitor->devices = GTK_LIST_BOX(gtk_list_box_new());
  gtk_widget_add_css_class(GTK_WIDGET(monitor->devices), "boxed-list");
  gtk_list_box_set_selection_mode(monitor->devices, GTK_SELECTION_NONE);
  gtk_list_box_set_activate_on_single_click(monitor->devices, TRUE);
  adw_preferences_group_add(device_group, GTK_WIDGET(monitor->devices));
  g_signal_connect(monitor->devices, "row-activated", G_CALLBACK(choose_device),
                   monitor);

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
  monitor->learn_status =
      GTK_LABEL(gtk_label_new("Name a knob for the live feed."));
  gtk_label_set_xalign(monitor->learn_status, 0.0);
  gtk_label_set_wrap(monitor->learn_status, TRUE);
  gtk_box_append(GTK_BOX(editor), GTK_WIDGET(monitor->learn_status));

  GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append(GTK_BOX(editor), controls);
  monitor->learn_name = GTK_ENTRY(gtk_entry_new());
  gtk_entry_set_placeholder_text(monitor->learn_name, "Knob name");
  gtk_entry_set_max_length(monitor->learn_name, 64);
  gtk_widget_set_hexpand(GTK_WIDGET(monitor->learn_name), TRUE);
  gtk_box_append(GTK_BOX(controls), GTK_WIDGET(monitor->learn_name));
  monitor->learn_save = gtk_button_new_with_label("Save");
  gtk_box_append(GTK_BOX(controls), monitor->learn_save);
  clear_learn(monitor);
  g_signal_connect(learn_button, "clicked", G_CALLBACK(start_learn), monitor);
  g_signal_connect(learn_all_button, "clicked", G_CALLBACK(start_learn_all),
                   monitor);
  g_signal_connect(monitor->learn_save, "clicked", G_CALLBACK(save_learn),
                   monitor);
  g_signal_connect(monitor->settings, "close-request",
                   G_CALLBACK(close_settings), monitor);
}

static void activate(GtkApplication *app, gpointer userdata) {
  (void)userdata;
  GtkWindow *window = gtk_application_get_active_window(app);
  if (window) {
    gtk_window_present(window);
    return;
  }

  window = GTK_WINDOW(adw_application_window_new(app));
  gtk_window_set_title(window, "CC Scout");
  gtk_window_set_default_size(window, 640, 440);
  monitor_view *monitor = g_new0(monitor_view, 1);
  load_bindings(monitor);

  AdwToolbarView *view = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
  AdwHeaderBar *header = ADW_HEADER_BAR(adw_header_bar_new());
  adw_toolbar_view_add_top_bar(view, GTK_WIDGET(header));
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(window),
                                     GTK_WIDGET(view));

  build_settings(monitor, window);

  GtkWidget *settings_button =
      gtk_button_new_from_icon_name("emblem-system-symbolic");
  gtk_widget_set_tooltip_text(settings_button, "MIDI device settings");
  adw_header_bar_pack_end(header, settings_button);
  g_signal_connect(settings_button, "clicked", G_CALLBACK(open_settings),
                   monitor->settings);

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top(content, 16);
  gtk_widget_set_margin_bottom(content, 16);
  gtk_widget_set_margin_start(content, 16);
  gtk_widget_set_margin_end(content, 16);
  adw_toolbar_view_set_content(view, content);
  monitor->status = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_xalign(monitor->status, 0.0);
  gtk_label_set_wrap(monitor->status, TRUE);
  gtk_box_append(GTK_BOX(content), GTK_WIDGET(monitor->status));

  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_box_append(GTK_BOX(content), scroll);
  monitor->feed = GTK_TEXT_VIEW(gtk_text_view_new());
  gtk_text_view_set_editable(monitor->feed, FALSE);
  gtk_text_view_set_cursor_visible(monitor->feed, FALSE);
  gtk_text_view_set_monospace(monitor->feed, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                GTK_WIDGET(monitor->feed));

  refresh_sources(monitor);
  // ponytail: poll for ports and events - upgrade when the core exposes ALSA
  // watches.
  monitor->scan_timer = g_timeout_add_seconds(1, refresh_sources, monitor);
  g_object_set_data_full(G_OBJECT(window), "monitor-view", monitor,
                         free_monitor_view);
  gtk_window_present(window);
}

int main(int argc, char **argv) {
  AdwApplication *app = adw_application_new("io.github.riccardo_enr.CCScoutGui",
                                            G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
  int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return status;
}
