#include <adwaita.h>
#include <errno.h>

#include "cc_scout.h"

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
  guint read_timer;
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

static gboolean read_events(gpointer userdata) {
  monitor_view *view = userdata;
  if (!view->monitor) {
    return G_SOURCE_CONTINUE;
  }

  GtkTextBuffer *buffer = gtk_text_view_get_buffer(view->feed);
  GString *lines = g_string_new(NULL);
  for (int i = 0; i < 64; ++i) {
    cc_scout_event event;
    int result = cc_scout_read_event(view->monitor, &event);
    if (result == -EAGAIN || result == -EINTR) {
      break;
    }
    if (result < 0) {
      report_error(view, "Could not read MIDI events.", result);
      close_monitor(view);
      break;
    }
    g_string_append_printf(lines, "%u:%u  ", (unsigned int)event.source.client,
                           (unsigned int)event.source.port);
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
  return G_SOURCE_CONTINUE;
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
  g_source_remove(view->read_timer);
  close_monitor(view);
  g_clear_pointer(&view->ports, g_variant_unref);
  g_clear_pointer(&view->sources, g_variant_unref);
  g_free(view->device_name);
  g_free(view);
}

static void open_settings(GtkButton *button, gpointer userdata) {
  (void)button;
  gtk_window_present(GTK_WINDOW(userdata));
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

  AdwToolbarView *view = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
  AdwHeaderBar *header = ADW_HEADER_BAR(adw_header_bar_new());
  adw_toolbar_view_add_top_bar(view, GTK_WIDGET(header));
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(window),
                                     GTK_WIDGET(view));

  monitor->settings = GTK_WINDOW(gtk_window_new());
  gtk_window_set_title(monitor->settings, "MIDI Device Settings");
  gtk_window_set_default_size(monitor->settings, 380, 320);
  gtk_window_set_transient_for(monitor->settings, window);
  gtk_window_set_modal(monitor->settings, TRUE);
  gtk_window_set_destroy_with_parent(monitor->settings, TRUE);
  gtk_window_set_hide_on_close(monitor->settings, TRUE);
  GtkWidget *device_scroll = gtk_scrolled_window_new();
  gtk_window_set_child(monitor->settings, device_scroll);
  monitor->devices = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(monitor->devices, GTK_SELECTION_NONE);
  gtk_list_box_set_activate_on_single_click(monitor->devices, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(device_scroll),
                                GTK_WIDGET(monitor->devices));
  g_signal_connect(monitor->devices, "row-activated", G_CALLBACK(choose_device),
                   monitor);

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
  monitor->read_timer = g_timeout_add(25, read_events, monitor);
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
