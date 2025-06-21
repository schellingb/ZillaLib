/*
  ZillaLib
  Copyright (C) 2010-2025 Bernhard Schelling

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

#if defined(__wasm__) || defined(__EMSCRIPTEN__)

#include "ZL_Platform.h"
#include "ZL_Display.h"
#include "ZL_Display_Impl.h"
#include "ZL_Impl.h"
#include "ZL_File.h"
#include <GL/glut.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <assert.h>

extern "C" {
extern void ZLJS_CreateWindow(int width, int height, float tpf_limit);
extern int ZLJS_GetWidth();
extern int ZLJS_GetHeight();
extern void ZLJS_SetFullscreen(int flag);
extern void ZLJS_SetPointerLock(int flag);
extern unsigned int ZLJS_GetTime();
extern void ZLJS_AsyncLoad(const char* url, struct ZL_HttpConnection_Impl*, char* postdata, size_t postlength, unsigned int timeout);
extern void ZLJS_StartAudio();
extern void ZLJS_OpenExternalUrl(const char* url);
extern void ZLJS_SettingsInit(const char* prefix);
extern void ZLJS_SettingsSet(const char* key, const char* val);
extern void ZLJS_SettingsDel(const char* key);
extern bool ZLJS_SettingsHas(const char* key);
extern char* ZLJS_SettingsGetMalloc(const char* key);
extern void ZLJS_Websocket(struct ZL_WebSocketClient_Impl* impl, int cmd, const void* param, size_t len = 0);
};

static void ZL_WindowEvent(unsigned char event, int data1 = 0, int data2 = 0);
static unsigned int WEB_WindowFlags = ZL_WINDOW_INPUT_FOCUS | ZL_WINDOW_MOUSE_FOCUS;
static int Web_MouseState = 0;
struct WEB_Touch { int lastx, lasty, touchId; };
static WEB_Touch WEB_Touches[10];

static unsigned char WEB_GetTouchIndex(int touchId, bool returnFreeIfNotKnown)
{
	for (unsigned char i = 0; i < (unsigned char)COUNT_OF(WEB_Touches); i++)
		if (WEB_Touches[i].touchId == touchId) return i;
	return (returnFreeIfNotKnown ? WEB_GetTouchIndex(0, false) : 0xFF);
}

extern "C" void ZLFNDraw()
{
	ZL_MainApplication->Frame();
}

extern "C" void ZLFNText(unsigned int code)
{
	ZL_Event e;
	e.type = ZL_EVENT_TEXTINPUT;
	if (code >= 0x800) { e.text.text[0] = char(0xE0 + ((code >> 12) & 0xF)); e.text.text[1] = char(0x80 + ((code >> 6) & 0x3F)); e.text.text[2] = char(0x80 + ((code) & 0x3F)); e.text.text[3] = 0; }
	else if (code >= 0x80) { e.text.text[0] = char(0xC0 + ((code >> 6) & 0x1F)); e.text.text[1] = char(0x80 + ((code) & 0x3F)); e.text.text[2] = 0; }
	else { e.text.text[0] = char(code); e.text.text[1] = 0; }
	ZL_Display_Process_Event(e);
}

extern "C" void ZLFNKey(bool is_down, int key_code, bool shift, bool ctrl, bool alt, bool meta)
{
	static const ZL_Key dom_zlkey_table[] = {
		ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_CANCEL,        ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_HELP,          ZLK_UNKNOWN,       ZLK_BACKSPACE,     ZLK_TAB,
		ZLK_KP_ENTER,      ZLK_UNKNOWN,       ZLK_CLEAR,         ZLK_RETURN,        ZLK_KP_ENTER,      ZLK_UNKNOWN,       ZLK_LSHIFT,        ZLK_LCTRL,         ZLK_LALT,          ZLK_PAUSE,
		ZLK_CAPSLOCK,      ZLK_LANG3,         ZLK_LANG6,         ZLK_LANG7,         ZLK_LANG8,         ZLK_LANG2,         ZLK_LANG9,         ZLK_ESCAPE,        ZLK_UNKNOWN,       ZLK_UNKNOWN,
		ZLK_UNKNOWN,       ZLK_MODE,          ZLK_SPACE,         ZLK_PAGEUP,        ZLK_PAGEDOWN,      ZLK_END,           ZLK_HOME,          ZLK_LEFT,          ZLK_UP,            ZLK_RIGHT,
		ZLK_DOWN,          ZLK_SELECT,        ZLK_PRINTSCREEN,   ZLK_EXECUTE,       ZLK_PRINTSCREEN,   ZLK_INSERT,        ZLK_DELETE,        ZLK_HELP,          ZLK_0,             ZLK_1,
		ZLK_2,             ZLK_3,             ZLK_4,             ZLK_5,             ZLK_6,             ZLK_7,             ZLK_8,             ZLK_9,             ZLK_KP_COLON,      ZLK_SEMICOLON,
		ZLK_KP_LESS,       ZLK_EQUALS,        ZLK_KP_GREATER,    ZLK_UNKNOWN,       ZLK_KP_AT,         ZLK_A,             ZLK_B,             ZLK_C,             ZLK_D,             ZLK_E,
		ZLK_F,             ZLK_G,             ZLK_H,             ZLK_I,             ZLK_J,             ZLK_K,             ZLK_L,             ZLK_M,             ZLK_N,             ZLK_O,
		ZLK_P,             ZLK_Q,             ZLK_R,             ZLK_S,             ZLK_T,             ZLK_U,             ZLK_V,             ZLK_W,             ZLK_X,             ZLK_Y,
		ZLK_Z,             ZLK_LGUI,          ZLK_RGUI,          ZLK_APPLICATION,   ZLK_UNKNOWN,       ZLK_SLEEP,         ZLK_KP_0,          ZLK_KP_1,          ZLK_KP_2,          ZLK_KP_3,
		ZLK_KP_4,          ZLK_KP_5,          ZLK_KP_6,          ZLK_KP_7,          ZLK_KP_8,          ZLK_KP_9,          ZLK_KP_MULTIPLY,   ZLK_KP_PLUS,       ZLK_SEPARATOR,     ZLK_KP_MINUS,
		ZLK_KP_DECIMAL,    ZLK_KP_DIVIDE,     ZLK_F1,            ZLK_F2,            ZLK_F3,            ZLK_F4,            ZLK_F5,            ZLK_F6,            ZLK_F7,            ZLK_F8,
		ZLK_F9,            ZLK_F10,           ZLK_F11,           ZLK_F12,           ZLK_F13,           ZLK_F14,           ZLK_F15,           ZLK_F16,           ZLK_F17,           ZLK_F18,
		ZLK_F19,           ZLK_F20,           ZLK_F21,           ZLK_F22,           ZLK_F23,           ZLK_F24,           ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,
		ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_NUMLOCKCLEAR,  ZLK_SCROLLLOCK,    ZLK_KP_EQUALS,     ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,
		ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,
		ZLK_LSHIFT,        ZLK_RSHIFT,        ZLK_LCTRL,         ZLK_RCTRL,         ZLK_LALT,          ZLK_RALT,          ZLK_AC_BACK,       ZLK_AC_FORWARD,    ZLK_AC_REFRESH,    ZLK_AC_STOP,
		ZLK_AC_SEARCH,     ZLK_AC_BOOKMARKS,  ZLK_AC_HOME,       ZLK_AUDIOMUTE,     ZLK_VOLUMEDOWN,    ZLK_VOLUMEUP,      ZLK_UNKNOWN,       ZLK_KP_000,        ZLK_KP_EQUALS,     ZLK_KP_00,
		ZLK_MAIL,          ZLK_MEDIASELECT,   ZLK_WWW,           ZLK_CALCULATOR,    ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_SEMICOLON,     ZLK_EQUALS,        ZLK_COMMA,         ZLK_MINUS,
		ZLK_PERIOD,        ZLK_SLASH,         ZLK_GRAVE,         ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,
		ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,
		ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_LEFTBRACKET,
		ZLK_BACKSLASH,     ZLK_RIGHTBRACKET,  ZLK_APOSTROPHE,    ZLK_UNKNOWN,       ZLK_LGUI,          ZLK_RALT,          ZLK_NONUSBACKSLASH,ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,
		ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,
		ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_LANG1,         ZLK_UNKNOWN,       ZLK_SYSREQ,        ZLK_CRSEL,         ZLK_EXSEL,         ZLK_UNKNOWN,
		ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_UNKNOWN,       ZLK_CLEAR,         ZLK_UNKNOWN
	};
	ZL_Event e;
	e.type = (is_down ? ZL_EVENT_KEYDOWN : ZL_EVENT_KEYUP);
	e.key.is_down = is_down;
	e.key.key = dom_zlkey_table[key_code & 255];
	e.key.mod = (shift ? ZLKMOD_SHIFT : 0) + (ctrl ? ZLKMOD_CTRL : 0) + (alt ? ZLKMOD_ALT : 0) + (meta ? ZLKMOD_META : 0);
	ZL_Display_Process_Event(e); //process event asap, might change fullscreen, lock mouse or open window (requires call in user events call stack)
	if (e.key.is_down)
	{
		if (e.key.key == ZLK_RETURN || e.key.key == ZLK_KP_ENTER) ZLFNText('\n');
		else if (e.key.key == ZLK_BACKSPACE) ZLFNText('\b');
		else if (e.key.key == ZLK_TAB) ZLFNText('\t');
	}
}

extern "C" void ZLFNMove(int x, int y, int relx, int rely, int touchId)
{
	unsigned char touchIdx = WEB_GetTouchIndex(touchId, (touchId == 1));
	if (touchIdx == 0xFF) return;
	if ((WEB_WindowFlags & ZL_WINDOW_POINTERLOCK) && touchId == 1)
	{
		x = WEB_Touches[touchIdx].lastx + relx;
		y = WEB_Touches[touchIdx].lasty + rely;
		if (x > native_width -1) x = native_width -1; else if (x < 0) x = 0;
		if (y > native_height-1) y = native_height-1; else if (y < 0) y = 0;
	}
	else
	{
		relx = x - WEB_Touches[touchIdx].lastx;
		rely = y - WEB_Touches[touchIdx].lasty;
	}
	if (!relx && !rely) return;
	ZL_Event e;
	e.type = ZL_EVENT_MOUSEMOTION;
	e.motion.which = touchIdx;
	e.motion.state = (touchId == 1 ? Web_MouseState : 1);
	e.motion.x = x;
	e.motion.y = y;
	e.motion.xrel = relx;
	e.motion.yrel = rely;
	WEB_Touches[touchIdx].lastx = x;
	WEB_Touches[touchIdx].lasty = y;
	ZL_Display_Process_Event(e);
}

extern "C" void ZLFNMouse(int button, bool is_down, int x, int y, int touchId)
{
	unsigned char touchIdx = WEB_GetTouchIndex(touchId, (is_down || touchId == 1));
	//ZL_LOG("ZLFNMouse", "button: %d - is_down: %d - x: %d - y: %d - touchId: %d - touchIdx: %d", button, is_down, x, y, touchId, touchIdx);
	if (touchIdx == 0xFF) return;
	if (touchId == 1)
	{
		if (is_down) Web_MouseState |= 1<<button;
		else Web_MouseState &= ~(1<<button);
		if (WEB_WindowFlags & ZL_WINDOW_POINTERLOCK) { x = WEB_Touches[touchIdx].lastx; y = WEB_Touches[touchIdx].lasty; }
	}
	ZL_Event e;
	if (is_down)
	{
		e.type = ZL_EVENT_MOUSEBUTTONDOWN;
		WEB_Touches[touchIdx] = { x, y, touchId };
	}
	else
	{
		e.type = ZL_EVENT_MOUSEBUTTONUP;
		WEB_Touches[touchIdx].touchId = 0;
	}
	e.button.which = touchIdx;
	e.button.button = (button + 1);
	e.button.is_down = is_down;
	e.button.x = x;
	e.button.y = y;
	ZL_Display_Process_Event(e);
}

extern "C" void ZLFNWheel(int deltay)
{
	ZL_Event e;
	e.type = ZL_EVENT_MOUSEWHEEL;
	e.wheel.which = 0;
	e.wheel.x = 0;
	e.wheel.y = deltay;
	ZL_Display_Process_Event(e);
}

extern "C" void ZLFNWindow(int event_type, int canvas_width, int canvas_height)
{
	switch (event_type)
	{
		case 1: //mousein
			if ((WEB_WindowFlags & ZL_WINDOW_MOUSE_FOCUS) == 0)
				ZL_WindowEvent(ZL_WINDOWEVENT_FOCUS, (WEB_WindowFlags |= ZL_WINDOW_MOUSE_FOCUS));
			break;
		case 2: //mouseout
			if (WEB_WindowFlags & ZL_WINDOW_MOUSE_FOCUS)
				ZL_WindowEvent(ZL_WINDOWEVENT_FOCUS, (WEB_WindowFlags &= ~ZL_WINDOW_MOUSE_FOCUS));
			break;
		case 3: //window focus
			if (((WEB_WindowFlags & ZL_WINDOW_INPUT_FOCUS) == 0) || (WEB_WindowFlags & ZL_WINDOW_MINIMIZED))
				ZL_WindowEvent(ZL_WINDOWEVENT_FOCUS, (WEB_WindowFlags = (WEB_WindowFlags | ZL_WINDOW_INPUT_FOCUS) & ~ZL_WINDOW_MINIMIZED));
			break;
		case 4: //window blur
			if ((WEB_WindowFlags & ZL_WINDOW_INPUT_FOCUS) || ((WEB_WindowFlags & ZL_WINDOW_MINIMIZED) == 0))
				ZL_WindowEvent(ZL_WINDOWEVENT_FOCUS, (WEB_WindowFlags = (WEB_WindowFlags & ~ZL_WINDOW_INPUT_FOCUS) | ZL_WINDOW_MINIMIZED));
			break;
		case 5: //fullscreen activated
			WEB_WindowFlags |= ZL_WINDOW_FULLSCREEN;
			ZL_WindowEvent(ZL_WINDOWEVENT_RESIZED, canvas_width, canvas_height);
			break;
		case 6: //fullscreen deactivated
			WEB_WindowFlags &= ~ZL_WINDOW_FULLSCREEN;
			ZL_WindowEvent(ZL_WINDOWEVENT_RESIZED, canvas_width, canvas_height);
			break;
		case 7: //pointerlock activated
			WEB_WindowFlags |= ZL_WINDOW_POINTERLOCK;
			break;
		case 8: //pointerlock deactivated
			WEB_WindowFlags &= ~ZL_WINDOW_POINTERLOCK;
			break;
	}
}

extern "C" int main(int argc, char *argv[])
{
	#if defined(__wasm__)
	ZL_File::DefaultReadFileContainer = ZL_FileContainer_ZIP("DATA");
	#endif
	ZillaLibInit(0, NULL);
	return 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void ZL_WindowEvent(unsigned char event, int data1, int data2)
{
	ZL_Event e; e.type = ZL_EVENT_WINDOW;
	e.window.event = event; e.window.data1 = data1; e.window.data2 = data2;
	ZL_Display_Process_Event(e);
}

void ZL_StartTicks()
{
}

ticks_t ZL_GetTicks()
{
	return ZLJS_GetTime();
}

void ZL_Delay(ticks_t ms)
{
	for (ticks_t until = ZL_GetTicks() + ms; until > ZL_GetTicks();) {}
}

// JOYSTICK
int ZL_NumJoysticks() { return 0; }
ZL_JoystickData* ZL_JoystickHandleOpen(int index) { return NULL; }
void ZL_JoystickHandleClose(ZL_JoystickData* joystick) { }

// AUDIO
extern "C" bool ZLFNAudio(float* sample_buffer, unsigned int samples)
{
	if (!ZL_PlatformAudioMix((short*)sample_buffer, samples<<2)) return false; //2 channels, 2 byte per short sized sample

	//fill right channel samples forward because there is nothing being overwritten
	const short *psr = ((short*)sample_buffer)+1;
	for (float *pfr = sample_buffer + samples, *pfrEnd = pfr + samples; pfr != pfrEnd; psr += 2)
		*(pfr++) = *psr / 32768.f;

	//fill left channel samples backward since shorts are getting overwritten with the floats
	const short *psl = ((short*)sample_buffer) + (samples<<1) - 2;
	for (float *pfl = sample_buffer + samples - 1; pfl >= sample_buffer; psl -= 2)
		*(pfl--) = *psl / 32768.f;

	return true;
}

bool ZL_AudioOpen(unsigned int /*buffer_length*/)
{
	static bool done; if (done) return true; done = true; // cannot restart on web
	ZL_LOG0("AUDIO", "Starting audio");
	ZLJS_StartAudio();
	return true;
}

// MISC
void ZL_OpenExternalUrl(const char* url)
{
	ZLJS_OpenExternalUrl(url);
}

// SETTINGS
void ZL_SettingsInit(const char* FallbackConfigFilePrefix)
{
	ZLJS_SettingsInit(FallbackConfigFilePrefix);
}

const ZL_String ZL_SettingsGet(const char* Key)
{
	char* buf = ZLJS_SettingsGetMalloc(Key);
	if (!buf) return ZL_String::EmptyString;
	ZL_String ret(buf);
	free(buf);
	return ret;
}

void ZL_SettingsSet(const char* Key, const char* Value)
{
	ZLJS_SettingsSet(Key, Value);
}

void ZL_SettingsDel(const char* Key)
{
	ZLJS_SettingsDel(Key);
}

bool ZL_SettingsHas(const char* Key)
{
	return ZLJS_SettingsHas(Key);
}

void ZL_SettingsSynchronize() { }

#ifdef ZL_DOUBLE_PRECISCION
#error The WebGL wrapper for memory buffers to GPU buffers only works with float preciscion
#endif

// WINDOW
bool ZL_CreateWindow(const char*, int w, int h, int displayflags)
{
	float tpf_limit = ZL_TPF_Limit;
	ZL_TPF_Limit = 0;
	ZLJS_CreateWindow(w, h, tpf_limit);

	glClearColor(0, 0, 0, 1.0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

	//WindowFlags
	pZL_WindowFlags = &WEB_WindowFlags;
	return true;
}

void ZL_GetWindowSize(int *w, int *h)
{
	*w = ZLJS_GetWidth();
	*h = ZLJS_GetHeight();
}

void ZL_SetFullscreen(bool toFullscreen)
{
	ZLJS_SetFullscreen((int)toFullscreen);
}

void ZL_SetPointerLock(bool doLockPointer)
{
	ZLJS_SetPointerLock((int)doLockPointer);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////

ZL_HTTPCONNECTION_IMPL_INTERFACE
ZL_HttpConnection_Impl::ZL_HttpConnection_Impl() : timeout_msec(10000), dostream(false) { }
extern "C" void ZLFNHTTP(ZL_HttpConnection_Impl* impl, int status, char* data, size_t length)
{
	if (impl->GetRefCount() == 1) { impl->DelRef(); return; }
	if (impl->sigReceivedString.HasConnections()) impl->sigReceivedString.call(status, (length ? ZL_String(data, length) : ZL_String::EmptyString));
	if (impl->sigReceivedData.HasConnections()) impl->sigReceivedData.call(status, data, length);
	if (impl->dostream && length) { impl->dostream = 0; ZLFNHTTP(impl, status, NULL, 0); } //send a 0 byte length stream termination packet
	else impl->DelRef();
}
void ZL_HttpConnection_Impl::Connect(const char* url)
{
	//ZL_LOG2("HTTP", "Loading URL: %s (Post data: %d bytes)", url.c_str(), post_data.size());
	if (dostream) { ZL_LOG0("HTTP", "WARNING: Unsupported option dostream is activated for this http request. It will be received as one big packet with an additional terminating zero length packet."); }
	AddRef();
	ZLJS_AsyncLoad(url, this, &post_data[0], post_data.size(), timeout_msec);
}

ZL_WEBSOCKETCLIENT_IMPL_INTERFACE
ZL_WebSocketClient_Impl::ZL_WebSocketClient_Impl() : websocket_active(false), started(false) { }
extern "C" void ZLFNWebSocket(ZL_WebSocketClient_Impl* impl, int status, char* data, size_t length)
{
	if (!impl->started || impl->GetRefCount() == 1) return;
	if (!impl->websocket_active && status < 3) { impl->websocket_active = true; impl->sigConnected.call(); }
	if      (status == 1) { impl->sigReceivedText.call(ZL_String(data, length)); }
	else if (status == 2) { impl->sigReceivedBinary.call(data, length); }
	if      (status >= 3 && impl->started) { impl->websocket_active = false; impl->sigDisconnected.call((unsigned short)(status - 3)); }
}
void ZL_WebSocketClient_Impl::Connect(const char* url) { Disconnect(1001, 0, 0); AddRef(); started = true; ZLJS_Websocket(this, 0, url); }
void ZL_WebSocketClient_Impl::Send(const void* buf, size_t len, bool is_text) { if (started) ZLJS_Websocket(this, (is_text ? 1 : 2), buf, len); }
void ZL_WebSocketClient_Impl::Disconnect(unsigned short code, const char* buf, size_t len) { if (started) { websocket_active = started = false; ZLJS_Websocket(this, 3+code, buf, len); DelRef(); } }

#endif //defined(__wasm__) || defined(__EMSCRIPTEN__)
