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

#ifndef __ZL_FONT__
#define __ZL_FONT__

#include "ZL_File.h"
#include "ZL_Display.h"

//A precalculated text buffer that uses less performance to draw than ZL_Front::Draw.
//Preferred for drawing text that does not change every frame.
//Also supports horizontal align and word wrapping for multi-line text
struct ZL_TextBuffer
{
	ZL_TextBuffer();
	ZL_TextBuffer(const struct ZL_Font& font, const char *text = NULL);
	ZL_TextBuffer(const struct ZL_Font& font, const char *text, scalar max_width, bool word_wrap_newline = false);
	ZL_TextBuffer(const struct ZL_Font& font, scalar multiline_x_align, const char *text);
	ZL_TextBuffer(const struct ZL_Font& font, scalar multiline_x_align, const char *text, scalar max_width, bool word_wrap_newline = false);
	~ZL_TextBuffer();
	ZL_TextBuffer(const ZL_TextBuffer& source);
	ZL_TextBuffer &operator=(const ZL_TextBuffer& source);
	operator bool () const { return (impl!=NULL); }
	bool operator==(const ZL_TextBuffer &b) const { return (impl==b.impl); }
	bool operator!=(const ZL_TextBuffer &b) const { return (impl!=b.impl); }

	//Recreate buffer with new text or different font
	ZL_TextBuffer& SetText(const char* new_text);
	ZL_TextBuffer& SetText(const char* new_text, scalar max_width, bool word_wrap_newline = false);
	ZL_TextBuffer& SetText(scalar multiline_x_align, const char* new_text);
	ZL_TextBuffer& SetText(scalar multiline_x_align, const char* new_text, scalar max_width, bool word_wrap_newline = false);
	ZL_TextBuffer& SetFont(const struct ZL_Font& font, const char* reset_text);
	ZL_TextBuffer& SetFont(const struct ZL_Font& font, const char* reset_text, scalar max_width, bool word_wrap_newline = false);
	ZL_TextBuffer& SetFont(const struct ZL_Font& font, scalar multiline_x_align, const char* reset_text);
	ZL_TextBuffer& SetFont(const struct ZL_Font& font, scalar multiline_x_align, const char* reset_text, scalar max_width, bool word_wrap_newline = false);

	//Set text buffer settings
	ZL_TextBuffer& SetColor(const ZL_Color &color); //defaults to fonts color
	ZL_TextBuffer& SetDrawOrigin(ZL_Origin::Type draw_origin); //defaults to fonts origin
	ZL_TextBuffer& SetScale(scalar scale); //defaults to fonts scale
	ZL_TextBuffer& SetScale(scalar scalew, scalar scaleh); //defaults to fonts scale
	ZL_TextBuffer& SetMultiLineHorizontalAlign(scalar multiline_x_align); //0.0 = left, 0.5 = center, 1.0 = right

	//Get text buffer settings
	ZL_Color& GetColor() const;
	ZL_Origin::Type GetDrawOrigin() const;
	scalar GetScaleW() const;
	scalar GetScaleH() const;

	//Get calculated dimensions of this text buffer
	scalar GetHeight() const;
	scalar GetHeight(scalar scale) const;
	scalar GetWidth() const;
	scalar GetWidth(scalar scale) const;
	ZL_Vector GetDimensions() const;
	ZL_Vector GetDimensions(scalar scale) const;
	ZL_Vector GetDimensions(scalar scalew, scalar scaleh) const;
	void GetDimensions(scalar* w, scalar* h) const;
	void GetDimensions(scalar* w, scalar* h, scalar scale) const;
	void GetDimensions(scalar* w, scalar* h, scalar scalew, scalar scaleh) const;

	// Draw methods using specified x and y for position
	void Draw(scalar x, scalar y) const;
	void Draw(scalar x, scalar y, const ZL_Color &color) const;
	void Draw(scalar x, scalar y, ZL_Origin::Type draw_at_origin) const;
	void Draw(scalar x, scalar y, const ZL_Color &color, ZL_Origin::Type draw_at_origin) const;

	void Draw(scalar x, scalar y, scalar scale) const;
	void Draw(scalar x, scalar y, scalar scale, const ZL_Color &color) const;
	void Draw(scalar x, scalar y, scalar scale, ZL_Origin::Type draw_at_origin) const;
	void Draw(scalar x, scalar y, scalar scale, const ZL_Color &color, ZL_Origin::Type draw_at_origin) const;

	void Draw(scalar x, scalar y, scalar scalew, scalar scaleh) const;
	void Draw(scalar x, scalar y, scalar scalew, scalar scaleh, const ZL_Color &color) const;
	void Draw(scalar x, scalar y, scalar scalew, scalar scaleh, ZL_Origin::Type draw_at_origin) const;
	void Draw(scalar x, scalar y, scalar scalew, scalar scaleh, const ZL_Color &color, ZL_Origin::Type draw_at_origin) const;

	// Draw methods using point structure for position
	void Draw(const ZL_Vector &p) const;
	void Draw(const ZL_Vector &p, const ZL_Color &color) const;
	void Draw(const ZL_Vector &p, ZL_Origin::Type draw_at_origin) const;
	void Draw(const ZL_Vector &p, const ZL_Color &color, ZL_Origin::Type draw_at_origin) const;

	void Draw(const ZL_Vector &p, scalar scale) const;
	void Draw(const ZL_Vector &p, scalar scale, const ZL_Color &color) const;
	void Draw(const ZL_Vector &p, scalar scale, ZL_Origin::Type draw_at_origin) const;
	void Draw(const ZL_Vector &p, scalar scale, const ZL_Color &color, ZL_Origin::Type draw_at_origin) const;

	void Draw(const ZL_Vector &p, scalar scalew, scalar scaleh) const;
	void Draw(const ZL_Vector &p, scalar scalew, scalar scaleh, const ZL_Color &color) const;
	void Draw(const ZL_Vector &p, scalar scalew, scalar scaleh, ZL_Origin::Type draw_at_origin) const;
	void Draw(const ZL_Vector &p, scalar scalew, scalar scaleh, const ZL_Color &color, ZL_Origin::Type draw_at_origin) const;

	private: struct ZL_TextBuffer_Impl* impl;
};

//A font that can be either a bitmap based font or a true type font (TTF)
struct ZL_Font
{
	ZL_Font();
	ZL_Font(const ZL_FileLink& BitmapFontFile);
	ZL_Font(const ZL_FileLink& TruetypeFontFile, scalar height, bool pixel_exact = true, unsigned char outline_width = 0); //supports utf8
	ZL_Font(const ZL_FileLink& TruetypeFontFile, scalar height, bool pixel_exact, unsigned char outline_left, unsigned char outline_right, unsigned char outline_top, unsigned char outline_bottom); //supports utf8
	~ZL_Font();
	ZL_Font(const ZL_Font &source);
	ZL_Font &operator=(const ZL_Font &source);
	operator bool () const { return (impl!=NULL); }
	bool operator==(const ZL_Font &b) const { return (impl==b.impl); }
	bool operator!=(const ZL_Font &b) const { return (impl!=b.impl); }

	//Set font settings
	ZL_Font& SetColor(const ZL_Color &color); //defaults to white
	ZL_Font& SetCharSpacing(scalar CharSpacing); //defaults to 0
	ZL_Font& SetLineSpacing(scalar LineSpacing); //default is font dependent
	ZL_Font& SetDrawOrigin(ZL_Origin::Type draw_origin); //defaults to bottom_left
	ZL_Font& SetDrawAtBaseline(bool draw_at_baseline); //defaults to off for bitmap fonts, on for true type fonts
	ZL_Font& SetScale(scalar scale);
	ZL_Font& AddScale(scalar scale);
	ZL_Font& SetScale(scalar scalew, scalar scaleh);
	ZL_Font& AddScale(scalar scalew, scalar scaleh);

	//Get font settings
	ZL_Color& GetColor() const;
	scalar GetCharSpacing() const;
	scalar GetLineSpacing() const;
	ZL_Origin::Type GetDrawOrigin() const;
	bool GetDrawAtBaseline() const;
	scalar GetScaleW() const;
	scalar GetScaleH() const;

	scalar GetLineHeight() const;
	scalar GetLineHeight(scalar scale) const;

	// Get calculated dimensions of how large a certain drawn text would end up as
	scalar GetHeight(const char *text) const;
	scalar GetHeight(const char *text, scalar scale) const;
	scalar GetWidth(const char *text) const;
	scalar GetWidth(const char *text, scalar scale) const;
	ZL_Vector GetDimensions(const char *text) const;
	ZL_Vector GetDimensions(const char *text, scalar scale) const;
	ZL_Vector GetDimensions(const char *text, scalar scalew, scalar scaleh) const;
	void GetDimensions(const char *text, scalar* w, scalar* h) const;
	void GetDimensions(const char *text, scalar* w, scalar* h, scalar scale) const;
	void GetDimensions(const char *text, scalar* w, scalar* h, scalar scalew, scalar scaleh) const;

	// Draw methods using specified x and y for position
	void Draw(scalar x, scalar y, const char *text) const;
	void Draw(scalar x, scalar y, const char *text, const ZL_Color &color) const;
	void Draw(scalar x, scalar y, const char *text, ZL_Origin::Type draw_at_origin) const;
	void Draw(scalar x, scalar y, const char *text, const ZL_Color &color, ZL_Origin::Type draw_at_origin) const;

	void Draw(scalar x, scalar y, const char *text, scalar scale) const;
	void Draw(scalar x, scalar y, const char *text, scalar scale, const ZL_Color &color) const;
	void Draw(scalar x, scalar y, const char *text, scalar scale, ZL_Origin::Type draw_at_origin) const;
	void Draw(scalar x, scalar y, const char *text, scalar scale, const ZL_Color &color, ZL_Origin::Type draw_at_origin) const;

	void Draw(scalar x, scalar y, const char *text, scalar scalew, scalar scaleh) const;
	void Draw(scalar x, scalar y, const char *text, scalar scalew, scalar scaleh, const ZL_Color &color) const;
	void Draw(scalar x, scalar y, const char *text, scalar scalew, scalar scaleh, ZL_Origin::Type draw_at_origin) const;
	void Draw(scalar x, scalar y, const char *text, scalar scalew, scalar scaleh, const ZL_Color &color, ZL_Origin::Type draw_at_origin) const;

	// Draw methods using point structure for position
	void Draw(const ZL_Vector &p, const char *text) const;
	void Draw(const ZL_Vector &p, const char *text, const ZL_Color &color) const;
	void Draw(const ZL_Vector &p, const char *text, ZL_Origin::Type draw_at_origin) const;
	void Draw(const ZL_Vector &p, const char *text, const ZL_Color &color, ZL_Origin::Type draw_at_origin) const;

	void Draw(const ZL_Vector &p, const char *text, scalar scale) const;
	void Draw(const ZL_Vector &p, const char *text, scalar scale, const ZL_Color &color) const;
	void Draw(const ZL_Vector &p, const char *text, scalar scale, ZL_Origin::Type draw_at_origin) const;
	void Draw(const ZL_Vector &p, const char *text, scalar scale, const ZL_Color &color, ZL_Origin::Type draw_at_origin) const;

	void Draw(const ZL_Vector &p, const char *text, scalar scalew, scalar scaleh) const;
	void Draw(const ZL_Vector &p, const char *text, scalar scalew, scalar scaleh, const ZL_Color &color) const;
	void Draw(const ZL_Vector &p, const char *text, scalar scalew, scalar scaleh, ZL_Origin::Type draw_at_origin) const;
	void Draw(const ZL_Vector &p, const char *text, scalar scalew, scalar scaleh, const ZL_Color &color, ZL_Origin::Type draw_at_origin) const;

	// Create a text buffer using this font
	inline ZL_TextBuffer CreateBuffer(const char *text = NULL) { return ZL_TextBuffer(*this, text); }
	inline ZL_TextBuffer CreateBuffer(const char *text, scalar max_width, bool word_wrap_newline = false) { return ZL_TextBuffer(*this, text, max_width, word_wrap_newline); }
	inline ZL_TextBuffer CreateBuffer(scalar multiline_x_align, const char *text) { return ZL_TextBuffer(*this, multiline_x_align, text); }
	inline ZL_TextBuffer CreateBuffer(scalar multiline_x_align, const char *text, scalar max_width, bool word_wrap_newline = false) { return ZL_TextBuffer(*this, multiline_x_align, text, max_width, word_wrap_newline); }

	// Requests a limit of the number of bytes processed for the next call to GetWidth/GetHeight/GetDimensions/Draw/CreateBuffer
	void RequestCharLimit(int limitCount);
	void DrawCharLimit(int charLimit);

	private: struct ZL_Font_Impl* impl;
};

#endif
