/*****************************************************************************/
/*  LibreDWG - free implementation of the DWG file format                    */
/*                                                                           */
/*  Copyright (C) 2010-2025 Free Software Foundation, Inc.                   */
/*                                                                           */
/*  This library is free software, licensed under the terms of the GNU       */
/*  General Public License as published by the Free Software Foundation,     */
/*  either version 3 of the License, or (at your option) any later version.  */
/*  You should have received a copy of the GNU General Public License        */
/*  along with this program.  If not, see <http://www.gnu.org/licenses/>.    */
/*****************************************************************************/

/*
 * decode_r2007.c: functions to decode R2007 (AC1021) sections
 * written by Till Heuschmann
 * modified by Reini Urban
 */

#define IS_DECODER
#define _GNU_SOURCE 1 /* for memmem on linux */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>
#include <stdbool.h>
#include <limits.h>
#include <assert.h>

#include "common.h"
#include "bits.h"
#include "decode.h"
#include "dynapi.h"

/* The logging level for the read (decode) path.  */

/* the current version per spec block */
static unsigned int cur_ver = 0;

#define DWG_LOGLEVEL loglevel
#include "logging.h"
#include "dec_macros.h"
#include "spec.h"

// only for temp. debugging, to abort on obviously wrong sizes.
// should be a bit larger then the filesize.
#define DBG_MAX_COUNT 0x100000
#define DBG_MAX_SIZE 0xff0000 /* should be dat->size */

/* page map */
typedef struct _r2007_page
{
  int64_t id;
  uint64_t size;
  uint64_t offset;
  struct _r2007_page *next;
} r2007_page;

/* section page */
typedef struct _r2007_section_page
{
  uint64_t offset;
  uint64_t size;
  int64_t id;
  uint64_t uncomp_size; // src_size
  uint64_t comp_size;
  uint64_t checksum;
  uint64_t crc;
} r2007_section_page;

/* section map */
typedef struct _r2007_section
{
  uint64_t data_size; // max size of page
  uint64_t max_size;
  int64_t encrypted;
  uint64_t hashcode;   // checksum in r2004
  int64_t name_length; // 0x22
  int64_t unknown;     // 0x00
  int64_t encoded;
  int64_t num_pages;
  DWGCHAR *name;
  Dwg_Section_Type type;
  r2007_section_page **pages;
  struct _r2007_section *next;
} r2007_section;

/* imported */
int rs_decode_block (BITCODE_RC *blk, int fix);

/* private */
static r2007_section *get_section (r2007_section *sections_map,
                                   Dwg_Section_Type sec_type);
static r2007_page *get_page (r2007_page *pages_map, int64_t id);
static void pages_destroy (r2007_page *page);
static void sections_destroy (r2007_section *section);
static r2007_section *read_sections_map (Bit_Chain *dat, int64_t size_comp,
                                         int64_t size_uncomp,
                                         int64_t correction) ATTRIBUTE_MALLOC;
static int read_data_section (Bit_Chain *sec_dat, Bit_Chain *dat,
                              r2007_section *restrict sections_map,
                              r2007_page *restrict pages_map,
                              Dwg_Section_Type sec_type);
static int read_2007_section_classes (Bit_Chain *restrict dat,
                                      Dwg_Data *restrict dwg,
                                      r2007_section *restrict sections_map,
                                      r2007_page *restrict pages_map);
static int read_2007_section_header (Bit_Chain *dat, Bit_Chain *hdl_dat,
                                     Dwg_Data *restrict dwg,
                                     r2007_section *restrict sections_map,
                                     r2007_page *restrict pages_map);
static int read_2007_section_handles (Bit_Chain *dat, Bit_Chain *hdl_dat,
                                      Dwg_Data *restrict dwg,
                                      r2007_section *restrict sections_map,
                                      r2007_page *restrict pages_map);
static int read_2007_section_summary (Bit_Chain *restrict dat,
                                      Dwg_Data *restrict dwg,
                                      r2007_section *restrict sections_map,
                                      r2007_page *restrict pages_map);
static int read_2007_section_preview (Bit_Chain *restrict dat,
                                      Dwg_Data *restrict dwg,
                                      r2007_section *restrict sections_map,
                                      r2007_page *restrict pages_map);
static r2007_page *read_pages_map (Bit_Chain *dat, int64_t size_comp,
                                   int64_t size_uncomp,
                                   int64_t correction) ATTRIBUTE_MALLOC;
static int read_file_header (Bit_Chain *restrict dat,
                             Dwg_R2007_Header *restrict file_header);
static void read_instructions (BITCODE_RC *restrict *restrict src,
                               BITCODE_RC *restrict opcode,
                               uint32_t *restrict offset,
                               uint32_t *restrict length);
static inline BITCODE_RC *copy_bytes_2 (BITCODE_RC *restrict dst,
                                        const BITCODE_RC *restrict src);
static inline BITCODE_RC *copy_bytes_3 (BITCODE_RC *restrict dst,
                                        const BITCODE_RC *restrict src);
static void copy_bytes (BITCODE_RC *dst, uint32_t length, uint32_t offset);
static uint32_t read_literal_length (BITCODE_RC *restrict *restrict src,
                                     unsigned char opcode);
static void copy_compressed_bytes (BITCODE_RC *restrict dst,
                                   BITCODE_RC *restrict src, int length);
static BITCODE_RC *decode_rs (const BITCODE_RC *src, int block_count,
                              int data_size,
                              const unsigned src_size) ATTRIBUTE_MALLOC;
static int decompress_r2007 (BITCODE_RC *restrict dst, const unsigned dst_size,
                             BITCODE_RC *restrict src, const unsigned src_size,
                             const BITCODE_RC *restrict dst_end);

#define copy_1(offset) *dst++ = *(src + offset);
#define copy_2(offset) dst = copy_bytes_2 (dst, src + offset);
#define copy_3(offset) dst = copy_bytes_3 (dst, src + offset)
// 4 and 8 is not reverse, 16 is
#define copy_n(n, offset)                                                     \
  memcpy (dst, &src[offset], n);                                              \
  dst += n

#define copy_4(offset) copy_n (4, offset)
#define copy_8(offset) copy_n (8, offset)
#define copy_16(offset)                                                       \
  memcpy (dst, &src[offset + 8], 8);                                          \
  memcpy (&dst[8], &src[offset], 8);                                          \
  dst += 16

/* Don't use restrict here: GH #141 broken for most newer compilers */
static inline BITCODE_RC *
copy_bytes_2 (BITCODE_RC *dst, const BITCODE_RC *src)
{
  dst[0] = src[1];
  dst[1] = src[0];
  return dst + 2;
}

static inline BITCODE_RC *
copy_bytes_3 (BITCODE_RC *dst, const BITCODE_RC *src)
{
  dst[0] = src[2];
  dst[1] = src[1];
  dst[2] = src[0];
  return dst + 3;
}

static void
copy_bytes (BITCODE_RC *dst, uint32_t length, uint32_t offset)
{
  BITCODE_RC *src = dst - offset;

  while (length-- > 0)
    *dst++ = *src++;
}

/* See spec version 5.0 page 30 */
static void
copy_compressed_bytes (BITCODE_RC *restrict dst, BITCODE_RC *restrict src,
                       int length)
{
  while (length >= 32)
    {
      copy_16 (16);
      copy_16 (0);

      src += 32;
      length -= 32;
    }

  switch (length)
    {
    case 0:
      break;
    case 1:
      copy_1 (0);
      break;
    case 2:
      copy_2 (0);
      break;
    case 3:
      copy_3 (0);
      break;
    case 4:
      copy_4 (0);
      break;
    case 5:
      copy_1 (4);
      copy_4 (0);
      break;
    case 6:
      copy_1 (5);
      copy_4 (1);
      copy_1 (0);
      break;
    case 7:
      copy_2 (5);
      copy_4 (1);
      copy_1 (0);
      break;
    case 8:
      copy_8 (0);
      break;
    case 9:
      copy_1 (8);
      copy_8 (0);
      break;
    case 10:
      copy_1 (9);
      copy_8 (1);
      copy_1 (0);
      break;
    case 11:
      copy_2 (9);
      copy_8 (1);
      copy_1 (0);
      break;
    case 12:
      copy_4 (8);
      copy_8 (0);
      break;
    case 13:
      copy_1 (12);
      copy_4 (8);
      copy_8 (0);
      break;
    case 14:
      copy_1 (13);
      copy_4 (9);
      copy_8 (1);
      copy_1 (0);
      break;
    case 15:
      copy_2 (13);
      copy_4 (9);
      copy_8 (1);
      copy_1 (0);
      break;
    case 16:
      copy_16 (0);
      break;
    case 17:
      copy_8 (9);
      copy_1 (8);
      copy_8 (0);
      break;
    case 18:
      copy_1 (17);
      copy_16 (1);
      copy_1 (0);
      break;
    case 19:
      copy_3 (16);
      copy_16 (0);
      break;
    case 20:
      copy_4 (16);
      copy_16 (0);
      break;
    case 21:
      copy_1 (20);
      copy_4 (16);
      copy_16 (0);
      break;
    case 22:
      copy_2 (20);
      copy_4 (16);
      copy_16 (0);
      break;
    case 23:
      copy_3 (20);
      copy_4 (16);
      copy_16 (0);
      break;
    case 24:
      copy_8 (16);
      copy_16 (0);
      break;
    case 25:
      copy_8 (17);
      copy_1 (16);
      copy_16 (0);
      break;
    case 26:
      copy_1 (25);
      copy_8 (17);
      copy_1 (16);
      copy_16 (0);
      break;
    case 27:
      copy_2 (25);
      copy_8 (17);
      copy_1 (16);
      copy_16 (0);
      break;
    case 28:
      copy_4 (24);
      copy_8 (16);
      copy_16 (0);
      break;
    case 29:
      copy_1 (28);
      copy_4 (24);
      copy_8 (16);
      copy_16 (0);
      break;
    case 30:
      copy_2 (28);
      copy_4 (24);
      copy_8 (16);
      copy_16 (0);
      break;
    case 31:
      copy_1 (30);
      copy_4 (26);
      copy_8 (18);
      copy_16 (2);
      copy_2 (0);
      break;
    default:
      LOG_ERROR ("Wrong length %d", length);
    }
}

/* See spec version 5.1 page 50 */
static uint32_t
read_literal_length (BITCODE_RC *restrict *src, unsigned char opcode)
{
  uint32_t length = opcode + 8;

  if (length == 0x17)
    {
      int n = *(*src)++;

      length += n;

      if (n == 0xff)
        {
          do
            {
              n = *(*src)++;
              n |= (*(*src)++ << 8);

              length += n;
            }
          while (n == 0xFFFF);
        }
    }

  return length;
}

/* See spec version 5.1 page 53 */
static void
read_instructions (BITCODE_RC *restrict *src, unsigned char *restrict opcode,
                   uint32_t *restrict offset, uint32_t *restrict length)
{
  switch (*opcode >> 4)
    {
    case 0:
      *length = (*opcode & 0xf) + 0x13;
      *offset = *(*src)++;
      *opcode = *(*src)++;
      *length = ((*opcode >> 3) & 0x10) + *length;
      *offset = ((*opcode & 0x78) << 5) + 1 + *offset;
      break;

    case 1:
      *length = (*opcode & 0xf) + 3;
      *offset = *(*src)++;
      *opcode = *(*src)++;
      *offset = ((*opcode & 0xf8) << 5) + 1 + *offset;
      break;

    case 2:
      *offset = *(*src)++;
      *offset = ((*(*src)++ << 8) & 0xff00) | *offset;
      *length = *opcode & 7;

      if ((*opcode & 8) == 0)
        {
          *opcode = *(*src)++;
          *length = (*opcode & 0xf8) + *length;
        }
      else
        {
          (*offset)++;
          *length = (*(*src)++ << 3) + *length;
          *opcode = *(*src)++;
          *length = (((*opcode & 0xf8) << 8) + *length) + 0x100;
        }
      break;

    default:
      *length = *opcode >> 4;
      *offset = *opcode & 15;
      *opcode = *(*src)++;
      *offset = (((*opcode & 0xf8) << 1) + *offset) + 1;
      break;
    }
}

/* par 4.7 Compression, page 32 (same as format 2004)
   TODO: replace by decompress_R2004_section(dat, decomp, comp_data_size)
   Note that dst + dst_size might deviate from dst_end.
*/
static int
decompress_r2007 (BITCODE_RC *restrict dst, const unsigned dst_size,
                  BITCODE_RC *restrict src, const unsigned src_size,
                  const BITCODE_RC *restrict dst_end)
{
  uint32_t length = 0;
  uint32_t offset = 0;

  BITCODE_RC *dst_start = dst;
  BITCODE_RC *src_end = src + src_size;
  unsigned char opcode;
  if (!dst_end)
    dst_end = dst + dst_size;

  LOG_INSANE ("decompress_r2007 (%p, %d, %p, %d)\n", dst, dst_size, src,
              src_size);
  if (!dst || !src || !dst_size || src_size < 2)
    {
      LOG_ERROR ("Invalid argument to %s\n", __FUNCTION__);
      return DWG_ERR_INTERNALERROR;
    }

  opcode = *src++;
  if ((opcode & 0xf0) == 0x20)
    {
      src += 2;
      length = *src++ & 0x07;
      if (length == 0)
        {
          LOG_ERROR ("Decompression error: zero length");
          return DWG_ERR_INTERNALERROR;
        }
    }

  while (src < src_end)
    {
      if (length == 0)
        length = read_literal_length (&src, opcode);

      if ((dst + length) > dst_end || (src + length) > src_end)
        {
          if (DWG_LOGLEVEL >= DWG_LOGLEVEL_HANDLE)
            {
              if ((dst + length) > dst_end)
                HANDLER (OUTPUT, "copy_compressed_bytes: dst %p + %u >= %p\n",
                         dst, (unsigned)length, dst_end);
              else
                HANDLER (OUTPUT, "copy_compressed_bytes: src %p + %u > %p\n",
                         src, (unsigned)length, src_end);
            }
          LOG_ERROR ("Decompression error: length overflow");
          return DWG_ERR_INTERNALERROR;
        }

      LOG_INSANE (
          "copy_compressed_bytes (%p, %p, %u). remaining src: %ld, dst: %ld\n",
          dst, src, (unsigned)length, (long)(src_end - src),
          (long)(dst_end - dst));
      copy_compressed_bytes (dst, src, length);

      dst += length;
      src += length;

      length = 0;

      if (src >= src_end)
        return 0;

      opcode = *src++;

      read_instructions (&src, &opcode, &offset, &length);

      while (1)
        {
          if ((dst + length) > dst_end)
            {
              LOG_HANDLE ("copy_bytes: dst %p + %u > %p\n", dst,
                          (unsigned)length, dst_end);
              LOG_ERROR ("Decompression error: length overflow");
              return DWG_ERR_INTERNALERROR;
            }
          if (offset > (uint32_t)(dst - dst_start))
            {
              LOG_HANDLE ("copy_bytes: offset %u > %p - %p\n",
                          (unsigned)offset, dst, dst_start);
              LOG_ERROR ("Decompression error: offset underflow");
              return DWG_ERR_INTERNALERROR;
            }
          LOG_INSANE ("copy_bytes (%p, %u, [%u])\n", dst, (unsigned)length,
                      (unsigned)offset);
          copy_bytes (dst, length, offset);

          dst += length;
          length = (opcode & 7);

          if (length != 0 || src >= src_end)
            break;

          opcode = *src++;

          if ((opcode >> 4) == 0)
            break;

          if ((opcode >> 4) == 0x0f)
            opcode &= 0xf;

          read_instructions ((unsigned char **)&src, &opcode, &offset,
                             &length);
        }
    }
  return 0;
}

// reed-solomon (255, 239) encoding with factor 3
// TODO: for now disabled, until we get proper data
ATTRIBUTE_MALLOC
static BITCODE_RC *
decode_rs (const BITCODE_RC *src, int block_count, int data_size,
           const unsigned src_size)
{
  int i, j;
  const BITCODE_RC *src_base = src;
  BITCODE_RC *dst_base, *dst;
  // TODO: round up data_size from 239 to 255

  if ((size_t)block_count * data_size > src_size)
    {
      LOG_ERROR ("decode_rs src overflow: %ld > %u",
                 (long)block_count * data_size, src_size);
      return NULL;
    }
  dst_base = dst = (BITCODE_RC *)calloc (block_count, data_size);
  if (!dst)
    {
      LOG_ERROR ("Out of memory");
      return NULL;
    }

  for (i = 0; i < block_count; ++i)
    {
      for (j = 0; j < data_size; ++j)
        {
          *dst++ = *src;
          src += block_count;
        }

      // rs_decode_block((unsigned char*)(dst_base + 239*i), 1);
      src = ++src_base;
    }

  return dst_base;
}

static bool
read_system_page (Bit_Chain *out, Bit_Chain *dat, int64_t size_comp,
                  int64_t size_uncomp, int64_t repeat_count)
{
  int error = 0;

  int64_t pesize;      // Pre RS encoded size
  int64_t block_count; // Number of RS encoded blocks
  int64_t page_size;
  long pedata_size;

  BITCODE_RC *rsdata;   // RS encoded data
  BITCODE_RC *pedata;   // Pre RS encoded data
  BITCODE_RC *data_end; // The data RS unencoded and uncompressed

  if (repeat_count < 0 || repeat_count > DBG_MAX_COUNT
      || (uint64_t)size_comp >= dat->size
      || (uint64_t)size_uncomp >= dat->size)
    {
      LOG_ERROR ("Invalid r2007 system page: "
                 "size_comp: %" PRId64 ", size_uncomp: %" PRId64
                 ", repeat_count: %" PRId64,
                 size_comp, size_uncomp, repeat_count);
      return false;
    }
  // Round to a multiple of 8
  pesize = ((size_comp + 7) & ~7) * repeat_count;
  // Divide pre encoded size by RS k-value (239)
  block_count = (pesize + 238) / 239;
  if (block_count <= 0 || block_count > DBG_MAX_COUNT)
    {
      LOG_ERROR ("Invalid r2007 system page: size_comp: %" PRId64
                 ", size_uncomp: %" PRId64,
                 size_comp, size_uncomp);
      return false;
    }
  // Multiply with codeword size (255) and round to a multiple of 8
  page_size = (block_count * 255 + 7) & ~7;
  if ((uint64_t)page_size >= DBG_MAX_COUNT
      || (size_t)page_size > dat->size - dat->byte)
    {
      LOG_ERROR ("Invalid r2007 system page: page_size: %" PRId64, page_size);
      return false;
    }
  LOG_HANDLE ("read_system_page: size_comp: %" PRId64 ", size_uncomp: %" PRId64
              ", repeat_count: %" PRId64 "\n",
              size_comp, size_uncomp, repeat_count);
  assert ((uint64_t)size_comp < dat->size);
  assert ((uint64_t)size_uncomp < dat->size);
  assert ((uint64_t)repeat_count < DBG_MAX_COUNT);
  assert ((uint64_t)page_size < DBG_MAX_COUNT);
  bit_chain_init_dat (out, size_uncomp + page_size, dat);
  // data = (BITCODE_RC *)calloc (size_uncomp + page_size, 1);
  LOG_HANDLE ("Alloc system page of size %" PRId64 "\n",
              size_uncomp + page_size);
  assert (out->size == (size_t)(size_uncomp + page_size));
  if (!out->chain)
    {
      LOG_ERROR ("Out of memory");
      return false;
    }
  data_end = &out->chain[size_uncomp + page_size];

  rsdata = &out->chain[size_uncomp];
  bit_read_fixed (dat, rsdata, page_size);
  pedata_size = block_count * 239;
  pedata = decode_rs (rsdata, block_count, 239, page_size);
  if (!pedata)
    {
      bit_chain_free (out);
      return false;
    }

  if (size_comp < size_uncomp)
    error = decompress_r2007 (out->chain, size_uncomp, pedata,
                              MIN (pedata_size, size_comp), data_end);
  else
    {
      if (out->byte + size_uncomp <= out->size)
        memcpy (out->chain, pedata, size_uncomp);
      else
        {
          LOG_ERROR ("data overflow");
          error = DWG_ERR_CRITICAL;
        }
    }

  free (pedata);
  if (error >= DWG_ERR_CRITICAL)
    {
      bit_chain_free (out);
      return false;
    }
  return true;
}

static int
read_data_page (Bit_Chain *restrict dat, BITCODE_RC *restrict decomp,
                int64_t page_size, int64_t size_comp, int64_t size_uncomp,
                BITCODE_RC *restrict decomp_end)
{
  int error = 0;

  int64_t pesize;      // Pre RS encoded size
  int64_t block_count; // Number of RS encoded blocks

  BITCODE_RC *rsdata; // RS encoded data
  BITCODE_RC *pedata; // Pre RS encoded data
  long pedata_size;

  // Round to a multiple of 8
  pesize = ((size_comp + 7) & ~7);
  block_count = (pesize + 0xFB - 1) / 0xFB;
  pedata_size = block_count * 0xFB;

  rsdata = (BITCODE_RC *)calloc (1, page_size);
  if (rsdata == NULL)
    {
      LOG_ERROR ("Out of memory");
      return DWG_ERR_OUTOFMEM;
    }
  bit_read_fixed (dat, rsdata, page_size);
  pedata = decode_rs (rsdata, block_count, 0xFB, page_size);
  if (!pedata)
    {
      free (rsdata);
      return DWG_ERR_OUTOFMEM;
    }

  if (size_comp < size_uncomp)
    error = decompress_r2007 (decomp, size_uncomp, pedata,
                              MIN (pedata_size, size_comp), decomp_end);
  else
    {
      if (decomp + size_uncomp <= decomp_end)
        memcpy (decomp, pedata, size_uncomp);
      else
        {
          LOG_ERROR ("decomp overflow");
          free (pedata);
          return DWG_ERR_INTERNALERROR;
        }
    }

  free (pedata);
  free (rsdata);

  return error;
}

static int
read_data_section (Bit_Chain *sec_dat, Bit_Chain *dat,
                   r2007_section *restrict sections_map,
                   r2007_page *restrict pages_map, Dwg_Section_Type sec_type)
{
  r2007_section *section;
  r2007_page *page;
  uint64_t max_decomp_size;
  BITCODE_RC *decomp, *decomp_end;
  int error = 0, i;

  section = get_section (sections_map, sec_type);
  sec_dat->chain = NULL;
  if (section == NULL)
    {
      if (sec_type < SECTION_REVHISTORY && sec_type != SECTION_TEMPLATE
          && sec_type != SECTION_OBJFREESPACE)
        {
          LOG_WARN ("Failed to find section_info[%u]", (int)sec_type);
          return DWG_ERR_SECTIONNOTFOUND;
        }
      else
        {
          LOG_TRACE ("Found no section_info[%u]\n", (int)sec_type);
          return DWG_ERR_VALUEOUTOFBOUNDS;
        }
    }

  max_decomp_size = section->data_size;
  if (max_decomp_size > 0x2f000000) // 790Mb
    {
      LOG_ERROR ("Invalid max decompression size %" PRIu64, max_decomp_size);
      return DWG_ERR_INVALIDDWG;
    }
  decomp = (BITCODE_RC *)calloc (max_decomp_size, 1);
  if (decomp == NULL)
    {
      LOG_ERROR ("Out of memory");
      return DWG_ERR_OUTOFMEM;
    }
  decomp_end = &decomp[max_decomp_size];
  LOG_HANDLE ("Alloc data section of size %" PRIu64 "\n", max_decomp_size);

  // sec_dat->chain = decomp;
  sec_dat->bit = 0;
  sec_dat->byte = 0;
  sec_dat->size = max_decomp_size;
  sec_dat->version = dat->version;
  sec_dat->from_version = dat->from_version;

  for (i = 0; i < (int)section->num_pages; i++)
    {
      r2007_section_page *section_page = section->pages[i];
      if (!section_page)
        {
          free (decomp);
          LOG_ERROR ("Failed to find section page %d", (int)i);
          return DWG_ERR_PAGENOTFOUND;
        }
      page = get_page (pages_map, section_page->id);
      if (page == NULL)
        {
          free (decomp);
          LOG_ERROR ("Failed to find page %d", (int)section_page->id);
          return DWG_ERR_PAGENOTFOUND;
        }
      if (section_page->offset > max_decomp_size)
        {
          free (decomp);
          LOG_ERROR ("Invalid section_page->offset %ld > %ld",
                     (long)section_page->offset, (long)max_decomp_size);
          return DWG_ERR_VALUEOUTOFBOUNDS;
        }
      if (max_decomp_size < section_page->uncomp_size)
        {
          free (decomp);
          LOG_ERROR ("Invalid section size %ld < %ld", (long)max_decomp_size,
                     (long)section_page->uncomp_size);
          return DWG_ERR_VALUEOUTOFBOUNDS;
        }

      dat->byte = page->offset;
      // only if compressed. TODO: Isn't there a compressed flag as with 2004+?
      // theoretically the sizes could still be the same.
      if (section_page->comp_size != section_page->uncomp_size)
        {
          error = read_data_page (dat, &decomp[section_page->offset],
                                  page->size, section_page->comp_size,
                                  section_page->uncomp_size, decomp_end);
          if (error)
            {
              free (decomp);
              LOG_ERROR ("Failed to read compressed page");
              return error;
            }
        }
      else
        {
          if (section_page->uncomp_size
                  > max_decomp_size - section_page->offset
              || section_page->uncomp_size > dat->size - dat->byte)
            {
              free (decomp);
              LOG_ERROR ("Invalid section size %ld",
                         (long)section_page->uncomp_size);
              return DWG_ERR_VALUEOUTOFBOUNDS;
            }
          memcpy (&decomp[section_page->offset], &dat->chain[dat->byte],
                  section_page->uncomp_size);
        }
    }
  sec_dat->chain = decomp;
  return 0;
}

#define LOG_POS_DAT(dat)                                                      \
  LOG_INSANE (" @%" PRIuSIZE ".%u", (dat)->byte, (dat)->bit);                 \
  LOG_TRACE ("\n");

static r2007_section *
read_sections_map (Bit_Chain *dat, int64_t size_comp, int64_t size_uncomp,
                   int64_t correction)
{
  r2007_section *sections = NULL, *last_section = NULL, *section = NULL;
  Bit_Chain page = { 0 };
  int i, j = 0;

  if (!read_system_page (&page, dat, size_comp, size_uncomp, correction))
    {
      LOG_ERROR ("Failed to read system page");
      return NULL;
    }

  LOG_TRACE ("\n=== System Section (Section Map) ===\n");
  while (page.byte < (size_t)size_uncomp)
    {
      section = (r2007_section *)calloc (1, sizeof (r2007_section));
      if (!section)
        {
          LOG_ERROR ("Out of memory");
          bit_chain_free (&page);
          sections_destroy (sections); // the root
          return NULL;
        }
      LOG_TRACE ("\nSection [%d]:\n", j);
      section->data_size = (int64_t)bit_read_RLL (&page);
      section->max_size = (int64_t)bit_read_RLL (&page);
      section->encrypted = (int64_t)bit_read_RLL (&page);
      section->hashcode = (int64_t)bit_read_RLL (&page);
      section->name_length = (int64_t)bit_read_RLL (&page);
      section->unknown = (int64_t)bit_read_RLL (&page);
      section->encoded = (int64_t)bit_read_RLL (&page);
      section->num_pages = (int64_t)bit_read_RLL (&page);
      LOG_TRACE ("  data size:     %" PRId64 "\n", section->data_size);
      LOG_TRACE ("  max size:      %" PRId64 "\n", section->max_size);
      LOG_TRACE ("  encryption:    %" PRId64 "\n", section->encrypted);
      LOG_HANDLE ("  hashcode:      %" PRIx64 "\n", section->hashcode);
      LOG_HANDLE ("  name length:   %" PRId64 "\n", section->name_length);
      LOG_TRACE ("  unknown:       %" PRId64 "\n", section->unknown);
      LOG_TRACE ("  encoding:      %" PRId64 "\n", section->encoded);
      LOG_TRACE ("  num pages:     %" PRId64, section->num_pages);
      LOG_POS_DAT (&page);
      // debugging sanity
#if 1
      /* compressed */
      if (section->data_size > 10 * dat->size
          || section->name_length >= (int64_t)dat->size
          || section->name_length >= 48)
        {
          LOG_ERROR ("Invalid System Section");
          free (section);
          bit_chain_free (&page);
          sections_destroy (sections); // the root
          return NULL;
        }
        // assert(section->data_size < dat->size + 0x100000);
        // assert(section->max_size  < dat->size + 0x100000);
        // assert(section->num_pages < DBG_MAX_COUNT);
#endif
      // section->next = NULL;
      // section->pages = NULL;
      // section->name = NULL;

      if (!sections)
        {
          sections = last_section = section;
        }
      else
        {
          last_section->next = section;
          last_section = section;
        }

      j++;
      if (page.byte >= page.size)
        break;

      if (section->name_length < 0L)
        {
          LOG_ERROR ("Invalid section name_length");
          bit_chain_free (&page);
          if (sections)
            sections_destroy (sections); // the root
          else
            sections_destroy (section);
          return NULL;
        }
      // Section Name (wchar)
      {
        size_t sz = (size_t)section->name_length; // size in bytes really
        size_t page_sz = page.size - page.byte;
        if (sz & 1) // must be even, 2 bytes
          {
            LOG_ERROR ("Invalid section name_length %" PRId64,
                       section->name_length);
            section->name_length++;
            sz++;
          }
        if (sz > MAX_SIZE_T)
          {
            LOG_ERROR ("Invalid section name_length %zu > %u", sz, MAX_SIZE_T);
            sz = MAX_SIZE_T;
          }
        if (sz > page_sz)
          {
            LOG_ERROR ("Invalid section name_length %zu > %zu", sz, page_sz);
            sz = page_sz;
          }
        section->name
            = (DWGCHAR *)calloc (1, section->name_length > 0 ? sz + 2 : 2);
        bit_read_fixed (&page, (BITCODE_RC *)section->name, sz);
      }
#ifdef HAVE_NATIVE_WCHAR2
      LOG_TRACE ("  name:          " FORMAT_TU, (BITCODE_TU)section->name);
#else
      LOG_TRACE ("  name:          ");
      LOG_TEXT_UNICODE (TRACE, section->name);
#endif
      LOG_POS_DAT (&page);
      LOG_TRACE ("\n");
      section->type = dwg_section_wtype (section->name);

      if (section->num_pages <= 0 || section->num_pages > 0xf0000)
        {
          LOG_ERROR ("Invalid num_pages %" PRIuSIZE ", skip",
                     (size_t)section->num_pages);
          section->num_pages = 0;
          continue;
        }

      section->pages = (r2007_section_page **)calloc (
          (size_t)section->num_pages, sizeof (r2007_section_page *));
      if (!section->pages)
        {
          LOG_ERROR ("Out of memory");
          bit_chain_free (&page);
          if (sections)
            sections_destroy (sections); // the root
          else
            sections_destroy (section);
          return NULL;
        }

      for (i = 0; i < section->num_pages; i++)
        {
          section->pages[i]
              = (r2007_section_page *)calloc (1, sizeof (r2007_section_page));
          if (!section->pages[i])
            {
              LOG_ERROR ("Out of memory");
              bit_chain_free (&page);
              if (sections)
                sections_destroy (sections); // the root
              else
                sections_destroy (section);
              return NULL;
            }

          if (page.byte + 56 > page.size)
            {
              LOG_ERROR ("Section[%d]->pages[%d] overflow (%" PRIuSIZE
                         " > %" PRIuSIZE ")",
                         j, i, page.byte + 56, page.size);
              free (section->pages[i]);
              section->num_pages = i; // skip this last section
              break;
            }

          section->pages[i]->offset = bit_read_RLL (&page);
          section->pages[i]->size = bit_read_RLL (&page);
          section->pages[i]->id = (int64_t)bit_read_RLL (&page);
          section->pages[i]->uncomp_size = bit_read_RLL (&page);
          section->pages[i]->comp_size = bit_read_RLL (&page);
          section->pages[i]->checksum = bit_read_RLL (&page);
          section->pages[i]->crc = bit_read_RLL (&page);

          LOG_TRACE (" Page[%d]: ", i);
          LOG_TRACE (" offset: 0x%07" PRIx64, section->pages[i]->offset);
          LOG_TRACE (" size: %5" PRIu64, section->pages[i]->size);
          LOG_TRACE (" id: %4" PRId64, section->pages[i]->id);
          LOG_TRACE (" uncomp_size: %5" PRIu64 "\n",
                     section->pages[i]->uncomp_size);
          LOG_HANDLE (" comp_size: %5" PRIu64, section->pages[i]->comp_size);
          LOG_HANDLE (" checksum: %016" PRIx64, section->pages[i]->checksum);
          LOG_HANDLE (" crc64: %016" PRIx64 "\n", section->pages[i]->crc);
          // debugging sanity
          if (section->pages[i]->size >= DBG_MAX_SIZE
              || section->pages[i]->uncomp_size >= DBG_MAX_SIZE
              || section->pages[i]->comp_size >= DBG_MAX_SIZE)
            {
              LOG_ERROR ("Invalid section->pages[%d] size", i);
              bit_chain_free (&page);
              free (section->pages[i]);
              section->num_pages = i; // skip this last section
              return sections;
            }
          assert (section->pages[i]->size < DBG_MAX_SIZE);
          assert (section->pages[i]->uncomp_size < DBG_MAX_SIZE);
          assert (section->pages[i]->comp_size < DBG_MAX_SIZE);
        }
    }

  bit_chain_free (&page);
  return sections;
}

static r2007_page *
read_pages_map (Bit_Chain *dat, int64_t size_comp, int64_t size_uncomp,
                int64_t correction)
{
  r2007_page *pages = NULL, *last_page = NULL, *page;
  int64_t offset = 0x480; // dat->byte;
  Bit_Chain sdat = { 0 };
  // int64_t index;

  if (!read_system_page (&sdat, dat, size_comp, size_uncomp, correction))
    {
      LOG_ERROR ("Failed to read system page");
      return NULL;
    }
  LOG_TRACE ("\n=== System Section (Pages Map) ===\n");
  while (sdat.byte < (size_t)size_uncomp)
    {
      page = (r2007_page *)malloc (sizeof (r2007_page));
      if (page == NULL)
        {
          LOG_ERROR ("Out of memory");
          bit_chain_free (&sdat);
          pages_destroy (pages);
          return NULL;
        }
      if (sdat.byte + 16 > sdat.size)
        {
          LOG_ERROR ("Page out of bounds");
          bit_chain_free (&sdat);
          pages_destroy (pages);
          return NULL;
        }
      page->size = bit_read_RLL (&sdat);
      page->id = (int64_t)bit_read_RLL (&sdat);
      page->offset = offset;
      offset += page->size;

      // index = page->id > 0 ? page->id : -page->id;
      LOG_TRACE ("Page [%3" PRId64 "]: ", page->id);
      LOG_TRACE ("size: %6" PRIu64 " ", page->size);
      LOG_TRACE ("offset: 0x%" PRIx64 " \n", page->offset);

      page->next = NULL;

      if (pages == NULL)
        pages = last_page = page;
      else
        {
          last_page->next = page;
          last_page = page;
        }
    }
  bit_chain_free (&sdat);
  return pages;
}

/* Lookup a page in the page map. The page is identified by its id.
 */
static r2007_page *
get_page (r2007_page *pages_map, int64_t id)
{
  r2007_page *page = pages_map;

  while (page != NULL)
    {
      if (page->id == id)
        break;
      page = page->next;
    }

  return page;
}

static void
pages_destroy (r2007_page *page)
{
  r2007_page *next;

  while (page != 0)
    {
      next = page->next;
      free (page);
      page = next;
    }
}

/* Lookup a section in the section map.
 * The section is identified by its numeric type.
 */
static r2007_section *
get_section (r2007_section *sections_map, Dwg_Section_Type sec_type)
{
  r2007_section *section = sections_map;
  while (section != NULL)
    {
      if (section->type == sec_type)
        break;
      section = section->next;
    }

  return section;
}

static void
sections_destroy (r2007_section *section)
{
  r2007_section *next;

  while (section != 0)
    {
      next = section->next;

      if (section->pages)
        {
          while (section->num_pages-- > 0)
            {
              free (section->pages[section->num_pages]);
            }
          free (section->pages);
        }

      if (section->name)
        free (section->name);

      free (section);
      section = next;
    }
}

static int
read_file_header (Bit_Chain *restrict dat,
                  Dwg_R2007_Header *restrict file_header)
{
  BITCODE_RC data[0x3d8]; // 0x400 - 5 long
  BITCODE_RC *pedata;
  uint64_t seqence_crc;
  uint64_t seqence_key;
  uint64_t compr_crc;
  int32_t compr_len, len2;
  int error = 0;
  const int pedata_size = 3 * 239; // size of pedata

  dat->byte = 0x80;
  LOG_TRACE ("\n=== r2007 File header ===\n");
  memset (file_header, 0, sizeof (Dwg_R2007_Header));
  memset (data, 0, 0x3d8);
  bit_read_fixed (dat, data, 0x3d8);
  pedata = decode_rs (data, 3, 239, 0x3d8);
  if (!pedata)
    return DWG_ERR_OUTOFMEM;

  seqence_crc = le64toh (*((uint64_t *)pedata));
  seqence_key = le64toh (*((uint64_t *)&pedata[8]));
  compr_crc = le64toh (*((uint64_t *)&pedata[16]));
  compr_len = le32toh (*((int32_t *)&pedata[24]));
  len2 = le32toh (*((int32_t *)&pedata[28]));
  LOG_TRACE ("seqence_crc64: %016" PRIX64 "\n", seqence_crc);
  LOG_TRACE ("seqence_key:   %016" PRIX64 "\n", seqence_key);
  LOG_TRACE ("compr_crc64:   %016" PRIX64 "\n", compr_crc);
  LOG_TRACE ("compr_len:     %d\n", (int)compr_len); // only this is used
  LOG_TRACE ("len2:          %d\n", (int)len2);      // 0 when compressed

  if (compr_len > 0)
    error = decompress_r2007 ((BITCODE_RC *)file_header,
                              sizeof (Dwg_R2007_Header), &pedata[32],
                              MIN (compr_len, pedata_size - 32), NULL);
  else
    memcpy (file_header, &pedata[32], sizeof (Dwg_R2007_Header));

#ifdef WORDS_BIGENDIAN
  {
    uint64_t *fields = (uint64_t *)file_header;
    for (unsigned j = 0; j < sizeof (Dwg_R2007_Header) / 8; j++)
      {
        fields[j] = le64toh (fields[j]);
      }
  }
#endif

  // check validity, for debugging only
  if (!error)
    {

#define VALID_SIZE(var)                                                       \
  if (var < 0 || (unsigned)var > dat->size)                                   \
    {                                                                         \
      error |= DWG_ERR_VALUEOUTOFBOUNDS;                                      \
      LOG_ERROR ("%s Invalid %s %ld > MAX_SIZE", __FUNCTION__, #var,          \
                 (long)var);                                                  \
      var = 0;                                                                \
    }
#define VALID_COUNT(var)                                                      \
  if (var < 0 || (unsigned)var > dat->size)                                   \
    {                                                                         \
      error |= DWG_ERR_VALUEOUTOFBOUNDS;                                      \
      LOG_ERROR ("%s Invalid %s %ld > MAX_COUNT", __FUNCTION__, #var,         \
                 (long)var);                                                  \
      var = 0;                                                                \
    }

      VALID_SIZE (file_header->header_size);
      VALID_SIZE (file_header->file_size);
      VALID_SIZE (file_header->pages_map_offset);
      VALID_SIZE (file_header->header2_offset);
      VALID_SIZE (file_header->pages_map_offset);
      VALID_SIZE (file_header->pages_map_size_comp);
      VALID_SIZE (file_header->pages_map_size_uncomp);
      VALID_COUNT (file_header->pages_maxid);
      VALID_COUNT (file_header->pages_amount);
      VALID_COUNT (file_header->num_sections);
    }

  free (pedata);
  return error;
}

/* Return the latest dat position for all three independent streams
   data, handle and string.
*/
size_t
obj_stream_position (Bit_Chain *restrict dat, Bit_Chain *restrict hdl_dat,
                     Bit_Chain *restrict str_dat)
{
  size_t p1 = bit_position (dat);
  /* all 3 now relative to obj */
  size_t p2 = bit_position (hdl_dat);
  SINCE (R_2007a)
  { // but only since 2007 there is a separate string stream
    size_t p3 = bit_position (str_dat);
    if (p2 > p1)
      return p3 > p2 ? p3 : p2;
    else
      return p3 > p1 ? p3 : p1;
  }
  else
  {
    return (p2 > p1) ? p2 : p1;
  }
}

int
obj_string_stream (Bit_Chain *restrict dat, Dwg_Object *restrict obj,
                   Bit_Chain *restrict str)
{
  BITCODE_RL data_size = 0;            // in byte
  BITCODE_RL start = obj->bitsize - 1; // in bits
  size_t old_size;                     // in byte
  size_t old_byte;
  // assert (dat != str); // r2007 objects are the same, just entities not
  old_size = str->size;
  old_byte = str->byte;

  // str->chain += str->byte;
  // obj->strpos = str->byte * 8 + str->bit;

  str->size = (obj->bitsize / 8) + ((obj->bitsize % 8) ? 1 : 0);
  bit_set_position (str, start);

  if (str->byte > old_size - old_byte)
    {
      LOG_ERROR ("obj_string_stream overflow, bitsize " FORMAT_RL
                 " => " FORMAT_RL " (strpos %" PRIuSIZE " > diff %" PRIuSIZE
                 ")",
                 obj->bitsize, obj->size * 8, str->byte, old_size - old_byte);
      str->byte = old_byte;
      str->size = old_size;
      obj->has_strings = 0;
      obj->bitsize = obj->size * 8;
      return DWG_ERR_VALUEOUTOFBOUNDS;
    }
  LOG_HANDLE (" obj string stream +" FORMAT_RL ": @%" PRIuSIZE
              ".%u (%" PRIuSIZE ")",
              start, str->byte, str->bit & 7, bit_position (str));
  obj->has_strings = bit_read_B (str);
  LOG_TRACE (" has_strings: %d\n", (int)obj->has_strings);
  if (!obj->has_strings)
    {
      // FIXME wrong bit
      if (obj->fixedtype == DWG_TYPE_SCALE)
        obj->has_strings = 1;
      // str->size = 0;
      // bit_reset_chain (str);
      return 0;
    }

  bit_advance_position (str, -1); //-17
  str->byte -= 2;
  LOG_HANDLE (" @%" PRIuSIZE ".%u", str->byte, str->bit & 7);
  data_size = (BITCODE_RL)bit_read_RS (str);
  LOG_HANDLE (" data_size: %u/0x%x [RS]", (unsigned)data_size,
              (unsigned)data_size);

  if (data_size & 0x8000)
    {
      BITCODE_RS hi_size;
      str->byte -= 4;
      data_size &= 0x7FFF;
      LOG_HANDLE (" @%" PRIuSIZE ".%u", str->byte, str->bit & 7);
      hi_size = bit_read_RS (str);
      LOG_HANDLE (" hi_size " FORMAT_RS "/" FORMAT_RSx " [RS]", hi_size,
                  hi_size);
      data_size |= (hi_size << 15);
      LOG_HANDLE (" => data_size: %u/0x%x\n", (unsigned)data_size,
                  (unsigned)data_size);
      // LOG_TRACE("  -33: @%" PRIuSIZE "\n", str->byte);
    }
  else
    LOG_HANDLE ("\n");
  str->byte -= 2;
  if (data_size > obj->bitsize)
    {
      LOG_WARN (
          "Invalid string stream data_size %u > bitsize %u at @%" PRIuSIZE
          ".%u\n",
          (unsigned)data_size, (unsigned)obj->bitsize, str->byte,
          str->bit & 7);
      if (dat->from_version == R_2007)
        {
          return 0;
        }
      obj->has_strings = 0;
      bit_reset_chain (str);
      return DWG_ERR_NOTYETSUPPORTED; // a very low severity error
    }
  if (data_size < obj->bitsize)
    {
      obj->stringstream_size = data_size;
      bit_advance_position (str, -(int)data_size);
    }
  else
    {
      bit_set_position (str, 0);
    }
  // bit_reset_chain (str);
  // LOG_TRACE(" %d: @%" PRIuSIZE ".%u (%" PRIuSIZE ")\n", -(int)data_size -
  // 16, str->byte, str->bit & 7,
  //          bit_position(str));
  // obj->strpos = obj->bitsize_pos + obj->bitsize - obj->stringstream_size;
  return 0;
}

void
section_string_stream (Dwg_Data *restrict dwg, Bit_Chain *restrict dat,
                       BITCODE_RL bitsize, Bit_Chain *restrict str)
{
  BITCODE_RL start;     // in bits
  BITCODE_RL data_size; // in bits
  BITCODE_B endbit;
  if (dwg->header.version < R_2010
      || (dwg->header.version == R_2010 && dwg->header.maint_version < 3))
    {
      // r2007: + 24 bytes (sentinel+size+hsize) - 1 bit (endbit)
      start = bitsize + 159;
    }
  else
    {
      // r2010: + 24 bytes (sentinel+size+hSize) - 1 bit (endbit)
      start = bitsize + 191; /* 8*24 = 192 */
    }
  *str = *dat;
  bit_set_position (str, start);
  LOG_TRACE ("section string stream\n  pos: " FORMAT_RL ", %" PRIuSIZE "/%u\n",
             start, str->byte, str->bit);
  endbit = bit_read_B (str);
  LOG_HANDLE ("  endbit: %d\n", (int)endbit);
  if (!endbit)
    return; // i.e. has no strings. without data_size should be 0
  start -= 16;
  bit_set_position (str, start);
  LOG_HANDLE ("  pos: " FORMAT_RL ", %" PRIuSIZE "\n", start, str->byte);
  // str->bit = start & 7;
  data_size = bit_read_RS (str);
  LOG_HANDLE ("  data_size: " FORMAT_RL "\n", data_size);
  if (data_size & 0x8000)
    {
      BITCODE_RS hi_size;
      start -= 16;
      data_size &= 0x7FFF;
      bit_set_position (str, start);
      LOG_HANDLE ("  pos: " FORMAT_RL ", %" PRIuSIZE "\n", start, str->byte);
      hi_size = bit_read_RS (str);
      data_size |= (hi_size << 15);
      LOG_HANDLE ("  hi_size: " FORMAT_RS ", data_size: " FORMAT_RL "\n",
                  hi_size, data_size);
    }
  start -= data_size;
  bit_set_position (str, start);
  LOG_HANDLE ("  pos: " FORMAT_RL ", %" PRIuSIZE "/%u\n", start, str->byte,
              str->bit);
}

// for string stream see p86
static int
read_2007_section_classes (Bit_Chain *restrict dat, Dwg_Data *restrict dwg,
                           r2007_section *restrict sections_map,
                           r2007_page *restrict pages_map)
{
  BITCODE_RL size, i;
  BITCODE_BS max_num;
  Bit_Chain sec_dat = { 0 }, str = { 0 };
  Dwg_Object *obj = NULL;
  int error;
  char c;

  error = read_data_section (&sec_dat, dat, sections_map, pages_map,
                             SECTION_CLASSES);
  if (error)
    {
      LOG_ERROR ("Failed to read class section");
      if (sec_dat.chain)
        free (sec_dat.chain);
      return error;
    }

  if (bit_search_sentinel (&sec_dat, dwg_sentinel (DWG_SENTINEL_CLASS_BEGIN)))
    {
      BITCODE_RL bitsize = 0;
      LOG_TRACE ("\nClasses\n-------------------\n");
      size = bit_read_RL (&sec_dat); // size of class data area
      LOG_TRACE ("size: " FORMAT_RL " [RL]\n", size);
      /*
      if (dat->from_version >= R_2010 && dwg->header.maint_version > 3)
        {
          BITCODE_RL hsize = bit_read_RL(&sec_dat);
          LOG_TRACE("hsize: " FORMAT_RL " [RL]\n", hsize);
        }
      */
      if (dat->from_version >= R_2007)
        {
          bitsize = bit_read_RL (&sec_dat);
          LOG_TRACE ("bitsize: " FORMAT_RL " [RL]\n", bitsize);
        }
      max_num = bit_read_BS (&sec_dat); // Maximum class number
      LOG_TRACE ("max_num: " FORMAT_BS " [BS]\n", max_num);
      c = bit_read_RC (&sec_dat); // 0x00
      LOG_HANDLE ("c: " FORMAT_RC " [RC]\n", c);
      c = bit_read_RC (&sec_dat); // 0x00
      LOG_HANDLE ("c: " FORMAT_RC " [RC]\n", c);
      c = bit_read_B (&sec_dat); // 1
      LOG_HANDLE ("c: " FORMAT_B " [B]\n", c);

      dwg->layout_type = 0;
      dwg->num_classes = max_num - 499;
      if (max_num < 500 || max_num > 5000)
        {
          LOG_ERROR ("Invalid max class number %d", max_num);
          dwg->num_classes = 0;
          if (sec_dat.chain)
            free (sec_dat.chain);
          return DWG_ERR_VALUEOUTOFBOUNDS;
        }
      assert (max_num >= 500);
      assert (max_num < 5000);

      section_string_stream (dwg, &sec_dat, bitsize, &str);

      dwg->dwg_class
          = (Dwg_Class *)calloc (dwg->num_classes, sizeof (Dwg_Class));
      if (!dwg->dwg_class)
        {
          LOG_ERROR ("Out of memory");
          if (sec_dat.chain)
            free (sec_dat.chain);
          return DWG_ERR_OUTOFMEM;
        }

      for (i = 0; i < dwg->num_classes; i++)
        {
          dwg->dwg_class[i].number = bit_read_BS (&sec_dat);
          dwg->dwg_class[i].proxyflag = bit_read_BS (&sec_dat);
          dwg->dwg_class[i].appname = (char *)bit_read_TU (&str);
          dwg->dwg_class[i].cppname = (char *)bit_read_TU (&str);
          dwg->dwg_class[i].dxfname_u = bit_read_TU (&str);
          dwg->dwg_class[i].is_zombie = bit_read_B (&sec_dat); // DXF 280
          // DXF 281, is_entity is 0x1f3
          dwg->dwg_class[i].item_class_id = bit_read_BS (&sec_dat);

          dwg->dwg_class[i].num_instances = bit_read_BL (&sec_dat); // DXF 91
          dwg->dwg_class[i].dwg_version = bit_read_BS (&sec_dat);
          dwg->dwg_class[i].maint_version = bit_read_BS (&sec_dat);
          dwg->dwg_class[i].unknown_1 = bit_read_BL (&sec_dat); // 0
          dwg->dwg_class[i].unknown_2 = bit_read_BL (&sec_dat); // 0

          LOG_TRACE ("-------------------\n");
          LOG_TRACE ("Number:           %d\n", dwg->dwg_class[i].number);
          LOG_TRACE ("Proxyflag:        0x%x\n", dwg->dwg_class[i].proxyflag);
          dwg_log_proxyflag (DWG_LOGLEVEL, DWG_LOGLEVEL_TRACE,
                             dwg->dwg_class[i].proxyflag);
          LOG_TRACE_TU ("Application name", dwg->dwg_class[i].appname, 0);
          LOG_TRACE_TU ("C++ class name  ", dwg->dwg_class[i].cppname, 0);
          LOG_TRACE_TU ("DXF record name ", dwg->dwg_class[i].dxfname_u, 0);
          LOG_TRACE ("Class ID:         0x%x "
                     "(0x1f3 for object, 0x1f2 for entity)\n",
                     dwg->dwg_class[i].item_class_id);
          LOG_TRACE ("instance count:   %u\n",
                     dwg->dwg_class[i].num_instances);
          LOG_TRACE ("dwg version:      %u (%u)\n",
                     dwg->dwg_class[i].dwg_version,
                     dwg->dwg_class[i].maint_version);
          LOG_HANDLE ("unknown:          %u %u\n", dwg->dwg_class[i].unknown_1,
                      dwg->dwg_class[i].unknown_2);

          dwg->dwg_class[i].dxfname
              = bit_convert_TU (dwg->dwg_class[i].dxfname_u);
          if (dwg->dwg_class[i].dxfname
              && strEQc (dwg->dwg_class[i].dxfname, "LAYOUT"))
            dwg->layout_type = dwg->dwg_class[i].number;
        }
    }
  else
    {
      LOG_ERROR ("Failed to find class section sentinel");
      free (sec_dat.chain);
      return DWG_ERR_CLASSESNOTFOUND;
    }

  if (sec_dat.chain)
    free (sec_dat.chain);

  return 0;
}

static int
read_2007_section_header (Bit_Chain *restrict dat, Bit_Chain *restrict hdl_dat,
                          Dwg_Data *restrict dwg,
                          r2007_section *restrict sections_map,
                          r2007_page *restrict pages_map)
{
  Bit_Chain sec_dat = { 0 }, str_dat = { 0 };
  int error;
  LOG_TRACE ("\nSection Header\n-------------------\n");
  error = read_data_section (&sec_dat, dat, sections_map, pages_map,
                             SECTION_HEADER);
  if (error)
    {
      LOG_ERROR ("Failed to read header section");
      if (sec_dat.chain)
        free (sec_dat.chain);
      return error;
    }
  if (bit_search_sentinel (&sec_dat,
                           dwg_sentinel (DWG_SENTINEL_VARIABLE_BEGIN)))
    {
      BITCODE_RL endbits = 160; // start bit: 16 sentinel + 4 size
      dwg->header_vars.size = bit_read_RL (&sec_dat);
      LOG_TRACE ("size: " FORMAT_RL "\n", dwg->header_vars.size);
      *hdl_dat = sec_dat;
      // unused: later versions re-use the 2004 section format
      /*
      if (dat->from_version >= R_2010 && dwg->header.maint_version > 3)
        {
          dwg->header_vars.bitsize_hi = bit_read_RL(&sec_dat);
          LOG_TRACE("bitsize_hi: " FORMAT_RL " [RL]\n",
                     dwg->header_vars.bitsize_hi);
          endbits += 32;
        }
      */
      if (dat->from_version == R_2007) // always true so far
        {
          dwg->header_vars.bitsize = bit_read_RL (&sec_dat);
          LOG_TRACE ("bitsize: " FORMAT_RL " [RL]\n",
                     dwg->header_vars.bitsize);
          endbits += dwg->header_vars.bitsize;
          bit_set_position (hdl_dat, endbits);
          section_string_stream (dwg, &sec_dat, dwg->header_vars.bitsize,
                                 &str_dat);
        }

      dwg_decode_header_variables (&sec_dat, hdl_dat, &str_dat, dwg);
    }
  else
    {
      DEBUG_HERE;
      error = DWG_ERR_SECTIONNOTFOUND;
    }

  if (sec_dat.chain)
    free (sec_dat.chain);

  return error;
}

typedef struct _r2007_handle_vec
{
  BITCODE_RLL *items;
  size_t num;
  size_t cap;
} r2007_handle_vec;

typedef struct _r2007_probe
{
  size_t address;
  size_t body_address;
  BITCODE_MS size;
  BITCODE_BS type;
  BITCODE_RL bitsize;
  BITCODE_RLL handle;
} r2007_probe;

static void
r2007_handle_vec_free (r2007_handle_vec *vec)
{
  free (vec->items);
  memset (vec, 0, sizeof (*vec));
}

static int
r2007_handle_vec_contains (const r2007_handle_vec *vec, BITCODE_RLL handle)
{
  size_t i;
  for (i = 0; i < vec->num; i++)
    if (vec->items[i] == handle)
      return 1;
  return 0;
}

static int
r2007_handle_vec_add (r2007_handle_vec *vec, BITCODE_RLL handle)
{
  if (!handle || r2007_handle_vec_contains (vec, handle))
    return 0;
  if (vec->num == vec->cap)
    {
      size_t new_cap = vec->cap ? vec->cap * 2 : 64;
      BITCODE_RLL *items = (BITCODE_RLL *)realloc (
          vec->items, new_cap * sizeof (BITCODE_RLL));
      if (!items)
        return DWG_ERR_OUTOFMEM;
      vec->items = items;
      vec->cap = new_cap;
    }
  vec->items[vec->num++] = handle;
  return 0;
}

static int
r2007_ref_abs (const Dwg_Object_Ref *ref, BITCODE_RLL *absref)
{
  if (!ref || !absref)
    return 0;
  *absref = ref->absolute_ref ? ref->absolute_ref : ref->handleref.value;
  return *absref != 0;
}

static int
r2007_object_has_handle (const Dwg_Data *dwg, BITCODE_RLL handle)
{
  Dwg_Object *obj = handle ? dwg_resolve_handle_silent (dwg, handle) : NULL;
  return obj != NULL;
}

static int
r2007_collect_unresolved_layer_handles (Dwg_Data *restrict dwg,
                                        r2007_handle_vec *restrict targets)
{
  Dwg_Object *ctrl = dwg_get_first_object (dwg, DWG_TYPE_LAYER_CONTROL);
  BITCODE_RLL absref;
  int error = 0;

  if (ctrl && ctrl->tio.object && ctrl->tio.object->tio.LAYER_CONTROL)
    {
      Dwg_Object_LAYER_CONTROL *layer_ctrl
          = ctrl->tio.object->tio.LAYER_CONTROL;
      BITCODE_BS i;
      for (i = 0; layer_ctrl->entries && i < layer_ctrl->num_entries; i++)
        {
          if (r2007_ref_abs (layer_ctrl->entries[i], &absref)
              && !r2007_object_has_handle (dwg, absref))
            error |= r2007_handle_vec_add (targets, absref);
        }
    }

  if (r2007_ref_abs (dwg->header_vars.CLAYER, &absref)
      && !r2007_object_has_handle (dwg, absref))
    error |= r2007_handle_vec_add (targets, absref);

  for (BITCODE_BL i = 0; i < dwg->num_objects; i++)
    {
      Dwg_Object *obj = &dwg->object[i];
      if (obj->supertype == DWG_SUPERTYPE_ENTITY && obj->tio.entity
          && r2007_ref_abs (obj->tio.entity->layer, &absref)
          && !r2007_object_has_handle (dwg, absref))
        error |= r2007_handle_vec_add (targets, absref);
    }

  return error;
}

static int
r2007_fixed_type_may_have_text (BITCODE_BS type)
{
  switch (type)
    {
    case DWG_TYPE_TEXT:
    case DWG_TYPE_ATTRIB:
    case DWG_TYPE_ATTDEF:
    case DWG_TYPE_MTEXT:
    case DWG_TYPE_LEADER:
    case DWG_TYPE_TOLERANCE:
    case DWG_TYPE_DIMENSION_ORDINATE:
    case DWG_TYPE_DIMENSION_LINEAR:
    case DWG_TYPE_DIMENSION_ALIGNED:
    case DWG_TYPE_DIMENSION_ANG3PT:
    case DWG_TYPE_DIMENSION_ANG2LN:
    case DWG_TYPE_DIMENSION_RADIUS:
    case DWG_TYPE_DIMENSION_DIAMETER:
      return 1;
    default:
      return 0;
    }
}

static int
r2007_class_name_may_have_text (const Dwg_Data *dwg, BITCODE_BS type)
{
  Dwg_Class *klass;
  const char *name;
  int i;

  if (type < 500)
    return 0;
  i = type - 500;
  if (i < 0 || i >= (int)dwg->num_classes)
    return 0;
  klass = &dwg->dwg_class[i];
  name = klass->dxfname ? klass->dxfname : klass->cppname;
  if (!name)
    return 0;
  return strstr (name, "TEXT") || strstr (name, "Text")
         || strstr (name, "LABEL") || strstr (name, "Label")
         || strstr (name, "DIMENSION") || strstr (name, "Dimension")
         || strstr (name, "LEADER") || strstr (name, "Leader")
         || strstr (name, "TABLE") || strstr (name, "Table");
}

static int
r2007_probe_type_is_candidate (const Dwg_Data *dwg, BITCODE_BS type)
{
  return type == DWG_TYPE_LAYER || r2007_fixed_type_may_have_text (type)
         || r2007_class_name_may_have_text (dwg, type);
}

static int
r2007_probe_type_is_plausible (const Dwg_Data *dwg, BITCODE_BS type)
{
  if (type > 0 && type <= DWG_TYPE_LAYOUT)
    return 1;
  if (type == DWG_TYPE_PROXY_ENTITY || type == DWG_TYPE_PROXY_OBJECT)
    return 1;
  if (type >= 500 && type - 500 < dwg->num_classes)
    return 1;
  return 0;
}

static BITCODE_MS
r2007_read_ms_at (const BITCODE_RC *chain, size_t size, size_t address,
                  size_t *body_address)
{
  BITCODE_RS lo;
  BITCODE_RS hi;

  if (address + 2 > size)
    return 0;
  lo = (BITCODE_RS)(chain[address] | (chain[address + 1] << 8));
  if (!(lo & 0x8000))
    {
      *body_address = address + 2;
      return lo;
    }
  if (address + 4 > size)
    return 0;
  hi = (BITCODE_RS)(chain[address + 2] | (chain[address + 3] << 8));
  if (hi & 0x8000)
    return 0;
  *body_address = address + 4;
  return (BITCODE_MS)((lo & 0x7fff) | ((BITCODE_MS)hi << 15));
}

static int
r2007_probe_object (Dwg_Data *restrict dwg, Bit_Chain *restrict obj_dat,
                    size_t address, r2007_probe *restrict probe)
{
  Bit_Chain dat;
  Dwg_Handle handle = { 0 };
  size_t body_address = 0;
  size_t end_address;
  BITCODE_MS size;
  BITCODE_BS type;
  BITCODE_RL bitsize;

  if (address + 12 >= obj_dat->size)
    return 0;

  size = r2007_read_ms_at (obj_dat->chain, obj_dat->size, address,
                           &body_address);
  if (size < 10 || body_address + size + 2 > obj_dat->size)
    return 0;

  end_address = body_address + size;
  if (end_address <= address + 2)
    return 0;

  dat = *obj_dat;
  dat.byte = body_address;
  dat.bit = 0;
  bit_reset_chain (&dat);
  dat.size = size;
  type = bit_read_BS (&dat);
  if (!r2007_probe_type_is_plausible (dwg, type))
    return 0;

  bitsize = bit_read_RL (&dat);
  if (bitsize > size * 8 || bitsize < bit_position (&dat))
    return 0;

  if (bit_read_H (&dat, &handle) || !handle.value || !handle.size)
    return 0;

  probe->address = address;
  probe->body_address = body_address;
  probe->size = size;
  probe->type = type;
  probe->bitsize = bitsize;
  probe->handle = handle.value;
  return 1;
}

static BITCODE_RLL
r2007_max_reasonable_handle (Dwg_Data *restrict dwg)
{
  BITCODE_RLL handseed = dwg->header_vars.HANDSEED
                             ? dwg->header_vars.HANDSEED->absolute_ref
                             : 0;
  BITCODE_RLL max_handle = 0;

  for (BITCODE_BL i = 0; i < dwg->num_objects; i++)
    {
      BITCODE_RLL h = dwg->object[i].handle.value;
      if (!h)
        continue;
      if (handseed && h > handseed)
        continue;
      if (h > max_handle)
        max_handle = h;
    }
  return max_handle;
}

static size_t
r2007_handle_pattern (BITCODE_RLL handle, unsigned code, BITCODE_RC pat[9])
{
  unsigned size = 0;
  BITCODE_RLL tmp = handle;

  while (tmp)
    {
      size++;
      tmp >>= 8;
    }
  if (!size || size > 8 || code > 0xf)
    return 0;
  pat[0] = (BITCODE_RC)((code << 4) | size);
  for (unsigned i = 0; i < size; i++)
    pat[1 + i] = (BITCODE_RC)((handle >> (8 * (size - i - 1))) & 0xff);
  return size + 1;
}

static size_t
r2007_find_bytes (const BITCODE_RC *haystack, size_t haystack_size,
                  const BITCODE_RC *needle, size_t needle_size,
                  size_t start)
{
  if (!needle_size || haystack_size < needle_size || start > haystack_size)
    return SIZE_MAX;
  for (size_t i = start; i + needle_size <= haystack_size; i++)
    {
      if (haystack[i] == needle[0]
          && memcmp (&haystack[i], needle, needle_size) == 0)
        return i;
    }
  return SIZE_MAX;
}

static int
r2007_decode_recovery_probe (Dwg_Data *restrict dwg,
                             Bit_Chain *restrict obj_dat,
                             Bit_Chain *restrict hdl,
                             const r2007_probe *restrict probe,
                             int *restrict error)
{
  BITCODE_BL before = dwg->num_objects;
  int added;

  if (r2007_object_has_handle (dwg, probe->handle))
    return 0;

  added = dwg_decode_add_object (dwg, obj_dat, hdl, probe->address);
  if (added > 0)
    *error |= added;
  return dwg->num_objects > before ? 1 : 0;
}

static int
r2007_recover_near_handle_pattern (Dwg_Data *restrict dwg,
                                   Bit_Chain *restrict obj_dat,
                                   Bit_Chain *restrict hdl,
                                   BITCODE_RLL handle,
                                   unsigned ref_code,
                                   int want_layer_object,
                                   unsigned *recovered_layers,
                                   unsigned *recovered_textlike,
                                   unsigned *pattern_hits,
                                   unsigned *probe_hits)
{
  BITCODE_RC pattern[9];
  size_t pattern_len = r2007_handle_pattern (handle, ref_code, pattern);
  size_t pos = 0;
  int error = 0;

  if (!pattern_len)
    return 0;

  for (unsigned variant = 0; variant < 2; variant++)
    {
      const BITCODE_RC *needle = variant ? &pattern[1] : pattern;
      size_t needle_len = variant ? pattern_len - 1 : pattern_len;
      pos = 0;
      if (!needle_len)
        continue;

      while ((pos = r2007_find_bytes (obj_dat->chain, obj_dat->size, needle,
                                      needle_len, pos))
             != SIZE_MAX)
        {
          const size_t back_limit = want_layer_object ? 128 : 4096;
          size_t start = pos > back_limit ? pos - back_limit : 0;
          int recovered_here = 0;
          (*pattern_hits)++;

          for (size_t address = start;
               address <= pos && address + 12 < obj_dat->size; address++)
            {
              r2007_probe probe;
              int decoded;

              if (!r2007_probe_object (dwg, obj_dat, address, &probe))
                continue;
              (*probe_hits)++;
              if (pos < probe.body_address
                  || pos >= probe.body_address + probe.size)
                continue;
              if (want_layer_object)
                {
                  if (probe.type != DWG_TYPE_LAYER || probe.handle != handle)
                    continue;
                }
              else
                {
                  if (!r2007_fixed_type_may_have_text (probe.type)
                      && !r2007_class_name_may_have_text (dwg, probe.type))
                    continue;
                }

              decoded = r2007_decode_recovery_probe (dwg, obj_dat, hdl,
                                                     &probe, &error);
              if (decoded > 0)
                {
                  Dwg_Object *obj = &dwg->object[dwg->num_objects - 1];
                  if (obj->fixedtype == DWG_TYPE_LAYER)
                    (*recovered_layers)++;
                  else
                    (*recovered_textlike)++;
                }
              recovered_here = 1;
              break;
            }

          pos += needle_len;
          if (want_layer_object && recovered_here)
            break;
        }
    }

  return error;
}

static int
r2007_recover_near_text_pattern (Dwg_Data *restrict dwg,
                                 Bit_Chain *restrict obj_dat,
                                 Bit_Chain *restrict hdl,
                                 const BITCODE_RC *pattern,
                                 size_t pattern_len,
                                 r2007_handle_vec *restrict decoded_addresses,
                                 unsigned *text_hits,
                                 unsigned *text_probe_hits,
                                 unsigned *recovered_textlike)
{
  size_t pos = 0;
  int error = 0;

  if (!pattern || !pattern_len)
    return 0;

  while ((pos = r2007_find_bytes (obj_dat->chain, obj_dat->size, pattern,
                                  pattern_len, pos))
         != SIZE_MAX)
    {
      const size_t back_limit = 2048;
      size_t start = pos > back_limit ? pos - back_limit : 0;

      (*text_hits)++;
      for (size_t address = pos;; address--)
        {
          r2007_probe probe;
          int decoded;

          if (address + 12 >= obj_dat->size)
            {
              if (address == start)
                break;
              continue;
            }
          if (r2007_handle_vec_contains (decoded_addresses,
                                         (BITCODE_RLL)address))
            {
              if (address == start)
                break;
              continue;
            }
          if (!r2007_probe_object (dwg, obj_dat, address, &probe))
            {
              if (address == start)
                break;
              continue;
            }
          if (pos < probe.body_address
              || pos >= probe.body_address + probe.size)
            {
              if (address == start)
                break;
              continue;
            }
          if (!r2007_fixed_type_may_have_text (probe.type)
              && !r2007_class_name_may_have_text (dwg, probe.type)
              && probe.type != DWG_TYPE_PROXY_ENTITY
              && probe.type != DWG_TYPE_PROXY_OBJECT)
            {
              if (address == start)
                break;
              continue;
            }

          (*text_probe_hits)++;
          decoded = r2007_decode_recovery_probe (dwg, obj_dat, hdl, &probe,
                                                 &error);
          r2007_handle_vec_add (decoded_addresses, (BITCODE_RLL)address);
          if (decoded > 0)
            (*recovered_textlike)++;
          break;
        }

      pos += pattern_len;
    }

  return error;
}

static int
r2007_recover_unresolved_layers_linear (Dwg_Data *restrict dwg,
                                        Bit_Chain *restrict obj_dat,
                                        Bit_Chain *restrict hdl,
                                        const r2007_handle_vec *targets,
                                        unsigned *recovered_layers)
{
  int error = 0;

  if (!targets || !targets->num)
    return 0;

  for (size_t address = 0; address + 12 < obj_dat->size; address++)
    {
      r2007_probe probe;
      int decoded;

      if (!r2007_probe_object (dwg, obj_dat, address, &probe))
        continue;
      if (probe.type != DWG_TYPE_LAYER)
        continue;
      if (!r2007_handle_vec_contains (targets, probe.handle)
          || r2007_object_has_handle (dwg, probe.handle))
        continue;

      decoded = r2007_decode_recovery_probe (dwg, obj_dat, hdl, &probe,
                                             &error);
      if (decoded > 0)
        (*recovered_layers)++;
      if (*recovered_layers >= targets->num)
        break;
    }

  return error;
}

static void
r2007_dump_probe_spans (Dwg_Data *restrict dwg, Bit_Chain *restrict obj_dat)
{
  const char *dump_path = getenv ("LIBDWG_DUMP_R2007_SPANS");
  FILE *fp;

  if (!dump_path || !dump_path[0] || !obj_dat || !obj_dat->chain)
    return;

  fp = fopen (dump_path, "wb");
  if (!fp)
    return;

  fprintf (fp, "address\tbody_address\tend_address\tsize\tbitsize\ttype\thandle\n");
  for (size_t address = 0; address + 12 < obj_dat->size; address++)
    {
      r2007_probe probe;
      if (!r2007_probe_object (dwg, obj_dat, address, &probe))
        continue;
      fprintf (fp, "%" PRIuSIZE "\t%" PRIuSIZE "\t%" PRIuSIZE
               "\t%lu\t%lu\t%u\t%llX\n",
               probe.address, probe.body_address,
               probe.body_address + probe.size, (unsigned long)probe.size,
               (unsigned long)probe.bitsize, (unsigned)probe.type,
               (unsigned long long)probe.handle);
    }

  fclose (fp);
}

static int
r2007_allow_speculative_text_recovery (void)
{
  const char *env = getenv ("LIBDWG_R2007_SPECULATIVE_TEXT_RECOVERY");
  return env && env[0] && env[0] != '0';
}

static int
r2007_allow_all_text_span_recovery (void)
{
  const char *env = getenv ("LIBDWG_R2007_RECOVER_ALL_TEXT_SPANS");
  return env && env[0] && env[0] != '0';
}

static int
r2007_disable_text_span_recovery (void)
{
  const char *env = getenv ("LIBDWG_R2007_DISABLE_TEXT_SPAN_RECOVERY");
  return env && env[0] && env[0] != '0';
}

static int
r2007_recover_all_text_spans (Dwg_Data *restrict dwg,
                              Bit_Chain *restrict obj_dat,
                              Bit_Chain *restrict hdl,
                              unsigned *recovered_textlike)
{
  int error = 0;
  unsigned long max_size = 512;
  const char *max_env = getenv ("LIBDWG_R2007_TEXT_SPAN_MAX_SIZE");

  if (max_env && max_env[0])
    {
      unsigned long value = strtoul (max_env, NULL, 10);
      if (value >= 24 && value <= 8192)
        max_size = value;
    }

  if (!obj_dat || !obj_dat->chain)
    return 0;

  for (size_t address = 0; address + 12 < obj_dat->size; address++)
    {
      r2007_probe probe;
      int decoded;

      if (!r2007_probe_object (dwg, obj_dat, address, &probe))
        continue;
      if (probe.type != DWG_TYPE_TEXT)
        continue;
      if (probe.size < 24 || probe.size > max_size)
        continue;
      if (r2007_object_has_handle (dwg, probe.handle))
        continue;

      decoded = r2007_decode_recovery_probe (dwg, obj_dat, hdl, &probe,
                                             &error);
      if (decoded > 0)
        (*recovered_textlike)++;
    }

  return error;
}

static int
recover_r2007_unmapped_objects (Dwg_Data *restrict dwg,
                                Bit_Chain *restrict obj_dat,
                                Bit_Chain *restrict hdl)
{
  r2007_handle_vec unresolved_layers = { 0 };
  BITCODE_RLL high_handle_floor;
  unsigned recovered_layers = 0;
  unsigned recovered_textlike = 0;
  unsigned pattern_hits = 0;
  unsigned probe_hits = 0;
  unsigned raw_ascii_s_eq = 0;
  unsigned raw_utf16_s_eq = 0;
  unsigned raw_ascii_s_151_94 = 0;
  unsigned raw_utf16_s_151_94 = 0;
  unsigned raw_text_hits = 0;
  unsigned raw_text_probe_hits = 0;
  int error = 0;
  int allow_speculative_text = r2007_allow_speculative_text_recovery ();
  int force_all_text_spans = r2007_allow_all_text_span_recovery ();
  int allow_all_text_spans = 0;
  r2007_handle_vec decoded_addresses = { 0 };

  if (obj_dat && obj_dat->chain && obj_dat->size >= 4)
    {
      static const BITCODE_RC s151_ascii[] = "S=151.94";
      static const BITCODE_RC s151_utf16[] = {
        'S', 0, '=', 0, '1', 0, '5', 0, '1', 0,
        '.', 0, '9', 0, '4', 0
      };
      for (size_t i = 0; i + 1 < obj_dat->size; i++)
        {
          if (obj_dat->chain[i] == 'S' && obj_dat->chain[i + 1] == '=')
            raw_ascii_s_eq++;
          if (i + 3 < obj_dat->size && obj_dat->chain[i] == 'S'
              && obj_dat->chain[i + 1] == 0 && obj_dat->chain[i + 2] == '='
              && obj_dat->chain[i + 3] == 0)
            raw_utf16_s_eq++;
          if (i + sizeof (s151_ascii) - 1 <= obj_dat->size
              && memcmp (&obj_dat->chain[i], s151_ascii,
                         sizeof (s151_ascii) - 1)
                     == 0)
            raw_ascii_s_151_94++;
          if (i + sizeof (s151_utf16) <= obj_dat->size
              && memcmp (&obj_dat->chain[i], s151_utf16,
                         sizeof (s151_utf16))
                     == 0)
            raw_utf16_s_151_94++;
        }
    }

  error |= r2007_collect_unresolved_layer_handles (dwg, &unresolved_layers);
  if (!unresolved_layers.num)
    {
      r2007_handle_vec_free (&unresolved_layers);
      return error;
    }

  high_handle_floor = r2007_max_reasonable_handle (dwg);
  LOG_INFO ("R2007 recovery: %u unresolved layer handles, "
            "high-handle floor " FORMAT_HV "\n",
            (unsigned)unresolved_layers.num, high_handle_floor);
  allow_all_text_spans
      = force_all_text_spans
        || (!r2007_disable_text_span_recovery ()
            && unresolved_layers.num >= 8 && raw_utf16_s_eq >= 100);
  dwg->r2007_text_span_recovery_active = allow_all_text_spans ? 1 : 0;

  error |= r2007_recover_unresolved_layers_linear (
      dwg, obj_dat, hdl, &unresolved_layers, &recovered_layers);

  for (size_t i = 0; i < unresolved_layers.num; i++)
    {
      BITCODE_RLL layer_handle = unresolved_layers.items[i];
      error |= r2007_recover_near_handle_pattern (
          dwg, obj_dat, hdl, layer_handle, 0, 1, &recovered_layers,
          &recovered_textlike, &pattern_hits, &probe_hits);
      if (allow_speculative_text)
        error |= r2007_recover_near_handle_pattern (
            dwg, obj_dat, hdl, layer_handle, 5, 0, &recovered_layers,
            &recovered_textlike, &pattern_hits, &probe_hits);
    }

  if (allow_speculative_text)
    {
      static const BITCODE_RC s_eq_ascii[] = { 'S', '=' };
      static const BITCODE_RC s_eq_utf16[] = { 'S', 0, '=', 0 };
      error |= r2007_recover_near_text_pattern (
          dwg, obj_dat, hdl, s_eq_ascii, sizeof (s_eq_ascii),
          &decoded_addresses, &raw_text_hits, &raw_text_probe_hits,
          &recovered_textlike);
      error |= r2007_recover_near_text_pattern (
          dwg, obj_dat, hdl, s_eq_utf16, sizeof (s_eq_utf16),
          &decoded_addresses, &raw_text_hits, &raw_text_probe_hits,
          &recovered_textlike);
    }
  if (allow_all_text_spans)
    error |= r2007_recover_all_text_spans (dwg, obj_dat, hdl,
                                           &recovered_textlike);

  LOG_INFO ("R2007 recovery recovered layers %u, "
            "text-like objects %u\n",
            recovered_layers, recovered_textlike);
  fprintf (stderr,
           "R2007 recovery diagnostics: unresolved_layers=%u, "
           "raw_S_eq_ascii=%u, raw_S_eq_utf16=%u, pattern_hits=%u, "
           "probe_hits=%u, raw_S_151_94_ascii=%u, raw_S_151_94_utf16=%u, "
           "raw_text_hits=%u, raw_text_probe_hits=%u, "
           "recovered_layers=%u, recovered_textlike=%u, "
           "speculative_text_recovery=%u, all_text_span_recovery=%u\n",
           (unsigned)unresolved_layers.num, raw_ascii_s_eq, raw_utf16_s_eq,
           pattern_hits, probe_hits, raw_ascii_s_151_94, raw_utf16_s_151_94,
           raw_text_hits, raw_text_probe_hits, recovered_layers,
           recovered_textlike, (unsigned)allow_speculative_text,
           (unsigned)allow_all_text_spans);
  r2007_handle_vec_free (&decoded_addresses);
  r2007_handle_vec_free (&unresolved_layers);
  return error;
}

static BITCODE_RS
r2007_read_RS_BE_at (const Bit_Chain *dat, size_t pos)
{
  if (!dat || !dat->chain || pos + 1 >= dat->size)
    return 0;
  return (BITCODE_RS)((dat->chain[pos] << 8) | dat->chain[pos + 1]);
}

static int
r2007_find_next_handles_page (const Bit_Chain *hdl_dat, size_t from,
                              size_t endpos, size_t *page_pos,
                              BITCODE_RS *page_size)
{
  const size_t scan_limit = 512;
  size_t maxpos;

  if (!hdl_dat || !hdl_dat->chain || from >= endpos)
    return 0;
  maxpos = from + scan_limit;
  if (maxpos > endpos)
    maxpos = endpos;

  for (size_t pos = from; pos + 4 <= maxpos; pos++)
    {
      BITCODE_RS size = r2007_read_RS_BE_at (hdl_dat, pos);
      uint16_t crc_read, crc_calc;

      if (size < 2 || size > 2050)
        continue;
      if (pos + size + 2 > endpos)
        continue;

      crc_read = r2007_read_RS_BE_at (hdl_dat, pos + size);
      crc_calc = bit_calc_CRC (0xC0C1, &hdl_dat->chain[pos], size);
      if (crc_read == crc_calc)
        {
          if (page_pos)
            *page_pos = pos;
          if (page_size)
            *page_size = size;
          return 1;
        }
    }

  return 0;
}

static int
read_2007_section_handles (Bit_Chain *dat, Bit_Chain *hdl,
                           Dwg_Data *restrict dwg,
                           r2007_section *restrict sections_map,
                           r2007_page *restrict pages_map)
{
  static Bit_Chain obj_dat = { 0 }, hdl_dat = { 0 };
  BITCODE_RS section_size = 0;
  size_t endpos;
  size_t map_last_offset = 0;
  BITCODE_RLL map_last_handle = 0;
  unsigned page_index = 0;
  unsigned decoded_objects = 0;
  unsigned failed_objects = 0;
  unsigned handle_mismatches = 0;
  int error;
  FILE *map_fp = NULL;

  error = read_data_section (&obj_dat, dat, sections_map, pages_map,
                             SECTION_OBJECTS);
  if (error >= DWG_ERR_CRITICAL || !obj_dat.chain)
    {
      LOG_ERROR ("Failed to read objects section");
      if (obj_dat.chain)
        free (obj_dat.chain);
      return error;
    }
  {
    const char *dump_path = getenv ("LIBDWG_DUMP_R2007_OBJECTS");
    if (dump_path && dump_path[0])
      {
        FILE *fp = fopen (dump_path, "wb");
        if (fp)
          {
            fwrite (obj_dat.chain, 1, obj_dat.size, fp);
            fclose (fp);
          }
      }
  }
  r2007_dump_probe_spans (dwg, &obj_dat);
  {
    const char *map_path = getenv ("LIBDWG_DUMP_R2007_MAP");
    if (map_path && map_path[0])
      {
        map_fp = fopen (map_path, "wb");
        if (map_fp)
          fprintf (map_fp,
                   "page\tmap_offset\texpected_handle\tdecoded_index\tdecoded_handle\tdecoded_type\tstatus\n");
      }
  }

  LOG_TRACE ("\nHandles\n-------------------\n");
  error = read_data_section (&hdl_dat, dat, sections_map, pages_map,
                             SECTION_HANDLES);
  if (error >= DWG_ERR_CRITICAL || !hdl_dat.chain)
    {
      LOG_ERROR ("Failed to read handles section");
      if (obj_dat.chain)
        free (obj_dat.chain);
      if (hdl_dat.chain)
        free (hdl_dat.chain);
      return error;
    }

  // From here on the same code as in decode:read_2004_section_handles 
  endpos = hdl_dat.byte + hdl_dat.size;
  dwg->num_objects = 0;

  do
    {
      size_t last_offset = 0;
      BITCODE_RLL last_handle = 0;
      size_t oldpos = 0;
      size_t startpos = hdl_dat.byte;
      uint16_t crc1, crc2;
      int crossed_boundary = 0;

      section_size = bit_read_RS_BE (&hdl_dat);
      LOG_TRACE ("\nSection[%u] size: %u\n", page_index, section_size);
      if (section_size > 2050 || section_size < 2)
        {
          size_t resync_pos = 0;
          BITCODE_RS resync_size = 0;
          LOG_ERROR ("Object-map/handles section[%u] invalid size %u",
                     page_index, section_size);
          error |= DWG_ERR_VALUEOUTOFBOUNDS;
          if (r2007_find_next_handles_page (&hdl_dat, startpos + 1, endpos,
                                            &resync_pos, &resync_size))
            {
              LOG_WARN ("Object-map/handles section[%u] resynced from %"
                        PRIuSIZE " to %" PRIuSIZE " (size %u)",
                        page_index, startpos, resync_pos, resync_size);
              fprintf (stderr,
                       "R2007 object-map resync section[%u]: %" PRIuSIZE
                       " -> %" PRIuSIZE " size=%u\n",
                       page_index, startpos, resync_pos, resync_size);
              hdl_dat.byte = resync_pos;
              hdl_dat.bit = 0;
              section_size = resync_size;
              page_index++;
              continue;
            }
          break;
        }

      while ((long)(hdl_dat.byte - startpos) < (long)section_size)
        {
          int added;
          BITCODE_BL prev_num_objects = dwg->num_objects;
          BITCODE_RLL expected_handle;
          BITCODE_UMC handleoff;
          BITCODE_MC offset;

          oldpos = hdl_dat.byte;
          handleoff = bit_read_UMC (&hdl_dat);
          offset = bit_read_MC (&hdl_dat);
          if (hdl_dat.byte - startpos > section_size)
            {
              LOG_WARN ("Handles section[%u] entry crosses page boundary "
                        "@%" PRIuSIZE ", rewind to CRC boundary",
                        page_index, oldpos);
              hdl_dat.byte = oldpos;
              hdl_dat.bit = 0;
              crossed_boundary = 1;
              break;
            }
          expected_handle = last_handle + handleoff;
          last_offset += offset;
          LOG_TRACE ("\nNext object: %lu ", (unsigned long)dwg->num_objects);
          LOG_TRACE ("Handleoff: " FORMAT_UMC " [UMC] "
                     "Handle: " FORMAT_HV " [map] "
                     "Offset: " FORMAT_MC " [MC] @%" PRIuSIZE "\n",
                     handleoff, expected_handle, offset, last_offset);

          if (hdl_dat.byte == oldpos)
            break;

          added = dwg_decode_add_object (dwg, &obj_dat, hdl, last_offset);
          if (added > 0)
            {
              failed_objects++;
              error |= added;
            }
          if (dwg->num_objects > prev_num_objects)
            {
              Dwg_Object *added_obj = &dwg->object[dwg->num_objects - 1];
              decoded_objects++;
              if (expected_handle && added_obj->handle.value
                  && added_obj->handle.value != expected_handle)
                {
                  handle_mismatches++;
                  LOG_WARN ("Object-map handle mismatch section[%u]: map "
                            FORMAT_HV " decoded " FORMAT_HV
                            " at object %lu offset %" PRIuSIZE,
                            page_index, expected_handle,
                            added_obj->handle.value,
                            (unsigned long)added_obj->index, last_offset);
                }
            }
          if (map_fp)
            {
              if (dwg->num_objects > prev_num_objects)
                {
                  Dwg_Object *added_obj = &dwg->object[dwg->num_objects - 1];
                  fprintf (map_fp, "%u\t%" PRIuSIZE "\t%llX\t%lu\t%llX\t%u\t%s\n",
                           page_index, last_offset,
                           (unsigned long long)expected_handle,
                           (unsigned long)added_obj->index,
                           (unsigned long long)added_obj->handle.value,
                           (unsigned)added_obj->fixedtype,
                           added > 0 ? "decoded_with_error" : "decoded");
                }
              else
                {
                  fprintf (map_fp, "%u\t%" PRIuSIZE "\t%llX\t\t\t\tfailed\n",
                           page_index, last_offset,
                           (unsigned long long)expected_handle);
                }
            }
          last_handle = expected_handle;
          map_last_handle = expected_handle;
          map_last_offset = last_offset;
        }

      if (hdl_dat.byte == oldpos && !crossed_boundary)
        break;
      if (hdl_dat.byte != startpos + section_size)
        {
          LOG_WARN ("Handles section[%u] ended at %" PRIuSIZE
                    ", expected CRC boundary %" PRIuSIZE,
                    page_index, hdl_dat.byte, startpos + section_size);
          hdl_dat.byte = startpos + section_size;
          hdl_dat.bit = 0;
        }
#if 0
      if (!bit_check_CRC(&hdl_dat, startpos, 0xC0C1))
        LOG_WARN("Handles section CRC mismatch at offset %lx", startpos);
#else
      crc1 = bit_calc_CRC (0xC0C1, &(hdl_dat.chain[startpos]),
                           section_size);
      crc2 = bit_read_RS_BE (&hdl_dat);
      if (crc1 == crc2)
        {
          LOG_INSANE ("Handles section page CRC: %04X from %zx-%zx\n", crc2,
                      startpos, hdl_dat.byte - 2);
        }
      else
        {
          LOG_WARN ("Handles section page CRC mismatch: %04X vs calc. %04X "
                    "from %zx-%zx\n",
                    crc2, crc1, startpos, hdl_dat.byte - 2);
           error |= DWG_ERR_WRONGCRC;
        }
#endif

      if (hdl_dat.byte >= endpos)
        break;
      page_index++;
      if (page_index > 10000)
        {
          LOG_ERROR ("Object-map/handles section page limit exceeded");
          error |= DWG_ERR_VALUEOUTOFBOUNDS;
          break;
        }
    }
  while (section_size > 2);   

  fprintf (stderr,
           "R2007 object map decoded %u objects, failed %u, "
           "handle mismatches %u, last handle " FORMAT_HV
           ", last offset %" PRIuSIZE "\n",
           decoded_objects, failed_objects, handle_mismatches,
           map_last_handle, map_last_offset);
  LOG_INFO ("R2007 object map decoded %u objects, failed %u, "
            "handle mismatches %u, last handle " FORMAT_HV
            ", last offset %" PRIuSIZE "\n",
            decoded_objects, failed_objects, handle_mismatches,
            map_last_handle, map_last_offset);

  error |= recover_r2007_unmapped_objects (dwg, &obj_dat, hdl);

  if (map_fp)
    fclose (map_fp);
  if (hdl_dat.chain)
    free (hdl_dat.chain);
  if (obj_dat.chain)
    free (obj_dat.chain);
  return error;
}

//--------------------------------------------------------------new1
/*
static int
read_2007_section_handles(Bit_Chain *dat, Bit_Chain *hdl,
	Dwg_Data *restrict dwg,
	r2007_section *restrict sections_map,
	r2007_page *restrict pages_map)
{
	static Bit_Chain obj_dat = { 0 }, hdl_dat = { 0 };
	BITCODE_RS section_size = 0;
	size_t endpos;
	int error = 0;                     // 不再使用原始的 error 累积
	unsigned long max_objects = 2000000; // 安全上限，防止死循环

	error = read_data_section(&obj_dat, dat, sections_map, pages_map,
		SECTION_OBJECTS);
	if (error >= DWG_ERR_CRITICAL || !obj_dat.chain)
	{
		LOG_ERROR("Failed to read objects section");
		if (obj_dat.chain)
			free(obj_dat.chain);
		return error;
	}

	LOG_TRACE("\nHandles\n-------------------\n");
	error = read_data_section(&hdl_dat, dat, sections_map, pages_map,
		SECTION_HANDLES);
	if (error >= DWG_ERR_CRITICAL || !hdl_dat.chain)
	{
		LOG_ERROR("Failed to read handles section");
		if (obj_dat.chain)
			free(obj_dat.chain);
		if (hdl_dat.chain)
			free(hdl_dat.chain);
		return error;
	}

	endpos = hdl_dat.byte + hdl_dat.size;
	dwg->num_objects = 0;
	static int fail_count = 0;
	do
	{
		size_t last_offset;
		size_t oldpos = 0;
		size_t startpos = hdl_dat.byte;
		uint16_t crc1, crc2;

		section_size = bit_read_RS_BE(&hdl_dat);
		LOG_TRACE("\nSection size: %u\n", section_size);
		if (section_size > 2050)
			LOG_WARN("Large handles section page: %u bytes", section_size);
		// 不再跳过，继续处理，但限制每个页最多读取 max_objects 个对象 

		last_offset = 0;
		unsigned long page_objects = 0;
		while ((long)(hdl_dat.byte - startpos) < (long)section_size
			&& page_objects < 50000)
		{
			int added;
			BITCODE_UMC handleoff;
			BITCODE_MC offset;

			oldpos = hdl_dat.byte;
			handleoff = bit_read_UMC(&hdl_dat);
			offset = bit_read_MC(&hdl_dat);

			if (hdl_dat.byte == oldpos) {
				hdl_dat.byte = oldpos + 1;
				hdl_dat.bit = 0;
				continue;
			}

			last_offset += offset;
			page_objects++;
			LOG_TRACE("\nNext object: %lu ", (unsigned long)dwg->num_objects);
			LOG_TRACE("Handleoff: " FORMAT_UMC " [UMC] "
				"Offset: " FORMAT_MC " [MC] @%" PRIuSIZE "\n",
				handleoff, offset, last_offset);

			if (dwg->num_objects >= max_objects)
				break;

			added = dwg_decode_add_object(dwg, &obj_dat, hdl, last_offset);
			if (added == 0)
			{
				LOG_TRACE("OK: object %lu added at offset %lu",
					(unsigned long)dwg->num_objects - 1,
					(unsigned long)last_offset);
			}
			else
			{
				
				fail_count++;
				LOG_WARN("FAIL: object at offset %lu, error %d (total failures: %d)",
					(unsigned long)last_offset, added, fail_count);
				LOG_WARN("FAIL: object at offset %lu, error %d",
					(unsigned long)last_offset, added);

			}
		}

		// CRC 检查仍然执行，但仅做日志记录，不修改 error 变量
		crc1 = bit_calc_CRC(0xC0C1, &(hdl_dat.chain[startpos]),
			hdl_dat.byte - startpos);
		crc2 = bit_read_RS_BE(&hdl_dat);
		if (crc1 != crc2)
		{
			LOG_WARN("Handles section page CRC mismatch: %04X vs calc. %04X "
				"from %zx-%zx\n",
				crc2, crc1, startpos, hdl_dat.byte - 2);
			// 不执行 error |= DWG_ERR_WRONGCRC;
		}

		if (hdl_dat.byte >= endpos)
			break;
	} while (section_size > 2 && dwg->num_objects < max_objects);

	// 统计所有对象类型，方便对比 
	fprintf(stderr, "\n=== Decoded object type counts ===\n");
	// 统计并打印自定义类型（type > 500） 
	{
		int *cust_counts = NULL;
		int cust_max = 0;
		for (unsigned long k = 0; k < dwg->num_objects; k++)
		{
			int t = dwg->object[k].fixedtype;
			if (t > 500)
			{
				if (t > cust_max)
				{
					cust_counts = (int *)realloc(cust_counts, (t + 1) * sizeof(int));
					memset(cust_counts + cust_max + 1, 0, (t - cust_max) * sizeof(int));
					cust_max = t;
				}
				cust_counts[t]++;
			}
		}
		fprintf(stderr, "\n=== Custom object type counts (type>500) ===\n");
		for (int t = 501; t <= cust_max; t++)
		{
			if (cust_counts[t] > 0)
			{
				const char *tname = dwg_type_name(t);
				fprintf(stderr, "Type %d (%s): %d\n", t, tname ? tname : "unknown", cust_counts[t]);
			}
		}
		free(cust_counts);
		fprintf(stderr, "===================================\n");
	}
	int type_counts[256] = { 0 };
	for (unsigned long k = 0; k < dwg->num_objects; k++)
	{
		int t = dwg->object[k].fixedtype;
		if (t >= 0 && t < 256)
			type_counts[t]++;
	}
	for (int t = 0; t < 256; t++)
	{
		if (type_counts[t] > 0)
		{
			const char *tname = dwg_type_name(t);
			fprintf(stderr, "Type %3d (%s): %d\n", t, tname ? tname : "unknown", type_counts[t]);
		}
	}
	fprintf(stderr, "===================================\n\n");
	fprintf(stderr, "Total decode_add_object failures: %d\n", fail_count);
	if (hdl_dat.chain)
		free(hdl_dat.chain);
	if (obj_dat.chain)
		free(obj_dat.chain);
	
	return error;
}
*/
//--------------------------------------------------------------------new2

/* VBAProject Section
 */
static int
read_2007_section_vbaproject (Bit_Chain *restrict dat, Dwg_Data *restrict dwg,
                              r2007_section *restrict sections_map,
                              r2007_page *restrict pages_map)
{
  Bit_Chain old_dat, sec_dat = { 0 };
  // Bit_Chain *str_dat;
  Dwg_VBAProject *_obj = &dwg->vbaproject;
  // Dwg_Object *obj = NULL;
  int error = 0;
  // BITCODE_RL rcount1 = 0, rcount2 = 0;

  // not compressed, page size: 0x80
  error = read_data_section (&sec_dat, dat, sections_map, pages_map,
                             SECTION_VBAPROJECT);
  if (error >= DWG_ERR_CRITICAL || !sec_dat.chain)
    {
      LOG_INFO ("%s section not found\n", "VBAProject");
      if (sec_dat.chain)
        free (sec_dat.chain);
      return error;
    }

  LOG_TRACE ("\nVBAProject (%" PRIuSIZE ")\n-------------------\n",
             sec_dat.size);
  old_dat = *dat;
  dat = &sec_dat; // restrict in size
  bit_chain_set_version (&old_dat, dat);

  DEBUG_HERE
  _obj->size = dat->size & 0xFFFFFFFF;
  _obj->unknown_bits = bit_read_TF (dat, _obj->size);
  LOG_TRACE_TF (_obj->unknown_bits, _obj->size);

  LOG_TRACE ("\n");
  if (sec_dat.chain)
    free (sec_dat.chain);
  *dat = old_dat; // unrestrict
  return error;
}

static int
read_2007_section_summary (Bit_Chain *restrict dat, Dwg_Data *restrict dwg,
                           r2007_section *restrict sections_map,
                           r2007_page *restrict pages_map)
{
  static Bit_Chain old_dat, sec_dat = { 0 };
  // Bit_Chain *str_dat;
  Dwg_SummaryInfo *_obj = &dwg->summaryinfo;
  Dwg_Object *obj = NULL;
  int error;
  BITCODE_RL rcount1 = 0, rcount2 = 0;

  old_dat = *dat;
  error = read_data_section (&sec_dat, dat, sections_map, pages_map,
                             SECTION_SUMMARYINFO);
  if (error >= DWG_ERR_CRITICAL || !sec_dat.chain)
    {
      LOG_ERROR ("Failed to read SummaryInfo section");
      if (sec_dat.chain)
        free (sec_dat.chain);
      return error;
    }

  if (dwg->header.summaryinfo_address != (BITCODE_RL)dat->byte)
    LOG_WARN ("summaryinfo_address mismatch: " FORMAT_RL " != %" PRIuSIZE,
              dwg->header.summaryinfo_address, dat->byte);
  LOG_TRACE ("\nSummaryInfo (%" PRIuSIZE ")\n-------------------\n",
             sec_dat.size);
  /*str_dat = */ dat = &sec_dat; // restrict in size
  bit_chain_set_version (&old_dat, dat);

  // clang-format off
  #include "summaryinfo.spec"
  // clang-format on

  if (sec_dat.chain)
    free (sec_dat.chain);
  *dat = old_dat; // unrestrict
  return error;
}

/* AppInfo Section
 */
static int
read_2007_section_appinfo (Bit_Chain *restrict dat, Dwg_Data *restrict dwg,
                           r2007_section *restrict sections_map,
                           r2007_page *restrict pages_map)
{
  Bit_Chain old_dat, sec_dat = { 0 };
  // Bit_Chain *str_dat;
  Dwg_AppInfo *_obj = &dwg->appinfo;
  Dwg_Object *obj = NULL;
  int error = 0;
  BITCODE_RL rcount1 = 0, rcount2 = 0;

  // not compressed, page size: 0x80
  error = read_data_section (&sec_dat, dat, sections_map, pages_map,
                             SECTION_APPINFO);
  if (error >= DWG_ERR_CRITICAL || !sec_dat.chain)
    {
      LOG_INFO ("%s section not found\n", "AppInfo");
      if (sec_dat.chain)
        free (sec_dat.chain);
      return error;
    }

  LOG_TRACE ("\nAppInfo (%" PRIuSIZE ")\n-------------------\n", sec_dat.size);
  old_dat = *dat;
  /*str_dat = */ dat = &sec_dat; // restrict in size
  bit_chain_set_version (&old_dat, dat);

  // clang-format off
  #include "appinfo.spec"
  // clang-format on

  LOG_TRACE ("\n");
  if (sec_dat.chain)
    free (sec_dat.chain);
  *dat = old_dat; // unrestrict
  return error;
}

/* AuxHeader Section
 */
static int
read_2007_section_auxheader (Bit_Chain *restrict dat, Dwg_Data *restrict dwg,
                             r2007_section *restrict sections_map,
                             r2007_page *restrict pages_map)
{
  Bit_Chain old_dat, sec_dat = { 0 };
  // Bit_Chain *str_dat;
  Dwg_AuxHeader *_obj = &dwg->auxheader;
  Dwg_Object *obj = NULL;
  int error = 0;
  BITCODE_RL vcount = 0, rcount1 = 0, rcount2 = 0;

  // type: 2, compressed, page size: 0x7400
  error = read_data_section (&sec_dat, dat, sections_map, pages_map,
                             SECTION_AUXHEADER);
  if (error >= DWG_ERR_CRITICAL || !sec_dat.chain)
    {
      LOG_INFO ("%s section not found\n", "AuxHeader");
      if (sec_dat.chain)
        free (sec_dat.chain);
      return error;
    }

  LOG_TRACE ("\nAuxHeader (%" PRIuSIZE ")\n-------------------\n",
             sec_dat.size);
  old_dat = *dat;
  dat = &sec_dat; // restrict in size
  bit_chain_set_version (&old_dat, dat);

  // clang-format off
  #include "auxheader.spec"
  // clang-format on

  LOG_TRACE ("\n");
  if (sec_dat.chain)
    free (sec_dat.chain);
  *dat = old_dat; // unrestrict
  return error;
}

/* Unknown AppInfoHistory Section
 */
static int
read_2007_section_appinfohistory (Bit_Chain *restrict dat,
                                  Dwg_Data *restrict dwg,
                                  r2007_section *restrict sections_map,
                                  r2007_page *restrict pages_map)
{
  Bit_Chain old_dat, sec_dat = { 0 };
  // Bit_Chain *str_dat;
  Dwg_AppInfoHistory *_obj = &dwg->appinfohistory;
  // Dwg_Object *obj = NULL;
  int error = 0;
  // BITCODE_RL rcount1 = 0, rcount2 = 0;

  // compressed, page size: 0x580
  error = read_data_section (&sec_dat, dat, sections_map, pages_map,
                             SECTION_APPINFOHISTORY);
  if (error >= DWG_ERR_CRITICAL || !sec_dat.chain)
    {
      LOG_INFO ("%s section not found\n", "AppInfoHistory");
      if (sec_dat.chain)
        free (sec_dat.chain);
      return error;
    }

  LOG_TRACE ("\nAppInfoHistory (%" PRIuSIZE ")\n-------------------\n",
             sec_dat.size);
  old_dat = *dat;
  dat = &sec_dat; // restrict in size
  bit_chain_set_version (&old_dat, dat);

  DEBUG_HERE
  _obj->size = dat->size & 0xFFFFFFFF;
  _obj->unknown_bits = bit_read_TF (dat, _obj->size);
  LOG_TRACE_TF (_obj->unknown_bits, _obj->size);

  LOG_TRACE ("\n");
  if (sec_dat.chain)
    free (sec_dat.chain);
  *dat = old_dat; // unrestrict
  return error;
}

/* RevHistory Section
 */
static int
read_2007_section_revhistory (Bit_Chain *restrict dat, Dwg_Data *restrict dwg,
                              r2007_section *restrict sections_map,
                              r2007_page *restrict pages_map)
{
  Bit_Chain old_dat, sec_dat = { 0 };
  // Bit_Chain *str_dat;
  Dwg_RevHistory *_obj = &dwg->revhistory;
  Dwg_Object *obj = NULL;
  int error = 0;
  BITCODE_RL rcount1 = 0, rcount2 = 0;

  // compressed, page size: 0x7400
  error = read_data_section (&sec_dat, dat, sections_map, pages_map,
                             SECTION_REVHISTORY);
  if (error >= DWG_ERR_CRITICAL || !sec_dat.chain)
    {
      LOG_INFO ("%s section not found\n", "RevHistory");
      if (sec_dat.chain)
        free (sec_dat.chain);
      return error;
    }

  LOG_TRACE ("\nRevHistory (%" PRIuSIZE ")\n-------------------\n",
             sec_dat.size);
  old_dat = *dat;
  dat = &sec_dat; // restrict in size
  bit_chain_set_version (&old_dat, dat);

  // clang-format off
  #include "revhistory.spec"
  // clang-format on

  LOG_TRACE ("\n");
  if (sec_dat.chain)
    free (sec_dat.chain);
  *dat = old_dat; // unrestrict
  return error;
}

/* ObjFreeSpace Section
 */
static int
read_2007_section_objfreespace (Bit_Chain *restrict dat,
                                Dwg_Data *restrict dwg,
                                r2007_section *restrict sections_map,
                                r2007_page *restrict pages_map)
{
  Bit_Chain old_dat, sec_dat = { 0 };
  // Bit_Chain *str_dat;
  Dwg_ObjFreeSpace *_obj = &dwg->objfreespace;
  // Dwg_Object *obj = NULL;
  int error = 0;
  BITCODE_RL rcount1 = 0, rcount2 = 0;

  // compressed, page size: 0x7400
  error = read_data_section (&sec_dat, dat, sections_map, pages_map,
                             SECTION_OBJFREESPACE);
  if (error >= DWG_ERR_CRITICAL || !sec_dat.chain)
    {
      LOG_INFO ("%s section not found\n", "ObjFreeSpace");
      if (sec_dat.chain)
        free (sec_dat.chain);
      return error;
    }

  LOG_TRACE ("\nObjFreeSpace (%" PRIuSIZE ")\n-------------------\n",
             sec_dat.size);
  old_dat = *dat;
  dat = &sec_dat; // restrict in size
  bit_chain_set_version (&old_dat, dat);

  // clang-format off
  #include "objfreespace.spec"
  // clang-format on

  LOG_TRACE ("\n");
  if (sec_dat.chain)
    free (sec_dat.chain);
  *dat = old_dat; // unrestrict
  return error;
}

/* Template Section. Optional r13-r15, mandatory r18+.
   Contains the MEASUREMENT variable (0 = English, 1 = Metric).
 */
static int
read_2007_section_template (Bit_Chain *restrict dat, Dwg_Data *restrict dwg,
                            r2007_section *restrict sections_map,
                            r2007_page *restrict pages_map)
{
  Bit_Chain old_dat, sec_dat = { 0 };
  // Bit_Chain *str_dat;
  Dwg_Template *_obj = &dwg->Template;
  Dwg_Object *obj = NULL;
  int error = 0;
  BITCODE_RL rcount1 = 0, rcount2 = 0;

  // compressed
  error = read_data_section (&sec_dat, dat, sections_map, pages_map,
                             SECTION_TEMPLATE);
  if (error >= DWG_ERR_CRITICAL || !sec_dat.chain)
    {
      LOG_ERROR ("%s section not found\n", "Template");
      if (sec_dat.chain)
        free (sec_dat.chain);
      return error | DWG_ERR_SECTIONNOTFOUND;
    }

  LOG_TRACE ("\nTemplate (%" PRIuSIZE ")\n-------------------\n",
             sec_dat.size);
  old_dat = *dat;
  dat = &sec_dat; // restrict in size
  bit_chain_set_version (&old_dat, dat);

  // clang-format off
  #include "template.spec"
  // clang-format on

  dwg->header_vars.MEASUREMENT = _obj->MEASUREMENT;
  LOG_TRACE ("HEADER.MEASUREMENT: " FORMAT_BS " (0 English/1 Metric)\n",
             dwg->header_vars.MEASUREMENT);

  LOG_TRACE ("\n");
  if (sec_dat.chain)
    free (sec_dat.chain);
  *dat = old_dat; // unrestrict
  return error;
}

/* r21 FileDepList Section
 */
static int
read_2007_section_filedeplist (Bit_Chain *restrict dat, Dwg_Data *restrict dwg,
                               r2007_section *restrict sections_map,
                               r2007_page *restrict pages_map)
{
  Bit_Chain old_dat, sec_dat = { 0 };
  int error;
  // Bit_Chain *str_dat;
  Dwg_FileDepList *_obj = &dwg->filedeplist;
  Dwg_Object *obj = NULL;
  BITCODE_BL vcount;
  BITCODE_RL rcount1 = 0, rcount2 = 0;

  // not compressed, page size: 0x80. 0xc or 0xd
  error = read_data_section (&sec_dat, dat, sections_map, pages_map,
                             SECTION_FILEDEPLIST);
  if (error >= DWG_ERR_CRITICAL || !sec_dat.chain)
    {
      LOG_INFO ("%s section not found\n", "FileDepList");
      if (sec_dat.chain)
        free (sec_dat.chain);
      return 0;
    }

  LOG_TRACE ("FileDepList (%" PRIuSIZE ")\n-------------------\n",
             sec_dat.size);
  old_dat = *dat;
  /*str_dat = */ dat = &sec_dat; // restrict in size
  bit_chain_set_version (&old_dat, dat);

  // clang-format off
  #include "filedeplist.spec"
  // clang-format on

  LOG_TRACE ("\n");
  if (sec_dat.chain)
    free (sec_dat.chain);
  *dat = old_dat; // unrestrict
  return error;
}

/* r21 Security Section, if saved with password
 */
static int
read_2007_section_security (Bit_Chain *restrict dat, Dwg_Data *restrict dwg,
                            r2007_section *restrict sections_map,
                            r2007_page *restrict pages_map)
{
  Bit_Chain old_dat, sec_dat = { 0 };
  int error;
  // Bit_Chain *str_dat;
  Dwg_Security *_obj = &dwg->security;
  Dwg_Object *obj = NULL;
  BITCODE_RL rcount1 = 0, rcount2 = 0;

  // compressed, page size: 0x7400
  error = read_data_section (&sec_dat, dat, sections_map, pages_map,
                             SECTION_SECURITY);
  if (error >= DWG_ERR_CRITICAL || !sec_dat.chain)
    {
      LOG_INFO ("%s section not found\n", "Security");
      if (sec_dat.chain)
        free (sec_dat.chain);
      return 0;
    }

  LOG_TRACE ("Security (%" PRIuSIZE ")\n-------------------\n", sec_dat.size);
  old_dat = *dat;
  /*str_dat = */ dat = &sec_dat; // restrict in size
  bit_chain_set_version (&old_dat, dat);

  // clang-format off
  #include "security.spec"
  // clang-format on

  LOG_TRACE ("\n");
  if (sec_dat.chain)
    free (sec_dat.chain);
  *dat = old_dat; // unrestrict
  return error;
}

#if 0
/* Signature Section, not written nor documented by Teigha
 */
static int
read_2007_section_signature (Bit_Chain *restrict dat, Dwg_Data *restrict dwg,
                           r2007_section *restrict sections_map,
                           r2007_page *restrict pages_map)
{
  Bit_Chain old_dat, sec_dat = { 0 };
  int error;
  Bit_Chain *str_dat;
  struct Dwg_Signature *_obj = &dwg->signature;
  Dwg_Object *obj = NULL;
  BITCODE_RL rcount1 = 0, rcount2 = 0;

  // compressed, page size: 0x7400
  error = read_data_section (&sec_dat, dat, sections_map, pages_map,
                             SECTION_SIGNATURE);
  if (error >= DWG_ERR_CRITICAL || !sec_dat.chain)
    {
      LOG_INFO ("%s section not found\n", "Signature");
      if (sec_dat.chain)
        free (sec_dat.chain);
      return 0;
    }

  LOG_TRACE ("Signature (%" PRIuSIZE ")\n-------------------\n", sec_dat.size);
  old_dat = *dat;
  str_dat = dat = &sec_dat; // restrict in size
  bit_chain_set_version (&old_dat, dat);

  // clang-format off
  #include "signature.spec"
  // clang-format on

  LOG_TRACE ("\n");
  if (sec_dat.chain)
    free (sec_dat.chain);
  *dat = old_dat; // unrestrict
  return error;
}
#endif

static int
acds_private (Bit_Chain *restrict dat, Dwg_Data *restrict dwg)
{
  // Bit_Chain *str_dat = dat;
  Dwg_AcDs *_obj = &dwg->acds;
  Dwg_Object *obj = NULL;
  int error = 0;
  BITCODE_BL rcount1 = 0, rcount2 = 0;
  BITCODE_BL rcount3 = 0, rcount4, vcount;

  // clang-format off
  #include "acds.spec"
  // clang-format on

  return error;
}

/* r2013+ datastorage Section, if saved with binary ACIS SAB data
 */
static int
read_2007_section_acds (Bit_Chain *restrict dat, Dwg_Data *restrict dwg,
                        r2007_section *restrict sections_map,
                        r2007_page *restrict pages_map)
{
  Bit_Chain old_dat, sec_dat = { 0 };
  int error;
  const char *secname = "AcDsPrototype_1b";

  // compressed, pagesize 0x7400, type 13
  error = read_data_section (&sec_dat, dat, sections_map, pages_map,
                             SECTION_ACDS);
  if (error >= DWG_ERR_CRITICAL || !sec_dat.chain)
    {
      LOG_INFO ("%s section not found\n", secname);
      if (sec_dat.chain)
        free (sec_dat.chain);
      return 0;
    }

  LOG_TRACE ("AcDs datastorage (%" PRIuSIZE ")\n-------------------\n",
             sec_dat.size);
  old_dat = *dat;
  dat = &sec_dat; // restrict in size
  bit_chain_set_version (&old_dat, dat);

  error |= acds_private (dat, dwg);
  error &= ~DWG_ERR_SECTIONNOTFOUND;

  LOG_TRACE ("\n");
  if (sec_dat.chain)
    free (sec_dat.chain);
  *dat = old_dat; // unrestrict
  return error;
}

static int
read_2007_section_preview (Bit_Chain *restrict dat, Dwg_Data *restrict dwg,
                           r2007_section *restrict sections_map,
                           r2007_page *restrict pages_map)
{
  static Bit_Chain sec_dat = { 0 };
  int error;
  BITCODE_RL size;
  BITCODE_RC type;
  const unsigned char *sentinel;

  error = read_data_section (&sec_dat, dat, sections_map, pages_map,
                             SECTION_PREVIEW);
  if (error >= DWG_ERR_CRITICAL || !sec_dat.chain)
    {
      LOG_ERROR ("Failed to read uncompressed %s section", "Preview");
      if (sec_dat.chain)
        free (sec_dat.chain);
      return error;
    }

  if (dwg->header.thumbnail_address != (BITCODE_RL)dat->byte)
    LOG_WARN ("thumbnail_address mismatch: " FORMAT_RL " != %" PRIuSIZE,
              dwg->header.thumbnail_address, dat->byte);
  LOG_TRACE ("\nPreview (%" PRIuSIZE ")\n-------------------\n", sec_dat.size);
  if (!sec_dat.chain || sec_dat.size < 32)
    {
      LOG_WARN ("Empty thumbnail");
      if (sec_dat.chain)
        free (sec_dat.chain);
      return error;
    }

  sentinel = dwg_sentinel (DWG_SENTINEL_THUMBNAIL_BEGIN);
  if (memcmp (sentinel, sec_dat.chain, 16))
    {
      LOG_WARN ("thumbnail sentinel mismatch");
      return error |= DWG_ERR_WRONGCRC;
    }

  assert (sec_dat.size >= 32);
  assert (sec_dat.chain);
  dwg->thumbnail.size = sec_dat.size - 32; // 2x sentinel
  dwg->thumbnail.chain = sec_dat.chain;
  dwg->thumbnail.byte = 16; // sentinel

  dwg_bmp (dwg, &size, &type);
  if (abs ((int)((long)size - (long)dwg->thumbnail.size))
      > 200) // various headers
    LOG_WARN ("thumbnail.size mismatch: %" PRIuSIZE " != " FORMAT_RL,
              dwg->thumbnail.size, size);

  dat->byte += dwg->thumbnail.size;

  return error;
}

/* exported */
void
read_r2007_init (Dwg_Data *restrict dwg)
{
  if (dwg->opts)
    loglevel = dwg->opts & DWG_OPTS_LOGLEVEL;
}

int
read_r2007_meta_data (Bit_Chain *dat, Bit_Chain *hdl_dat,
                      Dwg_Data *restrict dwg)
{
  Dwg_R2007_Header *file_header;
  r2007_page *restrict pages_map = NULL, *restrict page;
  r2007_section *restrict sections_map = NULL;
  int error;
#ifdef USE_TRACING
  char *probe;
#endif

  read_r2007_init (dwg);
#ifdef USE_TRACING
  probe = getenv ("LIBREDWG_TRACE");
  if (probe)
    loglevel = atoi (probe);
#endif
  // @ 0x62
  error = read_file_header (dat, &dwg->fhdr.r2007_file_header);
  if (error >= DWG_ERR_VALUEOUTOFBOUNDS)
    return error;
  file_header = &dwg->fhdr.r2007_file_header;

  // Pages Map
  dat->byte += 0x28; // overread check data
  dat->byte += file_header->pages_map_offset;
  if ((size_t)file_header->pages_map_size_comp > dat->size - dat->byte)
    {
      LOG_ERROR ("%s Invalid pages_map_size_comp %" PRIuSIZE " > %" PRIuSIZE
                 " bytes left",
                 __FUNCTION__, (size_t)file_header->pages_map_size_comp,
                 dat->size - dat->byte);
      error |= DWG_ERR_VALUEOUTOFBOUNDS;
      goto error;
    }
  pages_map = read_pages_map (dat, file_header->pages_map_size_comp,
                              file_header->pages_map_size_uncomp,
                              file_header->pages_map_correction);
  if (!pages_map)
    return DWG_ERR_PAGENOTFOUND; // Error already logged

  // Sections Map
  page = get_page (pages_map, file_header->sections_map_id);
  if (!page)
    {
      LOG_ERROR ("Failed to find sections page map %d",
                 (int)file_header->sections_map_id);
      error |= DWG_ERR_SECTIONNOTFOUND;
      goto error;
    }
  dat->byte = page->offset;
  if ((size_t)file_header->sections_map_size_comp > dat->size - dat->byte)
    {
      LOG_ERROR ("%s Invalid comp_data_size %" PRId64 " > %" PRIuSIZE
                 " bytes left",
                 __FUNCTION__, file_header->sections_map_size_comp,
                 dat->size - dat->byte);
      error |= DWG_ERR_VALUEOUTOFBOUNDS;
      goto error;
    }
  sections_map = read_sections_map (dat, file_header->sections_map_size_comp,
                                    file_header->sections_map_size_uncomp,
                                    file_header->sections_map_correction);
  if (!sections_map)
    goto error;

  error
      = read_2007_section_header (dat, hdl_dat, dwg, sections_map, pages_map);
  if (dwg->header.summaryinfo_address)
    error |= read_2007_section_summary (dat, dwg, sections_map, pages_map);
  error |= read_2007_section_classes (dat, dwg, sections_map, pages_map);
  error |= read_2007_section_handles (dat, hdl_dat, dwg, sections_map,
                                      pages_map);
  error |= read_2007_section_auxheader (dat, dwg, sections_map, pages_map);
  if (dwg->header.thumbnail_address)
    error |= read_2007_section_preview (dat, dwg, sections_map, pages_map);
  error |= read_2007_section_appinfo (dat, dwg, sections_map, pages_map);
  error
      |= read_2007_section_appinfohistory (dat, dwg, sections_map, pages_map);
  error |= read_2007_section_filedeplist (dat, dwg, sections_map, pages_map);
  error |= read_2007_section_security (dat, dwg, sections_map, pages_map);
  error |= read_2007_section_revhistory (dat, dwg, sections_map, pages_map);
  error |= read_2007_section_objfreespace (dat, dwg, sections_map, pages_map);
  error |= read_2007_section_template (dat, dwg, sections_map, pages_map);
  if (dwg->header.vbaproj_address)
    error |= read_2007_section_vbaproject (dat, dwg, sections_map, pages_map);
  // error |= read_2007_section_signature (dat, dwg, sections_map, pages_map);
  error |= read_2007_section_acds (dat, dwg, sections_map, pages_map);
  // read_2007_blocks (dat, hdl_dat, dwg, sections_map, pages_map);

error:
  pages_destroy (pages_map);
  if (sections_map)
    sections_destroy (sections_map);

  return error;
}
