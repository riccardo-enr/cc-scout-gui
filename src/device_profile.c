#include "device_profile.h"

#include <yaml.h>

static const char *scalar(yaml_node_t *node) {
  return node && node->type == YAML_SCALAR_NODE
             ? (const char *)node->data.scalar.value
             : NULL;
}

static yaml_node_t *member(yaml_document_t *doc, yaml_node_t *map,
                           const char *name) {
  if (!map || map->type != YAML_MAPPING_NODE) {
    return NULL;
  }
  for (yaml_node_pair_t *pair = map->data.mapping.pairs.start;
       pair < map->data.mapping.pairs.top; ++pair) {
    if (g_strcmp0(scalar(yaml_document_get_node(doc, pair->key)), name) == 0) {
      return yaml_document_get_node(doc, pair->value);
    }
  }
  return NULL;
}

static char *read_name(yaml_node_t *node) {
  const char *name = scalar(node);
  return name && *name ? g_strdup(name) : NULL;
}

static gboolean read_number(yaml_node_t *node, unsigned int min,
                            unsigned int max, unsigned int *out) {
  const char *value = scalar(node);
  guint64 number;
  if (!value ||
      !g_ascii_string_to_unsigned(value, 10, min, max, &number, NULL)) {
    return FALSE;
  }
  *out = number;
  return TRUE;
}

static gboolean read_pads(yaml_document_t *doc, yaml_node_t *node,
                          unsigned int *notes, gboolean *seen) {
  if (!node || node->type != YAML_SEQUENCE_NODE ||
      node->data.sequence.items.top - node->data.sequence.items.start !=
          PAD_COUNT) {
    return FALSE;
  }
  for (int i = 0; i < PAD_COUNT; ++i) {
    if (!read_number(
            yaml_document_get_node(doc, node->data.sequence.items.start[i]), 0,
            MIDI_VALUE_COUNT - 1, &notes[i]) ||
        seen[notes[i]]) {
      return FALSE;
    }
    seen[notes[i]] = TRUE;
  }
  return TRUE;
}

static gboolean read_knobs(yaml_document_t *doc, yaml_node_t *knobs,
                           device_profile *profile) {
  yaml_node_t *names = member(doc, knobs, "names");
  yaml_node_t *ccs = member(doc, knobs, "known_ccs");
  if (!names || names->type != YAML_SEQUENCE_NODE ||
      names->data.sequence.items.top - names->data.sequence.items.start !=
          KNOB_COUNT ||
      !ccs || ccs->type != YAML_SEQUENCE_NODE) {
    return FALSE;
  }
  for (int i = 0; i < KNOB_COUNT; ++i) {
    profile->knob_names[i] = read_name(
        yaml_document_get_node(doc, names->data.sequence.items.start[i]));
    if (!profile->knob_names[i]) {
      return FALSE;
    }
  }
  for (yaml_node_item_t *item = ccs->data.sequence.items.start;
       item < ccs->data.sequence.items.top; ++item) {
    unsigned int cc;
    if (!read_number(yaml_document_get_node(doc, *item), 0,
                     MIDI_VALUE_COUNT - 1, &cc)) {
      return FALSE;
    }
    profile->knob_ccs[cc] = TRUE;
  }
  return TRUE;
}

static gboolean parse_profile(yaml_document_t *doc, device_profile *profile) {
  yaml_node_t *root = yaml_document_get_root_node(doc);
  yaml_node_t *ports = member(doc, root, "ports");
  yaml_node_t *pads = member(doc, root, "pads");
  yaml_node_t *knobs = member(doc, root, "knobs");
  profile->client = read_name(member(doc, root, "client"));
  profile->midi_port = read_name(member(doc, ports, "midi"));
  profile->daw_port = read_name(member(doc, ports, "daw"));
  gboolean seen[MIDI_VALUE_COUNT] = {FALSE};
  return profile->client && profile->midi_port && profile->daw_port &&
         !g_str_equal(profile->midi_port, profile->daw_port) &&
         read_number(member(doc, root, "key_channel"), 1, 16,
                     &profile->key_channel) &&
         read_number(member(doc, pads, "channel"), 1, 16,
                     &profile->pad_channel) &&
         read_number(member(doc, knobs, "channel"), 1, 16,
                     &profile->knob_channel) &&
         read_pads(doc, member(doc, pads, "bank_a"), profile->pad_notes,
                   seen) &&
         read_pads(doc, member(doc, pads, "bank_b"),
                   profile->pad_notes + PAD_COUNT, seen) &&
         read_knobs(doc, knobs, profile);
}

device_profile *device_profile_load(GError **error) {
  GBytes *bytes = g_resources_lookup_data(
      "/io/github/riccardo_enr/CCScoutGui/presets/akai-mpk-mini-iv.yaml",
      G_RESOURCE_LOOKUP_FLAGS_NONE, error);
  if (!bytes) {
    return NULL;
  }
  yaml_parser_t parser;
  if (!yaml_parser_initialize(&parser)) {
    g_bytes_unref(bytes);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Could not initialize the YAML parser");
    return NULL;
  }
  gsize size;
  const unsigned char *data = g_bytes_get_data(bytes, &size);
  yaml_parser_set_input_string(&parser, data, size);
  yaml_document_t doc;
  device_profile *profile = NULL;
  if (yaml_parser_load(&parser, &doc)) {
    profile = g_new0(device_profile, 1);
    if (!parse_profile(&doc, profile)) {
      device_profile_free(profile);
      profile = NULL;
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                          "Invalid MPK mini IV preset fields");
    }
    yaml_document_delete(&doc);
  } else {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid MPK mini IV YAML at line %zu: %s",
                parser.problem_mark.line + 1, parser.problem);
  }
  yaml_parser_delete(&parser);
  g_bytes_unref(bytes);
  return profile;
}

void device_profile_free(device_profile *profile) {
  g_free(profile->client);
  g_free(profile->midi_port);
  g_free(profile->daw_port);
  for (int i = 0; i < KNOB_COUNT; ++i) {
    g_free(profile->knob_names[i]);
  }
  g_free(profile);
}
