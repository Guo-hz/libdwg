/*****************************************************************************/
/*  libdwg2text text extraction API                                           */
/*****************************************************************************/

#ifndef DWG_TEXT_API_H
#define DWG_TEXT_API_H

#include <stddef.h>
#include "dwg.h"

#ifdef __cplusplus
extern "C" {
#endif

EXPORT int dwgcore_extract_geojson_text (Dwg_Data *restrict dwg,
                                         char **restrict out_json,
                                         size_t *restrict out_size);

EXPORT int dwgcore_extract_geojson_text_file (const char *restrict dwg_path,
                                              char **restrict out_json,
                                              size_t *restrict out_size);

EXPORT void dwgcore_free (void *ptr);

/* Compatibility entry point. Prefer dwgcore_extract_geojson_text for new code. */
EXPORT int dwg_geojson_layers_text (char **out_json, Dwg_Data *restrict dwg);

#ifdef __cplusplus
}
#endif

#endif
