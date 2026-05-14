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
struct OP { unsigned char * p; unsigned int size; int x, y, w, h, valid, filters; };
struct rgb { unsigned char r, g, b; };

unsigned char * op_zbuf1;
unsigned char * op_zbuf2;
z_stream        op_zstream1;
z_stream        op_zstream2;
unsigned char * row_buf;
unsigned char * sub_row;
unsigned char * up_row;
unsigned char * avg_row;
unsigned char * paeth_row;
OP              op[6];
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
    return 1;

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
void write_chunk(FILE * f, const char * name, unsigned char * data, unsigned int length)
{
  unsigned char buf[4];
  unsigned int crc = crc32(0, Z_NULL, 0);

  png_save_uint_32(buf, length);
  fwrite(buf, 1, 4, f);
  fwrite(name, 1, 4, f);
  crc = crc32(crc, (const Bytef *)name, 4);

  if (memcmp(name, "fdAT", 4) == 0)
  {
    png_save_uint_32(buf, next_seq_num++);
    fwrite(buf, 1, 4, f);
    crc = crc32(crc, buf, 4);
    length -= 4;
  }

  if (data != NULL && length > 0)
  {
    fwrite(data, 1, length, f);
    crc = crc32(crc, data, length);
  }

  png_save_uint_32(buf, crc);
  fwrite(buf, 1, 4, f);
}

void write_IDATs(FILE * f, int frame, unsigned char * data, unsigned int length, unsigned int idat_size)
{
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
      write_chunk(f, "IDAT", data, ds);
    else
      write_chunk(f, "fdAT", data, ds+4);

    data += ds;
    length -= ds;
  }
}

void process_rect(unsigned char * row, int rowbytes, int bpp, int stride, int h, unsigned char * rows)
{
  int i, j, v;
  int a, b, c, pa, pb, pc, p;
  unsigned char * prev = NULL;
  unsigned char * dp  = rows;
  unsigned char * out;

  for (j=0; j<h; j++)
  {
    unsigned int    sum = 0;
    unsigned char * best_row = row_buf;
    unsigned int    mins = ((unsigned int)(-1)) >> 1;

    out = row_buf+1;
    for (i=0; i<rowbytes; i++)
    {
      v = out[i] = row[i];
      sum += (v < 128) ? v : 256 - v;
    }
    mins = sum;

    sum = 0;
    out = sub_row+1;
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
      best_row = sub_row;
    }

    if (prev)
    {
      sum = 0;
      out = up_row+1;
      for (i=0; i<rowbytes; i++)
      {
        v = out[i] = row[i] - prev[i];
        sum += (v < 128) ? v : 256 - v;
        if (sum > mins) break;
      }
      if (sum < mins)
      {
        mins = sum;
        best_row = up_row;
      }

      sum = 0;
      out = avg_row+1;
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
        best_row = avg_row;
      }

      sum = 0;
      out = paeth_row+1;
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
      {
        best_row = paeth_row;
      }
    }

    if (rows == NULL)
    {
      // deflate_rect_op()
      op_zstream1.next_in = row_buf;
      op_zstream1.avail_in = rowbytes + 1;
      deflate(&op_zstream1, Z_NO_FLUSH);

      op_zstream2.next_in = best_row;
      op_zstream2.avail_in = rowbytes + 1;
      deflate(&op_zstream2, Z_NO_FLUSH);
    }
    else
    {
      // deflate_rect_fin()
      memcpy(dp, best_row, rowbytes+1);
      dp += rowbytes+1;
    }

    prev = row;
    row += stride;
  }
}

bool deflate_rect_fin(int deflate_method, int iter, unsigned char * zbuf, unsigned int * zsize, int bpp, int stride, unsigned char * rows, int zbuf_size, int n)
{
  if (!zbuf || !zsize || !rows || zbuf_size <= 0)
    return false;

  unsigned char * row  = op[n].p + op[n].y*stride + op[n].x*bpp;
  int rowbytes = op[n].w*bpp;
  size_t filtered_size = 0;
  if (!checked_add_size((size_t)rowbytes, 1, &filtered_size) || !checked_mul_size(filtered_size, (size_t)op[n].h, &filtered_size))
    return false;
  if (filtered_size > (size_t)INT_MAX)
    return false;

  if (op[n].filters == 0)
  {
    unsigned char * dp  = rows;
    for (int j=0; j<op[n].h; j++)
    {
      *dp++ = 0;
      memcpy(dp, row, rowbytes);
      dp += rowbytes;
      row += stride;
    }
  }
  else
    process_rect(row, rowbytes, bpp, stride, op[n].h, rows);

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

    fin_zstream.data_type = Z_BINARY;
    fin_zstream.zalloc = Z_NULL;
    fin_zstream.zfree = Z_NULL;
    fin_zstream.opaque = Z_NULL;
    if (deflateInit2(&fin_zstream, Z_BEST_COMPRESSION, 8, 15, 8, op[n].filters ? Z_FILTERED : Z_DEFAULT_STRATEGY) != Z_OK)
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

void deflate_rect_op(unsigned char *pdata, int x, int y, int w, int h, int bpp, int stride, int zbuf_size, int n)
{
  unsigned char * row  = pdata + y*stride + x*bpp;
  int rowbytes = w * bpp;

  op_zstream1.data_type = Z_BINARY;
  op_zstream1.next_out = op_zbuf1;
  op_zstream1.avail_out = zbuf_size;

  op_zstream2.data_type = Z_BINARY;
  op_zstream2.next_out = op_zbuf2;
  op_zstream2.avail_out = zbuf_size;

  process_rect(row, rowbytes, bpp, stride, h, NULL);

  deflate(&op_zstream1, Z_FINISH);
  deflate(&op_zstream2, Z_FINISH);
  op[n].p = pdata;

  if (op_zstream1.total_out < op_zstream2.total_out)
  {
    op[n].size = op_zstream1.total_out;
    op[n].filters = 0;
  }
  else
  {
    op[n].size = op_zstream2.total_out;
    op[n].filters = 1;
  }
  op[n].x = x;
  op[n].y = y;
  op[n].w = w;
  op[n].h = h;
  op[n].valid = 1;
  deflateReset(&op_zstream1);
  deflateReset(&op_zstream2);
}

void get_rect(unsigned int w, unsigned int h, unsigned char *pimage1, unsigned char *pimage2, unsigned char *ptemp, unsigned int bpp, unsigned int stride, int zbuf_size, unsigned int has_tcolor, unsigned int tcolor, int n)
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

  deflate_rect_op(pimage2, x0, y0, w0, h0, bpp, stride, zbuf_size, n*2);

  if (over_is_possible)
    deflate_rect_op(ptemp, x0, y0, w0, h0, bpp, stride, zbuf_size, n*2+1);
}

int save_apng(const char * szOut, std::vector<APNGFrame>& frames, unsigned int first, unsigned int loops, unsigned int coltype, int deflate_method, int iter)
{
  FILE * f;
  unsigned int i, j, k;
  unsigned int x0, y0, w0, h0, dop, bop;
  unsigned int idat_size, zbuf_size, zsize;
  unsigned char * zbuf;
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

  if ((f = fopen(szOut, "wb")) != 0)
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

    if (fwrite(header, 1, 8, f) != 8)
    {
      fclose(f);
      delete[] temp;
      delete[] over1;
      delete[] over2;
      delete[] over3;
      delete[] rest;
      delete[] rows;
      return 1;
    }

    write_chunk(f, "IHDR", buf_IHDR, 13);

    if (num_frames > 1)
      write_chunk(f, "acTL", buf_acTL, 8);
    else
      first = 0;

    if (palsize > 0)
      write_chunk(f, "PLTE", (unsigned char *)(&palette), palsize*3);

    if (trnssize > 0)
      write_chunk(f, "tRNS", trns, trnssize);

    op_zstream1.data_type = Z_BINARY;
    op_zstream1.zalloc = Z_NULL;
    op_zstream1.zfree = Z_NULL;
    op_zstream1.opaque = Z_NULL;
    if (deflateInit2(&op_zstream1, Z_BEST_SPEED+1, 8, 15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
    {
      fclose(f);
      delete[] temp;
      delete[] over1;
      delete[] over2;
      delete[] over3;
      delete[] rest;
      delete[] rows;
      return 1;
    }

    op_zstream2.data_type = Z_BINARY;
    op_zstream2.zalloc = Z_NULL;
    op_zstream2.zfree = Z_NULL;
    op_zstream2.opaque = Z_NULL;
    if (deflateInit2(&op_zstream2, Z_BEST_SPEED+1, 8, 15, 8, Z_FILTERED) != Z_OK)
    {
      deflateEnd(&op_zstream1);
      fclose(f);
      delete[] temp;
      delete[] over1;
      delete[] over2;
      delete[] over3;
      delete[] rest;
      delete[] rows;
      return 1;
    }

    idat_size = (unsigned int)rowsbuf_sz;
    zbuf_size = (unsigned int)zbuf_size_sz;

    zbuf = new unsigned char[zbuf_size];
    op_zbuf1 = new unsigned char[zbuf_size];
    op_zbuf2 = new unsigned char[zbuf_size];
    row_buf = new unsigned char[rowbytes + 1];
    sub_row = new unsigned char[rowbytes + 1];
    up_row = new unsigned char[rowbytes + 1];
    avg_row = new unsigned char[rowbytes + 1];
    paeth_row = new unsigned char[rowbytes + 1];

    row_buf[0] = 0;
    sub_row[0] = 1;
    up_row[0] = 2;
    avg_row[0] = 3;
    paeth_row[0] = 4;

    x0 = 0;
    y0 = 0;
    w0 = width;
    h0 = height;
    bop = 0;
    next_seq_num = 0;

    printf("saving %s (frame %d of %d)\n", szOut, 1-first, num_frames-first);
    for (j=0; j<6; j++)
      op[j].valid = 0;
    deflate_rect_op(frames[0].p, x0, y0, w0, h0, bpp, rowbytes, zbuf_size, 0);
    if (!deflate_rect_fin(deflate_method, iter, zbuf, &zsize, bpp, rowbytes, rows, zbuf_size, 0))
    {
      fclose(f);
      delete[] zbuf;
      delete[] op_zbuf1;
      delete[] op_zbuf2;
      delete[] row_buf;
      delete[] sub_row;
      delete[] up_row;
      delete[] avg_row;
      delete[] paeth_row;
      deflateEnd(&op_zstream1);
      deflateEnd(&op_zstream2);
      delete[] temp;
      delete[] over1;
      delete[] over2;
      delete[] over3;
      delete[] rest;
      delete[] rows;
      return 1;
    }

    if (first)
    {
      write_IDATs(f, 0, zbuf, zsize, idat_size);

      printf("saving %s (frame %d of %d)\n", szOut, 1, num_frames-first);
      for (j=0; j<6; j++)
        op[j].valid = 0;
      deflate_rect_op(frames[1].p, x0, y0, w0, h0, bpp, rowbytes, zbuf_size, 0);
      if (!deflate_rect_fin(deflate_method, iter, zbuf, &zsize, bpp, rowbytes, rows, zbuf_size, 0))
      {
        fclose(f);
        delete[] zbuf;
        delete[] op_zbuf1;
        delete[] op_zbuf2;
        delete[] row_buf;
        delete[] sub_row;
        delete[] up_row;
        delete[] avg_row;
        delete[] paeth_row;
        deflateEnd(&op_zstream1);
        deflateEnd(&op_zstream2);
        delete[] temp;
        delete[] over1;
        delete[] over2;
        delete[] over3;
        delete[] rest;
        delete[] rows;
        return 1;
      }
    }

    for (i=first; i<num_frames-1; i++)
    {
      unsigned int op_min;
      int          op_best;

      printf("saving %s (frame %d of %d)\n", szOut, i-first+2, num_frames-first);
      for (j=0; j<6; j++)
        op[j].valid = 0;

      /* dispose = none */
      get_rect(width, height, frames[i].p, frames[i+1].p, over1, bpp, rowbytes, zbuf_size, has_tcolor, tcolor, 0);

      /* dispose = background */
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

        get_rect(width, height, temp, frames[i+1].p, over2, bpp, rowbytes, zbuf_size, has_tcolor, tcolor, 1);
      }

      /* dispose = previous */
      if (i > first)
        get_rect(width, height, rest, frames[i+1].p, over3, bpp, rowbytes, zbuf_size, has_tcolor, tcolor, 2);

      op_min = op[0].size;
      op_best = 0;
      for (j=1; j<6; j++)
      if (op[j].valid)
      {
        if (op[j].size < op_min)
        {
          op_min = op[j].size;
          op_best = j;
        }
      }

      dop = op_best >> 1;

      png_save_uint_32(buf_fcTL, next_seq_num++);
      png_save_uint_32(buf_fcTL + 4, w0);
      png_save_uint_32(buf_fcTL + 8, h0);
      png_save_uint_32(buf_fcTL + 12, x0);
      png_save_uint_32(buf_fcTL + 16, y0);
      png_save_uint_16(buf_fcTL + 20, frames[i].delay_num);
      png_save_uint_16(buf_fcTL + 22, frames[i].delay_den);
      buf_fcTL[24] = dop;
      buf_fcTL[25] = bop;
      write_chunk(f, "fcTL", buf_fcTL, 26);

      write_IDATs(f, i, zbuf, zsize, idat_size);

      /* process apng dispose - begin */
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
      /* process apng dispose - end */

      x0 = op[op_best].x;
      y0 = op[op_best].y;
      w0 = op[op_best].w;
      h0 = op[op_best].h;
      bop = op_best & 1;

      if (!deflate_rect_fin(deflate_method, iter, zbuf, &zsize, bpp, rowbytes, rows, zbuf_size, op_best))
      {
        fclose(f);
        delete[] zbuf;
        delete[] op_zbuf1;
        delete[] op_zbuf2;
        delete[] row_buf;
        delete[] sub_row;
        delete[] up_row;
        delete[] avg_row;
        delete[] paeth_row;
        deflateEnd(&op_zstream1);
        deflateEnd(&op_zstream2);
        delete[] temp;
        delete[] over1;
        delete[] over2;
        delete[] over3;
        delete[] rest;
        delete[] rows;
        return 1;
      }
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
      write_chunk(f, "fcTL", buf_fcTL, 26);
    }

    write_IDATs(f, num_frames-1, zbuf, zsize, idat_size);

    write_chunk(f, "IEND", 0, 0);
    fclose(f);

    delete[] zbuf;
    delete[] op_zbuf1;
    delete[] op_zbuf2;
    delete[] row_buf;
    delete[] sub_row;
    delete[] up_row;
    delete[] avg_row;
    delete[] paeth_row;

    deflateEnd(&op_zstream1);
    deflateEnd(&op_zstream2);
  }
  else
  {
    printf( "Error: couldn't open file for writing\n" );
    delete[] temp;
    delete[] over1;
    delete[] over2;
    delete[] over3;
    delete[] rest;
    delete[] rows;
    return 1;
  }

  delete[] temp;
  delete[] over1;
  delete[] over2;
  delete[] over3;
  delete[] rest;
  delete[] rows;

  return 0;
}
/* APNG encoder - end */

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
           "--mt=N            : apngopt local threads for independent stages\n",
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

  if (save_apng(szOut.c_str(), frames, first, loops, coltype, deflate_method, iter) != 0)
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
