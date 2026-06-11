#include "out_json.h"          // 包含 EXPORT, Dwg_Data, Bit_Chain 等
#include "dwg.h"               // libredwg 主头文件
#include "dwg_api.h"            // dwg_obj_table_get_name 等 API 声明
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>



/* ---------- 线程安全的图层缓存结构 ---------- */
typedef struct {
	char *name;
	char **texts;
	int text_count;
	int text_capacity;
} LayerTexts;

typedef struct {
	LayerTexts *layers;
	int num_layers;
	int layers_capacity;
} DwgLayers;

#define INIT_LAYERS_CAP  256
#define INIT_TEXTS_CAP   256
#define TEXTS_GROW_FACTOR 2

static int find_or_create_layer(DwgLayers *dl, const char *name)
{
	for (int i = 0; i < dl->num_layers; i++) {
		if (strcmp(dl->layers[i].name, name) == 0)
			return i;
	}

	/* 扩容图层数组 */
	if (dl->num_layers >= dl->layers_capacity) {
		int new_cap = dl->layers_capacity == 0 ? INIT_LAYERS_CAP : dl->layers_capacity * 2;
		LayerTexts *tmp = realloc(dl->layers, new_cap * sizeof(LayerTexts));
		if (!tmp) return -1;
		dl->layers = tmp;
		dl->layers_capacity = new_cap;
	}

	int idx = dl->num_layers++;
	dl->layers[idx].name = strdup(name);
	if (!dl->layers[idx].name) return -1;
	dl->layers[idx].texts = malloc(INIT_TEXTS_CAP * sizeof(char*));
	if (!dl->layers[idx].texts) {
		free(dl->layers[idx].name);
		return -1;
	}
	dl->layers[idx].text_count = 0;
	dl->layers[idx].text_capacity = INIT_TEXTS_CAP;
	return idx;
}

static int add_text_to_layer(DwgLayers *dl, int idx, const char *text)
{
	if (idx < 0 || idx >= dl->num_layers) return -1;
	LayerTexts *layer = &dl->layers[idx];

	if (layer->text_count >= layer->text_capacity) {
		int new_cap = layer->text_capacity * TEXTS_GROW_FACTOR;
		char **tmp = realloc(layer->texts, new_cap * sizeof(char*));
		if (!tmp) return -1;
		layer->texts = tmp;
		layer->text_capacity = new_cap;
	}

	layer->texts[layer->text_count] = strdup(text);
	if (!layer->texts[layer->text_count]) return -1;
	layer->text_count++;
	return 0;
}

static void free_all_layers(DwgLayers *dl)
{
	for (int i = 0; i < dl->num_layers; i++) {
		free(dl->layers[i].name);
		for (int j = 0; j < dl->layers[i].text_count; j++)
			free(dl->layers[i].texts[j]);
		free(dl->layers[i].texts);
	}
	free(dl->layers);
	dl->layers = NULL;
	dl->num_layers = 0;
	dl->layers_capacity = 0;
}

/* ---------- 动态字符串缓冲区（线程安全，基于栈） ---------- */
typedef struct {
	char *buf;
	size_t len;
	size_t cap;
} DynStr;

static void dynstr_init(DynStr *ds)
{
	ds->cap = 4096;
	ds->buf = malloc(ds->cap);
	if (ds->buf) {
		ds->buf[0] = '\0';
		ds->len = 0;
	}
}

static void dynstr_free(DynStr *ds)
{
	free(ds->buf);
	ds->buf = NULL;
	ds->len = ds->cap = 0;
}

static int dynstr_ensure(DynStr *ds, size_t add)
{
	if (ds->len + add + 1 > ds->cap) {
		size_t new_cap = ds->cap * 2;
		while (new_cap < ds->len + add + 1)
			new_cap *= 2;
		char *tmp = realloc(ds->buf, new_cap);
		if (!tmp) return 0;
		ds->buf = tmp;
		ds->cap = new_cap;
	}
	return 1;
}

static void dynstr_printf(DynStr *ds, const char *fmt, ...)
{
	va_list args;
	int needed;

	va_start(args, fmt);
	needed = vsnprintf(NULL, 0, fmt, args);
	va_end(args);

	if (needed < 0) return;
	if (!dynstr_ensure(ds, needed)) return;

	va_start(args, fmt);
	vsnprintf(ds->buf + ds->len, needed + 1, fmt, args);
	va_end(args);
	ds->len += needed;
}

static void dynstr_puts(DynStr *ds, const char *s)
{
	dynstr_printf(ds, "%s", s);
}

static void dynstr_putc(DynStr *ds, char c)
{
	if (!dynstr_ensure(ds, 1)) return;
	ds->buf[ds->len++] = c;
	ds->buf[ds->len] = '\0';
}

/* ============================================================
 * 公开 API：提取所有文本并按图层组织成 GeoJSON 字符串
 * 返回的字符串由调用者负责 free(*cszTextOut)
 * 完全线程安全，无共享状态
 * ============================================================ */
EXPORT int
dwg_geojson_layers_text(char **cszTextOut, Dwg_Data *restrict dwg)
{
	if (!cszTextOut || !dwg) return 1;
	*cszTextOut = NULL;

	int from_tu = (dwg->header.version >= R_2007) && !(dwg->opts & DWG_OPTS_IN);
	int codepage = from_tu ? 0 : dwg->header.codepage;

	DwgLayers dl;
	memset(&dl, 0, sizeof(dl));

	BITCODE_BL i;
	for (i = 0; i < dwg->num_objects; i++) {
		Dwg_Object *obj = &dwg->object[i];
		int ftype = obj->fixedtype;

		/* ---------- 1. 图层名称 ---------- */
		if (ftype == DWG_TYPE_LAYER) {
			int error;
			char *name = dwg_obj_table_get_name(obj, &error);
			if (!error && name) {
				// 安全拷贝，不再直接使用 name
				char *safe_name = strdup(name);
				if (from_tu) free(name);   // 按规则释放原指针
				if (!safe_name) continue;

				size_t len = strlen(safe_name);
				size_t esc_max = len * 2 + 3;
				char *layer_name_esc = malloc(esc_max);
				if (layer_name_esc) {
					json_cquote(layer_name_esc, safe_name, esc_max, codepage);
					if (layer_name_esc[0] != '\0')
						find_or_create_layer(&dl, layer_name_esc);
					free(layer_name_esc);
				}
				free(safe_name);
			}
			continue;
		}

		/* ---------- 2. 文本实体 ---------- */
		if (ftype != DWG_TYPE_TEXT &&
			ftype != DWG_TYPE_MTEXT &&
			ftype != DWG_TYPE_ATTRIB &&
			ftype != DWG_TYPE_ATTDEF &&
			ftype != DWG_TYPE_GEOPOSITIONMARKER)
			continue;

		/* 获取实体图层名（同样安全拷贝） */
		char *layer_name_esc = NULL;
		if (obj->supertype == DWG_SUPERTYPE_ENTITY && obj->tio.entity->layer) {
			int error;
			Dwg_Object *lobj = obj->tio.entity->layer->obj;
			if (lobj && (lobj->fixedtype == DWG_TYPE_LAYER ||
				lobj->fixedtype == DWG_TYPE_DICTIONARY)) {
				char *name = dwg_obj_table_get_name(lobj, &error);
				if (!error && name) {
					char *safe_name = strdup(name);
					if (from_tu) free(name);
					if (safe_name) {
						size_t len = strlen(safe_name);
						size_t esc_max = len * 2 + 3;
						layer_name_esc = malloc(esc_max);
						if (layer_name_esc) {
							json_cquote(layer_name_esc, safe_name, esc_max, codepage);
						}
						free(safe_name);
					}
				}
			}
		}
		if (!layer_name_esc)
			layer_name_esc = strdup("__UNKNOWN__");

		/* 提取文字内容 */
		char *raw_text = NULL;
		BITCODE_TV tv = NULL;
		switch (ftype) {
		case DWG_TYPE_TEXT:
			if (obj->tio.entity->tio.TEXT)
				tv = obj->tio.entity->tio.TEXT->text_value;
			break;
		case DWG_TYPE_MTEXT:
			if (obj->tio.entity->tio.MTEXT)
				tv = obj->tio.entity->tio.MTEXT->text;
			break;
		case DWG_TYPE_ATTRIB:
			if (obj->tio.entity->tio.ATTRIB)
				tv = obj->tio.entity->tio.ATTRIB->text_value;
			break;
		case DWG_TYPE_ATTDEF:
			if (obj->tio.entity->tio.ATTDEF)
				tv = obj->tio.entity->tio.ATTDEF->default_value;
			break;
		case DWG_TYPE_GEOPOSITIONMARKER:
			if (obj->tio.entity->tio.GEOPOSITIONMARKER)
				tv = obj->tio.entity->tio.GEOPOSITIONMARKER->notes;
			break;
		}

		if (tv) {
			char *txt = from_tu ? bit_convert_TU((BITCODE_TU)tv) : (char*)tv;
			if (txt) {
				raw_text = strdup(txt);
				if (from_tu) free(txt);
			}
		}

		if (raw_text) {
			size_t len = strlen(raw_text);
			size_t esc_max = len * 2 + 10;
			char *json_text = malloc(esc_max);
			if (json_text) {
				json_cquote(json_text, raw_text, esc_max, codepage);
				int idx = find_or_create_layer(&dl, layer_name_esc);
				if (idx >= 0)
					add_text_to_layer(&dl, idx, json_text);
				free(json_text);
			}
			free(raw_text);
		}
		free(layer_name_esc);
	}

	/* 生成 JSON 字符串 */
	DynStr ds;
	dynstr_init(&ds);
	if (!ds.buf) {
		free_all_layers(&dl);
		return 1;
	}

	dynstr_puts(&ds, "[\n");
	int first = 1;

	for (int l = 0; l < dl.num_layers; l++) {
		if (!first)
			dynstr_puts(&ds, ",\n");
		else
			first = 0;

		dynstr_printf(&ds, "  {\"Layer\": \"%s\", \"Text\": [", dl.layers[l].name);
		if (dl.layers[l].text_count > 0) {
			dynstr_putc(&ds, '\n');
			for (int t = 0; t < dl.layers[l].text_count; t++) {
				dynstr_printf(&ds, "      \"%s\"", dl.layers[l].texts[t]);
				if (t < dl.layers[l].text_count - 1)
					dynstr_putc(&ds, ',');
				dynstr_putc(&ds, '\n');
			}
			dynstr_puts(&ds, "    ]");
		}
		else {
			dynstr_putc(&ds, ']');
		}
		dynstr_putc(&ds, '}');
	}

	dynstr_puts(&ds, "\n]\n");
	*cszTextOut = ds.buf;

	free_all_layers(&dl);
	return 0;
}