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

#include "ZL_Platform.h"
#ifdef ZL_USE_SDL

#include "ZL_Application.h"
#include "ZL_String.h"
#include "ZL_Data.h"
#include "ZL_Display_Impl.h"
#include "ZL_Display.h"
#include "ZL_Impl.h"
#include "ZL_Thread.h"

#if (defined(_MSC_VER) && !defined(WINAPI_FAMILY))
#pragma comment (lib, "advapi32.lib")
#pragma comment (lib, "Imm32.lib")
#pragma comment (lib, "Ole32.lib")
#pragma comment (lib, "Oleaut32.lib")
#endif

#include <SDL_config.h>
#ifndef SDL_VIDEO_OPENGL
#error SDL is only usable with OpenGL
#endif
#include <SDL_timer.h>
#include <SDL_events.h>
#include <SDL_audio.h>
#include <SDL_joystick.h>
#include <SDL_endian.h>
#if SDL_BYTEORDER == SDL_BIG_ENDIAN && !defined(ZL_USE_BIGENDIAN)
#error This platform should define ZL_USE_BIGENDIAN
#endif

extern "C" {
#ifdef ZL_USE_EXTERNAL_SDL
#include <sdl.h>
#define SDL_internal_h_
#include <../src/video/SDL_sysvideo.h>
#else
#include "sdl/video/SDL_sysvideo.h"
#include "sdl/joystick/SDL_sysjoystick.h"
void SDL_ResetMouse(void);
#endif
}

//for ZL_OpenExternalUrl
#ifdef __WIN32__
#include <shellapi.h>
#else
#include <unistd.h>
#endif

#ifdef __MACOSX__
#include "CoreFoundation/CoreFoundation.h"
#endif

static void InitExtensionEntries();
static void ProcessSDLEvents();

static SDL_Window* ZL_SDL_Window = NULL;
static ZL_String jsonConfigFile;
static ZL_Json jsonConfig;
static SDL_FingerID ZL_SDL_FingerIDs[32];
static signed char ZL_SDL_FingerCount, ZL_SDL_Mouse_IgnoreMotion, ZL_SDL_Mouse_IgnoreButton;

void ZL_SettingsInit(const char* FallbackConfigFilePrefix)
{
	jsonConfigFile.erase();
	jsonConfigFile << FallbackConfigFilePrefix << ".cfg";
	if (ZL_File::Exists(jsonConfigFile)) jsonConfig = ZL_Json(ZL_File(jsonConfigFile));
}

const ZL_String ZL_SettingsGet(const char* Key)
{
	ZL_Json field = jsonConfig.GetByKey(Key);
	return (!field ? ZL_String() : ZL_String(field.GetString()));
}

void ZL_SettingsSet(const char* Key, const char* Value)
{
	jsonConfig[Key].SetString(Value);
}

void ZL_SettingsDel(const char* Key)
{
	jsonConfig.Erase(Key);
}

bool ZL_SettingsHas(const char* Key)
{
	return jsonConfig.HasKey(Key);
}

void ZL_SettingsSynchronize()
{
	ZL_File(jsonConfigFile, "w").SetContents(jsonConfig);
}

void ZL_OpenExternalUrl(const char* url)
{
	#ifdef __WIN32__
	ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
	#else
	int ret = system(ZL_String("xdg-open ")<<url<<"||gnome-open "<<url<<"||exo-open "<<url<<"||open "<<url<<" &");(void)ret;
	#endif
}

static void ZL_SDL_ShowError(const char* msg)
{
	#ifdef __WIN32__
	MessageBoxA(NULL, msg, "Error", MB_ICONSTOP);
	#else
	fprintf(stderr, "%s\n", msg);
	#endif
}

#if defined(NDEBUG)
static char *argv0;
bool ZL_LoadReleaseDesktopDataBundle(const char* DataBundleFileName)
{
	if ((ZL_File::DefaultReadFileContainer = ZL_FileContainer_ZIP(DataBundleFileName ? DataBundleFileName : argv0))) return true;
	if (argv0 && DataBundleFileName)
	{
		char *dirsepf = strrchr(argv0, '/'), *dirsepb = strrchr(argv0, '\\');
		size_t szp = (!dirsepb && !dirsepf ? 0 : (dirsepb && (!dirsepf || dirsepb < dirsepf) ? dirsepb : dirsepf) - argv0), szf = strlen(DataBundleFileName);
		if (szp && szp + szf < 510)
		{
			char buf[512];
			memcpy(buf, argv0, szp);
			buf[szp] = *(dirsepb && (!dirsepf || dirsepb < dirsepf) ? dirsepb : dirsepf);
			memcpy(buf+szp+1, DataBundleFileName, szf);
			buf[szp+1+szf] = 0;
			if ((ZL_File::DefaultReadFileContainer = ZL_FileContainer_ZIP(buf))) return true;
		}
	}
	#ifdef __WIN32__
	MessageBoxA(NULL, "Could not load data file", "Load Error", MB_ICONSTOP);
	#else
	fprintf(stderr, "Error: Could not load data file\n");
	#endif
	return false;
}
#endif

#if defined(__WIN32__)
extern "C" { extern LPCWSTR SDL_Appname; extern HINSTANCE SDL_Instance; }
#endif

int main(int argc, char *argv[])
{
	#if defined(NDEBUG)
		argv0 = (argc ? argv[0] : NULL);
	#endif
	#if defined(__MACOSX__)
		static char path[PATH_MAX];
		CFBundleRef mainBundle = CFBundleGetMainBundle();
		CFURLRef resourcesURL = CFBundleCopyResourcesDirectoryURL(mainBundle);
		if (CFURLGetFileSystemRepresentation(resourcesURL, TRUE, (UInt8 *)path, PATH_MAX)) chdir(path);
		CFRelease(resourcesURL);
		#if defined(NDEBUG)
		resourcesURL = CFBundleCopyExecutableURL(mainBundle);
		if (CFURLGetFileSystemRepresentation(resourcesURL, TRUE, (UInt8 *)path, PATH_MAX)) argv0 = path;
		CFRelease(resourcesURL);
		#endif
	#endif
	#if defined(__WIN32__)
		//set this for icon resource identifier
		SDL_Appname = L"ZL";
		SDL_Instance = GetModuleHandle(NULL);
	#endif
	SDL_EventState(SDL_SYSWMEVENT, SDL_DISABLE);
	ZillaLibInit(argc, argv);
	ZL_Application::sigKeepAlive.connect(&ProcessSDLEvents); // called in frame loop after _ZL_ApplicationUpdateTimingFps
	while (!(ZL_MainApplicationFlags & ZL_APPLICATION_DONE))
	{
		ZL_MainApplication->Frame();
		SDL_GL_SwapWindow(ZL_SDL_Window);
		//#ifdef __WIN32__
		//if ((ZL_MainApplicationFlags & (ZL_APPLICATION_HASVSYNC|ZL_APPLICATION_VSYNCFAILED|ZL_APPLICATION_VSYNCHACK)) == (ZL_APPLICATION_HASVSYNC|ZL_APPLICATION_VSYNCFAILED|ZL_APPLICATION_VSYNCHACK))
		//{
		//	//hack to fix vsync issue on ATI cards on windows (Maybe other platforms/cards affected?)
		//	//see http://www.gamedev.net/topic/544239-jerky-movement-when-vsync-turned-on-wglswapintervalext/page-2
		//	//see https://github.com/LaurentGomila/SFML/issues/320
		//	if (ZL_Application::FrameCount>>2) glFinish();
		//}
		//#endif
		//avoid any "input lag" (see: http://www.opengl.org/wiki/Swap_Interval#GPU_vs_CPU_synchronization)
		if (ZL_MainApplicationFlags & ZL_APPLICATION_HASVSYNC) glFinish();
	}
	ZL_MainApplication->OnQuit();
	//_exit(ZL_DoneReturn); //faster exit without freeing each object, let OS free all memory and resources
	return ZL_DoneReturn;
}

#if defined(__WIN32__) && defined(WINAPI)
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd)
{
	return main(__argc, __argv); //(lpCmdLine[0] ? 1 : 0), &lpCmdLine);
}
#endif

#if defined(__MACOSX__)
static scalar ZL_SDL_DPIScale;
static void ZL_SDL_CalcDPIScale()
{
	int ww, wh, glw, glh;
	SDL_GL_GetDrawableSize(ZL_SDL_Window, &glw, &glh); 
	SDL_GetWindowSize(ZL_SDL_Window, &ww, &wh);
	ZL_SDL_DPIScale = s(glw) / ww;
}

#define ZLDPISCALE(f) ((f)*ZL_SDL_DPIScale)
#define ZLDPISCALEINT(i) ((int)((i)*ZL_SDL_DPIScale+.4999))
#else
#define ZL_SDL_CalcDPIScale()
#define ZLDPISCALE(f) (f)
#define ZLDPISCALEINT(i) (i)
#endif

//application
ticks_t ZL_GetTicks()
{
	return SDL_GetTicks();
}

void ZL_Delay(ticks_t ms)
{
	SDL_Delay(ms);
}

void ZL_StartTicks()
{
}

//Joystick
bool ZL_InitJoystickSubSystem()
{
	#ifndef ZL_USE_EXTERNAL_SDL
	return (SDL_JoystickInit() ? true : false);
	#else
	return (SDL_Init(SDL_INIT_JOYSTICK) ? true : false);
	#endif
}

int ZL_NumJoysticks()
{
	return SDL_NumJoysticks();
}

ZL_JoystickData* ZL_JoystickHandleOpen(int index)
{
	#if (defined(_MSC_VER) && !defined(WINAPI_FAMILY))
	//if compiler errors here, SDL_Joystick struct definition does not match ZL_JoystickData
	enum { __asrt = 1/(int)( (void*)&((ZL_JoystickData*)0)->buttons == (void*)&((SDL_Joystick*)0)->buttons ) };
	#endif
	SDL_Joystick* joy = SDL_JoystickOpen(index);
	return (ZL_JoystickData*)joy;
}

void ZL_JoystickHandleClose(ZL_JoystickData* joystick)
{
	SDL_JoystickClose((SDL_Joystick*)joystick);
}

bool ZL_CreateWindow(const char* windowtitle, int width, int height, int displayflags)
{
	#if (defined(_MSC_VER) && !defined(WINAPI_FAMILY))
	//if compiler errors, enum ZL_WindowFlags does not map to enum SDL_WindowFlags
	enum { __asrt = 1/(int)(ZL_WINDOW_FULLSCREEN==SDL_WINDOW_FULLSCREEN&&ZL_WINDOW_MINIMIZED==SDL_WINDOW_MINIMIZED&&ZL_WINDOW_RESIZABLE==SDL_WINDOW_RESIZABLE) };
	#endif

	Uint32 windowflags = SDL_WINDOW_OPENGL;
	if (displayflags & ZL_DISPLAY_FULLSCREEN) windowflags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
	if (displayflags & ZL_DISPLAY_MAXIMIZED) windowflags |= SDL_WINDOW_MAXIMIZED;
	if (displayflags & ZL_DISPLAY_RESIZABLE) windowflags |= SDL_WINDOW_RESIZABLE;

	if (SDL_VideoInit(NULL) < 0) { ZL_SDL_ShowError("Could not initialize video display"); return false; }

	//limit window size to desktop resolution of primary display (if data can be acquired via SDL)
	SDL_DisplayMode DesktopMode = { 0, 0, 0, 0, 0 };
	SDL_GetDesktopDisplayMode(0, &DesktopMode);
	if (DesktopMode.w)
	{
		if (DesktopMode.w > 500 && width > DesktopMode.w) width = DesktopMode.w;
		if (DesktopMode.h > 400 && height > DesktopMode.h) height = DesktopMode.h;
	}

	SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);

	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, (displayflags & ZL_DISPLAY_DEPTHBUFFER ? 16 : 0));
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

	// should enable 4x AA if possible
	SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
	SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);

	#ifdef ZL_VIDEO_OPENGL_CORE
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
	#elif defined(ZL_VIDEO_OPENGL_ES1) || defined(ZL_VIDEO_OPENGL_ES2)
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	#endif

	#if defined(__WIN32__) && defined(ZILLALIB_TRANSPARENTONWINDOWSDESKTOP)
	windowflags |= SDL_WINDOW_BORDERLESS;
	#endif

	#if defined(__MACOSX__) // see SDL_cocoawindow.m
	windowflags |= SDL_WINDOW_ALLOW_HIGHDPI;
	#endif

	ZL_SDL_Window = SDL_CreateWindow(windowtitle, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, windowflags);
	if (!ZL_SDL_Window)
	{
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
		ZL_SDL_Window = SDL_CreateWindow(windowtitle, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, windowflags);
		if (!ZL_SDL_Window) { ZL_SDL_ShowError("Could not create OpenGL window"); return false; }
	}

	#if defined(__WIN32__) && defined(ZILLALIB_TRANSPARENTONWINDOWSDESKTOP)
	HMODULE libDwm = LoadLibraryA("Dwmapi.dll");
	if (libDwm)
	{
		typedef struct { DWORD dwFlags; BOOL fEnable; HRGN hRgnBlur; BOOL fTransitionOnMaximized; } DWM_BLURBEHIND;
		BOOL(__stdcall *procDwmEnableBlurBehindWindow)(HWND, const DWM_BLURBEHIND*)  = (BOOL(__stdcall *)(HWND, const DWM_BLURBEHIND*)) GetProcAddress(libDwm, "DwmEnableBlurBehindWindow");
		if (procDwmEnableBlurBehindWindow)
		{
			DWM_BLURBEHIND bb = {0};
			bb.dwFlags = 3;
			bb.fEnable = TRUE;
			bb.fTransitionOnMaximized = 1;
			bb.hRgnBlur = CreateRectRgn(0, 0, -1, -1);
			procDwmEnableBlurBehindWindow(*(((HWND*)(ZL_SDL_Window->driverdata))+1), &bb);
		}
		FreeLibrary(libDwm);
	}
	#endif

	if (!SDL_GL_CreateContext(ZL_SDL_Window)) { ZL_SDL_ShowError("Could not initialize OpenGL context"); return false; }

	ZL_SDL_CalcDPIScale();

	glClear(GL_COLOR_BUFFER_BIT);
	SDL_GL_SwapWindow(ZL_SDL_Window);

	if (ZL_Requested_FPS < 0)
		ZL_MainApplication->SetFpsLimit(DesktopMode.refresh_rate > 0 ? (float)DesktopMode.refresh_rate : 60.0f);
	else
		ZL_UpdateTPFLimit();

	//const GLubyte* pstrGlVendor = glGetString(GL_VENDOR);
	//if (pstrGlVendor[0] == 'A' && pstrGlVendor[1] == 'T' && pstrGlVendor[2] == 'I') ZL_MainApplicationFlags |= ZL_APPLICATION_VSYNCHACK;

	//printf("Vendor: %s\n", glGetString(GL_VENDOR));
	//printf("Renderer: %s\n", glGetString(GL_RENDERER));
	//printf("Version: %s\n", glGetString(GL_VERSION));
	//printf("GLSL: %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
	//printf("Extensions : %s\n", glGetString(GL_EXTENSIONS));
	//printf("\n");

	InitExtensionEntries();

	/*
#ifndef __MACOSX__
	printf("glDeleteBuffers =           %d\n", (glDeleteBuffers       ? 1 : 0));
	printf("glCreateShader =            %d\n", (glCreateShader        ? 1 : 0));
	printf("glCompileShader =           %d\n", (glCompileShader       ? 1 : 0));
	printf("glCreateProgram =           %d\n", (glCreateProgram       ? 1 : 0));
	printf("glLinkProgram =             %d\n", (glLinkProgram         ? 1 : 0));
	printf("glUseProgram =              %d\n", (glUseProgram          ? 1 : 0));
	printf("glBindAttribLocation =      %d\n", (glBindAttribLocation  ? 1 : 0));
	printf("glVertexAttribPointer =     %d\n", (glVertexAttribPointer ? 1 : 0));
	printf("glVertexAttrib4fv =         %d\n", (glVertexAttrib4fv     ? 1 : 0));
	printf("glUniform1f =               %d\n", (glUniform1f           ? 1 : 0));
	printf("glIsProgram =               %d\n", (glIsProgram           ? 1 : 0));
#if !defined(ZL_DOUBLE_PRECISCION)
	printf("glUniformMatrix4fv =        %d\n", (glUniformMatrix4fv    ? 1 : 0));
#else
#endif
#ifndef GL_ATI_blend_equation_separate
	printf("glBlendEquation =           %d\n", (glBlendEquation       ? 1 : 0));
#endif
#endif
	printf("glGenFramebuffers =         %d\n", (glGenFramebuffers     ? 1 : 0));
	printf("glDeleteFramebuffers =      %d\n", (glDeleteFramebuffers  ? 1 : 0));
	printf("glBindFramebuffer =         %d\n", (glBindFramebuffer     ? 1 : 0));
	printf("glFramebufferTexture2D =    %d\n", (glFramebufferTexture2D? 1 : 0));
	printf("\n");
	*/

	pZL_WindowFlags = (unsigned int*)&ZL_SDL_Window->flags;

	return true;
}

void ZL_UpdateTPFLimit()
{
	SDL_DisplayMode DisplayMode = { 0, 0, 0, 0, 0 };
	SDL_GetCurrentDisplayMode(0, &DisplayMode);
	//if (!DisplayMode.refresh_rate) DisplayMode.refresh_rate = 60; // at least matches on VMWare running Linux
	float RRate = (float)DisplayMode.refresh_rate, TPFRate = ((ZL_TPF_Limit && RRate) ? ZL_TPF_Limit / (1000.0f / RRate) : 0); //, TPFRateFrac = (TPFRate - (int)TPFRate);
	bool UseVSync = (TPFRate && TPFRate <= 1.01f && TPFRate >= 0.99f), setSwapSucceeded = (SDL_GL_SetSwapInterval(UseVSync ? 1 : 0) == 0);
	//ZL_LOG4("VSYNC", "Refresh Rate: %d - Want VSync: %d - Setting VSync Succeeded: %d - Actual Swap Interval: %d", DisplayMode.refresh_rate, (int)UseVSync, (int)setSwapSucceeded, (int)SDL_GL_GetSwapInterval());
	UseVSync = (setSwapSucceeded && UseVSync);
	ZL_MainApplicationFlags = (UseVSync ? ZL_MainApplicationFlags | ZL_APPLICATION_HASVSYNC : ZL_MainApplicationFlags & ~ZL_APPLICATION_HASVSYNC);
	if (UseVSync) ZL_Requested_FPS = (float)DisplayMode.refresh_rate;
}

static unsigned char ZL_SDL_FingerIDGetIndex(SDL_FingerID FingerID, unsigned char MinIndex = 0)
{
	for (; MinIndex < (unsigned char)COUNT_OF(ZL_SDL_FingerIDs); MinIndex++)
		if (ZL_SDL_FingerIDs[MinIndex] == FingerID) return MinIndex;
	return 0xFF;
}

static void ProcessSDLEvents()
{
	SDL_Event in;
	while (SDL_PollEvent(&in))
	{
		ZL_Event out;
		switch (in.type)
		{
			case SDL_MOUSEMOTION:
				if (ZL_SDL_Mouse_IgnoreMotion) { ZL_SDL_Mouse_IgnoreMotion--; continue; }
				out.type = ZL_EVENT_MOUSEMOTION;
				out.motion.which = 0;
				out.motion.state = in.motion.state;
				out.motion.x = ZLDPISCALE((scalar)in.motion.x);
				out.motion.y = ZLDPISCALE((scalar)in.motion.y);
				out.motion.xrel = ZLDPISCALE((scalar)in.motion.xrel);
				out.motion.yrel = ZLDPISCALE((scalar)in.motion.yrel);
				break;
			case SDL_MOUSEBUTTONDOWN:
			case SDL_MOUSEBUTTONUP:
				if (ZL_SDL_Mouse_IgnoreButton) { ZL_SDL_Mouse_IgnoreButton--; continue; }
				out.type = (in.type == SDL_MOUSEBUTTONDOWN ? ZL_EVENT_MOUSEBUTTONDOWN : ZL_EVENT_MOUSEBUTTONUP);
				out.button.which = 0;
				out.button.button = in.button.button;
				out.button.is_down = (in.button.state == SDL_PRESSED);
				out.button.x = ZLDPISCALE((scalar)in.button.x);
				out.button.y = ZLDPISCALE((scalar)in.button.y);
				break;
			case SDL_MOUSEWHEEL:
				out.type = ZL_EVENT_MOUSEWHEEL;
				out.wheel.which = 0;
				out.wheel.x = (scalar)(in.wheel.x*120);
				out.wheel.y = (scalar)(in.wheel.y*120);
				break;
			case SDL_FINGERMOTION:
				if ((out.motion.which = ZL_SDL_FingerIDGetIndex(in.tfinger.fingerId)) == 0xFF) continue;
				if (out.motion.which == 0) ZL_SDL_Mouse_IgnoreMotion = 1;
				out.type = ZL_EVENT_MOUSEMOTION;
				out.motion.state = 1;
				out.motion.x = ZLDPISCALE((scalar)in.tfinger.x * (scalar)ZL_SDL_Window->w);
				out.motion.y = ZLDPISCALE((scalar)in.tfinger.y * (scalar)ZL_SDL_Window->h);
				out.motion.xrel = ZLDPISCALE((scalar)in.tfinger.dx * (scalar)ZL_SDL_Window->w);
				out.motion.yrel = ZLDPISCALE((scalar)in.tfinger.dy * (scalar)ZL_SDL_Window->h);
				break;
			case SDL_FINGERDOWN:
			case SDL_FINGERUP:
				out.button.is_down = (in.type == SDL_FINGERDOWN);
				if ((out.motion.which = ZL_SDL_FingerIDGetIndex(out.button.is_down ? 0 : in.tfinger.fingerId)) == 0xFF) continue;
				if (out.button.is_down)
				{
					if (out.motion.which == 0)
					{
						if (!ZL_SDL_FingerCount) ZL_SDL_Mouse_IgnoreButton = 2;
						else if ((out.motion.which = ZL_SDL_FingerIDGetIndex(0, 1)) == 0xFF) continue;
					}
					ZL_SDL_FingerIDs[out.motion.which] = in.tfinger.fingerId;
					ZL_SDL_FingerCount++;
					out.type = ZL_EVENT_MOUSEBUTTONDOWN;
				}
				else
				{
					if (out.motion.which == 0) ZL_SDL_Mouse_IgnoreMotion = 0;
					ZL_SDL_FingerIDs[out.motion.which] = 0;
					ZL_SDL_FingerCount--;
					out.type = ZL_EVENT_MOUSEBUTTONUP;
				}
				out.button.button = ZL_BUTTON_LEFT;
				out.button.x = ZLDPISCALE((scalar)in.tfinger.x * (scalar)ZL_SDL_Window->w);
				out.button.y = ZLDPISCALE((scalar)in.tfinger.y * (scalar)ZL_SDL_Window->h);
				break;
			case SDL_KEYDOWN:
			case SDL_KEYUP:
				out.type = (in.type == SDL_KEYDOWN ? ZL_EVENT_KEYDOWN : ZL_EVENT_KEYUP);
				out.key.is_down = (in.key.state == SDL_PRESSED);
				out.key.key = (ZL_Key)in.key.keysym.scancode;
				out.key.mod = in.key.keysym.mod;
				if (in.type == SDL_KEYDOWN && (out.key.key == ZLK_TAB || out.key.key == ZLK_RETURN || out.key.key == ZLK_KP_ENTER || out.key.key == ZLK_BACKSPACE))
				{
					in.type = SDL_TEXTINPUT;
					in.text.text[0] = (out.key.key == ZLK_RETURN || out.key.key == ZLK_KP_ENTER ? '\n' : (out.key.key == ZLK_BACKSPACE ? '\b' : '\t'));
					in.text.text[1] = 0;
					SDL_PushEvent(&in);
				}
				break;
			case SDL_TEXTINPUT:
				out.type = ZL_EVENT_TEXTINPUT;
				#if (defined(_MSC_VER) && !defined(WINAPI_FAMILY))
				//if compiler errors here, SDL text event size does not match ZL text event size
				enum { __asrt = 1/(int)(sizeof(out.text.text) == sizeof(in.text.text)) };
				#endif
				if (in.text.text[1]) memcpy(out.text.text, in.text.text, sizeof(out.text.text));
				else { out.text.text[0] = (in.text.text[0] == '\r' ? '\n' : in.text.text[0]) ; out.text.text[1] = 0; }
				break;
			case SDL_JOYAXISMOTION:
				out.type = ZL_EVENT_JOYAXISMOTION;
				out.jaxis.which = in.jaxis.which;
				out.jaxis.axis = in.jaxis.axis;
				out.jaxis.value = in.jaxis.value;
				break;
			case SDL_JOYBALLMOTION:
				out.type = ZL_EVENT_JOYBALLMOTION;
				out.jball.which = in.jball.which;
				out.jball.ball = in.jball.ball;
				out.jball.xrel = in.jball.xrel;
				out.jball.yrel = in.jball.yrel;
				break;
			case SDL_JOYHATMOTION:
				out.type = ZL_EVENT_JOYHATMOTION;
				out.jhat.which = in.jhat.which;
				out.jhat.hat = in.jhat.hat;
				out.jhat.value = in.jhat.value;
				break;
			case SDL_JOYBUTTONDOWN:
			case SDL_JOYBUTTONUP:
				out.type = (in.type == SDL_JOYBUTTONDOWN ? ZL_EVENT_JOYBUTTONDOWN : ZL_EVENT_JOYBUTTONUP);
				out.jbutton.which = in.jbutton.which;
				out.jbutton.button = in.jbutton.button;
				out.jbutton.is_down = (in.jbutton.state == SDL_PRESSED);
				break;
			case SDL_QUIT:
				out.type = ZL_EVENT_QUIT;
				break;
			case SDL_WINDOWEVENT:
				if (in.window.event == SDL_WINDOWEVENT_EXPOSED) continue;
				#if defined(__WIN32__) && defined(WM_MOUSELEAVE) && defined(TME_LEAVE)
				if (in.window.event == SDL_WINDOWEVENT_ENTER) //fix for SDL2, might not work with MINGW...
				{ TRACKMOUSEEVENT t = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, *(((HWND*)(ZL_SDL_Window->driverdata))+1), 0 }; TrackMouseEvent(&t); }
				#endif
				out.type = ZL_EVENT_WINDOW;
				if (in.window.event == SDL_WINDOWEVENT_MOVED)          out.window.event = ZL_WINDOWEVENT_MOVED;
				else if (in.window.event == SDL_WINDOWEVENT_CLOSE)     out.window.event = ZL_WINDOWEVENT_CLOSE;
				else if (in.window.event == SDL_WINDOWEVENT_MINIMIZED) out.window.event = ZL_WINDOWEVENT_MINIMIZED;
				else if (in.window.event == SDL_WINDOWEVENT_MAXIMIZED) out.window.event = ZL_WINDOWEVENT_MAXIMIZED;
				else if (in.window.event == SDL_WINDOWEVENT_RESTORED)  out.window.event = ZL_WINDOWEVENT_RESTORED;
				else if (in.window.event == SDL_WINDOWEVENT_MOVED)     out.window.event = ZL_WINDOWEVENT_MOVED;
				else if (in.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
				{
					ZL_SDL_CalcDPIScale();
					out.window.event = ZL_WINDOWEVENT_RESIZED;
					out.window.data1 = ZLDPISCALEINT(in.window.data1);
					out.window.data2 = ZLDPISCALEINT(in.window.data2);
				}
				else
				{
					out.window.event = ZL_WINDOWEVENT_FOCUS;
					//linux SDL loses input focus when switching to fullscreen
					if (ZL_SDL_Window->flags & SDL_WINDOW_FULLSCREEN) ZL_SDL_Window->flags |= SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS;
				}
				#ifndef ZL_USE_EXTERNAL_SDL
				if ((out.window.event == ZL_WINDOWEVENT_MINIMIZED || out.window.event == ZL_WINDOWEVENT_FOCUS) && !ZL_WINDOWFLAGS_HAS(ZL_WINDOW_INPUT_FOCUS)) SDL_ResetMouse();
				#endif
				break;
			case SDL_DROPFILE:
				ZL_Display::sigDropFile.call(in.drop.file);
				free(in.drop.file);
				break;
			case SDL_JOYDEVICEADDED:
			case SDL_JOYDEVICEREMOVED:
				ZL_Display::sigJoyDeviceChange.call(in.type == SDL_JOYDEVICEADDED, in.jdevice.which);
				break;
			#if 0 //defined(ZILLALOG)
			case SDL_APP_TERMINATING: printf("UNUSED SDL EVENT [SDL_APP_TERMINATING]\n"); continue;
			case SDL_APP_LOWMEMORY: printf("UNUSED SDL EVENT [SDL_APP_LOWMEMORY]\n"); continue;
			case SDL_APP_WILLENTERBACKGROUND: printf("UNUSED SDL EVENT [SDL_APP_WILLENTERBACKGROUND]\n"); continue;
			case SDL_APP_DIDENTERBACKGROUND: printf("UNUSED SDL EVENT [SDL_APP_DIDENTERBACKGROUND]\n"); continue;
			case SDL_APP_WILLENTERFOREGROUND: printf("UNUSED SDL EVENT [SDL_APP_WILLENTERFOREGROUND]\n"); continue;
			case SDL_APP_DIDENTERFOREGROUND: printf("UNUSED SDL EVENT [SDL_APP_DIDENTERFOREGROUND]\n"); continue;
			case SDL_SYSWMEVENT: printf("UNUSED SDL EVENT [SDL_SYSWMEVENT]\n"); continue;
			case SDL_TEXTEDITING: printf("UNUSED SDL EVENT [SDL_TEXTEDITING]\n"); continue;
			case SDL_CONTROLLERAXISMOTION: printf("UNUSED SDL EVENT [SDL_CONTROLLERAXISMOTION]\n"); continue;
			case SDL_CONTROLLERBUTTONDOWN: printf("UNUSED SDL EVENT [SDL_CONTROLLERBUTTONDOWN]\n"); continue;
			case SDL_CONTROLLERBUTTONUP: printf("UNUSED SDL EVENT [SDL_CONTROLLERBUTTONUP]\n"); continue;
			case SDL_CONTROLLERDEVICEADDED: printf("UNUSED SDL EVENT [SDL_CONTROLLERDEVICEADDED]\n"); continue;
			case SDL_CONTROLLERDEVICEREMOVED: printf("UNUSED SDL EVENT [SDL_CONTROLLERDEVICEREMOVED]\n"); continue;
			case SDL_CONTROLLERDEVICEREMAPPED: printf("UNUSED SDL EVENT [SDL_CONTROLLERDEVICEREMAPPED]\n"); continue;
			case SDL_DOLLARGESTURE: printf("UNUSED SDL EVENT [SDL_DOLLARGESTURE]\n"); continue;
			case SDL_DOLLARRECORD: printf("UNUSED SDL EVENT [SDL_DOLLARRECORD]\n"); continue;
			case SDL_MULTIGESTURE: printf("UNUSED SDL EVENT [SDL_MULTIGESTURE]\n"); continue;
			case SDL_CLIPBOARDUPDATE: printf("UNUSED SDL EVENT [SDL_CLIPBOARDUPDATE]\n"); continue;
			case SDL_RENDER_TARGETS_RESET: printf("UNUSED SDL EVENT [SDL_RENDER_TARGETS_RESET]\n"); continue;
			case SDL_USEREVENT: printf("UNUSED SDL EVENT [SDL_USEREVENT]\n"); continue;
			#endif
			default: continue;
		}
		ZL_Display_Process_Event(out);
	}
}

void ZL_SetFullscreen(bool toFullscreen)
{
	#if 0 //#ifndef ZL_USE_EXTERNAL_SDL
	SDL_VideoDevice *device = SDL_GetVideoDevice();
	SDL_VideoDisplay *display = SDL_GetDisplayForWindow(ZL_SDL_Window);
	if (toFullscreen)
	{
		ZL_SDL_Window->flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
		ZL_SDL_Window->x = 0;
		ZL_SDL_Window->y = 0;
		ZL_SDL_Window->w = display->desktop_mode.w;
		ZL_SDL_Window->h = display->desktop_mode.h;
		device->SetWindowSize(device, ZL_SDL_Window);
	}
	else
	{
		ZL_SDL_Window->flags &= ~SDL_WINDOW_FULLSCREEN_DESKTOP;
		ZL_SDL_Window->x = ZL_SDL_Window->windowed.x;
		ZL_SDL_Window->y = ZL_SDL_Window->windowed.y;
		ZL_SDL_Window->w = ZL_SDL_Window->windowed.w;
		ZL_SDL_Window->h = ZL_SDL_Window->windowed.h;
	}
	device->SetWindowFullscreen(device, ZL_SDL_Window, display, (SDL_bool)toFullscreen);
	if(toFullscreen) SDL_OnWindowResized(ZL_SDL_Window);
	else SDL_SetWindowSize(ZL_SDL_Window, ZL_SDL_Window->windowed.w, ZL_SDL_Window->windowed.h);
	#else
	if (toFullscreen)
	{
		SDL_SetWindowFullscreen(ZL_SDL_Window, SDL_WINDOW_FULLSCREEN_DESKTOP);
	}
	else
	{
		SDL_SetWindowFullscreen(ZL_SDL_Window, 0);
		SDL_SetWindowSize(ZL_SDL_Window, ZL_SDL_Window->windowed.w, ZL_SDL_Window->windowed.h);
	}
	#endif
}

void ZL_SetPointerLock(bool doLockPointer)
{
	ZL_SDL_Window->flags = (doLockPointer ? (ZL_SDL_Window->flags|ZL_WINDOW_POINTERLOCK) : (ZL_SDL_Window->flags&~ZL_WINDOW_POINTERLOCK));
	SDL_SetRelativeMouseMode((SDL_bool)doLockPointer);
}

void ZL_GetWindowSize(int *w, int *h)
{
	SDL_GL_GetDrawableSize(ZL_SDL_Window, w, h);
}

static void ZL_SdlAudioMix(void *udata, Uint8 *stream, int len)
{
	ZL_PlatformAudioMix((short*)stream, (unsigned int)len);
}

bool ZL_AudioOpen(unsigned int buffer_length)
{
	if (SDL_GetAudioStatus() != SDL_AUDIO_STOPPED) SDL_AudioQuit();
	if (SDL_AudioInit(NULL) < 0) return false;
	ZL_LOG1("AUDIO", "Initialized audio driver: %s", SDL_GetCurrentAudioDriver());
	SDL_AudioSpec desired;
	desired.freq = 44100;
	desired.format = AUDIO_S16LSB;
	desired.channels = 2;
	desired.samples = (Uint16)buffer_length; //Used to be 4096, PCs are faster now
	desired.callback = ZL_SdlAudioMix;
	desired.userdata = NULL;
	if (SDL_OpenAudio(&desired, NULL) < 0) return false;
	SDL_PauseAudio(0);
	return true;
}

void ZL_SdlSetTitle(const char* newtitle)
{
	SDL_SetWindowTitle(ZL_SDL_Window, newtitle);
}

ZL_Vector ZL_SdlQueryMousePos()
{
	// This likely gives the same result as ZL_Display::PointerPos when called on the main thread so it is only really useful on a thread
	int x, y; 
	SDL_GetMouseState(&x, &y);
	return ZLV(ZLDPISCALE((scalar)x), ZL_Display::Height - 1 - ZLDPISCALE((scalar)y));
}

//thread
ZL_ThreadHandle ZL_CreateThread(void *(*start_routine) (void *p), void *arg)
{
	return SDL_CreateThread((SDL_ThreadFunction)start_routine, NULL, arg);
}
void ZL_WaitThread(ZL_ThreadHandle hthread, int *pstatus)
{
	SDL_WaitThread((SDL_Thread*)hthread, pstatus);
}
int ZL_MutexLock(ZL_MutexHandle mutex) { return SDL_LockMutex((SDL_mutex*)mutex); }
int ZL_MutexUnlock(ZL_MutexHandle mutex) { return SDL_UnlockMutex((SDL_mutex*)mutex); }
bool ZL_MutexTryLock(ZL_MutexHandle mutex) { return (SDL_TryLockMutex((SDL_mutex*)mutex)==0); }
ZL_MutexHandle ZL_CreateMutex() { return SDL_CreateMutex(); }
void ZL_MutexDestroy(ZL_MutexHandle mutex) { SDL_DestroyMutex((SDL_mutex*)mutex); }

struct ZL_Mutex_Impl : ZL_Impl { virtual ~ZL_Mutex_Impl() { SDL_DestroyMutex(mutex); } SDL_mutex* mutex; };
ZL_IMPL_OWNER_NONULLCON_IMPLEMENTATIONS(ZL_Mutex)
ZL_Mutex::ZL_Mutex() { (impl = new ZL_Mutex_Impl())->mutex = SDL_CreateMutex(); }
void ZL_Mutex::Lock() { SDL_LockMutex(impl->mutex); }
void ZL_Mutex::Unlock() { SDL_UnlockMutex(impl->mutex); }
bool ZL_Mutex::TryLock() { return (SDL_TryLockMutex(impl->mutex)==0); }

struct ZL_Semaphore_Impl : ZL_Impl { virtual ~ZL_Semaphore_Impl() { SDL_DestroySemaphore(sem); } SDL_sem* sem; };
ZL_IMPL_OWNER_NONULLCON_IMPLEMENTATIONS(ZL_Semaphore)
ZL_Semaphore::ZL_Semaphore() { (impl = new ZL_Semaphore_Impl())->sem = SDL_CreateSemaphore(0); }
bool ZL_Semaphore::Wait() { return (SDL_SemWait(impl->sem)==0); }
bool ZL_Semaphore::TryWait() { return (SDL_SemTryWait(impl->sem)==0); }
bool ZL_Semaphore::WaitTimeout(int ms) { return (SDL_SemWaitTimeout(impl->sem, (Uint32)ms)==0); }
void ZL_Semaphore::Post() { SDL_SemPost(impl->sem); }

//gl2 stuff
#ifndef __MACOSX__
PFNGLCREATESHADERPROC             glCreateShader;
PFNGLSHADERSOURCEPROC             glShaderSource;
PFNGLCOMPILESHADERPROC            glCompileShader;
PFNGLCREATEPROGRAMPROC            glCreateProgram;
PFNGLATTACHSHADERPROC             glAttachShader;
PFNGLDETACHSHADERPROC             glDetachShader;
PFNGLLINKPROGRAMPROC              glLinkProgram;
PFNGLUSEPROGRAMPROC               glUseProgram;
PFNGLGETSHADERIVPROC              glGetShaderiv;
PFNGLGETPROGRAMIVPROC             glGetProgramiv;
#ifdef ZILLALOG
PFNGLGETSHADERINFOLOGPROC         glGetShaderInfoLog;
PFNGLGETPROGRAMINFOLOGPROC        glGetProgramInfoLog;
#endif
PFNGLGETATTRIBLOCATIONPROC        glGetAttribLocation;
PFNGLBINDATTRIBLOCATIONPROC       glBindAttribLocation;
PFNGLVERTEXATTRIBPOINTERPROC      glVertexAttribPointer;
PFNGLENABLEVERTEXATTRIBARRAYPROC  glEnableVertexAttribArray;
PFNGLGETUNIFORMLOCATIONPROC       glGetUniformLocation;
#if !defined(ZL_DOUBLE_PRECISCION)
PFNGLUNIFORMMATRIX4FVPROC         glUniformMatrix4fv;
PFNGLVERTEXATTRIB4FVPROC          glVertexAttrib4fv;
PFNGLVERTEXATTRIB4FPROC           glVertexAttrib4f;
PFNGLUNIFORM1FPROC                glUniform1f;
#else
PFNGLUNIFORMMATRIX4DVPROC         glUniformMatrix4dv;
PFNGLVERTEXATTRIB4DVPROC          glVertexAttrib4dv;
PFNGLVERTEXATTRIB4DPROC           glVertexAttrib4d;
PFNGLUNIFORM1DPROC                glUniform1d;
#endif
PFNGLDISABLEVERTEXATTRIBARRAYPROC glDisableVertexAttribArray;
PFNGLDELETESHADERPROC             glDeleteShader;
PFNGLDELETEPROGRAMPROC            glDeleteProgram;
PFNGLISPROGRAMPROC                glIsProgram;
PFNGLBLENDEQUATIONSEPARATEPROC    glBlendEquationSeparate;
PFNGLBLENDFUNCSEPARATEPROC        glBlendFuncSeparate;
#ifndef GL_ATI_blend_equation_separate
PFNGLBLENDCOLORPROC               glBlendColor;
PFNGLBLENDEQUATIONPROC            glBlendEquation;
#endif
#endif
PFNGLGENFRAMEBUFFERSPROC          glGenFramebuffers;
PFNGLDELETEFRAMEBUFFERSPROC       glDeleteFramebuffers;
PFNGLBINDFRAMEBUFFERPROC          glBindFramebuffer;
PFNGLFRAMEBUFFERTEXTURE2DPROC     glFramebufferTexture2D;
#ifndef __MACOSX__
PFNGLGENBUFFERSPROC               glGenBuffers;
PFNGLDELETEBUFFERSPROC            glDeleteBuffers;
PFNGLBINDBUFFERPROC               glBindBuffer;
PFNGLBUFFERDATAPROC               glBufferData;
PFNGLBUFFERSUBDATAPROC            glBufferSubData;
#endif
PFNGLGENVERTEXARRAYSPROC          glGenVertexArrays;
PFNGLBINDVERTEXARRAYPROC          glBindVertexArray;
PFNGLDELETEVERTEXARRAYSPROC       glDeleteVertexArrays;
static void InitExtensionEntries()
{
#ifndef __MACOSX__
	glCreateShader =             (PFNGLCREATESHADERPROC            )(size_t)SDL_GL_GetProcAddress("glCreateShader");
	glShaderSource =             (PFNGLSHADERSOURCEPROC            )(size_t)SDL_GL_GetProcAddress("glShaderSource");
	glCompileShader =            (PFNGLCOMPILESHADERPROC           )(size_t)SDL_GL_GetProcAddress("glCompileShader");
	glCreateProgram =            (PFNGLCREATEPROGRAMPROC           )(size_t)SDL_GL_GetProcAddress("glCreateProgram");
	glAttachShader =             (PFNGLATTACHSHADERPROC            )(size_t)SDL_GL_GetProcAddress("glAttachShader");
	glDetachShader =             (PFNGLDETACHSHADERPROC            )(size_t)SDL_GL_GetProcAddress("glDetachShader");
	glLinkProgram =              (PFNGLLINKPROGRAMPROC             )(size_t)SDL_GL_GetProcAddress("glLinkProgram");
	glUseProgram =               (PFNGLUSEPROGRAMPROC              )(size_t)SDL_GL_GetProcAddress("glUseProgram");
	glGetShaderiv =              (PFNGLGETSHADERIVPROC             )(size_t)SDL_GL_GetProcAddress("glGetShaderiv");
	glGetProgramiv =             (PFNGLGETPROGRAMIVPROC            )(size_t)SDL_GL_GetProcAddress("glGetProgramiv");
#ifdef ZILLALOG
	glGetShaderInfoLog =         (PFNGLGETSHADERINFOLOGPROC        )(size_t)SDL_GL_GetProcAddress("glGetShaderInfoLog");
	glGetProgramInfoLog =        (PFNGLGETPROGRAMINFOLOGPROC       )(size_t)SDL_GL_GetProcAddress("glGetProgramInfoLog");
#endif
	glGetAttribLocation =        (PFNGLGETATTRIBLOCATIONPROC       )(size_t)SDL_GL_GetProcAddress("glGetAttribLocation");
	glBindAttribLocation =       (PFNGLBINDATTRIBLOCATIONPROC      )(size_t)SDL_GL_GetProcAddress("glBindAttribLocation");
	glVertexAttribPointer =      (PFNGLVERTEXATTRIBPOINTERPROC     )(size_t)SDL_GL_GetProcAddress("glVertexAttribPointer");
	glEnableVertexAttribArray =  (PFNGLENABLEVERTEXATTRIBARRAYPROC )(size_t)SDL_GL_GetProcAddress("glEnableVertexAttribArray");
	glGetUniformLocation =       (PFNGLGETUNIFORMLOCATIONPROC      )(size_t)SDL_GL_GetProcAddress("glGetUniformLocation");
#if !defined(ZL_DOUBLE_PRECISCION)
	glUniformMatrix4fv =         (PFNGLUNIFORMMATRIX4FVPROC        )(size_t)SDL_GL_GetProcAddress("glUniformMatrix4fv");
	glVertexAttrib4fv =          (PFNGLVERTEXATTRIB4FVPROC         )(size_t)SDL_GL_GetProcAddress("glVertexAttrib4fv");
	glVertexAttrib4f =           (PFNGLVERTEXATTRIB4FPROC          )(size_t)SDL_GL_GetProcAddress("glVertexAttrib4f");
	glUniform1f =                (PFNGLUNIFORM1FPROC               )(size_t)SDL_GL_GetProcAddress("glUniform1f");
#else
	glUniformMatrix4dv =         (PFNGLUNIFORMMATRIX4DVPROC        )(size_t)SDL_GL_GetProcAddress("glUniformMatrix4dv");
	glVertexAttrib4dv =          (PFNGLVERTEXATTRIB4DVPROC         )(size_t)SDL_GL_GetProcAddress("glVertexAttrib4dv");
	glVertexAttrib4d =           (PFNGLVERTEXATTRIB4DPROC          )(size_t)SDL_GL_GetProcAddress("glVertexAttrib4d");
	glUniform1d =                (PFNGLUNIFORM1DPROC               )(size_t)SDL_GL_GetProcAddress("glUniform1d");
#endif
	glDisableVertexAttribArray = (PFNGLDISABLEVERTEXATTRIBARRAYPROC)(size_t)SDL_GL_GetProcAddress("glDisableVertexAttribArray");
	glDeleteShader =             (PFNGLDELETESHADERPROC            )(size_t)SDL_GL_GetProcAddress("glDeleteShader");
	glDeleteProgram =            (PFNGLDELETEPROGRAMPROC           )(size_t)SDL_GL_GetProcAddress("glDeleteProgram");
	glIsProgram =                (PFNGLISPROGRAMPROC               )(size_t)SDL_GL_GetProcAddress("glIsProgram");
	glBlendEquationSeparate =    (PFNGLBLENDEQUATIONSEPARATEPROC   )(size_t)SDL_GL_GetProcAddress("glBlendEquationSeparate");
	glBlendFuncSeparate =        (PFNGLBLENDFUNCSEPARATEPROC       )(size_t)SDL_GL_GetProcAddress("glBlendFuncSeparate");
#ifndef GL_ATI_blend_equation_separate
	glBlendColor =               (PFNGLBLENDCOLORPROC              )(size_t)SDL_GL_GetProcAddress("glBlendColor");
	glBlendEquation =            (PFNGLBLENDEQUATIONPROC           )(size_t)SDL_GL_GetProcAddress("glBlendEquation");
#endif
#endif
	glGenFramebuffers =          (PFNGLGENFRAMEBUFFERSPROC         )(size_t)SDL_GL_GetProcAddress("glGenFramebuffers");
	glDeleteFramebuffers =       (PFNGLDELETEFRAMEBUFFERSPROC      )(size_t)SDL_GL_GetProcAddress("glDeleteFramebuffers");
	glBindFramebuffer =          (PFNGLBINDFRAMEBUFFERPROC         )(size_t)SDL_GL_GetProcAddress("glBindFramebuffer");
	glFramebufferTexture2D =     (PFNGLFRAMEBUFFERTEXTURE2DPROC    )(size_t)SDL_GL_GetProcAddress("glFramebufferTexture2D");
#ifndef __MACOSX__
	glGenBuffers =               (PFNGLGENBUFFERSPROC              )(size_t)SDL_GL_GetProcAddress("glGenBuffers");
	glDeleteBuffers =            (PFNGLDELETEBUFFERSPROC           )(size_t)SDL_GL_GetProcAddress("glDeleteBuffers");
	glBindBuffer =               (PFNGLBINDBUFFERPROC              )(size_t)SDL_GL_GetProcAddress("glBindBuffer");
	glBufferData =               (PFNGLBUFFERDATAPROC              )(size_t)SDL_GL_GetProcAddress("glBufferData");
	glBufferSubData =            (PFNGLBUFFERSUBDATAPROC           )(size_t)SDL_GL_GetProcAddress("glBufferSubData");
#endif
	glGenVertexArrays =          (PFNGLGENVERTEXARRAYSPROC         )(size_t)SDL_GL_GetProcAddress("glGenVertexArrays");
	glBindVertexArray =          (PFNGLBINDVERTEXARRAYPROC         )(size_t)SDL_GL_GetProcAddress("glBindVertexArray");
	glDeleteVertexArrays =       (PFNGLDELETEVERTEXARRAYSPROC      )(size_t)SDL_GL_GetProcAddress("glDeleteVertexArrays");
}

#ifdef ZL_REQUIRE_INIT3DGLEXTENSIONENTRIES
PFNGLGETACTIVEUNIFORMPROC         glGetActiveUniform;
#ifndef GL_ATI_blend_equation_separate
PFNGLACTIVETEXTUREPROC            glActiveTexture;
#endif
#ifdef ZILLALOG
PFNGLGETBUFFERPARAMETERIVPROC     glGetBufferParameteriv;
PFNGLMAPBUFFERPROC                glMapBuffer;
PFNGLUNMAPBUFFERPROC              glUnmapBuffer;
#endif
PFNGLUNIFORM1IPROC                glUniform1i;
#if !defined(ZL_DOUBLE_PRECISCION)
PFNGLUNIFORM2FPROC                glUniform2f;
PFNGLUNIFORM3FPROC                glUniform3f;
PFNGLUNIFORM3FVPROC               glUniform3fv;
PFNGLUNIFORM4FPROC                glUniform4f;
#else
PFNGLUNIFORM2DPROC                glUniform2d;
PFNGLUNIFORM3DPROC                glUniform3d;
PFNGLUNIFORM3DVPROC               glUniform3dv;
PFNGLUNIFORM4DPROC                glUniform4d;
#endif
void ZL_Init3DGLExtensionEntries()
{
#ifndef GL_ATI_blend_equation_separate
	glActiveTexture =            (PFNGLACTIVETEXTUREPROC           )(size_t)SDL_GL_GetProcAddress("glActiveTexture");
#endif
	glGetActiveUniform =         (PFNGLGETACTIVEUNIFORMPROC        )(size_t)SDL_GL_GetProcAddress("glGetActiveUniform");
#ifdef ZILLALOG
	glGetBufferParameteriv =     (PFNGLGETBUFFERPARAMETERIVPROC    )(size_t)SDL_GL_GetProcAddress("glGetBufferParameteriv");
	glMapBuffer =                (PFNGLMAPBUFFERPROC               )(size_t)SDL_GL_GetProcAddress("glMapBuffer");
	glUnmapBuffer =              (PFNGLUNMAPBUFFERPROC             )(size_t)SDL_GL_GetProcAddress("glUnmapBuffer");
#endif
	glUniform1i =                (PFNGLUNIFORM1IPROC               )(size_t)SDL_GL_GetProcAddress("glUniform1i");
#if !defined(ZL_DOUBLE_PRECISCION)
	glUniform2f =                (PFNGLUNIFORM2FPROC               )(size_t)SDL_GL_GetProcAddress("glUniform2f");
	glUniform3f =                (PFNGLUNIFORM3FPROC               )(size_t)SDL_GL_GetProcAddress("glUniform3f");
	glUniform3fv =               (PFNGLUNIFORM3FVPROC              )(size_t)SDL_GL_GetProcAddress("glUniform3fv");
	glUniform4f =                (PFNGLUNIFORM4FPROC               )(size_t)SDL_GL_GetProcAddress("glUniform4f");
#else
	glUniform2d =                (PFNGLUNIFORM2DPROC               )(size_t)SDL_GL_GetProcAddress("glUniform2d");
	glUniform3d =                (PFNGLUNIFORM3DPROC               )(size_t)SDL_GL_GetProcAddress("glUniform3d");
	glUniform3dv =               (PFNGLUNIFORM3DVPROC              )(size_t)SDL_GL_GetProcAddress("glUniform3dv");
	glUniform4d =                (PFNGLUNIFORM4DPROC               )(size_t)SDL_GL_GetProcAddress("glUniform4d");
#endif
}
#endif

#ifndef ZL_USE_EXTERNAL_SDL
#define ZL_SDL_DO_OVERRIDE_IMPLEMENTATIONS
#include <SDL_config_zillalib.h>
#endif

#endif
