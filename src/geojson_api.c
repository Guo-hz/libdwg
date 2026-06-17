#include "out_json.h"
#include "dwg.h"
#include "dwg_api.h"
#include "bits.h"
#include "codepages.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INIT_LAYERS_CAP 256
#define INIT_TEXTS_CAP 256
#define TEXTS_GROW_FACTOR 2
#define MAX_BLOCK_DEPTH 64
#define DWG_PI 3.14159265358979323846

typedef struct
{
  char *name;
  char **texts;
  int text_count;
  int text_capacity;
} LayerTexts;

typedef struct
{
  LayerTexts *layers;
  int num_layers;
  int layers_capacity;
} DwgLayers;

typedef struct
{
  char *buf;
  size_t len;
  size_t cap;
} DynStr;

typedef struct
{
  Dwg_Data *dwg;
  DwgLayers layers;
  unsigned char *object_seen;
  unsigned char *block_walked_seen;
  unsigned char *expanded_block_seen;
  BITCODE_BL object_count;
  int from_tu;
  int free_table_names;
  BITCODE_RS codepage;
  BITCODE_RS raw_codepage;
} TextExtractCtx;

static void
dynstr_init (DynStr *ds)
{
  ds->cap = 4096;
  ds->len = 0;
  ds->buf = (char *)malloc (ds->cap);
  if (ds->buf)
    ds->buf[0] = '\0';
}

static void
dynstr_free (DynStr *ds)
{
  free (ds->buf);
  ds->buf = NULL;
  ds->len = 0;
  ds->cap = 0;
}

static int
dynstr_ensure (DynStr *ds, size_t add)
{
  if (!ds->buf)
    return 0;
  if (ds->len + add + 1 > ds->cap)
    {
      size_t new_cap = ds->cap ? ds->cap * 2 : 4096;
      while (new_cap < ds->len + add + 1)
        new_cap *= 2;
      char *tmp = (char *)realloc (ds->buf, new_cap);
      if (!tmp)
        return 0;
      ds->buf = tmp;
      ds->cap = new_cap;
    }
  return 1;
}

static void
dynstr_printf (DynStr *ds, const char *fmt, ...)
{
  va_list args;
  int needed;

  va_start (args, fmt);
  needed = vsnprintf (NULL, 0, fmt, args);
  va_end (args);
  if (needed < 0 || !dynstr_ensure (ds, (size_t)needed))
    return;

  va_start (args, fmt);
  vsnprintf (ds->buf + ds->len, (size_t)needed + 1, fmt, args);
  va_end (args);
  ds->len += (size_t)needed;
}

static void
dynstr_puts (DynStr *ds, const char *s)
{
  dynstr_printf (ds, "%s", s);
}

static void
dynstr_putc (DynStr *ds, char c)
{
  if (!dynstr_ensure (ds, 1))
    return;
  ds->buf[ds->len++] = c;
  ds->buf[ds->len] = '\0';
}

static int
find_or_create_layer (DwgLayers *dl, const char *name)
{
  int i;
  char *name_copy;
  char **texts;

  if (!name || !name[0])
    name = "__UNKNOWN__";

  for (i = 0; i < dl->num_layers; i++)
    {
      if (strcmp (dl->layers[i].name, name) == 0)
        return i;
    }

  if (dl->num_layers >= dl->layers_capacity)
    {
      int new_cap
          = dl->layers_capacity == 0 ? INIT_LAYERS_CAP : dl->layers_capacity * 2;
      LayerTexts *tmp
          = (LayerTexts *)realloc (dl->layers, (size_t)new_cap * sizeof (*tmp));
      if (!tmp)
        return -1;
      dl->layers = tmp;
      dl->layers_capacity = new_cap;
    }

  name_copy = strdup (name);
  if (!name_copy)
    return -1;
  texts = (char **)calloc (INIT_TEXTS_CAP, sizeof (*texts));
  if (!texts)
    {
      free (name_copy);
      return -1;
    }

  i = dl->num_layers++;
  dl->layers[i].name = name_copy;
  dl->layers[i].texts = texts;
  dl->layers[i].text_count = 0;
  dl->layers[i].text_capacity = INIT_TEXTS_CAP;
  return i;
}

static int
add_text_to_layer (DwgLayers *dl, int idx, const char *text)
{
  LayerTexts *layer;
  char *copy;

  if (idx < 0 || idx >= dl->num_layers || !text)
    return -1;
  layer = &dl->layers[idx];

  if (layer->text_count >= layer->text_capacity)
    {
      int new_cap = layer->text_capacity * TEXTS_GROW_FACTOR;
      char **tmp
          = (char **)realloc (layer->texts, (size_t)new_cap * sizeof (*tmp));
      if (!tmp)
        return -1;
      layer->texts = tmp;
      layer->text_capacity = new_cap;
    }

  copy = strdup (text);
  if (!copy)
    return -1;
  layer->texts[layer->text_count++] = copy;
  return 0;
}

static void
free_all_layers (DwgLayers *dl)
{
  int i, j;
  for (i = 0; i < dl->num_layers; i++)
    {
      free (dl->layers[i].name);
      for (j = 0; j < dl->layers[i].text_count; j++)
        free (dl->layers[i].texts[j]);
      free (dl->layers[i].texts);
    }
  free (dl->layers);
  memset (dl, 0, sizeof (*dl));
}

static int
valid_index (const TextExtractCtx *ctx, const Dwg_Object *obj)
{
  return obj && obj->index < ctx->object_count;
}

static int
is_seen (const TextExtractCtx *ctx, const Dwg_Object *obj)
{
  return valid_index (ctx, obj) && ctx->object_seen[obj->index];
}

static void
mark_seen (TextExtractCtx *ctx, const Dwg_Object *obj)
{
  if (valid_index (ctx, obj))
    ctx->object_seen[obj->index] = 1;
}

static char *
escape_string_alloc (const TextExtractCtx *ctx, const char *src,
                     BITCODE_RS codepage)
{
  size_t len;
  size_t out_len;
  char *out;
  const unsigned char *s;
  char *tmp = NULL;
  char *d;
  static const char hex[] = "0123456789ABCDEF";

  (void)ctx;
  if (!src)
    return NULL;
  if (strlen (src) && codepage > CP_US_ASCII && codepage <= CP_ANSI_1258)
    {
      tmp = bit_TV_to_utf8 ((const char *restrict)src, codepage);
      if (tmp)
        src = tmp;
    }
  len = strlen (src);
  if (len > (((size_t)-1) - 1) / 6)
    {
      free (tmp);
      return NULL;
    }
  out_len = len * 6 + 1;
  out = (char *)malloc (out_len);
  if (!out)
    {
      free (tmp);
      return NULL;
    }

  s = (const unsigned char *)src;
  d = out;
  while (*s)
    {
      unsigned char c = *s;
      if (c == '"' || c == '\\')
        {
          *d++ = '\\';
          *d++ = (char)c;
          s++;
        }
      else if (c == '\n')
        {
          *d++ = '\\';
          *d++ = 'n';
          s++;
        }
      else if (c == '\r')
        {
          *d++ = '\\';
          *d++ = 'r';
          s++;
        }
      else if (c == '\t')
        {
          *d++ = '\\';
          *d++ = 't';
          s++;
        }
      else if (c < 0x20)
        {
          *d++ = '\\';
          *d++ = 'u';
          *d++ = '0';
          *d++ = '0';
          *d++ = hex[c >> 4];
          *d++ = hex[c & 0xf];
          s++;
        }
      else if (c < 0x80)
        {
          *d++ = (char)c;
          s++;
        }
      else
        {
          int n = 0;
          unsigned char min_next = 0x80;
          if (c >= 0xC2 && c <= 0xDF)
            n = 2;
          else if (c >= 0xE0 && c <= 0xEF)
            {
              n = 3;
              if (c == 0xE0)
                min_next = 0xA0;
            }
          else if (c >= 0xF0 && c <= 0xF4)
            {
              n = 4;
              if (c == 0xF0)
                min_next = 0x90;
            }

          if (n && s[n - 1])
            {
              int ok = 1;
              for (int i = 1; i < n; i++)
                {
                  if ((s[i] & 0xC0) != 0x80 || (i == 1 && s[i] < min_next)
                      || (i == 1 && c == 0xED && s[i] > 0x9F)
                      || (i == 1 && c == 0xF4 && s[i] > 0x8F))
                    {
                      ok = 0;
                      break;
                    }
                }
              if (ok)
                {
                  for (int i = 0; i < n; i++)
                    *d++ = (char)*s++;
                  continue;
                }
            }

          *d++ = '\\';
          *d++ = 'u';
          *d++ = '0';
          *d++ = '0';
          *d++ = hex[c >> 4];
          *d++ = hex[c & 0xf];
          s++;
        }
    }
  *d = '\0';
  free (tmp);
  return out;
}

static char *
convert_text_alloc (const TextExtractCtx *ctx, BITCODE_T value)
{
  if (!value)
    return NULL;
  if (ctx->from_tu)
    return bit_convert_TU ((BITCODE_TU)value);
  return strdup ((const char *)value);
}

static char *
table_name_escaped (TextExtractCtx *ctx, Dwg_Object *obj)
{
  int error = 1;
  char *name;
  char *escaped = NULL;

  if (!obj || obj->fixedtype != DWG_TYPE_LAYER)
    return NULL;

  name = dwg_obj_table_get_name (obj, &error);
  if (!error && name)
    escaped = escape_string_alloc (ctx, name, ctx->codepage);
  if (name && ctx->free_table_names)
    free (name);
  return escaped;
}

static BITCODE_RLL
ref_abs_value (BITCODE_H ref)
{
  if (!ref)
    return 0;
  return ref->absolute_ref ? ref->absolute_ref : ref->handleref.value;
}

static int
layer_control_has_ref (TextExtractCtx *ctx, BITCODE_RLL absref)
{
  Dwg_Object *ctrl;
  Dwg_Object_LAYER_CONTROL *layer_ctrl;
  BITCODE_BS i;

  if (!absref)
    return 0;
  ctrl = dwg_get_first_object (ctx->dwg, DWG_TYPE_LAYER_CONTROL);
  if (!ctrl || !ctrl->tio.object || !ctrl->tio.object->tio.LAYER_CONTROL)
    return 0;

  layer_ctrl = ctrl->tio.object->tio.LAYER_CONTROL;
  for (i = 0; layer_ctrl->entries && i < layer_ctrl->num_entries; i++)
    {
      if (ref_abs_value (layer_ctrl->entries[i]) == absref)
        return 1;
    }
  return 0;
}

static void
add_layer_from_object (TextExtractCtx *ctx, Dwg_Object *obj)
{
  char *escaped = table_name_escaped (ctx, obj);
  if (escaped)
    {
      if (escaped[0] != '\0')
        find_or_create_layer (&ctx->layers, escaped);
      free (escaped);
    }
}

static void
add_layer_from_ref (TextExtractCtx *ctx, BITCODE_H ref)
{
  Dwg_Object *obj = ref ? dwg_ref_object (ctx->dwg, ref) : NULL;
  if (obj && obj->fixedtype == DWG_TYPE_LAYER)
    add_layer_from_object (ctx, obj);
  else if (ref)
    {
      BITCODE_RLL absref = ref_abs_value (ref);
      if (absref >= 0x100)
        {
          char name[64];
          snprintf (name, sizeof (name), "__UNRESOLVED_LAYER_%llX__",
                    (unsigned long long)absref);
          find_or_create_layer (&ctx->layers, name);
        }
    }
}

static void
add_layers_from_layer_control (TextExtractCtx *ctx)
{
  Dwg_Object *ctrl = dwg_get_first_object (ctx->dwg, DWG_TYPE_LAYER_CONTROL);
  Dwg_Object_LAYER_CONTROL *layer_ctrl;
  BITCODE_BS i;

  if (!ctrl || !ctrl->tio.object || !ctrl->tio.object->tio.LAYER_CONTROL)
    return;

  layer_ctrl = ctrl->tio.object->tio.LAYER_CONTROL;
  for (i = 0; layer_ctrl->entries && i < layer_ctrl->num_entries; i++)
    add_layer_from_ref (ctx, layer_ctrl->entries[i]);
}

static void
add_all_layer_objects (TextExtractCtx *ctx)
{
  BITCODE_BL i;
  for (i = 0; i < ctx->dwg->num_objects; i++)
    {
      Dwg_Object *obj = &ctx->dwg->object[i];
      if (obj->fixedtype == DWG_TYPE_LAYER)
        add_layer_from_object (ctx, obj);
    }
}

static char *
default_layer_escaped (const TextExtractCtx *ctx, const char *name)
{
  return escape_string_alloc (ctx, name ? name : "__UNKNOWN__", 0);
}

static char *
get_entity_layer_name_escaped (TextExtractCtx *ctx, const Dwg_Object *obj,
                               const char *fallback_layer)
{
  Dwg_Object_Ref *layer_ref;
  Dwg_Object *layer_obj;
  char *escaped;

  if (!obj || obj->supertype != DWG_SUPERTYPE_ENTITY || !obj->tio.entity)
    return fallback_layer ? strdup (fallback_layer)
                          : default_layer_escaped (ctx, "__UNKNOWN__");

  layer_ref = obj->tio.entity->layer;
  if (!layer_ref)
    return fallback_layer ? strdup (fallback_layer)
                          : default_layer_escaped (ctx, "0");

  layer_obj = layer_ref->obj ? layer_ref->obj : dwg_ref_object (ctx->dwg, layer_ref);
  escaped = table_name_escaped (ctx, layer_obj);
  if (escaped && escaped[0] != '\0')
    return escaped;
  free (escaped);

  if (layer_ref)
    {
      BITCODE_RLL absref = ref_abs_value (layer_ref);
      if (absref >= 0x100 && layer_control_has_ref (ctx, absref))
        {
          char name[64];
          snprintf (name, sizeof (name), "__UNRESOLVED_LAYER_%llX__",
                    (unsigned long long)absref);
          return strdup (name);
        }
    }

  return fallback_layer ? strdup (fallback_layer)
                        : default_layer_escaped (ctx, "0");
}

static void
append_raw_text_to_layer (TextExtractCtx *ctx, const char *layer_name,
                          const char *raw_text)
{
  char *json_text;
  int idx;

  if (!raw_text || raw_text[0] == '\0')
    return;

  json_text = escape_string_alloc (ctx, raw_text, ctx->codepage);
  if (!json_text)
    return;

  idx = find_or_create_layer (&ctx->layers,
                              layer_name && layer_name[0] ? layer_name
                                                          : "__UNKNOWN__");
  if (idx >= 0)
    add_text_to_layer (&ctx->layers, idx, json_text);
  free (json_text);
}

static void
append_text_value (TextExtractCtx *ctx, const char *layer_name, BITCODE_T value)
{
  char *raw = convert_text_alloc (ctx, value);
  if (raw)
    {
      if (strcmp (raw, ",") != 0)
        append_raw_text_to_layer (ctx, layer_name, raw);
      free (raw);
    }
}

static int
raw_text_is_placeholder (const char *raw)
{
  return !raw || raw[0] == '\0' || strcmp (raw, "<>") == 0;
}

static Dwg_DIMENSION_common *
dimension_common (const Dwg_Object *obj)
{
  if (!obj || obj->supertype != DWG_SUPERTYPE_ENTITY || !obj->tio.entity)
    return NULL;
  return obj->tio.entity->tio.DIMENSION_common;
}

static void
append_generated_dimension_text (TextExtractCtx *ctx, const char *layer_name,
                                 const Dwg_Object *obj,
                                 const Dwg_DIMENSION_common *dim)
{
  double value;
  const char *prefix = "";
  const char *suffix = "";
  char buf[128];

  if (!dim)
    return;
  value = dim->act_measurement;
  if (value != value)
    return;

  switch (obj->fixedtype)
    {
    case DWG_TYPE_DIMENSION_ANG3PT:
    case DWG_TYPE_DIMENSION_ANG2LN:
    case DWG_TYPE_ARC_DIMENSION:
      value = value * 180.0 / DWG_PI;
      suffix = "deg";
      break;
    case DWG_TYPE_DIMENSION_RADIUS:
    case DWG_TYPE_LARGE_RADIAL_DIMENSION:
      prefix = "R";
      break;
    case DWG_TYPE_DIMENSION_DIAMETER:
      prefix = "D";
      break;
    default:
      break;
    }

  snprintf (buf, sizeof (buf), "%s%.4f%s", prefix, value, suffix);
  append_raw_text_to_layer (ctx, layer_name, buf);
}

static void
append_dimension_text (TextExtractCtx *ctx, const char *layer_name,
                       const Dwg_Object *obj)
{
  Dwg_DIMENSION_common *dim = dimension_common (obj);
  char *raw = NULL;

  if (!dim)
    return;

  raw = convert_text_alloc (ctx, dim->user_text);
  if (raw && !raw_text_is_placeholder (raw))
    {
      append_raw_text_to_layer (ctx, layer_name, raw);
      free (raw);
      return;
    }
  free (raw);
}

static char *
raw_recovery_layer_name (TextExtractCtx *ctx, BITCODE_RLL layer_handle,
                         BITCODE_RLL object_handle)
{
  Dwg_Object *layer_obj;
  char *escaped;

  if (layer_handle)
    {
      layer_obj = dwg_resolve_handle_silent (ctx->dwg, layer_handle);
      escaped = table_name_escaped (ctx, layer_obj);
      if (escaped && escaped[0] != '\0')
        return escaped;
      free (escaped);
    }

  if (object_handle)
    {
      Dwg_Object *obj = dwg_resolve_handle_silent (ctx->dwg, object_handle);
      if (obj && obj->supertype == DWG_SUPERTYPE_ENTITY && obj->tio.entity)
        {
          escaped = get_entity_layer_name_escaped (ctx, obj, NULL);
          if (escaped && escaped[0] != '\0')
            return escaped;
          free (escaped);
        }
    }

  return strdup ("__RAW_UNASSIGNED__");
}

static int
decoded_text_value_matches (TextExtractCtx *ctx, BITCODE_T value,
                            const char *raw_text)
{
  char *decoded;
  int matches = 0;

  if (!value || !raw_text)
    return 0;
  decoded = convert_text_alloc (ctx, value);
  if (decoded)
    {
      matches = strcmp (decoded, raw_text) == 0;
      free (decoded);
    }
  return matches;
}

static int
object_decoded_text_matches (TextExtractCtx *ctx, Dwg_Object *obj,
                             const char *raw_text)
{
  int type;

  if (!obj || obj->supertype != DWG_SUPERTYPE_ENTITY || !obj->tio.entity
      || !raw_text)
    return 0;

  type = obj->fixedtype ? obj->fixedtype : obj->type;
  switch (type)
    {
    case DWG_TYPE_TEXT:
      return obj->tio.entity->tio.TEXT
             && decoded_text_value_matches (
                 ctx, obj->tio.entity->tio.TEXT->text_value, raw_text);
    case DWG_TYPE_MTEXT:
      return obj->tio.entity->tio.MTEXT
             && decoded_text_value_matches (
                 ctx, obj->tio.entity->tio.MTEXT->text, raw_text);
    case DWG_TYPE_ATTRIB:
      return obj->tio.entity->tio.ATTRIB
             && decoded_text_value_matches (
                 ctx, obj->tio.entity->tio.ATTRIB->text_value, raw_text);
    case DWG_TYPE_ATTDEF:
      return obj->tio.entity->tio.ATTDEF
             && decoded_text_value_matches (
                 ctx, obj->tio.entity->tio.ATTDEF->default_value, raw_text);
    case DWG_TYPE_TOLERANCE:
      return obj->tio.entity->tio.TOLERANCE
             && decoded_text_value_matches (
                 ctx, obj->tio.entity->tio.TOLERANCE->text_value, raw_text);
    case DWG_TYPE_ARCALIGNEDTEXT:
      return obj->tio.entity->tio.ARCALIGNEDTEXT
             && decoded_text_value_matches (
                 ctx, obj->tio.entity->tio.ARCALIGNEDTEXT->text_value,
                 raw_text);
    case DWG_TYPE_RTEXT:
      return obj->tio.entity->tio.RTEXT
             && decoded_text_value_matches (
                 ctx, obj->tio.entity->tio.RTEXT->text_value, raw_text);
    default:
      break;
    }
  return 0;
}

static void
append_raw_recovered_texts (TextExtractCtx *ctx)
{
  Dwg_RawTextRecovery *raw;

  if (!ctx || !ctx->dwg)
    return;
  raw = &ctx->dwg->r2007_raw_texts;
  for (BITCODE_BL i = 0; raw->items && i < raw->num_items; i++)
    {
      Dwg_Object *raw_obj = raw->items[i].object_handle
                                ? dwg_resolve_handle_silent (
                                    ctx->dwg, raw->items[i].object_handle)
                                : NULL;
      char *layer_name = raw_recovery_layer_name (
          ctx, raw->items[i].layer_handle, raw->items[i].object_handle);
      int append = 1;
      if (object_decoded_text_matches (ctx, raw_obj, raw->items[i].text))
        append = 0;
      if (!raw->items[i].layer_handle && !raw->items[i].object_handle)
        {
          int seen_raw = 0;
          int existing = 0;
          char *json_text = escape_string_alloc (ctx, raw->items[i].text,
                                                 ctx->codepage);
          for (BITCODE_BL j = 0; j <= i; j++)
            if (raw->items[j].text
                && !raw->items[j].layer_handle && !raw->items[j].object_handle
                && strcmp (raw->items[j].text, raw->items[i].text) == 0)
              seen_raw++;
          if (json_text)
            {
              for (int l = 0; l < ctx->layers.num_layers; l++)
                for (int t = 0; t < ctx->layers.layers[l].text_count; t++)
                  if (strcmp (ctx->layers.layers[l].texts[t], json_text) == 0)
                    existing++;
              free (json_text);
            }
          append = seen_raw > existing;
        }
      if (append)
        append_raw_text_to_layer (ctx, layer_name, raw->items[i].text);
      free (layer_name);
    }
}

static void
append_embedded_mtext (TextExtractCtx *ctx, const char *layer_name,
                       Dwg_AcDbMTextObjectEmbedded *mtext)
{
  if (mtext && mtext->text)
    append_text_value (ctx, layer_name, mtext->text);
}

static void
append_table_value (TextExtractCtx *ctx, const char *layer_name,
                    Dwg_TABLE_value *value)
{
  if (!value)
    return;
  append_text_value (ctx, layer_name, value->data_string);
  append_text_value (ctx, layer_name, value->value_string);
}

static void
append_tablecontent_texts (TextExtractCtx *ctx, const char *layer_name,
                           Dwg_LinkedTableData *tdata)
{
  BITCODE_BL r, c, k;

  if (!tdata)
    return;

  for (c = 0; tdata->cols && c < tdata->num_cols; c++)
    append_text_value (ctx, layer_name, tdata->cols[c].name);

  for (r = 0; tdata->rows && r < tdata->num_rows; r++)
    {
      Dwg_TableRow *row = &tdata->rows[r];
      for (k = 0; row->customdata_items && k < row->num_customdata_items; k++)
        append_table_value (ctx, layer_name, &row->customdata_items[k].value);

      for (c = 0; row->cells && c < row->num_cells; c++)
        {
          Dwg_TableCell *cell = &row->cells[c];
          BITCODE_BL i;

          append_text_value (ctx, layer_name, cell->tooltip);
          for (i = 0; cell->customdata_items && i < cell->num_customdata_items;
               i++)
            append_table_value (ctx, layer_name,
                                &cell->customdata_items[i].value);

          for (i = 0; cell->cell_contents && i < cell->num_cell_contents; i++)
            {
              Dwg_TableCellContent *content = &cell->cell_contents[i];
              BITCODE_BL a;

              append_table_value (ctx, layer_name, &content->value);
              for (a = 0; content->attrs && a < content->num_attrs; a++)
                append_text_value (ctx, layer_name, content->attrs[a].value);
            }
        }
    }
}

static void
append_table_texts (TextExtractCtx *ctx, const char *layer_name,
                    Dwg_Entity_TABLE *table)
{
  unsigned long i;
  unsigned long num_cells;

  if (!table)
    return;

  append_tablecontent_texts (ctx, layer_name, &table->tdata);

  num_cells = table->num_cells;
  if (!num_cells && table->num_rows && table->num_cols)
    num_cells = (unsigned long)table->num_rows * (unsigned long)table->num_cols;

  for (i = 0; table->cells && i < num_cells; i++)
    {
      Dwg_TABLE_Cell *cell = &table->cells[i];
      BITCODE_BL a;

      append_text_value (ctx, layer_name, cell->text_value);
      append_table_value (ctx, layer_name, &cell->value);
      for (a = 0; cell->attr_defs && a < cell->num_attr_defs; a++)
        append_text_value (ctx, layer_name, cell->attr_defs[a].text);
    }
}

static void
append_mleader_texts (TextExtractCtx *ctx, const char *layer_name,
                      Dwg_Entity_MULTILEADER *mleader)
{
  BITCODE_BL i;

  if (!mleader)
    return;

  if (mleader->ctx.has_content_txt)
    append_text_value (ctx, layer_name,
                       mleader->ctx.content.txt.default_text);

  for (i = 0; mleader->blocklabels && i < mleader->num_blocklabels; i++)
    append_text_value (ctx, layer_name, mleader->blocklabels[i].label_text);
}

static int is_dimension_type (int type)
{
  switch (type)
    {
    case DWG_TYPE_DIMENSION_ORDINATE:
    case DWG_TYPE_DIMENSION_LINEAR:
    case DWG_TYPE_DIMENSION_ALIGNED:
    case DWG_TYPE_DIMENSION_ANG3PT:
    case DWG_TYPE_DIMENSION_ANG2LN:
    case DWG_TYPE_DIMENSION_RADIUS:
    case DWG_TYPE_DIMENSION_DIAMETER:
    case DWG_TYPE_ARC_DIMENSION:
    case DWG_TYPE_LARGE_RADIAL_DIMENSION:
      return 1;
    default:
      return 0;
    }
}

static int
object_effective_type (const Dwg_Object *obj)
{
  if (!obj)
    return 0;
  return obj->fixedtype ? obj->fixedtype : obj->type;
}

static int is_text_container_type (int type)
{
  if (is_dimension_type (type))
    return 1;
  switch (type)
    {
    case DWG_TYPE_TEXT:
    case DWG_TYPE_MTEXT:
    case DWG_TYPE_ATTRIB:
    case DWG_TYPE_ATTDEF:
    case DWG_TYPE_TOLERANCE:
    case DWG_TYPE_LEADER:
    case DWG_TYPE_INSERT:
    case DWG_TYPE_MINSERT:
    case DWG_TYPE_MULTILEADER:
    case DWG_TYPE_TABLE:
    case DWG_TYPE_GEOPOSITIONMARKER:
    case DWG_TYPE_ARCALIGNEDTEXT:
    case DWG_TYPE_RTEXT:
    case DWG_TYPE_PROXY_ENTITY:
    case DWG_TYPE_UNKNOWN_ENT:
      return 1;
    default:
      return 0;
    }
}

static int
is_layer_name_text_fallback_type (int type)
{
  switch (type)
    {
    case DWG_TYPE_LWPOLYLINE:
    case DWG_TYPE_POLYLINE_2D:
    case DWG_TYPE_POLYLINE_3D:
    case DWG_TYPE_POLYLINE_PFACE:
    case DWG_TYPE_REGION:
      return 1;
    default:
      return 0;
    }
}

static int
utf16_raw_char_is_text (unsigned c)
{
  return (c >= 0x20 && c <= 0x7e) || (c >= 0x3400 && c <= 0x9fff)
         || (c >= 0xf900 && c <= 0xfaff) || (c >= 0x3000 && c <= 0x303f)
         || (c >= 0xff00 && c <= 0xffef);
}

static int
utf16_raw_char_is_cjk (unsigned c)
{
  return (c >= 0x3400 && c <= 0x9fff) || (c >= 0xf900 && c <= 0xfaff);
}

static int
ascii_raw_text_is_useful (const char *s)
{
  int has_digit = 0;
  int has_alpha = 0;
  int dot_count = 0;

  if (!s || !s[0] || !s[1])
    return 0;
  if (strncmp (s, "S=", 2) == 0)
    {
      s += 2;
      if (*s < '0' || *s > '9')
        return 0;
      while (*s)
        {
          if (*s >= '0' && *s <= '9')
            has_digit = 1;
          else if (*s == '.' && dot_count++ == 0)
            ;
          else
            return 0;
          s++;
        }
      return has_digit;
    }
  for (const char *p = s; *p; p++)
    {
      if (*p >= '0' && *p <= '9')
        has_digit = 1;
      else if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))
        has_alpha = 1;
      else if (*p == '.' && !has_alpha && dot_count++ == 0)
        ;
      else
        return 0;
    }
  return (has_alpha && has_digit) || (!has_alpha && has_digit && dot_count == 1);
}

static int
utf8_text_is_useful (const char *s)
{
  int has_cjk = 0;
  int has_s_eq = 0;
  int has_digit = 0;
  int has_alpha = 0;

  if (!s || !s[0] || !s[1])
    return 0;
  has_s_eq = strstr (s, "S=") != NULL;
  for (const unsigned char *p = (const unsigned char *)s; *p;)
    {
      if (*p >= '0' && *p <= '9')
        {
          has_digit = 1;
          p++;
        }
      else if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))
        {
          has_alpha = 1;
          p++;
        }
      else if (*p >= 0xe0 && (*p & 0xf0) == 0xe0 && p[1] && p[2]
               && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80)
        {
          unsigned cp = ((*p & 0x0f) << 12) | ((p[1] & 0x3f) << 6)
                        | (p[2] & 0x3f);
          if ((cp >= 0x3400 && cp <= 0x9fff)
              || (cp >= 0xf900 && cp <= 0xfaff))
            has_cjk = 1;
          p += 3;
        }
      else if (*p >= 0xc0 && (*p & 0xe0) == 0xc0 && p[1]
               && (p[1] & 0xc0) == 0x80)
        p += 2;
      else if (*p >= 0xf0 && (*p & 0xf8) == 0xf0 && p[1] && p[2] && p[3]
               && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80
               && (p[3] & 0xc0) == 0x80)
        p += 4;
      else
        p++;
    }
  return has_cjk || has_s_eq || (has_alpha && has_digit);
}

static int
utf8_text_has_cjk (const char *s)
{
  if (!s)
    return 0;
  for (const unsigned char *p = (const unsigned char *)s; *p;)
    {
      if (*p >= 0xe0 && (*p & 0xf0) == 0xe0 && p[1] && p[2]
          && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80)
        {
          unsigned cp = ((*p & 0x0f) << 12) | ((p[1] & 0x3f) << 6)
                        | (p[2] & 0x3f);
          if ((cp >= 0x3400 && cp <= 0x9fff)
              || (cp >= 0xf900 && cp <= 0xfaff))
            return 1;
          p += 3;
        }
      else if (*p >= 0xc0 && (*p & 0xe0) == 0xc0 && p[1]
               && (p[1] & 0xc0) == 0x80)
        p += 2;
      else if (*p >= 0xf0 && (*p & 0xf8) == 0xf0 && p[1] && p[2] && p[3]
               && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80
               && (p[3] & 0xc0) == 0x80)
        p += 4;
      else
        p++;
    }
  return 0;
}

static int
utf16_units_are_useful_text (const uint16_t *units, size_t chars,
                             size_t *visible_chars)
{
  int has_cjk = 0;
  int has_digit = 0;
  int has_alpha = 0;
  int has_s_eq = 0;
  int dots = 0;
  size_t visible;

  if (!units || chars < 1 || chars > 65)
    return 0;
  visible = chars;
  if (visible && units[visible - 1] == 0)
    visible--;
  if (!visible || visible > 64)
    return 0;

  for (size_t i = 0; i < visible; i++)
    {
      uint16_t c = units[i];
      if (utf16_raw_char_is_cjk (c))
        has_cjk = 1;
      else if (c >= '0' && c <= '9')
        has_digit = 1;
      else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
        has_alpha = 1;
      else if (c == '.')
        dots++;
      else if (c == '=' || c == '-' || c == '_' || c == '#')
        ;
      else if (c == 0x3001 || c == 0x3002 || c == 0xff0c || c == 0xff08
               || c == 0xff09 || c == 0xff1d)
        ;
      else
        return 0;
      if (i + 1 < visible && c == 'S' && units[i + 1] == '=')
        has_s_eq = 1;
    }

  if (visible_chars)
    *visible_chars = visible;
  return has_cjk || has_s_eq || (has_alpha && has_digit)
         || (has_digit && !has_alpha && dots <= 1);
}

static void
append_utf16_units_text (TextExtractCtx *ctx, const char *layer_name,
                         const uint16_t *units, size_t chars)
{
  size_t visible = 0;
  BITCODE_RC *tmp;
  char *utf8;

  if (!utf16_units_are_useful_text (units, chars, &visible))
    return;
  tmp = (BITCODE_RC *)calloc (visible + 1, 2);
  if (!tmp)
    return;
  for (size_t i = 0; i < visible; i++)
    {
      tmp[i * 2] = (BITCODE_RC)(units[i] & 0xff);
      tmp[i * 2 + 1] = (BITCODE_RC)(units[i] >> 8);
    }
  utf8 = bit_convert_TU ((BITCODE_TU)tmp);
  if (utf8 && utf8_text_is_useful (utf8))
    append_raw_text_to_layer (ctx, layer_name, utf8);
  free (utf8);
  free (tmp);
}

static int
raw_codepage_byte_is_text (BITCODE_RC c)
{
  return (c >= 0x20 && c <= 0x7e) || c >= 0x80;
}

static void
append_codepage_raw_buffer_texts (TextExtractCtx *ctx, const char *layer_name,
                                  const BITCODE_RC *buf, BITCODE_BL size)
{
  BITCODE_RS codepage = ctx ? ctx->raw_codepage : 0;

  if (!ctx || !buf || !size || !codepage || codepage == CP_UTF16)
    return;

  for (BITCODE_BL i = 0; i < size;)
    {
      BITCODE_BL start = i;
      int has_high = 0;
      while (i < size && raw_codepage_byte_is_text (buf[i]))
        {
          if (buf[i] >= 0x80)
            has_high = 1;
          i++;
          if (i - start >= 64)
            break;
        }
      if (i > start)
        {
          BITCODE_BL len = i - start;
          if (has_high && len >= 2 && len <= 64)
            {
              char *raw = (char *)malloc ((size_t)len + 1);
              char *utf8;
              if (!raw)
                return;
              memcpy (raw, &buf[start], len);
              raw[len] = '\0';
              utf8 = bit_TV_to_utf8 (raw, codepage);
              if (utf8 && utf8_text_is_useful (utf8))
                append_raw_text_to_layer (ctx, layer_name, utf8);
              if (utf8 && utf8 != raw)
                free (utf8);
              free (raw);
            }
        }
      else
        i++;
    }
}

static void
append_length_prefixed_raw_buffer_texts (TextExtractCtx *ctx,
                                         const char *layer_name,
                                         const BITCODE_RC *buf,
                                         BITCODE_BL size)
{
  if (!ctx || !buf || size < 3)
    return;

  for (BITCODE_BL pos = 0; pos + 4 < size; pos++)
    {
      uint16_t chars = (uint16_t)(buf[pos] | (buf[pos + 1] << 8));
      uint16_t units[66];

      if (chars < 1 || chars > 65)
        continue;
      if (pos + 2 + ((BITCODE_BL)chars * 2) > size)
        continue;
      for (uint16_t i = 0; i < chars; i++)
        units[i] = (uint16_t)(buf[pos + 2 + (i * 2)]
                              | (buf[pos + 3 + (i * 2)] << 8));
      append_utf16_units_text (ctx, layer_name, units, chars);
    }

  if (ctx->raw_codepage && ctx->raw_codepage != CP_UTF16)
    for (BITCODE_BL pos = 0; pos + 2 < size; pos++)
      {
        BITCODE_BL len = buf[pos];
        int has_high = 0;
        char *raw;
        char *utf8;

        if (len < 2 || len > 64 || pos + 1 + len > size)
          continue;
        for (BITCODE_BL i = 0; i < len; i++)
          {
            BITCODE_RC c = buf[pos + 1 + i];
            if (!raw_codepage_byte_is_text (c))
              {
                has_high = 0;
                len = 0;
                break;
              }
            if (c >= 0x80)
              has_high = 1;
          }
        if (!len || !has_high)
          continue;
        raw = (char *)malloc ((size_t)len + 1);
        if (!raw)
          return;
        memcpy (raw, &buf[pos + 1], len);
        raw[len] = '\0';
        utf8 = bit_TV_to_utf8 (raw, ctx->raw_codepage);
        if (utf8 && utf8_text_is_useful (utf8))
          append_raw_text_to_layer (ctx, layer_name, utf8);
        if (utf8 && utf8 != raw)
          free (utf8);
        free (raw);
      }
}

static void
append_raw_buffer_texts (TextExtractCtx *ctx, const char *layer_name,
                         const BITCODE_RC *buf, BITCODE_BL size)
{
  int allow_single_cjk = layer_name && strcmp (layer_name,
                                               "__RAW_UNASSIGNED__") != 0;

  if (!buf || !size)
    return;

  for (BITCODE_BL i = 0; i < size;)
    {
      BITCODE_BL start = i;
      while (i < size && buf[i] >= 0x20 && buf[i] <= 0x7e)
        i++;
      if (i > start)
        {
          BITCODE_BL len = i - start;
          if (len >= 2 && len <= 64)
            {
              char tmp[65];
              memcpy (tmp, &buf[start], len);
              tmp[len] = '\0';
              if (ascii_raw_text_is_useful (tmp))
                append_raw_text_to_layer (ctx, layer_name, tmp);
            }
        }
      else
        i++;
    }

  append_codepage_raw_buffer_texts (ctx, layer_name, buf, size);

  for (BITCODE_BL base = 0; base < 2; base++)
    for (BITCODE_BL i = base; i + 1 < size;)
      {
        BITCODE_BL start = i;
        BITCODE_BL chars = 0;
        int cjk = 0;
        while (i + 1 < size)
          {
            unsigned c = (unsigned)(buf[i] | (buf[i + 1] << 8));
            if (!utf16_raw_char_is_text (c))
              break;
            if (utf16_raw_char_is_cjk (c))
              cjk = 1;
            chars++;
            i += 2;
            if (chars >= 64)
              break;
          }
        if ((chars >= 2 || (allow_single_cjk && chars == 1 && cjk))
            && chars <= 64)
          {
            BITCODE_RC tmp[130];
            char *utf8;
            memcpy (tmp, &buf[start], (size_t)chars * 2);
            tmp[chars * 2] = 0;
            tmp[chars * 2 + 1] = 0;
            utf8 = bit_convert_TU ((BITCODE_TU)tmp);
            if (utf8 && (cjk || utf8_text_is_useful (utf8)))
              append_raw_text_to_layer (ctx, layer_name, utf8);
            free (utf8);
          }
        if (i == start)
          i += 2;
      }

  append_length_prefixed_raw_buffer_texts (ctx, layer_name, buf, size);
}

static void
append_useful_utf8_text (TextExtractCtx *ctx, const char *layer_name,
                         const char *utf8)
{
  if (utf8 && utf8_text_is_useful (utf8))
    append_raw_text_to_layer (ctx, layer_name, utf8);
}

static void
append_eed_texts (TextExtractCtx *ctx, const char *layer_name, Dwg_Eed *eed,
                  BITCODE_BL num_eed)
{
  const char *target_layer = layer_name ? layer_name : "__RAW_UNASSIGNED__";

  if (!eed || !num_eed)
    return;

  for (BITCODE_BL i = 0; i < num_eed; i++)
    {
      Dwg_Eed_Data *data = eed[i].data;
      if (!data)
        continue;

      switch (data->code)
        {
        case 0:
          if (data->u.eed_0.is_tu)
            {
              char *utf8 = bit_TU_to_utf8_len (
                  data->u.eed_0_r2007.string,
                  (int)data->u.eed_0_r2007.length);
              append_useful_utf8_text (ctx, target_layer, utf8);
              free (utf8);
            }
          else if (data->u.eed_0.string)
            {
              BITCODE_RS codepage
                  = data->u.eed_0.codepage ? data->u.eed_0.codepage
                                            : ctx->raw_codepage;
              char *utf8 = bit_TV_to_utf8 (data->u.eed_0.string, codepage);
              append_useful_utf8_text (ctx, target_layer,
                                       utf8 ? utf8 : data->u.eed_0.string);
              if (utf8 && utf8 != data->u.eed_0.string)
                free (utf8);
            }
          break;
        case 4:
          append_raw_buffer_texts (ctx, target_layer, data->u.eed_4.data,
                                   data->u.eed_4.length);
          break;
        default:
          break;
        }
    }
}

static void
append_xdata_texts (TextExtractCtx *ctx, const char *layer_name,
                    Dwg_Resbuf *xdata)
{
  const char *target_layer = layer_name ? layer_name : "__RAW_UNASSIGNED__";

  for (Dwg_Resbuf *rbuf = xdata; rbuf; rbuf = rbuf->nextrb)
    {
      if (rbuf->type != 1000 && rbuf->type != 1004)
        continue;

      if (rbuf->type == 1000)
        {
          if (rbuf->value.str.is_tu)
            {
              char *utf8
                  = bit_TU_to_utf8_len (rbuf->value.str.u.wdata,
                                        (int)rbuf->value.str.size);
              append_useful_utf8_text (ctx, target_layer, utf8);
              free (utf8);
            }
          else if (rbuf->value.str.u.data)
            {
              BITCODE_RS codepage
                  = rbuf->value.str.codepage ? rbuf->value.str.codepage
                                             : ctx->raw_codepage;
              char *utf8 = bit_TV_to_utf8 (rbuf->value.str.u.data, codepage);
              append_useful_utf8_text (ctx, target_layer,
                                       utf8 ? utf8 : rbuf->value.str.u.data);
              if (utf8 && utf8 != rbuf->value.str.u.data)
                free (utf8);
            }
        }
      else
        append_raw_buffer_texts (ctx, target_layer,
                                 (BITCODE_RC *)rbuf->value.str.u.data,
                                 rbuf->value.str.size);
    }
}

static void extract_texts_from_object (TextExtractCtx *ctx, Dwg_Object *obj,
                                       const char *fallback_layer, int depth,
                                       int mark, int expand_inserts);

static void
walk_owned_block_entities (TextExtractCtx *ctx, Dwg_Object *hdr,
                           const char *fallback_layer, int depth, int mark,
                           int expand_inserts, int from_insert)
{
  Dwg_Object *ent;

  if (!hdr || hdr->fixedtype != DWG_TYPE_BLOCK_HEADER || depth > MAX_BLOCK_DEPTH)
    return;

  if (valid_index (ctx, hdr))
    {
      ctx->block_walked_seen[hdr->index] = 1;
      if (from_insert)
        ctx->expanded_block_seen[hdr->index] = 1;
    }

  ent = get_first_owned_entity (hdr);
  while (ent)
    {
      extract_texts_from_object (ctx, ent, fallback_layer, depth + 1, mark,
                                 expand_inserts);
      ent = get_next_owned_block_entity (hdr, ent);
    }
}

static void
extract_insert_attribs (TextExtractCtx *ctx, Dwg_Object *obj,
                        const char *fallback_layer, int depth)
{
  Dwg_Object *sub;

  if (depth > MAX_BLOCK_DEPTH)
    return;

  sub = get_first_owned_subentity (obj);
  while (sub)
    {
      Dwg_Object *next = get_next_owned_subentity (obj, sub);
      extract_texts_from_object (ctx, sub, fallback_layer, depth + 1, 1, 0);
      sub = next;
    }
}

static void
extract_table_attribs (TextExtractCtx *ctx, Dwg_Entity_TABLE *table,
                       const char *fallback_layer, int depth)
{
  BITCODE_BL i;

  if (!table || depth > MAX_BLOCK_DEPTH)
    return;

  for (i = 0; table->attribs && i < table->num_owned; i++)
    {
      Dwg_Object *attrib = dwg_ref_object (ctx->dwg, table->attribs[i]);
      extract_texts_from_object (ctx, attrib, fallback_layer, depth + 1, 1, 0);
    }
}

static void
extract_insert_block (TextExtractCtx *ctx, Dwg_Object *obj,
                      const char *fallback_layer, int depth)
{
  BITCODE_H block_header = NULL;
  Dwg_Object *hdr;

  if (!obj || depth > MAX_BLOCK_DEPTH)
    return;

  if (obj->fixedtype == DWG_TYPE_INSERT && obj->tio.entity->tio.INSERT)
    block_header = obj->tio.entity->tio.INSERT->block_header;
  else if (obj->fixedtype == DWG_TYPE_MINSERT && obj->tio.entity->tio.MINSERT)
    block_header = obj->tio.entity->tio.MINSERT->block_header;
  else if (obj->fixedtype == DWG_TYPE_TABLE && obj->tio.entity->tio.TABLE)
    block_header = obj->tio.entity->tio.TABLE->block_header;

  hdr = block_header ? dwg_ref_object (ctx->dwg, block_header) : NULL;
  if (hdr && hdr->fixedtype == DWG_TYPE_BLOCK_HEADER)
    walk_owned_block_entities (ctx, hdr, fallback_layer, depth + 1, 0, 1, 1);
}

static void
extract_leader_annotation (TextExtractCtx *ctx, Dwg_Object *obj,
                           Dwg_Entity_LEADER *leader,
                           const char *fallback_layer, int depth)
{
  Dwg_Object *annotation;

  if (!leader || !leader->associated_annotation || depth > MAX_BLOCK_DEPTH)
    return;

  annotation = dwg_ref_object (ctx->dwg, leader->associated_annotation);
  if (annotation && annotation != obj)
    extract_texts_from_object (ctx, annotation, fallback_layer, depth + 1, 1, 1);
}

static void
extract_texts_from_object (TextExtractCtx *ctx, Dwg_Object *obj,
                           const char *fallback_layer, int depth, int mark,
                           int expand_inserts)
{
  int type;
  char *layer_name;

  if (!obj || depth > MAX_BLOCK_DEPTH)
    return;
  if (mark && is_seen (ctx, obj))
    return;
  if (mark)
    mark_seen (ctx, obj);

  if (obj->supertype != DWG_SUPERTYPE_ENTITY || !obj->tio.entity)
    return;

  type = object_effective_type (obj);
  if (!is_text_container_type (type))
    return;

  layer_name = get_entity_layer_name_escaped (ctx, obj, fallback_layer);

  switch (type)
    {
    case DWG_TYPE_TEXT:
      if (obj->tio.entity->tio.TEXT)
        append_text_value (ctx, layer_name,
                           obj->tio.entity->tio.TEXT->text_value);
      break;
    case DWG_TYPE_MTEXT:
      if (obj->tio.entity->tio.MTEXT)
        append_text_value (ctx, layer_name, obj->tio.entity->tio.MTEXT->text);
      break;
    case DWG_TYPE_ATTRIB:
      if (obj->tio.entity->tio.ATTRIB)
        {
          Dwg_Entity_ATTRIB *attrib = obj->tio.entity->tio.ATTRIB;
          append_text_value (ctx, layer_name, attrib->text_value);
          if (attrib->mtext_type > 1)
            append_embedded_mtext (ctx, layer_name, &attrib->mtext);
        }
      break;
    case DWG_TYPE_ATTDEF:
      if (obj->tio.entity->tio.ATTDEF)
        {
          Dwg_Entity_ATTDEF *attdef = obj->tio.entity->tio.ATTDEF;
          append_text_value (ctx, layer_name, attdef->default_value);
          if (attdef->mtext_type > 1)
            append_embedded_mtext (ctx, layer_name, &attdef->mtext);
        }
      break;
    case DWG_TYPE_TOLERANCE:
      if (obj->tio.entity->tio.TOLERANCE)
        append_text_value (ctx, layer_name,
                           obj->tio.entity->tio.TOLERANCE->text_value);
      break;
    case DWG_TYPE_LEADER:
      extract_leader_annotation (ctx, obj, obj->tio.entity->tio.LEADER,
                                 layer_name, depth);
      break;
    case DWG_TYPE_INSERT:
    case DWG_TYPE_MINSERT:
      extract_insert_attribs (ctx, obj, layer_name, depth);
      if (expand_inserts)
        extract_insert_block (ctx, obj, layer_name, depth);
      break;
    case DWG_TYPE_MULTILEADER:
      append_mleader_texts (ctx, layer_name, obj->tio.entity->tio.MULTILEADER);
      break;
    case DWG_TYPE_TABLE:
      if (obj->tio.entity->tio.TABLE)
        {
          append_table_texts (ctx, layer_name, obj->tio.entity->tio.TABLE);
          extract_table_attribs (ctx, obj->tio.entity->tio.TABLE, layer_name,
                                 depth);
          if (expand_inserts)
            extract_insert_block (ctx, obj, layer_name, depth);
        }
      break;
    case DWG_TYPE_GEOPOSITIONMARKER:
      if (obj->tio.entity->tio.GEOPOSITIONMARKER)
        {
          Dwg_Entity_GEOPOSITIONMARKER *geo
              = obj->tio.entity->tio.GEOPOSITIONMARKER;
          append_text_value (ctx, layer_name, geo->notes);
          append_embedded_mtext (ctx, layer_name, &geo->mtext);
        }
      break;
    case DWG_TYPE_ARCALIGNEDTEXT:
      if (obj->tio.entity->tio.ARCALIGNEDTEXT)
        append_text_value (ctx, layer_name,
                           obj->tio.entity->tio.ARCALIGNEDTEXT->text_value);
      break;
    case DWG_TYPE_RTEXT:
      if (obj->tio.entity->tio.RTEXT)
        append_text_value (ctx, layer_name,
                           obj->tio.entity->tio.RTEXT->text_value);
      break;
    case DWG_TYPE_PROXY_ENTITY:
      if (obj->tio.entity->tio.PROXY_ENTITY)
        {
          Dwg_Entity_PROXY_ENTITY *proxy = obj->tio.entity->tio.PROXY_ENTITY;
          append_raw_buffer_texts (ctx, layer_name, proxy->proxy_data,
                                   proxy->proxy_data_size);
          append_raw_buffer_texts (ctx, layer_name, proxy->data,
                                   proxy->data_size);
        }
      append_raw_buffer_texts (ctx, layer_name, obj->unknown_bits,
                               (obj->num_unknown_bits + 7) / 8);
      append_raw_buffer_texts (ctx, layer_name, obj->unknown_rest,
                               (obj->num_unknown_rest + 7) / 8);
      break;
    case DWG_TYPE_UNKNOWN_ENT:
      append_raw_buffer_texts (ctx, layer_name, obj->unknown_bits,
                               (obj->num_unknown_bits + 7) / 8);
      append_raw_buffer_texts (ctx, layer_name, obj->unknown_rest,
                               (obj->num_unknown_rest + 7) / 8);
      break;
    default:
      if (is_dimension_type (type))
        append_dimension_text (ctx, layer_name, obj);
      break;
    }

  free (layer_name);
}

static int
owner_is_expanded_block (TextExtractCtx *ctx, const Dwg_Object *obj)
{
  Dwg_Object_Ref *owner_ref;
  Dwg_Object *owner;

  if (!obj || obj->supertype != DWG_SUPERTYPE_ENTITY || !obj->tio.entity)
    return 0;

  owner_ref = obj->tio.entity->ownerhandle;
  owner = owner_ref ? (owner_ref->obj ? owner_ref->obj
                                      : dwg_ref_object (ctx->dwg, owner_ref))
                    : NULL;
  return owner && owner->fixedtype == DWG_TYPE_BLOCK_HEADER
         && valid_index (ctx, owner) && ctx->expanded_block_seen[owner->index];
}

static void
walk_model_and_paper_space (TextExtractCtx *ctx)
{
  Dwg_Object *model = dwg_model_space_object (ctx->dwg);
  Dwg_Object *paper = dwg_paper_space_object (ctx->dwg);

  if (model && model->fixedtype == DWG_TYPE_BLOCK_HEADER)
    walk_owned_block_entities (ctx, model, NULL, 0, 1, 1, 0);
  if (paper && paper != model && paper->fixedtype == DWG_TYPE_BLOCK_HEADER)
    walk_owned_block_entities (ctx, paper, NULL, 0, 1, 1, 0);
}

static void
walk_unvisited_block_headers (TextExtractCtx *ctx)
{
  BITCODE_BL i;

  for (i = 0; i < ctx->dwg->num_objects; i++)
    {
      Dwg_Object *obj = &ctx->dwg->object[i];
      if (obj->fixedtype == DWG_TYPE_BLOCK_HEADER
          && !ctx->block_walked_seen[obj->index])
        walk_owned_block_entities (ctx, obj, NULL, 0, 1, 1, 0);
    }
}

static void
walk_unvisited_text_objects (TextExtractCtx *ctx)
{
  BITCODE_BL i;

  for (i = 0; i < ctx->dwg->num_objects; i++)
    {
      Dwg_Object *obj = &ctx->dwg->object[i];
      if (is_seen (ctx, obj) || owner_is_expanded_block (ctx, obj))
        continue;
      if (obj->supertype == DWG_SUPERTYPE_ENTITY
          && is_text_container_type (object_effective_type (obj)))
        extract_texts_from_object (ctx, obj, NULL, 0, 1, 1);
    }
}

static void
walk_raw_text_objects (TextExtractCtx *ctx)
{
  BITCODE_BL i;

  for (i = 0; i < ctx->dwg->num_objects; i++)
    {
      Dwg_Object *obj = &ctx->dwg->object[i];
      char *layer_name = NULL;

      if (obj->supertype == DWG_SUPERTYPE_ENTITY)
        layer_name = get_entity_layer_name_escaped (ctx, obj, NULL);

      if (obj->supertype == DWG_SUPERTYPE_ENTITY && obj->tio.entity)
        append_eed_texts (ctx, layer_name, obj->tio.entity->eed,
                          obj->tio.entity->num_eed);
      else if (obj->supertype == DWG_SUPERTYPE_OBJECT && obj->tio.object)
        append_eed_texts (ctx, "__RAW_UNASSIGNED__", obj->tio.object->eed,
                          obj->tio.object->num_eed);

      switch (obj->fixedtype)
        {
        case DWG_TYPE_PROXY_ENTITY:
          if (obj->supertype == DWG_SUPERTYPE_ENTITY && obj->tio.entity
              && obj->tio.entity->tio.PROXY_ENTITY)
            {
              Dwg_Entity_PROXY_ENTITY *proxy
                  = obj->tio.entity->tio.PROXY_ENTITY;
              append_raw_buffer_texts (ctx, layer_name ? layer_name
                                                        : "__RAW_UNASSIGNED__",
                                       proxy->proxy_data,
                                       proxy->proxy_data_size);
              append_raw_buffer_texts (ctx, layer_name ? layer_name
                                                        : "__RAW_UNASSIGNED__",
                                       proxy->data, proxy->data_size);
            }
          break;
        case DWG_TYPE_PROXY_OBJECT:
          if (obj->tio.object && obj->tio.object->tio.PROXY_OBJECT)
            {
              Dwg_Object_PROXY_OBJECT *proxy
                  = obj->tio.object->tio.PROXY_OBJECT;
              append_raw_buffer_texts (ctx, "__RAW_UNASSIGNED__",
                                       proxy->data, proxy->data_size);
            }
          break;
        case DWG_TYPE_UNKNOWN_OBJ:
        case DWG_TYPE_XRECORD:
          if (obj->fixedtype == DWG_TYPE_XRECORD && obj->tio.object
              && obj->tio.object->tio.XRECORD)
            append_xdata_texts (ctx, "__RAW_UNASSIGNED__",
                                obj->tio.object->tio.XRECORD->xdata);
          append_raw_buffer_texts (ctx, "__RAW_UNASSIGNED__",
                                   obj->unknown_bits,
                                   (obj->num_unknown_bits + 7) / 8);
          append_raw_buffer_texts (ctx, "__RAW_UNASSIGNED__",
                                   obj->unknown_rest,
                                   (obj->num_unknown_rest + 7) / 8);
          break;
        default:
          break;
        }

      append_raw_buffer_texts (ctx, layer_name ? layer_name
                                                : "__RAW_UNASSIGNED__",
                               obj->unknown_bits,
                               (obj->num_unknown_bits + 7) / 8);
      append_raw_buffer_texts (ctx, layer_name ? layer_name
                                                : "__RAW_UNASSIGNED__",
                               obj->unknown_rest,
                               (obj->num_unknown_rest + 7) / 8);
      free (layer_name);
    }
}

static void
append_layer_name_text_fallbacks (TextExtractCtx *ctx)
{
  BITCODE_BL i;
  const char *dump_path;
  FILE *dump = NULL;

  if (!ctx || !ctx->dwg)
    return;

  dump_path = getenv ("LIBDWG_GEOJSON_LAYER_ENTITY_DUMP");
  if (dump_path && dump_path[0])
    dump = fopen (dump_path, "wb");
  if (dump)
    fprintf (dump, "layer\ttype\thandle\n");

  for (i = 0; i < ctx->dwg->num_objects; i++)
    {
      Dwg_Object *obj = &ctx->dwg->object[i];
      char *layer_name;

      if (obj->supertype != DWG_SUPERTYPE_ENTITY || !obj->tio.entity)
        continue;

      layer_name = get_entity_layer_name_escaped (ctx, obj, NULL);
      if (layer_name && dump && utf8_text_has_cjk (layer_name))
        fprintf (dump, "%s\t%d\t%llX\n", layer_name,
                 object_effective_type (obj),
                 (unsigned long long)obj->handle.value);
      if (is_layer_name_text_fallback_type (object_effective_type (obj))
          && layer_name && utf8_text_has_cjk (layer_name)
          && strncmp (layer_name, "__", 2) != 0)
        append_raw_text_to_layer (ctx, layer_name, layer_name);
      free (layer_name);
    }
  if (dump)
    fclose (dump);
}

static Dwg_Object_STYLE *
style_from_ref (TextExtractCtx *ctx, Dwg_Object_Ref *ref,
                BITCODE_RLL *style_handle)
{
  Dwg_Object *obj;

  if (style_handle)
    *style_handle = ref ? (ref->absolute_ref ? ref->absolute_ref
                                             : ref->handleref.value)
                        : 0;
  if (!ctx || !ref)
    return NULL;
  obj = ref->obj ? ref->obj : dwg_ref_object (ctx->dwg, ref);
  if (!obj || obj->fixedtype != DWG_TYPE_STYLE || !obj->tio.object
      || !obj->tio.object->tio.STYLE)
    return NULL;
  return obj->tio.object->tio.STYLE;
}

static int
text_is_single_punctuation (const char *text)
{
  if (!text || !text[0])
    return 1;
  if (!text[1] && (text[0] == ',' || text[0] == '.' || text[0] == ';'
                   || text[0] == ':' || text[0] == '?' || text[0] == '!'))
    return 1;
  return 0;
}

static int
font_name_is_shx (const char *font)
{
  return font && (strstr (font, ".shx") || strstr (font, ".SHX"));
}

static void
fprint_tsv_field (FILE *fp, const char *text)
{
  if (!text)
    return;
  for (const unsigned char *p = (const unsigned char *)text; *p; p++)
    fputc (*p == '\t' || *p == '\r' || *p == '\n' ? ' ' : *p, fp);
}

static void
dump_bigfont_text_diagnostics (TextExtractCtx *ctx)
{
  const char *path = getenv ("LIBDWG_GEOJSON_BIGFONT_DIAG");
  FILE *fp;

  if (!ctx || !ctx->dwg || !path || !path[0])
    return;
  fp = fopen (path, "wb");
  if (!fp)
    {
      fprintf (stderr, "GeoJSON bigfont diagnostic dump failed: %s\n", path);
      return;
    }

  fprintf (fp, "handle\tlayer\ttype\tstyle_handle\tstyle_name\tfont_file\t"
               "bigfont_file\tsuspicious\tdecoded_text\n");
  for (BITCODE_BL i = 0; i < ctx->dwg->num_objects; i++)
    {
      Dwg_Object *obj = &ctx->dwg->object[i];
      Dwg_Object_Ref *style_ref = NULL;
      BITCODE_T value = NULL;
      Dwg_Object_STYLE *style;
      BITCODE_RLL style_handle = 0;
      char *layer = NULL;
      char *style_name = NULL;
      char *font = NULL;
      char *bigfont = NULL;
      char *text = NULL;
      int type;
      int interesting;

      if (obj->supertype != DWG_SUPERTYPE_ENTITY || !obj->tio.entity)
        continue;
      type = object_effective_type (obj);
      switch (type)
        {
        case DWG_TYPE_TEXT:
          if (obj->tio.entity->tio.TEXT)
            {
              value = obj->tio.entity->tio.TEXT->text_value;
              style_ref = obj->tio.entity->tio.TEXT->style;
            }
          break;
        case DWG_TYPE_MTEXT:
          if (obj->tio.entity->tio.MTEXT)
            {
              value = obj->tio.entity->tio.MTEXT->text;
              style_ref = obj->tio.entity->tio.MTEXT->style;
            }
          break;
        case DWG_TYPE_ATTRIB:
          if (obj->tio.entity->tio.ATTRIB)
            {
              value = obj->tio.entity->tio.ATTRIB->text_value;
              style_ref = obj->tio.entity->tio.ATTRIB->style;
            }
          break;
        case DWG_TYPE_ATTDEF:
          if (obj->tio.entity->tio.ATTDEF)
            {
              value = obj->tio.entity->tio.ATTDEF->default_value;
              style_ref = obj->tio.entity->tio.ATTDEF->style;
            }
          break;
        default:
          break;
        }
      if (!style_ref)
        continue;

      style = style_from_ref (ctx, style_ref, &style_handle);
      if (!style)
        continue;
      style_name = convert_text_alloc (ctx, style->name);
      font = convert_text_alloc (ctx, style->font_file);
      bigfont = convert_text_alloc (ctx, style->bigfont_file);
      text = convert_text_alloc (ctx, value);
      interesting = (bigfont && bigfont[0]) || font_name_is_shx (font);
      if (!interesting)
        goto cleanup;

      layer = get_entity_layer_name_escaped (ctx, obj, NULL);
      fprintf (fp, "%llX\t", (unsigned long long)obj->handle.value);
      fprint_tsv_field (fp, layer);
      fprintf (fp, "\t%d\t%llX\t", type, (unsigned long long)style_handle);
      fprint_tsv_field (fp, style_name);
      fputc ('\t', fp);
      fprint_tsv_field (fp, font);
      fputc ('\t', fp);
      fprint_tsv_field (fp, bigfont);
      fprintf (fp, "\t%d\t", text_is_single_punctuation (text));
      fprint_tsv_field (fp, text);
      fputc ('\n', fp);

    cleanup:
      free (layer);
      free (style_name);
      free (font);
      free (bigfont);
      free (text);
    }

  fclose (fp);
}

static int
build_output_json (DwgLayers *layers, char **out)
{
  DynStr ds;
  int first = 1;
  int l;

  dynstr_init (&ds);
  if (!ds.buf)
    return 1;

  dynstr_puts (&ds, "[\n");
  for (l = 0; l < layers->num_layers; l++)
    {
      int t;

      if (!first)
        dynstr_puts (&ds, ",\n");
      first = 0;

      dynstr_printf (&ds, "  {\"Layer\": \"%s\", \"Text\": [",
                     layers->layers[l].name);
      if (layers->layers[l].text_count > 0)
        {
          dynstr_putc (&ds, '\n');
          for (t = 0; t < layers->layers[l].text_count; t++)
            {
              dynstr_printf (&ds, "      \"%s\"", layers->layers[l].texts[t]);
              if (t < layers->layers[l].text_count - 1)
                dynstr_putc (&ds, ',');
              dynstr_putc (&ds, '\n');
            }
          dynstr_puts (&ds, "    ]");
        }
      else
        dynstr_putc (&ds, ']');
      dynstr_putc (&ds, '}');
    }
  dynstr_puts (&ds, "\n]\n");

  if (!ds.buf)
    return 1;
  *out = ds.buf;
  return 0;
}

EXPORT int
dwg_geojson_layers_text (char **cszTextOut, Dwg_Data *restrict dwg)
{
  TextExtractCtx ctx;
  int error;

  if (!cszTextOut || !dwg)
    return 1;
  *cszTextOut = NULL;

  memset (&ctx, 0, sizeof (ctx));
  ctx.dwg = dwg;
  ctx.object_count = dwg->num_objects;
  ctx.from_tu
      = (dwg->header.version >= R_2007 || dwg->header.from_version >= R_2007)
        && !(dwg->opts & DWG_OPTS_IN);
  ctx.free_table_names = IS_FROM_TU_DWG (dwg) ? 1 : 0;
  ctx.codepage = ctx.from_tu ? 0 : dwg->header.codepage;
  ctx.raw_codepage = dwg->header.codepage;

  ctx.object_seen
      = (unsigned char *)calloc (ctx.object_count ? ctx.object_count : 1,
                                 sizeof (*ctx.object_seen));
  ctx.block_walked_seen
      = (unsigned char *)calloc (ctx.object_count ? ctx.object_count : 1,
                                 sizeof (*ctx.block_walked_seen));
  ctx.expanded_block_seen
      = (unsigned char *)calloc (ctx.object_count ? ctx.object_count : 1,
                                 sizeof (*ctx.expanded_block_seen));
  if (!ctx.object_seen || !ctx.block_walked_seen || !ctx.expanded_block_seen)
    {
      free (ctx.object_seen);
      free (ctx.block_walked_seen);
      free (ctx.expanded_block_seen);
      return 1;
    }

  add_layers_from_layer_control (&ctx);
  add_all_layer_objects (&ctx);
  walk_model_and_paper_space (&ctx);
  walk_unvisited_block_headers (&ctx);
  walk_unvisited_text_objects (&ctx);
  walk_raw_text_objects (&ctx);
  append_layer_name_text_fallbacks (&ctx);
  append_raw_recovered_texts (&ctx);
  dump_bigfont_text_diagnostics (&ctx);

  error = build_output_json (&ctx.layers, cszTextOut);

  free_all_layers (&ctx.layers);
  free (ctx.object_seen);
  free (ctx.block_walked_seen);
  free (ctx.expanded_block_seen);

  return error;
}
