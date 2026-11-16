/*
cl_ttf.c - XGLua-CJK: TrueType font rendering for console and HUD text
Uses stb_truetype (public domain) to rasterize glyphs on demand into a
dynamically paged texture atlas. Any Unicode codepoint the font file
contains can be displayed (CJK included); glyphs missing from the font
fall back to .notdef.

Fallback: when no ttf is loaded through con_fontfile/hud_fontfile the
engine keeps using legacy 256-glyph qfont bitmaps (.fnt), zero regression.

Copyright (C) 2026 XGLua project
GPLv3+ (matches engine license)
*/

#include "common.h"
#include "filesystem.h"
#include "client.h"
#include "com_image.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#define STBTT_assert( x ) ((void)0)
#define STBTT_malloc( x, u )  ( (void)(u), Mem_Malloc( host.imagepool, (x) ))
#define STBTT_free( x, u )    ( (void)(u), Mem_Free( x ))
#include "stb_truetype.h"

#define TTF_ATLAS_SIZE  512
#define TTF_MAX_GLYPHS  8192  // glyph cache entries per font
#define TTF_MAX_PAGES   64

typedef struct
{
	uint32_t ch;              // codepoint
	int advance;              // pixel advance
	int page;                 // atlas page index
	wrect_t rc;               // position inside atlas page
} ttf_glyph_t;

typedef struct ttf_font_s
{
	byte *data;               // font file buffer
	stbtt_fontinfo info;
	float scale;              // pixels per font unit
	int height;               // cell height (== font->charHeight)
	int ascent;               // baseline offset from cell top
	uint32_t *page;           // current page rgba buffer (reused)
	int pageTexnum[TTF_MAX_PAGES];
	int cursorPage;
	int cursorX, cursorY;
	qboolean pageDirty;        // glyph insertion modified current page
	ttf_glyph_t *glyphs;
	int numGlyphs;
	int maxGlyphs;
} ttf_font_t;

static int g_ttfFonts = 0;

qboolean CL_TtfAnyLoaded( void )
{
	return g_ttfFonts > 0;
}

qboolean CL_FontIsTtf( cl_font_t *font )
{
	return font && font->valid && font->type == FONT_TTF;
}

static void CL_TtfUploadPage( ttf_font_t *tf )
{
	rgbdata_t pic;
	string name;

	if( !tf->page )
		return;

	memset( &pic, 0, sizeof( pic ));
	pic.width = TTF_ATLAS_SIZE;
	pic.height = TTF_ATLAS_SIZE;
	pic.depth = 1;
	pic.type = PF_RGBA_32;
	pic.flags = IMAGE_HAS_COLOR | IMAGE_HAS_ALPHA;
	pic.buffer = (byte *)tf->page;
	pic.size = TTF_ATLAS_SIZE * TTF_ATLAS_SIZE * 4;

	Q_snprintf( name, sizeof( name ), "#ttf_atlas_%p_page%i", (void *)tf, tf->cursorPage );
	tf->pageTexnum[tf->cursorPage] = ref.dllFuncs.GL_LoadTextureFromBuffer(
		name, &pic, TF_FONT|TF_NEAREST, tf->pageTexnum[tf->cursorPage] != 0 );
	tf->pageDirty = false;
}

// make sure the glyph's page region is actually present in the texture
static int CL_TtfGlyphTexture( ttf_font_t *tf, ttf_glyph_t *g )
{
	if( g->page == tf->cursorPage && ( tf->pageDirty || !tf->pageTexnum[g->page] ) )
		CL_TtfUploadPage( tf );

	return tf->pageTexnum[g->page];
}

static qboolean CL_TtfAdvancePage( ttf_font_t *tf )
{
	if( tf->cursorY + tf->height > TTF_ATLAS_SIZE )
	{
		if( tf->cursorPage + 1 >= TTF_MAX_PAGES )
			return false; // atlas exhausted, glyphs will be dropped

		// current page must become drawable before we clear the buffer
		CL_TtfUploadPage( tf );
		tf->cursorPage++;
		tf->cursorX = 0;
		tf->cursorY = 0;
		tf->pageDirty = true;
		memset( tf->page, 0, TTF_ATLAS_SIZE * TTF_ATLAS_SIZE * 4 );
	}

	return true;
}

static ttf_glyph_t *CL_TtfGetGlyph( cl_font_t *font, uint32_t ch )
{
	ttf_font_t *tf = font->ttf;
	ttf_glyph_t *g;
	int i, advance, lsb;

	for( i = 0; i < tf->numGlyphs; i++ )
	{
		if( tf->glyphs[i].ch == ch )
			return &tf->glyphs[i];
	}

	if( tf->numGlyphs >= tf->maxGlyphs )
		return NULL; // glyph cache full

	if( !stbtt_FindGlyphIndex( &tf->info, ch ))
		return NULL; // glyph is not in the font

	stbtt_GetCodepointHMetrics( &tf->info, ch, &advance, &lsb );
	advance = Q_min( TTF_ATLAS_SIZE, Q_max( 1, (int)( advance * tf->scale + 0.5f )));

	// make sure the next row fits the current page
	if( !CL_TtfAdvancePage( tf ))
		return NULL; // atlas exhausted

	if( tf->cursorX + advance > TTF_ATLAS_SIZE )
	{
		tf->cursorX = 0;
		tf->cursorY += tf->height + 1;

		if( !CL_TtfAdvancePage( tf ))
			return NULL; // atlas exhausted
	}

	// rasterize
	int bw, bh, xoff, yoff;
	byte *bmp = stbtt_GetCodepointBitmap( &tf->info, tf->scale, tf->scale, ch, &bw, &bh, &xoff, &yoff );

	g = &tf->glyphs[tf->numGlyphs++];
	g->ch = ch;
	g->advance = advance;
	g->page = tf->cursorPage;
	g->rc.left = tf->cursorX;
	g->rc.right = tf->cursorX + advance;
	g->rc.top = tf->cursorY;
	g->rc.bottom = tf->cursorY + tf->height;

	// blit glyph into the page, glyph origin is baseline-relative
	if( bmp )
	{
		int yBase = tf->ascent + yoff;

		for( int y = 0; y < bh; y++ )
		{
			int dy = yBase + y;
			if( dy < 0 || dy >= tf->height )
				continue;

			for( int x = 0; x < bw; x++ )
			{
				int dx = xoff + x;
				// A glyph is clipped to its own cell; it cannot overwrite neighbours.
				if( dx < 0 || dx >= advance ) continue;
				tf->page[( tf->cursorY + dy ) * TTF_ATLAS_SIZE + tf->cursorX + dx] =
					0x00FFFFFF | (( uint32_t )bmp[y * bw + x] << 24 );
			}
		}

		stbtt_FreeBitmap( bmp, NULL );
	}

	tf->pageDirty = true;
	tf->cursorX += advance;

	return g;
}

int CL_TtfCharAdvance( cl_font_t *font, int ch )
{
	ttf_glyph_t *g = CL_TtfGetGlyph( font, ch );

	if( g )
		return g->advance;

	// measure only, don't render unknown glyphs
	int advance, lsb;
	ttf_font_t *tf = font->ttf;

	if( !stbtt_FindGlyphIndex( &tf->info, ch ))
		return 0;

	stbtt_GetCodepointHMetrics( &tf->info, ch, &advance, &lsb );
	return Q_max((int)( advance * tf->scale + 0.5f ), 1 );
}

static int CL_TtfCalcTabStop( const cl_font_t *font, int x )
{
	int space = Q_max( 1, font->charWidths['0'] );
	int tab = space * 6; // 6 spaces
	int stop = tab - x % tab;

	if( stop < space )
		return tab * 2 - x % tab; // select next

	return stop;
}

int CL_TtfDrawCharacter( float x, float y, int number, const rgba_t color, cl_font_t *font, int flags )
{
	ttf_font_t *tf = font->ttf;
	ttf_glyph_t *g;
	float w, h, dx, dy;

	if( number == ' ' )
		return font->charWidths[' '];

	if( number == '\t' )
		return CL_TtfCalcTabStop( font, x );

	g = CL_TtfGetGlyph( font, number );

	if( !g )
		return 0;

	if( !FBitSet( flags, FONT_DRAW_NOCOLOR ))
		ref.dllFuncs.Color4ub( color[0], color[1], color[2], color[3] );

	w = (float)( g->rc.right - g->rc.left );
	h = (float)( g->rc.bottom - g->rc.top );
	dx = x;
	dy = y;

	if( FBitSet( flags, FONT_DRAW_HUD ))
		SPR_AdjustSize( &dx, &dy, &w, &h );

	float s1 = (float)g->rc.left / TTF_ATLAS_SIZE;
	float t1 = (float)g->rc.top / TTF_ATLAS_SIZE;
	float s2 = (float)g->rc.right / TTF_ATLAS_SIZE;
	float t2 = (float)g->rc.bottom / TTF_ATLAS_SIZE;

	ref.dllFuncs.R_DrawStretchPic( dx, dy, w, h, s1, t1, s2, t2, CL_TtfGlyphTexture( tf, g ));

	return g->advance;
}

void CL_TtfFreeFont( cl_font_t *font )
{
	ttf_font_t *tf = font->ttf;

	if( !tf )
		return;

	for( int i = 0; i <= tf->cursorPage; i++ )
	{
		if( tf->pageTexnum[i] )
			ref.dllFuncs.GL_FreeTexture( tf->pageTexnum[i] );
	}

	if( tf->glyphs )
		Mem_Free( tf->glyphs );

	if( tf->page )
		Mem_Free( tf->page );

	if( tf->data )
		Mem_Free( tf->data );

	Mem_Free( tf );
	font->ttf = NULL;

	if( g_ttfFonts > 0 )
		g_ttfFonts--;
}

qboolean CL_TtfLoadFont( const char *path, cl_font_t *font, float scale, int tall, convar_t *rendermode )
{
	ttf_font_t *tf;
	fs_offset_t len = 0;
	int ascent, descent, lineGap;
	byte *data;

	if( font->valid )
		return true;

	data = g_fsapi.LoadFile( path, &len, false );
	if( !data )
	{
		Con_DPrintf( S_ERROR "CL_TtfLoadFont: couldn't load %s\n", path );
		return false;
	}

	tf = Mem_Calloc( host.imagepool, sizeof( *tf ));
	tf->data = data;
	tf->height = Q_min( TTF_ATLAS_SIZE / 2, Q_max( 8, tall ));
	tf->maxGlyphs = TTF_MAX_GLYPHS;
	tf->glyphs = Mem_Malloc( host.imagepool, sizeof( ttf_glyph_t ) * tf->maxGlyphs );
	tf->page = Mem_Calloc( host.imagepool, TTF_ATLAS_SIZE * TTF_ATLAS_SIZE * 4 );

	if( !stbtt_InitFont( &tf->info, data, 0 ))
	{
		Con_DPrintf( S_ERROR "CL_TtfLoadFont: %s is not a valid TrueType font\n", path );
		Mem_Free( tf->glyphs );
		Mem_Free( tf->page );
		Mem_Free( tf->data );
		Mem_Free( tf );
		return false;
	}

	tf->scale = stbtt_ScaleForPixelHeight( &tf->info, tf->height );
	stbtt_GetFontVMetrics( &tf->info, &ascent, &descent, &lineGap );
	tf->ascent = (int)( ascent * tf->scale + 0.5f );

	// fill ASCII widths for legacy layout code (console geometry, tab stops)
	for( int i = 0; i < 256; i++ )
	{
		int advance, lsb;

		font->charWidths[i] = 0;

		if( i == '\t' || i == '\n' )
			continue;

		if( stbtt_FindGlyphIndex( &tf->info, i ))
		{
			stbtt_GetCodepointHMetrics( &tf->info, i, &advance, &lsb );
			font->charWidths[i] = (byte)Q_min( 255, Q_max((int)( advance * tf->scale + 0.5f ), 1 ));
		}
	}

	font->hFontTexture = 0;
	font->scale = scale > 0.0f ? scale : 1.0f;
	font->charHeight = tf->height;
	font->type = FONT_TTF;
	font->rendermode = rendermode;
	font->valid = true;
	font->ttf = tf;

	g_ttfFonts++;

	Con_Printf( "CL_TtfLoadFont: loaded %s (%i px), Unicode text rendering enabled\n", path, tf->height );

	return true;
}
