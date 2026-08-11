/* FreeType text rasterizer over the Switch shared fonts. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "cocos_text.h"

static int       g_ready = 0;
static FT_Library g_ft;
#define MAX_FACES 4
static FT_Face   g_faces[MAX_FACES];
static int       g_nfaces = 0;

static void add_face(PlSharedFontType type) {
  if (g_nfaces >= MAX_FACES) return;
  PlFontData fd;
  if (R_FAILED(plGetSharedFontByType(&fd, type))) return;
  FT_Face f;
  if (FT_New_Memory_Face(g_ft, (const FT_Byte *)fd.address, fd.size, 0, &f) == 0)
    g_faces[g_nfaces++] = f;
}

void cocos_text_init(void) {
  if (g_ready) return;
  g_ready = 1;
  if (R_FAILED(plInitialize(PlServiceType_User))) return;
  if (FT_Init_FreeType(&g_ft)) return;
  add_face(PlSharedFontType_Standard);
  add_face(PlSharedFontType_ChineseSimplified);
  add_face(PlSharedFontType_ExtChineseSimplified);
  add_face(PlSharedFontType_NintendoExt);
}

/* minimal UTF-8 -> codepoint decoder; advances *p past the char */
static uint32_t utf8_next(const char **p) {
  const unsigned char *s = (const unsigned char *)*p;
  uint32_t c = *s;
  int n;
  if (c < 0x80)      { n = 0; }
  else if (c < 0xE0) { c &= 0x1F; n = 1; }
  else if (c < 0xF0) { c &= 0x0F; n = 2; }
  else               { c &= 0x07; n = 3; }
  s++;
  for (int i = 0; i < n && (*s & 0xC0) == 0x80; i++) { c = (c << 6) | (*s & 0x3F); s++; }
  *p = (const char *)s;
  return c;
}

/* pick the first face that has a glyph for `cp`; returns face + sets glyph index */
static FT_Face face_for(uint32_t cp, FT_UInt *gi) {
  for (int i = 0; i < g_nfaces; i++) {
    FT_UInt g = FT_Get_Char_Index(g_faces[i], cp);
    if (g) { *gi = g; return g_faces[i]; }
  }
  if (g_nfaces) { *gi = FT_Get_Char_Index(g_faces[0], cp); return g_faces[0]; }
  *gi = 0; return NULL;
}

unsigned char *cocos_text_render(const char *text, int px, int cr, int cg, int cb,
                                 int req_w, int req_h, int align,
                                 int *out_w, int *out_h) {
  cocos_text_init();
  if (!g_nfaces || !text) return NULL;
  if (px < 6)  px = 6;
  if (px > 200) px = 200;
  for (int i = 0; i < g_nfaces; i++) FT_Set_Pixel_Sizes(g_faces[i], 0, px);

  const int line_h = px + px / 4;     /* approximate line height */
  const int ascent = px;              /* baseline offset from line top */

  /* --- pass 1: measure line widths (26.6 advances) --- */
  int max_w = 0, cur_w = 0, lines = 1;
  for (const char *p = text; *p; ) {
    uint32_t cp = utf8_next(&p);
    if (cp == '\n') { if (cur_w > max_w) max_w = cur_w; cur_w = 0; lines++; continue; }
    if (cp == '\r') continue;
    FT_UInt gi; FT_Face f = face_for(cp, &gi);
    if (f && FT_Load_Glyph(f, gi, FT_LOAD_DEFAULT) == 0)
      cur_w += (int)(f->glyph->advance.x >> 6);
  }
  if (cur_w > max_w) max_w = cur_w;

  int W = req_w > 0 ? req_w : max_w + 2;
  int H = req_h > 0 ? req_h : lines * line_h + 2;
  if (W < 1) W = 1;
  if (H < 1) H = 1;
  if (W > 2048) W = 2048;
  if (H > 2048) H = 2048;

  unsigned char *rgba = calloc((size_t)W * H, 4);
  if (!rgba) return NULL;
  if (cr < 0) cr = 0;
  if (cr > 255) cr = 255;
  if (cg < 0) cg = 0;
  if (cg > 255) cg = 255;
  if (cb < 0) cb = 0;
  if (cb > 255) cb = 255;

  /* --- pass 2: render, one line at a time, honoring horizontal align --- */
  int line = 0;
  const char *lp = text;
  while (lp && *lp && line < lines) {
    /* measure this line for alignment */
    int lw = 0;
    for (const char *q = lp; *q && *q != '\n'; ) {
      uint32_t cp = utf8_next(&q);
      if (cp == '\r') continue;
      FT_UInt gi; FT_Face f = face_for(cp, &gi);
      if (f && FT_Load_Glyph(f, gi, FT_LOAD_DEFAULT) == 0) lw += (int)(f->glyph->advance.x >> 6);
    }
    int pen_x = 0;
    if (align & 0x02)      pen_x = W - lw;          /* right  */
    else if (align & 0x01) pen_x = (W - lw) / 2;    /* center */
    if (pen_x < 0) pen_x = 0;
    int base_y = line * line_h + ascent;

    for (; *lp && *lp != '\n'; ) {
      uint32_t cp = utf8_next(&lp);
      if (cp == '\r') continue;
      FT_UInt gi; FT_Face f = face_for(cp, &gi);
      if (!f || FT_Load_Glyph(f, gi, FT_LOAD_RENDER) != 0) continue;
      FT_GlyphSlot g = f->glyph;
      const FT_Bitmap *bm = &g->bitmap;
      int ox = pen_x + g->bitmap_left;
      int oy = base_y - g->bitmap_top;
      for (unsigned row = 0; row < bm->rows; row++) {
        int y = oy + (int)row;
        if (y < 0 || y >= H) continue;
        const unsigned char *src = bm->buffer + (size_t)row * bm->pitch;
        for (unsigned col = 0; col < bm->width; col++) {
          int x = ox + (int)col;
          if (x < 0 || x >= W) continue;
          unsigned a = src[col];
          if (!a) continue;
          unsigned char *d = rgba + ((size_t)y * W + x) * 4;
          /* alpha-over onto existing (glyph coverage in A, colour in RGB) */
          if (a >= d[3]) { d[0] = (unsigned char)cr; d[1] = (unsigned char)cg; d[2] = (unsigned char)cb; d[3] = (unsigned char)a; }
        }
      }
      pen_x += (int)(g->advance.x >> 6);
    }
    if (*lp == '\n') lp++;
    line++;
  }

  *out_w = W; *out_h = H;
  return rgba;
}
