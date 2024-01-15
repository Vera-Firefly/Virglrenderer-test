// test based on https://github.com/matusnovak/rpi-opengl-without-x/blob/master/triangle.c
// clang egltest.c -lEGL build/vtest/libvirgl_test_server.so -o egltest

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static const EGLint configAttribs[] = {
    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_BLUE_SIZE, 8, EGL_GREEN_SIZE, 8,
    EGL_RED_SIZE, 8, EGL_DEPTH_SIZE, 8,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};

// Width and height of the desired framebuffer
static const EGLint pbufferAttribs[] = {
    EGL_WIDTH,
    1280,
    EGL_HEIGHT,
    720,
    EGL_NONE,
};

static const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3,
                                        EGL_NONE};

static const char *eglGetErrorStr()
{
    switch (eglGetError())
    {
    case EGL_SUCCESS:
        return "The last function succeeded without error.";
    case EGL_NOT_INITIALIZED:
        return "EGL is not initialized, or could not be initialized, for the "
               "specified EGL display connection.";
    case EGL_BAD_ACCESS:
        return "EGL cannot access a requested resource (for example a context "
               "is bound in another thread).";
    case EGL_BAD_ALLOC:
        return "EGL failed to allocate resources for the requested operation.";
    case EGL_BAD_ATTRIBUTE:
        return "An unrecognized attribute or attribute value was passed in the "
               "attribute list.";
    case EGL_BAD_CONTEXT:
        return "An EGLContext argument does not name a valid EGL rendering "
               "context.";
    case EGL_BAD_CONFIG:
        return "An EGLConfig argument does not name a valid EGL frame buffer "
               "configuration.";
    case EGL_BAD_CURRENT_SURFACE:
        return "The current surface of the calling thread is a window, pixel "
               "buffer or pixmap that is no longer valid.";
    case EGL_BAD_DISPLAY:
        return "An EGLDisplay argument does not name a valid EGL display "
               "connection.";
    case EGL_BAD_SURFACE:
        return "An EGLSurface argument does not name a valid surface (window, "
               "pixel buffer or pixmap) configured for GL rendering.";
    case EGL_BAD_MATCH:
        return "Arguments are inconsistent (for example, a valid context "
               "requires buffers not supplied by a valid surface).";
    case EGL_BAD_PARAMETER:
        return "One or more argument values are invalid.";
    case EGL_BAD_NATIVE_PIXMAP:
        return "A NativePixmapType argument does not refer to a valid native "
               "pixmap.";
    case EGL_BAD_NATIVE_WINDOW:
        return "A NativeWindowType argument does not refer to a valid native "
               "window.";
    case EGL_CONTEXT_LOST:
        return "A power management event has occurred. The application must "
               "destroy all contexts and reinitialise OpenGL ES state and "
               "objects to continue rendering.";
    default:
        break;
    }
    return "Unknown error!";
}

int vtest_main(int argc, char **argv);
int main(int argc, char **argv)
{
    EGLDisplay display;
    int major, minor;
    int desiredWidth, desiredHeight;
    GLuint program, vert, frag, vbo;
    GLint posLoc, colorLoc, result;

    if ((display = eglGetDisplay(EGL_DEFAULT_DISPLAY)) == EGL_NO_DISPLAY)
    {
        fprintf(stderr, "Failed to get EGL display! Error: %s\n",
                eglGetErrorStr());
        return EXIT_FAILURE;
    }

    if (eglInitialize(display, &major, &minor) == EGL_FALSE)
    {
        fprintf(stderr, "Failed to get EGL version! Error: %s\n",
                eglGetErrorStr());
        eglTerminate(display);
        return EXIT_FAILURE;
    }

    printf("Initialized EGL version: %d.%d\n", major, minor);

    EGLint numConfigs;
    EGLConfig config;
    if (!eglChooseConfig(display, configAttribs, &config, 1, &numConfigs))
    {
        fprintf(stderr, "Failed to get EGL config! Error: %s\n",
                eglGetErrorStr());
        eglTerminate(display);
        return EXIT_FAILURE;
    }

    EGLSurface surface =
        eglCreatePbufferSurface(display, config, pbufferAttribs);
    if (surface == EGL_NO_SURFACE)
    {
        fprintf(stderr, "Failed to create EGL surface! Error: %s\n",
                eglGetErrorStr());
        eglTerminate(display);
        return EXIT_FAILURE;
    }

    eglBindAPI(EGL_OPENGL_API);

    EGLContext context =
        eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
    if (context == EGL_NO_CONTEXT)
    {
        fprintf(stderr, "Failed to create EGL context! Error: %s\n",
                eglGetErrorStr());
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return EXIT_FAILURE;
    }

    eglMakeCurrent(display, surface, surface, context);

    // init vtest
    vtest_main(argc, argv);

    // Cleanup
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    eglTerminate(display);
    return EXIT_SUCCESS;
}
