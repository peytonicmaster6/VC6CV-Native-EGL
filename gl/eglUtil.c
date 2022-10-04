#include "eglUtil.h"
#include <stdio.h>


int x_;
int y_;
int width_;
int height_;
EGLDisplay egl_display_;
Display *display_;
	
int setupEGL(EGL_Setup *setup, Window window)
{

	static const EGLint attribs[] =
		{
			EGL_RED_SIZE, 1,
			EGL_GREEN_SIZE, 1,
			EGL_BLUE_SIZE, 1,
			EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
			EGL_NONE
		};
	
	setup->display = eglGetDisplay(display_);
	
	if (!eglInitialize(setup->display, &setup->versionMajor, &setup->versionMinor))
		printf("eglInitialize() failed\n");
		
	EGLConfig config;
	EGLint num_configs;
	
	if (!eglChooseConfig(setup->display, attribs, &config, 1, &num_configs))
		printf("couldn't get an EGL visual config\n");

	EGLint vid;
	if (!eglGetConfigAttrib(setup->display, config, EGL_NATIVE_VISUAL_ID, &vid))
		printf("eglGetConfigAttrib() failed\n");

	eglBindAPI(EGL_OPENGL_ES_API);

	static const EGLint ctx_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};
	setup->context = eglCreateContext(setup->display, config, EGL_NO_CONTEXT, ctx_attribs);
	if (!setup->context)
		printf("eglCreateContext failed\n");

	setup->surface = eglCreateWindowSurface(setup->display, config, reinterpret_cast<EGLNativeWindowType>(window), NULL);
	if (!setup->surface)
		printf("eglCreateWindowSurface failed\n");

	// We have to do eglMakeCurrent in the thread where it will run, but we must do it
	// here temporarily so as to get the maximum texture size.
	eglMakeCurrent(setup->display, EGL_NO_SURFACE, EGL_NO_SURFACE, setup->context);
	int max_texture_size = 0;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
	//max_image_width_ = max_image_height_ = max_texture_size;
	// This "undoes" the previous eglMakeCurrent.
	eglMakeCurrent(setup->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

void terminateEGL(EGL_Setup *setup)
{
	eglMakeCurrent(setup->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(setup->display, setup->context);
	eglDestroySurface(setup->display, setup->surface);
	eglTerminate(setup->display);
}

/* Create native window (basically just a rectangle on the screen we can render to */
int createNativeWindow(Window *window, char const *name, int width, int height)
{
	int screen_num = DefaultScreen(setup->display);
	XSetWindowAttributes attr;
	unsigned long mask;
	Window root = RootWindow(setup->display, screen_num);
	int screen_width = DisplayWidth(setup->display, screen_num);
	int screen_height = DisplayHeight(setup->display, screen_num);

	// Default behaviour here is to use a 1024x768 window.
	if (width == 0 || height == 0)
	{
		width = 1024;
		height = 768;
	}
	
	XVisualInfo visTemplate = {};
	visTemplate.visualid = (VisualID)&vid;
	int num_visuals;
	XVisualInfo *visinfo = XGetVisualInfo(setup->display, VisualIDMask, &visTemplate, &num_visuals);

	/* window attributes */
	attr.background_pixel = 0;
	attr.border_pixel = 0;
	attr.colormap = XCreateColormap(setup->display, root, visinfo->visual, AllocNone);
	attr.event_mask = StructureNotifyMask | ExposureMask | KeyPressMask;
	/* XXX this is a bad way to get a borderless window! */
	mask = CWBackPixel | CWBorderPixel | CWColormap | CWEventMask;

	window = XCreateWindow(setup->display, root, screen_width, screen_height, width, height, 0, visinfo->depth, InputOutput, visinfo->visual,
							mask, &attr);

	/* set hints and properties */
	{
		XSizeHints sizehints;
		sizehints.x = width;
		sizehints.y = height;
		sizehints.width = width;
		sizehints.height = height;
		sizehints.flags = USSize | USPosition;
		XSetNormalHints(setup->display, &window, &sizehints);
		XSetStandardProperties(setup->display, window, name, name, None, (char **)NULL, 0, &sizehints);
	}

	XFree(visinfo);

	XMapWindow(setup->display, window);

	// This stops the window manager from closing the window, so we get an event instead.
	wm_delete_window_ = XInternAtom(setup->display, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(setup->display, window, &wm_delete_window_, 1);
	
	return 0;
}
