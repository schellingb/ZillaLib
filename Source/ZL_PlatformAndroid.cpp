/*
  ZillaLib
  Copyright (C) 2010-2019 Bernhard Schelling

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

#if defined(ANDROID)
#include "ZL_Platform.h"
#include "ZL_Application.h"
#include "ZL_Display.h"
#include "ZL_File.h"
#include "ZL_File_Impl.h"
#include "ZL_Display_Impl.h"
#include "ZL_Audio.h"
#include <jni.h>
#include <assert.h>
#include <math.h>
#include <GLES/gl.h>
#include <EGL/egl.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#if defined(ZILLALOG)
#include <android/log.h>
void __android_log(const char* logtag, const char* logtext) { __android_log_print(ANDROID_LOG_INFO, "ZILLALIB", "%s: %s", logtag, logtext); }
#define __ANDROID_LOG_PRINT_INFO(tag,fmt, ...) __android_log_print(ANDROID_LOG_INFO, tag, fmt, ##__VA_ARGS__)
#undef assert
#define assert(e) do { if (!(e)) { __android_log_print(ANDROID_LOG_ERROR, "ZillaAssert", "Error with check %s on in %s on line %d", #e, __FILE__, __LINE__); __assert2(__FILE__, __LINE__, __func__, #e); } } while (0)
#else
#define __ANDROID_LOG_PRINT_INFO(tag,fmt, ...)
#endif

// Java stuff
static JavaVM* javaVM = NULL;
static JNIEnv *jniEnv = NULL;
static jobject JavaZillaActivity, JavaAudio;
static jmethodID JavaSoftKeyboard, JavaVibrate, JavaSettingsGet, JavaSettingsSet, JavaSettingsDel, JavaSettingsHas, JavaSettingsSynchronize, JavaOpenExternalUrl;

// Audio stuff
static SLObjectItf SLESEngine, SLESOutMix, SLESPlayer;
static char* SLESBuffer[2];
static unsigned int SLESBufferSize, SLESBufferNum;
static bool SLESWasActivated;
static jmethodID JavaAudioOpen, JavaAudioControl;
static void SLESShutdown();
static bool SLESInit();

// Video stuff
static int ZL_ANDROID_sWindowWidth  = 0, ZL_ANDROID_sWindowHeight = 0;
static unsigned int ZL_ANDROID_WindowFlags = ZL_WINDOW_FULLSCREEN | ZL_WINDOW_INPUT_FOCUS | ZL_WINDOW_MOUSE_FOCUS;
static bool ZL_ANDROID_sWantsLandscape = true;
static ANativeWindow* RenderWindow;
static EGLDisplay RenderDisplay;
static EGLSurface RenderSurface;
static EGLContext RenderContext;
static pthread_t RenderThreadId;
enum { ZL_EVENT_ANDROID_SURFACECHANGE = _ZL_EVENT_MAX, ZL_EVENT_ANDROID_ACTIVITYPAUSES, ZL_EVENT_ANDROID_ACTIVITYFINISHES };
static pthread_mutex_t QueuedEventsMutex;
static std::vector<ZL_Event> QueuedEvents;

// Input stuff
enum { MULTI_TOUCH_POINTERID_BASE = 100, MAX_SIMULTANEOUS_TOUCHES = 10 };
enum ANDROIDTOUCH_ACTION { ANDROIDTOUCH_DOWN = 0, ANDROIDTOUCH_UP = 1, ANDROIDTOUCH_MOVE = 2 };
struct ZL_AndroidTouch { int lastx, lasty, touchid; };
static ZL_AndroidTouch ZL_ANDROID_touch[MAX_SIMULTANEOUS_TOUCHES];
static ZL_JoystickData *ZL_ANDROID_joysticks[2] = { NULL, NULL };
static unsigned short ZL_Android_KeyModState = ZLKMOD_NONE;
static bool bJNI_InitActivity = false;

static void ZL_WindowEvent(unsigned char event, int data1 = 0, int data2 = 0)
{
	ZL_Event e; e.type = ZL_EVENT_WINDOW;
	e.window.event = event; e.window.data1 = data1; e.window.data2 = data2;
	ZL_Display_Process_Event(e);
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved)
{
	javaVM = vm;
	return JNI_VERSION_1_4;
}

static void QueueEvent(const ZL_Event& e)
{
	pthread_mutex_lock(&QueuedEventsMutex);
	QueuedEvents.push_back(e);
	pthread_mutex_unlock(&QueuedEventsMutex);
}

static void RendererDestroy()
{
	//__ANDROID_LOG_PRINT_INFO("ZillaActivityNative", "Renderer: Destroying context");
	if (!RenderDisplay) return;
	eglMakeCurrent(RenderDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if (RenderContext) { eglDestroyContext(RenderDisplay, RenderContext); RenderContext = EGL_NO_CONTEXT; }
	if (RenderSurface) { eglDestroySurface(RenderDisplay, RenderSurface); RenderSurface = EGL_NO_SURFACE; }
	                     eglTerminate(RenderDisplay);                     RenderDisplay = EGL_NO_DISPLAY;
}

static bool RendererInitialize()
{
	EGLConfig config;
	EGLint numConfigs, format, width, height;

	//__ANDROID_LOG_PRINT_INFO("ZillaActivityNative", "Renderer: Initializing context");

	const EGLint configattribs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_BLUE_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_RED_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_DEPTH_SIZE, (ZL_ANDROID_WindowFlags & ZL_WINDOW_DEPTHBUFFER ? 8 : 0), EGL_STENCIL_SIZE, 0, EGL_NONE };
	if (    !(RenderDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY))
	     || !eglInitialize(RenderDisplay, 0, 0)
	     || !eglChooseConfig(RenderDisplay, configattribs, &config, 1, &numConfigs)
	     || !eglGetConfigAttrib(RenderDisplay, config, EGL_NATIVE_VISUAL_ID, &format))
	{
		//__ANDROID_LOG_PRINT_INFO("ZillaActivityNative", "Renderer: egl display initialization error %d", eglGetError());
		RendererDestroy();
		return false;
	}

	ANativeWindow_setBuffersGeometry(RenderWindow, 0, 0, format);

	const EGLint contextattribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	if (    !(RenderSurface = eglCreateWindowSurface(RenderDisplay, config, RenderWindow, 0))
	     || !(RenderContext = eglCreateContext(RenderDisplay, config, 0, contextattribs))
	     || !eglMakeCurrent(RenderDisplay, RenderSurface, RenderSurface, RenderContext)
	     || !eglQuerySurface(RenderDisplay, RenderSurface, EGL_WIDTH, &width)
	     || !eglQuerySurface(RenderDisplay, RenderSurface, EGL_HEIGHT, &height))
	{
		//__ANDROID_LOG_PRINT_INFO("ZillaActivityNative", "Renderer: egl context initialization error %d", eglGetError());
		RendererDestroy();
		return false;
	}

	//__ANDROID_LOG_PRINT_INFO("ZillaActivityNative", "Renderer: egl context received size - width: %d - height: %d", width, height);
	if (!(ZL_ANDROID_WindowFlags & ZL_WINDOW_ALLOWANYORIENTATION) && (ZL_ANDROID_sWantsLandscape != (width > height))) { jint tmp = height; height = width; width = tmp; }
	ZL_ANDROID_sWindowWidth = width;
	ZL_ANDROID_sWindowHeight = height;
	return true;
}

static void* RenderThreadFunc(void*)
{
	javaVM->AttachCurrentThread(&jniEnv, NULL);
	//__ANDROID_LOG_PRINT_INFO("ZillaActivityNative", "Renderer: Start Thread - Window: %d - Display: %d - Surface: %d", RenderWindow!=0, RenderDisplay!=0, RenderSurface!=0);

	for (bool loopActive = true;;)
	{
		if (!QueuedEvents.empty())
		{
			pthread_mutex_lock(&QueuedEventsMutex);
			RestartEventProcessing:
			for (std::vector<ZL_Event>::iterator it = QueuedEvents.begin(); it != QueuedEvents.end(); ++it)
			{
				if (!RenderDisplay)
				{
					//Only ZL_EVENT_ANDROID_* events can be processed without a render display initialized
					if (it->type == ZL_EVENT_ANDROID_SURFACECHANGE)
					{
						if (pZL_WindowFlags != &ZL_ANDROID_WindowFlags)
						{
							//first time getting a surface, start ZillaLib up, will call RendererInitialize in ZL_CreateWindow
							ZillaLibInit(0, NULL);
						}
						else
						{
							int oldw = ZL_ANDROID_sWindowWidth, oldh = ZL_ANDROID_sWindowHeight;
							RendererInitialize();
							if (oldw == ZL_ANDROID_sWindowWidth && oldh == ZL_ANDROID_sWindowHeight) InitGL(oldw, oldh);
							else ZL_WindowEvent(ZL_WINDOWEVENT_RESIZED, ZL_ANDROID_sWindowWidth, ZL_ANDROID_sWindowHeight);
							ZL_LastFPSTicks = ZL_Application::Ticks = ZL_GetTicks();
							ZL_ANDROID_WindowFlags &= ~ZL_WINDOW_MINIMIZED;
							ZL_WindowEvent(ZL_WINDOWEVENT_RESTORED);
							if (SLESWasActivated) SLESInit();
						}
						//with the RenderDisplay now initialized, restart the event loop to forward other events
						QueuedEvents.erase(it);
						goto RestartEventProcessing;
					}
					else if (it->type == ZL_EVENT_ANDROID_ACTIVITYPAUSES || it->type == ZL_EVENT_ANDROID_ACTIVITYFINISHES)
					{
						loopActive = false;
						QueuedEvents.erase(it);
						goto RestartEventProcessing;
					}
					else continue;
				}

				if (it->type == ZL_EVENT_ANDROID_SURFACECHANGE)
				{
					//__ANDROID_LOG_PRINT_INFO("ZillaActivityNative", "Renderer: SurfaceChange W: %d - H: %d - OldW: %d - OldH: %d", it->window.data1, it->window.data2, ZL_ANDROID_sWindowWidth, ZL_ANDROID_sWindowHeight);
					if (it->window.data1 != ZL_ANDROID_sWindowWidth || it->window.data2 != ZL_ANDROID_sWindowHeight)
					{
						ZL_ANDROID_sWindowWidth = it->window.data1;
						ZL_ANDROID_sWindowHeight = it->window.data2;
						ZL_WindowEvent(ZL_WINDOWEVENT_RESIZED, ZL_ANDROID_sWindowWidth, ZL_ANDROID_sWindowHeight);
					}
				}
				else if (it->type == ZL_EVENT_ANDROID_ACTIVITYPAUSES)
				{
					#ifdef ZL_VIDEO_WEAKCONTEXT
					StoreAllFrameBufferTexturesOnDeactivate();
					#endif
					ZL_ANDROID_WindowFlags |= ZL_WINDOW_MINIMIZED;
					ZL_WindowEvent(ZL_WINDOWEVENT_MINIMIZED);
					SLESShutdown();
					//send remaining events then end render thread
					loopActive = false;
				}
				else if (it->type == ZL_EVENT_ANDROID_ACTIVITYFINISHES)
				{
					//send remaining events then end render thread
					loopActive = false;
				}
				else
				{
					//base event, forward it directly to ZillaLib
					ZL_Display_Process_Event(*it);
				}
			}
			if (RenderDisplay) QueuedEvents.clear();
			pthread_mutex_unlock(&QueuedEventsMutex);
		}

		if (!loopActive) break;
		if (!RenderDisplay) { ZL_Delay(1); continue; } //renderer not yet initialized

		ZL_MainApplication->Frame();

		if (ZL_MainApplicationFlags & ZL_APPLICATION_DONE)
		{
			jniEnv->CallVoidMethod(JavaZillaActivity, jniEnv->GetMethodID(jniEnv->GetObjectClass(JavaZillaActivity), "doFinish", "()V"));
			break;
		}
		if (!eglSwapBuffers(RenderDisplay, RenderSurface))
		{
			__ANDROID_LOG_PRINT_INFO("ZillaActivityNative", "Renderer: eglSwapBuffers() returned error %d", eglGetError());
		}
	}

	RendererDestroy();

	javaVM->DetachCurrentThread();
	pthread_exit(0);
}

extern "C" JNIEXPORT void JNICALL Java_org_zillalib_ZillaActivity_NativeOnCreate(JNIEnv* env, jobject objactivity, jstring apkPath)
{
	//__ANDROID_LOG_PRINT_INFO("ZillaActivityNative", "NativeOnCreate");

	memset(ZL_ANDROID_touch, 0, sizeof(ZL_ANDROID_touch));
	for (int i = 0; i < MAX_SIMULTANEOUS_TOUCHES; i++) ZL_ANDROID_touch[i].lastx = ZL_ANDROID_touch[i].lasty = -1;

	jclass JavaZillaActivityClass = env->GetObjectClass(objactivity);
	JavaSoftKeyboard        = env->GetMethodID(JavaZillaActivityClass, "softKeyboard", "(I)I");
	JavaVibrate             = env->GetMethodID(JavaZillaActivityClass, "vibrate", "(I)V");
	JavaSettingsGet         = env->GetMethodID(JavaZillaActivityClass, "settingsGet", "(Ljava/lang/String;)Ljava/lang/String;");
	JavaSettingsSet         = env->GetMethodID(JavaZillaActivityClass, "settingsSet", "(Ljava/lang/String;Ljava/lang/String;)V");
	JavaSettingsDel         = env->GetMethodID(JavaZillaActivityClass, "settingsDel", "(Ljava/lang/String;)V");
	JavaSettingsHas         = env->GetMethodID(JavaZillaActivityClass, "settingsHas", "(Ljava/lang/String;)Z");
	JavaSettingsSynchronize = env->GetMethodID(JavaZillaActivityClass, "settingsSynchronize", "()V");
	JavaOpenExternalUrl     = env->GetMethodID(JavaZillaActivityClass, "openExternalUrl", "(Ljava/lang/String;)V");
	JavaZillaActivity       = env->NewGlobalRef(objactivity);

	assert(!bJNI_InitActivity);
	bJNI_InitActivity = true;

	const char* pcApkPath, *pcUID;
	pcApkPath = env->GetStringUTFChars(apkPath, NULL);
	ZL_LOG1("ANDROID", "Got apkPath: %s - Load APK as ZIP file", pcApkPath);
	ZL_File::DefaultReadFileContainer = ZL_FileContainer_ZIP(pcApkPath);
	env->ReleaseStringUTFChars(apkPath, pcApkPath);

	pthread_mutex_init(&QueuedEventsMutex, 0);
}

extern "C" JNIEXPORT void JNICALL Java_org_zillalib_ZillaActivity_NativeOnResume(JNIEnv*, jobject)
{
	if (RenderWindow)
	{
		ZL_Event e = ZL_Event::Make(ZL_EVENT_ANDROID_SURFACECHANGE);
		e.window.data1 = ZL_ANDROID_sWindowWidth;
		e.window.data2 = ZL_ANDROID_sWindowHeight;
		QueueEvent(e);
	}
	//__ANDROID_LOG_PRINT_INFO("ZillaActivityNative", "NativeOnResume - Creating renderer thread (active thread: #%ld) - Window: %d - Display: %d - Surface: %d", RenderThreadId, RenderWindow!=0, RenderDisplay!=0, RenderSurface!=0);
	pthread_create(&RenderThreadId, 0, RenderThreadFunc, NULL);
	//__ANDROID_LOG_PRINT_INFO("ZillaActivityNative", "NativeOnResume - Created renderer thread #%ld - Window: %d - Display: %d - Surface: %d", RenderThreadId, RenderWindow!=0, RenderDisplay!=0, RenderSurface!=0);
}

extern "C" JNIEXPORT void JNICALL Java_org_zillalib_ZillaActivity_NativeOnPause(JNIEnv*, jobject, jboolean isFinishing)
{
	//__ANDROID_LOG_PRINT_INFO("ZillaActivityNative", "NativeOnPause - Stopping renderer thread #%ld - Window: %d - Display: %d - Surface: %d", RenderThreadId, RenderWindow!=0, RenderDisplay!=0, RenderSurface!=0);
	QueueEvent(ZL_Event::Make(isFinishing ? ZL_EVENT_ANDROID_ACTIVITYFINISHES : ZL_EVENT_ANDROID_ACTIVITYPAUSES));
	pthread_join(RenderThreadId, 0);
	RenderThreadId = 0;
	//__ANDROID_LOG_PRINT_INFO("ZillaActivityNative", "NativeOnPause - Renderer thread stopped - Window: %d - Display: %d - Surface: %d", RenderWindow!=0, RenderDisplay!=0, RenderSurface!=0);
}

extern "C" JNIEXPORT void JNICALL Java_org_zillalib_ZillaActivity_NativeOnDestroy(JNIEnv* jenv, jobject)
{
	pthread_mutex_destroy(&QueuedEventsMutex);
	if (!(ZL_MainApplicationFlags & ZL_APPLICATION_DONE))
	{
		ZL_Event e = ZL_Event::Make(ZL_EVENT_QUIT);
		ZL_Display_Process_Event(e);
	}
	ZL_MainApplication->OnQuit();
	SLESShutdown();
}

extern "C" JNIEXPORT void JNICALL Java_org_zillalib_ZillaActivity_NativeSetSurface(JNIEnv* jenv, jobject, jobject surface, jint w, jint h)
{
	if (surface)
	{
		RenderWindow = ANativeWindow_fromSurface(jenv, surface);
		//__ANDROID_LOG_PRINT_INFO("ZillaActivityNative", "Got window %p - w: %d - h: %d - HadWindowBefore: %d", RenderWindow, w, h, RenderDisplay!=0);
		ZL_Event e = ZL_Event::Make(ZL_EVENT_ANDROID_SURFACECHANGE);
		e.window.data1 = w;
		e.window.data2 = h;
		QueueEvent(e);
	}
	else
	{
		//__ANDROID_LOG_PRINT_INFO("ZillaActivityNative", "Releasing window");
		ANativeWindow_release(RenderWindow);
		RenderWindow = NULL;
	}
}

bool ZL_CreateWindow(const char*, int width, int height, int displayflags)
{
	if (displayflags & ZL_DISPLAY_ALLOWANYORIENTATION) ZL_ANDROID_WindowFlags |= ZL_WINDOW_ALLOWANYORIENTATION;
	if (displayflags & ZL_DISPLAY_DEPTHBUFFER) ZL_ANDROID_WindowFlags |= ZL_WINDOW_DEPTHBUFFER;

	ZL_ANDROID_sWantsLandscape = (width > height);
	pZL_WindowFlags = &ZL_ANDROID_WindowFlags;

	jboolean AllowAnyOrientation = (jboolean)((ZL_ANDROID_WindowFlags & ZL_WINDOW_ALLOWANYORIENTATION)!=0);
	jboolean WantsLandscape      = (jboolean)ZL_ANDROID_sWantsLandscape;
	jboolean OverridesVolumeKeys = (jboolean)((displayflags & ZL_DISPLAY_ANDROID_OVERRIDEVOLUMEKEYS)!=0);
	jboolean Immersive           = (jboolean)((displayflags & ZL_DISPLAY_ANDROID_SHOWNAVIGATIONBAR)==0);
	jniEnv->CallVoidMethod(JavaZillaActivity, jniEnv->GetMethodID(jniEnv->GetObjectClass(JavaZillaActivity), "setFlags", "(ZZZZ)V"), AllowAnyOrientation, WantsLandscape, OverridesVolumeKeys, Immersive);

	RendererInitialize();

	return true;
}

extern "C" JNIEXPORT void JNICALL Java_org_zillalib_ZillaActivity_NativeKey(JNIEnv* env, jobject thiz, jint key, jint action)
{
	static const ZL_Key android_zlkey_table[] = {
		ZLK_UNKNOWN,     ZLK_KP_4,        ZLK_KP_6,         ZLK_HOME,          ZLK_ESCAPE,       ZLK_END,         ZLK_LSHIFT,     ZLK_0,           ZLK_1,            ZLK_2,
		ZLK_3,           ZLK_4,           ZLK_5,            ZLK_6,             ZLK_7,            ZLK_8,           ZLK_9,          ZLK_KP_MULTIPLY, ZLK_KP_DIVIDE,    ZLK_UP,
		ZLK_DOWN,        ZLK_LEFT,        ZLK_RIGHT,        ZLK_KP_5,          ZLK_VOLUMEUP,     ZLK_VOLUMEDOWN,  ZLK_POWER,      ZLK_RSHIFT,      ZLK_CLEAR,        ZLK_A,
		ZLK_B,           ZLK_C,           ZLK_D,            ZLK_E,             ZLK_F,            ZLK_G,           ZLK_H,          ZLK_I,           ZLK_J,            ZLK_K,
		ZLK_L,           ZLK_M,           ZLK_N,            ZLK_O,             ZLK_P,            ZLK_Q,           ZLK_R,          ZLK_S,           ZLK_T,            ZLK_U,
		ZLK_V,           ZLK_W,           ZLK_X,            ZLK_Y,             ZLK_Z,            ZLK_COMMA,       ZLK_PERIOD,     ZLK_LALT,        ZLK_RALT,         ZLK_LSHIFT,
		ZLK_RSHIFT,      ZLK_TAB,         ZLK_SPACE,        ZLK_LGUI,          ZLK_MEDIASELECT,  ZLK_MAIL,        ZLK_RETURN,     ZLK_BACKSPACE,   ZLK_GRAVE,        ZLK_MINUS,
		ZLK_EQUALS,      ZLK_LEFTBRACKET, ZLK_RIGHTBRACKET, ZLK_BACKSLASH,     ZLK_SEMICOLON,    ZLK_APOSTROPHE,  ZLK_SLASH,      ZLK_KP_AT,       ZLK_NUMLOCKCLEAR, ZLK_MUTE,
		ZLK_TAB,         ZLK_KP_PLUS,     ZLK_MENU,         ZLK_MODE,          ZLK_FIND,         ZLK_AUDIOPLAY,   ZLK_AUDIOSTOP,  ZLK_AUDIONEXT,   ZLK_AUDIOPREV,    ZLK_AUDIOPLAY,
		ZLK_AUDIONEXT,   ZLK_AUDIOMUTE,   ZLK_PAGEUP,       ZLK_PAGEDOWN,      ZLK_LANG2,        ZLK_LANG1,       ZLK_F,          ZLK_G,           ZLK_H,            ZLK_R,
		ZLK_T,           ZLK_Y,           ZLK_Q,            ZLK_E,             ZLK_X,            ZLK_C,           ZLK_LSHIFT,     ZLK_RSHIFT,      ZLK_RETURN,       ZLK_LCTRL,
		ZLK_MODE,        ZLK_ESCAPE,      ZLK_DELETE,       ZLK_LCTRL,         ZLK_RCTRL,        ZLK_CAPSLOCK,    ZLK_SCROLLLOCK, ZLK_LGUI,        ZLK_RGUI,         ZLK_MODE,
		ZLK_PRINTSCREEN, ZLK_PAUSE,       ZLK_HOME,         ZLK_END,           ZLK_INSERT,       ZLK_AGAIN,       ZLK_AUDIOPLAY,  ZLK_AUDIOSTOP,   ZLK_AUDIOSTOP,    ZLK_AUDIOSTOP,
		ZLK_AUDIOPLAY,   ZLK_F1,          ZLK_F2,           ZLK_F3,            ZLK_F4,           ZLK_F5,          ZLK_F6,         ZLK_F7,          ZLK_F8,           ZLK_F9,
		ZLK_F10,         ZLK_F11,         ZLK_F12,          ZLK_NUMLOCKCLEAR,  ZLK_KP_0,         ZLK_KP_1,        ZLK_KP_2,       ZLK_KP_3,        ZLK_KP_4,         ZLK_KP_5,
		ZLK_KP_6,        ZLK_KP_7,        ZLK_KP_8,         ZLK_KP_9,          ZLK_KP_DIVIDE,    ZLK_KP_MULTIPLY, ZLK_KP_MINUS,   ZLK_KP_PLUS,     ZLK_KP_DECIMAL,   ZLK_KP_COMMA,
		ZLK_KP_ENTER,    ZLK_KP_EQUALS,   ZLK_KP_LEFTBRACE, ZLK_KP_RIGHTBRACE, ZLK_MUTE,         ZLK_HELP,        ZLK_VOLUMEUP,   ZLK_VOLUMEDOWN,  ZLK_KP_PLUS,      ZLK_KP_MINUS,
		ZLK_WWW,         ZLK_MAIL,        ZLK_COMPUTER,     ZLK_CALCULATOR,    ZLK_AC_BOOKMARKS, ZLK_AC_SEARCH,   ZLK_AC_HOME,    ZLK_POWER,       ZLK_LANG6,        ZLK_KP_POWER,
		ZLK_LANG7,       ZLK_KP_POWER,    ZLK_LANG8,        ZLK_KP_MEMSTORE,   ZLK_KP_MEMRECALL, ZLK_KP_MEMCLEAR, ZLK_KP_CLEAR,   ZLK_APPLICATION, ZLK_F1,           ZLK_F2,
		ZLK_F3,          ZLK_F4,          ZLK_F5,           ZLK_F6,            ZLK_F7,           ZLK_F8,          ZLK_F9,         ZLK_F10,         ZLK_F11,          ZLK_F12,
		ZLK_F13,         ZLK_F14,         ZLK_F15,          ZLK_F16,           ZLK_LANG1,        ZLK_MUTE,        ZLK_3, ZLK_MAIL, ZLK_WWW, ZLK_AUDIOPLAY, ZLK_CALCULATOR
	};
	//__ANDROID_LOG_PRINT_INFO("ZillaLib", "Keyboard %d - P: %d - Key: %d (max %d) -> %d (Status = %d)", (int)keyboard.driverdata, (int)ZL_GetKeyboard((int)keyboard.driverdata), key, ZL_arraysize(keymap), keymap[key], action)
	ZL_Key zlk = android_zlkey_table[key > (sizeof(android_zlkey_table)/sizeof(android_zlkey_table[0])) ? 0 : key];
	switch (zlk)
	{
		case ZLK_LALT:   (action ? ZL_Android_KeyModState |= ZLKMOD_LALT   : ZL_Android_KeyModState &= ~ZLKMOD_LALT  ); break;
		case ZLK_RALT:   (action ? ZL_Android_KeyModState |= ZLKMOD_RALT   : ZL_Android_KeyModState &= ~ZLKMOD_RALT  ); break;
		case ZLK_LSHIFT: (action ? ZL_Android_KeyModState |= ZLKMOD_LSHIFT : ZL_Android_KeyModState &= ~ZLKMOD_LSHIFT); break;
		case ZLK_RSHIFT: (action ? ZL_Android_KeyModState |= ZLKMOD_RSHIFT : ZL_Android_KeyModState &= ~ZLKMOD_RSHIFT); break;
		case ZLK_LGUI:
		case ZLK_RGUI:   (action ? ZL_Android_KeyModState |= ZLKMOD_META   : ZL_Android_KeyModState &= ~ZLKMOD_META  ); break;
		default: break;
	}
	ZL_Event e;
	e.type = (action ? ZL_EVENT_KEYDOWN : ZL_EVENT_KEYUP);
	e.key.is_down = (action ? 1 : 0);
	e.key.key = zlk;
	e.key.mod = ZL_Android_KeyModState;
	QueueEvent(e);
}

extern "C" JNIEXPORT void JNICALL Java_org_zillalib_ZillaActivity_NativeText(JNIEnv* env, jobject thiz, jstring text)
{
	const char* pcText = env->GetStringUTFChars(text, NULL);
	ZL_Event e;
	e.type = ZL_EVENT_TEXTINPUT;
	int len = strlen(pcText);
	int maxlen = (len >= sizeof(e.text.text) ? sizeof(e.text.text)-1: len);
	memcpy(e.text.text, pcText, maxlen);
	e.text.text[maxlen] = 0;
	QueueEvent(e);
	env->ReleaseStringUTFChars(text, pcText);
}

extern "C" JNIEXPORT void JNICALL Java_org_zillalib_ZillaActivity_NativeTouch(JNIEnv* env, jobject thiz, jint x, jint y, jint action, jint pointerId, jint pressure, jint radius)
{
	//__ANDROID_LOG_PRINT_INFO("ZillaLib", "nativeTouch: x: %d y: %d action: %d pointerId: %d", x, y, action, pointerId);
	for(int i = 0; i < MAX_SIMULTANEOUS_TOUCHES; i++)
	{
		//__ANDROID_LOG_PRINT_INFO("ZillaLib", "nativeTouch: Check touch %d - touchid: %d", i, ZL_ANDROID_touch[i].touchid);
		if (action == ANDROIDTOUCH_DOWN) { if (ZL_ANDROID_touch[i].touchid != 0) continue; }
		else if (ZL_ANDROID_touch[i].touchid != (MULTI_TOUCH_POINTERID_BASE + pointerId)) continue;

		if (action == ANDROIDTOUCH_DOWN) ZL_ANDROID_touch[i].touchid = (MULTI_TOUCH_POINTERID_BASE + pointerId);
		if (action == ANDROIDTOUCH_UP)   ZL_ANDROID_touch[i].touchid = 0;

		ZL_Event em;
		em.type = ZL_EVENT_MOUSEMOTION;
		em.motion.which = i;
		em.motion.state = (action == ANDROIDTOUCH_DOWN ? 0 : 1);
		em.motion.x = x;
		em.motion.y = y;
		em.motion.xrel = (ZL_ANDROID_touch[i].lastx >= 0 ? x - ZL_ANDROID_touch[i].lastx : 0);
		em.motion.yrel = (ZL_ANDROID_touch[i].lasty >= 0 ? y - ZL_ANDROID_touch[i].lasty : 0);
		//__ANDROID_LOG_PRINT_INFO("ZillaLib", "nativeTouch: %d action: %d, send motion: lastx: %d lasty: %d", i, action, ZL_ANDROID_touch[i].lastx, ZL_ANDROID_touch[i].lasty);
		if (em.motion.xrel || em.motion.yrel) QueueEvent(em);
		ZL_ANDROID_touch[i].lastx = x;
		ZL_ANDROID_touch[i].lasty = y;

		if (action == ANDROIDTOUCH_DOWN || action == ANDROIDTOUCH_UP)
		{
			ZL_Event eb;
			eb.type = (action == ANDROIDTOUCH_DOWN ? ZL_EVENT_MOUSEBUTTONDOWN : ZL_EVENT_MOUSEBUTTONUP);
			eb.button.which = i;
			eb.button.button = 1;
			eb.button.is_down = (action == ANDROIDTOUCH_DOWN);
			eb.button.x = x;
			eb.button.y = y;
			//__ANDROID_LOG_PRINT_INFO("ZillaLib", "nativeTouch: send button: %d", eb.button.state);
			QueueEvent(eb);
		}
		if (action == ANDROIDTOUCH_UP) ZL_ANDROID_touch[i].lastx = ZL_ANDROID_touch[i].lasty = -1;
		//__ANDROID_LOG_PRINT_INFO("ZillaLib", "nativeTouch: %d POSTaction: %d, send motion: lastx: %d lasty: %d", i, action, ZL_ANDROID_touch[i].lastx, ZL_ANDROID_touch[i].lasty);
		return;
	}
}

void ZL_JoystickAxisEvent(ZL_JoystickData* j, int axis, signed short val)
{
	if (j->axes[axis] == val) return;
	j->axes[axis] = val;
	ZL_Event e;
	e.type = ZL_EVENT_JOYAXISMOTION;
	e.jaxis.which = j->device_index;
	e.jaxis.axis = axis;
	e.jaxis.value = val;
	QueueEvent(e);
}

extern "C" JNIEXPORT void JNICALL Java_org_zillalib_ZillaActivity_NativeGyroscope(JNIEnv* env, jobject thiz, jfloat x, jfloat y, jfloat z)
{
	if (!ZL_ANDROID_joysticks[0]) return;
	int ix = x * -200; if (ix > 16000) ix = 16000 - ix; if (ix < -16000) ix = 32000 + ix;
	int iy = y * -200; if (iy > 16000) iy = 16000 - iy; if (iy < -16000) iy = 32000 + iy;
	int iz = z * -200; if (iz > 16000) iz = 16000 - iz; if (iz < -16000) iz = 32000 + iz;
	ZL_JoystickAxisEvent(ZL_ANDROID_joysticks[0], 0, (signed short)ix);
	ZL_JoystickAxisEvent(ZL_ANDROID_joysticks[0], 1, (signed short)iy);
	ZL_JoystickAxisEvent(ZL_ANDROID_joysticks[0], 2, (signed short)iz);
}

extern "C" JNIEXPORT void JNICALL Java_org_zillalib_ZillaActivity_NativeAccelerometer(JNIEnv* env, jobject thiz, jfloat x, jfloat y, jfloat z)
{
	if (!ZL_ANDROID_joysticks[1]) return;
	
	// Send raw accelerometer values (maintaining backward compatibility)
	ZL_JoystickAxisEvent(ZL_ANDROID_joysticks[1], 0, (signed short)(x * 1000));
	ZL_JoystickAxisEvent(ZL_ANDROID_joysticks[1], 1, (signed short)(y * 1000));
	ZL_JoystickAxisEvent(ZL_ANDROID_joysticks[1], 2, (signed short)(z * 1000));
	
	// Calculate orientation angles from accelerometer data (FIXED implementation)
	// Normalize the acceleration vector to get gravity direction
	float normal = sqrtf(x*x + y*y + z*z);
	if (normal <= 0.0000001f) normal = 1.0f;
	
	float normalizedX = x / normal;
	float normalizedY = y / normal;
	float normalizedZ = z / normal;
	
	// Calculate roll (rotation around X axis) and pitch (rotation around Y axis) in degrees
	// Roll: angle of rotation around the X axis (tilt left/right)
	float roll = atan2f(-normalizedY, normalizedZ) * 180.0f / 3.1415926535897932f;
	
	// Pitch: angle of rotation around the Y axis (tilt forward/backward)
	float pitch = atan2f(normalizedX, sqrtf(normalizedY*normalizedY + normalizedZ*normalizedZ)) * 180.0f / 3.1415926535897932f;
	
	// TODO: If needed, orientation angles could be sent as additional joystick axes
	// Currently keeping backward compatibility by only sending raw accelerometer values
}

ZL_String ZL_DeviceUniqueID()
{
	jstring juid = (jstring)jniEnv->CallObjectMethod(JavaZillaActivity, jniEnv->GetMethodID(jniEnv->GetObjectClass(JavaZillaActivity), "getUID", "()Ljava/lang/String;"));
	const char *pcuid = jniEnv->GetStringUTFChars(juid, NULL);
	ZL_String uid(pcuid);
	jniEnv->ReleaseStringUTFChars(juid, pcuid);
	return uid;
}

void ZL_SettingsInit(const char* FallbackConfigFilePrefix)
{
	jniEnv->CallVoidMethod(JavaZillaActivity, jniEnv->GetMethodID(jniEnv->GetObjectClass(JavaZillaActivity), "settingsInit", "(Ljava/lang/String;)V"), jniEnv->NewStringUTF(FallbackConfigFilePrefix));
}
const ZL_String ZL_SettingsGet(const char* Key)
{
	jstring jvalue = (jstring)jniEnv->CallObjectMethod(JavaZillaActivity, JavaSettingsGet, jniEnv->NewStringUTF(Key));
	const char *pcValue = jniEnv->GetStringUTFChars(jvalue, NULL);
	ZL_String Value(pcValue);
	jniEnv->ReleaseStringUTFChars(jvalue, pcValue);
	return Value;
}
void ZL_SettingsSet(const char* Key, const ZL_String& Value)
{
	jniEnv->CallVoidMethod(JavaZillaActivity, JavaSettingsSet, jniEnv->NewStringUTF(Key), jniEnv->NewStringUTF(Value.c_str()));
}
void ZL_SettingsDel(const char* Key)
{
	jniEnv->CallVoidMethod(JavaZillaActivity, JavaSettingsDel, jniEnv->NewStringUTF(Key));
}
bool ZL_SettingsHas(const char* Key)
{
	return jniEnv->CallBooleanMethod(JavaZillaActivity, JavaSettingsHas, jniEnv->NewStringUTF(Key));
}
void ZL_SettingsSynchronize()
{
	jniEnv->CallVoidMethod(JavaZillaActivity, JavaSettingsSynchronize);
}
void ZL_OpenExternalUrl(const char* url)
{
	jniEnv->CallVoidMethod(JavaZillaActivity, JavaOpenExternalUrl, jniEnv->NewStringUTF(url));
}

void ZL_SoftKeyboardToggle()
{
	jniEnv->CallIntMethod(JavaZillaActivity, JavaSoftKeyboard, (jint)0);
}
void ZL_SoftKeyboardShow()
{
	jniEnv->CallIntMethod(JavaZillaActivity, JavaSoftKeyboard, (jint)1);
}
void ZL_SoftKeyboardHide()
{
	jniEnv->CallIntMethod(JavaZillaActivity, JavaSoftKeyboard, (jint)2);
}
bool ZL_SoftKeyboardIsShown()
{
	return (jniEnv->CallIntMethod(JavaZillaActivity, JavaSoftKeyboard, (jint)3) != 0);
}
void ZL_DeviceVibrate(int duration)
{
	jniEnv->CallVoidMethod(JavaZillaActivity, JavaVibrate, (jint)duration);
}

void ZL_GetWindowSize(int *w, int *h)
{
	*w = ZL_ANDROID_sWindowWidth;
	*h = ZL_ANDROID_sWindowHeight;
}

int ZL_NumJoysticks()
{
	return 2;
}
ZL_JoystickData* ZL_JoystickHandleOpen(int index)
{
	if (index < 0 || index >= 2) return NULL;
	if (ZL_ANDROID_joysticks[index]) return ZL_ANDROID_joysticks[index];
	ZL_ANDROID_joysticks[index] = new ZL_JoystickData();
	ZL_ANDROID_joysticks[index]->device_index = index;
	if (index == 0) ZL_ANDROID_joysticks[index]->name = "Android gyroscope";
	if (index == 1) ZL_ANDROID_joysticks[index]->name = "Android accelerometer";
	ZL_ANDROID_joysticks[index]->nhats = ZL_ANDROID_joysticks[index]->nballs = ZL_ANDROID_joysticks[index]->nbuttons = 0;
	ZL_ANDROID_joysticks[index]->hats = NULL;
	ZL_ANDROID_joysticks[index]->balls = NULL;
	ZL_ANDROID_joysticks[index]->buttons = NULL;
	ZL_ANDROID_joysticks[index]->naxes = 3;
	ZL_ANDROID_joysticks[index]->axes = new signed short[3];
	jniEnv->CallVoidMethod(JavaZillaActivity, jniEnv->GetMethodID(jniEnv->GetObjectClass(JavaZillaActivity), "joystickStatus", "(IZ)V"), (jint)index, (jboolean)1);
	return ZL_ANDROID_joysticks[index];
}
void ZL_JoystickHandleClose(ZL_JoystickData* joystick)
{
	int index = (joystick ? joystick->device_index : -1);
	if (index < 0 || index >= 2 || !ZL_ANDROID_joysticks[index]) return;
	//if (ZL_ANDROID_joysticks[index]->hats) delete ZL_ANDROID_joysticks[index]->hats;
	//if (ZL_ANDROID_joysticks[index]->balls) delete ZL_ANDROID_joysticks[index]->balls;
	//if (ZL_ANDROID_joysticks[index]->buttons) delete ZL_ANDROID_joysticks[index]->buttons;
	if (ZL_ANDROID_joysticks[index]->axes) delete ZL_ANDROID_joysticks[index]->axes;
	jniEnv->CallVoidMethod(JavaZillaActivity, jniEnv->GetMethodID(jniEnv->GetObjectClass(JavaZillaActivity), "joystickStatus", "(IZ)V"), (jint)index, (jboolean)0);
	delete ZL_ANDROID_joysticks[index];
	ZL_ANDROID_joysticks[index] = NULL;
}

static void SLESPlayerCallback(SLAndroidSimpleBufferQueueItf bufferQueue, void*)
{
	SLESBufferNum ^= 1; // = (SLESBufferNum == 3 ? 0 : SLESBufferNum+1);
	ZL_PlatformAudioMix((short*)SLESBuffer[SLESBufferNum], SLESBufferSize);
	(*bufferQueue)->Enqueue(bufferQueue, SLESBuffer[SLESBufferNum], SLESBufferSize);
}

static bool SLESInit()
{
	if (SLESEngine) return false; //already playing

	SLEngineItf engineEngine;
	SLPlayItf playerPlay;
	SLAndroidSimpleBufferQueueItf bufferQueue;
	SLDataLocator_AndroidSimpleBufferQueue loc_bufq = { SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2 };
	SLDataFormat_PCM format_pcm = { SL_DATAFORMAT_PCM, 2, SL_SAMPLINGRATE_44_1, SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16, SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT, SL_BYTEORDER_LITTLEENDIAN };
	SLDataSource audioSrc = { &loc_bufq, &format_pcm };
	SLDataLocator_OutputMix loc_outmix = { SL_DATALOCATOR_OUTPUTMIX, NULL };
	SLDataSink audioSnk = { &loc_outmix, NULL };
	const SLInterfaceID ids[1] = { SL_IID_BUFFERQUEUE };
	const SLboolean req[1] = { SL_BOOLEAN_TRUE };
	const unsigned int minbuf = (unsigned int)jniEnv->CallIntMethod(JavaZillaActivity, jniEnv->GetMethodID(jniEnv->GetObjectClass(JavaZillaActivity), "getAudioFramesPerBuffer", "()I"))*2*2; //16 bit, stereo

	if (SLESBufferSize < minbuf) SLESBufferSize = minbuf;
	SLESBuffer[0] = (char*)malloc(SLESBufferSize * 2); // 2 buffers
	SLESBuffer[1] = SLESBuffer[0] + SLESBufferSize;
	memset(SLESBuffer[0], 0, SLESBufferSize * 2);
	SLESBufferNum = 0;
	SLESWasActivated = true;

	return ((SL_RESULT_SUCCESS == slCreateEngine(&SLESEngine, 0, NULL, 0, NULL, NULL))
	     && (SL_RESULT_SUCCESS == (*SLESEngine)->Realize(SLESEngine, SL_BOOLEAN_FALSE))
	     && (SL_RESULT_SUCCESS == (*SLESEngine)->GetInterface(SLESEngine, SL_IID_ENGINE, &engineEngine))
	     && (SL_RESULT_SUCCESS == (*engineEngine)->CreateOutputMix(engineEngine, &SLESOutMix, 0, 0, 0))
	     && (SL_RESULT_SUCCESS == (*SLESOutMix)->Realize(SLESOutMix, SL_BOOLEAN_FALSE))
	     && (NULL != (loc_outmix.outputMix = SLESOutMix))
	     && (SL_RESULT_SUCCESS == (*engineEngine)->CreateAudioPlayer(engineEngine, &SLESPlayer, &audioSrc, &audioSnk, 1, ids, req))
	     && (SL_RESULT_SUCCESS == (*SLESPlayer)->Realize(SLESPlayer, SL_BOOLEAN_FALSE))
	     && (SL_RESULT_SUCCESS == (*SLESPlayer)->GetInterface(SLESPlayer, SL_IID_PLAY, &playerPlay))
	     && (SL_RESULT_SUCCESS == (*SLESPlayer)->GetInterface(SLESPlayer, SL_IID_BUFFERQUEUE, &bufferQueue))
	     && (SL_RESULT_SUCCESS == (*bufferQueue)->RegisterCallback(bufferQueue, SLESPlayerCallback, NULL))
	     && (SL_RESULT_SUCCESS == (*playerPlay)->SetPlayState(playerPlay, SL_PLAYSTATE_PLAYING))
	     && (SL_RESULT_SUCCESS == (*bufferQueue)->Enqueue(bufferQueue, SLESBuffer[0], SLESBufferSize))
	     && (SL_RESULT_SUCCESS == (*bufferQueue)->Enqueue(bufferQueue, SLESBuffer[1], SLESBufferSize)));
}

static void SLESShutdown()
{
	if (SLESPlayer) { (*SLESPlayer)->Destroy(SLESPlayer); SLESPlayer = NULL; }
	if (SLESOutMix) { (*SLESOutMix)->Destroy(SLESOutMix); SLESOutMix = NULL; }
	if (SLESEngine) { (*SLESEngine)->Destroy(SLESEngine); SLESEngine = NULL; }
	if (SLESBuffer[0]) { free(SLESBuffer[0]); SLESBuffer[0] = NULL; }
}

bool ZL_AudioOpen(unsigned int buffer_length)
{
	//__ANDROID_LOG_PRINT_INFO("ZillaActivityNative", "ZL_AudioOpen");
	if (SLESBufferSize) SLESShutdown();
	SLESBufferSize = buffer_length * 4;  //16 bit, stereo
	return SLESInit();
}

void* ZL_AudioAndroidLoad(ZL_File_Impl* file_impl)
{
	if (!file_impl || file_impl->archive != ZL_ImplFromOwner<ZL_FileContainer_Impl>(ZL_File::DefaultReadFileContainer)) { return NULL; }
	ZL_RWopsZIP* rwopszip = (ZL_RWopsZIP*)file_impl->src;
	if (!rwopszip->is_stored_raw()) { return NULL; }
	if (JavaAudio == NULL)
	{
		//__ANDROID_LOG_PRINT_INFO("ZillaActivityNative", "ZL_AudioAndroidLoad - Call startAudio");
		JavaAudio = jniEnv->NewGlobalRef(jniEnv->CallObjectMethod(JavaZillaActivity, jniEnv->GetMethodID(jniEnv->GetObjectClass(JavaZillaActivity), "startAudio", "()Ljava/lang/Object;")));
		jclass JavaAudioClass = jniEnv->GetObjectClass(JavaAudio);
		JavaAudioOpen = jniEnv->GetMethodID(JavaAudioClass, "open", "(II)Ljava/lang/Object;");
		JavaAudioControl = jniEnv->GetMethodID(JavaAudioClass, "control", "(Ljava/lang/Object;II)V");
	}
	return jniEnv->NewGlobalRef(jniEnv->CallObjectMethod(JavaAudio, JavaAudioOpen, (jint)rwopszip->get_data_offset(), (jint)rwopszip->size()));
}

void ZL_AudioAndroidRelease(void* stream)           { jniEnv->CallVoidMethod(JavaAudio, JavaAudioControl, (jobject)stream, (jint)9, (jint)0); jniEnv->DeleteGlobalRef((jobject)stream); }
void ZL_AudioAndroidPlay(void* stream, bool looped) { jniEnv->CallVoidMethod(JavaAudio, JavaAudioControl, (jobject)stream, (jint)0, (jint)looped); }
void ZL_AudioAndroidStop(void* stream)              { jniEnv->CallVoidMethod(JavaAudio, JavaAudioControl, (jobject)stream, (jint)1, (jint)0); }
void ZL_AudioAndroidPause(void* stream)             { jniEnv->CallVoidMethod(JavaAudio, JavaAudioControl, (jobject)stream, (jint)2, (jint)0); }
void ZL_AudioAndroidResume(void* stream)            { jniEnv->CallVoidMethod(JavaAudio, JavaAudioControl, (jobject)stream, (jint)3, (jint)0); }

// Customizing this unused C++ exception related function saves 48064 bytes in the output file!
extern "C" void* __cxa_demangle() {return 0;}

#endif
