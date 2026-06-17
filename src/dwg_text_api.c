#include "dwg_text_api.h"

#include <stdlib.h>
#include <string.h>

int dwg_geojson_layers_text_impl (char **out_json, Dwg_Data *restrict dwg);

EXPORT int
dwgcore_extract_geojson_text (Dwg_Data *restrict dwg, char **restrict out_json,
                              size_t *restrict out_size)
{
  int error;

  if (!out_json || !dwg)
    return 1;

  *out_json = NULL;
  if (out_size)
    *out_size = 0;

  error = dwg_geojson_layers_text_impl (out_json, dwg);
  if (error || !*out_json)
    {
      free (*out_json);
      *out_json = NULL;
      return error ? error : 1;
    }

  if (out_size)
    *out_size = strlen (*out_json);
  return 0;
}

EXPORT int
dwgcore_extract_geojson_text_file (const char *restrict dwg_path,
                                   char **restrict out_json,
                                   size_t *restrict out_size)
{
  Dwg_Data dwg;
  int error;

  if (!out_json || !dwg_path || !dwg_path[0])
    return 1;

  *out_json = NULL;
  if (out_size)
    *out_size = 0;

  memset (&dwg, 0, sizeof (dwg));
  error = dwg_read_file (dwg_path, &dwg);
  if (error >= DWG_ERR_CRITICAL)
    return error;

  error = dwgcore_extract_geojson_text (&dwg, out_json, out_size);
  dwg_free (&dwg);
  return error;
}

EXPORT void
dwgcore_free (void *ptr)
{
  free (ptr);
}

EXPORT int
dwg_geojson_layers_text (char **out_json, Dwg_Data *restrict dwg)
{
  return dwgcore_extract_geojson_text (dwg, out_json, NULL);
}
