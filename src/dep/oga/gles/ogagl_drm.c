/**
** Copyright (C) 2019 Akop Karapetyan
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
** http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
**/

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm_fourcc.h>
#include <gbm.h>
//#include <EGL/egl.h>
//#include <GLES2/gl2.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdbool.h>

#include <go2/display.h>
#include <go2/input.h>
#include <go2/audio.h>

#include "ogagl.h"

#define EGL_EGLEXT_PROTOTYPES
//#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>

uint32_t pigl_screen_width = 0;
uint32_t pigl_screen_height = 0;

static int device = -1;
static uint32_t connector_id;
static struct gbm_device *gbm_device = NULL;
static struct gbm_surface *gbm_surface = NULL;
static EGLContext context = EGL_NO_CONTEXT;
static EGLDisplay display = EGL_NO_DISPLAY;
static EGLSurface egl_surface = EGL_NO_SURFACE;

static drmModeModeInfo mode_info;
static drmModeCrtc *crtc = NULL;

static struct gbm_bo *previous_bo = NULL;
static uint32_t previous_fb;


uint32_t drmFourCC;
uint32_t crtc_id;
uint32_t fb_id;
struct drm_mode_create_dumb args = {0};

#define BUFFER_MAX (3)

typedef struct go2_display
{
    int fd;
    uint32_t connector_id;
    drmModeModeInfo mode;
    uint32_t width;
    uint32_t height;
    uint32_t crtc_id;
} go2_display_t;

typedef struct go2_surface
{
    go2_display_t* display;
    uint32_t gem_handle;
    uint64_t size;
    int width;
    int height;
    int stride;
    uint32_t format;
    int prime_fd;
    bool is_mapped;
    uint8_t* map;
} go2_surface_t;

typedef struct buffer_surface_pair
{
    struct gbm_bo* gbmBuffer;
    go2_surface_t* surface;
} buffer_surface_pair_t;

typedef struct go2_context
{
    go2_display_t* display;    
    int width;
    int height;
    go2_context_attributes_t attributes;
    struct gbm_device* gbmDevice;
    EGLDisplay eglDisplay;
    struct gbm_surface* gbmSurface;
    EGLSurface eglSurface;
    EGLContext eglContext;
    uint32_t drmFourCC;
    buffer_surface_pair_t bufferMap[BUFFER_MAX];
    int bufferCount;
} go2_context_t;


//static SDL_Surface* sdlscreen = NULL;

go2_display_t* go2_display = NULL;
go2_surface_t* go2_surface = NULL;
go2_presenter_t* go2_presenter = NULL;
go2_context_t* go2_context3D = NULL;

static drmModeConnector *find_connector(drmModeRes *resources)
{
    int i;
    for (i = 0; i < resources->count_connectors; i++) {
        drmModeConnector *connector = drmModeGetConnector(device, resources->connectors[i]);
        if (connector->connection == DRM_MODE_CONNECTED)
            return connector;
        drmModeFreeConnector(connector);
    }

    return NULL;
}

static drmModeEncoder *find_encoder (drmModeRes *resources, drmModeConnector *connector)
{
    if (connector->encoder_id)
        return drmModeGetEncoder (device, connector->encoder_id);
    return NULL;
}

static int find_display_configuration()
{
    drmModeRes *resources = drmModeGetResources(device);
    if (!resources) {
        fprintf(stderr, "drmModeGetResources returned NULL\n");
        return 0;
    }

    drmModeConnector *connector = find_connector(resources);
    if (!connector) {
        fprintf(stderr, "find_connector returned NULL\n");
        drmModeFreeResources(resources);
        return 0;
    }

    // save the connector_id
    connector_id = connector->connector_id;
    // save the first mode
    //mode_info = connector->modes[0];
	drmModeModeInfo* mode;
    for (int i = 0; i < connector->count_modes; i++)
    {
        drmModeModeInfo *current_mode = &connector->modes[i];
        if (current_mode->type & DRM_MODE_TYPE_PREFERRED)
        {
            mode = current_mode;
            break;
        }

        mode = NULL;
    }

	mode_info = *mode;
	
    pigl_screen_width = mode_info.hdisplay;
    pigl_screen_height = mode_info.vdisplay;

    fprintf(stderr, "Hardware resolution: %dx%d\n",
        pigl_screen_width, pigl_screen_height);

    // find an encoder
    drmModeEncoder* encoder;
    for (int i = 0; i < resources->count_encoders; i++)
    {
        encoder = drmModeGetEncoder(device, resources->encoders[i]);
        if (encoder->encoder_id == connector->encoder_id)
        {
            break;
        }
        
        drmModeFreeEncoder(encoder);
        encoder = NULL;
    }	
	
    //drmModeEncoder *encoder = find_encoder(resources, connector);
    if (!encoder) {
        fprintf(stderr, "find_encoder returned NULL\n");
        drmModeFreeConnector(connector);
        drmModeFreeResources(resources);
        return 0;
    }

    // find a CRTC
	crtc_id = encoder->crtc_id;
    //if (encoder->crtc_id)
    //    crtc = drmModeGetCrtc(device, encoder->crtc_id);

    drmModeFreeEncoder(encoder);
    drmModeFreeConnector(connector);
    drmModeFreeResources(resources);

    return 1;
}

static int match_config_to_visual(EGLDisplay egl_display,
    EGLint visual_id, EGLConfig* configs, int count)
{
    EGLint id;
	printf("[trngaje] match_config_to_visual: visual_id=0x%x, count=%d\n", visual_id, count);
	
    for (int i = 0; i < count; ++i)
        if (eglGetConfigAttrib(egl_display, configs[i], EGL_NATIVE_VISUAL_ID, &id)
            && id == visual_id) return i;
		else 
		{
			printf("[trngaje] match_config_to_visual(%d):0x%x\n", i, id);
		}

    return -1;
}


static EGLConfig FindConfig(EGLDisplay eglDisplay, int redBits, int greenBits, int blueBits, int alphaBits, int depthBits, int stencilBits)
{
    EGLint configAttributes[] =
    {
        EGL_RED_SIZE,            redBits,
        EGL_GREEN_SIZE,          greenBits,
        EGL_BLUE_SIZE,           blueBits,
        EGL_ALPHA_SIZE,          alphaBits,

        EGL_DEPTH_SIZE,          depthBits,
        EGL_STENCIL_SIZE,        stencilBits,

        EGL_SURFACE_TYPE,        EGL_WINDOW_BIT,
		//EGL_RENDERABLE_TYPE,     EGL_OPENGL_ES_BIT/*EGL_OPENGL_ES2_BIT*/,
        EGL_NONE
    };


    int num_configs;
    EGLBoolean success = eglChooseConfig(eglDisplay, configAttributes, NULL, 0, &num_configs);
    if (success != EGL_TRUE)
    {
        printf("eglChooseConfig failed.\n");
        abort();
    }


    //EGLConfig* configs = new EGLConfig[num_configs];
    EGLConfig configs[num_configs];
    success = eglChooseConfig(eglDisplay, configAttributes, configs, num_configs, &num_configs);
    if (success != EGL_TRUE)
    {
        printf("eglChooseConfig failed.\n");
        abort();
    }


    EGLConfig match = 0;
    for (int i = 0; i < num_configs; ++i)
    {
        EGLint configRedSize;
        EGLint configGreenSize;
        EGLint configBlueSize;
        EGLint configAlphaSize;
        EGLint configDepthSize;
        EGLint configStencilSize;

        eglGetConfigAttrib(eglDisplay, configs[i], EGL_RED_SIZE, &configRedSize);
        eglGetConfigAttrib(eglDisplay, configs[i], EGL_GREEN_SIZE, &configGreenSize);
        eglGetConfigAttrib(eglDisplay, configs[i], EGL_BLUE_SIZE, &configBlueSize);
        eglGetConfigAttrib(eglDisplay, configs[i], EGL_ALPHA_SIZE, &configAlphaSize);
        eglGetConfigAttrib(eglDisplay, configs[i], EGL_DEPTH_SIZE, &configDepthSize);
        eglGetConfigAttrib(eglDisplay, configs[i], EGL_STENCIL_SIZE, &configStencilSize);

        //printf("Egl::FindConfig: index=%d, red=%d, green=%d, blue=%d, alpha=%d\n",
        //	i, configRedSize, configGreenSize, configBlueSize, configAlphaSize);

        if (configRedSize == redBits &&
            configBlueSize == blueBits &&
            configGreenSize == greenBits &&
            configAlphaSize == alphaBits &&
            configDepthSize == depthBits &&
            configStencilSize == stencilBits)
        {
            match = configs[i];
            break;
        }
    }

    return match;
}

int pigl_init()
{
    printf("[trngaje] Initializing DRM video\n");

	if (go2_display == NULL)
		go2_display = go2_display_create();

	device = go2_display->fd;
	connector_id = go2_display->connector_id;
	mode_info = go2_display->mode;
	crtc_id = go2_display->crtc_id;
	pigl_screen_width  = go2_display->width;
	pigl_screen_height = go2_display->height;
	
	printf("[trngaje] Initializing DRM video-step1\n");
	if (go2_presenter == NULL)
		go2_presenter = go2_presenter_create(go2_display, DRM_FORMAT_XRGB8888, 0xff080808);  // ABGR
	
	go2_surface = go2_surface_create(go2_display, pigl_screen_width, pigl_screen_height, DRM_FORMAT_RGB565);
	
	printf("[trngaje] Initializing DRM video-step2\n");
	go2_context_attributes_t attr;
	attr.major = 1;
	attr.minor = 4;
	attr.red_bits = 5;
	attr.green_bits = 6;
	attr.blue_bits = 5;
	attr.alpha_bits = 0;
	attr.depth_bits = 24;
	attr.stencil_bits = 8;


	
	printf("[trngaje] pigl_screen_width=%d, pigl_screen_height=%d\n", pigl_screen_width, pigl_screen_height);
	
	if (go2_context3D == NULL)
		go2_context3D = go2_context_create(go2_display, pigl_screen_width, pigl_screen_height, &attr);
#if 0
	if (go2_surface == NULL)
		go2_surface = go2_surface_create(go2_display, pigl_screen_width, pigl_screen_height, DRM_FORMAT_RGB565);


	egl_surface = go2_context3D->eglSurface;
	display = go2_context3D->eglDisplay;
	context = go2_context3D->eglContext;
#endif	

#if 1

#if 0
	// initialize the EGL display connection
	EGLBoolean result = eglInitialize(display, NULL, NULL);
	//assert(EGL_FALSE != result);
	
	// get an appropriate EGL frame buffer configuration
	EGLint num_config;
	EGLConfig config;
	static const EGLint attribute_list[] =
	{
	    EGL_RED_SIZE, 8,
	    EGL_GREEN_SIZE, 8,
	    EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 0, 
	    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT/*EGL_OPENGL_ES2_BIT*//*EGL_OPENGL_ES_BIT*/, // by trngaje
	    EGL_NONE
	};
	
	

	result = eglChooseConfig(display, attribute_list, &config, 1, &num_config);
	//assert(EGL_FALSE != result);

	result = eglBindAPI(EGL_OPENGL_ES_API);
	//assert(EGL_FALSE != result);

	// create an EGL rendering context
	static const EGLint context_attributes[] =
	{
	    EGL_CONTEXT_CLIENT_VERSION, 2,
	    EGL_NONE
	};
	context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
	//assert(context != EGL_NO_CONTEXT);
#endif
	
/*
    device = open("/dev/dri/card0", O_RDWR);
    if (device == -1) {
        fprintf(stderr, "Error opening device\n");
        return 0;
    }

    if (!find_display_configuration())
        return 0;
*/
    gbm_device = gbm_create_device(device);
		
	printf("[trngaje] pigl_init: gbm_device=0x%x\n", gbm_device);	

	printf("[trngaje] pigl_init: pigl_screen_width=%d, pigl_screen_height=%d\n", pigl_screen_width, pigl_screen_height);
#if 1	
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display = NULL;
    get_platform_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC) eglGetProcAddress("eglGetPlatformDisplayEXT");
    if(get_platform_display == NULL)
    {
        printf("eglGetProcAddress failed.\n");
        return 0;
    }

    display = get_platform_display(EGL_PLATFORM_GBM_KHR, gbm_device, NULL);
    if (display == EGL_NO_DISPLAY)
    {
        printf("eglGetPlatformDisplayEXT failed.\n");
        return 0;
    }
#else


    display = eglGetDisplay(gbm_device);
    if (display == EGL_NO_DISPLAY) {
        fprintf(stderr, "eglGetDisplay() failed: 0x%x\n", eglGetError());
        return 0;
    }
#endif



    // Initialize EGL
    EGLint major;
    EGLint minor;
    if (eglInitialize(display, &major, &minor) != EGL_TRUE)
    {
        printf("eglInitialize failed.\n");
        return 0;
    }

    printf("EGL: major=%d, minor=%d\n", major, minor);
    printf("EGL: Vendor=%s\n", eglQueryString(display, EGL_VENDOR));
    printf("EGL: Version=%s\n", eglQueryString(display, EGL_VERSION));
    printf("EGL: ClientAPIs=%s\n", eglQueryString(display, EGL_CLIENT_APIS));
    printf("EGL: Extensions=%s\n", eglQueryString(display, EGL_EXTENSIONS));
    printf("EGL: ClientExtensions=%s\n", eglQueryString(display, EGL_EXTENSIONS));
    printf("\n");	
/*
	//go2_display_t* go2_display
	go2_display = (go2_display_t*)malloc(sizeof(go2_display_t));
	go2_display->fd = device;
	go2_display->connector_id = connector_id;
	go2_display->mode = mode_info;
	go2_display->width = mode_info.hdisplay;
	go2_display->height = mode_info.vdisplay;
	go2_display->crtc_id = crtc_id;

	
	if (go2_presenter == NULL)
		go2_presenter = go2_presenter_create(go2_display, DRM_FORMAT_XRGB8888, 0xff080808);  // ABGR
	
	if (go2_presenter)
		printf("[trngaje] go2_presenter=0x%x\n", go2_presenter);
	
	go2_surface = go2_surface_create(go2_display, pigl_screen_width, pigl_screen_height, DRM_FORMAT_RGB565);
*/
#if 0
	go2_context_attributes_t attr;
	attr.major = 1;
	attr.minor = 4;
	attr.red_bits = 5;
	attr.green_bits = 6;
	attr.blue_bits = 5;
	attr.alpha_bits = 0;
	attr.depth_bits = 24;
	attr.stencil_bits = 8;

	if (go2_context3D == NULL)
		go2_context3D = go2_context_create(go2_display, pigl_screen_width, pigl_screen_height, &attr);
	if (go2_context3D)
		printf("[trngaje] go2_context3D=0x%x\n", go2_context3D);
	
	
#endif

 #if 1   
    // get an appropriate EGL frame buffer configuration
    EGLint count = 0;
    if (eglGetConfigs(display, NULL, 0, &count) == EGL_FALSE) {
        fprintf(stderr, "eglGetConfigs() failed: 0x%x\n", eglGetError());
        return 0;
    }
	
	printf("[trngaje] pigl_init: count=%d\n", count);
	
    EGLConfig *configs = malloc(count * sizeof *configs);
    EGLint attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 0,
        //EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT/*EGL_OPENGL_ES2_BIT*/, // for EGL1.4
        EGL_NONE };
    EGLint num_config;

    if (eglChooseConfig(display, attributes, configs, count, &num_config) == EGL_FALSE) {
        free(configs);
        fprintf(stderr, "eglChooseConfig() failed: 0x%x\n", eglGetError());
        return 0;
    }
	
	printf("[trngaje] pigl_init: num_config=%d\n", num_config);
	
#if 1
    int config_index = match_config_to_visual(display, GBM_FORMAT_XRGB8888,
        configs, num_config);
    if (config_index == -1) {
        free(configs);
        fprintf(stderr, "No suitable config match\n");
        return 0;
    }
    
	// create an EGL rendering context
    EGLConfig *config = configs[config_index];
#else
    //EGLConfig config = FindConfig(display, 5, 6, 5, 0, 24, 8);
	EGLConfig config = FindConfig(display, 8, 8, 8, 0, 24, 8);
#endif
	//uint32_t drmFourCC;
    eglGetConfigAttrib(display, config, EGL_NATIVE_VISUAL_ID, (EGLint*)&drmFourCC);
	
	printf("[trngaje] pigl_init:drmFourCC=0x%x\n", drmFourCC);
    gbm_surface = gbm_surface_create(gbm_device,
        pigl_screen_width, pigl_screen_height,
        drmFourCC,
        GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!gbm_surface)
    {
        printf("gbm_surface_create failed.\n");
    }
	printf("[trngaje] pigl_init: gbm_surface=0x%x\n", gbm_surface);
   // }

/*
    //struct drm_mode_create_dumb args = {0};
    args.width = pigl_screen_width;
    args.height = pigl_screen_height;
    args.bpp = 24;
    args.flags = 0;

    int io = drmIoctl(device, DRM_IOCTL_MODE_CREATE_DUMB, &args);
    if (io < 0)
    {
        printf("DRM_IOCTL_MODE_CREATE_DUMB failed.\n");
    }
*/

#endif
#if 0
    result->display = display;
    result->gem_handle = args.handle;
    result->size = args.size;
    result->width = width;
    result->height = height;
    result->stride = args.pitch;
    result->format = format;
#endif

#if 0
    const uint32_t handles[4] = {args.handle, 0, 0, 0};
    const uint32_t pitches[4] = {args.pitch, 0, 0, 0};
    const uint32_t offsets[4] = {0, 0, 0, 0};

    int ret = drmModeAddFB2(device,
        pigl_screen_width,
        pigl_screen_height,
        DRM_FORMAT_XRGB8888,
        handles,
        pitches,
        offsets,
        &fb_id,
        0);
    if (ret)
    {
        printf("drmModeAddFB2 failed.\n");
    }
#endif	

	//display = go2_context3D->eglDisplay;
	//egl_surface = go2_context3D->eglSurface;
	//context = go2_context3D->eglContext;
	

#if 1

    egl_surface = eglCreateWindowSurface(display, config, (EGLNativeWindowType)gbm_surface, NULL);

    if (egl_surface == EGL_NO_SURFACE) {
        fprintf(stderr, "eglCreateWindowSurface() failed: EGL_NO_SURFACE\n");
        //return 0;
		//egl_surface = go2_context3D->eglSurface;
    }
	
	//egl_surface = go2_context3D->eglSurface;
	
    // create an OpenGL context
    if (eglBindAPI(EGL_OPENGL_ES_API) == EGL_FALSE) {
        fprintf(stderr, "eglBindAPI() failed: 0x%x\n", eglGetError());
        return 0;
    }
	

	
    static const EGLint contextAttributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);
    if (context == EGL_NO_CONTEXT) {
        //free(configs);
        fprintf(stderr, "eglCreateContext() failed: EGL_NO_CONTEXT\n");
        return 0;
	}
#endif

#if 1	
	//if (eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context) == EGL_FALSE) {
    if (eglMakeCurrent(display, egl_surface, egl_surface, context) == EGL_FALSE) {
        fprintf(stderr, "eglMakeCurrent() failed: 0x%x\n", eglGetError());
        return 0;
    }
#endif	
	

    //fprintf(stderr, "Using renderer: %s\n", glGetString(GL_RENDERER));
 //free(configs);
#endif
    return 1;
}

void pigl_shutdown()
{
	printf("[trngaje] pigl_shutdown++\n");
    // set the previous crtc
    if (crtc != NULL) {
        drmModeSetCrtc(device, crtc_id/*crtc->crtc_id*/, crtc->buffer_id,
            crtc->x, crtc->y, &connector_id, 1, &crtc->mode);
        drmModeFreeCrtc(crtc);
    }

    if (previous_bo) {
        drmModeRmFB(device, previous_fb);
        gbm_surface_release_buffer(gbm_surface, previous_bo);
    }
    if (egl_surface != EGL_NO_SURFACE) {
        eglDestroySurface(display, egl_surface);
        egl_surface = EGL_NO_SURFACE;
    }
    if (gbm_surface) {
        gbm_surface_destroy(gbm_surface);
        gbm_surface = NULL;
    }
    if (context != EGL_NO_CONTEXT) {
        eglDestroyContext(display, context);
        context = EGL_NO_CONTEXT;
    }
    if (display != EGL_NO_DISPLAY) {
        eglTerminate(display);
        display = EGL_NO_DISPLAY;
    }
    if (gbm_device) {
        gbm_device_destroy(gbm_device);
        gbm_device = NULL;
    }
    if (device != -1) {
        close(device);
        device = -1;
    }
	

	if (go2_surface != NULL)
	{
		go2_surface_destroy(go2_surface);
		go2_surface = NULL;
	}
	
	if (go2_context3D != NULL)
	{
		go2_context_destroy(go2_context3D);
		go2_context3D = NULL;
	}
	
	if (go2_presenter != NULL)
	{
		go2_presenter_destroy(go2_presenter);
		go2_presenter = NULL;
	}

	if (go2_display != NULL)
	{
		go2_display_destroy(go2_display);
		go2_display = NULL;
	}
}

void pigl_swap()
{
	//printf("[trngaje] pigl_swap:try1\n");
#if 1
    if (eglSwapBuffers(display, egl_surface) == EGL_FALSE)
    {
        printf("pigl_swap:eglSwapBuffers failed\n");
        //abort();
    }
#endif	
#if 0	
	go2_context_swap_buffers(go2_context3D);
	go2_surface_t* gles_surface = go2_context_surface_lock(go2_context3D);
	
	go2_presenter_post(go2_presenter,
				gles_surface, //go2_surface
				0, 0, pigl_screen_width, pigl_screen_height,
				0, 0, pigl_screen_height, pigl_screen_width,
				GO2_ROTATION_DEGREES_270);
	go2_context_surface_unlock(go2_context3D, gles_surface);

	
#else
    struct gbm_bo *bo = gbm_surface_lock_front_buffer(gbm_surface);
    if (!bo)
    {
        //printf("gbm_surface_lock_front_buffer failed.\n");
    }
	else
	{
		//printf("[trngaje] pigl_swap 0x%x\n", bo);
		
		uint32_t handle = gbm_bo_get_handle(bo).u32;
		uint32_t pitch = gbm_bo_get_stride(bo);
		uint32_t fb;

		if(drmModeAddFB(device, mode_info.hdisplay, mode_info.vdisplay, 24, 32, pitch, handle, &fb))
		{
			printf("[trngaje] drmModeAddFB error:0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x\n",
				device, mode_info.hdisplay, mode_info.vdisplay, pitch, handle, fb);
		}
		
#if 0
		uint16_t* src = (uint16_t*)fb;
        uint16_t* dst = (uint16_t*)go2_surface_map(go2_surface);
		
		memcpy(dst, src, pigl_screen_width * pigl_screen_height * sizeof(uint16_t));
		
		go2_presenter_post(go2_presenter,
					go2_surface, 
					0, 0, pigl_screen_width, pigl_screen_height,
					0, 0, pigl_screen_height, pigl_screen_width,
					GO2_ROTATION_DEGREES_270);
#endif		
		
		//drmModeSetCrtc (int fd, uint32_t crtc_id, uint32_t fb_id, uint32_t x, uint32_t y, uint32_t * connectors, int count, drmModeModeInfoPtr drm_mode)
		if(drmModeSetCrtc(device, crtc_id, fb, 0, 0, &connector_id, 1, &mode_info))
		{
			printf("[trngaje] drmModeSetCrtc error:0x%x, 0x%x, 0x%x, 0x%x, 0x%x\n",
				device, crtc_id, fb, connector_id, mode_info);		
		}

		if (previous_bo) {
			drmModeRmFB(device, previous_fb);
			gbm_surface_release_buffer(gbm_surface, previous_bo);
		}

		previous_bo = bo;
		previous_fb = fb;
	}
	
#endif
	
}
