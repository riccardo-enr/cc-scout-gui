#include <adwaita.h>
#include <errno.h>

#include "cc_scout.h"
#include "device_profile.h"
#include "knob_grid.h"
#include "midi_learn.h"
#include "pad_grid.h"

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
  const device_profile *profile;
  midi_learn *learn;
  pad_grid pads;
  knob_grid knobs;
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
    midi_learn_set_device(view->learn, NULL);
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

static const char *control_label(const device_profile *profile,
                                 const char *client, const char *port,
                                 const cc_scout_event *event) {
  if (!g_str_equal(client, profile->client)) {
    return NULL;
  }

  gboolean midi = g_str_equal(port, profile->midi_port);
  gboolean daw = g_str_equal(port, profile->daw_port);
  if (event->type == CC_SCOUT_CONTROL_CHANGE) {
    return NULL;
  }
  if (event->channel == profile->pad_channel && (midi || daw)) {
    return "Pad";
  }
  return event->channel == profile->key_channel && midi ? "Key" : NULL;
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
      pad_grid_reset(&view->pads);
      knob_grid_reset(&view->knobs);
      break;
    }
    g_string_append_printf(lines, "%u:%u  ", (unsigned int)event.source.client,
                           (unsigned int)event.source.port);
    const char *client, *port;
    if (find_source(view->sources, event.source, &client, &port)) {
      int pad =
          pad_grid_event(&view->pads, view->profile, client, port, &event);
      if (pad >= 0) {
        midi_learn_pad_pressed(view->learn, pad);
      }
      int slot;
      char *name = midi_learn_event(view->learn, client, port, &event, &slot);
      if (slot >= 0) {
        knob_grid_turn(&view->knobs, slot, event.value);
      }
      const char *label = name;
      if (!label) {
        label = slot >= 0 ? "Knob"
                          : control_label(view->profile, client, port, &event);
      }
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
    pad_grid_reset(&view->pads);
    knob_grid_reset(&view->knobs);
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
  const char *name =
      view->device_name ? view->device_name : view->profile->client;
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
    if (g_str_equal(name, view->profile->client) &&
        !g_str_equal(port, view->profile->midi_port) &&
        !g_str_equal(port, view->profile->daw_port)) {
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
  pad_grid_reset(&view->pads);
  knob_grid_reset(&view->knobs);
  gboolean show_controller = g_str_equal(name, view->profile->client);
  gtk_widget_set_visible(view->pads.widget, show_controller);
  gtk_widget_set_visible(view->knobs.widget, show_controller);
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
  midi_learn_set_device(view->learn, name);
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
  midi_learn_reset(view->learn);
  gtk_widget_set_visible(GTK_WIDGET(view->settings), FALSE);
}

static void add_device(GtkListBox *list, const char *name, guint8 id,
                       const device_profile *profile) {
  AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  char *title = name ? g_strdup(name)
                     : g_strdup_printf("Automatic (%s)", profile->client);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  g_free(title);
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
    add_device(view->devices, NULL, 0, view->profile);
    gboolean seen[256] = {FALSE};
    GVariantIter iter;
    guint8 id, port_id;
    const char *client, *port;
    g_variant_iter_init(&iter, sources);
    while (
        g_variant_iter_next(&iter, "(yy&s&s)", &id, &port_id, &client, &port)) {
      if (!seen[id]) {
        seen[id] = TRUE;
        add_device(view->devices, client, id, view->profile);
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
  midi_learn_free(view->learn);
  g_free(view);
}

static void open_settings(GtkButton *button, gpointer userdata) {
  (void)button;
  gtk_window_present(GTK_WINDOW(userdata));
}

static void refresh_control_names(gpointer userdata) {
  monitor_view *view = userdata;
  for (unsigned int i = 0; i < KNOB_COUNT; ++i) {
    char *name = midi_learn_knob_name(view->learn, i);
    gtk_label_set_text(view->knobs.names[i], name);
    gtk_widget_set_tooltip_text(GTK_WIDGET(view->knobs.names[i]), name);
    g_free(name);
  }
  for (unsigned int i = 0; i < PAD_COUNT; ++i) {
    char *name = midi_learn_pad_name(view->learn, i);
    pad_grid_set_name(&view->pads, i, name);
    g_free(name);
  }
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

  monitor->learn = midi_learn_new(page, monitor->settings, monitor->profile,
                                  refresh_control_names, monitor);
}

static void activate(GtkApplication *app, gpointer userdata) {
  GtkWindow *window = gtk_application_get_active_window(app);
  if (window) {
    gtk_window_present(window);
    return;
  }

  window = GTK_WINDOW(adw_application_window_new(app));
  gtk_window_set_title(window, "CC Scout");
  gtk_window_set_default_size(window, 640, 560);
  monitor_view *monitor = g_new0(monitor_view, 1);
  monitor->profile = userdata;

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
  gtk_box_append(GTK_BOX(content), knob_grid_new(&monitor->knobs));
  gtk_box_append(GTK_BOX(content), pad_grid_new(&monitor->pads));
  refresh_control_names(monitor);

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
  GError *error = NULL;
  device_profile *profile = device_profile_load(&error);
  if (!profile) {
    g_printerr("Could not load the MIDI preset. %s\n", error->message);
    g_error_free(error);
    return 1;
  }
  AdwApplication *app = adw_application_new("io.github.riccardo_enr.CCScoutGui",
                                            G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(activate), profile);
  int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  device_profile_free(profile);
  return status;
}
