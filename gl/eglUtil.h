#ifndef EGLUTIL_H
#define EGLUTIL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <epoxy/egl.h>
#include <epoxy/gl.h>

#include "bcm_host.h"

#include "applog.h"

#define CHECK_EVAL(EVAL, MSG, ERRHANDLER) \
	if (!(EVAL)) { \
		vcos_log_error(MSG); \
		goto ERRHANDLER; \
	}

typedef struct EGL_Setup
{
	EGLDisplay display;
	EGLSurface surface;
	EGLContext context;
	int versionMinor;
	int versionMajor;
} EGL_Setup;

int setupEGL(EGL_Setup *setup, Window window);

void terminateEGL(EGL_Setup *setup);

/* Create native window (basically just a rectangle on the screen we can render to */
int createNativeWindow(Window *window, char const *name, int width, int height);

#ifdef __cplusplus
}
#endif

#endif
