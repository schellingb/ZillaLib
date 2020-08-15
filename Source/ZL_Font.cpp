/*
  ZillaLib
  Copyright (C) 2010-2020 Bernhard Schelling

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#include "ZL_Font.h"
#include "ZL_Texture_Impl.h"
#include "ZL_Platform.h"
#include "ZL_Data.h"
#include <vector>
#include "stb/stb_truetype.h"

struct ZL_Font_Impl_Settings
{
	ZL_Vector scale;
	ZL_Color color; ZL_Origin::Type draw_at_origin;
	ZL_Font_Impl_Settings() : scale(ZL_Vector::One), color(ZL_Color::White), draw_at_origin(ZL_Origin::BottomLeft) { }
};

struct ZL_Font_Impl : ZL_Impl, ZL_Font_Impl_Settings
{
	scalar fCharSpacing, fLineSpacing, fLineHeight, fSpaceWidth;
	int limitCount;
	int charLimit;
	bool draw_at_baseline;
	ZL_Font_Impl(bool draw_at_baseline) : fCharSpacing(0), fLineSpacing(0), fLineHeight(0), fSpaceWidth(0), limitCount(0), draw_at_baseline(draw_at_baseline) { }
	virtual ~ZL_Font_Impl() {}

	virtual void DoDraw(scalar x, scalar y, const char *text, scalar scalew, scalar scaleh, const ZL_Color &color) = 0;
	virtual GLsizei CountBuffer(const char *text, std::vector<int>* &vecTTFTexLastIndex) = 0;
	virtual void RenderBuffer(const char *text, GLscalar* vertices, GLscalar* texcoords, std::vector<int>* vecTTFTexLastIndex, GLsizei &len, scalar &width, scalar &height) = 0;
	virtual void DoDrawBuffer(std::vector<int>* vecTTFTexLastIndex, GLsizei len) = 0;
	virtual void GetDimensions(const char *text, scalar* width, scalar* height = NULL, bool resetLimitCount = true) = 0;

	ZL_Vector GetDrawOffset(const scalar& width, const scalar& height, ZL_Origin::Type draw_at_origin)
	{
		switch (draw_at_origin)
		{
			case ZL_Origin::TopLeft:       return ZL_Vector(          0, (draw_at_baseline ? 0 : -fLineHeight*s(0.8)));
			case ZL_Origin::TopCenter:     return ZL_Vector(-width*HALF, (draw_at_baseline ? 0 : -fLineHeight*s(0.8)));
			case ZL_Origin::TopRight:      return ZL_Vector(-width     , (draw_at_baseline ? 0 : -fLineHeight*s(0.8)));
			case ZL_Origin::CenterLeft:    return ZL_Vector(          0, height*HALF-fLineHeight*s(0.8));
			case ZL_Origin::Center:        return ZL_Vector(-width*HALF, height*HALF-fLineHeight*s(0.8));
			case ZL_Origin::CenterRight:   return ZL_Vector(-width     , height*HALF-fLineHeight*s(0.8));
			case ZL_Origin::BottomLeft:    return ZL_Vector(          0, (height - (draw_at_baseline ? fLineHeight : fLineHeight*s(0.8))));
			case ZL_Origin::BottomCenter:  return ZL_Vector(-width*HALF, (height - (draw_at_baseline ? fLineHeight : fLineHeight*s(0.8))));
			case ZL_Origin::BottomRight:   return ZL_Vector(-width     , (height - (draw_at_baseline ? fLineHeight : fLineHeight*s(0.8))));
			default:
				ZL_Vector custom_origin = ZL_Origin::FromCustom(draw_at_origin);
				return ZL_Vector(-width*custom_origin.x, height*custom_origin.y-fLineHeight*(draw_at_baseline ? custom_origin.y : s(0.8)));
		}
	}

	void Draw(scalar x, scalar y, const char *text, scalar scalew, scalar scaleh, const ZL_Color &color, ZL_Origin::Type draw_at_origin)
	{
		scalar width, height;
		GetDimensions(text,
			(draw_at_origin < ZL_Origin::_CUSTOM_START && (draw_at_origin & ZL_Origin::_MASK_LEFT) ? NULL : &width), //width only needed in some cases
			(draw_at_origin < ZL_Origin::_CUSTOM_START && (draw_at_origin & ZL_Origin::_MASK_TOP) ? NULL : &height), //height only needed in some cases
			false);
		ZL_Vector align_offset = GetDrawOffset(width, height, draw_at_origin);
		DoDraw(x + align_offset.x * scalew, y + align_offset.y * scaleh, text, scalew, scaleh, color);
		limitCount = 0;
	}

	void DrawBuffer(const scalar &x, const scalar &y, const scalar &scalew, const scalar &scaleh, const ZL_Color &color, ZL_Origin::Type draw_at_origin, GLscalar* vertices, GLscalar* texcoords, std::vector<int>* vecTTFTexLastIndex, GLsizei len, const scalar &width, const scalar &height)
	{
		ZL_Vector align_offset = GetDrawOffset(width, height, draw_at_origin);
		GLPUSHMATRIX();
		ZLGL_DISABLE_PROGRAM();
		GLTRANSLATE(x + align_offset.x * scalew, y + align_offset.y * scaleh);
		if (scalew != 1 || scaleh != 1) GLSCALE(scalew, scaleh);
		ZLGL_ENABLE_TEXTURE();
		ZLGL_COLOR(color);
		ZLGL_VERTEXTPOINTER(2, GL_SCALAR, 0, vertices);
		ZLGL_TEXCOORDPOINTER(2, GL_SCALAR, 0, texcoords);
		DoDrawBuffer(vecTTFTexLastIndex, len);
		GLPOPMATRIX();
	}
};

struct ZL_FontBitmap_Impl : ZL_Font_Impl
{
	ZL_Texture_Impl* tex;
	GLscalar CharWidths[0x100 - ' '-1];
	GLscalar TextureCoordinates[(0x100 - ' '-1)][8];
	struct ZL_FontCharRect { int left, right, top, bottom; };

	ZL_FontBitmap_Impl(const ZL_FileLink& file) : ZL_Font_Impl(false)
	{
		ZL_BitmapSurface Bitmap;
		tex = ZL_Texture_Impl::LoadTextureRef(file, &Bitmap);
		if (!tex->gltexid) { free(Bitmap.pixels); tex->DelRef(); tex = NULL; return; }

		std::vector<ZL_FontCharRect> rects;
		int LineCount = 0;
		int bpp = Bitmap.BytesPerPixel, pitch = Bitmap.w * bpp;
		for (unsigned char *pStart = Bitmap.pixels + (tex->h-1) * pitch + bpp - 1, *pEnd = pStart - tex->h * pitch, *pLineMin = pStart, *pLineMax = pStart; pLineMax >= pEnd; pLineMax -= pitch)
		{
			bool IsSplitRow = true;
			if (pLineMax >= Bitmap.pixels) for (unsigned char *p = pLineMax, *pLineEnd = pLineMax + pitch; p != pLineEnd; p += bpp) if (*p) { IsSplitRow = false;  break; }
			if (!IsSplitRow) continue;
			if (pLineMin == pLineMax) { pLineMin = pLineMax-pitch; continue; }

			for (unsigned char *pCharMin = pLineMin, *pCharMax = pLineMin, *pLineEnd = pLineMin + pitch; pCharMax <= pLineEnd; pCharMax += bpp)
			{
				bool IsSplitColumn = true;
				for (unsigned char *p = pCharMax; pCharMax < pLineEnd && p >= pLineMax + pitch; p -= pitch) if (*p) { IsSplitColumn = false; break; }
				if (!IsSplitColumn) continue;
				if (pCharMin == pCharMax) { pCharMin = pCharMax+bpp; continue; }

				ZL_FontCharRect r = { (int)(pCharMin - pLineMin)/bpp, (int)(pCharMax - pLineMin)/bpp, (int)(pLineMin - pEnd) / pitch, (int)(pLineMax - pEnd) / pitch };
				rects.push_back(r);

				pCharMin = pCharMax+bpp;
			}
			LineCount++;
			pLineMin = pLineMax-pitch;
		}

		////Uncomment this to disable bilinear filtering on bitmap font texture
		//tex->SetTextureFilter(GL_NEAREST, GL_NEAREST);

		ZL_FontCharRect spaceRect = rects[0];
		int spaceHeight = spaceRect.top - spaceRect.bottom;
		bool exact = ((LineCount * (spaceHeight + 1) - 1) == (spaceRect.top - rects.back().bottom));

		int LineHeight = (exact ? (spaceHeight + 1) : (tex->h / (LineCount ? LineCount : 1)));
		fLineHeight = s(LineHeight-1);
		fSpaceWidth = s(spaceRect.right - spaceRect.left);

		GLuint iCharCount = (rects.size() >= 0x7F-' '-1 ? 0x100-' '-1 : (GLuint)(rects.size()-1));
		memset(CharWidths, 0, sizeof(CharWidths));
		for (GLuint i = 1; i < rects.size(); i++)
		{
			ZL_FontCharRect &rect = rects[i];
			GLuint iCharIndex = (i >= 0x7F-' '-1 ? iCharCount - (GLuint)(rects.size() - i) : i-1);
			CharWidths[iCharIndex] = s(rect.right - rect.left);
			int charLine = (spaceRect.top - ((rect.bottom + rect.top) / 2)) / LineHeight;
			rect.top    = spaceRect.top    - charLine * LineHeight;
			rect.bottom = spaceRect.bottom - charLine * LineHeight;

			GLscalar *pTexCoord = TextureCoordinates[iCharIndex];
			pTexCoord[0] = pTexCoord[4] = ((GLscalar)rect.left   / tex->wTex);
			pTexCoord[2] = pTexCoord[6] = ((GLscalar)rect.right  / tex->wTex);
			pTexCoord[1] = pTexCoord[3] = ((GLscalar)rect.bottom / tex->hTex);
			pTexCoord[5] = pTexCoord[7] = ((GLscalar)rect.top    / tex->hTex);
		}
		free(Bitmap.pixels);
	}

	~ZL_FontBitmap_Impl()
	{
		if (tex) tex->DelRef();
	}

	void DoDraw(scalar x, scalar y, const char *text, scalar scalew, scalar scaleh, const ZL_Color &color)
	{
		glBindTexture(GL_TEXTURE_2D, tex->gltexid);
		ZLGL_ENABLE_TEXTURE();
		ZLGL_COLOR(color);
		#define VBSIZE 64
		GLscalar vbox[8], vertices[12*VBSIZE], texcoords[12*VBSIZE];
		GLscalar cs = (scalew * fCharSpacing), lh = (scaleh * fLineHeight);
		vbox[7] = vbox[5] = y + lh*s(0.8); //top
		vbox[1] = vbox[3] = y - lh*s(0.2); //bottom
		vbox[0] = vbox[4] = x;             //left
		int v = 0;
		ZLGL_TEXCOORDPOINTER(2, GL_SCALAR, 0, texcoords);
		ZLGL_VERTEXTPOINTER(2, GL_SCALAR, 0, vertices);
		for (const unsigned char *p = (const unsigned char*)text, *pEnd = (limitCount ? p + limitCount : (unsigned char*)-1); *p && p < pEnd; p++)
		{
			if (*p == '\r') continue;
			if (*p == '\n') { vbox[4] = vbox[0] = x; vbox[7] = vbox[5] -= (lh + (scaleh * fLineSpacing)); vbox[1] = vbox[3] -= (lh + (scaleh * fLineSpacing)); continue; }
			if (*p <= ' ') { vbox[4] = vbox[0] = vbox[0] + (scalew * fSpaceWidth) + cs; continue; }
			unsigned char charidx = *p-' '-1;
			if (charidx >= (sizeof(CharWidths)/sizeof(CharWidths[0])) || !CharWidths[charidx]) continue;
			vbox[6] = vbox[2] = vbox[0] + (scalew * CharWidths[charidx]);
			memcpy(&texcoords[12*v+0], TextureCoordinates[*p-' '-1]+0, sizeof(texcoords[0])*6);
			memcpy(&texcoords[12*v+6], TextureCoordinates[*p-' '-1]+2, sizeof(texcoords[0])*6);
			memcpy(&vertices[12*v+0], vbox+0, sizeof(texcoords[0])*6);
			memcpy(&vertices[12*v+6], vbox+2, sizeof(texcoords[0])*6);
			if (++v==VBSIZE) {
				glDrawArraysUnbuffered(GL_TRIANGLES, 0, 6*VBSIZE);
				v = 0;
			}
			vbox[4] = vbox[0] = vbox[2] + cs;
		}
		if (v) glDrawArraysUnbuffered(GL_TRIANGLES, 0, 6*v);
	}

	GLsizei CountBuffer(const char *text, std::vector<int>* &vecTTFTexLastIndex)
	{
		if (vecTTFTexLastIndex) { delete vecTTFTexLastIndex; vecTTFTexLastIndex = NULL; }
		GLsizei cnt = 0;
		for (const unsigned char *p = (const unsigned char*)text, *pEnd = (limitCount ? p + limitCount : (unsigned char*)-1); *p && p < pEnd; p++)
			if (*p > ' ' && (*p-(unsigned char)' '-1) < (unsigned char)(sizeof(CharWidths)/sizeof(CharWidths[0])) && CharWidths[*p-' '-1]) cnt++;
		return cnt;
	}

	void RenderBuffer(const char *text, GLscalar* vertices, GLscalar* texcoords, std::vector<int>* vecTTFTexLastIndex, GLsizei &len, scalar &width, scalar &height)
	{
		GLscalar vbox[8];
		vbox[7] = vbox[5] =  fLineHeight*s(0.8); //top
		vbox[1] = vbox[3] = -fLineHeight*s(0.2); //bottom
		vbox[0] = vbox[4] = 0;                   //left
		width = 0;
		int vv = 0;
		for (const unsigned char *p = (const unsigned char*)text, *pEnd = (limitCount ? p + limitCount : (unsigned char*)-1); *p && p < pEnd; p++)
		{
			if (*p == '\r') continue;
			if (*p == '\n') { if (vbox[2] > width) width = vbox[2]; vbox[4] = vbox[0] = 0; vbox[7] = vbox[5] -= (fLineHeight + fLineSpacing); vbox[1] = vbox[3] -= (fLineHeight + fLineSpacing); continue; }
			if (*p <= ' ') { vbox[4] = vbox[0] = vbox[0] + fSpaceWidth + fCharSpacing; continue; }
			unsigned char charidx = *p-' '-1;
			if (charidx >= (sizeof(CharWidths)/sizeof(CharWidths[0])) || !CharWidths[charidx]) continue;
			vbox[6] = vbox[2] = vbox[0] + CharWidths[charidx];
			memcpy(&texcoords[vv+0], TextureCoordinates[*p-' '-1]+0, sizeof(texcoords[0])*6);
			memcpy(&texcoords[vv+6], TextureCoordinates[*p-' '-1]+2, sizeof(texcoords[0])*6);
			memcpy(&vertices[vv+0], vbox+0, sizeof(vertices[0])*6);
			memcpy(&vertices[vv+6], vbox+2, sizeof(vertices[0])*6);
			vbox[4] = vbox[0] = vbox[2] + fCharSpacing;
			vv += 12;
		}
		if (vbox[2] > width) width = vbox[2];
		height = 0 - (vbox[1] - fLineHeight*s(0.8));
	}

	void DoDrawBuffer(std::vector<int>* vecTTFTexLastIndex, GLsizei len)
	{
		glBindTexture(GL_TEXTURE_2D, tex->gltexid);
		glDrawArraysUnbuffered(GL_TRIANGLES, 0, len * 6);
	}

	void GetDimensions(const char *text, scalar* width, scalar* height, bool resetLimitCount)
	{
		GLscalar cs = 0, linewidth;
		if (width) *width = 0, linewidth = 0, cs = fCharSpacing;
		if (height) *height = fLineHeight;
		for (const unsigned char *p = (const unsigned char*)text, *pEnd = (limitCount ? p + limitCount : (unsigned char*)-1); *p && p < pEnd; p++)
		{
			if (*p == '\n')
			{
				if (height) { *height += fLineHeight + fLineSpacing; }
				if (width) { if (linewidth - cs > *width) { *width = linewidth - cs; } linewidth = 0; }
				continue;
			}
			if (*p == '\r' || !width) continue;
			if (*p <= ' ') linewidth += fSpaceWidth + cs;
			else
			{
				unsigned char charidx = *p-' '-1;
				if (charidx < (sizeof(CharWidths)/sizeof(CharWidths[0])) && CharWidths[charidx])
					linewidth += CharWidths[charidx] + cs;
			}
		}
		if (width) *width = (linewidth - cs > *width ? linewidth - cs : *width);
		if (resetLimitCount) limitCount = 0;
	}
};

#ifdef ZL_VIDEO_WEAKCONTEXT
//#include <algorithm>
static std::vector<struct ZL_FontTTF_Impl*> *pLoadedTTFFonts = NULL;
static std::vector<struct ZL_TextBuffer_Impl*> *pActiveTTFTextBuffers = NULL;
#endif

struct ZL_FontTTF_Impl : ZL_Font_Impl
{
	unsigned char *ttf_buffer;
	stbtt_fontinfo font;
	float stbtt_scale;
	std::vector<GLuint> gltexids;
	struct Char { signed char tex; GLscalar offx, offy, advance, width, TextureCoordinates[8]; };
	ZL_TMap<unsigned short, Char> chars;
	GLscalar last_char_coord1, last_char_coord2, last_char_coord5;
	bool last_char;
	bool pixel_exact;
	unsigned char oll, olr, olt, olb;

	ZL_FontTTF_Impl(const ZL_FileLink& file, scalar height, bool pixel_exact, unsigned char oll, unsigned char olr, unsigned char olt, unsigned char olb)
		: ZL_Font_Impl(true), ttf_buffer(NULL), last_char(false), pixel_exact(pixel_exact), oll(oll), olr(olr), olt(olt), olb(olb)
	{
		ZL_File f = file.Open();
		ZL_File_Impl* fileimpl = ZL_ImplFromOwner<ZL_File_Impl>(f);
		if (!fileimpl || !fileimpl->src) return;
		ptrdiff_t cur = fileimpl->src->seektell(0, RW_SEEK_CUR);
		char magic[2]; fileimpl->src->read(magic, 1, 2);
		fileimpl->src->seektell(cur, RW_SEEK_SET);
		if (magic[0] == 'P' && magic[1] == 'K')
		{
			ttf_buffer = ZL_RWopsZIP::ReadSingle(fileimpl->src);
			if (!ttf_buffer) return;
		}
		else
		{
			size_t ttf_size = fileimpl->src->size();
			if (!ttf_size) return;
			ttf_buffer = (unsigned char*)malloc(ttf_size);
			fileimpl->src->read(ttf_buffer, 1, ttf_size);
		}
		//if (!stbtt_InitFont(&font, ttf_buffer, stbtt_GetFontOffsetForIndex(ttf_buffer,0))) return; //offset for ttc
		if (!stbtt_InitFont(&font, ttf_buffer, 0)) return;
		stbtt_scale = stbtt_ScaleForPixelHeight(&font, (float)height);
		fLineHeight = height;
		fLineSpacing = s((int)(fLineHeight / 7));
		int iSpaceWidth = 10;
		stbtt_GetCodepointHMetrics(&font, ' ', &iSpaceWidth, NULL);
		fSpaceWidth = s(stbtt_scale * s(iSpaceWidth));
		if (pixel_exact) fSpaceWidth = (GLscalar)(int)(fSpaceWidth + .5f);
		#ifdef ZL_VIDEO_WEAKCONTEXT
		if (!pLoadedTTFFonts) pLoadedTTFFonts = new std::vector<ZL_FontTTF_Impl*>();
		pLoadedTTFFonts->push_back(this);
		#endif
	}

	~ZL_FontTTF_Impl()
	{
		if (ttf_buffer) free(ttf_buffer);
		if (gltexids.size()) glDeleteTextures((GLsizei)gltexids.size(), &gltexids[0]);
		#ifdef ZL_VIDEO_WEAKCONTEXT
		std::vector<ZL_FontTTF_Impl*>::iterator itClear = std::find(pLoadedTTFFonts->begin(), pLoadedTTFFonts->end(), this);
		if (itClear != pLoadedTTFFonts->end()) pLoadedTTFFonts->erase(itClear);
		#endif
	}

	const Char& MakeChar(unsigned short cd)
	{
		int bmp_w, bmp_h, off_x, off_y;
		int glyph = stbtt_FindGlyphIndex(&font, cd), advanceWidth, leftSideBearing;
		unsigned char *bitmap = stbtt_GetGlyphBitmap(&font, stbtt_scale, stbtt_scale, glyph, &bmp_w, &bmp_h, &off_x, &off_y);

		if (!bitmap || !bmp_w)
		{
			Char c;
			memset(&c, 0, sizeof(Char));
			c.tex = -1;
			return chars.Put(cd, c);
		}

		#ifndef ZL_VIDEO_DIRECT3D
		#define ZLFONTVIDEO_FORMAT GL_LUMINANCE_ALPHA
		#define ZLFONTVIDEO_BPP 2
		#else //D3D must be RGBA
		#define ZLFONTVIDEO_FORMAT GL_RGBA
		#define ZLFONTVIDEO_BPP 4
		#endif

		GLuint gltexid, x, y, tex_w = bmp_w + oll + olr, tex_h = bmp_h + olt + olb;
		bool textnextline;
		if (!last_char || ((textnextline = ((last_char_coord2 + s(tex_w)/s(512)) >= 1)) && (last_char_coord1 + s(tex_h)/s(512)) >= 1))
		{
			glGenTextures(1, &gltexid);
			glBindTexture(GL_TEXTURE_2D, gltexid);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			GLubyte *data = (GLubyte*)malloc(512*512*ZLFONTVIDEO_BPP);
			memset(data, 0, (512*512*ZLFONTVIDEO_BPP));
			glTexImage2D(GL_TEXTURE_2D, 0, ZLFONTVIDEO_FORMAT, 512, 512, 0, ZLFONTVIDEO_FORMAT, GL_UNSIGNED_BYTE, data);
			free(data);
			gltexids.push_back(gltexid);
			x = y = 0;
		}
		else
		{
			gltexid = gltexids.back();
			glBindTexture(GL_TEXTURE_2D, gltexid);
			x = (GLuint)(textnextline ? 0 : last_char_coord2 * 512.0f + 1);
			y = (GLuint)(textnextline ? last_char_coord1 * 512.0f + 1 : last_char_coord5 * 512.0f);
		}

		glPixelStorei(GL_UNPACK_ALIGNMENT, (!(tex_w&7) ? 8 : (!(tex_w&3) ? 4 : (!(tex_w&1) ? 2 : 1))));
		unsigned char *tex = (unsigned char*)malloc(tex_w*tex_h*ZLFONTVIDEO_BPP), *texalpha = tex+(ZLFONTVIDEO_BPP-1);
		memset(tex, 0xFF, tex_w*tex_h*ZLFONTVIDEO_BPP); //clear alpha and lum to fully lit
		unsigned char *bitmapend = bitmap+(bmp_w*bmp_h);
		if (tex_w == (GLuint)bmp_w && tex_h == (GLuint)bmp_h)
		{
			//just set alpha to the value of input if tex and bmp sizes match
			for (unsigned char *ptex = texalpha, *pin = bitmap; pin != bitmapend; ptex+=ZLFONTVIDEO_BPP)
				*ptex = *(pin++);
		}
		else
		{
			//clear alpha to 0, set alpha to bmp for all outline directions, clear alpha in center to keep only outline
			int texstride = tex_w*ZLFONTVIDEO_BPP;
			for (unsigned char *ptex = texalpha, *ptexend = ptex+tex_h*texstride; ptex != ptexend; ptex+=ZLFONTVIDEO_BPP) *ptex = 0;
			for (int r = 0, rw = oll+1+olr, rh = olt+1+olb, rwh = rw*rh; r < rwh; ++r)
				for (unsigned char *pin = bitmap, *ptexline = texalpha+ZLFONTVIDEO_BPP*((tex_w*(r/rw))+(r%rw)); pin != bitmapend; ptexline+=texstride)
					for (unsigned char *ptex = ptexline, *pinlineend = pin + bmp_w; pin != pinlineend; ptex+=ZLFONTVIDEO_BPP, ++pin)
						if (*pin > *ptex) *ptex = *pin;
			for (unsigned char *pin = bitmap, *ptexline = texalpha+ZLFONTVIDEO_BPP*((tex_w*olt)+(oll)); pin != bitmapend; ptexline+=texstride)
				for (unsigned char *ptex = ptexline, *pinlineend = pin + bmp_w; pin != pinlineend; ptex+=ZLFONTVIDEO_BPP, ++pin)
					*ptex -= *pin;
		}
		stbtt_FreeBitmap(bitmap, NULL);
		glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, tex_w, tex_h, ZLFONTVIDEO_FORMAT, GL_UNSIGNED_BYTE, tex);
		free(tex);

		//ZL_Application::Log("FONT", "Char %c at off %d, %d with size %d, %d (glyoff: %d, %d)", (char)cd, x, y, bmp_w, bmp_h, off_x, off_y);

		stbtt_GetGlyphHMetrics(&font, glyph, &advanceWidth, &leftSideBearing);

		Char c;
		c.tex = (signed char)(gltexids.size()-1);
		c.width = (GLscalar)tex_w;
		c.advance = (GLscalar)(stbtt_scale * (GLscalar)advanceWidth);
		c.offx = (GLscalar)(stbtt_scale * (GLscalar)leftSideBearing)-oll;
		c.offy = (GLscalar)off_y-olt;
		if (pixel_exact)
		{
			c.advance = (GLscalar)(int)(c.advance + .5f);
			c.offx = (GLscalar)(int)(c.offx + .5f);
		}
		GLscalar *pTexCoord = c.TextureCoordinates;
		pTexCoord[0] = pTexCoord[4] = (GLscalar)x / s(512);
		last_char_coord5 = pTexCoord[5] = pTexCoord[7] = (GLscalar)y / s(512);
		last_char_coord2 = pTexCoord[2] = pTexCoord[6] = (GLscalar)(x + tex_w) / s(512);
		last_char_coord1 = pTexCoord[1] = pTexCoord[3] = (GLscalar)(y + fLineHeight + (olt+olb)) / s(512);
		last_char = true;
		return chars.Put(cd, c);
	}

	#define UCS(txt, sz, cd) (txt[0] < 0xc2 ? sz=1,cd=txt[0] : \
	        (txt[0] > 0xdf && txt[0] < 0xf0 ? sz=(txt[1] && txt[2] ? 3 : 1),cd=((txt[0]&0x1f)<<12) + ((txt[1]&0x7f)<<6) + (txt[2]&0x7f) : \
	        (txt[0] > 0xc1 && txt[0] < 0xe0 ? sz=(txt[1] ? 2 : 1),cd=((txt[0]&0x3f)<<6) + (txt[1]&0x7f) : sz=4,cd=0)))

	GLsizei CountBuffer(const char *text, std::vector<int>* &vecTTFTexLastIndex)
	{
		GLsizei cnt = 0;
		unsigned char sz;
		unsigned short cd;
		int *texoffsetbegin, *texoffsetend;
		if (!vecTTFTexLastIndex) vecTTFTexLastIndex = new std::vector<int>();
		else vecTTFTexLastIndex->clear();
		vecTTFTexLastIndex->resize(gltexids.size());
		if (gltexids.size()) { texoffsetbegin = &vecTTFTexLastIndex->operator[](0); texoffsetend = &vecTTFTexLastIndex->back(); }
		else texoffsetbegin = texoffsetend = NULL;
		for (const unsigned char *p = (const unsigned char*)text, *pEnd = (limitCount ? p + limitCount : (unsigned char*)-1); *p && p < pEnd; p += sz)
		{
			if (*p <= ' ') { sz = 1; continue; }
			UCS(p, sz, cd);
			if (!cd) continue;
			signed char tex;
			const Char& c = chars.Get(cd);
			if (&c == &chars.NotFoundValue)
			{
				tex = MakeChar(cd).tex;
				if (gltexids.size() != vecTTFTexLastIndex->size())
					{ vecTTFTexLastIndex->resize(gltexids.size()); texoffsetbegin = &vecTTFTexLastIndex->operator[](0); texoffsetend = &vecTTFTexLastIndex->back(); }
			}
			else tex = c.tex;
			if (tex < 0) continue;
			for (int *texoffset = texoffsetbegin + tex + 1; texoffset <= texoffsetend; ++texoffset) (*texoffset)++;
			cnt++;
		}
		return cnt;
	}

	void RenderBuffer(const char *text, GLscalar* vertices, GLscalar* texcoords, std::vector<int>* vecTTFTexLastIndex, GLsizei &len, scalar &width, scalar &height)
	{
		scalar x = 0, y = 0, lh = (fLineHeight+(olt+olb));
		unsigned char sz;
		unsigned short cd;
		width = 0;
		for (const unsigned char *p = (const unsigned char*)text, *pEnd = (limitCount ? p + limitCount : (unsigned char*)-1); *p && p < pEnd; p += sz)
		{
			if (*p == '\r') { sz = 1; continue; }
			if (*p == '\n') { sz = 1; if (x - fCharSpacing + olr > width) width = x - fCharSpacing + olr; x = 0; y -= (fLineHeight + fLineSpacing); continue; }
			if (*p <= ' ' ) { sz = 1; x += fSpaceWidth + fCharSpacing; continue; }
			UCS(p, sz, cd);
			if (!cd) continue;
			const Char& c = chars.Get(cd);
			if (c.tex < 0) { x += fSpaceWidth + fCharSpacing; continue; }
			int vv = 12 * (vecTTFTexLastIndex->operator[](c.tex)++);
			memcpy(&texcoords[vv+0], c.TextureCoordinates+0, sizeof(texcoords[0])*6);
			memcpy(&texcoords[vv+6], c.TextureCoordinates+2, sizeof(texcoords[0])*6);
			vertices[vv+0] = vertices[vv+4] = vertices[vv+ 8] = x + c.offx;             //left
			vertices[vv+2] = vertices[vv+6] = vertices[vv+10] = vertices[vv] + c.width; //right
			vertices[vv+5] = vertices[vv+9] = vertices[vv+11] = y - c.offy;             //top
			vertices[vv+1] = vertices[vv+3] = vertices[vv+ 7] = vertices[vv+5] - lh;    //bottom
			x += c.advance + fCharSpacing;
		}
		height = 0 - (y - fLineHeight);
		if (x - fCharSpacing + olr > width) width = x - fCharSpacing + olr;
	}

	void DoDrawBuffer(std::vector<int>* vecTTFTexLastIndex, GLsizei len)
	{
		for (int texcount = (int)vecTTFTexLastIndex->size(), offset = 0, tex = 0; tex < texcount; tex++)
		{
			int last = vecTTFTexLastIndex->operator[](tex);
			if (last == offset) continue;
			glBindTexture(GL_TEXTURE_2D, gltexids[tex]);
			glDrawArraysUnbuffered(GL_TRIANGLES, offset*6, (last-offset)*6);
			offset = last;
		}
	}

	void DoDraw(scalar x, scalar y, const char *text, scalar scalew, scalar scaleh, const ZL_Color &color)
	{
		scalar xleft = x;
		GLscalar vertices[12*VBSIZE], texcoords[12*VBSIZE];
		ZLGL_ENABLE_TEXTURE();
		ZLGL_COLOR(color);
		ZLGL_TEXCOORDPOINTER(2, GL_SCALAR, 0, texcoords);
		ZLGL_VERTEXTPOINTER(2, GL_SCALAR, 0, vertices);
		GLscalar cs = fCharSpacing, lh = (scaleh * (fLineHeight+(olt+olb)));
		int v = 0;
		signed char last_tex = -1;
		unsigned char sz;
		unsigned short cd;
		for (const unsigned char *p = (const unsigned char*)text, *pEnd = (limitCount ? p + limitCount : (unsigned char*)-1); *p && p < pEnd; p += sz)
		{
			if (*p == '\r') { sz = 1; continue; }
			if (*p == '\n') { sz = 1; x = xleft; y -= ((fLineHeight+fLineSpacing)*scaleh); continue; }
			if (*p <= ' ' ) { sz = 1; x += (fSpaceWidth + cs) * scalew; continue; }
			UCS(p, sz, cd);
			if (!cd) continue;
			const Char& it = chars.Get(cd);
			const Char& c = (&it == &chars.NotFoundValue ? MakeChar(cd) : it);
			if (c.tex < 0) { x += (fSpaceWidth + cs) * scalew; continue; }
			if (last_tex != c.tex)
			{
				if (v) { glDrawArraysUnbuffered(GL_TRIANGLES, 0, 6*v); v = 0; }
				last_tex = c.tex;
				glBindTexture(GL_TEXTURE_2D, gltexids[last_tex]);
			}
			int vv = 12 * v++;
			memcpy(&texcoords[vv+0], c.TextureCoordinates+0, sizeof(texcoords[0])*6);
			memcpy(&texcoords[vv+6], c.TextureCoordinates+2, sizeof(texcoords[0])*6);
			vertices[vv+0] = vertices[vv+4] = vertices[vv+ 8] = x + (c.offx*scalew);               //left
			vertices[vv+2] = vertices[vv+6] = vertices[vv+10] = vertices[vv+0] + (c.width*scalew); //right
			vertices[vv+5] = vertices[vv+9] = vertices[vv+11] = y - (c.offy*scaleh);               //top
			vertices[vv+1] = vertices[vv+3] = vertices[vv+ 7] = vertices[vv+5] - lh;               //bottom
			if (v == VBSIZE) { glDrawArraysUnbuffered(GL_TRIANGLES, 0, 6*VBSIZE); v = 0; }
			x += (c.advance + cs) * scalew;
		}
		if (v) glDrawArraysUnbuffered(GL_TRIANGLES, 0, 6*v);
	}

	void GetDimensions(const char *text, scalar* width, scalar* height, bool resetLimitCount)
	{
		GLscalar cs = 0, linewidth;
		if (width) *width = linewidth = 0, cs = fCharSpacing;
		if (height) *height = fLineHeight;
		unsigned char sz;
		unsigned short cd;
		const Char* c = NULL;
		for (const unsigned char *p = (const unsigned char*)text, *pEnd = (limitCount ? p + limitCount : (unsigned char*)-1);; p += sz)
		{
			if (!*p || p >= pEnd || *p == '\n')
			{
				if (width)
				{
					if (linewidth > *width) { *width = linewidth; }
					linewidth = 0;
				}
				if (!*p || p >= pEnd)
				{
					if (resetLimitCount) limitCount = 0;
					return;
				}
				if (height) { *height += fLineHeight + fLineSpacing; }
				sz = 1; c = NULL; continue;
			}
			if (*p == '\r' || !width) { sz = 1; continue; }
			if (*p <= ' ') { linewidth += fSpaceWidth + cs; sz = 1; c = NULL; continue; }
			UCS(p, sz, cd);
			if (!cd) continue;
			const Char& it = chars.Get(cd);
			c = (&it == &chars.NotFoundValue ? &MakeChar(cd) : &it);
			linewidth += (c->tex < 0 ? fSpaceWidth : c->advance) + cs;
		}
	}
};

ZL_IMPL_OWNER_DEFAULT_IMPLEMENTATIONS(ZL_Font)

ZL_Font::ZL_Font(const ZL_FileLink& BitmapFontFile) : impl(new ZL_FontBitmap_Impl(BitmapFontFile))
{
	if (!((ZL_FontBitmap_Impl*)impl)->tex) { delete (ZL_FontBitmap_Impl*)impl; impl = NULL; }
}

ZL_Font::ZL_Font(const ZL_FileLink& TruetypeFontFile, scalar height, bool pixel_exact, unsigned char ol) : impl(new ZL_FontTTF_Impl(TruetypeFontFile, height, pixel_exact, ol, ol, ol, ol))
{
	if (!impl->fLineHeight) { delete (ZL_FontTTF_Impl*)impl; impl = NULL; }
}

ZL_Font::ZL_Font(const ZL_FileLink& TruetypeFontFile, scalar height, bool pixel_exact, unsigned char oll, unsigned char olr, unsigned char olt, unsigned char olb) : impl(new ZL_FontTTF_Impl(TruetypeFontFile, height, pixel_exact, oll, olr, olt, olb))
{
	if (!impl->fLineHeight) { delete (ZL_FontTTF_Impl*)impl; impl = NULL; }
}

ZL_Font& ZL_Font::SetColor(const ZL_Color& color)
{
	if (impl) impl->color = color;
	return *this;
}

ZL_Font& ZL_Font::SetCharSpacing(scalar CharSpacing)
{
	if (impl) impl->fCharSpacing = CharSpacing;
	return *this;
}

ZL_Font& ZL_Font::SetLineSpacing(scalar LineSpacing)
{
	if (impl) impl->fLineSpacing = LineSpacing;
	return *this;
}

ZL_Font& ZL_Font::SetDrawOrigin(ZL_Origin::Type draw_origin)
{
	if (impl) impl->draw_at_origin = draw_origin;
	return *this;
}

ZL_Font& ZL_Font::SetDrawAtBaseline(bool draw_at_baseline)
{
	if (impl) impl->draw_at_baseline = draw_at_baseline;
	return *this;
}

ZL_Font& ZL_Font::SetScale(scalar scale)
{
	if (impl) impl->scale.x = impl->scale.y = scale;
	return *this;
}

ZL_Font& ZL_Font::AddScale(scalar scale)
{
	if (impl) { impl->scale.x += scale; impl->scale.y += scale; }
	return *this;
}

ZL_Font& ZL_Font::SetScale(scalar scalew, scalar scaleh)
{
	if (impl) { impl->scale.x = scalew; impl->scale.y = scaleh; }
	return *this;
}

ZL_Font& ZL_Font::AddScale(scalar scalew, scalar scaleh)
{
	if (impl) { impl->scale.x += scalew; impl->scale.y += scaleh; }
	return *this;
}

ZL_Color& ZL_Font::GetColor() const { return impl->color; }
scalar ZL_Font::GetCharSpacing() const { return (impl ? impl->fCharSpacing : s(0)); }
scalar ZL_Font::GetLineSpacing() const { return (impl ? impl->fLineSpacing : s(0)); }
ZL_Origin::Type ZL_Font::GetDrawOrigin() const { return (impl ? impl->draw_at_origin : ZL_Origin::BottomLeft); }
bool ZL_Font::GetDrawAtBaseline() const { return (impl ? impl->draw_at_baseline : false); }
scalar ZL_Font::GetScaleW() const { return (impl ? impl->scale.x : s(1)); }
scalar ZL_Font::GetScaleH() const { return (impl ? impl->scale.x : s(1)); }

scalar ZL_Font::GetLineHeight() const { return (impl ? impl->fLineHeight*impl->scale.y : s(0)); }
scalar ZL_Font::GetLineHeight(scalar scale) const { return (impl ? impl->fLineHeight*scale : s(0)); }

scalar ZL_Font::GetHeight(const char *text) const { if (!impl) return 0; scalar h; impl->GetDimensions(text, NULL, &h); return h*impl->scale.y; }
scalar ZL_Font::GetHeight(const char *text, scalar scale) const { if (!impl) return 0; scalar h; impl->GetDimensions(text, NULL, &h); return h*scale; }
scalar ZL_Font::GetWidth(const char *text) const { if (!impl) return 0; scalar w; impl->GetDimensions(text, &w, NULL); return w*impl->scale.x; }
scalar ZL_Font::GetWidth(const char *text, scalar scale) const  { if (!impl) return 0; scalar w; impl->GetDimensions(text, &w, NULL); return w*scale; }
ZL_Vector ZL_Font::GetDimensions(const char *text) const { ZL_Vector s; if (impl) impl->GetDimensions(text, &s.x, &s.y); return s * impl->scale; }
ZL_Vector ZL_Font::GetDimensions(const char *text, scalar scale) const { ZL_Vector s; if (impl) impl->GetDimensions(text, &s.x, &s.y); return s*scale; }
ZL_Vector ZL_Font::GetDimensions(const char *text, scalar scalew, scalar scaleh) const { ZL_Vector s; if (impl) impl->GetDimensions(text, &s.x, &s.y); return s * ZL_Vector(scalew, scaleh); }
void ZL_Font::GetDimensions(const char *text, scalar* w, scalar* h) const { if (impl) { impl->GetDimensions(text, w, h); *w *= impl->scale.x; *h *= impl->scale.y; } else *w = *h = 0; }
void ZL_Font::GetDimensions(const char *text, scalar* w, scalar* h, scalar scale) const { if (impl) { impl->GetDimensions(text, w, h); *w *= scale; *h *=  scale; } else *w = *h = 0; }
void ZL_Font::GetDimensions(const char *text, scalar* w, scalar* h, scalar scalew, scalar scaleh) const { if (impl) { impl->GetDimensions(text, w, h); *w *= scalew; *h *= scaleh; } else *w = *h = 0; }

void ZL_Font::Draw(scalar x, scalar y, const char *text) const
{ if (impl) impl->Draw(x, y, text, impl->scale.x, impl->scale.y, impl->color, impl->draw_at_origin); }
void ZL_Font::Draw(scalar x, scalar y, const char *text, const ZL_Color &color) const
{ if (impl) impl->Draw(x, y, text, impl->scale.x, impl->scale.y, color, impl->draw_at_origin); }
void ZL_Font::Draw(scalar x, scalar y, const char *text, ZL_Origin::Type draw_at_origin) const
{ if (impl) impl->Draw(x, y, text, impl->scale.x, impl->scale.y, impl->color, draw_at_origin); }
void ZL_Font::Draw(scalar x, scalar y, const char *text, const ZL_Color &color, ZL_Origin::Type draw_at_origin) const
{ if (impl) impl->Draw(x, y, text, impl->scale.x, impl->scale.y, color, draw_at_origin); }

void ZL_Font::Draw(scalar x, scalar y, const char *text, scalar scale) const
{ if (impl) impl->Draw(x, y, text, scale, scale, impl->color, impl->draw_at_origin); }
void ZL_Font::Draw(scalar x, scalar y, const char *text, scalar scale, const ZL_Color &color) const
{ if (impl) impl->Draw(x, y, text, scale, scale, color, impl->draw_at_origin); }
void ZL_Font::Draw(scalar x, scalar y, const char *text, scalar scale, ZL_Origin::Type draw_at_origin) const
{ if (impl) impl->Draw(x, y, text, scale, scale, impl->color, draw_at_origin); }
void ZL_Font::Draw(scalar x, scalar y, const char *text, scalar scale, const ZL_Color &color, ZL_Origin::Type draw_at_origin) const
{ if (impl) impl->Draw(x, y, text, scale, scale, color, draw_at_origin); }

void ZL_Font::Draw(scalar x, scalar y, const char *text, scalar scalew, scalar scaleh) const
{ if (impl) impl->Draw(x, y, text, scalew, scaleh, impl->color, impl->draw_at_origin); }
void ZL_Font::Draw(scalar x, scalar y, const char *text, scalar scalew, scalar scaleh, const ZL_Color &color) const
{ if (impl) impl->Draw(x, y, text, scalew, scaleh, color, impl->draw_at_origin); }
void ZL_Font::Draw(scalar x, scalar y, const char *text, scalar scalew, scalar scaleh, ZL_Origin::Type draw_at_origin) const
{ if (impl) impl->Draw(x, y, text, scalew, scaleh, impl->color, draw_at_origin); }
void ZL_Font::Draw(scalar x, scalar y, const char *text, scalar scalew, scalar scaleh, const ZL_Color &color, ZL_Origin::Type draw_at_origin) const
{ if (impl) impl->Draw(x, y, text, scalew, scaleh, color, draw_at_origin); }

void ZL_Font::Draw(const ZL_Vector &p, const char *text) const
{ if (impl) impl->Draw(p.x, p.y, text, impl->scale.x, impl->scale.y, impl->color, impl->draw_at_origin); }
void ZL_Font::Draw(const ZL_Vector &p, const char *text, const ZL_Color &color) const
{ if (impl) impl->Draw(p.x, p.y, text, impl->scale.x, impl->scale.y, color, impl->draw_at_origin); }
void ZL_Font::Draw(const ZL_Vector &p, const char *text, ZL_Origin::Type draw_at_origin) const
{ if (impl) impl->Draw(p.x, p.y, text, impl->scale.x, impl->scale.y, impl->color, draw_at_origin); }
void ZL_Font::Draw(const ZL_Vector &p, const char *text, const ZL_Color &color, ZL_Origin::Type draw_at_origin) const
{ if (impl) impl->Draw(p.x, p.y, text, impl->scale.x, impl->scale.y, color, draw_at_origin); }

void ZL_Font::Draw(const ZL_Vector &p, const char *text, scalar scale) const
{ if (impl) impl->Draw(p.x, p.y, text, scale, scale, impl->color, impl->draw_at_origin); }
void ZL_Font::Draw(const ZL_Vector &p, const char *text, scalar scale, const ZL_Color &color) const
{ if (impl) impl->Draw(p.x, p.y, text, scale, scale, color, impl->draw_at_origin); }
void ZL_Font::Draw(const ZL_Vector &p, const char *text, scalar scale, ZL_Origin::Type draw_at_origin) const
{ if (impl) impl->Draw(p.x, p.y, text, scale, scale, impl->color, draw_at_origin); }
void ZL_Font::Draw(const ZL_Vector &p, const char *text, scalar scale, const ZL_Color &color, ZL_Origin::Type draw_at_origin) const
{ if (impl) impl->Draw(p.x, p.y, text, scale, scale, color, draw_at_origin); }

void ZL_Font::Draw(const ZL_Vector &p, const char *text, scalar scalew, scalar scaleh) const
{ if (impl) impl->Draw(p.x, p.y, text, scalew, scaleh, impl->color, impl->draw_at_origin); }
void ZL_Font::Draw(const ZL_Vector &p, const char *text, scalar scalew, scalar scaleh, const ZL_Color &color) const
{ if (impl) impl->Draw(p.x, p.y, text, scalew, scaleh, color, impl->draw_at_origin); }
void ZL_Font::Draw(const ZL_Vector &p, const char *text, scalar scalew, scalar scaleh, ZL_Origin::Type draw_at_origin) const
{ if (impl) impl->Draw(p.x, p.y, text, scalew, scaleh, impl->color, draw_at_origin); }
void ZL_Font::Draw(const ZL_Vector &p, const char *text, scalar scalew, scalar scaleh, const ZL_Color &color, ZL_Origin::Type draw_at_origin) const
{ if (impl) impl->Draw(p.x, p.y, text, scalew, scaleh, color, draw_at_origin); }

void ZL_Font::RequestCharLimit(int limitCount) { impl->limitCount = limitCount; }
void ZL_Font::DrawCharLimit(int charLimit) { impl->charLimit = charLimit; }

/* alternative using parameter struct for 2 or more options instead of that many overloaded draw methods
void Draw(const ZL_Vector &p, const char *text, scalar scale) const;
void Draw(const ZL_Vector &p, const char *text, const ZL_FontDrawDef &def) const;
struct ZL_FontDrawDef
{
	scalar scalew, scaleh; ZL_Origin::Type draw_at_origin; const ZL_Color& color; bool hasscale, hascolor;
	inline ZL_FontDrawDef(const ZL_Color& color, ZL_Origin::Type draw_at_origin)                                              :                                 draw_at_origin(draw_at_origin), color(color)                , hascolor(true) , hasscale(false) {}
	inline ZL_FontDrawDef(scalar scale, ZL_Origin::Type draw_at_origin)                                                       : scalew(scale) , scaleh(scale) , draw_at_origin(draw_at_origin), color(ZL_Color::Transparent), hascolor(false), hasscale(true)  {}
	inline ZL_FontDrawDef(scalar scale, const ZL_Color& color)                                                          : scalew(scale) , scaleh(scale) , draw_at_origin(draw_at_origin), color(color)                , hascolor(true) , hasscale(true)  {}
	inline ZL_FontDrawDef(scalar scale, const ZL_Color& color, ZL_Origin::Type draw_at_origin = (ZL_Origin::Type)0)                 : scalew(scalew), scaleh(scaleh), draw_at_origin(draw_at_origin), color(color)                , hascolor(true) , hasscale(true)  {}
	inline ZL_FontDrawDef(scalar scalew, scalar scaleh)                                                                 : scalew(scalew), scaleh(scaleh), draw_at_origin(draw_at_origin), color(ZL_Color::Transparent), hascolor(false), hasscale(true)  {}
	inline ZL_FontDrawDef(scalar scalew, scalar scaleh, ZL_Origin::Type draw_at_origin)                                       : scalew(scalew), scaleh(scaleh), draw_at_origin(draw_at_origin), color(ZL_Color::Transparent), hascolor(false), hasscale(true)  {}
	inline ZL_FontDrawDef(scalar scalew, scalar scaleh, const ZL_Color& color, ZL_Origin::Type draw_at_origin = (ZL_Origin::Type)0) : scalew(scalew), scaleh(scaleh), draw_at_origin(draw_at_origin), color(color)                , hascolor(true) , hasscale(true)  {}
};
*/

struct ZL_TextBuffer_Impl : ZL_Impl
{
	ZL_Font_Impl* fnt;
	ZL_Font_Impl_Settings* fntSettings;
	GLscalar *vertices, *texcoords;
	scalar width, height;
	std::vector<int>* vecTTFTexLastIndex;
	GLsizei len, lenmax;
	#ifdef ZL_VIDEO_WEAKCONTEXT
	ZL_String TTFBufferString;
	#endif

	ZL_TextBuffer_Impl(const ZL_Font& font) : fnt(ZL_ImplFromOwner<ZL_Font_Impl>(font)), fntSettings(fnt), vertices(NULL), texcoords(NULL), vecTTFTexLastIndex(NULL), len(0), lenmax(0)
	{
		if (fnt) fnt->AddRef();
	}

	inline void SetFont(const ZL_Font& font)
	{
		if (fntSettings == fnt) fntSettings = NULL;
		if (fnt) fnt->DelRef();
		fnt = ZL_ImplFromOwner<ZL_Font_Impl>(font);
		if (fnt) fnt->AddRef();
		if (!fntSettings) fntSettings = fnt;
	}

	size_t FindMaxWidthIndex(char *t, size_t end, scalar max_width, bool word_wrap_newline)
	{
		size_t test = end/2;
		for (size_t start = 0; test != start; test = start + (end - start) / 2)
		{
			char testc = t[test];
			t[test] = '\0';
			scalar check_width;
			fnt->GetDimensions(t, &check_width);
			t[test] = testc;
			(check_width*fntSettings->scale.x <= max_width ? start : end) = test;
		}

		if (!test) return 1; //safeguard against infiniteloop if max_width is less than width of a character
		if (!word_wrap_newline) return test;
		size_t lineMaxLetterBreakIndex = test;
		for (; test > 0 && t[test] != ' '; test--) {}
		return (test ? test : lineMaxLetterBreakIndex);
	}

	void RenderMaxWidth(const char *text, scalar max_width, bool word_wrap_newline)
	{
		if (!text || !text[0] || !fnt) { Render(text); return; }
		scalar check_width;
		fnt->limitCount = 0;
		fnt->GetDimensions(text, &check_width);
		if (check_width*fntSettings->scale.x <= max_width) { Render(text); return; }

		std::vector<char> textbuf;
		textbuf.assign(text, text + strlen(text) + 1);
		for (size_t breakIndex = 0; ;)
		{
			breakIndex += FindMaxWidthIndex(&textbuf[breakIndex], textbuf.size()-breakIndex, max_width, word_wrap_newline);
			if (!word_wrap_newline) { textbuf[breakIndex] = '\0'; break; }

			if (textbuf[breakIndex] == ' ') textbuf[breakIndex] = '\n';
			else textbuf.insert(textbuf.begin() + breakIndex, '\n');
			breakIndex++;

			fnt->GetDimensions(&textbuf[breakIndex], &check_width);
			if (check_width*fntSettings->scale.x <= max_width) break;
		}
		Render(&textbuf[0]);
	}

	void Render(const char *text)
	{
		GLsizei newlen = (text && text[0] && fnt ? fnt->CountBuffer(text, vecTTFTexLastIndex) : 0);
		if (!fnt || newlen > lenmax)
		{
			ZL_ASSERTMSG(fnt || !text || !text[0], "The font needs to be loaded before rendering to a text buffer with it");
			if (vertices) { delete vertices; vertices = NULL; }
			if (texcoords) { delete texcoords; texcoords = NULL; }
		}
		len = newlen;
		if (newlen > lenmax) lenmax = newlen;
		if (!newlen || !fnt)
		{
			if (vecTTFTexLastIndex) { delete vecTTFTexLastIndex; vecTTFTexLastIndex = NULL; }
		}
		else
		{
			if (!vertices) vertices = new GLscalar[12*lenmax];
			if (!texcoords) texcoords = new GLscalar[12*lenmax];
			fnt->RenderBuffer(text, vertices, texcoords, vecTTFTexLastIndex, len, width, height);
			//ZL_LOG3("FONT", "Rendered Text Buffer: %s - Len: %d - vec: %d", text, len, (vecTTFTexLastIndex != NULL));
		}
		fnt->limitCount = 0;
		#ifdef ZL_VIDEO_WEAKCONTEXT
		if (vecTTFTexLastIndex)
		{
			if (!pActiveTTFTextBuffers) pActiveTTFTextBuffers = new std::vector<ZL_TextBuffer_Impl*>();
			std::vector<ZL_TextBuffer_Impl*>::iterator itAlready = std::find(pActiveTTFTextBuffers->begin(), pActiveTTFTextBuffers->end(), this);
			if (itAlready == pActiveTTFTextBuffers->end()) pActiveTTFTextBuffers->push_back(this);
			TTFBufferString = text;
		}
		else
		{
			if (pActiveTTFTextBuffers)
			{
				std::vector<ZL_TextBuffer_Impl*>::iterator itClear = std::find(pActiveTTFTextBuffers->begin(), pActiveTTFTextBuffers->end(), this);
				if (itClear != pActiveTTFTextBuffers->end()) pActiveTTFTextBuffers->erase(itClear);
			}
			TTFBufferString = ZL_String::EmptyString;
		}
		#endif
	}

	void SetMultiLineHorizontalAlign(scalar align)
	{
		if (!len || !fnt || height < fnt->fLineHeight*s(1.5)) return;
		GLscalar *verticesEnd = &vertices[len*12], *v;
		for (GLscalar line = (fnt->fLineHeight + fnt->fLineSpacing), halfline = line*s(0.49), y_line = fnt->fLineHeight*s(0.367), y_end = -height + fnt->fLineHeight; y_line > y_end; y_line -= line)
		{
			GLscalar xmin = S_MAX, xmax = 0;
			for (v = vertices; v != verticesEnd; v += 12)
			{
				if (sabs(y_line - v[5]) >= halfline) continue;
				if (xmin > v[0]) xmin = v[0];
				if (xmax < v[2]) xmax = v[2];
			}
			if (xmax == width) continue;

			GLscalar offset = (width - xmax + xmin) * align - xmin;
			if (sabs(offset) < s(0.005)) continue;

			for (v = vertices; v != verticesEnd; v += 12)
			{
				if (sabs(y_line - v[5]) >= halfline) continue;
				v[0] = v[4] = v[ 8] += offset; //left
				v[2] = v[6] = v[10] += offset; //right
			}
		}
	}

	~ZL_TextBuffer_Impl()
	{
		#ifdef ZL_VIDEO_WEAKCONTEXT
		if (vecTTFTexLastIndex)
		{
			std::vector<ZL_TextBuffer_Impl*>::iterator itClear = std::find(pActiveTTFTextBuffers->begin(), pActiveTTFTextBuffers->end(), this);
			if (itClear != pActiveTTFTextBuffers->end()) pActiveTTFTextBuffers->erase(itClear);
		}
		#endif
		if (fntSettings != fnt) delete fntSettings;
		if (fnt) fnt->DelRef();
		if (vecTTFTexLastIndex) delete vecTTFTexLastIndex;
		if (vertices) delete vertices;
		if (texcoords) delete texcoords;
	}

	inline void Draw(const scalar &x, const scalar &y, const scalar &scalew, const scalar &scaleh, const ZL_Color &color, ZL_Origin::Type draw_at_origin)
	{
		if (fnt && vertices) fnt->DrawBuffer(x, y, scalew, scaleh, color, draw_at_origin, vertices, texcoords, vecTTFTexLastIndex, len, width, height);
	}

	inline void CustomizeSettings()
	{
		if (fntSettings == fnt) fntSettings = new ZL_Font_Impl_Settings(*fnt);
	}
};

ZL_IMPL_OWNER_DEFAULT_IMPLEMENTATIONS(ZL_TextBuffer)

ZL_TextBuffer::ZL_TextBuffer(const ZL_Font& font, const char *text) : impl(new ZL_TextBuffer_Impl(font))
{
	impl->Render(text);
}

ZL_TextBuffer::ZL_TextBuffer(const ZL_Font& font, const char *text, scalar max_width, bool word_wrap_newline) : impl(new ZL_TextBuffer_Impl(font))
{
	impl->RenderMaxWidth(text, max_width, word_wrap_newline);
}

ZL_TextBuffer::ZL_TextBuffer(const ZL_Font& font, scalar multiline_x_align, const char *text) : impl(new ZL_TextBuffer_Impl(font))
{
	impl->Render(text);
	if (multiline_x_align) impl->SetMultiLineHorizontalAlign(multiline_x_align);
}

ZL_TextBuffer::ZL_TextBuffer(const ZL_Font& font, scalar multiline_x_align, const char *text, scalar max_width, bool word_wrap_newline) : impl(new ZL_TextBuffer_Impl(font))
{
	impl->RenderMaxWidth(text, max_width, word_wrap_newline);
	if (multiline_x_align) impl->SetMultiLineHorizontalAlign(multiline_x_align);
}

ZL_TextBuffer& ZL_TextBuffer::SetText(const char* new_text)
{
	if (impl) impl->Render(new_text);
	return *this;
}

ZL_TextBuffer& ZL_TextBuffer::SetText(const char* new_text, scalar max_width, bool word_wrap_newline)
{
	if (impl) impl->RenderMaxWidth(new_text, max_width, word_wrap_newline);
	return *this;
}

ZL_TextBuffer& ZL_TextBuffer::SetText(scalar multiline_x_align, const char* new_text)
{
	if (impl) { impl->Render(new_text); if (multiline_x_align) impl->SetMultiLineHorizontalAlign(multiline_x_align); }
	return *this;
}

ZL_TextBuffer& ZL_TextBuffer::SetText(scalar multiline_x_align, const char* new_text, scalar max_width, bool word_wrap_newline)
{
	if (impl) { impl->RenderMaxWidth(new_text, max_width, word_wrap_newline); if (multiline_x_align) impl->SetMultiLineHorizontalAlign(multiline_x_align); }
	return *this;
}

ZL_TextBuffer& ZL_TextBuffer::SetFont(const ZL_Font& font, const char* reset_text)
{
	if (impl) { impl->SetFont(font); impl->Render(reset_text); }
	return *this;
}

ZL_TextBuffer& ZL_TextBuffer::SetFont(const ZL_Font& font, const char* reset_text, scalar max_width, bool word_wrap_newline)
{
	if (impl) { impl->SetFont(font); impl->RenderMaxWidth(reset_text, max_width, word_wrap_newline); }
	return *this;
}

ZL_TextBuffer& ZL_TextBuffer::SetFont(const ZL_Font& font, scalar multiline_x_align, const char* reset_text)
{
	if (impl) { impl->SetFont(font); impl->Render(reset_text); if (multiline_x_align) impl->SetMultiLineHorizontalAlign(multiline_x_align); }
	return *this;
}

ZL_TextBuffer& ZL_TextBuffer::SetFont(const ZL_Font& font, scalar multiline_x_align, const char* reset_text, scalar max_width, bool word_wrap_newline)
{
	if (impl) { impl->SetFont(font); impl->RenderMaxWidth(reset_text, max_width, word_wrap_newline); if (multiline_x_align) impl->SetMultiLineHorizontalAlign(multiline_x_align); }
	return *this;
}

ZL_TextBuffer& ZL_TextBuffer::SetColor(const ZL_Color &color)
{
	if (impl) { impl->CustomizeSettings(); impl->fntSettings->color = color; }
	return *this;
}

ZL_TextBuffer& ZL_TextBuffer::SetDrawOrigin(ZL_Origin::Type draw_origin)
{
	if (impl) { impl->CustomizeSettings(); impl->fntSettings->draw_at_origin = draw_origin; }
	return *this;
}

ZL_TextBuffer& ZL_TextBuffer::SetScale(scalar scale)
{
	if (impl) { impl->CustomizeSettings(); impl->fntSettings->scale.x = impl->fntSettings->scale.y = scale; }
	return *this;
}

ZL_TextBuffer& ZL_TextBuffer::SetScale(scalar scalew, scalar scaleh)
{
	if (impl) { impl->CustomizeSettings(); impl->fntSettings->scale.x = scalew; impl->fntSettings->scale.y = scaleh; }
	return *this;
}

ZL_TextBuffer& ZL_TextBuffer::SetMultiLineHorizontalAlign(scalar multiline_x_align)
{
	if (impl) { impl->SetMultiLineHorizontalAlign(multiline_x_align); }
	return *this;
}

ZL_Color& ZL_TextBuffer::GetColor() const { return impl->fntSettings->color; }
ZL_Origin::Type ZL_TextBuffer::GetDrawOrigin() const { return (impl ? impl->fntSettings->draw_at_origin : ZL_Origin::BottomLeft); }
scalar ZL_TextBuffer::GetScaleW() const { return (impl ? impl->fntSettings->scale.x : s(1)); }
scalar ZL_TextBuffer::GetScaleH() const { return (impl ? impl->fntSettings->scale.x : s(1)); }

scalar ZL_TextBuffer::GetHeight() const { return (impl && impl->fnt ? impl->height*impl->fnt->scale.y : 0); }
scalar ZL_TextBuffer::GetHeight(scalar scale) const { return (impl ? impl->height*scale : 0); }
scalar ZL_TextBuffer::GetWidth() const { return (impl && impl->fnt ? impl->width*impl->fnt->scale.x : 0); }
scalar ZL_TextBuffer::GetWidth(scalar scale) const { return (impl ? impl->width*scale : 0); }
ZL_Vector ZL_TextBuffer::GetDimensions() const { if (!impl || !impl->fnt) return ZL_Vector(); return ZL_Vector(impl->width*impl->fnt->scale.x, impl->height*impl->fnt->scale.y); }
ZL_Vector ZL_TextBuffer::GetDimensions(scalar scale) const { if (!impl) return ZL_Vector(); return ZL_Vector(impl->width*scale, impl->height*scale); }
ZL_Vector ZL_TextBuffer::GetDimensions(scalar scalew, scalar scaleh) const { if (!impl) return ZL_Vector(); return ZL_Vector(impl->width*scalew, impl->height*scaleh); }
void ZL_TextBuffer::GetDimensions(scalar* w, scalar* h) const { if (!impl || !impl->fnt) *w = *h = 0; else { *w = impl->width*impl->fnt->scale.x; *h = impl->height*impl->fnt->scale.y; } }
void ZL_TextBuffer::GetDimensions(scalar* w, scalar* h, scalar scale) const { if (!impl) *w = *h = 0; else { *w = impl->width*scale; *h = impl->height*scale; } }
void ZL_TextBuffer::GetDimensions(scalar* w, scalar* h, scalar scalew, scalar scaleh) const { if (!impl) *w = *h = 0; else { *w = impl->width*scalew; *h = impl->height*scaleh; } }

void ZL_TextBuffer::Draw(scalar x, scalar y) const
{ if (impl) impl->Draw(x, y, impl->fntSettings->scale.x, impl->fntSettings->scale.y, impl->fntSettings->color, impl->fntSettings->draw_at_origin); }
void ZL_TextBuffer::Draw(scalar x, scalar y, const ZL_Color &color) const
{ if (impl) impl->Draw(x, y, impl->fntSettings->scale.x, impl->fntSettings->scale.y, color, impl->fntSettings->draw_at_origin); }
void ZL_TextBuffer::Draw(scalar x, scalar y, ZL_Origin::Type draw_at_origin) const
{ if (impl) impl->Draw(x, y, impl->fntSettings->scale.x, impl->fntSettings->scale.y, impl->fntSettings->color, draw_at_origin); }
void ZL_TextBuffer::Draw(scalar x, scalar y, const ZL_Color &color, ZL_Origin::Type draw_at_origin) const
{ if (impl) impl->Draw(x, y, impl->fntSettings->scale.x, impl->fntSettings->scale.y, color, draw_at_origin); }

void ZL_TextBuffer::Draw(scalar x, scalar y, scalar scale) const
{ if (impl) impl->Draw(x, y, scale, scale, impl->fntSettings->color, impl->fntSettings->draw_at_origin); }
void ZL_TextBuffer::Draw(scalar x, scalar y, scalar scale, const ZL_Color &color) const
{ if (impl) impl->Draw(x, y, scale, scale, color, impl->fntSettings->draw_at_origin); }
void ZL_TextBuffer::Draw(scalar x, scalar y, scalar scale, ZL_Origin::Type draw_at_origin) const
{ if (impl) impl->Draw(x, y, scale, scale, impl->fntSettings->color, draw_at_origin); }
void ZL_TextBuffer::Draw(scalar x, scalar y, scalar scale, const ZL_Color &color, ZL_Origin::Type draw_at_origin) const
{ if (impl) impl->Draw(x, y, scale, scale, color, draw_at_origin); }

void ZL_TextBuffer::Draw(scalar x, scalar y, scalar scalew, scalar scaleh) const
{ if (impl) impl->Draw(x, y, scalew, scaleh, impl->fntSettings->color, impl->fntSettings->draw_at_origin); }
void ZL_TextBuffer::Draw(scalar x, scalar y, scalar scalew, scalar scaleh, const ZL_Color &color) const
{ if (impl) impl->Draw(x, y, scalew, scaleh, color, impl->fntSettings->draw_at_origin); }
void ZL_TextBuffer::Draw(scalar x, scalar y, scalar scalew, scalar scaleh, ZL_Origin::Type draw_at_origin) const
{ if (impl) impl->Draw(x, y, scalew, scaleh, impl->fntSettings->color, draw_at_origin); }
void ZL_TextBuffer::Draw(scalar x, scalar y, scalar scalew, scalar scaleh, const ZL_Color &color, ZL_Origin::Type draw_at_origin) const
{ if (impl) impl->Draw(x, y, scalew, scaleh, color, draw_at_origin); }

void ZL_TextBuffer::Draw(const ZL_Vector &p) const
{ if (impl) impl->Draw(p.x, p.y, impl->fntSettings->scale.x, impl->fntSettings->scale.y, impl->fntSettings->color, impl->fntSettings->draw_at_origin); }
void ZL_TextBuffer::Draw(const ZL_Vector &p, const ZL_Color &color) const
{ if (impl) impl->Draw(p.x, p.y, impl->fntSettings->scale.x, impl->fntSettings->scale.y, color, impl->fntSettings->draw_at_origin); }
void ZL_TextBuffer::Draw(const ZL_Vector &p, ZL_Origin::Type draw_at_origin) const
{ if (impl) impl->Draw(p.x, p.y, impl->fntSettings->scale.x, impl->fntSettings->scale.y, impl->fntSettings->color, draw_at_origin); }
void ZL_TextBuffer::Draw(const ZL_Vector &p, const ZL_Color &color, ZL_Origin::Type draw_at_origin) const
{ if (impl) impl->Draw(p.x, p.y, impl->fntSettings->scale.x, impl->fntSettings->scale.y, color, draw_at_origin); }

void ZL_TextBuffer::Draw(const ZL_Vector &p, scalar scale) const
{ if (impl) impl->Draw(p.x, p.y, scale, scale, impl->fntSettings->color, impl->fntSettings->draw_at_origin); }
void ZL_TextBuffer::Draw(const ZL_Vector &p, scalar scale, const ZL_Color &color) const
{ if (impl) impl->Draw(p.x, p.y, scale, scale, color, impl->fntSettings->draw_at_origin); }
void ZL_TextBuffer::Draw(const ZL_Vector &p, scalar scale, ZL_Origin::Type draw_at_origin) const
{ if (impl) impl->Draw(p.x, p.y, scale, scale, impl->fntSettings->color, draw_at_origin); }
void ZL_TextBuffer::Draw(const ZL_Vector &p, scalar scale, const ZL_Color &color, ZL_Origin::Type draw_at_origin) const
{ if (impl) impl->Draw(p.x, p.y, scale, scale, color, draw_at_origin); }

void ZL_TextBuffer::Draw(const ZL_Vector &p, scalar scalew, scalar scaleh) const
{ if (impl) impl->Draw(p.x, p.y, scalew, scaleh, impl->fntSettings->color, impl->fntSettings->draw_at_origin); }
void ZL_TextBuffer::Draw(const ZL_Vector &p, scalar scalew, scalar scaleh, const ZL_Color &color) const
{ if (impl) impl->Draw(p.x, p.y, scalew, scaleh, color, impl->fntSettings->draw_at_origin); }
void ZL_TextBuffer::Draw(const ZL_Vector &p, scalar scalew, scalar scaleh, ZL_Origin::Type draw_at_origin) const
{ if (impl) impl->Draw(p.x, p.y, scalew, scaleh, impl->fntSettings->color, draw_at_origin); }
void ZL_TextBuffer::Draw(const ZL_Vector &p, scalar scalew, scalar scaleh, const ZL_Color &color, ZL_Origin::Type draw_at_origin) const
{ if (impl) impl->Draw(p.x, p.y, scalew, scaleh, color, draw_at_origin); }

#ifdef ZL_VIDEO_WEAKCONTEXT
#ifndef ZL_VIDEO_USE_GLSL
bool CheckFontTexturesIfContextLost()
{
	return (pLoadedTTFFonts && pLoadedTTFFonts->size() && (*(pLoadedTTFFonts->begin()))->gltexids.size() && !glIsTexture(*(*(pLoadedTTFFonts->begin()))->gltexids.begin()));
}
#endif
void RecreateAllFontTexturesOnContextLost()
{
	if (!pLoadedTTFFonts) return;
	//for (std::vector<ZL_FontTTF_Impl*>::iterator itx = pLoadedTTFFonts->begin(); itx != pLoadedTTFFonts->end(); ++itx) for (std::vector<GLuint>::iterator ittex = (*itx)->gltexids.begin(); ittex != (*itx)->gltexids.end(); ++ittex) glDeleteTextures(1, &*ittex); //DEBUG
	ZL_LOG1("TEXTURE", "RecreateAllFontTexturesIfContextLost with %d fonts to reload", pLoadedTTFFonts->size());
	for (std::vector<ZL_FontTTF_Impl*>::iterator itTTFFont = pLoadedTTFFonts->begin(); itTTFFont != pLoadedTTFFonts->end(); ++itTTFFont)
	{
		(*itTTFFont)->last_char = false;
		(*itTTFFont)->chars.Clear();
		(*itTTFFont)->gltexids.clear();
	}
	ZL_LOG1("TEXTURE", "RecreateAllFontTexturesIfContextLost with %d ttf buffers to reload", (pActiveTTFTextBuffers ? pActiveTTFTextBuffers->size() : 0));
	if (pActiveTTFTextBuffers)
		for (std::vector<ZL_TextBuffer_Impl*>::iterator itTTFTB = pActiveTTFTextBuffers->begin(); itTTFTB != pActiveTTFTextBuffers->end(); ++itTTFTB)
			if ((*itTTFTB)->vecTTFTexLastIndex) (*itTTFTB)->Render((*itTTFTB)->TTFBufferString.c_str());
}
#endif
