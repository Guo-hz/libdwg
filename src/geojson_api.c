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
  int trim_output_text;
  BITCODE_RS codepage;
  FILE *diag;
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

static BITCODE_RLL
entity_owner_abs (const Dwg_Object *obj)
{
  if (!obj || obj->supertype != DWG_SUPERTYPE_ENTITY || !obj->tio.entity)
    return 0;
  return ref_abs_value (obj->tio.entity->ownerhandle);
}

static BITCODE_RLL
entity_style_abs (const Dwg_Object *obj)
{
  if (!obj || obj->supertype != DWG_SUPERTYPE_ENTITY || !obj->tio.entity)
    return 0;
  switch (obj->fixedtype)
    {
    case DWG_TYPE_TEXT:
      return obj->tio.entity->tio.TEXT
                 ? ref_abs_value (obj->tio.entity->tio.TEXT->style)
                 : 0;
    case DWG_TYPE_ATTRIB:
      return obj->tio.entity->tio.ATTRIB
                 ? ref_abs_value (obj->tio.entity->tio.ATTRIB->style)
                 : 0;
    case DWG_TYPE_ATTDEF:
      return obj->tio.entity->tio.ATTDEF
                 ? ref_abs_value (obj->tio.entity->tio.ATTDEF->style)
                 : 0;
    default:
      return 0;
    }
}

static char *
entity_text_for_diag (TextExtractCtx *ctx, const Dwg_Object *obj)
{
  if (!obj || obj->supertype != DWG_SUPERTYPE_ENTITY || !obj->tio.entity)
    return NULL;
  switch (obj->fixedtype)
    {
    case DWG_TYPE_TEXT:
      return obj->tio.entity->tio.TEXT
                 ? convert_text_alloc (ctx, obj->tio.entity->tio.TEXT->text_value)
                 : NULL;
    case DWG_TYPE_ATTRIB:
      return obj->tio.entity->tio.ATTRIB
                 ? convert_text_alloc (ctx, obj->tio.entity->tio.ATTRIB->text_value)
                 : NULL;
    case DWG_TYPE_ATTDEF:
      return obj->tio.entity->tio.ATTDEF
                 ? convert_text_alloc (ctx,
                                       obj->tio.entity->tio.ATTDEF->default_value)
                 : NULL;
    case DWG_TYPE_MTEXT:
      return obj->tio.entity->tio.MTEXT
                 ? convert_text_alloc (ctx, obj->tio.entity->tio.MTEXT->text)
                 : NULL;
    default:
      return NULL;
    }
}

static void
write_entity_diag (TextExtractCtx *ctx)
{
  BITCODE_BL i;

  if (!ctx->diag)
    return;
  fprintf (ctx->diag,
           "index\thandle\taddress\tsize\tbitsize\ttype\tfixedtype\tlayer\towner\tprev\tnext\tstyle\tinvisible\tcolor\tltype_flags\ttext_x\ttext_y\ttext_height\ttext_rotation\ttext_flags\ttext\n");
  for (i = 0; i < ctx->dwg->num_objects; i++)
    {
      Dwg_Object *obj = &ctx->dwg->object[i];
      char *layer_name;
      char *text;
      char *text_esc = NULL;

      if (obj->supertype != DWG_SUPERTYPE_ENTITY || !obj->tio.entity)
        continue;
      layer_name = get_entity_layer_name_escaped (ctx, obj, NULL);
      text = entity_text_for_diag (ctx, obj);
      if (text)
        text_esc = escape_string_alloc (ctx, text, ctx->codepage);
      {
        double text_x = 0.0, text_y = 0.0, text_height = 0.0;
        double text_rotation = 0.0;
        unsigned text_flags = 0;
        if (obj->fixedtype == DWG_TYPE_TEXT && obj->tio.entity->tio.TEXT)
          {
            Dwg_Entity_TEXT *txt = obj->tio.entity->tio.TEXT;
            text_x = txt->ins_pt.x;
            text_y = txt->ins_pt.y;
            text_height = txt->height;
            text_rotation = txt->rotation;
            text_flags = txt->dataflags;
          }
        fprintf (ctx->diag,
                 "%lu\t%llX\t%" PRIuSIZE "\t%lu\t%lu\t%s\t%d\t%s\t%llX\t%llX\t%llX\t%llX\t%u\t%d\t%u\t%.17g\t%.17g\t%.17g\t%.17g\t%u\t%s\n",
                 (unsigned long)obj->index,
                 (unsigned long long)obj->handle.value, obj->address,
                 (unsigned long)obj->size, (unsigned long)obj->bitsize,
                 obj->name ? obj->name : (obj->dxfname ? obj->dxfname : ""),
                 (int)obj->fixedtype, layer_name ? layer_name : "",
                 (unsigned long long)entity_owner_abs (obj),
                 (unsigned long long)(obj->tio.entity->prev_entity
                                           ? obj->tio.entity->prev_entity
                                                 ->absolute_ref
                                           : 0),
                 (unsigned long long)(obj->tio.entity->next_entity
                                           ? obj->tio.entity->next_entity
                                                 ->absolute_ref
                                           : 0),
                 (unsigned long long)entity_style_abs (obj),
                 (unsigned)obj->tio.entity->invisible,
                 (int)obj->tio.entity->color.index,
                 (unsigned)obj->tio.entity->ltype_flags, text_x, text_y,
                 text_height, text_rotation, text_flags, text_esc ? text_esc : "");
      }
      free (text_esc);
      free (text);
      free (layer_name);
    }
}

static void
append_raw_text_to_layer (TextExtractCtx *ctx, const char *layer_name,
                          const char *raw_text)
{
  char *trimmed;
  char *start;
  char *end;
  char *json_text;
  int idx;

  if (!raw_text || raw_text[0] == '\0')
    return;

  if (ctx->trim_output_text)
    {
      trimmed = strdup (raw_text);
      if (!trimmed)
        return;
      start = trimmed;
      while (*start == ' ' || *start == '\t' || *start == '\r'
             || *start == '\n' || *start == '\f' || *start == '\v')
        start++;
      end = start + strlen (start);
      while (end > start
             && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'
                 || end[-1] == '\n' || end[-1] == '\f'
                 || end[-1] == '\v'))
        end--;
      *end = '\0';
      if (start[0] == '\0')
        {
          free (trimmed);
          return;
        }
    }
  else
    {
      trimmed = NULL;
      start = (char *)raw_text;
    }

  json_text = escape_string_alloc (ctx, start, ctx->codepage);
  if (!json_text)
    {
      free (trimmed);
      return;
    }

  idx = find_or_create_layer (&ctx->layers,
                              layer_name && layer_name[0] ? layer_name
                                                          : "__UNKNOWN__");
  if (idx >= 0)
    add_text_to_layer (&ctx->layers, idx, json_text);
  free (json_text);
  free (trimmed);
}

static void
append_text_value (TextExtractCtx *ctx, const char *layer_name, BITCODE_T value)
{
  char *raw = convert_text_alloc (ctx, value);
  if (raw)
    {
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
      return 1;
    default:
      return 0;
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

  type = obj->fixedtype;
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
          && is_text_container_type (obj->fixedtype))
        extract_texts_from_object (ctx, obj, NULL, 0, 1, 1);
    }
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

int
dwg_geojson_layers_text_impl (char **cszTextOut, Dwg_Data *restrict dwg)
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
  ctx.trim_output_text = dwg->r2007_text_span_recovery_active ? 1 : 0;
  ctx.codepage = ctx.from_tu ? 0 : dwg->header.codepage;
  {
    const char *diag_path = getenv ("LIBDWG_GEOJSON_ENTITY_DIAG");
    if (diag_path && diag_path[0])
      ctx.diag = fopen (diag_path, "wb");
  }

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
      if (ctx.diag)
        fclose (ctx.diag);
      free (ctx.object_seen);
      free (ctx.block_walked_seen);
      free (ctx.expanded_block_seen);
      return 1;
    }

  add_layers_from_layer_control (&ctx);
  add_all_layer_objects (&ctx);
  write_entity_diag (&ctx);
  walk_model_and_paper_space (&ctx);
  walk_unvisited_block_headers (&ctx);
  walk_unvisited_text_objects (&ctx);

  error = build_output_json (&ctx.layers, cszTextOut);

  if (ctx.diag)
    fclose (ctx.diag);
  free_all_layers (&ctx.layers);
  free (ctx.object_seen);
  free (ctx.block_walked_seen);
  free (ctx.expanded_block_seen);

  return error;
}
