/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2014 Sam Lantinga <slouken@libsdl.org>

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
#include "../../SDL_internal.h"

#ifdef SDL_LOADSO_DLOPEN

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* System dependent library loading routines                           */

#include <stdio.h>
#include <dlfcn.h>

#include "SDL_loadso.h"

void *
SDL_LoadObject(const char *sofile)
{
    void *handle = dlopen(sofile, RTLD_NOW|RTLD_LOCAL);
    const char *loaderror = (char *) dlerror();
    if (handle == NULL) {
        SDL_SetError("Failed loading %s: %s", sofile, loaderror);
    }
    return (handle);
}

void *
SDL_LoadFunction(void *handle, const char *name)
{
    void *symbol = dlsym(handle, name);
    if (symbol == NULL) {
        /* append an underscore for platforms that need that. */
        size_t len = 1 + SDL_strlen(name) + 1;
        char *_name = SDL_stack_alloc(char, len);
        _name[0] = '_';
        SDL_strlcpy(&_name[1], name, len);
        symbol = dlsym(handle, _name);
        SDL_stack_free(_name);
        if (symbol == NULL) {
            SDL_SetError("Failed loading %s: %s", name,
                         (const char *) dlerror());
        }
    }
    return (symbol);
}

void
SDL_UnloadObject(void *handle)
{
    if (handle != NULL) {
        dlclose(handle);
    }
}

#endif /* SDL_LOADSO_DLOPEN */

/* vi: set ts=4 sw=4 expandtab: */
