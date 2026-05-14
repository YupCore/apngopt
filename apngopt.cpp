/* APNG Optimizer 1.4
 *
 * Makes APNG files smaller.
 *
 * http://sourceforge.net/projects/apng/files
 *
 * Copyright (c) 2011-2015 Max Stepin
 * maxst at users.sourceforge.net
 *
 * zlib license
 * ------------
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <limits>
#include <string>
#include <thread>
#include <vector>
#include "png.h"     /* original (unpatched) libpng is ok */
#include "zlib.h"
#include "7z.h"
#include "libimagequant.h"
extern "C" {
#include "zopfli.h"
}

#define notabc(c) ((c) < 65 || (c) > 122 || ((c) > 90 && (c) < 97))

#define id_IHDR 0x52444849
#define id_acTL 0x4C546361
#define id_fcTL 0x4C546366
#define id_IDAT 0x54414449
#define id_fdAT 0x54416466
#define id_IEND 0x444E4549

struct CHUNK { unsigned char * p; unsigned int size; };
struct APNGFrame { unsigned char * p, ** rows; unsigned int w, h, delay_num, delay_den; };
struct COLORS { unsigned int num; unsigned char r, g, b, a; };
struct rgb { unsigned char r, g, b; };

enum ExactCandidatesMode
{
  kExactCandidatesOff = 0,
  kExactCandidatesOn = 1
};

struct ZlibFinalOptions
{
  int level;
  int memLevel;
  int strategy;
  int mode;
  int hasLevel;
  int hasMemLevel;
  int hasStrategy;
  int hasMode;
};

struct EvalScratch
{
  z_stream zs_default;
  z_stream zs_filtered;
  unsigned char * zbuf_default;
  unsigned char * zbuf_filtered;
  unsigned char * row_buf;
  unsigned char * sub_row;
  unsigned char * up_row;
  unsigned char * avg_row;
  unsigned char * paeth_row;
  unsigned char * exact_rows;
  unsigned char * exact_zbuf;
  unsigned int zbuf_size;
  unsigned int rowbytes;
  size_t rowsbuf_size;
};

struct CandidateEval
{
  unsigned char * p;
  unsigned int estimate_size;
  unsigned int exact_size;
  int x;
  int y;
  int w;
  int h;
  int op_id;
  int filters;
  int valid;
  int exact_valid;
};

rgb             palette[256];
unsigned char   trns[256];
unsigned int    palsize, trnssize;
unsigned int    next_seq_num;

const unsigned long cMaxPNGSize = 1000000UL;
const size_t cMaxChunkDataLength = 0x7fffffffULL;

static bool checked_mul_size(size_t a, size_t b, size_t *out)
{
  if (!out)
    return false;
  if (a != 0 && b > (std::numeric_limits<size_t>::max() / a))
    return false;
  *out = a * b;
  return true;
}

static bool checked_add_size(size_t a, size_t b, size_t *out)
{
  if (!out)
    return false;
  if (a > (std::numeric_limits<size_t>::max() - b))
    return false;
  *out = a + b;
  return true;
}

static bool compute_image_layout(unsigned int w, unsigned int h, unsigned int bpp, size_t *rowbytes, size_t *imagesize)
{
  size_t rb = 0;
  size_t isz = 0;
  if (!checked_mul_size((size_t)w, (size_t)bpp, &rb))
    return false;
  if (!checked_mul_size(rb, (size_t)h, &isz))
    return false;
  if (rowbytes)
    *rowbytes = rb;
  if (imagesize)
    *imagesize = isz;
  return true;
}

static bool set_imagequant_thread_env(unsigned int threads)
{
  if (threads == 0)
    return true;
#ifdef _WIN32
  char buf[16];
  sprintf(buf, "%u", threads);
  return (_putenv_s("RAYON_NUM_THREADS", buf) == 0);
#else
  char buf[16];
  snprintf(buf, sizeof(buf), "%u", threads);
  return (setenv("RAYON_NUM_THREADS", buf, 1) == 0);
#endif
}

/* APNG decoder - begin */
void info_fn(png_structp png_ptr, png_infop info_ptr)
{
  png_set_expand(png_ptr);
  png_set_strip_16(png_ptr);
  png_set_gray_to_rgb(png_ptr);
  png_set_add_alpha(png_ptr, 0xff, PNG_FILLER_AFTER);
  (void)png_set_interlace_handling(png_ptr);
  png_read_update_info(png_ptr, info_ptr);
}

void row_fn(png_structp png_ptr, png_bytep new_row, png_uint_32 row_num, int pass)
{
  APNGFrame * frame = (APNGFrame *)png_get_progressive_ptr(png_ptr);
  png_progressive_combine_row(png_ptr, frame->rows[row_num], new_row);
}

void compose_frame(unsigned char ** rows_dst, unsigned char ** rows_src, unsigned char bop, unsigned int x, unsigned int y, unsigned int w, unsigned int h)
{
  unsigned int  i, j;
  int u, v, al;

  for (j=0; j<h; j++)
  {
    unsigned char * sp = rows_src[j];
    unsigned char * dp = rows_dst[j+y] + x*4;

    if (bop == 0)
      memcpy(dp, sp, w*4);
    else
    for (i=0; i<w; i++, sp+=4, dp+=4)
    {
      if (sp[3] == 255)
        memcpy(dp, sp, 4);
      else
      if (sp[3] != 0)
      {
        if (dp[3] != 0)
        {
          u = sp[3]*255;
          v = (255-sp[3])*dp[3];
          al = u + v;
          dp[0] = (sp[0]*u + dp[0]*v)/al;
          dp[1] = (sp[1]*u + dp[1]*v)/al;
          dp[2] = (sp[2]*u + dp[2]*v)/al;
          dp[3] = al/255;
        }
        else
          memcpy(dp, sp, 4);
      }
    }
  }
}

inline unsigned int read_chunk(FILE * f, CHUNK * pChunk)
{
  unsigned char len[4];
  pChunk->size = 0;
  pChunk->p = 0;
  if (fread(&len, 4, 1, f) == 1)
  {
    const size_t data_len = png_get_uint_32(len);
    size_t chunk_size = 0;
    if (data_len > cMaxChunkDataLength || !checked_add_size(data_len, 12, &chunk_size) || chunk_size > (size_t)UINT_MAX)
      return 0;

    pChunk->size = (unsigned int)chunk_size;
    pChunk->p = new unsigned char[pChunk->size];
    memcpy(pChunk->p, len, 4);
    if (fread(pChunk->p + 4, pChunk->size - 4, 1, f) == 1)
      return *(unsigned int *)(pChunk->p + 4);
    delete[] pChunk->p;
    pChunk->p = 0;
    pChunk->size = 0;
  }
  return 0;
}

int processing_start(png_structp & png_ptr, png_infop & info_ptr, void * frame_ptr, bool hasInfo, CHUNK & chunkIHDR, std::vector<CHUNK>& chunksInfo)
{
  unsigned char header[8] = {137, 80, 78, 71, 13, 10, 26, 10};

  png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if (!png_ptr)
    return 1;
  info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr)
  {
    png_destroy_read_struct(&png_ptr, 0, 0);
    return 1;
  }

  if (setjmp(png_jmpbuf(png_ptr)))
  {
    png_destroy_read_struct(&png_ptr, &info_ptr, 0);
    return 1;
  }

  png_set_crc_action(png_ptr, PNG_CRC_QUIET_USE, PNG_CRC_QUIET_USE);
  png_set_progressive_read_fn(png_ptr, frame_ptr, info_fn, row_fn, NULL);

  png_process_data(png_ptr, info_ptr, header, 8);
  png_process_data(png_ptr, info_ptr, chunkIHDR.p, chunkIHDR.size);

  if (hasInfo)
    for (unsigned int i=0; i<chunksInfo.size(); i++)
      png_process_data(png_ptr, info_ptr, chunksInfo[i].p, chunksInfo[i].size);
  return 0;
}

int processing_data(png_structp png_ptr, png_infop info_ptr, unsigned char * p, unsigned int size)
{
  if (!png_ptr || !info_ptr)
    return 1;

  if (setjmp(png_jmpbuf(png_ptr)))
  {
    png_destroy_read_struct(&png_ptr, &info_ptr, 0);
    return 1;
  }

  png_process_data(png_ptr, info_ptr, p, size);
  return 0;
}

int processing_finish(png_structp png_ptr, png_infop info_ptr)
{
  unsigned char footer[12] = {0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130};

  if (!png_ptr || !info_ptr)
    return 1;

  if (setjmp(png_jmpbuf(png_ptr)))
  {
    png_destroy_read_struct(&png_ptr, &info_ptr, 0);
    return 1;
  }

  png_process_data(png_ptr, info_ptr, footer, 12);
  png_destroy_read_struct(&png_ptr, &info_ptr, 0);

  return 0;
}

int load_apng(const char * szIn, std::vector<APNGFrame>& frames, unsigned int & first, unsigned int & loops)
{
  FILE * f;
  unsigned int id, i, j, w, h, w0, h0, x0, y0;
  unsigned int delay_num, delay_den, dop, bop, rowbytes, imagesize;
  unsigned char sig[8];
  png_structp png_ptr;
  png_infop info_ptr;
  CHUNK chunk;
  CHUNK chunkIHDR;
  std::vector<CHUNK> chunksInfo;
  bool isAnimated = false;
  bool hasInfo = false;
  APNGFrame frameRaw = {0};
  APNGFrame frameCur = {0};
  APNGFrame frameNext = {0};
  int res = -1;
  first = 0;

  printf("Reading '%s'...\n", szIn);

  if ((f = fopen(szIn, "rb")) != 0)
  {
    if (fread(sig, 1, 8, f) == 8 && png_sig_cmp(sig, 0, 8) == 0)
    {
      id = read_chunk(f, &chunkIHDR);

      if (id == id_IHDR && chunkIHDR.size == 25)
      {
        w0 = w = png_get_uint_32(chunkIHDR.p + 8);
        h0 = h = png_get_uint_32(chunkIHDR.p + 12);

        if (w > cMaxPNGSize || h > cMaxPNGSize)
        {
          fclose(f);
          return res;
        }

        x0 = 0;
        y0 = 0;
        delay_num = 1;
        delay_den = 10;
        dop = 0;
        bop = 0;
        size_t rowbytes_sz = 0;
        size_t imagesize_sz = 0;
        if (!compute_image_layout(w, h, 4, &rowbytes_sz, &imagesize_sz) ||
            rowbytes_sz > (size_t)UINT_MAX || imagesize_sz > (size_t)UINT_MAX)
        {
          delete[] chunkIHDR.p;
          fclose(f);
          return res;
        }
        rowbytes = (unsigned int)rowbytes_sz;
        imagesize = (unsigned int)imagesize_sz;

        frameRaw.p = new unsigned char[imagesize];
        frameRaw.rows = new png_bytep[h];
        for (j=0; j<h; j++)
          frameRaw.rows[j] = frameRaw.p + j * rowbytes;

        if (!processing_start(png_ptr, info_ptr, (void *)&frameRaw, hasInfo, chunkIHDR, chunksInfo))
        {
          frameCur.w = w;
          frameCur.h = h;
          frameCur.p = new unsigned char[imagesize];
          frameCur.rows = new png_bytep[h];
          for (j=0; j<h; j++)
            frameCur.rows[j] = frameCur.p + j * rowbytes;

          while ( !feof(f) )
          {
            id = read_chunk(f, &chunk);
            if (!id)
              break;

            if (id == id_acTL && !hasInfo && !isAnimated)
            {
              isAnimated = true;
              first = 1;
              loops = png_get_uint_32(chunk.p + 12);
            }
            else
            if (id == id_fcTL && (!hasInfo || isAnimated))
            {
              if (hasInfo)
              {
                if (!processing_finish(png_ptr, info_ptr))
                {
                  frameNext.p = new unsigned char[imagesize];
                  frameNext.rows = new png_bytep[h];
                  for (j=0; j<h; j++)
                    frameNext.rows[j] = frameNext.p + j * rowbytes;

                  if (dop == 2)
                    memcpy(frameNext.p, frameCur.p, imagesize);

                  compose_frame(frameCur.rows, frameRaw.rows, bop, x0, y0, w0, h0);
                  frameCur.delay_num = delay_num;
                  frameCur.delay_den = delay_den;

                  frames.push_back(frameCur);

                  if (dop != 2)
                  {
                    memcpy(frameNext.p, frameCur.p, imagesize);
                    if (dop == 1)
                      for (j=0; j<h0; j++)
                        memset(frameNext.rows[y0 + j] + x0*4, 0, w0*4);
                  }
                  frameCur.p = frameNext.p;
                  frameCur.rows = frameNext.rows;
                }
                else
                {
                  delete[] frameCur.rows;
                  delete[] frameCur.p;
                  delete[] chunk.p;
                  break;
                }
              }

              // At this point the old frame is done. Let's start a new one.
              w0 = png_get_uint_32(chunk.p + 12);
              h0 = png_get_uint_32(chunk.p + 16);
              x0 = png_get_uint_32(chunk.p + 20);
              y0 = png_get_uint_32(chunk.p + 24);
              delay_num = png_get_uint_16(chunk.p + 28);
              delay_den = png_get_uint_16(chunk.p + 30);
              dop = chunk.p[32];
              bop = chunk.p[33];

              if (w0 > cMaxPNGSize || h0 > cMaxPNGSize || x0 > cMaxPNGSize || y0 > cMaxPNGSize
                  || x0 + w0 > w || y0 + h0 > h || dop > 2 || bop > 1)
              {
                delete[] frameCur.rows;
                delete[] frameCur.p;
                delete[] chunk.p;
                break;
              }

              if (hasInfo)
              {
                memcpy(chunkIHDR.p + 8, chunk.p + 12, 8);
                if (processing_start(png_ptr, info_ptr, (void *)&frameRaw, hasInfo, chunkIHDR, chunksInfo))
                {
                  delete[] frameCur.rows;
                  delete[] frameCur.p;
                  delete[] chunk.p;
                  break;
                }
              }
              else
                first = 0;

              if (frames.size() == first)
              {
                bop = 0;
                if (dop == 2)
                  dop = 1;
              }
            }
            else
            if (id == id_IDAT)
            {
              hasInfo = true;
              if (processing_data(png_ptr, info_ptr, chunk.p, chunk.size))
              {
                delete[] frameCur.rows;
                delete[] frameCur.p;
                delete[] chunk.p;
                break;
              }
            }
            else
            if (id == id_fdAT && isAnimated)
            {
              png_save_uint_32(chunk.p + 4, chunk.size - 16);
              memcpy(chunk.p + 8, "IDAT", 4);
              if (processing_data(png_ptr, info_ptr, chunk.p + 4, chunk.size - 4))
              {
                delete[] frameCur.rows;
                delete[] frameCur.p;
                delete[] chunk.p;
                break;
              }
            }
            else
            if (id == id_IEND)
            {
              if (hasInfo && !processing_finish(png_ptr, info_ptr))
              {
                compose_frame(frameCur.rows, frameRaw.rows, bop, x0, y0, w0, h0);
                frameCur.delay_num = delay_num;
                frameCur.delay_den = delay_den;
                frames.push_back(frameCur);
              }
              else
              {
                delete[] frameCur.rows;
                delete[] frameCur.p;
              }
              delete[] chunk.p;
              break;
            }
            else
            if (notabc(chunk.p[4]) || notabc(chunk.p[5]) || notabc(chunk.p[6]) || notabc(chunk.p[7]))
            {
              delete[] chunk.p;
              break;
            }
            else
            if (!hasInfo)
            {
              if (processing_data(png_ptr, info_ptr, chunk.p, chunk.size))
              {
                delete[] frameCur.rows;
                delete[] frameCur.p;
                delete[] chunk.p;
                break;
              }
              chunksInfo.push_back(chunk);
              continue;
            }
            delete[] chunk.p;
          }
        }
        delete[] frameRaw.rows;
        delete[] frameRaw.p;

        if (!frames.empty())
          res = 0;
      }

      for (i=0; i<chunksInfo.size(); i++)
        delete[] chunksInfo[i].p;

      chunksInfo.clear();
      delete[] chunkIHDR.p;
    }
    fclose(f);
  }

  return res;
}
/* APNG decoder - end */

void optim_dirty(std::vector<APNGFrame>& frames)
{
  unsigned int i, j;
  unsigned char * sp;
  unsigned int size = frames[0].w * frames[0].h;

  for (i=0; i<frames.size(); i++)
  {
    sp = frames[i].p;
    for (j=0; j<size; j++, sp+=4)
      if (sp[3] == 0)
         sp[0] = sp[1] = sp[2] = 0;
  }
}

void optim_dirty_mt(std::vector<APNGFrame>& frames, unsigned int threads)
{
  if (threads <= 1 || frames.size() < 2)
  {
    optim_dirty(frames);
    return;
  }

  const unsigned int size = frames[0].w * frames[0].h;
  if (threads > frames.size())
    threads = (unsigned int)frames.size();
  if (threads < 1)
    threads = 1;

  std::vector<std::thread> workers;
  workers.reserve(threads);

  const unsigned int chunk = (unsigned int)((frames.size() + threads - 1) / threads);
  for (unsigned int t = 0; t < threads; t++)
  {
    const unsigned int begin = t * chunk;
    unsigned int end = begin + chunk;
    if (begin >= frames.size())
      break;
    if (end > frames.size())
      end = (unsigned int)frames.size();

    workers.emplace_back([&frames, size, begin, end]() {
      for (unsigned int i = begin; i < end; i++)
      {
        unsigned char * sp = frames[i].p;
        for (unsigned int j = 0; j < size; j++, sp += 4)
        {
          if (sp[3] == 0)
            sp[0] = sp[1] = sp[2] = 0;
        }
      }
    });
  }

  for (unsigned int t = 0; t < workers.size(); t++)
    workers[t].join();
}

void optim_duplicates(std::vector<APNGFrame>& frames, unsigned int first)
{
  unsigned int imagesize = frames[0].w * frames[0].h * 4;
  unsigned int i = first;

  while (++i < frames.size())
  {
    if (memcmp(frames[i-1].p, frames[i].p, imagesize) != 0)
      continue;

    i--;
    delete[] frames[i].p;
    delete[] frames[i].rows;
    unsigned int num = frames[i].delay_num;
    unsigned int den = frames[i].delay_den;
    frames.erase(frames.begin() + i);

    if (frames[i].delay_den == den)
      frames[i].delay_num += num;
    else
    {
      frames[i].delay_num = num = num*frames[i].delay_den + den*frames[i].delay_num;
      frames[i].delay_den = den = den*frames[i].delay_den;
      while (num && den)
      {
        if (num > den)
          num = num % den;
        else
          den = den % num;
      }
      num += den;
      frames[i].delay_num /= num;
      frames[i].delay_den /= num;
    }
  }
}

/* APNG encoder - begin */
int cmp_colors( const void *arg1, const void *arg2 )
{
  if ( ((COLORS*)arg1)->a != ((COLORS*)arg2)->a )
    return (int)(((COLORS*)arg1)->a) - (int)(((COLORS*)arg2)->a);

  if ( ((COLORS*)arg1)->num != ((COLORS*)arg2)->num )
    return (int)(((COLORS*)arg2)->num) - (int)(((COLORS*)arg1)->num);

  if ( ((COLORS*)arg1)->r != ((COLORS*)arg2)->r )
    return (int)(((COLORS*)arg1)->r) - (int)(((COLORS*)arg2)->r);

  if ( ((COLORS*)arg1)->g != ((COLORS*)arg2)->g )
    return (int)(((COLORS*)arg1)->g) - (int)(((COLORS*)arg2)->g);

  return (int)(((COLORS*)arg1)->b) - (int)(((COLORS*)arg2)->b);
}

void optim_downconvert(std::vector<APNGFrame>& frames, unsigned int & coltype)
{
  unsigned int  i, j, k, r, g, b, a;
  unsigned char * sp, * dp;
  unsigned char cube[4096];
  unsigned char gray[256];
  COLORS        col[256];
  unsigned int  colors = 0;
  unsigned int  size = frames[0].w * frames[0].h;
  unsigned int  has_tcolor = 0;
  unsigned int  num_frames = frames.size();

  memset(&cube, 0, sizeof(cube));
  memset(&gray, 0, sizeof(gray));

  for (i=0; i<256; i++)
  {
    col[i].num = 0;
    col[i].r = col[i].g = col[i].b = i;
    col[i].a = trns[i] = 255;
  }
  palsize = trnssize = 0;
  coltype = 6;

  int transparent = 255;
  int simple_trans = 1;
  int grayscale = 1;

  for (i=0; i<num_frames; i++)
  {
    sp = frames[i].p;
    for (j=0; j<size; j++)
    {
      r = *sp++;
      g = *sp++;
      b = *sp++;
      a = *sp++;
      transparent &= a;

      if (a != 0)
      {
        if (a != 255)
          simple_trans = 0;
        else
          if (((r | g | b) & 15) == 0)
            cube[(r<<4) + g + (b>>4)] = 1;

        if (r != g || g != b)
          grayscale = 0;
        else
          gray[r] = 1;
      }

      if (colors <= 256)
      {
        int found = 0;
        for (k=0; k<colors; k++)
        if (col[k].r == r && col[k].g == g && col[k].b == b && col[k].a == a)
        {
          found = 1;
          col[k].num++;
          break;
        }
        if (found == 0)
        {
          if (colors < 256)
          {
            col[colors].num++;
            col[colors].r = r;
            col[colors].g = g;
            col[colors].b = b;
            col[colors].a = a;
            if (a == 0) has_tcolor = 1;
          }
          colors++;
        }
      }
    }
  }

  if (grayscale && simple_trans && colors<=256) /* 6 -> 0 */
  {
    coltype = 0;

    for (i=0; i<256; i++)
    if (gray[i] == 0)
    {
      trns[0] = 0;
      trns[1] = i;
      trnssize = 2;
      break;
    }

    for (i=0; i<num_frames; i++)
    {
      sp = dp = frames[i].p;
      for (j=0; j<size; j++, sp+=4)
      {
        if (sp[3] == 0)
          *dp++ = trns[1];
        else
          *dp++ = sp[0];
      }
    }
  }
  else
  if (colors<=256)   /* 6 -> 3 */
  {
    coltype = 3;

    if (has_tcolor==0 && colors<256)
      col[colors++].a = 0;

    qsort(&col[0], colors, sizeof(COLORS), cmp_colors);

    palsize = colors;
    for (i=0; i<colors; i++)
    {
      palette[i].r = col[i].r;
      palette[i].g = col[i].g;
      palette[i].b = col[i].b;
      trns[i]      = col[i].a;
      if (trns[i] != 255) trnssize = i+1;
    }

    for (i=0; i<num_frames; i++)
    {
      sp = dp = frames[i].p;
      for (j=0; j<size; j++)
      {
        r = *sp++;
        g = *sp++;
        b = *sp++;
        a = *sp++;
        for (k=0; k<colors; k++)
          if (col[k].r == r && col[k].g == g && col[k].b == b && col[k].a == a)
            break;
        *dp++ = k;
      }
    }
  }
  else
  if (grayscale)     /* 6 -> 4 */
  {
    coltype = 4;
    for (i=0; i<num_frames; i++)
    {
      sp = dp = frames[i].p;
      for (j=0; j<size; j++, sp+=4)
      {
        *dp++ = sp[2];
        *dp++ = sp[3];
      }
    }
  }
  else
  if (simple_trans)  /* 6 -> 2 */
  {
    for (i=0; i<4096; i++)
    if (cube[i] == 0)
    {
      trns[0] = 0;
      trns[1] = (i>>4)&0xF0;
      trns[2] = 0;
      trns[3] = i&0xF0;
      trns[4] = 0;
      trns[5] = (i<<4)&0xF0;
      trnssize = 6;
      break;
    }
    if (transparent == 255)
    {
      coltype = 2;
      for (i=0; i<num_frames; i++)
      {
        sp = dp = frames[i].p;
        for (j=0; j<size; j++)
        {
          *dp++ = *sp++;
          *dp++ = *sp++;
          *dp++ = *sp++;
          sp++;
        }
      }
    }
    else
    if (trnssize != 0)
    {
      coltype = 2;
      for (i=0; i<num_frames; i++)
      {
        sp = dp = frames[i].p;
        for (j=0; j<size; j++)
        {
          r = *sp++;
          g = *sp++;
          b = *sp++;
          a = *sp++;
          if (a == 0)
          {
            *dp++ = trns[1];
            *dp++ = trns[3];
            *dp++ = trns[5];
          }
          else
          {
            *dp++ = r;
            *dp++ = g;
            *dp++ = b;
          }
        }
      }
    }
  }
}
static bool apply_imagequant_options(liq_attr *attr, int minQuality, int maxQuality, int liqSpeed, int liqMaxColors, int liqPosterization)
{
  if (!attr)
    return false;
  if (liq_set_quality(attr, minQuality, maxQuality) != LIQ_OK)
    return false;
  if (liqSpeed > 0 && liq_set_speed(attr, liqSpeed) != LIQ_OK)
    return false;
  if (liqMaxColors > 0 && liq_set_max_colors(attr, liqMaxColors) != LIQ_OK)
    return false;
  if (liqPosterization >= 0 && liq_set_min_posterization(attr, liqPosterization) != LIQ_OK)
    return false;
  return true;
}

bool optim_image(std::vector<APNGFrame>& frames, unsigned int & coltype, int minQuality, int maxQuality, int liqSpeed, int liqMaxColors, int liqPosterization, float liqDither)
{
    unsigned int size = frames.size();
    if (size == 0)
      return false;

    unsigned int width = frames[0].w;
    unsigned int height = frames[0].h;
    size_t imageSize = 0;
    if (!compute_image_layout(width, height, 4, 0, &imageSize))
      return false;
    if (imageSize > (size_t)INT_MAX)
      return false;

    liq_attr *attr = liq_attr_create();
    if (!attr)
      return false;

    if (!apply_imagequant_options(attr, minQuality, maxQuality, liqSpeed, liqMaxColors, liqPosterization))
    {
      liq_attr_destroy(attr);
      return false;
    }

    liq_histogram *hist = liq_histogram_create(attr);
    if (!hist)
    {
      liq_attr_destroy(attr);
      return false;
    }

    std::vector<liq_image*> images(size, NULL);
    std::vector<unsigned char> indexed;
    bool ok = true;

    for (unsigned int i = 0; i < size; i++)
    {
      liq_image *image = liq_image_create_rgba(attr, frames[i].p, width, height, 0);
      if (!image || liq_histogram_add_image(hist, attr, image) != LIQ_OK)
      {
        if (image)
          liq_image_destroy(image);
        ok = false;
        break;
      }

      images[i] = image;
      /*process_callback(0.3 + i / float(size) * 0.1);*/
    }

    if (ok)
    {
      liq_result *res = NULL;
      liq_error qerr = liq_histogram_quantize(hist, attr, &res);
      if (qerr == LIQ_OK && res)
      {
        liq_error dither_err = liq_set_dithering_level(res, liqDither);
        (void)dither_err;
        if (ok)
        {
          size_t total_indexed = 0;
          if (!checked_mul_size(imageSize, (size_t)size, &total_indexed))
            ok = false;
          else
            indexed.resize(total_indexed);
        }

        for (unsigned int i = 0; i < size; i++)
        {
          unsigned char *dst = indexed.data() + ((size_t)i * imageSize);
          liq_error remap_err = liq_write_remapped_image(res, images[i], dst, imageSize);
          if (!ok || remap_err != LIQ_OK)
          {
            ok = false;
            break;
          }
        }

        if (ok)
        {
          const liq_palette *liqPalette = liq_get_palette(res);
          palsize = liqPalette->count;
          trnssize = 0;
          for (unsigned int i = 0; i < liqPalette->count; i++)
          {
            palette[i].r = liqPalette->entries[i].r;
            palette[i].g = liqPalette->entries[i].g;
            palette[i].b = liqPalette->entries[i].b;
            trns[i] = liqPalette->entries[i].a;
            if (trns[i] != 255)
              trnssize = i + 1;

            /*process_callback(0.4 + i / float(palsize) * 0.1);*/
          }

          for (unsigned int i = 0; i < size; i++)
          {
            memcpy(frames[i].p, indexed.data() + ((size_t)i * imageSize), imageSize);
          }
          coltype = 3;
        }
      }
      else if (qerr == LIQ_QUALITY_TOO_LOW && minQuality > 0)
      {
        liq_result *retry_res = NULL;
        if (liq_set_quality(attr, 0, maxQuality) == LIQ_OK &&
            liq_histogram_quantize(hist, attr, &retry_res) == LIQ_OK && retry_res)
        {
          liq_set_dithering_level(retry_res, liqDither);
          size_t total_indexed = 0;
          if (checked_mul_size(imageSize, (size_t)size, &total_indexed))
            indexed.resize(total_indexed);
          else
            ok = false;

          for (unsigned int i = 0; ok && i < size; i++)
          {
            unsigned char *dst = indexed.data() + ((size_t)i * imageSize);
            if (liq_write_remapped_image(retry_res, images[i], dst, imageSize) != LIQ_OK)
              ok = false;
          }

          if (ok)
          {
            const liq_palette *liqPalette = liq_get_palette(retry_res);
            palsize = liqPalette->count;
            trnssize = 0;
            for (unsigned int i = 0; i < liqPalette->count; i++)
            {
              palette[i].r = liqPalette->entries[i].r;
              palette[i].g = liqPalette->entries[i].g;
              palette[i].b = liqPalette->entries[i].b;
              trns[i] = liqPalette->entries[i].a;
              if (trns[i] != 255)
                trnssize = i + 1;
            }
            for (unsigned int i = 0; i < size; i++)
              memcpy(frames[i].p, indexed.data() + ((size_t)i * imageSize), imageSize);
            coltype = 3;
          }
        }
        else
          ok = false;
        if (retry_res)
          liq_result_destroy(retry_res);
      }
      else
      {
        ok = false;
      }

      if (res)
        liq_result_destroy(res);
    }

    for (unsigned int i = 0; i < size; i++)
      if (images[i])
        liq_image_destroy(images[i]);

    liq_histogram_destroy(hist);
    liq_attr_destroy(attr);
    return ok;
}
bool write_chunk(FILE * f, const char * name, unsigned char * data, unsigned int length)
{
  if (!f || !name)
    return false;

  unsigned char buf[4];
  unsigned int crc = crc32(0, Z_NULL, 0);

  png_save_uint_32(buf, length);
  if (fwrite(buf, 1, 4, f) != 4) return false;
  if (fwrite(name, 1, 4, f) != 4) return false;
  crc = crc32(crc, (const Bytef *)name, 4);

  if (memcmp(name, "fdAT", 4) == 0)
  {
    if (length < 4)
      return false;
    png_save_uint_32(buf, next_seq_num++);
    if (fwrite(buf, 1, 4, f) != 4) return false;
    crc = crc32(crc, buf, 4);
    length -= 4;
  }

  if (data != NULL && length > 0)
  {
    if (fwrite(data, 1, length, f) != length) return false;
    crc = crc32(crc, data, length);
  }

  png_save_uint_32(buf, crc);
  if (fwrite(buf, 1, 4, f) != 4) return false;
  return true;
}

bool write_IDATs(FILE * f, int frame, unsigned char * data, unsigned int length, unsigned int idat_size)
{
  if (!f || !data || length == 0)
    return false;
  unsigned int z_cmf = data[0];
  if ((z_cmf & 0x0f) == 8 && (z_cmf & 0xf0) <= 0x70)
  {
    if (length >= 2)
    {
      unsigned int z_cinfo = z_cmf >> 4;
      unsigned int half_z_window_size = 1 << (z_cinfo + 7);
      while (idat_size <= half_z_window_size && half_z_window_size >= 256)
      {
        z_cinfo--;
        half_z_window_size >>= 1;
      }
      z_cmf = (z_cmf & 0x0f) | (z_cinfo << 4);
      if (data[0] != (unsigned char)z_cmf)
      {
        data[0] = (unsigned char)z_cmf;
        data[1] &= 0xe0;
        data[1] += (unsigned char)(0x1f - ((z_cmf << 8) + data[1]) % 0x1f);
      }
    }
  }

  while (length > 0)
  {
    unsigned int ds = length;
    if (ds > 32768)
      ds = 32768;

    if (frame == 0)
    {
      if (!write_chunk(f, "IDAT", data, ds)) return false;
    }
    else
    {
      if (!write_chunk(f, "fdAT", data, ds+4)) return false;
    }

    data += ds;
    length -= ds;
  }
  return true;
}

bool init_eval_scratch(EvalScratch& scratch, unsigned int rowbytes, unsigned int zbuf_size, size_t rowsbuf_size, int need_exact)
{
  memset(&scratch, 0, sizeof(EvalScratch));
  scratch.rowbytes = rowbytes;
  scratch.zbuf_size = zbuf_size;
  scratch.rowsbuf_size = rowsbuf_size;

  scratch.zbuf_default = new unsigned char[zbuf_size];
  scratch.zbuf_filtered = new unsigned char[zbuf_size];
  scratch.row_buf = new unsigned char[rowbytes + 1];
  scratch.sub_row = new unsigned char[rowbytes + 1];
  scratch.up_row = new unsigned char[rowbytes + 1];
  scratch.avg_row = new unsigned char[rowbytes + 1];
  scratch.paeth_row = new unsigned char[rowbytes + 1];
  if (!scratch.zbuf_default || !scratch.zbuf_filtered || !scratch.row_buf || !scratch.sub_row || !scratch.up_row || !scratch.avg_row || !scratch.paeth_row)
    return false;

  if (need_exact)
  {
    scratch.exact_rows = new unsigned char[rowsbuf_size];
    scratch.exact_zbuf = new unsigned char[zbuf_size];
    if (!scratch.exact_rows || !scratch.exact_zbuf)
      return false;
  }

  scratch.row_buf[0] = 0;
  scratch.sub_row[0] = 1;
  scratch.up_row[0] = 2;
  scratch.avg_row[0] = 3;
  scratch.paeth_row[0] = 4;

  scratch.zs_default.data_type = Z_BINARY;
  scratch.zs_default.zalloc = Z_NULL;
  scratch.zs_default.zfree = Z_NULL;
  scratch.zs_default.opaque = Z_NULL;
  if (deflateInit2(&scratch.zs_default, Z_BEST_SPEED+1, 8, 15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
    return false;

  scratch.zs_filtered.data_type = Z_BINARY;
  scratch.zs_filtered.zalloc = Z_NULL;
  scratch.zs_filtered.zfree = Z_NULL;
  scratch.zs_filtered.opaque = Z_NULL;
  if (deflateInit2(&scratch.zs_filtered, Z_BEST_SPEED+1, 8, 15, 8, Z_FILTERED) != Z_OK)
    return false;

  return true;
}

void free_eval_scratch(EvalScratch& scratch)
{
  if (scratch.zs_default.state)
    deflateEnd(&scratch.zs_default);
  if (scratch.zs_filtered.state)
    deflateEnd(&scratch.zs_filtered);

  delete[] scratch.zbuf_default;
  delete[] scratch.zbuf_filtered;
  delete[] scratch.row_buf;
  delete[] scratch.sub_row;
  delete[] scratch.up_row;
  delete[] scratch.avg_row;
  delete[] scratch.paeth_row;
  delete[] scratch.exact_rows;
  delete[] scratch.exact_zbuf;
  memset(&scratch, 0, sizeof(EvalScratch));
}

template <typename Fn>
void run_parallel_indices(unsigned int count, unsigned int threads, Fn fn)
{
  if (count == 0)
    return;
  if (threads <= 1 || count == 1)
  {
    fn(0, count, 0);
    return;
  }

  if (threads > count)
    threads = count;
  if (threads < 1)
    threads = 1;

  const unsigned int chunk = (count + threads - 1) / threads;
  std::vector<std::thread> workers;
  workers.reserve(threads);
  for (unsigned int t = 0; t < threads; t++)
  {
    const unsigned int begin = t * chunk;
    unsigned int end = begin + chunk;
    if (begin >= count)
      break;
    if (end > count)
      end = count;
    workers.emplace_back([begin, end, t, &fn]() { fn(begin, end, t); });
  }
  for (unsigned int t = 0; t < workers.size(); t++)
    workers[t].join();
}

bool process_rect_with_scratch(EvalScratch& scratch, unsigned char * row, int rowbytes, int bpp, int stride, int h, unsigned char * rows)
{
  int i, j, v;
  int a, b, c, pa, pb, pc, p;
  unsigned char * prev = NULL;
  unsigned char * dp  = rows;
  unsigned char * out;

  for (j=0; j<h; j++)
  {
    unsigned int    sum = 0;
    unsigned char * best_row = scratch.row_buf;
    unsigned int    mins = ((unsigned int)(-1)) >> 1;

    out = scratch.row_buf+1;
    for (i=0; i<rowbytes; i++)
    {
      v = out[i] = row[i];
      sum += (v < 128) ? v : 256 - v;
    }
    mins = sum;

    sum = 0;
    out = scratch.sub_row+1;
    for (i=0; i<bpp; i++)
    {
      v = out[i] = row[i];
      sum += (v < 128) ? v : 256 - v;
    }
    for (i=bpp; i<rowbytes; i++)
    {
      v = out[i] = row[i] - row[i-bpp];
      sum += (v < 128) ? v : 256 - v;
      if (sum > mins) break;
    }
    if (sum < mins)
    {
      mins = sum;
      best_row = scratch.sub_row;
    }

    if (prev)
    {
      sum = 0;
      out = scratch.up_row+1;
      for (i=0; i<rowbytes; i++)
      {
        v = out[i] = row[i] - prev[i];
        sum += (v < 128) ? v : 256 - v;
        if (sum > mins) break;
      }
      if (sum < mins)
      {
        mins = sum;
        best_row = scratch.up_row;
      }

      sum = 0;
      out = scratch.avg_row+1;
      for (i=0; i<bpp; i++)
      {
        v = out[i] = row[i] - prev[i]/2;
        sum += (v < 128) ? v : 256 - v;
      }
      for (i=bpp; i<rowbytes; i++)
      {
        v = out[i] = row[i] - (prev[i] + row[i-bpp])/2;
        sum += (v < 128) ? v : 256 - v;
        if (sum > mins) break;
      }
      if (sum < mins)
      {
        mins = sum;
        best_row = scratch.avg_row;
      }

      sum = 0;
      out = scratch.paeth_row+1;
      for (i=0; i<bpp; i++)
      {
        v = out[i] = row[i] - prev[i];
        sum += (v < 128) ? v : 256 - v;
      }
      for (i=bpp; i<rowbytes; i++)
      {
        a = row[i-bpp];
        b = prev[i];
        c = prev[i-bpp];
        p = b - c;
        pc = a - c;
        pa = abs(p);
        pb = abs(pc);
        pc = abs(p + pc);
        p = (pa <= pb && pa <=pc) ? a : (pb <= pc) ? b : c;
        v = out[i] = row[i] - p;
        sum += (v < 128) ? v : 256 - v;
        if (sum > mins) break;
      }
      if (sum < mins)
        best_row = scratch.paeth_row;
    }

    if (rows == NULL)
    {
      scratch.zs_default.next_in = scratch.row_buf;
      scratch.zs_default.avail_in = rowbytes + 1;
      if (deflate(&scratch.zs_default, Z_NO_FLUSH) != Z_OK)
        return false;

      scratch.zs_filtered.next_in = best_row;
      scratch.zs_filtered.avail_in = rowbytes + 1;
      if (deflate(&scratch.zs_filtered, Z_NO_FLUSH) != Z_OK)
        return false;
    }
    else
    {
      memcpy(dp, best_row, rowbytes+1);
      dp += rowbytes+1;
    }

    prev = row;
    row += stride;
  }
  return true;
}

bool evaluate_candidate(EvalScratch& scratch, CandidateEval& cand, int bpp, int stride, int zbuf_size)
{
  cand.valid = 0;
  cand.exact_valid = 0;

  if (cand.w <= 0 || cand.h <= 0)
    return false;

  unsigned char * row  = cand.p + cand.y*stride + cand.x*bpp;
  int rowbytes = cand.w * bpp;
  if (rowbytes < 1)
    return false;

  scratch.zs_default.data_type = Z_BINARY;
  scratch.zs_default.next_out = scratch.zbuf_default;
  scratch.zs_default.avail_out = zbuf_size;
  scratch.zs_filtered.data_type = Z_BINARY;
  scratch.zs_filtered.next_out = scratch.zbuf_filtered;
  scratch.zs_filtered.avail_out = zbuf_size;

  if (!process_rect_with_scratch(scratch, row, rowbytes, bpp, stride, cand.h, NULL))
  {
    deflateReset(&scratch.zs_default);
    deflateReset(&scratch.zs_filtered);
    return false;
  }

  int z1 = deflate(&scratch.zs_default, Z_FINISH);
  int z2 = deflate(&scratch.zs_filtered, Z_FINISH);
  if (z1 != Z_STREAM_END || z2 != Z_STREAM_END)
  {
    deflateReset(&scratch.zs_default);
    deflateReset(&scratch.zs_filtered);
    return false;
  }

  if (scratch.zs_default.total_out < scratch.zs_filtered.total_out)
  {
    cand.estimate_size = (unsigned int)scratch.zs_default.total_out;
    cand.filters = 0;
  }
  else
  {
    cand.estimate_size = (unsigned int)scratch.zs_filtered.total_out;
    cand.filters = 1;
  }
  cand.valid = 1;
  deflateReset(&scratch.zs_default);
  deflateReset(&scratch.zs_filtered);
  return true;
}

void get_rect_candidates(unsigned int w, unsigned int h, unsigned char *pimage1, unsigned char *pimage2, unsigned char *ptemp, unsigned int bpp, unsigned int stride, unsigned int has_tcolor, unsigned int tcolor, int dispose_code, std::vector<CandidateEval>& out)
{
  unsigned int   i, j, x0, y0, w0, h0;
  unsigned int   x_min = w-1;
  unsigned int   y_min = h-1;
  unsigned int   x_max = 0;
  unsigned int   y_max = 0;
  unsigned int   diffnum = 0;
  unsigned int   over_is_possible = 1;

  if (!has_tcolor)
    over_is_possible = 0;

  if (bpp == 1)
  {
    unsigned char *pa = pimage1;
    unsigned char *pb = pimage2;
    unsigned char *pc = ptemp;

    for (j=0; j<h; j++)
    for (i=0; i<w; i++)
    {
      unsigned char c = *pb++;
      if (*pa++ != c)
      {
        diffnum++;
        if (has_tcolor && c == tcolor) over_is_possible = 0;
        if (i<x_min) x_min = i;
        if (i>x_max) x_max = i;
        if (j<y_min) y_min = j;
        if (j>y_max) y_max = j;
      }
      else
        c = tcolor;

      *pc++ = c;
    }
  }
  else
  if (bpp == 2)
  {
    unsigned short *pa = (unsigned short *)pimage1;
    unsigned short *pb = (unsigned short *)pimage2;
    unsigned short *pc = (unsigned short *)ptemp;

    for (j=0; j<h; j++)
    for (i=0; i<w; i++)
    {
      unsigned int c1 = *pa++;
      unsigned int c2 = *pb++;
      if ((c1 != c2) && ((c1>>8) || (c2>>8)))
      {
        diffnum++;
        if ((c2 >> 8) != 0xFF) over_is_possible = 0;
        if (i<x_min) x_min = i;
        if (i>x_max) x_max = i;
        if (j<y_min) y_min = j;
        if (j>y_max) y_max = j;
      }
      else
        c2 = 0;

      *pc++ = c2;
    }
  }
  else
  if (bpp == 3)
  {
    unsigned char *pa = pimage1;
    unsigned char *pb = pimage2;
    unsigned char *pc = ptemp;

    for (j=0; j<h; j++)
    for (i=0; i<w; i++)
    {
      unsigned int c1 = (pa[2]<<16) + (pa[1]<<8) + pa[0];
      unsigned int c2 = (pb[2]<<16) + (pb[1]<<8) + pb[0];
      if (c1 != c2)
      {
        diffnum++;
        if (has_tcolor && c2 == tcolor) over_is_possible = 0;
        if (i<x_min) x_min = i;
        if (i>x_max) x_max = i;
        if (j<y_min) y_min = j;
        if (j>y_max) y_max = j;
      }
      else
        c2 = tcolor;

      memcpy(pc, &c2, 3);
      pa += 3;
      pb += 3;
      pc += 3;
    }
  }
  else
  if (bpp == 4)
  {
    unsigned int *pa = (unsigned int *)pimage1;
    unsigned int *pb = (unsigned int *)pimage2;
    unsigned int *pc = (unsigned int *)ptemp;

    for (j=0; j<h; j++)
    for (i=0; i<w; i++)
    {
      unsigned int c1 = *pa++;
      unsigned int c2 = *pb++;
      if ((c1 != c2) && ((c1>>24) || (c2>>24)))
      {
        diffnum++;
        if ((c2 >> 24) != 0xFF) over_is_possible = 0;
        if (i<x_min) x_min = i;
        if (i>x_max) x_max = i;
        if (j<y_min) y_min = j;
        if (j>y_max) y_max = j;
      }
      else
        c2 = 0;

      *pc++ = c2;
    }
  }

  if (diffnum == 0)
  {
    x0 = y0 = 0;
    w0 = h0 = 1;
  }
  else
  {
    x0 = x_min;
    y0 = y_min;
    w0 = x_max-x_min+1;
    h0 = y_max-y_min+1;
  }

  CandidateEval c0;
  memset(&c0, 0, sizeof(CandidateEval));
  c0.p = pimage2;
  c0.x = (int)x0;
  c0.y = (int)y0;
  c0.w = (int)w0;
  c0.h = (int)h0;
  c0.op_id = dispose_code * 2;
  out.push_back(c0);

  if (over_is_possible)
  {
    CandidateEval c1;
    memset(&c1, 0, sizeof(CandidateEval));
    c1.p = ptemp;
    c1.x = (int)x0;
    c1.y = (int)y0;
    c1.w = (int)w0;
    c1.h = (int)h0;
    c1.op_id = dispose_code * 2 + 1;
    out.push_back(c1);
  }
}

bool compress_candidate_exact(EvalScratch& scratch, CandidateEval& cand, int deflate_method, int iter, int bpp, int stride, int zbuf_size)
{
  cand.exact_valid = 0;
  if (deflate_method == 0)
    return false;
  if (!cand.valid || !scratch.exact_rows || !scratch.exact_zbuf || cand.w <= 0 || cand.h <= 0)
    return false;

  unsigned char * row  = cand.p + cand.y*stride + cand.x*bpp;
  int rowbytes = cand.w * bpp;
  size_t filtered_size = 0;
  if (!checked_add_size((size_t)rowbytes, 1, &filtered_size) || !checked_mul_size(filtered_size, (size_t)cand.h, &filtered_size))
    return false;
  if (filtered_size > scratch.rowsbuf_size || filtered_size > (size_t)INT_MAX)
    return false;

  if (cand.filters == 0)
  {
    unsigned char * dp = scratch.exact_rows;
    for (int j=0; j<cand.h; j++)
    {
      *dp++ = 0;
      memcpy(dp, row, rowbytes);
      dp += rowbytes;
      row += stride;
    }
  }
  else
  {
    if (!process_rect_with_scratch(scratch, row, rowbytes, bpp, stride, cand.h, scratch.exact_rows))
      return false;
  }

  if (deflate_method == 2)
  {
    ZopfliOptions opt_zopfli;
    unsigned char* data = 0;
    size_t size = 0;
    ZopfliInitOptions(&opt_zopfli);
    opt_zopfli.numiterations = iter;
    ZopfliCompress(&opt_zopfli, ZOPFLI_FORMAT_ZLIB, scratch.exact_rows, filtered_size, &data, &size);
    if (!data)
      return false;
    if (size >= (size_t)zbuf_size)
    {
      free(data);
      return false;
    }
    cand.exact_size = (unsigned int)size;
    cand.exact_valid = 1;
    free(data);
    return true;
  }
  if (deflate_method == 1)
  {
    unsigned size = zbuf_size;
    if (!compress_rfc1950_7z(scratch.exact_rows, (unsigned)filtered_size, scratch.exact_zbuf, size, iter<100 ? iter : 100, 255))
      return false;
    cand.exact_size = size;
    cand.exact_valid = 1;
    return true;
  }
  return false;
}

int choose_best_candidate(const std::vector<CandidateEval>& candidates, int use_exact)
{
  int best = -1;
  for (unsigned int i=0; i<candidates.size(); i++)
  {
    const CandidateEval& c = candidates[i];
    if (!c.valid)
      continue;
    if (use_exact && !c.exact_valid)
      continue;

    if (best < 0)
    {
      best = (int)i;
      continue;
    }

    const CandidateEval& b = candidates[best];
    unsigned int size_c = use_exact ? c.exact_size : c.estimate_size;
    unsigned int size_b = use_exact ? b.exact_size : b.estimate_size;

    if (size_c != size_b)
    {
      if (size_c < size_b)
        best = (int)i;
      continue;
    }

    int dispose_c = c.op_id >> 1;
    int dispose_b = b.op_id >> 1;
    if (dispose_c != dispose_b)
    {
      if (dispose_c < dispose_b)
        best = (int)i;
      continue;
    }

    int blend_c = c.op_id & 1;
    int blend_b = b.op_id & 1;
    if (blend_c != blend_b)
    {
      if (blend_c < blend_b)
        best = (int)i;
      continue;
    }

    unsigned long long area_c = (unsigned long long)c.w * (unsigned long long)c.h;
    unsigned long long area_b = (unsigned long long)b.w * (unsigned long long)b.h;
    if (area_c != area_b)
    {
      if (area_c < area_b)
        best = (int)i;
      continue;
    }

    if (c.op_id < b.op_id)
      best = (int)i;
  }
  return best;
}

void resolve_zlib_final_options(const ZlibFinalOptions& opts, int use_filtered, int * level, int * memLevel, int * strategy)
{
  int l = Z_BEST_COMPRESSION;
  int m = 8;
  int s = use_filtered ? Z_FILTERED : Z_DEFAULT_STRATEGY;

  if (opts.hasMode)
  {
    if (opts.mode == 0)      { l = 1; m = 8; s = Z_DEFAULT_STRATEGY; }
    else if (opts.mode == 1) { l = 6; m = 8; s = use_filtered ? Z_FILTERED : Z_DEFAULT_STRATEGY; }
    else if (opts.mode == 2) { l = 9; m = 9; s = use_filtered ? Z_FILTERED : Z_DEFAULT_STRATEGY; }
  }

  if (opts.hasLevel)
    l = opts.level;
  if (opts.hasMemLevel)
    m = opts.memLevel;
  if (opts.hasStrategy)
    s = opts.strategy;

  *level = l;
  *memLevel = m;
  *strategy = s;
}

bool deflate_rect_fin(EvalScratch& scratch, int deflate_method, int iter, unsigned char * zbuf, unsigned int * zsize, int bpp, int stride, unsigned char * rows, int zbuf_size, const CandidateEval& cand, const ZlibFinalOptions& zlib_opts)
{
  if (!zbuf || !zsize || !rows || zbuf_size <= 0 || !cand.valid)
    return false;

  unsigned char * row  = cand.p + cand.y*stride + cand.x*bpp;
  int rowbytes = cand.w*bpp;
  size_t filtered_size = 0;
  if (!checked_add_size((size_t)rowbytes, 1, &filtered_size) || !checked_mul_size(filtered_size, (size_t)cand.h, &filtered_size))
    return false;
  if (filtered_size > (size_t)INT_MAX)
    return false;

  if (cand.filters == 0)
  {
    unsigned char * dp  = rows;
    for (int j=0; j<cand.h; j++)
    {
      *dp++ = 0;
      memcpy(dp, row, rowbytes);
      dp += rowbytes;
      row += stride;
    }
  }
  else
  {
    if (!process_rect_with_scratch(scratch, row, rowbytes, bpp, stride, cand.h, rows))
      return false;
  }

  if (deflate_method == 2)
  {
    ZopfliOptions opt_zopfli;
    unsigned char* data = 0;
    size_t size = 0;
    ZopfliInitOptions(&opt_zopfli);
    opt_zopfli.numiterations = iter;
    ZopfliCompress(&opt_zopfli, ZOPFLI_FORMAT_ZLIB, rows, filtered_size, &data, &size);
    if (!data)
      return false;
    if (size < (size_t)zbuf_size)
    {
      memcpy(zbuf, data, size);
      *zsize = (unsigned int)size;
    }
    else
    {
      free(data);
      return false;
    }
    free(data);
  }
  else
  if (deflate_method == 1)
  {
    unsigned size = zbuf_size;
    if (!compress_rfc1950_7z(rows, (unsigned)filtered_size, zbuf, size, iter<100 ? iter : 100, 255))
      return false;
    *zsize = size;
  }
  else
  {
    z_stream fin_zstream;
    int level = Z_BEST_COMPRESSION;
    int memLevel = 8;
    int strategy = cand.filters ? Z_FILTERED : Z_DEFAULT_STRATEGY;
    resolve_zlib_final_options(zlib_opts, cand.filters, &level, &memLevel, &strategy);

    fin_zstream.data_type = Z_BINARY;
    fin_zstream.zalloc = Z_NULL;
    fin_zstream.zfree = Z_NULL;
    fin_zstream.opaque = Z_NULL;
    if (deflateInit2(&fin_zstream, level, 8, 15, memLevel, strategy) != Z_OK)
      return false;

    fin_zstream.next_out = zbuf;
    fin_zstream.avail_out = zbuf_size;
    fin_zstream.next_in = rows;
    fin_zstream.avail_in = (uInt)filtered_size;
    int zret = deflate(&fin_zstream, Z_FINISH);
    if (zret != Z_STREAM_END)
    {
      deflateEnd(&fin_zstream);
      return false;
    }
    *zsize = fin_zstream.total_out;
    deflateEnd(&fin_zstream);
  }
  return true;
}

int save_apng(const char * szOut, std::vector<APNGFrame>& frames, unsigned int first, unsigned int loops, unsigned int coltype, int deflate_method, int iter, unsigned int mt_threads, ExactCandidatesMode exact_mode, const ZlibFinalOptions& zlib_opts)
{
  FILE * f = 0;
  unsigned int i, j, k;
  unsigned int x0, y0, w0, h0, dop, bop;
  unsigned int idat_size, zbuf_size, zsize;
  unsigned char * zbuf = 0;
  unsigned char header[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  unsigned int num_frames = frames.size();
  unsigned int width = frames[0].w;
  unsigned int height = frames[0].h;
  unsigned int bpp = (coltype == 6) ? 4 : (coltype == 2) ? 3 : (coltype == 4) ? 2 : 1;
  unsigned int has_tcolor = (coltype >= 4 || (coltype <= 2 && trnssize)) ? 1 : 0;
  unsigned int tcolor = 0;
  size_t rowbytes_sz = 0;
  size_t imagesize_sz = 0;
  size_t rowsbuf_sz = 0;
  size_t zbuf_size_sz = 0;
  if (!compute_image_layout(width, height, bpp, &rowbytes_sz, &imagesize_sz))
    return 1;
  if (!checked_add_size(rowbytes_sz, 1, &rowsbuf_sz) || !checked_mul_size(rowsbuf_sz, (size_t)height, &rowsbuf_sz))
    return 1;
  if (!checked_add_size(rowsbuf_sz, ((rowsbuf_sz + 7) >> 3), &zbuf_size_sz) ||
      !checked_add_size(zbuf_size_sz, ((rowsbuf_sz + 63) >> 6), &zbuf_size_sz) ||
      !checked_add_size(zbuf_size_sz, 11, &zbuf_size_sz))
    return 1;
  if (rowbytes_sz > (size_t)UINT_MAX || imagesize_sz > (size_t)UINT_MAX || rowsbuf_sz > (size_t)UINT_MAX || zbuf_size_sz > (size_t)INT_MAX)
    return 1;
  unsigned int rowbytes = (unsigned int)rowbytes_sz;
  unsigned int imagesize = (unsigned int)imagesize_sz;

  unsigned char * temp  = new unsigned char[imagesize];
  unsigned char * over1 = new unsigned char[imagesize];
  unsigned char * over2 = new unsigned char[imagesize];
  unsigned char * over3 = new unsigned char[imagesize];
  unsigned char * rest  = new unsigned char[imagesize];
  unsigned char * rows  = new unsigned char[rowsbuf_sz];
  EvalScratch final_scratch;
  memset(&final_scratch, 0, sizeof(EvalScratch));
  std::vector<EvalScratch> eval_pool;
  int status = 1;
  const unsigned int pool_threads = (mt_threads > 0) ? ((mt_threads > 6) ? 6 : mt_threads) : 1;
  const int exact_enabled = (exact_mode == kExactCandidatesOn && (deflate_method == 1 || deflate_method == 2)) ? 1 : 0;

  if (!temp || !over1 || !over2 || !over3 || !rest || !rows)
    goto cleanup;

  if (trnssize)
  {
    if (coltype == 0)
      tcolor = trns[1];
    else
    if (coltype == 2)
      tcolor = (((trns[5]<<8)+trns[3])<<8)+trns[1];
    else
    if (coltype == 3)
    {
      for (i=0; i<trnssize; i++)
      if (trns[i] == 0)
      {
        has_tcolor = 1;
        tcolor = i;
        break;
      }
    }
  }

  f = fopen(szOut, "wb");
  if (!f)
  {
    printf( "Error: couldn't open file for writing\n" );
    goto cleanup;
  }

  {
    unsigned char buf_IHDR[13];
    unsigned char buf_acTL[8];
    unsigned char buf_fcTL[26];

    png_save_uint_32(buf_IHDR, width);
    png_save_uint_32(buf_IHDR + 4, height);
    buf_IHDR[8] = 8;
    buf_IHDR[9] = coltype;
    buf_IHDR[10] = 0;
    buf_IHDR[11] = 0;
    buf_IHDR[12] = 0;

    png_save_uint_32(buf_acTL, num_frames-first);
    png_save_uint_32(buf_acTL + 4, loops);

    if (fwrite(header, 1, 8, f) != 8) goto cleanup;
    if (!write_chunk(f, "IHDR", buf_IHDR, 13)) goto cleanup;
    if (num_frames > 1)
    {
      if (!write_chunk(f, "acTL", buf_acTL, 8)) goto cleanup;
    }
    else
      first = 0;
    if (palsize > 0 && !write_chunk(f, "PLTE", (unsigned char *)(&palette), palsize*3)) goto cleanup;
    if (trnssize > 0 && !write_chunk(f, "tRNS", trns, trnssize)) goto cleanup;

    idat_size = (unsigned int)rowsbuf_sz;
    zbuf_size = (unsigned int)zbuf_size_sz;
    zbuf = new unsigned char[zbuf_size];
    if (!zbuf)
      goto cleanup;

    if (!init_eval_scratch(final_scratch, rowbytes, zbuf_size, rowsbuf_sz, 0))
      goto cleanup;

    eval_pool.resize(pool_threads);
    for (unsigned int t=0; t<pool_threads; t++)
    {
      if (!init_eval_scratch(eval_pool[t], rowbytes, zbuf_size, rowsbuf_sz, exact_enabled))
        goto cleanup;
    }

    x0 = 0;
    y0 = 0;
    w0 = width;
    h0 = height;
    bop = 0;
    next_seq_num = 0;

    CandidateEval current;
    memset(&current, 0, sizeof(CandidateEval));
    current.p = frames[0].p;
    current.x = (int)x0;
    current.y = (int)y0;
    current.w = (int)w0;
    current.h = (int)h0;
    current.op_id = 0;

    printf("saving %s (frame %d of %d)\n", szOut, 1-first, num_frames-first);
    if (!evaluate_candidate(final_scratch, current, bpp, (int)rowbytes, (int)zbuf_size)) goto cleanup;
    if (!deflate_rect_fin(final_scratch, deflate_method, iter, zbuf, &zsize, bpp, (int)rowbytes, rows, (int)zbuf_size, current, zlib_opts)) goto cleanup;

    if (first)
    {
      if (!write_IDATs(f, 0, zbuf, zsize, idat_size)) goto cleanup;

      memset(&current, 0, sizeof(CandidateEval));
      current.p = frames[1].p;
      current.x = (int)x0;
      current.y = (int)y0;
      current.w = (int)w0;
      current.h = (int)h0;
      current.op_id = 0;
      printf("saving %s (frame %d of %d)\n", szOut, 1, num_frames-first);
      if (!evaluate_candidate(final_scratch, current, bpp, (int)rowbytes, (int)zbuf_size)) goto cleanup;
      if (!deflate_rect_fin(final_scratch, deflate_method, iter, zbuf, &zsize, bpp, (int)rowbytes, rows, (int)zbuf_size, current, zlib_opts)) goto cleanup;
    }

    for (i=first; i<num_frames-1; i++)
    {
      std::vector<CandidateEval> candidates;
      candidates.reserve(6);
      printf("saving %s (frame %d of %d)\n", szOut, i-first+2, num_frames-first);

      get_rect_candidates(width, height, frames[i].p, frames[i+1].p, over1, bpp, rowbytes, has_tcolor, tcolor, 0, candidates);

      if (has_tcolor)
      {
        memcpy(temp, frames[i].p, imagesize);
        if (coltype == 2)
          for (j=0; j<h0; j++)
            for (k=0; k<w0; k++)
              memcpy(temp + ((j+y0)*width + (k+x0))*3, &tcolor, 3);
        else
          for (j=0; j<h0; j++)
            memset(temp + ((j+y0)*width + x0)*bpp, tcolor, w0*bpp);
        get_rect_candidates(width, height, temp, frames[i+1].p, over2, bpp, rowbytes, has_tcolor, tcolor, 1, candidates);
      }

      if (i > first)
        get_rect_candidates(width, height, rest, frames[i+1].p, over3, bpp, rowbytes, has_tcolor, tcolor, 2, candidates);

      run_parallel_indices((unsigned int)candidates.size(), pool_threads, [&](unsigned int begin, unsigned int end, unsigned int worker_id) {
        EvalScratch& scratch = eval_pool[worker_id];
        for (unsigned int idx = begin; idx < end; idx++)
          evaluate_candidate(scratch, candidates[idx], bpp, (int)rowbytes, (int)zbuf_size);
      });

      if (exact_enabled)
      {
        run_parallel_indices((unsigned int)candidates.size(), pool_threads, [&](unsigned int begin, unsigned int end, unsigned int worker_id) {
          EvalScratch& scratch = eval_pool[worker_id];
          for (unsigned int idx = begin; idx < end; idx++)
            compress_candidate_exact(scratch, candidates[idx], deflate_method, iter, bpp, (int)rowbytes, (int)zbuf_size);
        });
      }

      int best_idx = choose_best_candidate(candidates, exact_enabled);
      if (best_idx < 0)
        goto cleanup;
      CandidateEval& best = candidates[best_idx];

      dop = (unsigned int)(best.op_id >> 1);
      png_save_uint_32(buf_fcTL, next_seq_num++);
      png_save_uint_32(buf_fcTL + 4, w0);
      png_save_uint_32(buf_fcTL + 8, h0);
      png_save_uint_32(buf_fcTL + 12, x0);
      png_save_uint_32(buf_fcTL + 16, y0);
      png_save_uint_16(buf_fcTL + 20, frames[i].delay_num);
      png_save_uint_16(buf_fcTL + 22, frames[i].delay_den);
      buf_fcTL[24] = dop;
      buf_fcTL[25] = bop;
      if (!write_chunk(f, "fcTL", buf_fcTL, 26)) goto cleanup;

      if (!write_IDATs(f, i, zbuf, zsize, idat_size)) goto cleanup;

      if (dop != 2)
        memcpy(rest, frames[i].p, imagesize);
      if (dop == 1)
      {
        if (coltype == 2)
          for (j=0; j<h0; j++)
            for (k=0; k<w0; k++)
              memcpy(rest + ((j+y0)*width + (k+x0))*3, &tcolor, 3);
        else
          for (j=0; j<h0; j++)
            memset(rest + ((j+y0)*width + x0)*bpp, tcolor, w0*bpp);
      }

      x0 = (unsigned int)best.x;
      y0 = (unsigned int)best.y;
      w0 = (unsigned int)best.w;
      h0 = (unsigned int)best.h;
      bop = (unsigned int)(best.op_id & 1);

      if (!deflate_rect_fin(final_scratch, deflate_method, iter, zbuf, &zsize, bpp, (int)rowbytes, rows, (int)zbuf_size, best, zlib_opts))
        goto cleanup;
    }

    if (num_frames > 1)
    {
      png_save_uint_32(buf_fcTL, next_seq_num++);
      png_save_uint_32(buf_fcTL + 4, w0);
      png_save_uint_32(buf_fcTL + 8, h0);
      png_save_uint_32(buf_fcTL + 12, x0);
      png_save_uint_32(buf_fcTL + 16, y0);
      png_save_uint_16(buf_fcTL + 20, frames[num_frames-1].delay_num);
      png_save_uint_16(buf_fcTL + 22, frames[num_frames-1].delay_den);
      buf_fcTL[24] = 0;
      buf_fcTL[25] = bop;
      if (!write_chunk(f, "fcTL", buf_fcTL, 26)) goto cleanup;
    }

    if (!write_IDATs(f, num_frames-1, zbuf, zsize, idat_size)) goto cleanup;
    if (!write_chunk(f, "IEND", 0, 0)) goto cleanup;
    status = 0;
  }

cleanup:
  if (f)
  {
    if (status == 0)
    {
      if (fclose(f) != 0)
        status = 1;
    }
    else
      fclose(f);
    f = 0;
  }

  free_eval_scratch(final_scratch);
  for (unsigned int t=0; t<eval_pool.size(); t++)
    free_eval_scratch(eval_pool[t]);

  delete[] zbuf;
  delete[] temp;
  delete[] over1;
  delete[] over2;
  delete[] over3;
  delete[] rest;
  delete[] rows;

  return status;
}
/* APNG encoder - end */

int parse_zlib_strategy(const char * name, int * strategy)
{
  if (!name || !strategy)
    return 0;
  if (strcmp(name, "default") == 0)  { *strategy = Z_DEFAULT_STRATEGY; return 1; }
  if (strcmp(name, "filtered") == 0) { *strategy = Z_FILTERED; return 1; }
  if (strcmp(name, "huffman") == 0)  { *strategy = Z_HUFFMAN_ONLY; return 1; }
  if (strcmp(name, "rle") == 0)      { *strategy = Z_RLE; return 1; }
  if (strcmp(name, "fixed") == 0)    { *strategy = Z_FIXED; return 1; }
  return 0;
}

int parse_zlib_mode(const char * name, int * mode)
{
  if (!name || !mode)
    return 0;
  if (strcmp(name, "speed") == 0)    { *mode = 0; return 1; }
  if (strcmp(name, "balanced") == 0) { *mode = 1; return 1; }
  if (strcmp(name, "size") == 0)     { *mode = 2; return 1; }
  return 0;
}

int main(int argc, char** argv)
{
  std::string szInput;
  std::string szOut;
  char * szOpt;
  std::vector<APNGFrame> frames;
  unsigned int first, loops, coltype;
  int    deflate_method = 0;
  int    iter = 15;
  int    disableImageQuant = 0;
  int    liqSpeed = 4;
  int    liqMaxColors = 256;
  int    liqPosterization = 0;
  float  liqDither = 1.0f;
  int    liqMinQuality = 50;
  int    liqMaxQuality = 100;
  unsigned int liqThreads = 0;
  unsigned int mtThreads = 1;
  ExactCandidatesMode exact_mode = kExactCandidatesOff;
  ZlibFinalOptions zlib_opts;
  memset(&zlib_opts, 0, sizeof(ZlibFinalOptions));
  zlib_opts.level = Z_BEST_COMPRESSION;
  zlib_opts.memLevel = 8;
  zlib_opts.strategy = Z_DEFAULT_STRATEGY;
  zlib_opts.mode = 1;

  printf("\nAPNG Optimizer 1.4");

  if (argc <= 1)
  {
    printf("\n\nUsage: apngopt [options] anim.png [anim_opt.png]\n\n"
           "-z0  : zlib compression (default)\n"
           "-z1  : 7zip compression \n"
           "-z2  : zopfli compression\n"
           "-i## : number of iterations, default -i%d\n"
           "-d## : disable imagequant compress 0 or 1, default 0 -d%d\n"
           "--liq-speed=N     : imagequant speed (1..10), default %d\n"
           "--liq-colors=N    : imagequant max colors (2..256), default %d\n"
           "--liq-posterize=N : imagequant posterization bits (0..4), default %d\n"
           "--liq-dither=F    : imagequant dithering (0..1), default %.2f\n"
           "--liq-quality=A-B : imagequant quality range (0..100), default %d-%d\n"
           "--liq-threads=N   : set RAYON_NUM_THREADS for imagequant\n"
           "--mt=N            : apngopt local threads for independent stages\n"
           "--exact-candidates: exact candidate ranking for -z1/-z2 (slower)\n"
           "--zlib-level=N    : final -z0 level (1..9)\n"
           "--zlib-mem-level=N: final -z0 memLevel (1..9)\n"
           "--zlib-strategy=S : final -z0 strategy (default|filtered|huffman|rle|fixed)\n"
           "--zlib-mode=M     : final -z0 preset (speed|balanced|size), overridden by explicit --zlib-*\n",
           iter, disableImageQuant, liqSpeed, liqMaxColors, liqPosterization, liqDither, liqMinQuality, liqMaxQuality);
    return 1;
  }

  for (int i=1; i<argc; i++)
  {
    szOpt = argv[i];
	printf("\n szOpt: %s\n", szOpt);
	/* szOpt[0] == '/' || */
    if (strncmp(szOpt, "--", 2) == 0)
    {
      if (strncmp(szOpt, "--liq-speed=", 12) == 0)
      {
        liqSpeed = atoi(szOpt + 12);
        if (liqSpeed < 1) liqSpeed = 1;
        if (liqSpeed > 10) liqSpeed = 10;
      }
      else if (strncmp(szOpt, "--liq-colors=", 13) == 0)
      {
        liqMaxColors = atoi(szOpt + 13);
        if (liqMaxColors < 2) liqMaxColors = 2;
        if (liqMaxColors > 256) liqMaxColors = 256;
      }
      else if (strncmp(szOpt, "--liq-posterize=", 16) == 0)
      {
        liqPosterization = atoi(szOpt + 16);
        if (liqPosterization < 0) liqPosterization = 0;
        if (liqPosterization > 4) liqPosterization = 4;
      }
      else if (strncmp(szOpt, "--liq-dither=", 13) == 0)
      {
        liqDither = (float)atof(szOpt + 13);
        if (liqDither < 0.0f) liqDither = 0.0f;
        if (liqDither > 1.0f) liqDither = 1.0f;
      }
      else if (strncmp(szOpt, "--liq-quality=", 14) == 0)
      {
        const char *q = szOpt + 14;
        const char *dash = strchr(q, '-');
        if (dash)
        {
          liqMinQuality = atoi(q);
          liqMaxQuality = atoi(dash + 1);
          if (liqMinQuality < 0) liqMinQuality = 0;
          if (liqMinQuality > 100) liqMinQuality = 100;
          if (liqMaxQuality < 0) liqMaxQuality = 0;
          if (liqMaxQuality > 100) liqMaxQuality = 100;
          if (liqMinQuality > liqMaxQuality)
            liqMinQuality = liqMaxQuality;
        }
      }
      else if (strncmp(szOpt, "--liq-threads=", 14) == 0)
      {
        int t = atoi(szOpt + 14);
        if (t > 0)
          liqThreads = (unsigned int)t;
      }
      else if (strncmp(szOpt, "--mt=", 5) == 0)
      {
        int t = atoi(szOpt + 5);
        if (t > 0)
          mtThreads = (unsigned int)t;
      }
      else if (strcmp(szOpt, "--exact-candidates") == 0)
      {
        exact_mode = kExactCandidatesOn;
      }
      else if (strncmp(szOpt, "--exact-candidates=", 19) == 0)
      {
        int v = atoi(szOpt + 19);
        exact_mode = (v > 0) ? kExactCandidatesOn : kExactCandidatesOff;
      }
      else if (strncmp(szOpt, "--zlib-level=", 13) == 0)
      {
        int v = atoi(szOpt + 13);
        if (v < 1) v = 1;
        if (v > 9) v = 9;
        zlib_opts.level = v;
        zlib_opts.hasLevel = 1;
      }
      else if (strncmp(szOpt, "--zlib-mem-level=", 17) == 0)
      {
        int v = atoi(szOpt + 17);
        if (v < 1) v = 1;
        if (v > 9) v = 9;
        zlib_opts.memLevel = v;
        zlib_opts.hasMemLevel = 1;
      }
      else if (strncmp(szOpt, "--zlib-strategy=", 16) == 0)
      {
        int strategy = 0;
        if (parse_zlib_strategy(szOpt + 16, &strategy))
        {
          zlib_opts.strategy = strategy;
          zlib_opts.hasStrategy = 1;
        }
        else
          printf("warning: unknown zlib strategy '%s' ignored\n", szOpt + 16);
      }
      else if (strncmp(szOpt, "--zlib-mode=", 12) == 0)
      {
        int mode = 0;
        if (parse_zlib_mode(szOpt + 12, &mode))
        {
          zlib_opts.mode = mode;
          zlib_opts.hasMode = 1;
        }
        else
          printf("warning: unknown zlib mode '%s' ignored\n", szOpt + 12);
      }
    }
    else if (szOpt[0] == '-')
    {
      if (szOpt[1] == 'z' || szOpt[1] == 'Z')
      {
        if (szOpt[2] == '0')
          deflate_method = 0;
        if (szOpt[2] == '1')
          deflate_method = 1;
        if (szOpt[2] == '2')
          deflate_method = 2;
      }
      if (szOpt[1] == 'i' || szOpt[1] == 'I')
      {
        iter = atoi(szOpt+2);
        if (iter < 1) iter = 1;
      }
	  if (szOpt[1] == 'd' || szOpt[1] == 'D')
      {
        disableImageQuant = atoi(szOpt+2);
        if (disableImageQuant < 1) disableImageQuant = 0;
      }
    }
    else
    if (szInput.empty())
      szInput = szOpt;
    else
    if (szOut.empty())
      szOut = szOpt;
  }
  printf(" input file: %s\n", szInput.c_str());
  if (deflate_method == 0)
    printf(" using ZLIB\n\n");
  else if (deflate_method == 1)
    printf(" using 7ZIP with %d iterations\n\n", iter);
  else if (deflate_method == 2)
    printf(" using ZOPFLI with %d iterations\n\n", iter);

  if (exact_mode == kExactCandidatesOn && deflate_method == 0)
    printf("warning: --exact-candidates is ignored for -z0\n");

  if (deflate_method != 0 && (zlib_opts.hasMode || zlib_opts.hasLevel || zlib_opts.hasMemLevel || zlib_opts.hasStrategy))
    printf("warning: --zlib-* options apply to -z0 only and are ignored for current mode\n");

  if (szOut.empty())
  {
    szOut = szInput;
    size_t dot = szOut.find_last_of('.');
    if (dot != std::string::npos)
      szOut.erase(dot);
    szOut += "_opt.png";
  }
  printf(" output file: %s\n", szOut.c_str());

  int res = load_apng(szInput.c_str(), frames, first, loops);
  if (res < 0)
  {
    printf("load_apng() failed: '%s'\n", szInput.c_str());
    return 1;
  }

  optim_dirty_mt(frames, mtThreads);
  optim_duplicates(frames, first);
  
  if (disableImageQuant > 0)
    optim_downconvert(frames, coltype);
  else
  {
    if (!set_imagequant_thread_env(liqThreads))
      printf("warning: unable to set RAYON_NUM_THREADS=%u\n", liqThreads);
    if (!optim_image(frames, coltype, liqMinQuality, liqMaxQuality, liqSpeed, liqMaxColors, liqPosterization, liqDither))
    {
      printf("warning: imagequant path failed, using downconvert fallback\n");
      optim_downconvert(frames, coltype);
    }
  }

  if (save_apng(szOut.c_str(), frames, first, loops, coltype, deflate_method, iter, mtThreads, exact_mode, zlib_opts) != 0)
  {
    printf("save_apng() failed: '%s'\n", szOut.c_str());
    for (size_t j=0; j<frames.size(); j++)
    {
      delete[] frames[j].rows;
      delete[] frames[j].p;
    }
    frames.clear();
    return 1;
  }

  for (size_t j=0; j<frames.size(); j++)
  {
    delete[] frames[j].rows;
    delete[] frames[j].p;
  }
  frames.clear();

  printf("all done\n");

  return 0;
}
