/* This is an implementation file to be included after certain #defines have been set.
See a particular renderer's *.c file for specifics. */


#include "SDL_gpu_GL_matrix.h"
#include "SDL_platform.h"

#include "stb_image.h"
#include "stb_image_write.h"


// Forces a flush when vertex limit is reached (roughly 1000 sprites)
#define GPU_BLIT_BUFFER_VERTICES_PER_SPRITE 4
#define GPU_BLIT_BUFFER_INIT_MAX_NUM_VERTICES (GPU_BLIT_BUFFER_VERTICES_PER_SPRITE*1000)

#ifndef SDL_GPU_USE_GL_TIER3
// x, y, s, t
#define GPU_BLIT_BUFFER_FLOATS_PER_VERTEX 4
#else
// x, y, s, t, r, g, b, a
#define GPU_BLIT_BUFFER_FLOATS_PER_VERTEX 8
#endif

// bytes per vertex
#define GPU_BLIT_BUFFER_STRIDE (sizeof(float)*GPU_BLIT_BUFFER_FLOATS_PER_VERTEX)
#define GPU_BLIT_BUFFER_VERTEX_OFFSET 0
#define GPU_BLIT_BUFFER_TEX_COORD_OFFSET 2
#define GPU_BLIT_BUFFER_COLOR_OFFSET 4


#include <math.h>
#include <string.h>
#include <strings.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


#ifdef SDL_GPU_USE_SDL2
    #define GET_ALPHA(sdl_color) (sdl_color.a)
#else
    #define GET_ALPHA(sdl_color) (sdl_color.unused)
#endif


#ifndef GL_VERTEX_SHADER
    #ifndef SDL_GPU_DISABLE_SHADERS
        #define SDL_GPU_DISABLE_SHADERS
    #endif
#endif






static SDL_PixelFormat* AllocFormat(GLenum glFormat);
static void FreeFormat(SDL_PixelFormat* format);



static Uint8 isExtensionSupported(const char* extension_str)
{
#ifdef SDL_GPU_USE_OPENGL
    return glewIsExtensionSupported(extension_str);
#else
    // As suggested by Mesa3D.org
    char* p = (char*)glGetString(GL_EXTENSIONS);
    char* end;
    int extNameLen;

    extNameLen = strlen(extension_str);
    end = p + strlen(p);

    while(p < end)
    {
        int n = strcspn(p, " ");
        if((extNameLen == n) && (strncmp(extension_str, p, n) == 0))
            return 1;
        
        p += (n + 1);
    }
    return 0;
#endif
}

static void init_features(GPU_Renderer* renderer)
{
    // NPOT textures
#ifdef SDL_GPU_USE_OPENGL
    if(isExtensionSupported("GL_ARB_texture_non_power_of_two"))
        renderer->enabled_features |= GPU_FEATURE_NON_POWER_OF_TWO;
    else
        renderer->enabled_features &= ~GPU_FEATURE_NON_POWER_OF_TWO;
#elif defined(SDL_GPU_USE_GLES)
    if(isExtensionSupported("GL_OES_texture_npot") || isExtensionSupported("GL_IMG_texture_npot")
       || isExtensionSupported("GL_APPLE_texture_2D_limited_npot") || isExtensionSupported("GL_ARB_texture_non_power_of_two"))
        renderer->enabled_features |= GPU_FEATURE_NON_POWER_OF_TWO;
    else
        renderer->enabled_features &= ~GPU_FEATURE_NON_POWER_OF_TWO;
#endif

    // FBO
#ifdef SDL_GPU_USE_OPENGL
    if(isExtensionSupported("GL_EXT_framebuffer_object"))
        renderer->enabled_features |= GPU_FEATURE_RENDER_TARGETS;
    else
        renderer->enabled_features &= ~GPU_FEATURE_RENDER_TARGETS;
#elif defined(SDL_GPU_USE_GLES)
    #if SDL_GPU_GL_TIER < 3
        if(isExtensionSupported("GL_OES_framebuffer_object"))
            renderer->enabled_features |= GPU_FEATURE_RENDER_TARGETS;
        else
            renderer->enabled_features &= ~GPU_FEATURE_RENDER_TARGETS;
    #else
            renderer->enabled_features |= GPU_FEATURE_RENDER_TARGETS;
    #endif
#endif

    // Blending
#ifdef SDL_GPU_USE_OPENGL
    renderer->enabled_features |= GPU_FEATURE_BLEND_EQUATIONS;
    renderer->enabled_features |= GPU_FEATURE_BLEND_FUNC_SEPARATE;
#elif defined(SDL_GPU_USE_GLES)
    if(isExtensionSupported("GL_OES_blend_subtract"))
        renderer->enabled_features |= GPU_FEATURE_BLEND_EQUATIONS;
    else
        renderer->enabled_features &= ~GPU_FEATURE_BLEND_EQUATIONS;
    if(isExtensionSupported("GL_OES_blend_func_separate"))
        renderer->enabled_features |= GPU_FEATURE_BLEND_FUNC_SEPARATE;
    else
        renderer->enabled_features &= ~GPU_FEATURE_BLEND_FUNC_SEPARATE;
#endif

    // GL texture formats
    if(isExtensionSupported("GL_EXT_bgr"))
        renderer->enabled_features |= GPU_FEATURE_GL_BGR;
    if(isExtensionSupported("GL_EXT_bgra"))
        renderer->enabled_features |= GPU_FEATURE_GL_BGRA;
    if(isExtensionSupported("GL_EXT_abgr"))
        renderer->enabled_features |= GPU_FEATURE_GL_ABGR;

    if(isExtensionSupported("GL_ARB_fragment_shader"))
        renderer->enabled_features |= GPU_FEATURE_FRAGMENT_SHADER;
    if(isExtensionSupported("GL_ARB_vertex_shader"))
        renderer->enabled_features |= GPU_FEATURE_VERTEX_SHADER;
    if(isExtensionSupported("GL_ARB_geometry_shader4"))
        renderer->enabled_features |= GPU_FEATURE_GEOMETRY_SHADER;
}

static void extBindFramebuffer(GPU_Renderer* renderer, GLuint handle)
{
    if(renderer->enabled_features & GPU_FEATURE_RENDER_TARGETS)
        glBindFramebuffer(GL_FRAMEBUFFER, handle);
}


static inline Uint8 isPowerOfTwo(unsigned int x)
{
    return ((x != 0) && !(x & (x - 1)));
}

static inline unsigned int getNearestPowerOf2(unsigned int n)
{
    unsigned int x = 1;
    while(x < n)
    {
        x <<= 1;
    }
    return x;
}

static void bindTexture(GPU_Renderer* renderer, GPU_Image* image)
{
    // Bind the texture to which subsequent calls refer
    if(image != ((GPU_CONTEXT_DATA*)renderer->current_context_target->context->data)->last_image)
    {
        GLuint handle = ((GPU_IMAGE_DATA*)image->data)->handle;
        renderer->FlushBlitBuffer(renderer);

        glBindTexture( GL_TEXTURE_2D, handle );
        ((GPU_CONTEXT_DATA*)renderer->current_context_target->context->data)->last_image = image;
    }
}

static inline void flushAndBindTexture(GPU_Renderer* renderer, GLuint handle)
{
    // Bind the texture to which subsequent calls refer
    renderer->FlushBlitBuffer(renderer);

    glBindTexture( GL_TEXTURE_2D, handle );
    ((GPU_CONTEXT_DATA*)renderer->current_context_target->context->data)->last_image = NULL;
}

// Returns false if it can't be bound
static Uint8 bindFramebuffer(GPU_Renderer* renderer, GPU_Target* target)
{
    if(renderer->enabled_features & GPU_FEATURE_RENDER_TARGETS)
    {
        // Bind the FBO
        if(target != ((GPU_CONTEXT_DATA*)renderer->current_context_target->context->data)->last_target)
        {
            GLuint handle = 0;
            if(target != NULL)
                handle = ((GPU_TARGET_DATA*)target->data)->handle;
            renderer->FlushBlitBuffer(renderer);

            extBindFramebuffer(renderer, handle);
            ((GPU_CONTEXT_DATA*)renderer->current_context_target->context->data)->last_target = target;
        }
        return 1;
    }
    else
    {
        return (target != NULL && ((GPU_TARGET_DATA*)target->data)->handle == 0);
    }
}

static inline void flushAndBindFramebuffer(GPU_Renderer* renderer, GLuint handle)
{
    // Bind the FBO
    renderer->FlushBlitBuffer(renderer);

    extBindFramebuffer(renderer, handle);
    ((GPU_CONTEXT_DATA*)renderer->current_context_target->context->data)->last_target = NULL;
}

static inline void flushBlitBufferIfCurrentTexture(GPU_Renderer* renderer, GPU_Image* image)
{
    if(image == ((GPU_CONTEXT_DATA*)renderer->current_context_target->context->data)->last_image)
    {
        renderer->FlushBlitBuffer(renderer);
    }
}

static inline void flushAndClearBlitBufferIfCurrentTexture(GPU_Renderer* renderer, GPU_Image* image)
{
    if(image == ((GPU_CONTEXT_DATA*)renderer->current_context_target->context->data)->last_image)
    {
        renderer->FlushBlitBuffer(renderer);
        ((GPU_CONTEXT_DATA*)renderer->current_context_target->context->data)->last_image = NULL;
    }
}

static inline Uint8 isCurrentTarget(GPU_Renderer* renderer, GPU_Target* target)
{
    return (target == ((GPU_CONTEXT_DATA*)renderer->current_context_target->context->data)->last_target
            || ((GPU_CONTEXT_DATA*)renderer->current_context_target->context->data)->last_target == NULL);
}

static inline void flushAndClearBlitBufferIfCurrentFramebuffer(GPU_Renderer* renderer, GPU_Target* target)
{
    if(target == ((GPU_CONTEXT_DATA*)renderer->current_context_target->context->data)->last_target
            || ((GPU_CONTEXT_DATA*)renderer->current_context_target->context->data)->last_target == NULL)
    {
        renderer->FlushBlitBuffer(renderer);
        ((GPU_CONTEXT_DATA*)renderer->current_context_target->context->data)->last_target = NULL;
    }
}


// Only for window targets, which have their own contexts.
static void makeContextCurrent(GPU_Renderer* renderer, GPU_Target* target)
{
    if(target == NULL || target->context == NULL || renderer->current_context_target == target)
        return;
    
    renderer->FlushBlitBuffer(renderer);
    
    #ifdef SDL_GPU_USE_SDL2
    SDL_GL_MakeCurrent(SDL_GetWindowFromID(target->context->windowID), target->context->context);
    renderer->current_context_target = target;
    #endif
}

static void setClipRect(GPU_Renderer* renderer, GPU_Target* target)
{
    if(target->use_clip_rect)
    {
        glEnable(GL_SCISSOR_TEST);
        GPU_Target* context_target = renderer->current_context_target;
        if(target->context != NULL)
        {
            int y = context_target->h - (target->clip_rect.y + target->clip_rect.h);
            float xFactor = ((float)context_target->context->window_w)/context_target->w;
            float yFactor = ((float)context_target->context->window_h)/context_target->h;
            glScissor(target->clip_rect.x * xFactor, y * yFactor, target->clip_rect.w * xFactor, target->clip_rect.h * yFactor);
        }
        else
            glScissor(target->clip_rect.x, target->clip_rect.y, target->clip_rect.w, target->clip_rect.h);
    }
}

static void unsetClipRect(GPU_Renderer* renderer, GPU_Target* target)
{
    if(target->use_clip_rect)
        glDisable(GL_SCISSOR_TEST);
}

static void prepareToRenderToTarget(GPU_Renderer* renderer, GPU_Target* target)
{
    // Set up the camera
    renderer->SetCamera(renderer, target, &target->camera);
    
    setClipRect(renderer, target);
}



static void changeColor(GPU_Renderer* renderer, SDL_Color color)
{
    #ifdef SDL_GPU_USE_GL_TIER3
    return;
    #else
    GPU_CONTEXT_DATA* cdata = (GPU_CONTEXT_DATA*)renderer->current_context_target->context->data;
    if(cdata->last_color.r != color.r
        || cdata->last_color.g != color.g
        || cdata->last_color.b != color.b
        || GET_ALPHA(cdata->last_color) != GET_ALPHA(color))
    {
        renderer->FlushBlitBuffer(renderer);
        cdata->last_color = color;
        glColor4f(color.r/255.01f, color.g/255.01f, color.b/255.01f, GET_ALPHA(color)/255.01f);
    }
    #endif
}

static void changeBlending(GPU_Renderer* renderer, Uint8 enable)
{
    GPU_CONTEXT_DATA* cdata = (GPU_CONTEXT_DATA*)renderer->current_context_target->context->data;
    if(cdata->last_use_blending == enable)
        return;
    
    renderer->FlushBlitBuffer(renderer);

    if(enable)
        glEnable(GL_BLEND);
    else
        glDisable(GL_BLEND);

    cdata->last_use_blending = enable;
}

static void changeBlendMode(GPU_Renderer* renderer, GPU_BlendEnum mode)
{
    GPU_CONTEXT_DATA* cdata = (GPU_CONTEXT_DATA*)renderer->current_context_target->context->data;
    if(cdata->last_blend_mode == mode)
        return;
    
    renderer->FlushBlitBuffer(renderer);

    cdata->last_blend_mode = mode;
    
    if(mode == GPU_BLEND_NORMAL)
    {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        if(!(renderer->enabled_features & GPU_FEATURE_BLEND_EQUATIONS))
            return;  // TODO: Return false so we can avoid depending on it if it fails
        glBlendEquation(GL_FUNC_ADD);
    }
    else if(mode == GPU_BLEND_PREMULTIPLIED_ALPHA)
    {
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        if(!(renderer->enabled_features & GPU_FEATURE_BLEND_EQUATIONS))
            return;
        glBlendEquation(GL_FUNC_ADD);
    }
    else if(mode == GPU_BLEND_MULTIPLY)
    {
        if(!(renderer->enabled_features & GPU_FEATURE_BLEND_FUNC_SEPARATE))
            return;
        glBlendFuncSeparate(GL_DST_COLOR, GL_ZERO, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        if(!(renderer->enabled_features & GPU_FEATURE_BLEND_EQUATIONS))
            return;
        glBlendEquation(GL_FUNC_ADD);
    }
    else if(mode == GPU_BLEND_ADD)
    {
        glBlendFunc(GL_ONE, GL_ONE);
        if(!(renderer->enabled_features & GPU_FEATURE_BLEND_EQUATIONS))
            return;
        glBlendEquation(GL_FUNC_ADD);
    }
    else if(mode == GPU_BLEND_SUBTRACT)
    {
        if(!(renderer->enabled_features & GPU_FEATURE_BLEND_EQUATIONS))
            return;
        glBlendFunc(GL_ONE, GL_ONE);
        glBlendEquation(GL_FUNC_SUBTRACT);
    }
    else if(mode == GPU_BLEND_ADD_COLOR)
    {
        if(!(renderer->enabled_features & GPU_FEATURE_BLEND_FUNC_SEPARATE))
            return;
        glBlendFuncSeparate(GL_ONE, GL_ONE, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        if(!(renderer->enabled_features & GPU_FEATURE_BLEND_EQUATIONS))
            return;
        glBlendEquation(GL_FUNC_ADD);
    }
    else if(mode == GPU_BLEND_SUBTRACT_COLOR)
    {
        if(!(renderer->enabled_features & GPU_FEATURE_BLEND_FUNC_SEPARATE))
            return;
        if(!(renderer->enabled_features & GPU_FEATURE_BLEND_EQUATIONS))
            return;
        glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA);
        glBlendEquation(GL_FUNC_SUBTRACT);
    }
    else if(mode == GPU_BLEND_DIFFERENCE)
    {
        if(!(renderer->enabled_features & GPU_FEATURE_BLEND_FUNC_SEPARATE))
            return;
        if(!(renderer->enabled_features & GPU_FEATURE_BLEND_EQUATIONS))
            return;
        glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
        glBlendEquation(GL_FUNC_SUBTRACT);
    }
    else if(mode == GPU_BLEND_PUNCHOUT)
    {
        if(!(renderer->enabled_features & GPU_FEATURE_BLEND_EQUATIONS))
            return;
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
    }
    else if(mode == GPU_BLEND_CUTOUT)
    {
        if(!(renderer->enabled_features & GPU_FEATURE_BLEND_EQUATIONS))
            return;
        glBlendFunc(GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA);
        glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
    }
}


// If 0 is returned, there is no valid shader.
static Uint32 get_proper_program_id(GPU_Renderer* renderer, Uint32 program_object)
{
    GPU_Context* context = renderer->current_context_target->context;
    if(context->default_textured_shader_program == 0)  // No shaders loaded!
        return 0;
    
    if(program_object == 0)
        return context->default_textured_shader_program;
    
    return program_object;
}

#define MIX_COLOR_COMPONENT(a, b) (((a)/255.0f * (b)/255.0f)*255)
#define MIX_COLORS(color1, color2) {MIX_COLOR_COMPONENT(color1.r, color2.r), MIX_COLOR_COMPONENT(color1.g, color2.g), MIX_COLOR_COMPONENT(color1.b, color2.b), MIX_COLOR_COMPONENT(GET_ALPHA(color1), GET_ALPHA(color2))}

static void prepareToRenderImage(GPU_Renderer* renderer, GPU_Target* target, GPU_Image* image)
{
    GPU_Context* context = renderer->current_context_target->context;
    
    // TODO: Store this state and only call it from FlushBlitBuffer()
    glEnable(GL_TEXTURE_2D);
    
    // Blitting
    if(target->use_color)
    {
        SDL_Color color = MIX_COLORS(target->color, image->color);
        changeColor(renderer, color);
    }
    else
        changeColor(renderer, image->color);
    changeBlending(renderer, image->use_blending);
    changeBlendMode(renderer, image->blend_mode);
    
    // If we're using the untextured shader, switch it.
    if(context->current_shader_program == context->default_untextured_shader_program)
        renderer->ActivateShaderProgram(renderer, context->default_textured_shader_program, NULL);
}

static void prepareToRenderShapes(GPU_Renderer* renderer)
{
    GPU_Context* context = renderer->current_context_target->context;
    
    // TODO: Store this state and only call it from FlushBlitBuffer()
    glDisable(GL_TEXTURE_2D);
    
    // Shape rendering
    // Color is set elsewhere for shapes
    changeBlending(renderer, context->shapes_use_blending);
    changeBlendMode(renderer, context->shapes_blend_mode);
    
    // If we're using the textured shader, switch it.
    if(context->current_shader_program == context->default_textured_shader_program)
        renderer->ActivateShaderProgram(renderer, context->default_untextured_shader_program, NULL);
}



static void changeViewport(GPU_Target* target)
{
    GPU_CONTEXT_DATA* cdata = (GPU_CONTEXT_DATA*)(GPU_GetContextTarget()->context->data);
    GPU_Rect viewport = target->viewport;
    if(cdata->last_viewport.x == viewport.x && cdata->last_viewport.y == viewport.y && cdata->last_viewport.w == viewport.w && cdata->last_viewport.h == viewport.h)
        return;
    cdata->last_viewport = viewport;
    // Need the real height to flip the y-coord (from OpenGL coord system)
    float h;
    if(target->image != NULL)
        h = target->image->h;
    else if(target->context != NULL)
        h = target->context->window_h;
    
    glViewport(viewport.x, h - viewport.h - viewport.y, viewport.w, h);
}

static void applyTargetCamera(GPU_Target* target)
{
    ((GPU_CONTEXT_DATA*)(GPU_GetContextTarget()->context->data))->last_camera = target->camera;
    
    GPU_MatrixMode( GPU_PROJECTION );
    GPU_LoadIdentity();

    // The default z for objects is 0
    
    GPU_Ortho(target->camera.x, target->w + target->camera.x, target->h + target->camera.y, target->camera.y, -1.0f, 1.0f);

    GPU_MatrixMode( GPU_MODELVIEW );
    GPU_LoadIdentity();


    float offsetX = target->w/2.0f;
    float offsetY = target->h/2.0f;
    GPU_Translate(offsetX, offsetY, -0.01);
    GPU_Rotate(target->camera.angle, 0, 0, 1);
    GPU_Translate(-offsetX, -offsetY, 0);

    GPU_Translate(target->camera.x + offsetX, target->camera.y + offsetY, 0);
    GPU_Scale(target->camera.zoom, target->camera.zoom, 1.0f);
    GPU_Translate(-target->camera.x - offsetX, -target->camera.y - offsetY, 0);
}

#ifdef SDL_GPU_APPLY_TRANSFORMS_TO_GL_STACK
static void applyTransforms(void)
{
    float* p = GPU_GetProjection();
    float* m = GPU_GetModelView();
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(p);
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(m);
}
#endif


// Workaround for Intel HD glVertexAttrib() bug.
#ifdef SDL_GPU_USE_OPENGL
// FIXME: This should probably exist in context storage, as I expect it to be a problem across contexts.
static Uint8 apply_Intel_attrib_workaround = 0;
static Uint8 vendor_is_Intel = 0;
#endif

static GPU_Target* Init(GPU_Renderer* renderer, GPU_RendererID renderer_request, Uint16 w, Uint16 h, GPU_WindowFlagEnum SDL_flags)
{
    if(renderer_request.major_version < 1)
    {
        renderer_request.major_version = 1;
        renderer_request.minor_version = 1;
    }
    
    GPU_InitFlagEnum GPU_flags = GPU_GetPreInitFlags();
    // Tell SDL what we want.
    renderer->GPU_init_flags = GPU_flags;
    if(GPU_flags & GPU_INIT_DISABLE_DOUBLE_BUFFER)
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 0);
    else
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
#ifdef SDL_GPU_USE_SDL2
    #ifdef SDL_GPU_USE_GLES
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    #endif
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, renderer_request.major_version);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, renderer_request.minor_version);
#else
    if(!(GPU_flags & GPU_INIT_DISABLE_VSYNC))
        SDL_GL_SetAttribute(SDL_GL_SWAP_CONTROL, 1);
#endif

    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);

    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
	
	renderer->requested_id = renderer_request;

#ifdef SDL_GPU_USE_SDL2

    SDL_Window* window = NULL;
    
    // Is there a window already set up that we are supposed to use?
    if(renderer->current_context_target != NULL)
        window = SDL_GetWindowFromID(renderer->current_context_target->context->windowID);
    else
        window = SDL_GetWindowFromID(GPU_GetInitWindow());
    
    if(window == NULL)
    {
        // Set up window flags
        SDL_flags |= SDL_WINDOW_OPENGL;
        if(!(SDL_flags & SDL_WINDOW_HIDDEN))
            SDL_flags |= SDL_WINDOW_SHOWN;
        
        renderer->SDL_init_flags = SDL_flags;
        window = SDL_CreateWindow("",
                                  SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  w, h,
                                  SDL_flags);

        if(window == NULL)
        {
            GPU_LogError("Window creation failed.\n");
            return NULL;
        }
        
        GPU_SetInitWindow(SDL_GetWindowID(window));
    }
    else
        renderer->SDL_init_flags = SDL_flags;

#else
    SDL_flags |= SDL_OPENGL;
    renderer->SDL_init_flags = SDL_flags;
    SDL_Surface* screen = SDL_SetVideoMode(w, h, 0, SDL_flags);

    if(screen == NULL)
        return NULL;
#endif
    
    renderer->enabled_features = 0xFFFFFFFF;  // Pretend to support them all if using incompatible headers
    
    
    // Create or re-init the current target.  This also creates the GL context and initializes enabled_features.
    #ifdef SDL_GPU_USE_SDL2
    if(renderer->CreateTargetFromWindow(renderer, SDL_GetWindowID(window), renderer->current_context_target) == NULL)
        return NULL;
    #else
    if(renderer->CreateTargetFromWindow(renderer, 0, renderer->current_context_target) == NULL)
        return NULL;
    #endif
    
    // Init glVertexAttrib workaround
    #ifdef SDL_GPU_USE_OPENGL
    const char* vendor_string = (const char*)glGetString(GL_VENDOR);
    if(strstr(vendor_string, "Intel") != NULL)
    {
        vendor_is_Intel = 1;
        apply_Intel_attrib_workaround = 1;
    }
    #endif
    
    return renderer->current_context_target;
}


static Uint8 IsFeatureEnabled(GPU_Renderer* renderer, GPU_FeatureEnum feature)
{
    return ((renderer->enabled_features & feature) == feature);
}


static GPU_Target* CreateTargetFromWindow(GPU_Renderer* renderer, Uint32 windowID, GPU_Target* target)
{
    Uint8 created = 0;  // Make a new one or repurpose an existing target?
    GPU_CONTEXT_DATA* cdata;
    if(target == NULL)
    {
        created = 1;
        target = (GPU_Target*)malloc(sizeof(GPU_Target));
        target->data = (GPU_TARGET_DATA*)malloc(sizeof(GPU_TARGET_DATA));
        target->image = NULL;
        cdata = (GPU_CONTEXT_DATA*)malloc(sizeof(GPU_CONTEXT_DATA));
        target->context = (GPU_Context*)malloc(sizeof(GPU_Context));
        target->context->data = cdata;
        target->context->context = NULL;
        
        cdata->last_image = NULL;
        cdata->last_target = NULL;
        // Initialize the blit buffer
        cdata->blit_buffer_max_num_vertices = GPU_BLIT_BUFFER_INIT_MAX_NUM_VERTICES;
        cdata->blit_buffer_num_vertices = 0;
        int blit_buffer_storage_size = GPU_BLIT_BUFFER_INIT_MAX_NUM_VERTICES*GPU_BLIT_BUFFER_STRIDE;
        cdata->blit_buffer = (float*)malloc(blit_buffer_storage_size);
        cdata->index_buffer_max_num_vertices = GPU_BLIT_BUFFER_INIT_MAX_NUM_VERTICES;
        cdata->index_buffer_num_vertices = 0;
        int index_buffer_storage_size = GPU_BLIT_BUFFER_INIT_MAX_NUM_VERTICES*GPU_BLIT_BUFFER_STRIDE;
        cdata->index_buffer = (unsigned short*)malloc(index_buffer_storage_size);
        // Init index buffer
        int i, n;
        for(i = 0, n = 0; i < cdata->index_buffer_max_num_vertices; i+=4)
        {
            // First tri
            cdata->index_buffer[n++] = i;  // 0
            cdata->index_buffer[n++] = i+1;  // 1
            cdata->index_buffer[n++] = i+2;  // 2

            // Second tri
            cdata->index_buffer[n++] = i; // 0
            cdata->index_buffer[n++] = i+2;  // 2
            cdata->index_buffer[n++] = i+3;  // 3
        }
    }
    else
        cdata = (GPU_CONTEXT_DATA*)target->context->data;
    
    #ifdef SDL_GPU_USE_SDL2
    
    SDL_Window* window = SDL_GetWindowFromID(windowID);
    if(window == NULL)
    {
        if(created)
        {
            free(cdata->blit_buffer);
            free(cdata->index_buffer);
            free(target->context->data);
            free(target->context);
            free(target->data);
            free(target);
        }
        return NULL;
    }
    
    // Store the window info
    SDL_GetWindowSize(window, &target->context->window_w, &target->context->window_h);
    target->context->windowID = SDL_GetWindowID(window);
    
    // Make a new context if needed and make it current
    if(created || target->context->context == NULL)
    {
        target->context->context = SDL_GL_CreateContext(window);
        renderer->current_context_target = target;
    }
    else
        renderer->MakeCurrent(renderer, target, target->context->windowID);
    
    #else
    
    SDL_Surface* screen = SDL_GetVideoSurface();
    if(screen == NULL)
    {
        if(created)
        {
            free(cdata->blit_buffer);
            free(cdata->index_buffer);
            free(target->context->data);
            free(target->context);
            free(target->data);
            free(target);
        }
        return NULL;
    }
    
    target->context->windowID = 0;
    target->context->window_w = screen->w;
    target->context->window_h = screen->h;
    
    renderer->MakeCurrent(renderer, target, target->context->windowID);
    
    #endif
    
    int framebuffer_handle;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer_handle);
    ((GPU_TARGET_DATA*)target->data)->handle = framebuffer_handle;
    ((GPU_TARGET_DATA*)target->data)->format = GL_RGBA;

    target->renderer = renderer;
    target->w = target->context->window_w;
    target->h = target->context->window_h;

    target->use_clip_rect = 0;
    target->clip_rect.x = 0;
    target->clip_rect.y = 0;
    target->clip_rect.w = target->w;
    target->clip_rect.h = target->h;
    target->use_color = 0;
    
    target->viewport = GPU_MakeRect(0, 0, target->context->window_w, target->context->window_h);
    target->camera = GPU_GetDefaultCamera();
    
    target->context->line_thickness = 1.0f;
    target->context->shapes_use_blending = 1;
    target->context->shapes_blend_mode = GPU_BLEND_NORMAL;
    
    SDL_Color white = {255, 255, 255, 255};
    cdata->last_color = white;
    cdata->last_use_blending = 0;
    cdata->last_blend_mode = GPU_BLEND_NORMAL;
    cdata->last_viewport = target->viewport;
    cdata->last_camera = target->camera;  // Redundant due to applyTargetCamera()
    

    #ifdef SDL_GPU_USE_OPENGL
    GLenum err = glewInit();
    if (GLEW_OK != err)
    {
        // Probably don't have the right GL version for this renderer
        return NULL;
    }
    #endif
    
    
    // Update our renderer info from the current GL context.
    #ifdef SDL_GPU_USE_OPENGL
    // OpenGL < 3.0 doesn't have GL_MAJOR_VERSION.  Check via version string instead.
    const char* version_string = (const char*)glGetString(GL_VERSION);
    if(sscanf(version_string, "%d.%d", &renderer->id.major_version, &renderer->id.minor_version) <= 0)
    {
        renderer->id.major_version = SDL_GPU_GL_MAJOR_VERSION;
        #if SDL_GPU_GL_MAJOR_VERSION != 3
            renderer->id.minor_version = 1;
        #else
            renderer->id.minor_version = 0;
        #endif
        
        GPU_LogError("Failed to parse OpenGL version string: %s\n  Defaulting to version %d.%d.\n", version_string, renderer->id.major_version, renderer->id.minor_version);
    }
    #else
    // GLES doesn't have GL_MAJOR_VERSION.  Check via version string instead.
    const char* version_string = (const char*)glGetString(GL_VERSION);
    // OpenGL ES 2.0?
    if(sscanf(version_string, "OpenGL ES %d.%d", &renderer->id.major_version, &renderer->id.minor_version) <= 0)
    {
        // OpenGL ES-CM 1.1?  OpenGL ES-CL 1.1?
        if(sscanf(version_string, "OpenGL ES-C%*c %d.%d", &renderer->id.major_version, &renderer->id.minor_version) <= 0)
        {
            renderer->id.major_version = SDL_GPU_GLES_MAJOR_VERSION;
            #if SDL_GPU_GLES_MAJOR_VERSION == 1
                renderer->id.minor_version = 1;
            #else
                renderer->id.minor_version = 0;
            #endif
            
            GPU_LogError("Failed to parse OpenGLES version string: %s\n  Defaulting to version %d.%d.\n", version_string, renderer->id.major_version, renderer->id.minor_version);
        }
    }
    #endif
    
    // Did the wrong runtime library try to use a later versioned renderer?
    if(renderer->id.major_version < renderer->requested_id.major_version)
    {
		#ifdef SDL_GPU_USE_GLES
			GPU_LogError("GPU_Init failed: Renderer %s can not be run by the version %d.%d library that is linked.\n", GPU_GetRendererEnumString(renderer->requested_id.id), renderer->id.major_version, renderer->id.minor_version);
		#endif
        return NULL;
    }

    init_features(renderer);
    
    GPU_FeatureEnum required_features = (renderer->GPU_init_flags & GPU_FEATURE_MASK);
    if(!renderer->IsFeatureEnabled(renderer, required_features))
    {
        GPU_LogError("Error: Renderer %s does not support required features.\n", GPU_GetRendererEnumString(renderer->id.id));
        return NULL;
    }
    
    #ifdef SDL_GPU_USE_SDL2
    // No preference for vsync?
    if(!(renderer->GPU_init_flags & (GPU_INIT_DISABLE_VSYNC | GPU_INIT_ENABLE_VSYNC)))
    {
        // Default to late swap vsync if available
        if(SDL_GL_SetSwapInterval(-1) < 0)
            SDL_GL_SetSwapInterval(1);  // Or go for vsync
    }
    else if(renderer->GPU_init_flags & GPU_INIT_ENABLE_VSYNC)
        SDL_GL_SetSwapInterval(1);
    else if(renderer->GPU_init_flags & GPU_INIT_DISABLE_VSYNC)
        SDL_GL_SetSwapInterval(0);
    #endif
    
    // Set up GL state
    
    target->context->projection_matrix.size = 1;
    GPU_MatrixIdentity(target->context->projection_matrix.matrix[0]);
    
    target->context->modelview_matrix.size = 1;
    GPU_MatrixIdentity(target->context->modelview_matrix.matrix[0]);
    
    target->context->matrix_mode = GPU_MODELVIEW;
    
    // Modes
    glEnable( GL_TEXTURE_2D );
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glDisable(GL_BLEND);
    glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );

    // Viewport and Framebuffer
    glViewport(0.0f, 0.0f, target->viewport.w, target->viewport.h);

    glClear( GL_COLOR_BUFFER_BIT );
    #if SDL_GPU_GL_TIER < 3
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    #endif
    
    // Set up camera
    applyTargetCamera(target);
    
    renderer->SetLineThickness(renderer, 1.0f);
    
    
    target->context->default_textured_shader_program = 0;
    target->context->default_untextured_shader_program = 0;
    target->context->current_shader_program = 0;
    
    #ifndef SDL_GPU_DISABLE_SHADERS
    // Do we need a default shader?
    if(renderer->id.major_version >= 2)
    {
        // Textured shader
        Uint32 v = renderer->CompileShader(renderer, GPU_VERTEX_SHADER, GPU_DEFAULT_TEXTURED_VERTEX_SHADER_SOURCE);
        
        if(!v)
            GPU_LogError("Failed to load default textured vertex shader: %s\n", renderer->GetShaderMessage(renderer));
        
        Uint32 f = renderer->CompileShader(renderer, GPU_FRAGMENT_SHADER, GPU_DEFAULT_TEXTURED_FRAGMENT_SHADER_SOURCE);
        
        if(!f)
            GPU_LogError("Failed to load default textured fragment shader: %s\n", renderer->GetShaderMessage(renderer));
        
        Uint32 p = renderer->LinkShaders(renderer, v, f);
        
        if(!p)
            GPU_LogError("Failed to link default textured shader program: %s\n", renderer->GetShaderMessage(renderer));
        
        target->context->default_textured_shader_program = p;
        
        #ifdef SDL_GPU_USE_GL_TIER3
        // Get locations of the attributes in the shader
        cdata->shader_block[0] = GPU_LoadShaderBlock(p, "gpu_Vertex", "gpu_TexCoord", "gpu_Color", "gpu_ModelViewProjectionMatrix");
        #endif
        
        // Untextured shader
        v = renderer->CompileShader(renderer, GPU_VERTEX_SHADER, GPU_DEFAULT_UNTEXTURED_VERTEX_SHADER_SOURCE);
        
        if(!v)
            GPU_LogError("Failed to load default untextured vertex shader: %s\n", renderer->GetShaderMessage(renderer));
        
        f = renderer->CompileShader(renderer, GPU_FRAGMENT_SHADER, GPU_DEFAULT_UNTEXTURED_FRAGMENT_SHADER_SOURCE);
        
        if(!f)
            GPU_LogError("Failed to load default untextured fragment shader: %s\n", renderer->GetShaderMessage(renderer));
        
        p = renderer->LinkShaders(renderer, v, f);
        
        if(!p)
            GPU_LogError("Failed to link default untextured shader program: %s\n", renderer->GetShaderMessage(renderer));
        
        glUseProgram(p);
        
        target->context->default_untextured_shader_program = target->context->current_shader_program = p;
        
        #ifdef SDL_GPU_USE_GL_TIER3
            // Get locations of the attributes in the shader
            cdata->shader_block[1] = GPU_LoadShaderBlock(p, "gpu_Vertex", NULL, "gpu_Color", "gpu_ModelViewProjectionMatrix");
            GPU_SetShaderBlock(cdata->shader_block[1]);
            
            // Create vertex array container and buffer
            #if !defined(SDL_GPU_NO_VAO)
            glGenVertexArrays(1, &cdata->blit_VAO);
            glBindVertexArray(cdata->blit_VAO);
            #endif
            
            glGenBuffers(2, cdata->blit_VBO);
            // Create space on the GPU
            glBindBuffer(GL_ARRAY_BUFFER, cdata->blit_VBO[0]);
            glBufferData(GL_ARRAY_BUFFER, GPU_BLIT_BUFFER_STRIDE * cdata->blit_buffer_max_num_vertices, NULL, GL_STREAM_DRAW);
            glBindBuffer(GL_ARRAY_BUFFER, cdata->blit_VBO[1]);
            glBufferData(GL_ARRAY_BUFFER, GPU_BLIT_BUFFER_STRIDE * cdata->blit_buffer_max_num_vertices, NULL, GL_STREAM_DRAW);
            cdata->blit_VBO_flop = 0;
            
            glGenBuffers(16, cdata->attribute_VBO);
            
            // Init 16 attributes to 0 / NULL.
            memset(cdata->shader_attributes, 0, 16*sizeof(GPU_AttributeSource));
        #endif
    }
    #endif
    
    
    return target;
}

static void MakeCurrent(GPU_Renderer* renderer, GPU_Target* target, Uint32 windowID)
{
    if(target == NULL)
        return;
    #ifdef SDL_GPU_USE_SDL2
    if(target->image != NULL)
        return;
    
    SDL_GLContext c = target->context->context;
    if(c != NULL)
    {
        renderer->current_context_target = target;
        SDL_GL_MakeCurrent(SDL_GetWindowFromID(windowID), c);
        // Reset camera if the target's window was changed
        if(target->context->windowID != windowID)
        {
            renderer->FlushBlitBuffer(renderer);
            target->context->windowID = windowID;
            applyTargetCamera(((GPU_CONTEXT_DATA*)renderer->current_context_target->context->data)->last_target);
        }
    }
    #else
    renderer->current_context_target = target;
    #endif
}


static void SetAsCurrent(GPU_Renderer* renderer)
{
    if(renderer->current_context_target == NULL)
        return;
    
    renderer->MakeCurrent(renderer, renderer->current_context_target, renderer->current_context_target->context->windowID);
}

static int SetWindowResolution(GPU_Renderer* renderer, Uint16 w, Uint16 h)
{
#ifdef SDL_GPU_USE_SDL2
    SDL_SetWindowSize(SDL_GetWindowFromID(renderer->current_context_target->context->windowID), w, h);
    SDL_GetWindowSize(SDL_GetWindowFromID(renderer->current_context_target->context->windowID), &renderer->current_context_target->context->window_w, &renderer->current_context_target->context->window_h);
#else
    SDL_Surface* surf = SDL_GetVideoSurface();
    Uint32 flags = surf->flags;


    SDL_Surface* screen = SDL_SetVideoMode(w, h, 0, flags);
    // There's a bug in SDL.  This is a workaround.  Let's resize again:
    screen = SDL_SetVideoMode(w, h, 0, flags);

    if(screen == NULL)
        return 0;

    renderer->current_context_target->context->window_w = screen->w;
    renderer->current_context_target->context->window_h = screen->h;
#endif

    Uint16 virtualW = renderer->current_context_target->w;
    Uint16 virtualH = renderer->current_context_target->h;
    
    // FIXME: This might interfere with cameras or be ruined by them.
    glEnable( GL_TEXTURE_2D );
    glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );

    renderer->current_context_target->viewport = GPU_MakeRect(0, 0, w, h);
    changeViewport(renderer->current_context_target);

    glClear( GL_COLOR_BUFFER_BIT );

    GPU_MatrixMode( GPU_PROJECTION );
    GPU_LoadIdentity();

    GPU_Ortho(0.0f, virtualW, virtualH, 0.0f, -1.0f, 1.0f);

    GPU_MatrixMode( GPU_MODELVIEW );
    GPU_LoadIdentity();

    // Update display
    GPU_ClearClip(renderer->current_context_target);

    return 1;
}

static void SetVirtualResolution(GPU_Renderer* renderer, GPU_Target* target, Uint16 w, Uint16 h)
{
    if(target == NULL)
        return;

    target->w = w;
    target->h = h;
    
    if(isCurrentTarget(renderer, target))
    {
        renderer->FlushBlitBuffer(renderer);
        applyTargetCamera(target);
    }
}

static void Quit(GPU_Renderer* renderer)
{
    renderer->FreeTarget(renderer, renderer->current_context_target);
    renderer->current_context_target = NULL;
}



static int ToggleFullscreen(GPU_Renderer* renderer)
{
#ifdef SDL_GPU_USE_SDL2
    Uint8 enable = !(SDL_GetWindowFlags(SDL_GetWindowFromID(renderer->current_context_target->context->windowID)) & SDL_WINDOW_FULLSCREEN);

    if(SDL_SetWindowFullscreen(SDL_GetWindowFromID(renderer->current_context_target->context->windowID), enable) < 0)
        return 0;

    return 1;
#else
    SDL_Surface* surf = SDL_GetVideoSurface();

    if(SDL_WM_ToggleFullScreen(surf))
        return 1;

    Uint16 w = surf->w;
    Uint16 h = surf->h;
    surf->flags ^= SDL_FULLSCREEN;
    return SetWindowResolution(renderer, w, h);
#endif
}


static GPU_Camera SetCamera(GPU_Renderer* renderer, GPU_Target* target, GPU_Camera* cam)
{
    if(target == NULL)
        return GPU_GetDefaultCamera();
    
    
    GPU_Camera result = target->camera;

    if(cam == NULL)
        target->camera = GPU_GetDefaultCamera();
    else
        target->camera = *cam;
    
    if(isCurrentTarget(renderer, target))
    {
        // Change the active camera
        
        // Skip change if the camera is already the same.
        GPU_CONTEXT_DATA* cdata = (GPU_CONTEXT_DATA*)renderer->current_context_target->context->data;
        if(result.x == cdata->last_camera.x && result.y == cdata->last_camera.y && result.z == cdata->last_camera.z
           && result.angle == cdata->last_camera.angle && result.zoom == cdata->last_camera.zoom)
            return result;

        renderer->FlushBlitBuffer(renderer);
        applyTargetCamera(target);
    }

    return result;
}


static GPU_Image* CreateUninitializedImage(GPU_Renderer* renderer, Uint16 w, Uint16 h, Uint8 channels)
{
    if(channels < 3 || channels > 4)
    {
        GPU_LogError("GPU_CreateUninitializedImage() could not create an image with %d color channels.  Try 3 or 4 instead.\n", channels);
        return NULL;
    }

    GLuint handle;
    GLenum format;
    if(channels == 3)
        format = GL_RGB;
    else
        format = GL_RGBA;

    glGenTextures( 1, &handle );
    if(handle == 0)
    {
        GPU_LogError("GPU_CreateUninitializedImage() failed to generate a texture handle.\n");
        return NULL;
    }

    flushAndBindTexture( renderer, handle );

    // Set the texture's stretching properties
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
    #if defined(SDL_GPU_USE_GLES) && (SDL_GPU_GLES_TIER == 1)
    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    #endif

    GPU_Image* result = (GPU_Image*)malloc(sizeof(GPU_Image));
    GPU_IMAGE_DATA* data = (GPU_IMAGE_DATA*)malloc(sizeof(GPU_IMAGE_DATA));
    result->target = NULL;
    result->renderer = renderer;
    result->channels = channels;
    result->has_mipmaps = 0;
    
    SDL_Color white = {255, 255, 255, 255};
    result->color = white;
    result->use_blending = (channels > 3? 1 : 0);
    result->blend_mode = GPU_BLEND_NORMAL;
    result->filter_mode = GPU_LINEAR;
    
    result->data = data;
    result->refcount = 1;
    data->handle = handle;
    data->format = format;

    result->w = w;
    result->h = h;
    // POT textures will change this later
    result->texture_w = w;
    result->texture_h = h;

    return result;
}


static GPU_Image* CreateImage(GPU_Renderer* renderer, Uint16 w, Uint16 h, Uint8 channels)
{
    if(channels < 3 || channels > 4)
    {
        GPU_LogError("GPU_CreateImage() could not create an image with %d color channels.  Try 3 or 4 instead.\n", channels);
        return NULL;
    }

    GPU_Image* result = CreateUninitializedImage(renderer, w, h, channels);

    if(result == NULL)
    {
        GPU_LogError("GPU_CreateImage() could not create %ux%ux%u image.\n", w, h, channels);
        return NULL;
    }

    glEnable(GL_TEXTURE_2D);
    bindTexture(renderer, result);

    GLenum internal_format = ((GPU_IMAGE_DATA*)(result->data))->format;
    w = result->w;
    h = result->h;
    if(!(renderer->enabled_features & GPU_FEATURE_NON_POWER_OF_TWO))
    {
        if(!isPowerOfTwo(w))
            w = getNearestPowerOf2(w);
        if(!isPowerOfTwo(h))
            h = getNearestPowerOf2(h);
    }

    // Initialize texture
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, w, h, 0,
                 internal_format, GL_UNSIGNED_BYTE, NULL);
    // Tell SDL_gpu what we got.
    result->texture_w = w;
    result->texture_h = h;

    return result;
}

static GPU_Image* LoadImage(GPU_Renderer* renderer, const char* filename)
{
    SDL_Surface* surface = GPU_LoadSurface(filename);
    if(surface == NULL)
    {
        GPU_LogError("Failed to load image \"%s\"\n", filename);
        return NULL;
    }

    GPU_Image* result = renderer->CopyImageFromSurface(renderer, surface);
    SDL_FreeSurface(surface);

    return result;
}


static Uint8 readTargetPixels(GPU_Renderer* renderer, GPU_Target* source, GLint format, GLubyte* pixels)
{
    if(source == NULL)
        return 0;
    
    if(isCurrentTarget(renderer, source))
        renderer->FlushBlitBuffer(renderer);
    
    if(bindFramebuffer(renderer, source))
    {
        glReadPixels(0, 0, source->w, source->h, format, GL_UNSIGNED_BYTE, pixels);
        return 1;
    }
    return 0;
}

static Uint8 readImagePixels(GPU_Renderer* renderer, GPU_Image* source, GLint format, GLubyte* pixels)
{
    if(source == NULL)
        return 0;
    
    // No glGetTexImage() in OpenGLES
    #ifdef SDL_GPU_USE_GLES
    // Load up the target
    Uint8 created_target = 0;
    if(source->target == NULL)
    {
        renderer->LoadTarget(renderer, source);
        created_target = 1;
    }
    // Get the data
    Uint8 result = readTargetPixels(renderer, source->target, format, pixels);
    // Free the target
    if(created_target)
        renderer->FreeTarget(renderer, source->target);
    return result;
    #else
    // Bind the texture temporarily
    glBindTexture(GL_TEXTURE_2D, ((GPU_IMAGE_DATA*)source->data)->handle);
    // Get the data
    glGetTexImage(GL_TEXTURE_2D, 0, format, GL_UNSIGNED_BYTE, pixels);
    // Rebind the last texture
    if(((GPU_CONTEXT_DATA*)renderer->current_context_target->context->data)->last_image != NULL)
        glBindTexture(GL_TEXTURE_2D, ((GPU_IMAGE_DATA*)(((GPU_CONTEXT_DATA*)renderer->current_context_target->context->data)->last_image)->data)->handle);
    return 1;
    #endif
}

static unsigned char* getRawTargetData(GPU_Renderer* renderer, GPU_Target* target)
{
    int channels = 4;
    if(target->image != NULL)
        channels = target->image->channels;
    unsigned char* data = (unsigned char*)malloc(target->w * target->h * channels);
    
    if(!readTargetPixels(renderer, target, ((GPU_TARGET_DATA*)target->data)->format, data))
    {
        free(data);
        return NULL;
    }
    
    // Flip the data vertically (OpenGL framebuffer is read upside down)
    int pitch = target->w * channels;
    unsigned char* copy = (unsigned char*)malloc(pitch);
    int y;
    for(y = 0; y < target->h/2; y++)
    {
        unsigned char* top = &data[target->w * y * channels];
        unsigned char* bottom = &data[target->w * (target->h - y - 1) * channels];
        memcpy(copy, top, pitch);
        memcpy(top, bottom, pitch);
        memcpy(bottom, copy, pitch);
    }

    return data;
}

static unsigned char* getRawImageData(GPU_Renderer* renderer, GPU_Image* image)
{
    unsigned char* data = (unsigned char*)malloc(image->w * image->h * image->channels);

    if(!readImagePixels(renderer, image, ((GPU_IMAGE_DATA*)image->data)->format, data))
    {
        free(data);
        return NULL;
    }

    return data;
}

// From http://stackoverflow.com/questions/5309471/getting-file-extension-in-c
static const char *get_filename_ext(const char *filename)
{
    const char *dot = strrchr(filename, '.');
    if(!dot || dot == filename)
        return "";
    return dot + 1;
}

static Uint8 SaveImage(GPU_Renderer* renderer, GPU_Image* image, const char* filename)
{
    const char* extension;
    Uint8 result;
    unsigned char* data;

    if(image == NULL || filename == NULL ||
            image->w < 1 || image->h < 1 || image->channels < 1 || image->channels > 4)
    {
        return 0;
    }

    extension = get_filename_ext(filename);

    data = getRawImageData(renderer, image);

    if(data == NULL)
    {
        GPU_LogError("GPU_SaveImage() failed: Could not retrieve image data.\n");
        return 0;
    }

    if(SDL_strcasecmp(extension, "png") == 0)
        result = stbi_write_png(filename, image->w, image->h, image->channels, (const unsigned char *const)data, 0);
    else if(SDL_strcasecmp(extension, "bmp") == 0)
        result = stbi_write_bmp(filename, image->w, image->h, image->channels, (void*)data);
    else if(SDL_strcasecmp(extension, "tga") == 0)
        result = stbi_write_tga(filename, image->w, image->h, image->channels, (void*)data);
    //else if(SDL_strcasecmp(extension, "dds") == 0)
    //    result = stbi_write_dds(filename, image->w, image->h, image->channels, (const unsigned char *const)data);
    else
    {
        GPU_LogError("GPU_SaveImage() failed: Unsupported format (%s).\n", extension);
        result = 0;
    }

    free(data);
    return result;
}

static SDL_Surface* CopySurfaceFromTarget(GPU_Renderer* renderer, GPU_Target* target)
{
    unsigned char* data;
    SDL_Surface* result;

    if(target == NULL || target->w < 1 || target->h < 1)
        return NULL;

    data = getRawTargetData(renderer, target);

    if(data == NULL)
    {
        GPU_LogError("GPU_CopySurfaceFromTarget() failed: Could not retrieve target data.\n");
        return NULL;
    }
    
    SDL_PixelFormat* format = AllocFormat(((GPU_TARGET_DATA*)target->data)->format);
    
    result = SDL_CreateRGBSurfaceFrom(data, target->w, target->h, format->BitsPerPixel, target->w*format->BytesPerPixel, format->Rmask, format->Gmask, format->Bmask, format->Amask);
    
    FreeFormat(format);
    return result;
}

static SDL_Surface* CopySurfaceFromImage(GPU_Renderer* renderer, GPU_Image* image)
{
    unsigned char* data;
    SDL_Surface* result;

    if(image == NULL || image->w < 1 || image->h < 1)
        return NULL;

    data = getRawImageData(renderer, image);

    if(data == NULL)
    {
        GPU_LogError("GPU_CopySurfaceFromImage() failed: Could not retrieve image data.\n");
        return NULL;
    }
    
    SDL_PixelFormat* format = AllocFormat(((GPU_IMAGE_DATA*)image->data)->format);
    
    result = SDL_CreateRGBSurfaceFrom(data, image->w, image->h, format->BitsPerPixel, image->w*format->BytesPerPixel, format->Rmask, format->Gmask, format->Bmask, format->Amask);
    
    FreeFormat(format);
    return result;
}















// Returns 0 if a direct conversion (asking OpenGL to do it) is safe.  Returns 1 if a copy is needed.  Returns -1 on error.
// The surfaceFormatResult is used to specify what direct conversion format the surface pixels are in (source format).
#ifdef SDL_GPU_USE_GLES
// OpenGLES does not do direct conversion.  Internal format (glFormat) and original format (surfaceFormatResult) must be the same.
static int compareFormats(GPU_Renderer* renderer, GLenum glFormat, SDL_Surface* surface, GLenum* surfaceFormatResult)
{
    SDL_PixelFormat* format = surface->format;
    switch(glFormat)
    {
        // 3-channel formats
    case GL_RGB:
        if(format->BytesPerPixel != 3)
            return 1;

        if(format->Rmask == 0x0000FF && format->Gmask == 0x00FF00 && format->Bmask ==  0xFF0000)
        {
            if(surfaceFormatResult != NULL)
                *surfaceFormatResult = GL_RGB;
            return 0;
        }
#ifdef GL_BGR
        if(format->Rmask == 0xFF0000 && format->Gmask == 0x00FF00 && format->Bmask == 0x0000FF)
        {

            if(renderer->enabled_features & GPU_FEATURE_GL_BGR)
            {
                if(surfaceFormatResult != NULL)
                    *surfaceFormatResult = GL_BGR;
            }
            else
                return 1;
            return 0;
        }
#endif
        return 1;
        // 4-channel formats
    case GL_RGBA:
        if(format->BytesPerPixel != 4)
            return 1;

        if (format->Rmask == 0x000000FF && format->Gmask == 0x0000FF00 && format->Bmask ==  0x00FF0000)
        {
            if(surfaceFormatResult != NULL)
                *surfaceFormatResult = GL_RGBA;
            return 0;
        }
#ifdef GL_BGRA
        if (format->Rmask == 0x00FF0000 && format->Gmask == 0x0000FF00 && format->Bmask == 0x000000FF)
        {
            if(surfaceFormatResult != NULL)
                *surfaceFormatResult = GL_BGRA;
            return 0;
        }
#endif
#ifdef GL_ABGR
        if (format->Rmask == 0xFF000000 && format->Gmask == 0x00FF0000 && format->Bmask == 0x0000FF00)
        {
            if(surfaceFormatResult != NULL)
                *surfaceFormatResult = GL_ABGR;
            return 0;
        }
#endif
        return 1;
    default:
        GPU_LogError("GPU_UpdateImage() was passed an image with an invalid format.\n");
        return -1;
    }
}
#else
//GL_RGB/GL_RGBA and Surface format
static int compareFormats(GPU_Renderer* renderer, GLenum glFormat, SDL_Surface* surface, GLenum* surfaceFormatResult)
{
    SDL_PixelFormat* format = surface->format;
    switch(glFormat)
    {
        // 3-channel formats
    case GL_RGB:
        if(format->BytesPerPixel != 3)
            return 1;

        // Looks like RGB?  Easy!
        if(format->Rmask == 0x0000FF && format->Gmask == 0x00FF00 && format->Bmask == 0xFF0000)
        {
            if(surfaceFormatResult != NULL)
                *surfaceFormatResult = GL_RGB;
            return 0;
        }
        // Looks like BGR?
        if(format->Rmask == 0xFF0000 && format->Gmask == 0x00FF00 && format->Bmask == 0x0000FF)
        {
#ifdef GL_BGR
            if(renderer->enabled_features & GPU_FEATURE_GL_BGR)
            {
                if(surfaceFormatResult != NULL)
                    *surfaceFormatResult = GL_BGR;
                return 0;
            }
#endif
        }
        return 1;

        // 4-channel formats
    case GL_RGBA:

        if(format->BytesPerPixel != 4)
            return 1;

        // Looks like RGBA?  Easy!
        if(format->Rmask == 0x000000FF && format->Gmask == 0x0000FF00 && format->Bmask == 0x00FF0000)
        {
            if(surfaceFormatResult != NULL)
                *surfaceFormatResult = GL_RGBA;
            return 0;
        }
        // Looks like ABGR?
        if(format->Rmask == 0xFF000000 && format->Gmask == 0x00FF0000 && format->Bmask == 0x0000FF00)
        {
#ifdef GL_ABGR
            if(renderer->enabled_features & GPU_FEATURE_GL_ABGR)
            {
                if(surfaceFormatResult != NULL)
                    *surfaceFormatResult = GL_ABGR;
                return 0;
            }
#endif
        }
        // Looks like BGRA?
        else if(format->Rmask == 0x00FF0000 && format->Gmask == 0x0000FF00 && format->Bmask == 0x000000FF)
        {
#ifdef GL_BGRA
            if(renderer->enabled_features & GPU_FEATURE_GL_BGRA)
            {
                //ARGB, for OpenGL BGRA
                if(surfaceFormatResult != NULL)
                    *surfaceFormatResult = GL_BGRA;
                return 0;
            }
#endif
        }
        return 1;
    default:
        GPU_LogError("GPU_UpdateImage() was passed an image with an invalid format.\n");
        return -1;
    }
}
#endif


// Adapted from SDL_AllocFormat()
static SDL_PixelFormat* AllocFormat(GLenum glFormat)
{
    // Yes, I need to do the whole thing myself... :(
    int channels;
    Uint32 Rmask, Gmask, Bmask, Amask = 0, mask;

    switch(glFormat)
    {
    case GL_RGB:
        channels = 3;
        Rmask = 0x0000FF;
        Gmask = 0x00FF00;
        Bmask = 0xFF0000;
        break;
#ifdef GL_BGR
    case GL_BGR:
        channels = 3;
        Rmask = 0xFF0000;
        Gmask = 0x00FF00;
        Bmask = 0x0000FF;
        break;
#endif
    case GL_RGBA:
        channels = 4;
        Rmask = 0x000000FF;
        Gmask = 0x0000FF00;
        Bmask = 0x00FF0000;
        Amask = 0xFF000000;
        break;
#ifdef GL_BGRA
    case GL_BGRA:
        channels = 4;
        Rmask = 0x00FF0000;
        Gmask = 0x0000FF00;
        Bmask = 0x000000FF;
        Amask = 0xFF000000;
        break;
#endif
#ifdef GL_ABGR
    case GL_ABGR:
        channels = 4;
        Rmask = 0xFF000000;
        Gmask = 0x00FF0000;
        Bmask = 0x0000FF00;
        Amask = 0x000000FF;
        break;
#endif
    default:
        return NULL;
    }

    //GPU_LogError("AllocFormat(): %d, Masks: %X %X %X %X\n", glFormat, Rmask, Gmask, Bmask, Amask);

    SDL_PixelFormat* result = (SDL_PixelFormat*)malloc(sizeof(SDL_PixelFormat));
    memset(result, 0, sizeof(SDL_PixelFormat));

    result->BitsPerPixel = 8*channels;
    result->BytesPerPixel = channels;

    result->Rmask = Rmask;
    result->Rshift = 0;
    result->Rloss = 8;
    if (Rmask) {
        for (mask = Rmask; !(mask & 0x01); mask >>= 1)
            ++result->Rshift;
        for (; (mask & 0x01); mask >>= 1)
            --result->Rloss;
    }

    result->Gmask = Gmask;
    result->Gshift = 0;
    result->Gloss = 8;
    if (Gmask) {
        for (mask = Gmask; !(mask & 0x01); mask >>= 1)
            ++result->Gshift;
        for (; (mask & 0x01); mask >>= 1)
            --result->Gloss;
    }

    result->Bmask = Bmask;
    result->Bshift = 0;
    result->Bloss = 8;
    if (Bmask) {
        for (mask = Bmask; !(mask & 0x01); mask >>= 1)
            ++result->Bshift;
        for (; (mask & 0x01); mask >>= 1)
            --result->Bloss;
    }

    result->Amask = Amask;
    result->Ashift = 0;
    result->Aloss = 8;
    if (Amask) {
        for (mask = Amask; !(mask & 0x01); mask >>= 1)
            ++result->Ashift;
        for (; (mask & 0x01); mask >>= 1)
            --result->Aloss;
    }

    return result;
}

static Uint8 hasColorkey(SDL_Surface* surface)
{
#ifdef SDL_GPU_USE_SDL2
    return (SDL_GetColorKey(surface, NULL) == 0);
#else
    return (surface->flags & SDL_SRCCOLORKEY);
#endif
}

static void FreeFormat(SDL_PixelFormat* format)
{
    free(format);
}

// Returns NULL on failure.  Returns the original surface if no copy is needed.  Returns a new surface converted to the right format otherwise.
static SDL_Surface* copySurfaceIfNeeded(GPU_Renderer* renderer, GLenum glFormat, SDL_Surface* surface, GLenum* surfaceFormatResult)
{
    // If format doesn't match, we need to do a copy
    int format_compare = compareFormats(renderer, glFormat, surface, surfaceFormatResult);

    // There's a problem
    if(format_compare < 0)
        return NULL;
    
    #ifdef SDL_GPU_USE_GLES
    // GLES needs a tightly-packed pixel array
    // Based on SDL_UpdateTexture()
    SDL_Surface* newSurface = NULL;
    Uint8 *blob = NULL;
    SDL_Rect rect = {0, 0, surface->w, surface->h};
    int srcPitch = rect.w * surface->format->BytesPerPixel;
    int pitch = surface->pitch;
    if(srcPitch != pitch)
    {
        Uint8 *src;
        Uint8 *pixels = (Uint8*)surface->pixels;
        int y;
        
        /* Bail out if we're supposed to update an empty rectangle */
        if(rect.w <= 0 || rect.h <= 0)
            return NULL;
        
        /* Reformat the texture data into a tightly packed array */
        src = pixels;
        if(pitch != srcPitch)
        {
            blob = (Uint8*)malloc(srcPitch * rect.h);
            if(blob == NULL)
            {
                // Out of memory
                return NULL;
            }
            src = blob;
            for(y = 0; y < rect.h; ++y)
            {
                memcpy(src, pixels, srcPitch);
                src += srcPitch;
                pixels += pitch;
            }
            src = blob;
        }
        
        newSurface = SDL_CreateRGBSurfaceFrom(src, rect.w, rect.h, surface->format->BytesPerPixel, srcPitch, surface->format->Rmask, surface->format->Gmask, surface->format->Bmask, surface->format->Amask);
    }
    
    // Copy it to a different format
    if(format_compare > 0)
    {
        // Convert to the right format
        SDL_PixelFormat* dst_fmt = AllocFormat(glFormat);
        if(newSurface != NULL)
        {
            surface = SDL_ConvertSurface(newSurface, dst_fmt, 0);
            SDL_FreeSurface(newSurface);
            free(blob);
        }
        else
            surface = SDL_ConvertSurface(surface, dst_fmt, 0);
        FreeFormat(dst_fmt);
        if(surfaceFormatResult != NULL && surface != NULL)
            *surfaceFormatResult = glFormat;
    }
    
    #else
    // Copy it to a different format
    if(format_compare > 0)
    {
        // Convert to the right format
        SDL_PixelFormat* dst_fmt = AllocFormat(glFormat);
        surface = SDL_ConvertSurface(surface, dst_fmt, 0);
        FreeFormat(dst_fmt);
        if(surfaceFormatResult != NULL && surface != NULL)
            *surfaceFormatResult = glFormat;
    }
    #endif

    // No copy needed
    return surface;
}

// From SDL_UpdateTexture()
static int InitImageWithSurface(GPU_Renderer* renderer, GPU_Image* image, SDL_Surface* surface)
{
    if(image == NULL || surface == NULL)
        return 0;

    GPU_IMAGE_DATA* data = (GPU_IMAGE_DATA*)image->data;

    GLenum internal_format = data->format;
    GLenum original_format = internal_format;

    SDL_Surface* newSurface = copySurfaceIfNeeded(renderer, internal_format, surface, &original_format);
    if(newSurface == NULL)
    {
        GPU_LogError("GPU_InitImageWithSurface() failed to convert surface to proper pixel format.\n");
        return 0;
    }

    Uint8 need_power_of_two_upload = 0;
    unsigned int w = newSurface->w;
    unsigned int h = newSurface->h;
    if(!(renderer->enabled_features & GPU_FEATURE_NON_POWER_OF_TWO))
    {
        if(!isPowerOfTwo(w))
        {
            w = getNearestPowerOf2(w);
            need_power_of_two_upload = 1;
        }
        if(!isPowerOfTwo(h))
        {
            h = getNearestPowerOf2(h);
            need_power_of_two_upload = 1;
        }
    }

    glEnable(GL_TEXTURE_2D);
    bindTexture(renderer, image);
    int alignment = 1;
    if(newSurface->format->BytesPerPixel == 4)
        alignment = 4;

    glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    #ifdef SDL_GPU_USE_OPENGL
    glPixelStorei(GL_UNPACK_ROW_LENGTH, (newSurface->pitch / newSurface->format->BytesPerPixel));
    #endif
    if(!need_power_of_two_upload)
    {
        //GPU_LogError("InitImageWithSurface(), Copy? %d, internal: %d, original: %d, GL_RGB: %d, GL_RGBA: %d\n", (newSurface != surface), internal_format, original_format, GL_RGB, GL_RGBA);
        glTexImage2D(GL_TEXTURE_2D, 0, internal_format, newSurface->w, newSurface->h, 0,
                     original_format, GL_UNSIGNED_BYTE, newSurface->pixels);
    }
    else
    {
        //GPU_LogError("InitImageWithSurface(), POT upload. Copy? %d, internal: %d, original: %d, GL_RGB: %d, GL_RGBA: %d\n", (newSurface != surface), internal_format, original_format, GL_RGB, GL_RGBA);
        
        // Create POT texture
        glTexImage2D(GL_TEXTURE_2D, 0, internal_format, w, h, 0,
                     original_format, GL_UNSIGNED_BYTE, NULL);

        // Upload NPOT data
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, newSurface->w, newSurface->h,
                        original_format, GL_UNSIGNED_BYTE, newSurface->pixels);

        // Tell everyone what we did.
        image->texture_w = w;
        image->texture_h = h;
    }

    // Delete temporary surface
    if(surface != newSurface)
        SDL_FreeSurface(newSurface);
    return 1;
}

static GPU_Image* CopyImage(GPU_Renderer* renderer, GPU_Image* image)
{
    if(image == NULL)
        return NULL;
    
    GPU_Image* result = CreateUninitializedImage(renderer, image->w, image->h, image->channels);
    if(result == NULL)
        return NULL;
    
    SDL_Surface* surface = renderer->CopySurfaceFromImage(renderer, image);
    if(surface == NULL)
        return NULL;
    
    InitImageWithSurface(renderer, result, surface);
    
    SDL_FreeSurface(surface);
    
    return result;
}



// From SDL_UpdateTexture()
static void UpdateImage(GPU_Renderer* renderer, GPU_Image* image, const GPU_Rect* rect, SDL_Surface* surface)
{
    if(image == NULL || surface == NULL)
        return;

    GPU_IMAGE_DATA* data = (GPU_IMAGE_DATA*)image->data;
    GLenum original_format = data->format;

    SDL_Surface* newSurface = copySurfaceIfNeeded(renderer, data->format, surface, &original_format);
    if(newSurface == NULL)
    {
        GPU_LogError("GPU_UpdateImage() failed to convert surface to proper pixel format.\n");
        return;
    }


    GPU_Rect updateRect;
    if(rect != NULL)
        updateRect = *rect;
    else
    {
        updateRect.x = 0;
        updateRect.y = 0;
        updateRect.w = newSurface->w;
        updateRect.h = newSurface->h;
        if(updateRect.w < 0.0f || updateRect.h < 0.0f)
        {
            GPU_LogError("GPU_UpdateImage(): Given negative rect: %dx%d\n", (int)updateRect.w, (int)updateRect.h);
            return;
        }
    }


    glEnable(GL_TEXTURE_2D);
    if(image->target != NULL && isCurrentTarget(renderer, image->target))
        renderer->FlushBlitBuffer(renderer);
    bindTexture(renderer, image);
    int alignment = 1;
    if(newSurface->format->BytesPerPixel == 4)
        alignment = 4;
    glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    #ifdef SDL_GPU_USE_OPENGL
    glPixelStorei(GL_UNPACK_ROW_LENGTH, (newSurface->pitch / newSurface->format->BytesPerPixel));
    #endif
    glTexSubImage2D(GL_TEXTURE_2D, 0, updateRect.x, updateRect.y, updateRect.w,
                    updateRect.h, original_format, GL_UNSIGNED_BYTE,
                    newSurface->pixels);

    // Delete temporary surface
    if(surface != newSurface)
        SDL_FreeSurface(newSurface);
}


static inline Uint32 getPixel(SDL_Surface *Surface, int x, int y)
{
    Uint8* bits;
    Uint32 bpp;

    if(x < 0 || x >= Surface->w)
        return 0;  // Best I could do for errors

    bpp = Surface->format->BytesPerPixel;
    bits = ((Uint8*)Surface->pixels) + y*Surface->pitch + x*bpp;

    switch (bpp)
    {
    case 1:
        return *((Uint8*)Surface->pixels + y * Surface->pitch + x);
        break;
    case 2:
        return *((Uint16*)Surface->pixels + y * Surface->pitch/2 + x);
        break;
    case 3:
        // Endian-correct, but slower
    {
        Uint8 r, g, b;
        r = *((bits)+Surface->format->Rshift/8);
        g = *((bits)+Surface->format->Gshift/8);
        b = *((bits)+Surface->format->Bshift/8);
        return SDL_MapRGB(Surface->format, r, g, b);
    }
    break;
    case 4:
        return *((Uint32*)Surface->pixels + y * Surface->pitch/4 + x);
        break;
    }

    return 0;  // FIXME: Handle errors better
}

// From SDL_CreateTextureFromSurface
static GPU_Image* CopyImageFromSurface(GPU_Renderer* renderer, SDL_Surface* surface)
{
    const SDL_PixelFormat *fmt;
    Uint8 needAlpha;
    GPU_Image* image;
    int channels;

    if (!surface) {
        GPU_LogError("GPU_CopyImageFromSurface() passed NULL surface.\n");
        return NULL;
    }

    /* See what the best texture format is */
    fmt = surface->format;
    if (fmt->Amask || hasColorkey(surface)) {
        needAlpha = 1;
    } else {
        needAlpha = 0;
    }

    // Get appropriate storage format
    // TODO: More options would be nice...
    if(needAlpha)
    {
        channels = 4;
    }
    else
    {
        channels = 3;
    }
    
    //GPU_LogError("Format...  Channels: %d, BPP: %d, Masks: %X %X %X %X\n", channels, fmt->BytesPerPixel, fmt->Rmask, fmt->Gmask, fmt->Bmask, fmt->Amask);
    
    //Uint32 pix = getPixel(surface, surface->w/2, surface->h/2);
    //GPU_LogError("Middle pixel: %X\n", pix);
    image = CreateUninitializedImage(renderer, surface->w, surface->h, channels);
    if(image == NULL)
        return NULL;

    if(SDL_MUSTLOCK(surface))
    {
        SDL_LockSurface(surface);
        InitImageWithSurface(renderer, image, surface);
        SDL_UnlockSurface(surface);
    }
    else
        InitImageWithSurface(renderer, image, surface);

    return image;
}


static GPU_Image* CopyImageFromTarget(GPU_Renderer* renderer, GPU_Target* target)
{
    if(target == NULL)
        return NULL;
    
    SDL_Surface* surface = renderer->CopySurfaceFromTarget(renderer, target);
    GPU_Image* image = renderer->CopyImageFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
    
    return image;
}


static void FreeImage(GPU_Renderer* renderer, GPU_Image* image)
{
    if(image == NULL)
        return;
    
    if(image->refcount > 1)
    {
        image->refcount--;
        return;
    }

    // Delete the attached target first
    if(image->target != NULL)
        renderer->FreeTarget(renderer, image->target);

    flushAndClearBlitBufferIfCurrentTexture(renderer, image);
    glDeleteTextures( 1, &((GPU_IMAGE_DATA*)image->data)->handle);
    free(image->data);
    free(image);
}



static void SubSurfaceCopy(GPU_Renderer* renderer, SDL_Surface* src, GPU_Rect* srcrect, GPU_Target* dest, Sint16 x, Sint16 y)
{
    if(src == NULL || dest == NULL || dest->image == NULL)
        return;

    if(renderer != dest->renderer)
        return;

    GPU_Rect r;
    if(srcrect != NULL)
        r = *srcrect;
    else
    {
        r.x = 0;
        r.y = 0;
        r.w = src->w;
        r.h = src->h;
        if(r.w < 0.0f || r.h < 0.0f)
        {
            GPU_LogError("GPU_SubSurfaceCopy(): Given negative rectangle: %.2fx%.2f\n", r.w, r.h);
            return;
        }
    }

    bindTexture(renderer, dest->image);

    //GLenum texture_format = GL_RGBA;//((GPU_IMAGE_DATA*)image->data)->format;

    SDL_Surface* temp = SDL_CreateRGBSurface(SDL_SWSURFACE, r.w, r.h, src->format->BitsPerPixel, src->format->Rmask, src->format->Gmask, src->format->Bmask, src->format->Amask);

    if(temp == NULL)
    {
        GPU_LogError("GPU_SubSurfaceCopy(): Failed to create new %dx%d RGB surface.\n", (int)r.w, (int)r.h);
        return;
    }

    // Copy data to new surface
#ifdef SDL_GPU_USE_SDL2
    SDL_BlendMode blendmode;
    SDL_GetSurfaceBlendMode(src, &blendmode);
    SDL_SetSurfaceBlendMode(src, SDL_BLENDMODE_NONE);
#else
    Uint32 srcAlpha = src->flags & SDL_SRCALPHA;
    SDL_SetAlpha(src, 0, src->format->alpha);
#endif

    SDL_Rect destrect = {r.x, r.y, r.w, r.h};
    SDL_BlitSurface(src, &destrect, temp, NULL);
    // FIXME: What if destrect does not equal r anymore?

#ifdef SDL_GPU_USE_SDL2
    SDL_SetSurfaceBlendMode(src, blendmode);
#else
    SDL_SetAlpha(src, srcAlpha, src->format->alpha);
#endif

    // Make surface into an image
    GPU_Image* image = GPU_CopyImageFromSurface(temp);
    if(image == NULL)
    {
        GPU_LogError("GPU_SubSurfaceCopy(): Failed to create new image texture.\n");
        return;
    }

    // Copy image to dest
    GPU_FlushBlitBuffer();
    GPU_SetBlending(image, 0);
    GPU_Blit(image, NULL, dest, x + r.w/2, y + r.h/2);
    GPU_FlushBlitBuffer();

    // Using glTexSubImage might be more efficient
    //glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, r.w, r.h, texture_format, GL_UNSIGNED_BYTE, buffer);

    GPU_FreeImage(image);

    SDL_FreeSurface(temp);
}


static GPU_Target* LoadTarget(GPU_Renderer* renderer, GPU_Image* image)
{
    if(image == NULL)
        return NULL;

    if(image->target != NULL)
        return image->target;

    if(!(renderer->enabled_features & GPU_FEATURE_RENDER_TARGETS))
        return NULL;

    GLuint handle;
    // Create framebuffer object
    glGenFramebuffers(1, &handle);
    flushAndBindFramebuffer(renderer, handle);

    // Attach the texture to it
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ((GPU_IMAGE_DATA*)image->data)->handle, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if(status != GL_FRAMEBUFFER_COMPLETE)
        return NULL;

    GPU_Target* result = (GPU_Target*)malloc(sizeof(GPU_Target));
    GPU_TARGET_DATA* data = (GPU_TARGET_DATA*)malloc(sizeof(GPU_TARGET_DATA));
    result->data = data;
    data->handle = handle;
    data->format = ((GPU_IMAGE_DATA*)image->data)->format;
    
    result->renderer = renderer;
    result->context = NULL;
    result->image = image;
    result->w = image->w;
    result->h = image->h;
    
    result->viewport = GPU_MakeRect(0, 0, result->w, result->h);
    
    result->camera = GPU_GetDefaultCamera();
    
    result->use_clip_rect = 0;
    result->clip_rect.x = 0;
    result->clip_rect.y = 0;
    result->clip_rect.w = image->w;
    result->clip_rect.h = image->h;
    result->use_color = 0;

    image->target = result;
    return result;
}



static void FreeTarget(GPU_Renderer* renderer, GPU_Target* target)
{
    if(target == NULL)
        return;
    if(target == renderer->current_context_target)
    {
        renderer->FlushBlitBuffer(renderer);
        renderer->current_context_target = NULL;
    }

    GPU_TARGET_DATA* data = ((GPU_TARGET_DATA*)target->data);
    
    if(renderer->enabled_features & GPU_FEATURE_RENDER_TARGETS)
    {
        if(renderer->current_context_target != NULL)
            flushAndClearBlitBufferIfCurrentFramebuffer(renderer, target);
        if(data->handle != 0)
            glDeleteFramebuffers(1, &data->handle);
    }

    if(target->image != NULL)
        target->image->target = NULL;  // Remove reference to this object
    
    if(target->context != NULL)
    {
        GPU_CONTEXT_DATA* cdata = (GPU_CONTEXT_DATA*)target->context->data;
        
        free(cdata->blit_buffer);
        free(cdata->index_buffer);
    
        #ifdef SDL_GPU_USE_SDL2
        if(target->context->context != 0)
            SDL_GL_DeleteContext(target->context->context);
        #endif
    
        #ifdef SDL_GPU_USE_GL_TIER3
        if(data->handle != 0)
        {
            glDeleteBuffers(2, cdata->blit_VBO);
            glDeleteBuffers(16, cdata->attribute_VBO);
            #if !defined(SDL_GPU_NO_VAO)
            glDeleteVertexArrays(1, &cdata->blit_VAO);
            #endif
        }
        #endif
        
        free(target->context->data);
        free(target->context);
        target->context = NULL;
    }
    
    // Free specialized data
    
    free(target->data);
    target->data = NULL;
    free(target);
}



static int Blit(GPU_Renderer* renderer, GPU_Image* src, GPU_Rect* srcrect, GPU_Target* dest, float x, float y)
{
    if(src == NULL || dest == NULL)
        return -1;
    if(renderer != src->renderer || renderer != dest->renderer)
        return -2;
    
    makeContextCurrent(renderer, dest);
    if(renderer->current_context_target == NULL)
        return -3;

    // Bind the texture to which subsequent calls refer
    bindTexture(renderer, src);

    // Bind the FBO
    if(bindFramebuffer(renderer, dest))
    {
        prepareToRenderToTarget(renderer, dest);
        prepareToRenderImage(renderer, dest, src);
        
        Uint16 tex_w = src->texture_w;
        Uint16 tex_h = src->texture_h;
        
        if(src->filter_mode == GPU_NEAREST)
        {
            // Center the texels on the pixels
            x += 0.375f;
            y += 0.375f;
        }

        float x1, y1, x2, y2;
        float dx1, dy1, dx2, dy2;
        if(srcrect == NULL)
        {
            // Scale tex coords according to actual texture dims
            x1 = 0.0f;
            y1 = 0.0f;
            x2 = ((float)src->w)/tex_w;
            y2 = ((float)src->h)/tex_h;
            // Center the image on the given coords
            dx1 = x - src->w/2.0f;
            dy1 = y - src->h/2.0f;
            dx2 = x + src->w/2.0f;
            dy2 = y + src->h/2.0f;
        }
        else
        {
            // Scale srcrect tex coords according to actual texture dims
            x1 = srcrect->x/(float)tex_w;
            y1 = srcrect->y/(float)tex_h;
            x2 = (srcrect->x + srcrect->w)/(float)tex_w;
            y2 = (srcrect->y + srcrect->h)/(float)tex_h;
            // Center the image on the given coords
            dx1 = x - srcrect->w/2.0f;
            dy1 = y - srcrect->h/2.0f;
            dx2 = x + srcrect->w/2.0f;
            dy2 = y + srcrect->h/2.0f;
        }

        GPU_CONTEXT_DATA* cdata = (GPU_CONTEXT_DATA*)renderer->current_context_target->context->data;
        float* blit_buffer = cdata->blit_buffer;

        if(cdata->blit_buffer_num_vertices + GPU_BLIT_BUFFER_VERTICES_PER_SPRITE >= cdata->blit_buffer_max_num_vertices)
            renderer->FlushBlitBuffer(renderer);

        #ifdef SDL_GPU_USE_GL_TIER3
        int color_index = GPU_BLIT_BUFFER_COLOR_OFFSET + cdata->blit_buffer_num_vertices*GPU_BLIT_BUFFER_FLOATS_PER_VERTEX;
        float r =  src->color.r/255.0f;
        float g =  src->color.g/255.0f;
        float b =  src->color.b/255.0f;
        float a =  GET_ALPHA(src->color)/255.0f;
        #endif
        
        // Sprite quad vertices
        
        // Vertex 0
        int vert_index = GPU_BLIT_BUFFER_VERTEX_OFFSET + cdata->blit_buffer_num_vertices*GPU_BLIT_BUFFER_FLOATS_PER_VERTEX;
        int tex_index = GPU_BLIT_BUFFER_TEX_COORD_OFFSET + cdata->blit_buffer_num_vertices*GPU_BLIT_BUFFER_FLOATS_PER_VERTEX;
        blit_buffer[vert_index] = dx1;
        blit_buffer[vert_index+1] = dy1;
        blit_buffer[tex_index] = x1;
        blit_buffer[tex_index+1] = y1;
        #ifdef SDL_GPU_USE_GL_TIER3
        blit_buffer[color_index] = r;
        blit_buffer[color_index+1] = g;
        blit_buffer[color_index+2] = b;
        blit_buffer[color_index+3] = a;
        #endif
        
        // Vertex 1
        vert_index += GPU_BLIT_BUFFER_FLOATS_PER_VERTEX;
        tex_index += GPU_BLIT_BUFFER_FLOATS_PER_VERTEX;
        blit_buffer[vert_index] = dx2;
        blit_buffer[vert_index+1] = dy1;
        blit_buffer[tex_index] = x2;
        blit_buffer[tex_index+1] = y1;
        #ifdef SDL_GPU_USE_GL_TIER3
        color_index += GPU_BLIT_BUFFER_FLOATS_PER_VERTEX;
        blit_buffer[color_index] = r;
        blit_buffer[color_index+1] = g;
        blit_buffer[color_index+2] = b;
        blit_buffer[color_index+3] = a;
        #endif
        
        // Vertex 2
        vert_index += GPU_BLIT_BUFFER_FLOATS_PER_VERTEX;
        tex_index += GPU_BLIT_BUFFER_FLOATS_PER_VERTEX;
        blit_buffer[vert_index] = dx2;
        blit_buffer[vert_index+1] = dy2;
        blit_buffer[tex_index] = x2;
        blit_buffer[tex_index+1] = y2;
        #ifdef SDL_GPU_USE_GL_TIER3
        color_index += GPU_BLIT_BUFFER_FLOATS_PER_VERTEX;
        blit_buffer[color_index] = r;
        blit_buffer[color_index+1] = g;
        blit_buffer[color_index+2] = b;
        blit_buffer[color_index+3] = a;
        #endif
        
        // Vertex 3
        vert_index += GPU_BLIT_BUFFER_FLOATS_PER_VERTEX;
        tex_index += GPU_BLIT_BUFFER_FLOATS_PER_VERTEX;
        blit_buffer[vert_index] = dx1;
        blit_buffer[vert_index+1] = dy2;
        blit_buffer[tex_index] = x1;
        blit_buffer[tex_index+1] = y2;
        #ifdef SDL_GPU_USE_GL_TIER3
        color_index += GPU_BLIT_BUFFER_FLOATS_PER_VERTEX;
        blit_buffer[color_index] = r;
        blit_buffer[color_index+1] = g;
        blit_buffer[color_index+2] = b;
        blit_buffer[color_index+3] = a;
        #endif

        // Triangle indices
        cdata->index_buffer_num_vertices += 6;

        cdata->blit_buffer_num_vertices += GPU_BLIT_BUFFER_VERTICES_PER_SPRITE;
    }

    return 0;
}


static int BlitRotate(GPU_Renderer* renderer, GPU_Image* src, GPU_Rect* srcrect, GPU_Target* dest, float x, float y, float angle)
{
    if(src == NULL || dest == NULL)
        return -1;

    return renderer->BlitTransformX(renderer, src, srcrect, dest, x, y, src->w/2.0f, src->h/2.0f, angle, 1.0f, 1.0f);
}

static int BlitScale(GPU_Renderer* renderer, GPU_Image* src, GPU_Rect* srcrect, GPU_Target* dest, float x, float y, float scaleX, float scaleY)
{
    if(src == NULL || dest == NULL)
        return -1;

    return renderer->BlitTransformX(renderer, src, srcrect, dest, x, y, src->w/2.0f, src->h/2.0f, 0.0f, scaleX, scaleY);
}

static int BlitTransform(GPU_Renderer* renderer, GPU_Image* src, GPU_Rect* srcrect, GPU_Target* dest, float x, float y, float angle, float scaleX, float scaleY)
{
    if(src == NULL || dest == NULL)
        return -1;

    return renderer->BlitTransformX(renderer, src, srcrect, dest, x, y, src->w/2.0f, src->h/2.0f, angle, scaleX, scaleY);
}

static int BlitTransformX(GPU_Renderer* renderer, GPU_Image* src, GPU_Rect* srcrect, GPU_Target* dest, float x, float y, float pivot_x, float pivot_y, float angle, float scaleX, float scaleY)
{
    if(src == NULL || dest == NULL)
        return -1;
    if(renderer != src->renderer || renderer != dest->renderer)
        return -2;


    makeContextCurrent(renderer, dest);
    
    // Bind the texture to which subsequent calls refer
    bindTexture(renderer, src);

    // Bind the FBO
    if(bindFramebuffer(renderer, dest))
    {
        prepareToRenderToTarget(renderer, dest);
        prepareToRenderImage(renderer, dest, src);
        
        Uint16 tex_w = src->texture_w;
        Uint16 tex_h = src->texture_h;
        
        if(src->filter_mode == GPU_NEAREST)
        {
            // Center the texels on the pixels
            x += 0.375f;
            y += 0.375f;
        }

        float x1, y1, x2, y2;
        /*
            1,1 --- 3,3
             |       |
             |       |
            4,4 --- 2,2
        */
        float dx1, dy1, dx2, dy2, dx3, dy3, dx4, dy4;
        if(srcrect == NULL)
        {
            // Scale tex coords according to actual texture dims
            x1 = 0.0f;
            y1 = 0.0f;
            x2 = ((float)src->w)/tex_w;
            y2 = ((float)src->h)/tex_h;
            // Center the image on the given coords
            dx1 = -src->w/2.0f;
            dy1 = -src->h/2.0f;
            dx2 = src->w/2.0f;
            dy2 = src->h/2.0f;
        }
        else
        {
            // Scale srcrect tex coords according to actual texture dims
            x1 = srcrect->x/(float)tex_w;
            y1 = srcrect->y/(float)tex_h;
            x2 = (srcrect->x + srcrect->w)/(float)tex_w;
            y2 = (srcrect->y + srcrect->h)/(float)tex_h;
            // Center the image on the given coords
            dx1 = -srcrect->w/2.0f;
            dy1 = -srcrect->h/2.0f;
            dx2 = srcrect->w/2.0f;
            dy2 = srcrect->h/2.0f;
        }

        // Apply transforms

        // Scale
        if(scaleX != 1.0f || scaleY != 1.0f)
        {
            float w = (dx2 - dx1)*scaleX;
            float h = (dy2 - dy1)*scaleY;
            dx1 = (dx2 + dx1)/2 - w/2;
            dx2 = dx1 + w;
            dy1 = (dy2 + dy1)/2 - h/2;
            dy2 = dy1 + h;
        }

        // Shift away from the center (these are relative to the image corner)
        pivot_x -= src->w/2.0f;
        pivot_y -= src->h/2.0f;

        // Translate origin to pivot
        dx1 -= pivot_x*scaleX;
        dy1 -= pivot_y*scaleY;
        dx2 -= pivot_x*scaleX;
        dy2 -= pivot_y*scaleY;

        // Get extra vertices for rotation
        dx3 = dx2;
        dy3 = dy1;
        dx4 = dx1;
        dy4 = dy2;

        // Rotate about origin (the pivot)
        if(angle != 0.0f)
        {
            float cosA = cos(angle*M_PI/180);
            float sinA = sin(angle*M_PI/180);
            float tempX = dx1;
            dx1 = dx1*cosA - dy1*sinA;
            dy1 = tempX*sinA + dy1*cosA;
            tempX = dx2;
            dx2 = dx2*cosA - dy2*sinA;
            dy2 = tempX*sinA + dy2*cosA;
            tempX = dx3;
            dx3 = dx3*cosA - dy3*sinA;
            dy3 = tempX*sinA + dy3*cosA;
            tempX = dx4;
            dx4 = dx4*cosA - dy4*sinA;
            dy4 = tempX*sinA + dy4*cosA;
        }

        // Translate to pos
        dx1 += x;
        dx2 += x;
        dx3 += x;
        dx4 += x;
        dy1 += y;
        dy2 += y;
        dy3 += y;
        dy4 += y;

        GPU_CONTEXT_DATA* cdata = (GPU_CONTEXT_DATA*)renderer->current_context_target->context->data;
        float* blit_buffer = cdata->blit_buffer;

        if(cdata->blit_buffer_num_vertices + GPU_BLIT_BUFFER_VERTICES_PER_SPRITE >= cdata->blit_buffer_max_num_vertices)
            renderer->FlushBlitBuffer(renderer);

        #ifdef SDL_GPU_USE_GL_TIER3
        int color_index = GPU_BLIT_BUFFER_COLOR_OFFSET + cdata->blit_buffer_num_vertices*GPU_BLIT_BUFFER_FLOATS_PER_VERTEX;
        float r =  src->color.r/255.0f;
        float g =  src->color.g/255.0f;
        float b =  src->color.b/255.0f;
        float a =  GET_ALPHA(src->color)/255.0f;
        #endif

        // Sprite quad vertices
        
        // Vertex 0
        int vert_index = GPU_BLIT_BUFFER_VERTEX_OFFSET + cdata->blit_buffer_num_vertices*GPU_BLIT_BUFFER_FLOATS_PER_VERTEX;
        int tex_index = GPU_BLIT_BUFFER_TEX_COORD_OFFSET + cdata->blit_buffer_num_vertices*GPU_BLIT_BUFFER_FLOATS_PER_VERTEX;
        blit_buffer[vert_index] = dx1;
        blit_buffer[vert_index+1] = dy1;
        blit_buffer[tex_index] = x1;
        blit_buffer[tex_index+1] = y1;
        #ifdef SDL_GPU_USE_GL_TIER3
        blit_buffer[color_index] = r;
        blit_buffer[color_index+1] = g;
        blit_buffer[color_index+2] = b;
        blit_buffer[color_index+3] = a;
        #endif

        // Vertex 1
        vert_index += GPU_BLIT_BUFFER_FLOATS_PER_VERTEX;
        tex_index += GPU_BLIT_BUFFER_FLOATS_PER_VERTEX;
        blit_buffer[vert_index] = dx3;
        blit_buffer[vert_index+1] = dy3;
        blit_buffer[tex_index] = x2;
        blit_buffer[tex_index+1] = y1;
        #ifdef SDL_GPU_USE_GL_TIER3
        color_index += GPU_BLIT_BUFFER_FLOATS_PER_VERTEX;
        blit_buffer[color_index] = r;
        blit_buffer[color_index+1] = g;
        blit_buffer[color_index+2] = b;
        blit_buffer[color_index+3] = a;
        #endif

        // Vertex 2
        vert_index += GPU_BLIT_BUFFER_FLOATS_PER_VERTEX;
        tex_index += GPU_BLIT_BUFFER_FLOATS_PER_VERTEX;
        blit_buffer[vert_index] = dx2;
        blit_buffer[vert_index+1] = dy2;
        blit_buffer[tex_index] = x2;
        blit_buffer[tex_index+1] = y2;
        #ifdef SDL_GPU_USE_GL_TIER3
        color_index += GPU_BLIT_BUFFER_FLOATS_PER_VERTEX;
        blit_buffer[color_index] = r;
        blit_buffer[color_index+1] = g;
        blit_buffer[color_index+2] = b;
        blit_buffer[color_index+3] = a;
        #endif

        // Vertex 3
        vert_index += GPU_BLIT_BUFFER_FLOATS_PER_VERTEX;
        tex_index += GPU_BLIT_BUFFER_FLOATS_PER_VERTEX;
        blit_buffer[vert_index] = dx4;
        blit_buffer[vert_index+1] = dy4;
        blit_buffer[tex_index] = x1;
        blit_buffer[tex_index+1] = y2;
        #ifdef SDL_GPU_USE_GL_TIER3
        color_index += GPU_BLIT_BUFFER_FLOATS_PER_VERTEX;
        blit_buffer[color_index] = r;
        blit_buffer[color_index+1] = g;
        blit_buffer[color_index+2] = b;
        blit_buffer[color_index+3] = a;
        #endif
        

        // Triangle indices
        cdata->index_buffer_num_vertices += 6;

        cdata->blit_buffer_num_vertices += GPU_BLIT_BUFFER_VERTICES_PER_SPRITE;
    }

    return 0;
}

static int BlitTransformMatrix(GPU_Renderer* renderer, GPU_Image* src, GPU_Rect* srcrect, GPU_Target* dest, float x, float y, float* matrix3x3)
{
    if(src == NULL || dest == NULL)
        return -1;
    if(renderer != src->renderer || renderer != dest->renderer)
        return -2;
    
    // TODO: See below.
    renderer->FlushBlitBuffer(renderer);
    
    GPU_PushMatrix();

    // column-major 3x3 to column-major 4x4 (and scooting the 2D translations to the homogeneous column)
    // TODO: Should index 8 replace the homogeneous 1?  This looks like it adjusts the z-value...
    float matrix[16] = {matrix3x3[0], matrix3x3[1], matrix3x3[2], 0,
                        matrix3x3[3], matrix3x3[4], matrix3x3[5], 0,
                        0,            0,            matrix3x3[8], 0,
                        matrix3x3[6], matrix3x3[7], 0,            1
                       };
    GPU_Translate(x, y, 0);
    GPU_MultMatrix(matrix);

    int result = renderer->Blit(renderer, src, srcrect, dest, 0, 0);
    
    // Popping the matrix will revert the transform before it can be used, so we have to flush for now.
    // TODO: Do the matrix math myself on the vertex coords.
    renderer->FlushBlitBuffer(renderer);

    GPU_PopMatrix();

    return result;
}



#ifdef SDL_GPU_USE_GL_TIER3


static inline int sizeof_GPU_type(GPU_TypeEnum type)
{
    if(type == GPU_DOUBLE) return sizeof(double);
    if(type == GPU_FLOAT) return sizeof(float);
    if(type == GPU_INT) return sizeof(int);
    if(type == GPU_UNSIGNED_INT) return sizeof(unsigned int);
    if(type == GPU_SHORT) return sizeof(short);
    if(type == GPU_UNSIGNED_SHORT) return sizeof(unsigned short);
    if(type == GPU_BYTE) return sizeof(char);
    if(type == GPU_UNSIGNED_BYTE) return sizeof(unsigned char);
    return 0;
}

static void refresh_attribute_data(GPU_CONTEXT_DATA* cdata)
{
    int i;
    for(i = 0; i < 16; i++)
    {
        GPU_AttributeSource* a = &cdata->shader_attributes[i];
        if(a->attribute.values != NULL && a->attribute.location >= 0 && a->num_values > 0 && a->attribute.format.is_per_sprite)
        {
            // Expand the values to 4 vertices
            int n;
            void* storage_ptr = a->per_vertex_storage;
            void* values_ptr = (void*)((char*)a->attribute.values + a->attribute.format.offset_bytes);
            int value_size_bytes = a->attribute.format.num_elems_per_value * sizeof_GPU_type(a->attribute.format.type);
            for(n = 0; n < a->num_values; n+=4)
            {
                memcpy(storage_ptr, values_ptr, value_size_bytes);
                storage_ptr = (void*)((char*)storage_ptr + a->per_vertex_storage_stride_bytes);
                memcpy(storage_ptr, values_ptr, value_size_bytes);
                storage_ptr = (void*)((char*)storage_ptr + a->per_vertex_storage_stride_bytes);
                memcpy(storage_ptr, values_ptr, value_size_bytes);
                storage_ptr = (void*)((char*)storage_ptr + a->per_vertex_storage_stride_bytes);
                memcpy(storage_ptr, values_ptr, value_size_bytes);
                storage_ptr = (void*)((char*)storage_ptr + a->per_vertex_storage_stride_bytes);
                
                values_ptr = (void*)((char*)values_ptr + a->attribute.format.stride_bytes);
            }
        }
    }
}

static void upload_attribute_data(GPU_CONTEXT_DATA* cdata, int num_vertices)
{
    int i;
    for(i = 0; i < 16; i++)
    {
        GPU_AttributeSource* a = &cdata->shader_attributes[i];
        if(a->attribute.values != NULL && a->attribute.location >= 0 && a->num_values > 0)
        {
            int num_values_used = num_vertices;
            if(a->num_values < num_values_used)
                num_values_used = a->num_values;
            
            glBindBuffer(GL_ARRAY_BUFFER, cdata->attribute_VBO[i]);
            
            int bytes_used = a->per_vertex_storage_stride_bytes * num_values_used;
            glBufferData(GL_ARRAY_BUFFER, bytes_used, a->next_value, GL_STREAM_DRAW);
            
            glEnableVertexAttribArray(a->attribute.location);
            glVertexAttribPointer(a->attribute.location, a->attribute.format.num_elems_per_value, a->attribute.format.type, a->attribute.format.normalize, a->per_vertex_storage_stride_bytes, (void*)a->per_vertex_storage_offset_bytes);
            
            a->enabled = 1;
            // Move the data along so we use the next values for the next flush
            a->num_values -= num_values_used;
            if(a->num_values <= 0)
                a->next_value = a->per_vertex_storage;
            else
                a->next_value = (void*)(((char*)a->next_value) + bytes_used);
        }
    }
}

static void disable_attribute_data(GPU_CONTEXT_DATA* cdata)
{
    int i;
    for(i = 0; i < 16; i++)
    {
        GPU_AttributeSource* a = &cdata->shader_attributes[i];
        if(a->enabled)
        {
            glDisableVertexAttribArray(a->attribute.location);
            a->enabled = 0;
        }
    }
}

#endif

static int get_lowest_attribute_num_values(GPU_CONTEXT_DATA* cdata, int cap)
{
    int lowest = cap;
    
#ifdef SDL_GPU_USE_GL_TIER3
    int i;
    for(i = 0; i < 16; i++)
    {
        GPU_AttributeSource* a = &cdata->shader_attributes[i];
        if(a->attribute.values != NULL && a->attribute.location >= 0)
        {
            if(a->num_values < lowest)
                lowest = a->num_values;
        }
    }
#endif
    
    return lowest;
}




static int BlitBatch(GPU_Renderer* renderer, GPU_Image* src, GPU_Target* dest, unsigned int numSprites, float* values, GPU_BlitFlagEnum flags)
{
    if(src == NULL || dest == NULL)
        return -1;
    if(renderer != src->renderer || renderer != dest->renderer)
        return -2;
    
    makeContextCurrent(renderer, dest);

    // Bind the texture to which subsequent calls refer
    bindTexture(renderer, src);

    // Bind the FBO
    if(bindFramebuffer(renderer, dest))
    {
        prepareToRenderToTarget(renderer, dest);
        prepareToRenderImage(renderer, dest, src);
        changeViewport(dest);
        
        glEnable(GL_TEXTURE_2D);
        Uint8 isRTT = (dest->image != NULL);
        
        // Modify the projection matrix if rendering to a texture
        if(isRTT)
        {
            GPU_MatrixMode( GPU_PROJECTION );
            GPU_PushMatrix();
            GPU_LoadIdentity();

            GPU_Ortho(0.0f, dest->w, 0.0f, dest->h, -1.0f, 1.0f); // Special inverted orthographic projection because tex coords are inverted already.

            GPU_MatrixMode( GPU_MODELVIEW );
        }

        setClipRect(renderer, dest);
        
        #ifdef SDL_GPU_APPLY_TRANSFORMS_TO_GL_STACK
        //if(!renderer->IsFeatureEnabled(GPU_FEATURE_VERTEX_SHADER))
            applyTransforms();
        #endif
        

        GPU_CONTEXT_DATA* cdata = (GPU_CONTEXT_DATA*)renderer->current_context_target->context->data;

        renderer->FlushBlitBuffer(renderer);
        
        #ifdef SDL_GPU_USE_GL_TIER3
        refresh_attribute_data(cdata);
        #endif
        
        int floats_per_vertex = 8;
        
        // Only do so many at a time
        int partial_num_sprites = cdata->blit_buffer_max_num_vertices/4;
        while(1)
        {
            if(numSprites < partial_num_sprites)
                partial_num_sprites = numSprites;
            if(partial_num_sprites <= 0)
                break;

            // Triangle indices
            cdata->index_buffer_num_vertices += 6*partial_num_sprites;
            
    #ifdef SDL_GPU_USE_GL_TIER1

            float* vertex_pointer = values;
            float* texcoord_pointer = values + 2;
            float* color_pointer = values + 4;
            
            glBegin(GL_QUADS);
            int i;
            for(i = 0; i < numSprites; i++)
            {
                glColor4f( *color_pointer, *(color_pointer+1), *(color_pointer+2), *(color_pointer+3) );
                glTexCoord2f( *texcoord_pointer, *(texcoord_pointer+1) );
                glVertex3f( *vertex_pointer, *(vertex_pointer+1), 0.0f );
                color_pointer += floats_per_vertex;
                texcoord_pointer += floats_per_vertex;
                vertex_pointer += floats_per_vertex;

                glColor4f( *color_pointer, *(color_pointer+1), *(color_pointer+2), *(color_pointer+3) );
                glTexCoord2f( *texcoord_pointer, *(texcoord_pointer+1) );
                glVertex3f( *vertex_pointer, *(vertex_pointer+1), 0.0f );
                color_pointer += floats_per_vertex;
                texcoord_pointer += floats_per_vertex;
                vertex_pointer += floats_per_vertex;

                glColor4f( *color_pointer, *(color_pointer+1), *(color_pointer+2), *(color_pointer+3) );
                glTexCoord2f( *texcoord_pointer, *(texcoord_pointer+1) );
                glVertex3f( *vertex_pointer, *(vertex_pointer+1), 0.0f );
                color_pointer += floats_per_vertex;
                texcoord_pointer += floats_per_vertex;
                vertex_pointer += floats_per_vertex;

                glColor4f( *color_pointer, *(color_pointer+1), *(color_pointer+2), *(color_pointer+3) );
                glTexCoord2f( *texcoord_pointer, *(texcoord_pointer+1) );
                glVertex3f( *vertex_pointer, *(vertex_pointer+1), 0.0f );
                color_pointer += floats_per_vertex;
                texcoord_pointer += floats_per_vertex;
                vertex_pointer += floats_per_vertex;
            }
            glEnd();
    #elif defined(SDL_GPU_USE_GL_TIER2)

            glEnableClientState(GL_VERTEX_ARRAY);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glEnableClientState(GL_COLOR_ARRAY);
            
            int stride = 8*sizeof(float);
            glVertexPointer(2, GL_FLOAT, stride, values + GPU_BLIT_BUFFER_VERTEX_OFFSET);
            glTexCoordPointer(2, GL_FLOAT, stride, values + GPU_BLIT_BUFFER_TEX_COORD_OFFSET);
            glColorPointer(4, GL_FLOAT, stride, values + GPU_BLIT_BUFFER_COLOR_OFFSET);

            glDrawElements(GL_TRIANGLES, cdata->index_buffer_num_vertices, GL_UNSIGNED_SHORT, cdata->index_buffer);

            glDisableClientState(GL_COLOR_ARRAY);
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            glDisableClientState(GL_VERTEX_ARRAY);

    #elif defined(SDL_GPU_USE_GL_TIER3)
            
            // Upload our modelviewprojection matrix
            if(cdata->current_shader_block.modelViewProjection_loc >= 0)
            {
                float mvp[16];
                GPU_GetModelViewProjection(mvp);
                glUniformMatrix4fv(cdata->current_shader_block.modelViewProjection_loc, 1, 0, mvp);
            }
        
            // Update the vertex array object's buffers
            #if !defined(SDL_GPU_NO_VAO)
            glBindVertexArray(cdata->blit_VAO);
            #endif
            
            if(values != NULL)
            {
                // Upload blit buffer to a single buffer object
                glBindBuffer(GL_ARRAY_BUFFER, cdata->blit_VBO[cdata->blit_VBO_flop]);
                cdata->blit_VBO_flop = !cdata->blit_VBO_flop;
                
                // Copy the whole blit buffer to the GPU
                glBufferSubData(GL_ARRAY_BUFFER, 0, GPU_BLIT_BUFFER_STRIDE * (partial_num_sprites*4), values);  // Fills GPU buffer with data.
                
                // Specify the formatting of the blit buffer
                if(cdata->current_shader_block.position_loc >= 0)
                {
                    glEnableVertexAttribArray(cdata->current_shader_block.position_loc);  // Tell GL to use client-side attribute data
                    glVertexAttribPointer(cdata->current_shader_block.position_loc, 2, GL_FLOAT, GL_FALSE, GPU_BLIT_BUFFER_STRIDE, 0);  // Tell how the data is formatted
                }
                if(cdata->current_shader_block.texcoord_loc >= 0)
                {
                    glEnableVertexAttribArray(cdata->current_shader_block.texcoord_loc);
                    glVertexAttribPointer(cdata->current_shader_block.texcoord_loc, 2, GL_FLOAT, GL_FALSE, GPU_BLIT_BUFFER_STRIDE, (void*)(GPU_BLIT_BUFFER_TEX_COORD_OFFSET * sizeof(float)));
                }
                if(cdata->current_shader_block.color_loc >= 0)
                {
                    glEnableVertexAttribArray(cdata->current_shader_block.color_loc);
                    glVertexAttribPointer(cdata->current_shader_block.color_loc, 4, GL_FLOAT, GL_FALSE, GPU_BLIT_BUFFER_STRIDE, (void*)(GPU_BLIT_BUFFER_COLOR_OFFSET * sizeof(float)));
                }
            }
            
            upload_attribute_data(cdata, partial_num_sprites*4);
            
            glDrawElements(GL_TRIANGLES, cdata->index_buffer_num_vertices, GL_UNSIGNED_SHORT, cdata->index_buffer);
            
            // Disable the vertex arrays again
            if(cdata->current_shader_block.position_loc >= 0)
                glDisableVertexAttribArray(cdata->current_shader_block.position_loc);
            if(cdata->current_shader_block.texcoord_loc >= 0)
                glDisableVertexAttribArray(cdata->current_shader_block.texcoord_loc);
            if(cdata->current_shader_block.color_loc >= 0)
                glDisableVertexAttribArray(cdata->current_shader_block.color_loc);
            
            disable_attribute_data(cdata);
            
            #if !defined(SDL_GPU_NO_VAO)
            glBindVertexArray(0);
            #endif

    #endif

            values += partial_num_sprites*4*floats_per_vertex;
            
            numSprites -= partial_num_sprites;
            
            cdata->blit_buffer_num_vertices = 0;
            cdata->index_buffer_num_vertices = 0;
        }

        unsetClipRect(renderer, dest);

        // restore matrices
        if(isRTT)
        {
            GPU_MatrixMode( GPU_PROJECTION );
            GPU_PopMatrix();
            GPU_MatrixMode( GPU_MODELVIEW );
        }
    }

    return 0;
}

static void GenerateMipmaps(GPU_Renderer* renderer, GPU_Image* image)
{
    #ifndef __IPHONEOS__
    if(image == NULL)
        return;
    
    if(image->target != NULL && isCurrentTarget(renderer, image->target))
        renderer->FlushBlitBuffer(renderer);
    bindTexture(renderer, image);
    glGenerateMipmap(GL_TEXTURE_2D);
    image->has_mipmaps = 1;

    GLint filter;
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &filter);
    if(filter == GL_LINEAR)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
    #endif
}




static GPU_Rect SetClip(GPU_Renderer* renderer, GPU_Target* target, Sint16 x, Sint16 y, Uint16 w, Uint16 h)
{
    if(target == NULL)
    {
        GPU_Rect r = {0,0,0,0};
        return r;
    }

    if(isCurrentTarget(renderer, target))
        renderer->FlushBlitBuffer(renderer);
    target->use_clip_rect = 1;

    GPU_Rect r = target->clip_rect;

    target->clip_rect.x = x;
    target->clip_rect.y = y;
    target->clip_rect.w = w;
    target->clip_rect.h = h;

    return r;
}

static void ClearClip(GPU_Renderer* renderer, GPU_Target* target)
{
    if(target == NULL)
        return;

    makeContextCurrent(renderer, target);
    
    if(isCurrentTarget(renderer, target))
        renderer->FlushBlitBuffer(renderer);
    target->use_clip_rect = 0;
    target->clip_rect.x = 0;
    target->clip_rect.y = 0;
    target->clip_rect.w = target->w;
    target->clip_rect.h = target->h;
}






static SDL_Color GetPixel(GPU_Renderer* renderer, GPU_Target* target, Sint16 x, Sint16 y)
{
    SDL_Color result = {0,0,0,0};
    if(target == NULL)
        return result;
    if(renderer != target->renderer)
        return result;
    if(x < 0 || y < 0 || x >= target->w || y >= target->h)
        return result;

    if(isCurrentTarget(renderer, target))
        renderer->FlushBlitBuffer(renderer);
    if(bindFramebuffer(renderer, target))
    {
        unsigned char pixels[4];
        glReadPixels(x, y, 1, 1, ((GPU_TARGET_DATA*)target->data)->format, GL_UNSIGNED_BYTE, pixels);

        result.r = pixels[0];
        result.g = pixels[1];
        result.b = pixels[2];
        GET_ALPHA(result) = pixels[3];
    }

    return result;
}

static void SetImageFilter(GPU_Renderer* renderer, GPU_Image* image, GPU_FilterEnum filter)
{
    if(image == NULL)
        return;
    if(renderer != image->renderer)
        return;

    bindTexture(renderer, image);

    GLenum minFilter = GL_NEAREST;
    GLenum magFilter = GL_NEAREST;

    if(filter == GPU_LINEAR)
    {
        if(image->has_mipmaps)
            minFilter = GL_LINEAR_MIPMAP_NEAREST;
        else
            minFilter = GL_LINEAR;

        magFilter = GL_LINEAR;
    }
    else if(filter == GPU_LINEAR_MIPMAP)
    {
        if(image->has_mipmaps)
            minFilter = GL_LINEAR_MIPMAP_LINEAR;
        else
            minFilter = GL_LINEAR;

        magFilter = GL_LINEAR;
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);
}






static void Clear(GPU_Renderer* renderer, GPU_Target* target)
{
    if(target == NULL)
        return;
    if(renderer != target->renderer)
        return;

    makeContextCurrent(renderer, target);
    
    if(isCurrentTarget(renderer, target))
        renderer->FlushBlitBuffer(renderer);
    if(bindFramebuffer(renderer, target))
    {
        setClipRect(renderer, target);

        //glPushAttrib(GL_COLOR_BUFFER_BIT);

        glClearColor(0,0,0,0);
        glClear(GL_COLOR_BUFFER_BIT);
        //glPopAttrib();

        unsetClipRect(renderer, target);
    }
}


static void ClearRGBA(GPU_Renderer* renderer, GPU_Target* target, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    if(target == NULL)
        return;
    if(renderer != target->renderer)
        return;

    makeContextCurrent(renderer, target);
    
    if(isCurrentTarget(renderer, target))
        renderer->FlushBlitBuffer(renderer);
    if(bindFramebuffer(renderer, target))
    {
        setClipRect(renderer, target);

        glClearColor(r/255.0f, g/255.0f, b/255.0f, a/255.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        unsetClipRect(renderer, target);
    }
}

static void DoPartialFlush(GPU_CONTEXT_DATA* cdata, int num_vertices, float* blit_buffer, int num_indices, unsigned short* index_buffer)
{
#ifdef SDL_GPU_USE_GL_TIER1

        float* vertex_pointer = blit_buffer + GPU_BLIT_BUFFER_VERTEX_OFFSET;
        float* texcoord_pointer = blit_buffer + GPU_BLIT_BUFFER_TEX_COORD_OFFSET;
        
        glBegin(GL_QUADS);
        int i;
        for(i = 0; i < num_vertices; i += GPU_BLIT_BUFFER_VERTICES_PER_SPRITE)
        {
            glTexCoord2f( *texcoord_pointer, *(texcoord_pointer+1) );
            glVertex3f( *vertex_pointer, *(vertex_pointer+1), 0.0f );
            texcoord_pointer += GPU_BLIT_BUFFER_FLOATS_PER_VERTEX;
            vertex_pointer += GPU_BLIT_BUFFER_FLOATS_PER_VERTEX;

            glTexCoord2f( *texcoord_pointer, *(texcoord_pointer+1) );
            glVertex3f( *vertex_pointer, *(vertex_pointer+1), 0.0f );
            texcoord_pointer += GPU_BLIT_BUFFER_FLOATS_PER_VERTEX;
            vertex_pointer += GPU_BLIT_BUFFER_FLOATS_PER_VERTEX;

            glTexCoord2f( *texcoord_pointer, *(texcoord_pointer+1) );
            glVertex3f( *vertex_pointer, *(vertex_pointer+1), 0.0f );
            texcoord_pointer += GPU_BLIT_BUFFER_FLOATS_PER_VERTEX;
            vertex_pointer += GPU_BLIT_BUFFER_FLOATS_PER_VERTEX;

            glTexCoord2f( *texcoord_pointer, *(texcoord_pointer+1) );
            glVertex3f( *vertex_pointer, *(vertex_pointer+1), 0.0f );
            texcoord_pointer += GPU_BLIT_BUFFER_FLOATS_PER_VERTEX;
            vertex_pointer += GPU_BLIT_BUFFER_FLOATS_PER_VERTEX;
        }
        glEnd();
#elif defined(SDL_GPU_USE_GL_TIER2)

        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glVertexPointer(2, GL_FLOAT, GPU_BLIT_BUFFER_STRIDE, blit_buffer + GPU_BLIT_BUFFER_VERTEX_OFFSET);
        glTexCoordPointer(2, GL_FLOAT, GPU_BLIT_BUFFER_STRIDE, blit_buffer + GPU_BLIT_BUFFER_TEX_COORD_OFFSET);

        glDrawElements(GL_TRIANGLES, num_indices, GL_UNSIGNED_SHORT, index_buffer);

        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDisableClientState(GL_VERTEX_ARRAY);

#elif defined(SDL_GPU_USE_GL_TIER3)
        
        // Upload our modelviewprojection matrix
        if(cdata->current_shader_block.modelViewProjection_loc >= 0)
        {
            float mvp[16];
            GPU_GetModelViewProjection(mvp);
            glUniformMatrix4fv(cdata->current_shader_block.modelViewProjection_loc, 1, 0, mvp);
        }
    
        // Update the vertex array object's buffers
        #if !defined(SDL_GPU_NO_VAO)
        glBindVertexArray(cdata->blit_VAO);
        #endif
        
        // Upload blit buffer to a single buffer object
        glBindBuffer(GL_ARRAY_BUFFER, cdata->blit_VBO[cdata->blit_VBO_flop]);
        cdata->blit_VBO_flop = !cdata->blit_VBO_flop;
        
        // Copy the whole blit buffer to the GPU
        glBufferSubData(GL_ARRAY_BUFFER, 0, GPU_BLIT_BUFFER_STRIDE * num_vertices, blit_buffer);  // Fills GPU buffer with data.
        
        // Specify the formatting of the blit buffer
        if(cdata->current_shader_block.position_loc >= 0)
        {
            glEnableVertexAttribArray(cdata->current_shader_block.position_loc);  // Tell GL to use client-side attribute data
            glVertexAttribPointer(cdata->current_shader_block.position_loc, 2, GL_FLOAT, GL_FALSE, GPU_BLIT_BUFFER_STRIDE, 0);  // Tell how the data is formatted
        }
        if(cdata->current_shader_block.texcoord_loc >= 0)
        {
            glEnableVertexAttribArray(cdata->current_shader_block.texcoord_loc);
            glVertexAttribPointer(cdata->current_shader_block.texcoord_loc, 2, GL_FLOAT, GL_FALSE, GPU_BLIT_BUFFER_STRIDE, (void*)(GPU_BLIT_BUFFER_TEX_COORD_OFFSET * sizeof(float)));
        }
        if(cdata->current_shader_block.color_loc >= 0)
        {
            glEnableVertexAttribArray(cdata->current_shader_block.color_loc);
            glVertexAttribPointer(cdata->current_shader_block.color_loc, 4, GL_FLOAT, GL_FALSE, GPU_BLIT_BUFFER_STRIDE, (void*)(GPU_BLIT_BUFFER_COLOR_OFFSET * sizeof(float)));
        }
        
        upload_attribute_data(cdata, num_vertices);
        
        glDrawElements(GL_TRIANGLES, num_indices, GL_UNSIGNED_SHORT, index_buffer);
        
        // Disable the vertex arrays again
        if(cdata->current_shader_block.position_loc >= 0)
            glDisableVertexAttribArray(cdata->current_shader_block.position_loc);
        if(cdata->current_shader_block.texcoord_loc >= 0)
            glDisableVertexAttribArray(cdata->current_shader_block.texcoord_loc);
        if(cdata->current_shader_block.color_loc >= 0)
            glDisableVertexAttribArray(cdata->current_shader_block.color_loc);
        
        disable_attribute_data(cdata);
        
        #if !defined(SDL_GPU_NO_VAO)
        glBindVertexArray(0);
        #endif

#endif
}

#define MAX(a, b) ((a) > (b)? (a) : (b))

static void FlushBlitBuffer(GPU_Renderer* renderer)
{
    GPU_CONTEXT_DATA* cdata = (GPU_CONTEXT_DATA*)renderer->current_context_target->context->data;
    if(cdata->blit_buffer_num_vertices > 0 && cdata->last_target != NULL && cdata->last_image != NULL)
    {
        GPU_Target* dest = cdata->last_target;
        
        changeViewport(dest);
        
        glEnable(GL_TEXTURE_2D);
        Uint8 isRTT = (dest->image != NULL);

        // Modify the projection matrix if rendering to a texture
        if(isRTT)
        {
            GPU_MatrixMode( GPU_PROJECTION );
            GPU_PushMatrix();
            GPU_LoadIdentity();

            GPU_Ortho(0.0f, dest->w, 0.0f, dest->h, -1.0f, 1.0f); // Special inverted orthographic projection because tex coords are inverted already.

            GPU_MatrixMode( GPU_MODELVIEW );
        }
        
        #ifdef SDL_GPU_APPLY_TRANSFORMS_TO_GL_STACK
        //if(!renderer->IsFeatureEnabled(GPU_FEATURE_VERTEX_SHADER))
            applyTransforms();
        #endif
        
        setClipRect(renderer, dest);
        
        #ifdef SDL_GPU_USE_GL_TIER3
        refresh_attribute_data(cdata);
        #endif
        
        int num_vertices;
        int num_indices;
        float* blit_buffer = cdata->blit_buffer;
        unsigned short* index_buffer = cdata->index_buffer;
        while(cdata->blit_buffer_num_vertices > 0)
        {
            num_vertices = MAX(cdata->blit_buffer_num_vertices, get_lowest_attribute_num_values(cdata, cdata->blit_buffer_num_vertices));
            num_indices = num_vertices * 3 / 2;  // 6 indices per sprite / 4 vertices per sprite = 3/2
            
            DoPartialFlush(cdata, num_vertices, blit_buffer, num_indices, index_buffer);
            
            cdata->blit_buffer_num_vertices -= num_vertices;
            // Move our pointers ahead
            blit_buffer += GPU_BLIT_BUFFER_FLOATS_PER_VERTEX*num_vertices;
            index_buffer += num_indices;
        }

        cdata->blit_buffer_num_vertices = 0;
        cdata->index_buffer_num_vertices = 0;

        unsetClipRect(renderer, dest);

        // restore matrices
        if(isRTT)
        {
            GPU_MatrixMode( GPU_PROJECTION );
            GPU_PopMatrix();
            GPU_MatrixMode( GPU_MODELVIEW );
        }

    }
}

static void Flip(GPU_Renderer* renderer, GPU_Target* target)
{
    renderer->FlushBlitBuffer(renderer);
    
    makeContextCurrent(renderer, target);

#ifdef SDL_GPU_USE_SDL2
    SDL_GL_SwapWindow(SDL_GetWindowFromID(renderer->current_context_target->context->windowID));
#else
    SDL_GL_SwapBuffers();
#endif

    #ifdef SDL_GPU_USE_OPENGL
    if(vendor_is_Intel)
        apply_Intel_attrib_workaround = 1;
    #endif
}




// Shader API


#include <string.h>

// On some platforms (e.g. Android), it might not be possible to just create a rwops and get the expected #included files.
// To do it, I might want to add an optional argument that specifies a base directory to prepend to #include file names.

static Uint32 GetShaderSourceSize(const char* filename);
static Uint32 GetShaderSource(const char* filename, char* result);

static void read_until_end_of_comment(SDL_RWops* rwops, char multiline)
{
    char buffer;
    while(SDL_RWread(rwops, &buffer, 1, 1) > 0)
    {
        if(!multiline)
        {
            if(buffer == '\n')
                break;
        }
        else
        {
            if(buffer == '*')
            {
                // If the stream ends at the next character or it is a '/', then we're done.
                if(SDL_RWread(rwops, &buffer, 1, 1) <= 0 || buffer == '/')
                    break;
            }
        }
    }
}

static Uint32 GetShaderSourceSize_RW(SDL_RWops* shader_source)
{
    Uint32 size = 0;
    
    // Read 1 byte at a time until we reach the end
    char last_char = ' ';
    char buffer[512];
    long len = 0;
    while((len = SDL_RWread(shader_source, &buffer, 1, 1)) > 0)
    {
        // Follow through an #include directive?
        if(buffer[0] == '#')
        {
            // Get the rest of the line
            int line_size = 1;
            int line_len;
            while((line_len = SDL_RWread(shader_source, buffer+line_size, 1, 1)) > 0)
            {
                line_size += line_len;
                if(buffer[line_size - line_len] == '\n')
                    break;
            }
            buffer[line_size] = '\0';
            
            // Is there "include" after '#'?
            char* token = strtok(buffer, "# \t");
            
            if(token != NULL && strcmp(token, "include") == 0)
            {
                // Get filename token
                token = strtok(NULL, "\"");  // Skip the empty token before the quote
                if(token != NULL)
                {
                    // Add the size of the included file and a newline character
                    size += GetShaderSourceSize(token) + 1;
                }
            }
            else
                size += line_size;
            last_char = ' ';
            continue;
        }
        
        size += len;
        
        if(last_char == '/')
        {
            if(buffer[0] == '/')
            {
                read_until_end_of_comment(shader_source, 0);
                size++;  // For the end of the comment
            }
            else if(buffer[0] == '*')
            {
                read_until_end_of_comment(shader_source, 1);
                size += 2;  // For the end of the comments
            }
            last_char = ' ';
        }
        else
            last_char = buffer[0];
    }
    
    // Go back to the beginning of the stream
    SDL_RWseek(shader_source, 0, SEEK_SET);
    return size;
}


static Uint32 GetShaderSource_RW(SDL_RWops* shader_source, char* result)
{
    Uint32 size = 0;
    
    // Read 1 byte at a time until we reach the end
    char last_char = ' ';
    char buffer[512];
    long len = 0;
    while((len = SDL_RWread(shader_source, &buffer, 1, 1)) > 0)
    {
        // Follow through an #include directive?
        if(buffer[0] == '#')
        {
            // Get the rest of the line
            int line_size = 1;
            int line_len;
            while((line_len = SDL_RWread(shader_source, buffer+line_size, 1, 1)) > 0)
            {
                line_size += line_len;
                if(buffer[line_size - line_len] == '\n')
                    break;
            }
            
            // Is there "include" after '#'?
            char token_buffer[512];  // strtok() is destructive
            memcpy(token_buffer, buffer, line_size+1);
            token_buffer[line_size] = '\0';
            char* token = strtok(token_buffer, "# \t");
            
            if(token != NULL && strcmp(token, "include") == 0)
            {
                // Get filename token
                token = strtok(NULL, "\"");  // Skip the empty token before the quote
                if(token != NULL)
                {
                    // Add the size of the included file and a newline character
                    size += GetShaderSource(token, result + size);
                    result[size] = '\n';
                    size++;
                }
            }
            else
            {
                memcpy(result + size, buffer, line_size);
                size += line_size;
            }
            last_char = ' ';
            continue;
        }
        
        memcpy(result + size, buffer, len);
        size += len;
        
        if(last_char == '/')
        {
            if(buffer[0] == '/')
            {
                read_until_end_of_comment(shader_source, 0);
                memcpy(result + size, "\n", 1);
                size++;
            }
            else if(buffer[0] == '*')
            {
                read_until_end_of_comment(shader_source, 1);
                memcpy(result + size, "*/", 2);
                size += 2;
            }
            last_char = ' ';
        }
        else
            last_char = buffer[0];
    }
    result[size] = '\0';
    
    // Go back to the beginning of the stream
    SDL_RWseek(shader_source, 0, SEEK_SET);
    return size;
}

static Uint32 GetShaderSource(const char* filename, char* result)
{
    if(filename == NULL)
        return 0;
    SDL_RWops* rwops = SDL_RWFromFile(filename, "r");
    
    Uint32 size = GetShaderSource_RW(rwops, result);
    
    SDL_RWclose(rwops);
    return size;
}

static Uint32 GetShaderSourceSize(const char* filename)
{
    if(filename == NULL)
        return 0;
    SDL_RWops* rwops = SDL_RWFromFile(filename, "r");
    
    Uint32 result = GetShaderSourceSize_RW(rwops);
    
    SDL_RWclose(rwops);
    return result;
}

static int get_rw_size(SDL_RWops* rwops)
{
    int size = 0;
    
    // Read 1 byte at a time until we reach the end
    char buffer;
    long len = 0;
    while((len = SDL_RWread(rwops, &buffer, 1, 1)) > 0)
    {
        size += len;
    }
    
    // Go back to the beginning of the stream
    SDL_RWseek(rwops, 0, SEEK_SET);
    return size;
}

static int read_string_rw(SDL_RWops* rwops, char* result)
{
   if(rwops == NULL)
        return 0;
    
    size_t size = 100;
    long total = 0;
    long len = 0;
    while((len = SDL_RWread(rwops, &result[total], 1, size)) > 0)
    {
        total += len;
    }
    
    result[total] = '\0';
    
    return total;
}

static char shader_message[256];


static Uint32 compile_shader_source(int shader_type, const char* shader_source)
{
    // Create the proper new shader object
    GLuint shader_object = 0;
    
    #ifndef SDL_GPU_DISABLE_SHADERS
    
    switch(shader_type)
    {
    case GPU_VERTEX_SHADER:
        shader_object = glCreateShader(GL_VERTEX_SHADER);
        break;
    case GPU_FRAGMENT_SHADER:
        shader_object = glCreateShader(GL_FRAGMENT_SHADER);
        break;
    #ifdef GL_GEOMETRY_SHADER
    case GPU_GEOMETRY_SHADER:
        shader_object = glCreateShader(GL_GEOMETRY_SHADER);
        break;
    #endif
    }
    
    if(shader_object == 0)
    {
        GPU_LogError("Failed to create new shader object.\n");
        snprintf(shader_message, 256, "Failed to create new shader object.\n");
        return 0;
    }
   
	glShaderSource(shader_object, 1, &shader_source, NULL);
    
    // Compile the shader source
    GLint compiled;
	
	glCompileShader(shader_object);
	
    glGetShaderiv(shader_object, GL_COMPILE_STATUS, &compiled);
    if(!compiled)
    {
        GPU_LogError("Failed to compile shader source.\n");
        glGetShaderInfoLog(shader_object, 256, NULL, shader_message);
        glDeleteShader(shader_object);
        return 0;
    }
    
    #endif
    
    return shader_object;
}


static Uint32 CompileShader_RW(GPU_Renderer* renderer, int shader_type, SDL_RWops* shader_source)
{
    // Read in the shader source code
    Uint32 size = GetShaderSourceSize_RW(shader_source);
    char* source_string = (char*)malloc(size+1);
    int result = GetShaderSource_RW(shader_source, source_string);
    if(!result)
    {
        GPU_LogError("Failed to read shader source.\n");
        snprintf(shader_message, 256, "Failed to read shader source.\n");
        free(source_string);
        return 0;
    }
    
    Uint32 result2 = compile_shader_source(shader_type, source_string);
    free(source_string);
    
    return result2;
}

static Uint32 CompileShader(GPU_Renderer* renderer, int shader_type, const char* shader_source)
{
    Uint32 size = strlen(shader_source);
    if(size == 0)
        return 0;
    SDL_RWops* rwops = SDL_RWFromConstMem(shader_source, size);
    size = renderer->CompileShader_RW(renderer, shader_type, rwops);
    SDL_RWclose(rwops);
    return size;
}

static Uint32 LinkShaderProgram(GPU_Renderer* renderer, Uint32 program_object)
{
    #ifndef SDL_GPU_DISABLE_SHADERS
	glLinkProgram(program_object);
	
	int linked;
	glGetProgramiv(program_object, GL_LINK_STATUS, &linked);
	
	if(!linked)
    {
        GPU_LogError("Failed to link shader program.\n");
        glGetProgramInfoLog(program_object, 256, NULL, shader_message);
        glDeleteProgram(program_object);
        return 0;
    }
	#endif
    
	return program_object;
}

static Uint32 LinkShaders(GPU_Renderer* renderer, Uint32 shader_object1, Uint32 shader_object2)
{
    #ifndef SDL_GPU_DISABLE_SHADERS
    GLuint p = glCreateProgram();

	glAttachShader(p, shader_object1);
	glAttachShader(p, shader_object2);
	
	return renderer->LinkShaderProgram(renderer, p);
	#else
	return 0;
	#endif
}

static void FreeShader(GPU_Renderer* renderer, Uint32 shader_object)
{
    #ifndef SDL_GPU_DISABLE_SHADERS
    glDeleteShader(shader_object);
    #endif
}

static void FreeShaderProgram(GPU_Renderer* renderer, Uint32 program_object)
{
    #ifndef SDL_GPU_DISABLE_SHADERS
    glDeleteProgram(program_object);
    #endif
}

static void AttachShader(GPU_Renderer* renderer, Uint32 program_object, Uint32 shader_object)
{
    #ifndef SDL_GPU_DISABLE_SHADERS
    glAttachShader(program_object, shader_object);
    #endif
}

static void DetachShader(GPU_Renderer* renderer, Uint32 program_object, Uint32 shader_object)
{
    #ifndef SDL_GPU_DISABLE_SHADERS
    glDetachShader(program_object, shader_object);
    #endif
}

static Uint8 IsDefaultShaderProgram(GPU_Renderer* renderer, Uint32 program_object)
{
    GPU_Context* context = renderer->current_context_target->context;
    return (program_object == context->default_textured_shader_program || program_object == context->default_untextured_shader_program);
}

static void ActivateShaderProgram(GPU_Renderer* renderer, Uint32 program_object, GPU_ShaderBlock* block)
{
    GPU_Target* target = renderer->current_context_target;
    #ifndef SDL_GPU_DISABLE_SHADERS
    
    if(program_object == 0) // Implies default shader
    {
        // Already using a default shader?
        if(target->context->current_shader_program == target->context->default_textured_shader_program
            || target->context->current_shader_program == target->context->default_untextured_shader_program)
            return;
        
        program_object = target->context->default_untextured_shader_program;
    }
    
    renderer->FlushBlitBuffer(renderer);
    glUseProgram(program_object);
    
        #ifdef SDL_GPU_USE_GL_TIER3
        // Set up our shader attribute and uniform locations
        GPU_CONTEXT_DATA* cdata = ((GPU_CONTEXT_DATA*)target->context->data);
        if(block == NULL)
        {
            if(program_object == target->context->default_textured_shader_program)
                cdata->current_shader_block = cdata->shader_block[0];
            else if(program_object == target->context->default_untextured_shader_program)
                cdata->current_shader_block = cdata->shader_block[1];
            else
            {
                    GPU_ShaderBlock b;
                    b.position_loc = -1;
                    b.texcoord_loc = -1;
                    b.color_loc = -1;
                    b.modelViewProjection_loc = -1;
                    cdata->current_shader_block = b;
            }
        }
        else
            cdata->current_shader_block = *block;
        #endif
    #endif
    
    target->context->current_shader_program = program_object;
}

static void DeactivateShaderProgram(GPU_Renderer* renderer)
{
    renderer->ActivateShaderProgram(renderer, 0, NULL);
}

static const char* GetShaderMessage(GPU_Renderer* renderer)
{
    return shader_message;
}

static int GetAttributeLocation(GPU_Renderer* renderer, Uint32 program_object, const char* attrib_name)
{
    #ifndef SDL_GPU_DISABLE_SHADERS
    program_object = get_proper_program_id(renderer, program_object);
    if(program_object == 0)
        return -1;
    return glGetAttribLocation(program_object, attrib_name);
    #else
    return -1;
    #endif
}

static int GetUniformLocation(GPU_Renderer* renderer, Uint32 program_object, const char* uniform_name)
{
    #ifndef SDL_GPU_DISABLE_SHADERS
    program_object = get_proper_program_id(renderer, program_object);
    if(program_object == 0)
        return -1;
    return glGetUniformLocation(program_object, uniform_name);
    #else
    return -1;
    #endif
}

static GPU_ShaderBlock LoadShaderBlock(GPU_Renderer* renderer, Uint32 program_object, const char* position_name, const char* texcoord_name, const char* color_name, const char* modelViewMatrix_name)
{
    GPU_ShaderBlock b;
    program_object = get_proper_program_id(renderer, program_object);
    if(program_object == 0)
    {
        b.position_loc = -1;
        b.texcoord_loc = -1;
        b.color_loc = -1;
        b.modelViewProjection_loc = -1;
        return b;
    }
    
    if(position_name == NULL)
        b.position_loc = -1;
    else
        b.position_loc = renderer->GetAttributeLocation(renderer, program_object, position_name);
        
    if(texcoord_name == NULL)
        b.texcoord_loc = -1;
    else
        b.texcoord_loc = renderer->GetAttributeLocation(renderer, program_object, texcoord_name);
        
    if(color_name == NULL)
        b.color_loc = -1;
    else
        b.color_loc = renderer->GetAttributeLocation(renderer, program_object, color_name);
        
    if(modelViewMatrix_name == NULL)
        b.modelViewProjection_loc = -1;
    else
        b.modelViewProjection_loc = renderer->GetUniformLocation(renderer, program_object, modelViewMatrix_name);
    
    return b;
}

static void SetShaderBlock(GPU_Renderer* renderer, GPU_ShaderBlock block)
{
    #ifdef SDL_GPU_USE_GL_TIER3
    ((GPU_CONTEXT_DATA*)renderer->current_context_target->context->data)->current_shader_block = block;
    #endif
}

static void SetShaderImage(GPU_Renderer* renderer, GPU_Image* image, int location, int image_unit)
{
    // TODO: OpenGL 1 needs to check for ARB_multitexture to use glActiveTexture().
    #ifndef SDL_GPU_DISABLE_SHADERS
    renderer->FlushBlitBuffer(renderer);
    if(renderer->current_context_target->context->current_shader_program == 0 || image_unit < 0)
        return;
    
    Uint32 new_texture = 0;
    if(image != NULL)
        new_texture = ((GPU_IMAGE_DATA*)image->data)->handle;
    
    // Set the new image unit
    glUniform1i(location, image_unit);
    glActiveTexture(GL_TEXTURE0 + image_unit);
    glBindTexture(GL_TEXTURE_2D, new_texture);
    
    if(image_unit != 0)
        glActiveTexture(GL_TEXTURE0);
    
    #endif
}


static void GetUniformiv(GPU_Renderer* renderer, Uint32 program_object, int location, int* values)
{
    #ifndef SDL_GPU_DISABLE_SHADERS
    program_object = get_proper_program_id(renderer, program_object);
    if(program_object != 0)
        glGetUniformiv(program_object, location, values);
    #endif
}

static void SetUniformi(GPU_Renderer* renderer, int location, int value)
{
    #ifndef SDL_GPU_DISABLE_SHADERS
    renderer->FlushBlitBuffer(renderer);
    if(renderer->current_context_target->context->current_shader_program == 0)
        return;
    glUniform1i(location, value);
    #endif
}

static void SetUniformiv(GPU_Renderer* renderer, int location, int num_elements_per_value, int num_values, int* values)
{
    #ifndef SDL_GPU_DISABLE_SHADERS
    renderer->FlushBlitBuffer(renderer);
    if(renderer->current_context_target->context->current_shader_program == 0)
        return;
    switch(num_elements_per_value)
    {
        case 1:
        glUniform1iv(location, num_values, values);
        break;
        case 2:
        glUniform2iv(location, num_values, values);
        break;
        case 3:
        glUniform3iv(location, num_values, values);
        break;
        case 4:
        glUniform4iv(location, num_values, values);
        break;
    }
    #endif
}


static void GetUniformuiv(GPU_Renderer* renderer, Uint32 program_object, int location, unsigned int* values)
{
    #ifndef SDL_GPU_DISABLE_SHADERS
    program_object = get_proper_program_id(renderer, program_object);
    if(program_object != 0)
        #if defined(SDL_GPU_USE_GLES) && SDL_GPU_GLES_MAJOR_VERSION < 3
        glGetUniformiv(program_object, location, (int*)values);
        #else
        glGetUniformuiv(program_object, location, values);
        #endif
    #endif
}

static void SetUniformui(GPU_Renderer* renderer, int location, unsigned int value)
{
    #ifndef SDL_GPU_DISABLE_SHADERS
    renderer->FlushBlitBuffer(renderer);
    if(renderer->current_context_target->context->current_shader_program == 0)
        return;
    #if defined(SDL_GPU_USE_GLES) && SDL_GPU_GLES_MAJOR_VERSION < 3
    glUniform1i(location, (int)value);
    #else
    glUniform1ui(location, value);
    #endif
    #endif
}

static void SetUniformuiv(GPU_Renderer* renderer, int location, int num_elements_per_value, int num_values, unsigned int* values)
{
    #ifndef SDL_GPU_DISABLE_SHADERS
    renderer->FlushBlitBuffer(renderer);
    if(renderer->current_context_target->context->current_shader_program == 0)
        return;
    #if defined(SDL_GPU_USE_GLES) && SDL_GPU_GLES_MAJOR_VERSION < 3
    switch(num_elements_per_value)
    {
        case 1:
        glUniform1iv(location, num_values, (int*)values);
        break;
        case 2:
        glUniform2iv(location, num_values, (int*)values);
        break;
        case 3:
        glUniform3iv(location, num_values, (int*)values);
        break;
        case 4:
        glUniform4iv(location, num_values, (int*)values);
        break;
    }
    #else
    switch(num_elements_per_value)
    {
        case 1:
        glUniform1uiv(location, num_values, values);
        break;
        case 2:
        glUniform2uiv(location, num_values, values);
        break;
        case 3:
        glUniform3uiv(location, num_values, values);
        break;
        case 4:
        glUniform4uiv(location, num_values, values);
        break;
    }
    #endif
    #endif
}


static void GetUniformfv(GPU_Renderer* renderer, Uint32 program_object, int location, float* values)
{
    #ifndef SDL_GPU_DISABLE_SHADERS
    program_object = get_proper_program_id(renderer, program_object);
    if(program_object != 0)
        glGetUniformfv(program_object, location, values);
    #endif
}

static void SetUniformf(GPU_Renderer* renderer, int location, float value)
{
    #ifndef SDL_GPU_DISABLE_SHADERS
    renderer->FlushBlitBuffer(renderer);
    if(renderer->current_context_target->context->current_shader_program == 0)
        return;
    glUniform1f(location, value);
    #endif
}

static void SetUniformfv(GPU_Renderer* renderer, int location, int num_elements_per_value, int num_values, float* values)
{
    #ifndef SDL_GPU_DISABLE_SHADERS
    renderer->FlushBlitBuffer(renderer);
    if(renderer->current_context_target->context->current_shader_program == 0)
        return;
    switch(num_elements_per_value)
    {
        case 1:
        glUniform1fv(location, num_values, values);
        break;
        case 2:
        glUniform2fv(location, num_values, values);
        break;
        case 3:
        glUniform3fv(location, num_values, values);
        break;
        case 4:
        glUniform4fv(location, num_values, values);
        break;
    }
    #endif
}

static void SetUniformMatrixfv(GPU_Renderer* renderer, int location, int num_matrices, int num_rows, int num_columns, Uint8 transpose, float* values)
{
    #ifndef SDL_GPU_DISABLE_SHADERS
    renderer->FlushBlitBuffer(renderer);
    if(renderer->current_context_target->context->current_shader_program == 0)
        return;
    if(num_rows < 2 || num_rows > 4 || num_columns < 2 || num_columns > 4)
    {
        GPU_LogError("GPU_SetUniformMatrixfv(): Given invalid dimensions (%dx%d).\n", num_rows, num_columns);
        return;
    }
    #if defined(SDL_GPU_USE_GLES)
    // Hide these symbols so it compiles, but make sure they never get called because GLES only supports square matrices.
    #define glUniformMatrix2x3fv glUniformMatrix2fv
    #define glUniformMatrix2x4fv glUniformMatrix2fv
    #define glUniformMatrix3x2fv glUniformMatrix2fv
    #define glUniformMatrix3x4fv glUniformMatrix2fv
    #define glUniformMatrix4x2fv glUniformMatrix2fv
    #define glUniformMatrix4x3fv glUniformMatrix2fv
    if(num_rows != num_columns)
    {
        GPU_LogError("GPU_SetUniformMatrixfv(): GLES renderers do not accept non-square matrices (%dx%d).\n", num_rows, num_columns);
        return;
    }
    #endif
    
    switch(num_rows)
    {
    case 2:
        if(num_columns == 2)
            glUniformMatrix2fv(location, num_matrices, transpose, values);
        else if(num_columns == 3)
            glUniformMatrix2x3fv(location, num_matrices, transpose, values);
        else if(num_columns == 4)
            glUniformMatrix2x4fv(location, num_matrices, transpose, values);
        break;
    case 3:
        if(num_columns == 2)
            glUniformMatrix3x2fv(location, num_matrices, transpose, values);
        else if(num_columns == 3)
            glUniformMatrix3fv(location, num_matrices, transpose, values);
        else if(num_columns == 4)
            glUniformMatrix3x4fv(location, num_matrices, transpose, values);
        break;
    case 4:
        if(num_columns == 2)
            glUniformMatrix4x2fv(location, num_matrices, transpose, values);
        else if(num_columns == 3)
            glUniformMatrix4x3fv(location, num_matrices, transpose, values);
        else if(num_columns == 4)
            glUniformMatrix4fv(location, num_matrices, transpose, values);
        break;
    }
    #endif
}


static void SetAttributef(GPU_Renderer* renderer, int location, float value)
{
    #ifndef SDL_GPU_DISABLE_SHADERS
    renderer->FlushBlitBuffer(renderer);
    if(renderer->current_context_target->context->current_shader_program == 0)
        return;
    
    #ifdef SDL_GPU_USE_OPENGL
    if(apply_Intel_attrib_workaround && location == 0)
    {
        apply_Intel_attrib_workaround = 0;
        glBegin(GL_TRIANGLES);
        glEnd();
    }
    #endif
    
    glVertexAttrib1f(location, value);
    
    #endif
}

static void SetAttributei(GPU_Renderer* renderer, int location, int value)
{
    #ifndef SDL_GPU_DISABLE_SHADERS
    renderer->FlushBlitBuffer(renderer);
    if(renderer->current_context_target->context->current_shader_program == 0)
        return;
    
    #ifdef SDL_GPU_USE_OPENGL
    if(apply_Intel_attrib_workaround && location == 0)
    {
        apply_Intel_attrib_workaround = 0;
        glBegin(GL_TRIANGLES);
        glEnd();
    }
    #endif
    
    glVertexAttribI1i(location, value);
    
    #endif
}

static void SetAttributeui(GPU_Renderer* renderer, int location, unsigned int value)
{
    #ifndef SDL_GPU_DISABLE_SHADERS
    renderer->FlushBlitBuffer(renderer);
    if(renderer->current_context_target->context->current_shader_program == 0)
        return;
    
    #ifdef SDL_GPU_USE_OPENGL
    if(apply_Intel_attrib_workaround && location == 0)
    {
        apply_Intel_attrib_workaround = 0;
        glBegin(GL_TRIANGLES);
        glEnd();
    }
    #endif
    
    glVertexAttribI1ui(location, value);
    
    #endif
}


static void SetAttributefv(GPU_Renderer* renderer, int location, int num_elements, float* value)
{
    #ifndef SDL_GPU_DISABLE_SHADERS
    renderer->FlushBlitBuffer(renderer);
    if(renderer->current_context_target->context->current_shader_program == 0)
        return;
    
    #ifdef SDL_GPU_USE_OPENGL
    if(apply_Intel_attrib_workaround && location == 0)
    {
        apply_Intel_attrib_workaround = 0;
        glBegin(GL_TRIANGLES);
        glEnd();
    }
    #endif
    
    switch(num_elements)
    {
        case 1:
            glVertexAttrib1f(location, value[0]);
            break;
        case 2:
            glVertexAttrib2f(location, value[0], value[1]);
            break;
        case 3:
            glVertexAttrib3f(location, value[0], value[1], value[2]);
            break;
        case 4:
            glVertexAttrib4f(location, value[0], value[1], value[2], value[3]);
            break;
    }
    
    #endif
}

static void SetAttributeiv(GPU_Renderer* renderer, int location, int num_elements, int* value)
{
    #ifndef SDL_GPU_DISABLE_SHADERS
    renderer->FlushBlitBuffer(renderer);
    if(renderer->current_context_target->context->current_shader_program == 0)
        return;
    
    #ifdef SDL_GPU_USE_OPENGL
    if(apply_Intel_attrib_workaround && location == 0)
    {
        apply_Intel_attrib_workaround = 0;
        glBegin(GL_TRIANGLES);
        glEnd();
    }
    #endif
    
    switch(num_elements)
    {
        case 1:
            glVertexAttribI1i(location, value[0]);
            break;
        case 2:
            glVertexAttribI2i(location, value[0], value[1]);
            break;
        case 3:
            glVertexAttribI3i(location, value[0], value[1], value[2]);
            break;
        case 4:
            glVertexAttribI4i(location, value[0], value[1], value[2], value[3]);
            break;
    }
    
    #endif
}

static void SetAttributeuiv(GPU_Renderer* renderer, int location, int num_elements, unsigned int* value)
{
    #ifndef SDL_GPU_DISABLE_SHADERS
    renderer->FlushBlitBuffer(renderer);
    if(renderer->current_context_target->context->current_shader_program == 0)
        return;
    
    #ifdef SDL_GPU_USE_OPENGL
    if(apply_Intel_attrib_workaround && location == 0)
    {
        apply_Intel_attrib_workaround = 0;
        glBegin(GL_TRIANGLES);
        glEnd();
    }
    #endif
    
    switch(num_elements)
    {
        case 1:
            glVertexAttribI1ui(location, value[0]);
            break;
        case 2:
            glVertexAttribI2ui(location, value[0], value[1]);
            break;
        case 3:
            glVertexAttribI3ui(location, value[0], value[1], value[2]);
            break;
        case 4:
            glVertexAttribI4ui(location, value[0], value[1], value[2], value[3]);
            break;
    }
    
    #endif
}

static void SetAttributeSource(GPU_Renderer* renderer, int num_values, GPU_Attribute source)
{
    #ifdef SDL_GPU_USE_GL_TIER3
    if(source.location < 0 || source.location >= 16)
        return;
    GPU_CONTEXT_DATA* cdata = (GPU_CONTEXT_DATA*)renderer->current_context_target->context->data;
    GPU_AttributeSource* a = &cdata->shader_attributes[source.location];
    if(source.format.is_per_sprite)
    {
        a->per_vertex_storage_offset_bytes = 0;
        a->per_vertex_storage_stride_bytes = source.format.num_elems_per_value * sizeof_GPU_type(source.format.type);
        a->num_values = 4 * num_values;  // 4 vertices now
        int needed_size = a->num_values * a->per_vertex_storage_stride_bytes;
        
        // Make sure we have enough room for converted per-vertex data
        if(a->per_vertex_storage_size < needed_size)
        {
            free(a->per_vertex_storage);
            a->per_vertex_storage = malloc(needed_size);
            a->per_vertex_storage_size = needed_size;
        }
    }
    else if(a->per_vertex_storage_size > 0)
    {
        free(a->per_vertex_storage);
        a->per_vertex_storage = NULL;
        a->per_vertex_storage_size = 0;
    }
    
    a->enabled = 0;
    a->attribute = source;
    
    if(!source.format.is_per_sprite)
    {
        a->per_vertex_storage = source.values;
        a->num_values = num_values;
        a->per_vertex_storage_stride_bytes = source.format.stride_bytes;
        a->per_vertex_storage_offset_bytes = source.format.offset_bytes;
    }
    
    a->next_value = a->per_vertex_storage;
    
    #endif
}



#define SET_COMMON_FUNCTIONS(renderer) \
    renderer->Init = &Init; \
    renderer->IsFeatureEnabled = &IsFeatureEnabled; \
    renderer->CreateTargetFromWindow = &CreateTargetFromWindow; \
    renderer->MakeCurrent = &MakeCurrent; \
    renderer->SetAsCurrent = &SetAsCurrent; \
    renderer->SetWindowResolution = &SetWindowResolution; \
    renderer->SetVirtualResolution = &SetVirtualResolution; \
    renderer->Quit = &Quit; \
 \
    renderer->ToggleFullscreen = &ToggleFullscreen; \
    renderer->SetCamera = &SetCamera; \
 \
    renderer->CreateImage = &CreateImage; \
    renderer->LoadImage = &LoadImage; \
    renderer->SaveImage = &SaveImage; \
    renderer->CopyImage = &CopyImage; \
    renderer->UpdateImage = &UpdateImage; \
    renderer->CopyImageFromSurface = &CopyImageFromSurface; \
    renderer->CopyImageFromTarget = &CopyImageFromTarget; \
    renderer->CopySurfaceFromTarget = &CopySurfaceFromTarget; \
    renderer->CopySurfaceFromImage = &CopySurfaceFromImage; \
    renderer->SubSurfaceCopy = &SubSurfaceCopy; \
    renderer->FreeImage = &FreeImage; \
 \
    renderer->LoadTarget = &LoadTarget; \
    renderer->FreeTarget = &FreeTarget; \
 \
    renderer->Blit = &Blit; \
    renderer->BlitRotate = &BlitRotate; \
    renderer->BlitScale = &BlitScale; \
    renderer->BlitTransform = &BlitTransform; \
    renderer->BlitTransformX = &BlitTransformX; \
    renderer->BlitTransformMatrix = &BlitTransformMatrix; \
    renderer->BlitBatch = &BlitBatch; \
 \
    renderer->GenerateMipmaps = &GenerateMipmaps; \
 \
    renderer->SetClip = &SetClip; \
    renderer->ClearClip = &ClearClip; \
     \
    renderer->GetPixel = &GetPixel; \
    renderer->SetImageFilter = &SetImageFilter; \
 \
    renderer->Clear = &Clear; \
    renderer->ClearRGBA = &ClearRGBA; \
    renderer->FlushBlitBuffer = &FlushBlitBuffer; \
    renderer->Flip = &Flip; \
     \
    renderer->CompileShader_RW = &CompileShader_RW; \
    renderer->CompileShader = &CompileShader; \
    renderer->LinkShaderProgram = &LinkShaderProgram; \
    renderer->LinkShaders = &LinkShaders; \
    renderer->FreeShader = &FreeShader; \
    renderer->FreeShaderProgram = &FreeShaderProgram; \
    renderer->AttachShader = &AttachShader; \
    renderer->DetachShader = &DetachShader; \
    renderer->IsDefaultShaderProgram = &IsDefaultShaderProgram; \
    renderer->ActivateShaderProgram = &ActivateShaderProgram; \
    renderer->DeactivateShaderProgram = &DeactivateShaderProgram; \
    renderer->GetShaderMessage = &GetShaderMessage; \
    renderer->GetAttributeLocation = &GetAttributeLocation; \
    renderer->GetUniformLocation = &GetUniformLocation; \
    renderer->LoadShaderBlock = &LoadShaderBlock; \
    renderer->SetShaderBlock = &SetShaderBlock; \
    renderer->SetShaderImage = &SetShaderImage; \
    renderer->GetUniformiv = &GetUniformiv; \
    renderer->SetUniformi = &SetUniformi; \
    renderer->SetUniformiv = &SetUniformiv; \
    renderer->GetUniformuiv = &GetUniformuiv; \
    renderer->SetUniformui = &SetUniformui; \
    renderer->SetUniformuiv = &SetUniformuiv; \
    renderer->GetUniformfv = &GetUniformfv; \
    renderer->SetUniformf = &SetUniformf; \
    renderer->SetUniformfv = &SetUniformfv; \
    renderer->SetUniformMatrixfv = &SetUniformMatrixfv; \
    renderer->SetAttributef = &SetAttributef; \
    renderer->SetAttributei = &SetAttributei; \
    renderer->SetAttributeui = &SetAttributeui; \
    renderer->SetAttributefv = &SetAttributefv; \
    renderer->SetAttributeiv = &SetAttributeiv; \
    renderer->SetAttributeuiv = &SetAttributeuiv; \
    renderer->SetAttributeSource = &SetAttributeSource; \
	 \
	/* Shape rendering */ \
	 \
    renderer->SetLineThickness = &SetLineThickness; \
    renderer->SetLineThickness(renderer, 1.0f); \
    renderer->GetLineThickness = &GetLineThickness; \
    renderer->Pixel = &Pixel; \
    renderer->Line = &Line; \
    renderer->Arc = &Arc; \
    renderer->ArcFilled = &ArcFilled; \
    renderer->Circle = &Circle; \
    renderer->CircleFilled = &CircleFilled; \
    renderer->Tri = &Tri; \
    renderer->TriFilled = &TriFilled; \
    renderer->Rectangle = &Rectangle; \
    renderer->RectangleFilled = &RectangleFilled; \
    renderer->RectangleRound = &RectangleRound; \
    renderer->RectangleRoundFilled = &RectangleRoundFilled; \
    renderer->Polygon = &Polygon; \
    renderer->PolygonFilled = &PolygonFilled; \
    renderer->PolygonBlit = &PolygonBlit;

