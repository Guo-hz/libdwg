/*****************************************************************************/
/*  LibreDWG - free implementation of the DWG file format                    */
/*                                                                           */
/*  Copyright (C) 2018-2023 Free Software Foundation, Inc.                   */
/*                                                                           */
/*  This library is free software, licensed under the terms of the GNU       */
/*  General Public License as published by the Free Software Foundation,     */
/*  either version 3 of the License, or (at your option) any later version.  */
/*  You should have received a copy of the GNU General Public License        */
/*  along with this program.  If not, see <http://www.gnu.org/licenses/>.    */
/*****************************************************************************/

/*
 * out_geojson.c: write as GeoJSON
 * written by Reini Urban
 */
/* FIXME: Arc, Circle, Ellipsis, Bulge (Curve) arc_split.
 * TODO: ocs/ucs transforms, explode of inserts?
 *       NOCOMMA:
 *         We really have to add the comma before, not after, and special case
 *         the first field, not the last to omit the comma.
 *       GeoJSON 2008 or newer RFC7946
 * https://tools.ietf.org/html/rfc7946#appendix-B For the new format we need to
 * follow the right-hand rule for orientation (counterclockwise polygons).
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <assert.h>
#include <stdarg.h>//---------------------------new

#define IS_PRINT
#include "dwg.h"
#define DWG_LOGLEVEL DWG_LOGLEVEL_NONE
#include "logging.h"
#include "dwg_api.h"

#include "common.h"
#include "bits.h"
#include "dwg.h"
#include "decode.h"
#include "out_json.h"
#include "dwg_text_api.h"
#include "geom.h"
//extern char *convert_to_utf8(const char *str, int dwg_cp);//------------------------------------------------------------newnewnewnenwnewn
/* the current version per spec block */
// static unsigned int cur_ver = 0;

/* https://tools.ietf.org/html/rfc7946#section-11.2 recommends.
   Set via --with-geojson-precision=rfc */
#undef FORMAT_RD
#ifndef GEOJSON_PRECISION
#  define GEOJSON_PRECISION 6
#endif
#define FORMAT_RD "%0." _XSTR (GEOJSON_PRECISION) "f"
// #define FORMAT_RD "%f"
#undef FORMAT_BD
#define FORMAT_BD FORMAT_RD

/*--------------------------------------------------------------------------------
 * See http://geojson.org/geojson-spec.html
 * Arc, AttributeDefinition, BlockReference, Ellipse, Hatch, Line,
   MText, Point, Polyline, Spline, Text =>
 * Point, LineString, Polygon, MultiPoint, MultiLineString, MultiPolygon
 * { "type": "FeatureCollection",
     "features": [
       { "type": "Feature",
         "properties":
           { "Layer": "SomeLayer",
             "SubClasses": "AcDbEntity:AcDbLine",
             "ExtendedEntity": null,
             "Linetype": null,
             "EntityHandle": "8B",
             "Text": null
           },
         "geometry":
           { "type": "LineString",
             "coordinates": [
               [ 370.858611, 730.630303 ],
               [ 450.039756, 619.219273 ]
             ]
           }
       },
     ], ...
   }
 *
 * MACROS
 */

#define ACTION geojson

#define PREFIX                                                                \
  for (int _i = 0; _i < dat->bit; _i++)                                       \
    {                                                                         \
      fprintf (dat->fh, "  ");                                                \
    }
#define ARRAY                                                                 \
  {                                                                           \
    PREFIX fprintf (dat->fh, "[\n");                                          \
    dat->bit++;                                                               \
  }
#define SAMEARRAY                                                             \
  {                                                                           \
    PREFIX fprintf (dat->fh, "[");                                            \
    dat->bit++;                                                               \
  }
#define ENDARRAY                                                              \
  {                                                                           \
    dat->bit--;                                                               \
    PREFIX fprintf (dat->fh, "],\n");                                         \
  }
#define LASTENDARRAY                                                          \
  {                                                                           \
    dat->bit--;                                                               \
    PREFIX fprintf (dat->fh, "]\n");                                          \
  }
#define HASH                                                                  \
  {                                                                           \
    PREFIX fprintf (dat->fh, "{\n");                                          \
    dat->bit++;                                                               \
  }
#define SAMEHASH                                                              \
  {                                                                           \
    fprintf (dat->fh, "{\n");                                                 \
    dat->bit++;                                                               \
  }
#define ENDHASH                                                               \
  {                                                                           \
    dat->bit--;                                                               \
    PREFIX fprintf (dat->fh, "},\n");                                         \
  }
#define LASTENDHASH                                                           \
  {                                                                           \
    dat->bit--;                                                               \
    PREFIX fprintf (dat->fh, "}\n");                                          \
  }
#define SECTION(name)                                                         \
  {                                                                           \
    PREFIX fprintf (dat->fh, "\"%s\": [\n", #name);                           \
    dat->bit++;                                                               \
  }

#define ENDSEC() ENDARRAY
#define OLD_NOCOMMA fseek (dat->fh, -2, SEEK_CUR)                                
#define NOCOMMA assert (0 = "NOCOMMA")
// guaranteed non-null str                                                      // --------------原来只有一行      json_cquote (_buf, str, _len, dat->codepage));   \  将两行 json_cquote替换成一行就行 \也要复制
#define PAIR_Sc(name, str)                                                    \
  {                                                                           \
    const size_t len = strlen (str);                                          \
    if (len < 42)                                                             \
      {                                                                       \
        const size_t _len = 6 * len + 1;                                      \
        char _buf[256];                                                       \
        PREFIX fprintf (dat->fh, "\"" #name "\": \"%s\",\n",                  \
                        json_cquote (_buf, str, _len,                         \
                                     (IS_FROM_TU(dat) ? 0 : dat->codepage))); \
      }                                                                       \
    else                                                                      \
      {                                                                       \
        const size_t _len = 6 * len + 1;                                      \
        char *_buf = (char *)malloc (_len);                                   \
        PREFIX fprintf (dat->fh, "\"" #name "\": \"%s\",\n",                  \
                        json_cquote (_buf, str, _len,                         \
                                     (IS_FROM_TU(dat) ? 0 : dat->codepage))); \
        free (_buf);                                                          \
      }                                                                       \
  }
#define PAIR_S(name, str)                                                     \
  if (str)                                                                    \
  PAIR_Sc (name, str)
#define PAIR_D(name, value)                                                   \
  {                                                                           \
    PREFIX fprintf (dat->fh, "\"" #name "\": %d,\n", value);                  \
  }
// guaranteed non-null str
#define LASTPAIR_Sc(name, value)                                              \
  {                                                                           \
    PREFIX fprintf (dat->fh, "\"" #name "\": \"%s\"\n", value);               \
  }
#define LASTPAIR_S(name, value)                                               \
  if (value)                                                                  \
    {                                                                         \
      PREFIX fprintf (dat->fh, "\"" #name "\": \"%s\"\n", value);             \
    }
#define PAIR_NULL(name)                                                       \
  {                                                                           \
    PREFIX fprintf (dat->fh, "\"" #name "\": null,\n");                       \
  }
#define LASTPAIR_NULL(name)                                                   \
  {                                                                           \
    PREFIX fprintf (dat->fh, "\"" #name "\": null\n");                        \
  }
#define KEY(name)                                                             \
  {                                                                           \
    PREFIX fprintf (dat->fh, "\"" #name "\": ");                              \
  }
#define GEOMETRY(name)                                                        \
  {                                                                           \
    KEY (geometry);                                                           \
    SAMEHASH;                                                                 \
    PAIR_S (type, #name)                                                      \
  }
#define ENDGEOMETRY LASTENDHASH

// #define VALUE(value,type,dxf)
//     fprintf(dat->fh, FORMAT_##type, value)
// #define VALUE_RC(value,dxf) VALUE(value, RC, dxf)

#define FIELD(name, type, dxf)
#define _FIELD(name, type, value)
#define ENT_FIELD(name, type, value)
#define FIELD_CAST(name, type, cast, dxf) FIELD (name, cast, dxf)
#define FIELD_TRACE(name, type)
#define FIELD_TEXT(name, str)
#define FIELD_TEXT_TU(name, wstr)

#define FIELD_VALUE(name) _obj->name
#define ANYCODE -1
// todo: only the name, not the ref
#define FIELD_HANDLE(name, handle_code, dxf)
#define FIELD_DATAHANDLE(name, code, dxf)
#define FIELD_HANDLE_N(name, vcount, handle_code, dxf)
#define FIELD_B(name, dxf) FIELD (name, B, dxf)
#define FIELD_BB(name, dxf) FIELD (name, BB, dxf)
#define FIELD_3B(name, dxf) FIELD (name, 3B, dxf)
#define FIELD_BS(name, dxf) FIELD (name, BS, dxf)
#define FIELD_BL(name, dxf) FIELD (name, BL, dxf)
#define FIELD_BLL(name, dxf) FIELD (name, BLL, dxf)
#define FIELD_BD(name, dxf) FIELD (name, BD, dxf)
#define FIELD_RC(name, dxf) FIELD (name, RC, dxf)
#define FIELD_RS(name, dxf) FIELD (name, RS, dxf)
#define FIELD_RD(name, dxf) FIELD_BD (name, dxf)
#define FIELD_RL(name, dxf) FIELD (name, RL, dxf)
#define FIELD_RLL(name, dxf) FIELD (name, RLL, dxf)
#define FIELD_MC(name, dxf) FIELD (name, MC, dxf)
#define FIELD_MS(name, dxf) FIELD (name, MS, dxf)
#define FIELD_TF(name, len, dxf) FIELD_TEXT (name, _obj->name)
#define FIELD_TFF(name, len, dxf) FIELD_TEXT (name, _obj->name)
#define FIELD_TV(name, dxf) FIELD_TEXT (name, _obj->name)
#define FIELD_TU(name, dxf) FIELD_TEXT_TU (name, (BITCODE_TU)_obj->name)
#define FIELD_T(name, dxf)
//  { if (dat->version >= R_2007) { FIELD_TU(name, dxf); }
//    else                        { FIELD_TV(name, dxf); } }
#define FIELD_BT(name, dxf) FIELD (name, BT, dxf)
#define FIELD_4BITS(name, dxf) FIELD (name, 4BITS, dxf)
#define FIELD_BE(name, dxf) FIELD_3RD (name, dxf)
#define FIELD_2DD(name, def, dxf)
#define FIELD_3DD(name, def, dxf)
#define FIELD_2RD(name, dxf)
#define FIELD_2BD(name, dxf)
#define FIELD_2BD_1(name, dxf)
#define FIELD_3RD(name, dxf) ;
#define FIELD_3BD(name, dxf)
#define FIELD_3BD_1(name, dxf)
#define FIELD_DD(name, _default, dxf)

#define _VALUE_RD(value) fprintf (dat->fh, FORMAT_RD, value)
#ifdef IS_RELEASE
#  define VALUE_RD(value)                                                     \
    {                                                                         \
      if (bit_isnan (value))                                                  \
        _VALUE_RD (0.0);                                                      \
      else                                                                    \
        _VALUE_RD (value);                                                    \
    }
#else
#  define VALUE_RD(value) _VALUE_RD (value)
#endif
#define VALUE_2DPOINT(px, py)                                                 \
  {                                                                           \
    PREFIX fprintf (dat->fh, "[ ");                                           \
    VALUE_RD (px);                                                            \
    fprintf (dat->fh, ", ");                                                  \
    VALUE_RD (py);                                                            \
    fprintf (dat->fh, " ],\n");                                               \
  }
#define LASTVALUE_2DPOINT(px, py)                                             \
  {                                                                           \
    PREFIX fprintf (dat->fh, "[ ");                                           \
    VALUE_RD (px);                                                            \
    fprintf (dat->fh, ", ");                                                  \
    VALUE_RD (py);                                                            \
    fprintf (dat->fh, " ]\n");                                                \
  }
#define FIELD_2DPOINT(name) VALUE_2DPOINT (_obj->name.x, _obj->name.y)
#define LASTFIELD_2DPOINT(name) LASTVALUE_2DPOINT (_obj->name.x, _obj->name.y)
#define VALUE_3DPOINT(px, py, pz)                                             \
  {                                                                           \
    PREFIX fprintf (dat->fh, "[ ");                                           \
    VALUE_RD (px);                                                            \
    fprintf (dat->fh, ", ");                                                  \
    VALUE_RD (py);                                                            \
    if (pz != 0.0)                                                            \
      {                                                                       \
        fprintf (dat->fh, ", ");                                              \
        VALUE_RD (pz);                                                        \
      }                                                                       \
    fprintf (dat->fh, " ],\n");                                               \
  }
#define LASTVALUE_3DPOINT(px, py, pz)                                         \
  {                                                                           \
    PREFIX fprintf (dat->fh, "[ ");                                           \
    VALUE_RD (px);                                                            \
    fprintf (dat->fh, ", ");                                                  \
    VALUE_RD (py);                                                            \
    if (pz != 0.0)                                                            \
      {                                                                       \
        fprintf (dat->fh, ", ");                                              \
        VALUE_RD (pz);                                                        \
      }                                                                       \
    fprintf (dat->fh, " ]\n");                                                \
  }
#define FIELD_3DPOINT(name)                                                   \
  {                                                                           \
    if (_obj->name.z != 0.0)                                                  \
      VALUE_3DPOINT (_obj->name.x, _obj->name.y, _obj->name.z)                \
    else                                                                      \
      FIELD_2DPOINT (name)                                                    \
  }
#define LASTFIELD_3DPOINT(name)                                               \
  {                                                                           \
    if (_obj->name.z != 0.0)                                                  \
      LASTVALUE_3DPOINT (_obj->name.x, _obj->name.y, _obj->name.z)            \
    else                                                                      \
      LASTFIELD_2DPOINT (name)                                                \
  }

#define FIELD_CMC(name, dxf1, dxf2)
#define FIELD_TIMEBLL(name, dxf)


// FIELD_VECTOR_N(name, type, size):
// reads data of the type indicated by 'type' 'size' times and stores
// it all in the vector called 'name'.
#define FIELD_VECTOR_N(name, type, size, dxf)                                 \
  ARRAY;                                                                      \
  for (vcount = 0; vcount < (BITCODE_BL)size; vcount++)                       \
    {                                                                         \
      PREFIX fprintf (dat->fh, "\"" #name "\": " FORMAT_##type "%s\n",        \
                      _obj->name[vcount],                                     \
                      vcount == (BITCODE_BL)size - 1 ? "" : ",");             \
    }                                                                         \
  ENDARRAY;

#define FIELD_VECTOR_T(name, type, size, dxf)                                 \
  ARRAY;                                                                      \
  if (!(IS_FROM_TU (dat)))                                                    \
    {                                                                         \
      for (vcount = 0; vcount < (BITCODE_BL)_obj->size; vcount++)             \
        {                                                                     \
          PREFIX fprintf (dat->fh, "\"" #name "\": \"%s\"%s\n",               \
                          _obj->name[vcount],                                 \
                          vcount == (BITCODE_BL)_obj->size - 1 ? "" : ",");   \
        }                                                                     \
    }                                                                         \
  else                                                                        \
    {                                                                         \
      for (vcount = 0; vcount < (BITCODE_BL)_obj->size; vcount++)             \
        FIELD_TEXT_TU (name, _obj->name[vcount]);                             \
    }                                                                         \
  ENDARRAY;

#define FIELD_VECTOR(name, type, size, dxf)                                   \
  FIELD_VECTOR_N (name, type, _obj->size, dxf)

#define FIELD_2RD_VECTOR(name, size, dxf)
#define FIELD_2DD_VECTOR(name, size, dxf)

#define FIELD_3DPOINT_VECTOR(name, size, dxf)                                 \
  ARRAY;                                                                      \
  for (vcount = 0; vcount < (BITCODE_BL)_obj->size; vcount++)                 \
    {                                                                         \
      if (vcount == (BITCODE_BL)_obj->size - 1)                               \
        LASTFIELD_3DPOINT (name[vcount], dxf)                                 \
      else                                                                    \
        FIELD_3DPOINT (name[vcount], dxf)                                     \
    }                                                                         \
  ENDARRAY;

#define WARN_UNSTABLE_CLASS                                                   \
  LOG_WARN ("Unstable Class %s %d %s (0x%x%s) -@%" PRIuSIZE,                  \
            is_entity ? "entity" : "object", klass->number, dxfname,          \
            klass->proxyflag, klass->is_zombie ? "is_zombie" : "",            \
            obj->address + obj->size)



//------------------------------------------------------------------------------new1qt
// 输出缓冲区最大尺寸（10 MB），防止内存溢出
//#define MAX_TEXT_OUTPUT_SIZE   (10 * 1024 * 1024)

//------------------------------------------------------------------------------new2qt
//------------------------------------------------------------------------------new1
static char *
dim_generate_text(Dwg_Object *obj)
{
	// 所有尺寸实体第一个成员都是 DIMENSION_COMMON
	Dwg_DIMENSION_common *dim = (Dwg_DIMENSION_common *)obj->tio.entity->tio.DIMENSION_ORDINATE;
	double value = dim->act_measurement;       // 实际测量值
	const char *prefix = "";
	const char *suffix = "";
	char buf[128];
	int is_angle = 0;

	// 根据标注类型决定格式
	switch (obj->fixedtype) {
	case DWG_TYPE_DIMENSION_ANG3PT:
	case DWG_TYPE_DIMENSION_ANG2LN:
		is_angle = 1;
		value = value * 180.0 / M_PI;          // 弧度转角度
		suffix = "°";
		break;
	case DWG_TYPE_DIMENSION_DIAMETER:
		prefix = "Ø";
		break;
	case DWG_TYPE_DIMENSION_RADIUS:
		prefix = "R";
		break;
	default:
		// 线性、对齐、坐标等，直接使用 value
		break;
	}

	// 检查是否有手动文字（user_text），如果有则直接返回它
	if (dim->user_text && dim->user_text[0]) {
		return strdup((char *)dim->user_text);   // 简单复制，调用者需 free
	}

	// 根据值大小决定小数位数
	if (is_angle)
		snprintf(buf, sizeof(buf), "%s%.4f%s", prefix, value, suffix);
	else if (fabs(value - round(value)) < 1e-6)
		snprintf(buf, sizeof(buf), "%s%.0f%s", prefix, value, suffix);
	else
		snprintf(buf, sizeof(buf), "%s%.4f%s", prefix, value, suffix);

	return strdup(buf);
}
//------------------------------------------------------------------------------new2

// ensure counter-clockwise orientation of a closed polygon. 2d only.
static int
normalize_polygon_orient (BITCODE_BL numpts, dwg_point_2d **const pts_p)
{
  double sum = 0.0;
  dwg_point_2d *pts = *pts_p;
  // check orientation
  for (unsigned i = 0; i < numpts - 1; i++)
    {
      sum += (pts[i + 1].x - pts[i].x) * (pts[i + 1].y + pts[i].y);
    }
  if (sum > 0.0) // if clockwise
    {
      // reverse and return a copy
      unsigned last = numpts - 1;
      dwg_point_2d *newpts
          = (dwg_point_2d *)malloc (numpts * sizeof (BITCODE_2RD));
      // fprintf (stderr, "%u pts, sum %f: reverse orient\n", numpts, sum);
      for (unsigned i = 0; i < numpts; i++)
        {
          newpts[i].x = pts[last - i].x;
          newpts[i].y = pts[last - i].y;
        }
      *pts_p = newpts;
      return 1;
    }
  else
    {
      // fprintf (stderr, "%u pts, sum %f: keep orient\n", numpts, sum);
      return 0;
    }
}

// common properties
#if 0 //------------------------------------------------------------------newline
static void
dwg_geojson_feature (Bit_Chain *restrict dat, Dwg_Object *restrict obj,
                     const char *restrict subclass)
{
  int error;
  char *name;
  char tmp[64];
  PAIR_Sc (type, "Feature");
  snprintf (tmp, sizeof (tmp), FORMAT_HV, obj->handle.value);
  PAIR_Sc (id, tmp);
  KEY (properties);
  SAMEHASH;
  PAIR_S (SubClasses, subclass);

  /*
  if (obj->supertype == DWG_SUPERTYPE_ENTITY)
    {
      Dwg_Object *layer
          = obj->tio.entity->layer ? obj->tio.entity->layer->obj : NULL;
      if (layer
          && (layer->fixedtype == DWG_TYPE_LAYER
              || layer->fixedtype == DWG_TYPE_DICTIONARY))
        {
          name = dwg_obj_table_get_name (layer, &error);
          if (!error)
            {
              PAIR_S (Layer, name);
              if (IS_FROM_TU (dat))
                free (name);
            }
        }*/

	  //--------------------------------------------------------------------new1
  if (obj->supertype == DWG_SUPERTYPE_ENTITY)
  {
	  int layer_error = 1;
	  char *layer_name = NULL;
	  Dwg_Object *layer_obj = obj->tio.entity->layer ? obj->tio.entity->layer->obj : NULL;
	  if (layer_obj &&
		  (layer_obj->fixedtype == DWG_TYPE_LAYER ||
			  layer_obj->fixedtype == DWG_TYPE_DICTIONARY))
	  {
		  layer_name = dwg_obj_table_get_name(layer_obj, &layer_error);
	  }

	  // 如果获取不到层名，强制输出 "0"
	  if (layer_error || !layer_name)
	  {
		  PAIR_S(Layer, "__UNKNOWN__");
	  }
	  else
	  {
		  PAIR_S(Layer, layer_name);
		  if (IS_FROM_TU(dat))
			  free(layer_name);
	  }
	  //--------------------------------------------------------------------new2

      // See #95: index as int or rgb as hexstring
      if (dat->version >= R_2004
          && (obj->tio.entity->color.method == 0xc3     // Truecolor
              || obj->tio.entity->color.method == 0xc2) // Entity
          && obj->tio.entity->color.index == 256)
        {
          snprintf (tmp, sizeof (tmp), "#%06X",
                    obj->tio.entity->color.rgb & 0xffffff);
          PAIR_Sc (Color, tmp);
        }
      else if ((obj->tio.entity->color.index != 256)
               || (dat->version >= R_2004
                   && obj->tio.entity->color.method != 0xc0 // ByLayer
                   && obj->tio.entity->color.method != 0xc1 // ByBlock
                   && obj->tio.entity->color.method != 0xc8 // none
                   ))
        {
          // no names for the first palette entries yet.
          PAIR_D (Color, obj->tio.entity->color.index);
        }

      name = dwg_ent_get_ltype_name (obj->tio.entity, &error);
      if (!error && strNE (name, "ByLayer")) // skip the default
        {
          PAIR_S (Linetype, name);
          if (IS_FROM_TU (dat))
            free (name);
        }
    }

  // if has notes and opt. an mtext frame_text
  if (obj->fixedtype == DWG_TYPE_GEOPOSITIONMARKER)
    {
      Dwg_Entity_GEOPOSITIONMARKER *_obj
          = obj->tio.entity->tio.GEOPOSITIONMARKER;
      if (IS_FROM_TU (dat))
        {
          char *utf8 = bit_convert_TU ((BITCODE_TU)_obj->notes);
          PAIR_S (Text, utf8)
          free (utf8);
        }
      else
        {
          PAIR_S (Text, _obj->notes)
        }
    }


  /*
  else if (obj->fixedtype == DWG_TYPE_TEXT)
    {
      Dwg_Entity_TEXT *_obj = obj->tio.entity->tio.TEXT;


      if (IS_FROM_TU (dat))
        {
          char *utf8 = bit_convert_TU ((BITCODE_TU)_obj->text_value);
          PAIR_S (Text, utf8)
          free (utf8);
        }
      else
        {
          PAIR_S (Text, _obj->text_value)
        }
    }
  else if (obj->fixedtype == DWG_TYPE_MTEXT)
    {
      Dwg_Entity_MTEXT *_obj = obj->tio.entity->tio.MTEXT;
      if (IS_FROM_TU (dat))
        {
          char *utf8 = bit_convert_TU ((BITCODE_TU)_obj->text);
          PAIR_S (Text, utf8)
          free (utf8);
        }
      else
        {
          PAIR_S (Text, _obj->text)
        }
    }*/

  //---------------------------------------------------------------new1
  else if (obj->fixedtype == DWG_TYPE_TEXT)
  {
	  Dwg_Entity_TEXT *_obj = obj->tio.entity->tio.TEXT;
	  if (IS_FROM_TU(dat))
	  {
		  char *utf8 = bit_convert_TU((BITCODE_TU)_obj->text_value);
		  if (utf8 && strlen(utf8) < 500) {
			  PAIR_S(Text, utf8);
		  }
		  else {
			  PAIR_S(Text, "");
		  }
		  free(utf8);
	  }
	  else
	  {
		  if (_obj->text_value && strlen((const char*)_obj->text_value) < 500) {
			  PAIR_S(Text, _obj->text_value);
		  }
		  else {
			  PAIR_S(Text, "");
		  }
	  }
  }
  else if (obj->fixedtype == DWG_TYPE_MTEXT)
  {
	  Dwg_Entity_MTEXT *_obj = obj->tio.entity->tio.MTEXT;
	  if (IS_FROM_TU(dat))
	  {
		  char *utf8 = bit_convert_TU((BITCODE_TU)_obj->text);
		  if (utf8 && strlen(utf8) < 500) {
			  PAIR_S(Text, utf8);
		  }
		  else {
			  PAIR_S(Text, "");
		  }
		  free(utf8);
	  }
	  else
	  {
		  if (_obj->text && strlen((const char*)_obj->text) < 500) {
			  PAIR_S(Text, _obj->text);
		  }
		  else {
			  PAIR_S(Text, "");
		  }
	  }
  }

  else if (obj->fixedtype == DWG_TYPE_ATTRIB)
  {
	  Dwg_Entity_ATTRIB *_obj = obj->tio.entity->tio.ATTRIB;
	  if (IS_FROM_TU(dat))
	  {
		  char *utf8 = bit_convert_TU((BITCODE_TU)_obj->text_value);
		  if (utf8 && strlen(utf8) < 500) {
			  PAIR_S(Text, utf8);
		  }
		  else {
			  PAIR_S(Text, "");
		  }
		  free(utf8);
	  }
	  else
	  {
		  if (_obj->text_value && strlen((const char*)_obj->text_value) < 500) {
			  PAIR_S(Text, _obj->text_value);
		  }
		  else {
			  PAIR_S(Text, "");
		  }
	  }
  }

  else if (obj->fixedtype == DWG_TYPE_INSERT)
  {
	  Dwg_Entity_INSERT *_obj = obj->tio.entity->tio.INSERT;
	  Dwg_Object *hdr = dwg_ref_get_object(_obj->block_header, &error);
	  if (!error && hdr && hdr->fixedtype == DWG_TYPE_BLOCK_HEADER)
	  {
		  Dwg_Object_BLOCK_HEADER *_hdr = hdr->tio.object->tio.BLOCK_HEADER;
		  char *text;
		  if (IS_FROM_TU(dat))
			  text = bit_convert_TU((BITCODE_TU)_hdr->name);
		  else
			  text = _hdr->name;
		  if (text)
		  {
			  PAIR_S(name, text);
			  if (IS_FROM_TU(dat))
				  free(text);
		  }
	  }
  }
  else if (obj->fixedtype == DWG_TYPE_MINSERT)   // 新增
  {
	  Dwg_Entity_MINSERT *_obj = obj->tio.entity->tio.MINSERT;
	  Dwg_Object *hdr = dwg_ref_get_object(_obj->block_header, &error);
	  if (!error && hdr && hdr->fixedtype == DWG_TYPE_BLOCK_HEADER)
	  {
		  Dwg_Object_BLOCK_HEADER *_hdr = hdr->tio.object->tio.BLOCK_HEADER;
		  char *text;
		  if (IS_FROM_TU(dat))
			  text = bit_convert_TU((BITCODE_TU)_hdr->name);
		  else
			  text = _hdr->name;
		  if (text)
		  {
			  PAIR_S(name, text);
			  if (IS_FROM_TU(dat))
				  free(text);
		  }
	  }
  }

  else if (obj->fixedtype == DWG_TYPE_ATTDEF)
{
Dwg_Entity_ATTDEF *_obj = obj->tio.entity->tio.ATTDEF;

if (IS_FROM_TU(dat))
{
	char *tag_utf8 = bit_convert_TU((BITCODE_TU)_obj->tag);
	char *text_utf8 = bit_convert_TU((BITCODE_TU)_obj->default_value);
	PAIR_S(Tag, tag_utf8);
	if (text_utf8 && strlen(text_utf8) < 500) {
		PAIR_S(Text, text_utf8);
	}
	else {
		PAIR_S(Text, "");
	}
	free(tag_utf8);
	free(text_utf8);
}
else
{
	PAIR_S(Tag, _obj->tag);
	if (_obj->default_value && strlen((const char*)_obj->default_value) < 500) {
		PAIR_S(Text, _obj->default_value);
	}
	else {
		PAIR_S(Text, "");
	}
}
}

  //-------------------------------------------------------------------new2

  /*
  else if (obj->fixedtype == DWG_TYPE_INSERT)
    {
      Dwg_Entity_INSERT *_obj = obj->tio.entity->tio.INSERT;
      Dwg_Object *hdr = dwg_ref_get_object (_obj->block_header, &error);
      if (!error && hdr && hdr->fixedtype == DWG_TYPE_BLOCK_HEADER)
        {
          Dwg_Object_BLOCK_HEADER *_hdr = hdr->tio.object->tio.BLOCK_HEADER;
          char *text;
          if (IS_FROM_TU (dat))
            text = bit_convert_TU ((BITCODE_TU)_hdr->name);
          else
            text = _hdr->name;
          if (text)
            {
              PAIR_S (name, text);
              if (IS_FROM_TU (dat))
                free (text);
            }
        }
    }
  
  else if (obj->fixedtype == DWG_TYPE_MINSERT)
    {
      Dwg_Entity_MINSERT *_obj = obj->tio.entity->tio.MINSERT;
      Dwg_Object *hdr = dwg_ref_get_object (_obj->block_header, &error);
      if (!error && hdr && hdr->fixedtype == DWG_TYPE_BLOCK_HEADER)
        {
          Dwg_Object_BLOCK_HEADER *_hdr = hdr->tio.object->tio.BLOCK_HEADER;
          char *text;
          if (IS_FROM_TU (dat))
            text = bit_convert_TU ((BITCODE_TU)_hdr->name);
          else
            text = _hdr->name;
          if (text)
            {
              PAIR_S (name, text);
              if (IS_FROM_TU (dat))
                free (text);
            }
        }
    }*/

   //PAIR_NULL(ExtendedEntity);
  snprintf (tmp, sizeof (tmp), FORMAT_HV, obj->handle.value);
  LASTPAIR_Sc (EntityHandle, tmp);
  ENDHASH;
}
#endif  //---------------------------------------newline
static void
dwg_geojson_feature(Bit_Chain *restrict dat, Dwg_Object *restrict obj,
	const char *restrict subclass)
{
}
#if 0
//------------------------------------------------------------------------------new1   dwg_geojson_feature
static void
dwg_geojson_feature(Bit_Chain *restrict dat, Dwg_Object *restrict obj,
	const char *restrict subclass)
{
	int error;
	char tmp[64];
	PAIR_Sc(type, "Feature");
	snprintf(tmp, sizeof(tmp), FORMAT_HV, obj->handle.value);
	PAIR_Sc(id, tmp);
	KEY(properties);
	SAMEHASH;

	/* 输出图层（强制输出，获取不到时回退为 "__UNKNOWN__"） */
	if (obj->supertype == DWG_SUPERTYPE_ENTITY)
	{
		int layer_error = 1;
		char *layer_name = NULL;
		Dwg_Object *layer_obj = obj->tio.entity->layer ? obj->tio.entity->layer->obj : NULL;
		if (layer_obj &&
			(layer_obj->fixedtype == DWG_TYPE_LAYER ||
				layer_obj->fixedtype == DWG_TYPE_DICTIONARY))
		{
			layer_name = dwg_obj_table_get_name(layer_obj, &layer_error);
		}

		if (layer_error || !layer_name)
			PAIR_S(Layer, "__UNKNOWN__")
		else
		{
			PAIR_S(Layer, layer_name);
			if (IS_FROM_TU(dat))
				free(layer_name);
		}
	}
	else
	{
		PAIR_S(Layer, "__UNKNOWN__");
	}

	/* 输出文字（仅当实体有文本内容时） */
	if (obj->fixedtype == DWG_TYPE_GEOPOSITIONMARKER)
	{
		Dwg_Entity_GEOPOSITIONMARKER *_obj = obj->tio.entity->tio.GEOPOSITIONMARKER;
		if (IS_FROM_TU(dat))
		{
			char *utf8 = bit_convert_TU((BITCODE_TU)_obj->notes);
			if (utf8 && strlen(utf8) < 500)
				PAIR_S(Text, utf8)
			else
				PAIR_S(Text, "");
			free(utf8);
		}
		else
		{
			if (_obj->notes && strlen((const char*)_obj->notes) < 500)
				PAIR_S(Text, _obj->notes)
			else
				PAIR_S(Text, "");
		}
	}
	else if (obj->fixedtype == DWG_TYPE_TEXT)
	{
		Dwg_Entity_TEXT *_obj = obj->tio.entity->tio.TEXT;
		if (IS_FROM_TU(dat))
		{
			char *utf8 = bit_convert_TU((BITCODE_TU)_obj->text_value);
			if (utf8 && strlen(utf8) < 500)
				PAIR_S(Text, utf8)
			else
				PAIR_S(Text, "");
			free(utf8);
		}
		else
		{
			if (_obj->text_value && strlen((const char*)_obj->text_value) < 500)
				PAIR_S(Text, _obj->text_value)
			else
				PAIR_S(Text, "");
		}
	}
	else if (obj->fixedtype == DWG_TYPE_MTEXT)
	{
		Dwg_Entity_MTEXT *_obj = obj->tio.entity->tio.MTEXT;
		if (IS_FROM_TU(dat))
		{
			char *utf8 = bit_convert_TU((BITCODE_TU)_obj->text);
			if (utf8 && strlen(utf8) < 500)
				PAIR_S(Text, utf8)
			else
				PAIR_S(Text, "");
			free(utf8);
		}
		else
		{
			if (_obj->text && strlen((const char*)_obj->text) < 500)
				PAIR_S(Text, _obj->text)
			else
				PAIR_S(Text, "");
		}
	}
	else if (obj->fixedtype == DWG_TYPE_ATTRIB)
	{
		Dwg_Entity_ATTRIB *_obj = obj->tio.entity->tio.ATTRIB;
		if (IS_FROM_TU(dat))
		{
			char *utf8 = bit_convert_TU((BITCODE_TU)_obj->text_value);
			if (utf8 && strlen(utf8) < 500)
				PAIR_S(Text, utf8)
			else
				PAIR_S(Text, "");
			free(utf8);
		}
		else
		{
			if (_obj->text_value && strlen((const char*)_obj->text_value) < 500)
				PAIR_S(Text, _obj->text_value)
			else
				PAIR_S(Text, "");
		}
	}
	else if (obj->fixedtype == DWG_TYPE_ATTDEF)
	{
		Dwg_Entity_ATTDEF *_obj = obj->tio.entity->tio.ATTDEF;
		if (IS_FROM_TU(dat))
		{
			char *utf8 = bit_convert_TU((BITCODE_TU)_obj->default_value);
			if (utf8 && strlen(utf8) < 500)
				PAIR_S(Text, utf8)
			else
				PAIR_S(Text, "");
			free(utf8);
		}
		else
		{
			if (_obj->default_value && strlen((const char*)_obj->default_value) < 500)
				PAIR_S(Text, _obj->default_value)
			else
				PAIR_S(Text, "");
		}
	}

	ENDHASH; /* properties */
}
//-------------------------------------------------------------------------new2
#endif


#define FEATURE(subclass, obj)                                                \
  HASH;                                                                       \
  dwg_geojson_feature (dat, obj, #subclass)
#define ENDFEATURE                                                            \
  if (is_last)                                                                \
    LASTENDHASH                                                               \
  else                                                                        \
    ENDHASH

static int
dwg_geojson_LWPOLYLINE (Bit_Chain *restrict dat, Dwg_Object *restrict obj,
                        int is_last)
{
  BITCODE_BL j, last_j;
  Dwg_Entity_LWPOLYLINE *_obj = obj->tio.entity->tio.LWPOLYLINE;
  dwg_point_2d *pts = (dwg_point_2d *)_obj->points;
  if (!_obj->points)
    return 1;

  FEATURE (AcDbEntity : AcDbLwPolyline, obj);
  // TODO bulges, splines, ...

  // if closed and num_points > 3 use a Polygon
  if (_obj->flag & 512 && _obj->num_points > 3)
    {
      int changed
          = normalize_polygon_orient (_obj->num_points, &pts); // RFC7946
      GEOMETRY (Polygon)
      KEY (coordinates);
      ARRAY;
      ARRAY;
      for (j = 0; j < _obj->num_points; j++)
        VALUE_2DPOINT (pts[j].x, pts[j].y)
      LASTVALUE_2DPOINT (pts[0].x, pts[0].y);
      LASTENDARRAY;
      LASTENDARRAY;
      if (changed)
        free (pts);
    }
  else
    {
      GEOMETRY (LineString)
      KEY (coordinates);
      ARRAY;
      last_j = _obj->num_points - 1;
      for (j = 0; j < last_j; j++)
        VALUE_2DPOINT (pts[j].x, pts[j].y);
      LASTVALUE_2DPOINT (pts[last_j].x, pts[last_j].y);
      LASTENDARRAY;
    }
  ENDGEOMETRY;
  ENDFEATURE;
  return 1;
}

/* returns 0 if object could be printed
 */
static int
dwg_geojson_variable_type (Dwg_Data *restrict dwg, Bit_Chain *restrict dat,
                           Dwg_Object *restrict obj, int is_last)
{
  int i;
  char *dxfname;
  Dwg_Class *klass;
  int is_entity;

  i = obj->fixedtype - 500;
  if (i < 0 || i >= (int)dwg->num_classes)
    return 0;
  if (obj->fixedtype == DWG_TYPE_UNKNOWN_ENT
      || obj->fixedtype == DWG_TYPE_UNKNOWN_OBJ)
    return DWG_ERR_UNHANDLEDCLASS;

  klass = &dwg->dwg_class[i];
  if (!klass || !klass->dxfname)
    return DWG_ERR_INTERNALERROR;
  dxfname = klass->dxfname;
  // almost always false
  is_entity = dwg_class_is_entity (klass);

  if (strEQc (dxfname, "LWPOLYLINE"))
    {
      return dwg_geojson_LWPOLYLINE (dat, obj, is_last);
    }
  /*
  if (strEQc (dxfname, "GEODATA"))
    {
      Dwg_Object_GEODATA *_obj = obj->tio.object->tio.GEODATA;
      WARN_UNSTABLE_CLASS;
      FEATURE (AcDbObject : AcDbGeoData, obj);
      // which fields? transformation for the world-coordinates?
      // crs links of type proj4, ogcwkt, esriwkt or such?
      ENDFEATURE;
      return 0;
    }
  */
  if (strEQc (dxfname, "GEOPOSITIONMARKER"))
    {
      Dwg_Entity_GEOPOSITIONMARKER *_obj
          = obj->tio.entity->tio.GEOPOSITIONMARKER;
      WARN_UNSTABLE_CLASS;
      // now even with text
      FEATURE (AcDbEntity : AcDbGeoPositionMarker, obj);
      GEOMETRY (Point);
      KEY (coordinates);
      if (fabs (_obj->position.z) > 0.000001)
        VALUE_3DPOINT (_obj->position.x, _obj->position.y, _obj->position.z)
      else
        VALUE_2DPOINT (_obj->position.x, _obj->position.y);
      ENDGEOMETRY;
      ENDFEATURE;
      return 1;
    }

  return 0;
}
#if 0    //----------------------------------------------------newline
static int
dwg_geojson_object (Bit_Chain *restrict dat, Dwg_Object *restrict obj,
                    int is_last)
{

  switch (obj->fixedtype)
    {
	  /*
    case DWG_TYPE_INSERT: // Just the insertion point yet
      {
        Dwg_Entity_INSERT *_obj = obj->tio.entity->tio.INSERT;
        FEATURE (AcDbEntity : AcDbBlockReference, obj);
        // TODO: explode insert into a GeometryCollection
        GEOMETRY (Point);
        KEY (coordinates);
        LASTFIELD_3DPOINT (ins_pt);
        ENDGEOMETRY;
        ENDFEATURE;


        return 1;
      }*/
	  /*                       //-------------------------------newline
    case DWG_TYPE_MINSERT:
      // a grid of INSERT's (named points)
      // dwg_geojson_MINSERT(dat, obj);
      LOG_TRACE ("MINSERT not yet supported");
      break;
      */					    //-------------------------------newline
    case DWG_TYPE_POLYLINE_2D:
      {
        int error;
        BITCODE_BL j, numpts;
        // bool is_polygon = false;
        int changed = 0;
        dwg_point_2d *pts, *orig;
        Dwg_Entity_POLYLINE_2D *_obj = obj->tio.entity->tio.POLYLINE_2D;
        numpts = dwg_object_polyline_2d_get_numpoints (obj, &error);
        if (error || !numpts)
          return 0;
        pts = dwg_object_polyline_2d_get_points (obj, &error);
        if (error || !pts)
          return 0;
        // TODO bulges needs explosion into lines. divided by polyline curve
        // smoothness (default 8)

        // if closed and num_points > 3 use a Polygon
        FEATURE (AcDbEntity : AcDbPolyline, obj);
        if (_obj->flag & 512 && numpts > 3)
          {
            orig = pts; // pts is already a new copy
            changed = normalize_polygon_orient (numpts, &pts); // RFC7946
            if (changed)
              free (orig);
            GEOMETRY (Polygon)
            KEY (coordinates);
            ARRAY;
            ARRAY;
            for (j = 0; j < numpts; j++)
              VALUE_2DPOINT (pts[j].x, pts[j].y)
            LASTVALUE_2DPOINT (pts[0].x, pts[0].y);
            LASTENDARRAY;
            LASTENDARRAY;
            if (changed)
              free (pts);
          }
        else
          {
            GEOMETRY (LineString)
            KEY (coordinates);
            ARRAY;
            for (j = 0; j < numpts; j++)
              {
                if (j == numpts - 1)
                  LASTVALUE_2DPOINT (pts[j].x, pts[j].y)
                else
                  VALUE_2DPOINT (pts[j].x, pts[j].y);
              }
            free (pts);
            LASTENDARRAY;
          }
        ENDGEOMETRY;
        ENDFEATURE;
        return 1;
      }
    case DWG_TYPE_POLYLINE_3D:
      {
        int error;
        BITCODE_BL j, numpts;
        dwg_point_3d *pts;
        numpts = dwg_object_polyline_3d_get_numpoints (obj, &error);
        if (error || !numpts)
          return 0;
        pts = dwg_object_polyline_3d_get_points (obj, &error);
        if (error || !pts)
          return 0;
        FEATURE (AcDbEntity : AcDbPolyline, obj);
        GEOMETRY (LineString);
        KEY (coordinates);
        ARRAY;
        for (j = 0; j < numpts; j++)
          {
            if (j == numpts - 1)
              {
                LASTVALUE_3DPOINT (pts[j].x, pts[j].y, pts[j].z);
              }
            else
              {
                VALUE_3DPOINT (pts[j].x, pts[j].y, pts[j].z);
              }
          }
        free (pts);
        LASTENDARRAY;
        ENDGEOMETRY;
        ENDFEATURE;
        return 1;
      }
    case DWG_TYPE_ARC:
      // dwg_geojson_ARC(dat, obj);
      if (1)
        {
          Dwg_Entity_ARC *_obj = obj->tio.entity->tio.ARC;
          const int viewres = 1000;
          BITCODE_2BD ctr = { _obj->center.x, _obj->center.y };
          BITCODE_2BD *pts;
          int num_pts;
          double end_angle = _obj->end_angle;
          // viewres is for 2PI. we need anglediff(deg)/2PI
          while (end_angle - _obj->start_angle < 1e-6)
            end_angle += M_PI;
          num_pts
              = (int)trunc (viewres / rad2deg (end_angle - _obj->start_angle));
          if (num_pts > 10000 || num_pts < 0)
            {
              LOG_ERROR ("Invalid angles");
              return DWG_ERR_VALUEOUTOFBOUNDS;
            }
          num_pts = MIN (num_pts, 120);
          pts = (BITCODE_2BD *)malloc (num_pts * sizeof (BITCODE_2BD));
          if (!pts)
            {
              LOG_ERROR ("Out of memory");
              return DWG_ERR_OUTOFMEM;
            }
          // explode into line segments. divided by VIEWRES (default 1000)
          arc_split (pts, num_pts, ctr, _obj->start_angle, _obj->end_angle,
                     _obj->radius);
          FEATURE (AcDbEntity : AcDbArc, obj);
          GEOMETRY (Polygon)
          KEY (coordinates);
          ARRAY;
          ARRAY;
          for (int j = 0; j < num_pts; j++)
            {
              VALUE_2DPOINT (pts[j].x, pts[j].y)
            }
          LASTVALUE_2DPOINT (pts[0].x, pts[0].y);
          LASTENDARRAY;
          LASTENDARRAY;
          ENDGEOMETRY;
          ENDFEATURE;
          free (pts);
        }
      else
        LOG_TRACE ("ARC not yet supported");
      break;
    case DWG_TYPE_CIRCLE:
      // dwg_geojson_CIRCLE(dat, obj);
      if (1)
        {
          Dwg_Entity_CIRCLE *_obj = obj->tio.entity->tio.CIRCLE;
          // const int viewres = 1000; //dwg->header_vars.VIEWRES;
          BITCODE_2BD ctr = { _obj->center.x, _obj->center.y };
          // double res = viewres / 360.0;
          int num_pts = 120;
          BITCODE_2BD *pts
              = (BITCODE_2BD *)malloc (num_pts * sizeof (BITCODE_2BD));
          arc_split (pts, num_pts, ctr, 0, M_PI * 2.0, _obj->radius);
          FEATURE (AcDbEntity : AcDbCircle, obj);
          GEOMETRY (Polygon)
          KEY (coordinates);
          ARRAY;
          ARRAY;
          for (int j = 0; j < num_pts; j++)
            {
              VALUE_2DPOINT (pts[j].x, pts[j].y)
            }
          LASTVALUE_2DPOINT (pts[0].x, pts[0].y);
          LASTENDARRAY;
          LASTENDARRAY;
          ENDGEOMETRY;
          ENDFEATURE;
          free (pts);
        }
      else
        LOG_TRACE ("CIRCLE not yet supported");
      break;
    case DWG_TYPE_LINE:
      {
        Dwg_Entity_LINE *_obj = obj->tio.entity->tio.LINE;
        FEATURE (AcDbEntity : AcDbLine, obj);
        GEOMETRY (LineString);
        KEY (coordinates);
        ARRAY;
        FIELD_3DPOINT (start);
        LASTFIELD_3DPOINT (end);
        LASTENDARRAY;
        ENDGEOMETRY;
        ENDFEATURE;
        return 1;
      }
    case DWG_TYPE_POINT:
      {
        Dwg_Entity_POINT *_obj = obj->tio.entity->tio.POINT;
        FEATURE (AcDbEntity : AcDbPoint, obj);
        GEOMETRY (Point);
        KEY (coordinates);
        if (fabs (_obj->z) > 0.000001)
          {
            LASTVALUE_3DPOINT (_obj->x, _obj->y, _obj->z);
          }
        else
          {
            LASTVALUE_2DPOINT (_obj->x, _obj->y);
          }
        ENDGEOMETRY;
        ENDFEATURE;
        return 1;
      }
    case DWG_TYPE__3DFACE:
      // really a Polygon
      // dwg_geojson__3DFACE(dat, obj);
      LOG_TRACE ("3DFACE not yet supported");
      break;
    case DWG_TYPE_POLYLINE_PFACE:
      // dwg_geojson_POLYLINE_PFACE(dat, obj);
      LOG_TRACE ("POLYLINE_PFACE not yet supported");
      break;
    case DWG_TYPE_POLYLINE_MESH:
      // dwg_geojson_POLYLINE_MESH(dat, obj);
      LOG_TRACE ("POLYLINE_MESH not yet supported");
      break;
    case DWG_TYPE_SOLID:
      // dwg_geojson_SOLID(dat, obj);
      LOG_TRACE ("SOLID not yet supported");
      break;
    case DWG_TYPE_TRACE:
      // dwg_geojson_TRACE(dat, obj);
      LOG_TRACE ("TRACE not yet supported");
      break;
    case DWG_TYPE_ELLIPSE:
      // dwg_geojson_ELLIPSE(dat, obj);
      LOG_TRACE ("ELLIPSE not yet supported");
      break;
    case DWG_TYPE_SPLINE:
      // dwg_geojson_SPLINE(dat, obj);
      LOG_TRACE ("SPLINE not yet supported");
      break;
    case DWG_TYPE_HATCH:
      // dwg_geojson_HATCH(dat, obj);
      break;
    case DWG_TYPE__3DSOLID:
      // dwg_geojson__3DSOLID(dat, obj);
      break;
    case DWG_TYPE_REGION:
      // dwg_geojson_REGION(dat, obj);
      break;
    case DWG_TYPE_BODY:
      // dwg_geojson_BODY(dat, obj);
      break;
    case DWG_TYPE_RAY:
      // dwg_geojson_RAY(dat, obj);
      LOG_TRACE ("RAY not yet supported");
      break;
    case DWG_TYPE_XLINE:
      // dwg_geojson_XLINE(dat, obj);
      LOG_TRACE ("XLINE not yet supported");
      break;
	  /*
    case DWG_TYPE_TEXT:
      {
		
        // add Text property to a point
        Dwg_Entity_TEXT *_obj = obj->tio.entity->tio.TEXT;

        FEATURE (AcDbEntity : AcDbText, obj);
        GEOMETRY (Point);
        KEY (coordinates);
        LASTFIELD_2DPOINT (ins_pt);
        ENDGEOMETRY;
	
        ENDFEATURE;
        return 1;
      }
	  
    case DWG_TYPE_MTEXT:
      {
        // add Text property to a point
        Dwg_Entity_MTEXT *_obj = obj->tio.entity->tio.MTEXT;
	
        FEATURE (AcDbEntity : AcDbMText, obj);
        GEOMETRY (Point);
        KEY (coordinates);
        LASTFIELD_3DPOINT (ins_pt);
        ENDGEOMETRY;
	
        ENDFEATURE;
        return 1;
      }*/
	  //----------------------------------------------------------------new1
	case DWG_TYPE_TEXT:
	{

		// add Text property to a point
		Dwg_Entity_TEXT *_obj = obj->tio.entity->tio.TEXT;

		FEATURE(AcDbEntity : AcDbText, obj);
		GEOMETRY(Point);
		KEY(coordinates);
		LASTFIELD_2DPOINT(ins_pt);
		ENDGEOMETRY;

		ENDFEATURE;
		return 1;
	}

	case DWG_TYPE_MTEXT:
	{
		// add Text property to a point
		Dwg_Entity_MTEXT *_obj = obj->tio.entity->tio.MTEXT;

		FEATURE(AcDbEntity : AcDbMText, obj);
		GEOMETRY(Point);
		KEY(coordinates);
		LASTFIELD_3DPOINT(ins_pt);
		ENDGEOMETRY;

		ENDFEATURE;
		return 1;
	}

	case DWG_TYPE_INSERT: // Just the insertion point yet
	{
		Dwg_Entity_INSERT *_obj = obj->tio.entity->tio.INSERT;
		FEATURE(AcDbEntity : AcDbBlockReference, obj);
		// TODO: explode insert into a GeometryCollection
		GEOMETRY(Point);
		KEY(coordinates);
		LASTFIELD_3DPOINT(ins_pt);
		ENDGEOMETRY;
		ENDFEATURE;

		return 1;
	}

	case DWG_TYPE_ATTRIB:
	{
		Dwg_Entity_ATTRIB *_obj = obj->tio.entity->tio.ATTRIB;
		FEATURE(AcDbEntity : AcDbAttribute, obj);
		GEOMETRY(Point);
		KEY(coordinates);
		LASTFIELD_2DPOINT(ins_pt);
		ENDGEOMETRY;
		ENDFEATURE;
		return 1;
	}

	case DWG_TYPE_MINSERT:
	{
		Dwg_Entity_MINSERT *_obj = obj->tio.entity->tio.MINSERT;
		FEATURE(AcDbEntity : AcDbMInsert, obj);
		GEOMETRY(Point);
		KEY(coordinates);
		LASTFIELD_3DPOINT(ins_pt);
		ENDGEOMETRY;
		ENDFEATURE;
		return 1;
	}

	case DWG_TYPE_ATTDEF:
	{
		Dwg_Entity_ATTDEF *_obj = obj->tio.entity->tio.ATTDEF;
		FEATURE(AcDbEntity : AcDbAttributeDefinition, obj);
		GEOMETRY(Point);
		KEY(coordinates);
		LASTFIELD_2DPOINT(ins_pt);
		ENDGEOMETRY;
		ENDFEATURE;
		return 1;
	}

	case DWG_TYPE_DIMENSION_ORDINATE:
	{
		Dwg_Entity_DIMENSION_ORDINATE *_obj = obj->tio.entity->tio.DIMENSION_ORDINATE;
		FEATURE(AcDbEntity : AcDbDimension, obj);
		GEOMETRY(Point);
		KEY(coordinates);
		LASTFIELD_2DPOINT(text_midpt);
		ENDGEOMETRY;
		ENDFEATURE;
		return 1;
	}
	case DWG_TYPE_DIMENSION_LINEAR:
	{
		Dwg_Entity_DIMENSION_LINEAR *_obj = obj->tio.entity->tio.DIMENSION_LINEAR;
		FEATURE(AcDbEntity : AcDbDimension, obj);
		GEOMETRY(Point);
		KEY(coordinates);
		LASTFIELD_2DPOINT(text_midpt);
		ENDGEOMETRY;
		ENDFEATURE;
		return 1;
	}
	case DWG_TYPE_DIMENSION_ALIGNED:
	{
		Dwg_Entity_DIMENSION_ALIGNED *_obj = obj->tio.entity->tio.DIMENSION_ALIGNED;
		FEATURE(AcDbEntity : AcDbDimension, obj);
		GEOMETRY(Point);
		KEY(coordinates);
		LASTFIELD_2DPOINT(text_midpt);
		ENDGEOMETRY;
		ENDFEATURE;
		return 1;
	}
	case DWG_TYPE_DIMENSION_ANG3PT:
	{
		Dwg_Entity_DIMENSION_ANG3PT *_obj = obj->tio.entity->tio.DIMENSION_ANG3PT;
		FEATURE(AcDbEntity : AcDbDimension, obj);
		GEOMETRY(Point);
		KEY(coordinates);
		LASTFIELD_2DPOINT(text_midpt);
		ENDGEOMETRY;
		ENDFEATURE;
		return 1;
	}
	case DWG_TYPE_DIMENSION_ANG2LN:
	{
		Dwg_Entity_DIMENSION_ANG2LN *_obj = obj->tio.entity->tio.DIMENSION_ANG2LN;
		FEATURE(AcDbEntity : AcDbDimension, obj);
		GEOMETRY(Point);
		KEY(coordinates);
		LASTFIELD_2DPOINT(text_midpt);
		ENDGEOMETRY;
		ENDFEATURE;
		return 1;
	}
	case DWG_TYPE_DIMENSION_RADIUS:
	{
		Dwg_Entity_DIMENSION_RADIUS *_obj = obj->tio.entity->tio.DIMENSION_RADIUS;
		FEATURE(AcDbEntity : AcDbDimension, obj);
		GEOMETRY(Point);
		KEY(coordinates);
		LASTFIELD_2DPOINT(text_midpt);
		ENDGEOMETRY;
		ENDFEATURE;
		return 1;
	}
	case DWG_TYPE_DIMENSION_DIAMETER:
	{
		Dwg_Entity_DIMENSION_DIAMETER *_obj = obj->tio.entity->tio.DIMENSION_DIAMETER;
		FEATURE(AcDbEntity : AcDbDimension, obj);
		GEOMETRY(Point);
		KEY(coordinates);
		LASTFIELD_2DPOINT(text_midpt);
		ENDGEOMETRY;
		ENDFEATURE;
		return 1;
	}

	//---------------------------------------------------------------------new2
	
    case DWG_TYPE_MLINE:
      // dwg_geojson_MLINE(dat, obj);
      LOG_TRACE ("MLINE not yet supported");
      break;
    case DWG_TYPE_LWPOLYLINE:
      return dwg_geojson_LWPOLYLINE (dat, obj, is_last);
	 
    default:
      if (obj->parent && dat->version > R_12
          && obj->fixedtype != obj->parent->layout_type)
        return dwg_geojson_variable_type (obj->parent, dat, obj, is_last);
    }

    return 0;
}
#endif   //-------------------------------------------newline
/*
static int
geojson_entities_write (Bit_Chain *restrict dat, Dwg_Data *restrict dwg)
{

  BITCODE_BL i;
  int success;
  SECTION (features);
  for (i = 0; i < dwg->num_objects; i++)
    {

      int is_last = i == dwg->num_objects - 1;
      Dwg_Object *obj = &dwg->object[i];

      success = dwg_geojson_object (dat, obj, is_last);
      if (is_last && !success) // needed for the LASTFEATURE comma. end with an
                               // empty dummy
        {
          HASH PAIR_Sc (type, "Feature");
          PAIR_NULL (properties);
          LASTPAIR_NULL (geometry);
          LASTENDHASH;
        }
    }

  ENDSEC (); // because afterwards is always the final geocoding object
  return 0;
}*/
#if 0  //--------------------------------------------newline
//----------------------------------------------------------------------------new1
// 将块内局部坐标 (x,y) 变换到模型空间

static void
transform_point_2d(double *x, double *y,
	double ins_x, double ins_y,
	double scale_x, double scale_y,
	double rotation)
{
	double tx = *x * scale_x;
	double ty = *y * scale_y;
	double c = cos(rotation);
	double s = sin(rotation);
	*x = tx * c - ty * s + ins_x;
	*y = tx * s + ty * c + ins_y;
}

// 直接向 dat 写入一个文本 Feature（简化版，暂不输出颜色）
static int
output_text_feature(Bit_Chain *restrict dat, Dwg_Object *ent, double x, double y)
{
	char tmp[64];
	char *text_utf8 = NULL;
	const char *subclass = NULL;

	if (ent->fixedtype == DWG_TYPE_TEXT)
	{
		Dwg_Entity_TEXT *txt = ent->tio.entity->tio.TEXT;
		subclass = "AcDbEntity:AcDbText";
		text_utf8 = IS_FROM_TU(dat)
			? bit_convert_TU((BITCODE_TU)txt->text_value)
			: txt->text_value;
	}
	else if (ent->fixedtype == DWG_TYPE_MTEXT)
	{
		Dwg_Entity_MTEXT *mtext = ent->tio.entity->tio.MTEXT;
		subclass = "AcDbEntity:AcDbMText";
		text_utf8 = IS_FROM_TU(dat)
			? bit_convert_TU((BITCODE_TU)mtext->text)
			: mtext->text;
	}
	else
		return 0;

	if (!text_utf8) text_utf8 = "";
	// 过滤空字符串或超长乱码 
	if (text_utf8[0] == '\0') {
		return 0;   // 不输出空文字
	}
	if (strlen(text_utf8) > 200) {
		fprintf(stderr, "WARNING: skipping long text (%zu chars) from handle %lu\n",
			strlen(text_utf8), (unsigned long)ent->handle.value);
		return 0;   // 疑似乱码，跳过
	}

	// Feature 头部 
	PAIR_Sc(type, "Feature");
	snprintf(tmp, sizeof(tmp), FORMAT_HV, ent->handle.value);
	PAIR_Sc(id, tmp);

	// properties 
	KEY(properties);
	SAMEHASH;
	PAIR_S(SubClasses, subclass);

	// 图层
	{
		int layer_error = 1;
		char *layer_name = NULL;
		if (ent->tio.entity->layer && ent->tio.entity->layer->obj)
		{
			Dwg_Object *layer_obj = ent->tio.entity->layer->obj;
			if (layer_obj->fixedtype == DWG_TYPE_LAYER ||
				layer_obj->fixedtype == DWG_TYPE_DICTIONARY)
				layer_name = dwg_obj_table_get_name(layer_obj, &layer_error);
		}
		PAIR_S(Layer, (layer_error || !layer_name) ? "0" : layer_name);
		if (!layer_error && layer_name && IS_FROM_TU(dat))
			free(layer_name);
	}

	// 文字内容 
	if (IS_FROM_TU(dat))
	{
		PAIR_S(Text, text_utf8);
		free(text_utf8);
	}
	else
		PAIR_S(Text, text_utf8);

	ENDHASH; // properties

	// geometry: Point 
	KEY(geometry);
	HASH;
	PAIR_Sc(type, "Point");
	KEY(coordinates);
	LASTVALUE_2DPOINT(x, y);
	ENDHASH; // geometry

	ENDHASH; // feature
	return 1;
}

// 根据句柄值在 dwg->object[] 中查找对象 
static Dwg_Object *
find_obj_by_handle(Dwg_Data *restrict dwg, BITCODE_H handle)
{
	BITCODE_BL i;
	for (i = 0; i < dwg->num_objects; i++)
	{
		if (dwg->object[i].handle.value == handle)
			return &dwg->object[i];
	}
	return NULL;
}


static int
geojson_entities_write(Bit_Chain *restrict dat, Dwg_Data *restrict dwg)
{
	BITCODE_BL i;
	int success;
	int written = 0, skipped = 0;
	int skip_counts[256] = { 0 };
	unsigned long block_text_count = 0;

	SECTION(features);

	/* 第一遍：输出所有顶层实体 */
	for (i = 0; i < dwg->num_objects; i++)
	{
		int is_last = 0;
		Dwg_Object *obj = &dwg->object[i];
		success = dwg_geojson_object(dat, obj, is_last);

		if (success)
			written++;
		else
		{
			skipped++;
			if (obj->fixedtype < 256)
				skip_counts[obj->fixedtype]++;
			if (skipped <= 20 || i == dwg->num_objects - 1)
			{
				const char *type_name = dwg_type_name(obj->fixedtype);
			//	fprintf(stderr, "SKIPPED obj[%lu] fixedtype=%d (%s)\n",
			//	i, obj->fixedtype, type_name ? type_name : "unknown");
			}
		}
	}

	/* 第二遍：展开 INSERT 块内的文字实体 (TEXT / MTEXT) */
	
	int insert_count = 0, block_text_found = 0;

	for (i = 0; i < dwg->num_objects; i++)
	{
		Dwg_Object *obj = &dwg->object[i];
		if (obj->fixedtype != DWG_TYPE_INSERT)
			continue;
		insert_count++;

		Dwg_Entity_INSERT *ins = obj->tio.entity->tio.INSERT;
		int error;
		Dwg_Object *block_hdr = dwg_ref_get_object(ins->block_header, &error);
		if (error || !block_hdr || block_hdr->fixedtype != DWG_TYPE_BLOCK_HEADER)
			continue;

		Dwg_Object_BLOCK_HEADER *bh = block_hdr->tio.object->tio.BLOCK_HEADER;
		if (!bh->entities || bh->num_owned == 0)
			continue;

		BITCODE_2BD ins_pt = { ins->ins_pt.x, ins->ins_pt.y };

		for (BITCODE_BL j = 0; j < bh->num_owned; j++)
		{
			// 使用手动句柄查找（因为 dwg_resolve_handle 可能失败） 
			Dwg_Object *ent = find_obj_by_handle(dwg, bh->entities[j]);
			if (!ent || !ent->tio.entity)
				continue;

			if (ent->fixedtype != DWG_TYPE_TEXT && ent->fixedtype != DWG_TYPE_MTEXT)
				continue;

			double local_x, local_y;
			if (ent->fixedtype == DWG_TYPE_TEXT)
			{
				Dwg_Entity_TEXT *txt = ent->tio.entity->tio.TEXT;
				local_x = txt->ins_pt.x;
				local_y = txt->ins_pt.y;
			}
			else
			{
				Dwg_Entity_MTEXT *mtxt = ent->tio.entity->tio.MTEXT;
				local_x = mtxt->ins_pt.x;
				local_y = mtxt->ins_pt.y;
			}

			// 坐标变换 
			transform_point_2d(&local_x, &local_y,
				ins_pt.x, ins_pt.y,
				ins->scale.x, ins->scale.y,
				ins->rotation);

			success = output_text_feature(dat, ent, local_x, local_y);
			if (success)
			{
				written++;
				block_text_count++;
				block_text_found++;
			}
		}
	} 

	/* JSON 数组结束哨兵 */
	HASH PAIR_Sc(type, "Feature");
	PAIR_NULL(properties);
	LASTPAIR_NULL(geometry);
	LASTENDHASH;

	/* 跳过类型统计 */
	fprintf(stderr, "=== Skipped entity type counts ===\n");
	for (int t = 0; t < 256; t++)
		if (skip_counts[t] > 0)
			fprintf(stderr, "Type %3d (%s): %d\n", t,
				dwg_type_name(t) ? dwg_type_name(t) : "unknown", skip_counts[t]);
	fprintf(stderr, "=================================\n");
	fprintf(stderr, "GeoJSON entities: written=%d (from INSERT texts: %lu), skipped=%d\n",
		written, block_text_count, skipped);

	ENDSEC();
	return 0;
}
//----------------------------------------------------------------------------new2
#endif //------------------------------------------------------------newline

//--------------------------------------------------------------------------new1 geojson_entities_write   注释掉了 dwg_geojson_object和dwg_geojson_feature ，因为不在依赖它们
/* ========== 动态图层记录结构 ========== */
typedef struct {
	char *name;
	char **texts;           // 动态文字数组
	int text_count;
	int text_capacity;      // texts 当前容量
} LayerTexts;

static LayerTexts *layers = NULL;       // 动态图层数组
static int num_layers = 0;
static int layers_capacity = 0;

#define INIT_LAYERS_CAP  256
#define INIT_TEXTS_CAP   256
#define TEXTS_GROW_FACTOR 2

/* 查找或创建图层，返回索引，失败返回 -1 */
static int find_or_create_layer(const char *name) {
	int i;
	for (i = 0; i < num_layers; i++) {
		if (strcmp(layers[i].name, name) == 0)
			return i;
	}

	/* 扩容图层数组 */
	if (num_layers >= layers_capacity) {
		int new_cap = layers_capacity == 0 ? INIT_LAYERS_CAP : layers_capacity * 2;
		LayerTexts *tmp = realloc(layers, new_cap * sizeof(LayerTexts));
		if (!tmp) return -1;
		layers = tmp;
		layers_capacity = new_cap;
	}

	int idx = num_layers++;
	layers[idx].name = strdup(name);
	if (!layers[idx].name) return -1;
	layers[idx].texts = malloc(INIT_TEXTS_CAP * sizeof(char*));
	if (!layers[idx].texts) {
		free(layers[idx].name);
		return -1;
	}
	layers[idx].text_count = 0;
	layers[idx].text_capacity = INIT_TEXTS_CAP;
	return idx;
}

/* 向图层 idx 添加文字（自动扩容） */
static int add_text_to_layer(int idx, const char *text) {
	if (idx < 0 || idx >= num_layers) return -1;
	LayerTexts *layer = &layers[idx];

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

/* 释放所有图层数据 */
static void free_all_layers(void) {
	int i, j;
	for (i = 0; i < num_layers; i++) {
		free(layers[i].name);
		for (j = 0; j < layers[i].text_count; j++)
			free(layers[i].texts[j]);
		free(layers[i].texts);
	}
	free(layers);
	layers = NULL;
	num_layers = 0;
	layers_capacity = 0;
}


/* ========== 主函数（动态版本，消除所有硬编码长度） ========== */
static int
geojson_entities_write(Bit_Chain *restrict dat, Dwg_Data *restrict dwg)
{
	BITCODE_BL i;
	int first = 1;

	dat->from_version = dwg->header.version;
	dat->codepage = dwg->header.codepage;

	free_all_layers();

	for (i = 0; i < dwg->num_objects; i++)
	{
		Dwg_Object *obj = &dwg->object[i];
		int ftype = obj->fixedtype;

		/* ---------- 1. 确保 LAYER 对象被记录 ---------- */
		if (ftype == DWG_TYPE_LAYER) {
			int error;
			char *name = dwg_obj_table_get_name(obj, &error);
			if (!error && name) {
				size_t len = strlen(name);
				// 转义后最大长度：每个字符最多变成2个字符（如 \" 或 \\）
				size_t esc_max = len * 2 + 3;
				char *layer_name_esc = malloc(esc_max);
				if (layer_name_esc) {
					json_cquote(layer_name_esc, name, esc_max,
						IS_FROM_TU(dat) ? 0 : dat->codepage);
					if (layer_name_esc[0] != '\0')
						find_or_create_layer(layer_name_esc);
					free(layer_name_esc);
				}
				if (IS_FROM_TU(dat)) {
					free(name);
				}
			}
			continue;
		}

		/* ---------- 2. 处理文本实体 ---------- */
		if (ftype != DWG_TYPE_TEXT &&
			ftype != DWG_TYPE_MTEXT &&
			ftype != DWG_TYPE_ATTRIB &&
			ftype != DWG_TYPE_ATTDEF &&
			ftype != DWG_TYPE_GEOPOSITIONMARKER)
			continue;

		// 动态获取图层名（转义后）
		char *layer_name_esc = NULL;
		if (obj->supertype == DWG_SUPERTYPE_ENTITY && obj->tio.entity->layer) {
			int error;
			Dwg_Object *layer_obj = obj->tio.entity->layer->obj;
			if (layer_obj && (layer_obj->fixedtype == DWG_TYPE_LAYER ||
				layer_obj->fixedtype == DWG_TYPE_DICTIONARY)) {
				char *name = dwg_obj_table_get_name(layer_obj, &error);
				if (!error && name) {
					size_t len = strlen(name);
					size_t esc_max = len * 2 + 3;
					layer_name_esc = malloc(esc_max);
					if (layer_name_esc) {
						json_cquote(layer_name_esc, name, esc_max,
							IS_FROM_TU(dat) ? 0 : dat->codepage);
					}
					if (IS_FROM_TU(dat)) {
						free(name);
					}
				}
			}
		}
		if (!layer_name_esc) {
			// 图层名获取失败，归入 __UNKNOWN__
			layer_name_esc = strdup("__UNKNOWN__");
		}

		/* 提取文字内容（动态，不限制长度） */
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
			char *txt = IS_FROM_TU(dat) ? bit_convert_TU((BITCODE_TU)tv) : (char*)tv;
			if (txt) {
				// 直接复制原始文本，不管长度
				raw_text = strdup(txt);
				if (IS_FROM_TU(dat))
					free(txt);
			}
		}

		if (raw_text) {
			// 动态生成 JSON 转义后的字符串
			size_t len = strlen(raw_text);
			size_t esc_max = len * 2 + 10;   // 留出余量
			char *json_text = malloc(esc_max);
			if (json_text) {
				json_cquote(json_text, raw_text, esc_max,
					(IS_FROM_TU(dat) ? 0 : dat->codepage));

				int idx = find_or_create_layer(layer_name_esc);
				if (idx >= 0) {
					add_text_to_layer(idx, json_text);
				}
				free(json_text);
			}
			free(raw_text);
		}

		free(layer_name_esc);
	}

	/* ========== 输出 JSON ========== */
	fprintf(dat->fh, "[\n");

	for (int l = 0; l < num_layers; l++) {
		//if (strcmp(layers[l].name, "__UNKNOWN__") == 0)
		//	continue;

		if (!first)
			fprintf(dat->fh, ",\n");
		else
			first = 0;

		fprintf(dat->fh, "  {\"Layer\": \"%s\", \"Text\": [", layers[l].name);
		if (layers[l].text_count > 0) {
			fprintf(dat->fh, "\n");
			for (int t = 0; t < layers[l].text_count; t++) {
				fprintf(dat->fh, "      \"%s\"", layers[l].texts[t]);
				if (t < layers[l].text_count - 1)
					fprintf(dat->fh, ",");
				fprintf(dat->fh, "\n");
			}
			fprintf(dat->fh, "    ]");
		}
		else {
			fprintf(dat->fh, "]");
		}
		fprintf(dat->fh, "}");
	}

	fprintf(dat->fh, "\n]\n");

	return 0;
}


/* ========== 提取 INSERT/MINSERT 内部文字（动态版本） ========== */
static int
geojson_entities_with_inserts_write(Bit_Chain *restrict dat, Dwg_Data *restrict dwg)
{
	BITCODE_BL i;
	int first = 1;

	dat->from_version = dwg->header.version;
	dat->codepage = dwg->header.codepage;

	free_all_layers();

	/* ---------- 第一遍：处理所有对象（普通文本 + 图层 + INSERT 块） ---------- */
	for (i = 0; i < dwg->num_objects; i++)
	{
		Dwg_Object *obj = &dwg->object[i];
		int ftype = obj->fixedtype;

		/* ---- 1. 记录图层 ---- */
		if (ftype == DWG_TYPE_LAYER) {
			int error;
			char *name = dwg_obj_table_get_name(obj, &error);
			if (!error && name) {
				size_t len = strlen(name);
				size_t esc_max = len * 2 + 3;
				char *layer_name_esc = malloc(esc_max);
				if (layer_name_esc) {
					json_cquote(layer_name_esc, name, esc_max,
						IS_FROM_TU(dat) ? 0 : dat->codepage);
					if (layer_name_esc[0] != '\0')
						find_or_create_layer(layer_name_esc);
					free(layer_name_esc);
				}
				if (IS_FROM_TU(dat)) free(name);
			}
			continue;
		}

		/* ---- 2. 普通文本实体（非 INSERT） ---- */
		if (ftype != DWG_TYPE_INSERT && ftype != DWG_TYPE_MINSERT) {
			if (ftype != DWG_TYPE_TEXT &&
				ftype != DWG_TYPE_MTEXT &&
				ftype != DWG_TYPE_ATTRIB &&
				ftype != DWG_TYPE_ATTDEF &&
				ftype != DWG_TYPE_GEOPOSITIONMARKER)
				continue;

			/* 动态获取图层名（转义后） */
			char *layer_name_esc = NULL;
			if (obj->supertype == DWG_SUPERTYPE_ENTITY && obj->tio.entity->layer) {
				int error;
				Dwg_Object *layer_obj = obj->tio.entity->layer->obj;
				if (layer_obj && (layer_obj->fixedtype == DWG_TYPE_LAYER ||
					layer_obj->fixedtype == DWG_TYPE_DICTIONARY)) {
					char *name = dwg_obj_table_get_name(layer_obj, &error);
					if (!error && name) {
						size_t len = strlen(name);
						size_t esc_max = len * 2 + 3;
						layer_name_esc = malloc(esc_max);
						if (layer_name_esc) {
							json_cquote(layer_name_esc, name, esc_max,
								IS_FROM_TU(dat) ? 0 : dat->codepage);
						}
						if (IS_FROM_TU(dat)) free(name);
					}
				}
			}
			if (!layer_name_esc) layer_name_esc = strdup("__UNKNOWN__");

			/* 提取文字（动态） */
			char *raw_text = NULL;
			BITCODE_TV tv = NULL;
			switch (ftype) {
			case DWG_TYPE_TEXT:
				if (obj->tio.entity->tio.TEXT) tv = obj->tio.entity->tio.TEXT->text_value;
				break;
			case DWG_TYPE_MTEXT:
				if (obj->tio.entity->tio.MTEXT) tv = obj->tio.entity->tio.MTEXT->text;
				break;
			case DWG_TYPE_ATTRIB:
				if (obj->tio.entity->tio.ATTRIB) tv = obj->tio.entity->tio.ATTRIB->text_value;
				break;
			case DWG_TYPE_ATTDEF:
				if (obj->tio.entity->tio.ATTDEF) tv = obj->tio.entity->tio.ATTDEF->default_value;
				break;
			case DWG_TYPE_GEOPOSITIONMARKER:
				if (obj->tio.entity->tio.GEOPOSITIONMARKER) tv = obj->tio.entity->tio.GEOPOSITIONMARKER->notes;
				break;
			}
			if (tv) {
				char *txt = IS_FROM_TU(dat) ? bit_convert_TU((BITCODE_TU)tv) : (char*)tv;
				if (txt) {
					raw_text = strdup(txt);
					if (IS_FROM_TU(dat)) free(txt);
				}
			}

			if (raw_text) {
				size_t len = strlen(raw_text);
				size_t esc_max = len * 2 + 10;
				char *json_text = malloc(esc_max);
				if (json_text) {
					json_cquote(json_text, raw_text, esc_max,
						(IS_FROM_TU(dat) ? 0 : dat->codepage));
					int idx = find_or_create_layer(layer_name_esc);
					if (idx >= 0) add_text_to_layer(idx, json_text);
					free(json_text);
				}
				free(raw_text);
			}
			free(layer_name_esc);
			continue;
		}

		/* ---- 3. INSERT / MINSERT 对象 ---- */
		if (ftype != DWG_TYPE_INSERT && ftype != DWG_TYPE_MINSERT)
			continue;  /* 安全起见，但前面已排除 */

		BITCODE_H block_header;
		BITCODE_BL num_owned = 0;
		BITCODE_H *attribs = NULL;
		int error;

		if (ftype == DWG_TYPE_INSERT) {
			Dwg_Entity_INSERT *ins = obj->tio.entity->tio.INSERT;
			block_header = ins->block_header;
			num_owned = ins->num_owned;
			attribs = ins->attribs;
		}
		else {
			Dwg_Entity_MINSERT *mins = obj->tio.entity->tio.MINSERT;
			block_header = mins->block_header;
			num_owned = mins->num_owned;
			attribs = mins->attribs;
		}

		/* ---- 3a. 块定义内的实体 ---- */
		Dwg_Object *block_hdr = dwg_ref_get_object(block_header, &error);
		if (!error && block_hdr && block_hdr->fixedtype == DWG_TYPE_BLOCK_HEADER) {
			Dwg_Object_BLOCK_HEADER *bh = block_hdr->tio.object->tio.BLOCK_HEADER;
			BITCODE_BL j;
			for (j = 0; bh->entities && j < bh->num_owned; j++) {
				Dwg_Object *ent = NULL;
				BITCODE_H h = bh->entities[j];
				BITCODE_BL k;
				for (k = 0; k < dwg->num_objects; k++) {
					if (dwg->object[k].handle.value == h) {
						ent = &dwg->object[k];
						break;
					}
				}
				if (!ent || !ent->tio.entity) continue;

				int etype = ent->fixedtype;
				if (etype != DWG_TYPE_TEXT && etype != DWG_TYPE_MTEXT &&
					etype != DWG_TYPE_ATTRIB && etype != DWG_TYPE_ATTDEF &&
					etype != DWG_TYPE_GEOPOSITIONMARKER) continue;

				/* 图层名 */
				char *layer_name_esc = NULL;
				if (ent->supertype == DWG_SUPERTYPE_ENTITY && ent->tio.entity->layer) {
					int lerr;
					Dwg_Object *lobj = ent->tio.entity->layer->obj;
					if (lobj && (lobj->fixedtype == DWG_TYPE_LAYER ||
						lobj->fixedtype == DWG_TYPE_DICTIONARY)) {
						char *name = dwg_obj_table_get_name(lobj, &lerr);
						if (!lerr && name) {
							size_t len = strlen(name);
							size_t esc_max = len * 2 + 3;
							layer_name_esc = malloc(esc_max);
							if (layer_name_esc) {
								json_cquote(layer_name_esc, name, esc_max,
									IS_FROM_TU(dat) ? 0 : dat->codepage);
							}
							if (IS_FROM_TU(dat)) free(name);
						}
					}
				}
				if (!layer_name_esc) layer_name_esc = strdup("__UNKNOWN__");

				/* 文字 */
				char *raw_text = NULL;
				BITCODE_TV tv = NULL;
				switch (etype) {
				case DWG_TYPE_TEXT:
					if (ent->tio.entity->tio.TEXT) tv = ent->tio.entity->tio.TEXT->text_value;
					break;
				case DWG_TYPE_MTEXT:
					if (ent->tio.entity->tio.MTEXT) tv = ent->tio.entity->tio.MTEXT->text;
					break;
				case DWG_TYPE_ATTRIB:
					if (ent->tio.entity->tio.ATTRIB) tv = ent->tio.entity->tio.ATTRIB->text_value;
					break;
				case DWG_TYPE_ATTDEF:
					if (ent->tio.entity->tio.ATTDEF) tv = ent->tio.entity->tio.ATTDEF->default_value;
					break;
				case DWG_TYPE_GEOPOSITIONMARKER:
					if (ent->tio.entity->tio.GEOPOSITIONMARKER) tv = ent->tio.entity->tio.GEOPOSITIONMARKER->notes;
					break;
				}
				if (tv) {
					char *txt = IS_FROM_TU(dat) ? bit_convert_TU((BITCODE_TU)tv) : (char*)tv;
					if (txt) {
						raw_text = strdup(txt);
						if (IS_FROM_TU(dat)) free(txt);
					}
				}

				if (raw_text) {
					size_t len = strlen(raw_text);
					size_t esc_max = len * 2 + 10;
					char *json_text = malloc(esc_max);
					if (json_text) {
						json_cquote(json_text, raw_text, esc_max,
							(IS_FROM_TU(dat) ? 0 : dat->codepage));
						int idx = find_or_create_layer(layer_name_esc);
						if (idx >= 0) add_text_to_layer(idx, json_text);
						free(json_text);
					}
					free(raw_text);
				}
				free(layer_name_esc);
			}
		}

		/* ---- 3b. INSERT 直接拥有的 attribs 数组 ---- */
		{
			BITCODE_BL j;
			for (j = 0; attribs && j < num_owned; j++) {
				Dwg_Object *ent = NULL;
				BITCODE_H h = attribs[j];
				BITCODE_BL k;
				for (k = 0; k < dwg->num_objects; k++) {
					if (dwg->object[k].handle.value == h) {
						ent = &dwg->object[k];
						break;
					}
				}
				if (!ent || !ent->tio.entity) continue;

				int etype = ent->fixedtype;
				if (etype != DWG_TYPE_TEXT && etype != DWG_TYPE_MTEXT &&
					etype != DWG_TYPE_ATTRIB && etype != DWG_TYPE_ATTDEF &&
					etype != DWG_TYPE_GEOPOSITIONMARKER) continue;

				char *layer_name_esc = NULL;
				if (ent->supertype == DWG_SUPERTYPE_ENTITY && ent->tio.entity->layer) {
					int lerr;
					Dwg_Object *lobj = ent->tio.entity->layer->obj;
					if (lobj && (lobj->fixedtype == DWG_TYPE_LAYER ||
						lobj->fixedtype == DWG_TYPE_DICTIONARY)) {
						char *name = dwg_obj_table_get_name(lobj, &lerr);
						if (!lerr && name) {
							size_t len = strlen(name);
							size_t esc_max = len * 2 + 3;
							layer_name_esc = malloc(esc_max);
							if (layer_name_esc) {
								json_cquote(layer_name_esc, name, esc_max,
									IS_FROM_TU(dat) ? 0 : dat->codepage);
							}
							if (IS_FROM_TU(dat)) free(name);
						}
					}
				}
				if (!layer_name_esc) layer_name_esc = strdup("__UNKNOWN__");

				char *raw_text = NULL;
				BITCODE_TV tv = NULL;
				switch (etype) {
				case DWG_TYPE_TEXT:
					if (ent->tio.entity->tio.TEXT) tv = ent->tio.entity->tio.TEXT->text_value;
					break;
				case DWG_TYPE_MTEXT:
					if (ent->tio.entity->tio.MTEXT) tv = ent->tio.entity->tio.MTEXT->text;
					break;
				case DWG_TYPE_ATTRIB:
					if (ent->tio.entity->tio.ATTRIB) tv = ent->tio.entity->tio.ATTRIB->text_value;
					break;
				case DWG_TYPE_ATTDEF:
					if (ent->tio.entity->tio.ATTDEF) tv = ent->tio.entity->tio.ATTDEF->default_value;
					break;
				case DWG_TYPE_GEOPOSITIONMARKER:
					if (ent->tio.entity->tio.GEOPOSITIONMARKER) tv = ent->tio.entity->tio.GEOPOSITIONMARKER->notes;
					break;
				}
				if (tv) {
					char *txt = IS_FROM_TU(dat) ? bit_convert_TU((BITCODE_TU)tv) : (char*)tv;
					if (txt) {
						raw_text = strdup(txt);
						if (IS_FROM_TU(dat)) free(txt);
					}
				}

				if (raw_text) {
					size_t len = strlen(raw_text);
					size_t esc_max = len * 2 + 10;
					char *json_text = malloc(esc_max);
					if (json_text) {
						json_cquote(json_text, raw_text, esc_max,
							(IS_FROM_TU(dat) ? 0 : dat->codepage));
						int idx = find_or_create_layer(layer_name_esc);
						if (idx >= 0) add_text_to_layer(idx, json_text);
						free(json_text);
					}
					free(raw_text);
				}
				free(layer_name_esc);
			}
		}
	}

	/* ========== 输出 JSON（与参考代码完全一致） ========== */
	fprintf(dat->fh, "[\n");

	for (int l = 0; l < num_layers; l++) {
		if (strcmp(layers[l].name, "__UNKNOWN__") == 0)
			continue;

		if (!first)
			fprintf(dat->fh, ",\n");
		else
			first = 0;

		fprintf(dat->fh, "  {\"Layer\": \"%s\", \"Text\": [", layers[l].name);
		if (layers[l].text_count > 0) {
			fprintf(dat->fh, "\n");
			for (int t = 0; t < layers[l].text_count; t++) {
				fprintf(dat->fh, "      \"%s\"", layers[l].texts[t]);
				if (t < layers[l].text_count - 1)
					fprintf(dat->fh, ",");
				fprintf(dat->fh, "\n");
			}
			fprintf(dat->fh, "    ]");
		}
		else {
			fprintf(dat->fh, "]");
		}
		fprintf(dat->fh, "}");
	}

	fprintf(dat->fh, "\n]\n");

	return 0;
}

//----------------------------------------------------------------------new2

//----------------------------------------------------------------------new1
EXPORT int
dwg_write_geojson(Bit_Chain *restrict dat, Dwg_Data *restrict dwg)
{ 
	char *json = NULL;
	int error;

	if (!dwg || !dwg->num_objects || !dat || !dat->fh)
		return 1;

	error = dwgcore_extract_geojson_text(dwg, &json, NULL);
	if (error || !json)
		return 1;

	fputs(json, dat->fh);
	dwgcore_free(json);
	return ferror(dat->fh) ? 1 : 0;
}

//----------------------------------------------------------------------new2





#if 0
EXPORT int
dwg_write_geojson(Bit_Chain *restrict dat, Dwg_Data *restrict dwg)
{
	// const int minimal = dwg->opts & DWG_OPTS_MINIMAL;
	char date[12] = "YYYY-MM-DD";
	time_t rawtime;

	if (!dwg->num_objects || !dat->fh)
		goto fail;

	HASH;
	PAIR_Sc(type, "FeatureCollection");

	// array of features
	if (geojson_entities_write(dat, dwg))
		goto fail;

	KEY(geocoding);
	HASH;
	time(&rawtime);
	strftime(date, 12, "%Y-%m-%d", localtime(&rawtime));
	PAIR_Sc(creation_date, date);
	KEY(generator);
	HASH;
	KEY(author);
	HASH;
	LASTPAIR_Sc(name, "dwgread");
	ENDHASH;
	PAIR_Sc(package, PACKAGE_NAME);
	LASTPAIR_Sc(version, PACKAGE_VERSION);
	LASTENDHASH;
	// PAIR_S(license, "?");
	LASTENDHASH;

	LASTENDHASH;
	return 0;
fail:
	return 1;
}
#endif


#if 0
/* ========================================================
 * 以下为公开 API：dwg_geojson_layers_text
 * 返回图层+文字的 GeoJSON 字符串，不写文件
 * 编码处理与 geojson_entities_write 完全一致
 * ======================================================== */

 /* 简单的动态字符串缓冲区，用于替代 FILE* 输出 */
typedef struct {
	char *buf;
	size_t len;     // 当前已用长度（不含结尾 '\0'）
	size_t cap;     // 总容量（包括结尾 '\0'）
} DynStr;

static void dynstr_init(DynStr *ds) {
	ds->cap = 4096;
	ds->buf = malloc(ds->cap);
	if (ds->buf) {
		ds->buf[0] = '\0';
		ds->len = 0;
	}
}

static void dynstr_free(DynStr *ds) {
	free(ds->buf);
	ds->buf = NULL;
	ds->len = ds->cap = 0;
}

/* 确保能再追加 add 个字符（含结尾 '\0'） */
static int dynstr_ensure(DynStr *ds, size_t add) {
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

static void dynstr_printf(DynStr *ds, const char *fmt, ...) {
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

static void dynstr_puts(DynStr *ds, const char *s) {
	dynstr_printf(ds, "%s", s);
}

static void dynstr_putc(DynStr *ds, char c) {
	if (!dynstr_ensure(ds, 1)) return;
	ds->buf[ds->len++] = c;
	ds->buf[ds->len] = '\0';
}

/* 公开函数 */
EXPORT int
dwg_geojson_layers_text(char** cszTextOut, Dwg_Data *restrict dwg)
{
	if (!cszTextOut || !dwg) return 1;
	*cszTextOut = NULL;

	/* 判断是否来自 TU 版本，以此决定编码转换与释放逻辑 */
	int from_tu = (dwg->header.version >= R_2007);  // R_2007 为 libredwg 版本枚举，TU 的分界线
	int codepage = from_tu ? 0 : dwg->header.codepage;

	free_all_layers();

	BITCODE_BL i;
	for (i = 0; i < dwg->num_objects; i++)
	{
		Dwg_Object *obj = &dwg->object[i];
		int ftype = obj->fixedtype;

		/* ---------- 1. 确保 LAYER 对象被记录 ---------- */
		if (ftype == DWG_TYPE_LAYER) {
			int error;
			char *name = dwg_obj_table_get_name(obj, &error);
			if (!error && name) {
				size_t len = strlen(name);
				size_t esc_max = len * 2 + 3;
				char *layer_name_esc = malloc(esc_max);
				if (layer_name_esc) {
					json_cquote(layer_name_esc, name, esc_max, codepage);
					if (layer_name_esc[0] != '\0')
						find_or_create_layer(layer_name_esc);
					free(layer_name_esc);
				}
				if (from_tu) free(name);   // TU 版本 name 为动态分配，需要释放
			}
			continue;
		}

		/* ---------- 2. 处理文本实体 ---------- */
		if (ftype != DWG_TYPE_TEXT &&
			ftype != DWG_TYPE_MTEXT &&
			ftype != DWG_TYPE_ATTRIB &&
			ftype != DWG_TYPE_ATTDEF &&
			ftype != DWG_TYPE_GEOPOSITIONMARKER)
			continue;

		// 动态获取图层名（转义后）
		char *layer_name_esc = NULL;
		if (obj->supertype == DWG_SUPERTYPE_ENTITY && obj->tio.entity->layer) {
			int error;
			Dwg_Object *layer_obj = obj->tio.entity->layer->obj;
			if (layer_obj && (layer_obj->fixedtype == DWG_TYPE_LAYER ||
				layer_obj->fixedtype == DWG_TYPE_DICTIONARY)) {
				char *name = dwg_obj_table_get_name(layer_obj, &error);
				if (!error && name) {
					size_t len = strlen(name);
					size_t esc_max = len * 2 + 3;
					layer_name_esc = malloc(esc_max);
					if (layer_name_esc) {
						json_cquote(layer_name_esc, name, esc_max, codepage);
					}
					if (from_tu) free(name);
				}
			}
		}
		if (!layer_name_esc) {
			layer_name_esc = strdup("__UNKNOWN__");
		}

		/* 提取文字内容（动态，不限制长度） */
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
			// 严格按 geojson_entities_write 逻辑
			char *txt = from_tu ? bit_convert_TU((BITCODE_TU)tv) : (char*)tv;
			if (txt) {
				raw_text = strdup(txt);
				if (from_tu) free(txt);  // bit_convert_TU 返回的动态内存需要释放
			}
		}

		if (raw_text) {
			size_t len = strlen(raw_text);
			size_t esc_max = len * 2 + 10;
			char *json_text = malloc(esc_max);
			if (json_text) {
				json_cquote(json_text, raw_text, esc_max, codepage);

				int idx = find_or_create_layer(layer_name_esc);
				if (idx >= 0) {
					add_text_to_layer(idx, json_text);   // 存入已转义的 JSON 字符串
				}
				free(json_text);
			}
			free(raw_text);
		}

		free(layer_name_esc);
	}

	/* ========== 生成 JSON 字符串到动态缓冲区 ========== */
	DynStr ds;
	dynstr_init(&ds);
	if (!ds.buf) {
		free_all_layers();
		return 1;
	}

	dynstr_puts(&ds, "[\n");
	int first = 1;

	for (int l = 0; l < num_layers; l++) {
		//if (strcmp(layers[l].name, "__UNKNOWN__") == 0)
			//continue;

		if (!first)
			dynstr_puts(&ds, ",\n");
		else
			first = 0;

		dynstr_printf(&ds, "  {\"Layer\": \"%s\", \"Text\": [", layers[l].name);
		if (layers[l].text_count > 0) {
			dynstr_putc(&ds, '\n');
			for (int t = 0; t < layers[l].text_count; t++) {
				// 文本已经是 JSON 转义好的，直接输出
				dynstr_printf(&ds, "      \"%s\"", layers[l].texts[t]);
				if (t < layers[l].text_count - 1)
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
	free_all_layers();   // 图层数据已用不到，输出字符串由调用者释放
	return 0;
}
//---------------------------------------------------------------------new2qt
#endif
#undef IS_PRINT
