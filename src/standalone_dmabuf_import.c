#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <wayland-util.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <xdg-shell.h>
#include <wayland-egl-core.h>
#include <wayland-egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "egl.h"

#define PRINT_DRM_FORMAT(drm_format) \
    ((drm_format) >>  0) & 0xff, \
    ((drm_format) >>  8) & 0xff, \
    ((drm_format) >> 16) & 0xff, \
    ((drm_format) >> 24) & 0xff

typedef struct standalone_ctx {
    struct wl_display * display;
    struct wl_registry * registry;

    struct wl_compositor * compositor;
    struct xdg_wm_base * xdg_wm_base;
    uint32_t compositor_id;
    uint32_t xdg_wm_base_id;

    struct wl_surface * surface;
    struct xdg_surface * xdg_surface;
    struct xdg_toplevel * xdg_toplevel;

    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLConfig egl_config;
    EGLSurface egl_surface;
    struct wl_egl_window * egl_window;
    GLuint egl_vbo;
    GLuint egl_texture;
    GLuint egl_shader_program;
    EGLAttrib * egl_image_attribs;

    uint32_t last_surface_serial;
    bool xdg_surface_configured;
    bool xdg_toplevel_configured;
    bool configured;
    bool closing;
} standalone_ctx_t;

static void exit_fail(standalone_ctx_t * ctx) {
    exit(1);
}

static void ignore() {}

// --- wl_registry event handlers ---

static void registry_event_add(
    void * data, struct wl_registry * registry,
    uint32_t id, const char * interface, uint32_t version
) {
    standalone_ctx_t * ctx = (standalone_ctx_t *)data;
    printf("[registry][+] id=%08x %s v%d\n", id, interface, version);

    if (strcmp(interface, "wl_compositor") == 0) {
        if (ctx->compositor != NULL) {
            printf("[!] wl_registry: duplicate compositor\n");
            exit_fail(ctx);
        }

        ctx->compositor = (struct wl_compositor *)wl_registry_bind(registry, id, &wl_compositor_interface, 4);
        ctx->compositor_id = id;
    } else if (strcmp(interface, "xdg_wm_base") == 0) {
        if (ctx->xdg_wm_base != NULL) {
            printf("[!] wl_registry: duplicate xdg_wm_base\n");
            exit_fail(ctx);
        }

        ctx->xdg_wm_base = (struct xdg_wm_base *)wl_registry_bind(registry, id, &xdg_wm_base_interface, 2);
        ctx->xdg_wm_base_id = id;
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_event_add,
    .global_remove = ignore
};

// --- configure callbacks ---

static void surface_configure_finished(standalone_ctx_t * ctx) {
    printf("[info] acknowledging configure\n");
    xdg_surface_ack_configure(ctx->xdg_surface, ctx->last_surface_serial);

    printf("[info] committing surface\n");
    wl_surface_commit(ctx->surface);

    ctx->xdg_surface_configured = false;
    ctx->xdg_toplevel_configured = false;
    ctx->configured = true;
}

// --- xdg_surface event handlers ---

static void xdg_surface_event_configure(
    void * data, struct xdg_surface * xdg_surface, uint32_t serial
) {
    standalone_ctx_t * ctx = (standalone_ctx_t *)data;
    printf("[xdg_surface] configure %d\n", serial);

    ctx->last_surface_serial = serial;
    ctx->xdg_surface_configured = true;
    if (ctx->xdg_surface_configured && ctx->xdg_toplevel_configured) {
        surface_configure_finished(ctx);
    }
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_event_configure,
};

// --- xdg_toplevel event handlers ---

static void xdg_toplevel_event_configure(
    void * data, struct xdg_toplevel * xdg_toplevel,
    int32_t width, int32_t height, struct wl_array * states
) {
    standalone_ctx_t * ctx = (standalone_ctx_t *)data;
    printf("[xdg_toplevel] configure width=%d, height=%d\n", width, height);

    wl_egl_window_resize(ctx->egl_window, width, height, 0, 0);
    glBindTexture(GL_TEXTURE_2D, ctx->egl_texture);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    ctx->xdg_toplevel_configured = true;
    if (ctx->xdg_surface_configured && ctx->xdg_toplevel_configured) {
        surface_configure_finished(ctx);
    }
}

static void xdg_toplevel_event_close(
    void * data, struct xdg_toplevel * xdg_toplevel
) {
    standalone_ctx_t * ctx = (standalone_ctx_t *)data;
    ctx->closing = true;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_event_configure,
    .close = xdg_toplevel_event_close
};

static const float vertex_array[] = {
    -1.0, -1.0, 0.0, 1.0,
     1.0, -1.0, 1.0, 1.0,
    -1.0,  1.0, 0.0, 0.0,
    -1.0,  1.0, 0.0, 0.0,
     1.0, -1.0, 1.0, 1.0,
     1.0,  1.0, 1.0, 0.0
};

static const char * vertex_shader =
    "#version 100\n"
    "precision mediump float;\n"
    "\n"
    "attribute vec2 aPosition;\n"
    "attribute vec2 aTexCoord;\n"
    "varying vec2 vTexCoord;\n"
    "\n"
    "void main() {\n"
    "    gl_Position = vec4(aPosition, 0.0, 1.0);\n"
    "    vTexCoord = aTexCoord;\n"
    "}\n"
;

static const char * fragment_shader =
    "#version 100\n"
    "precision mediump float;\n"
    "\n"
    "uniform sampler2D uTexture;\n"
    "varying vec2 vTexCoord;\n"
    "\n"
    "void main() {\n"
    "    vec4 color = texture2D(uTexture, vTexCoord);\n"
    "    gl_FragColor = vec4(color.rgb, 1.0);\n"
    "}\n"
;

static const EGLAttrib fd_attribs[] = {
    EGL_DMA_BUF_PLANE0_FD_EXT,
    EGL_DMA_BUF_PLANE1_FD_EXT,
    EGL_DMA_BUF_PLANE2_FD_EXT,
    EGL_DMA_BUF_PLANE3_FD_EXT
};

static const EGLAttrib offset_attribs[] = {
    EGL_DMA_BUF_PLANE0_OFFSET_EXT,
    EGL_DMA_BUF_PLANE1_OFFSET_EXT,
    EGL_DMA_BUF_PLANE2_OFFSET_EXT,
    EGL_DMA_BUF_PLANE3_OFFSET_EXT
};

static const EGLAttrib stride_attribs[] = {
    EGL_DMA_BUF_PLANE0_PITCH_EXT,
    EGL_DMA_BUF_PLANE1_PITCH_EXT,
    EGL_DMA_BUF_PLANE2_PITCH_EXT,
    EGL_DMA_BUF_PLANE3_PITCH_EXT
};

static const EGLAttrib modifier_low_attribs[] = {
    EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
    EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
    EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
    EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT
};

static const EGLAttrib modifier_high_attribs[] = {
    EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
    EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
    EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
    EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT
};

standalone_ctx_t * standalone_dmabuf_import_init() {
    printf("[info] allocating context\n");
    standalone_ctx_t * ctx = malloc(sizeof (standalone_ctx_t));
    ctx->display = NULL;
    ctx->registry = NULL;

    ctx->compositor = NULL;
    ctx->compositor_id = 0;
    ctx->xdg_wm_base = NULL;
    ctx->xdg_wm_base_id = 0;

    ctx->surface = NULL;
    ctx->xdg_surface = NULL;
    ctx->xdg_toplevel = NULL;

    ctx->egl_display = EGL_NO_DISPLAY;
    ctx->egl_context = EGL_NO_CONTEXT;
    ctx->egl_surface = EGL_NO_SURFACE;
    ctx->egl_window = EGL_NO_SURFACE;
    ctx->egl_vbo = -1;
    ctx->egl_texture = -1;
    ctx->egl_shader_program = -1;
    ctx->egl_image_attribs = NULL;

    ctx->last_surface_serial = 0;
    ctx->xdg_surface_configured = false;
    ctx->xdg_toplevel_configured = false;
    ctx->configured = false;
    ctx->closing = false;

    if (ctx == NULL) {
        printf("[!] malloc: allocating context failed\n");
        exit_fail(ctx);
    }

    printf("[info] connecting to display\n");
    ctx->display = wl_display_connect(NULL);
    if (ctx->display == NULL) {
        printf("[!] wl_display: connect failed\n");
        exit_fail(ctx);
    }

    printf("[info] getting registry\n");
    ctx->registry = wl_display_get_registry(ctx->display);
    wl_registry_add_listener(ctx->registry, &registry_listener, (void *)ctx);

    printf("[info] waiting for events\n");
    wl_display_roundtrip(ctx->display);

    printf("[info] checking if protocols found\n");
    if (ctx->compositor == NULL) {
        printf("[!] wl_registry: no compositor found\n");
        exit_fail(ctx);
    } else if (ctx->xdg_wm_base == NULL) {
        printf("[!] wl_registry: no xdg_wm_base found\n");
        exit_fail(ctx);
    }

    printf("[info] creating surface\n");
    ctx->surface = wl_compositor_create_surface(ctx->compositor);
    if (ctx->surface == NULL) {
        printf("[!] wl_compositor: failed to create surface\n");
        exit_fail(ctx);
    }

    printf("[info] creating xdg_surface\n");
    ctx->xdg_surface = xdg_wm_base_get_xdg_surface(ctx->xdg_wm_base, ctx->surface);
    if (ctx->xdg_surface == NULL) {
        printf("[!] xdg_wm_base: failed to create xdg_surface\n");
        exit_fail(ctx);
    }
    xdg_surface_add_listener(ctx->xdg_surface, &xdg_surface_listener, (void *)ctx);

    printf("[info] creating xdg_toplevel\n");
    ctx->xdg_toplevel = xdg_surface_get_toplevel(ctx->xdg_surface);
    if (ctx->xdg_toplevel == NULL) {
        printf("[!] xdg_surface: failed to create xdg_toplevel\n");
        exit_fail(ctx);
    }
    xdg_toplevel_add_listener(ctx->xdg_toplevel, &xdg_toplevel_listener, (void *)ctx);

    printf("[info] setting xdg_toplevel properties\n");
    xdg_toplevel_set_app_id(ctx->xdg_toplevel, "example");
    xdg_toplevel_set_title(ctx->xdg_toplevel, "example window");

    printf("[info] committing surface to trigger configure events\n");
    wl_surface_commit(ctx->surface);

    printf("[info] waiting for events\n");
    wl_display_roundtrip(ctx->display);

    printf("[info] checking if surface configured\n");
    if (!ctx->configured) {
        printf("[!] xdg_surface: surface not configured\n");
        exit_fail(ctx);
    }

    printf("[info] creating EGL display\n");
    ctx->egl_display = eglGetDisplay((EGLNativeDisplayType)ctx->display);
    if (ctx->egl_display == EGL_NO_DISPLAY) {
        printf("[!] eglGetDisplay: failed to create EGL display\n");
        exit_fail(ctx);
    }

    EGLint major, minor;
    printf("[info] initializing EGL display\n");
    if (eglInitialize(ctx->egl_display, &major, &minor) != EGL_TRUE) {
        printf("[!] eglGetDisplay: failed to initialize EGL display\n");
        exit_fail(ctx);
    }
    printf("[info] initialized EGL %d.%d\n", major, minor);

    EGLint num_configs;
    printf("[info] getting number of EGL configs\n");
    if (eglGetConfigs(ctx->egl_display, NULL, 0, &num_configs) != EGL_TRUE) {
        printf("[!] eglGetConfigs: failed to get number of EGL configs\n");
        exit_fail(ctx);
    }

    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
    printf("[info] getting EGL config\n");
    if (eglChooseConfig(ctx->egl_display, config_attribs, &ctx->egl_config, 1, &num_configs) != EGL_TRUE) {
        printf("[!] eglChooseConfig: failed to get EGL config\n");
        exit_fail(ctx);
    }

    printf("[info] creating EGL window\n");
    ctx->egl_window = wl_egl_window_create(ctx->surface, 100, 100);
    if (ctx->egl_window == EGL_NO_SURFACE) {
        printf("[!] wl_egl_window: failed to create EGL window\n");
        exit_fail(ctx);
    }

    printf("[info] creating EGL surface\n");
    ctx->egl_surface = eglCreateWindowSurface(ctx->egl_display, ctx->egl_config, ctx->egl_window, NULL);

    EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    printf("[info] creating EGL context\n");
    ctx->egl_context = eglCreateContext(ctx->egl_display, ctx->egl_config, EGL_NO_CONTEXT, context_attribs);
    if (ctx->egl_context == EGL_NO_CONTEXT) {
        printf("[!] eglCreateContext: failed to create EGL context\n");
        exit_fail(ctx);
    }

    printf("[info] activating EGL context\n");
    if (eglMakeCurrent(ctx->egl_display, ctx->egl_surface, ctx->egl_surface, ctx->egl_context) != EGL_TRUE) {
        printf("[!] eglMakeCurrent: failed to activate EGL context\n");
        exit_fail(ctx);
    }

    printf("[info] create vertex buffer object\n");
    glGenBuffers(1, &ctx->egl_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, ctx->egl_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof vertex_array, vertex_array, GL_STATIC_DRAW);

    printf("[info] create texture and set scaling mode\n");
    glGenTextures(1, &ctx->egl_texture);
    glBindTexture(GL_TEXTURE_2D, ctx->egl_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    printf("[info] error log for shader compilation error messages\n");
    GLint success;
    const char * shader_source = NULL;
    char errorLog[1024] = { 0 };

    printf("[info] compile vertex shader\n");
    shader_source = vertex_shader;
    GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex_shader, 1, &shader_source, NULL);
    glCompileShader(vertex_shader);
    glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &success);
    if (success != GL_TRUE) {
        glGetShaderInfoLog(vertex_shader, sizeof errorLog, NULL, errorLog);
        errorLog[strcspn(errorLog, "\n")] = '\0';
        printf("[error] failed to compile vertex shader: %s\n", errorLog);
        glDeleteShader(vertex_shader);
        exit_fail(ctx);
    }

    printf("[info] compile fragment shader\n");
    shader_source = fragment_shader;
    GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment_shader, 1, &shader_source, NULL);
    glCompileShader(fragment_shader);
    glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &success);
    if (success != GL_TRUE) {
        glGetShaderInfoLog(fragment_shader, sizeof errorLog, NULL, errorLog);
        errorLog[strcspn(errorLog, "\n")] = '\0';
        printf("[error] failed to compile fragment shader: %s\n", errorLog);
        glDeleteShader(vertex_shader);
        glDeleteShader(fragment_shader);
        exit_fail(ctx);
    }

    printf("[info] create shader program and get pointers to shader uniforms\n");
    ctx->egl_shader_program = glCreateProgram();
    glAttachShader(ctx->egl_shader_program, vertex_shader);
    glAttachShader(ctx->egl_shader_program, fragment_shader);
    glLinkProgram(ctx->egl_shader_program);
    glGetProgramiv(ctx->egl_shader_program, GL_LINK_STATUS, &success);
    if (success != GL_TRUE) {
        printf("[error] failed to link shader program\n");
        glDeleteShader(vertex_shader);
        glDeleteShader(fragment_shader);
        glDeleteProgram(ctx->egl_shader_program);
        exit_fail(ctx);
    }
    glUseProgram(ctx->egl_shader_program);
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    printf("[info] set GL clear color to back and set GL vertex layout\n");
    glClearColor(0.0, 0.0, 0.0, 1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof (float), (void *)(0 * sizeof (float)));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof (float), (void *)(2 * sizeof (float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    printf("[info] clearing frame\n");
    glClearColor(1.0, 1.0, 0.0, 1.0);
    glClear(GL_COLOR_BUFFER_BIT);
    glFlush();

    printf("[info] swapping buffers\n");
    if (eglSwapBuffers(ctx->egl_display, ctx->egl_surface) != EGL_TRUE) {
        printf("[!] eglSwapBuffers: failed to swap buffers\n");
        exit_fail(ctx);
    }


    printf("[info] clearing screen\n");
    glBindTexture(GL_TEXTURE_2D, ctx->egl_texture);
    glClear(GL_COLOR_BUFFER_BIT);

    printf("[info] swapping buffers\n");
    if (eglSwapBuffers(ctx->egl_display, ctx->egl_surface) != EGL_TRUE) {
        printf("[!] eglSwapBuffers: failed to swap buffers\n");
        exit_fail(ctx);
    }

    printf("[info] committing surface\n");
    wl_surface_commit(ctx->surface);

    return ctx;
}

void standalone_dmabuf_import_dispatch(standalone_ctx_t * ctx) {
    eglMakeCurrent(ctx->egl_display, ctx->egl_surface, ctx->egl_surface, ctx->egl_context);
    wl_display_dispatch(ctx->display);
}

void standalone_dmabuf_import_to_texture(EGLDisplay egl_display, GLuint egl_texture, dmabuf_t * dmabuf) {
    int i = 0;
    size_t planes = dmabuf->planes;
    EGLAttrib * image_attribs = malloc((6 + 10 * planes + 1) * sizeof (EGLAttrib));
    if (image_attribs == NULL) {
        printf("[error] failed to allocate EGL image attribs\n");
        exit(1);
    }

    image_attribs[i++] = EGL_WIDTH;
    image_attribs[i++] = dmabuf->width;
    image_attribs[i++] = EGL_HEIGHT;
    image_attribs[i++] = dmabuf->height;
    image_attribs[i++] = EGL_LINUX_DRM_FOURCC_EXT;
    image_attribs[i++] = dmabuf->drm_format;

    uint64_t modifier = dmabuf->modifier;
    //uint64_t modifier = dmabuf->modifiers[0];
    printf("[info] dmabuf %dx%d@%c%c%c%c with modifier %lx\n", dmabuf->width, dmabuf->height, PRINT_DRM_FORMAT(dmabuf->drm_format), modifier);

    for (size_t plane = 0; plane < planes; plane++) {
        image_attribs[i++] = fd_attribs[plane];
        image_attribs[i++] = dmabuf->fds[plane];
        image_attribs[i++] = offset_attribs[plane];
        image_attribs[i++] = dmabuf->offsets[plane];
        image_attribs[i++] = stride_attribs[plane];
        image_attribs[i++] = dmabuf->strides[plane];
        image_attribs[i++] = modifier_low_attribs[plane];
        image_attribs[i++] = modifier;
        image_attribs[i++] = modifier_high_attribs[plane];
        image_attribs[i++] = modifier >> 32;

        printf("[info] plane %zd: offset %d, stride %d\n", plane, dmabuf->offsets[plane], dmabuf->strides[plane]);
    }

    image_attribs[i++] = EGL_NONE;

    glViewport(0, 0, dmabuf->width, dmabuf->height);

    printf("[info] import dmabuf\n");
    // create EGLImage from dmabuf with attribute array
    EGLImage frame_image = eglCreateImage(egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, image_attribs);
    free(image_attribs);

    if (frame_image == EGL_NO_IMAGE) {
        printf("[error] error = %x\n", eglGetError());
        exit(1);
    }

    // convert EGLImage to GL texture
    glBindTexture(GL_TEXTURE_2D, egl_texture);
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, frame_image);

    // destroy temporary image
    eglDestroyImage(egl_display, frame_image);
}

void standalone_dmabuf_import_render(standalone_ctx_t * ctx, dmabuf_t * dmabuf) {
    eglMakeCurrent(ctx->egl_display, ctx->egl_surface, ctx->egl_surface, ctx->egl_context);
    standalone_dmabuf_import_to_texture(ctx->egl_display, ctx->egl_texture, dmabuf);

    printf("[info] drawing texture\n");
    glBindTexture(GL_TEXTURE_2D, ctx->egl_texture);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    printf("[info] swapping buffers\n");
    if (eglSwapBuffers(ctx->egl_display, ctx->egl_surface) != EGL_TRUE) {
        printf("[!] eglSwapBuffers: failed to swap buffers\n");
        exit_fail(ctx);
    }

    printf("[info] committing surface\n");
    wl_surface_commit(ctx->surface);
}
