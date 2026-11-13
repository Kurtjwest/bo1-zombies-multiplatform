// WebGL 2 implementation of the rendering interface of the Direct3D 9 layer (../d3d9mtl/mtl_rhi.h), for the browser
// build. It follows the OpenGL renderer (../opengl/gl_rhi.cpp), with what WebGL 2 (OpenGL ES 3.0) lacks done another
// way:
//  - one context, on the render thread: WebGL contexts are not shared between Web Workers, so what other threads ask
//    for (the database thread's textures, the shader compiler threads' pipelines) is queued and done by the render
//    thread at its next call (textures, samplers and GL buffers are made when first used; shaders compile and link at
//    the first draw that needs them);
//  - clip control: EXT_clip_control where the browser has it, otherwise every vertex shader ends by turning clip space
//    the way glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE) does (y negated, depth from [0, 1] to [-1, 1]): row 0 of a
//    render target stays the top of the picture, so viewports, scissor rectangles and gl_FragCoord are unchanged, and
//    the self-test at Init measures the front-face winding that results;
//  - no texture views and no texture swizzle: a Direct3D format's channel rules (X8R8G8B8 reads alpha 1, L8 replicates,
//    A8 is (0, 0, 0, a), ...) are applied where the shaders sample, from a per-draw table of each unit's swizzle
//    (bo1_misc_block); the BGRA formats are swapped to RGBA as they are uploaded and read back;
//  - GLSL ES 3.00: the BO1 GLSL profile's output (mojoshader_profile_glsl.c) without its layout bindings, which are
//    set after linking by name; D3DCOLOR vertex inputs (BGRA bytes) are swapped in the vertex shader;
//  - no base vertex: the vertex streams are re-pointed by the base vertex instead; buffers are uploaded with
//    bufferSubData (no persistent mapping), and a buffer used both for vertices and indices has a GL buffer for each
//    (WebGL never lets one buffer be both);
//  - no multisampling, polygon mode, border colors or mirror-clamp addressing; occlusion queries count as visible (a
//    WebGL query's result only arrives once the thread returns to its event loop, which the render thread never does);
//  - frames go to the page as ImageBitmaps (bo1_web_lib.js), paced by the page's display of them;
//  - S3TC / RGTC textures are decoded on the CPU where the browser has no such extension (iPhones, some Android).
//
// Threading: every call is made with the Direct3D device lock held (as with Metal and OpenGL).

#include "../d3d9mtl/mtl_rhi.h"
#include "web_shared.h"
#include "web_surface.h"

#include <GLES3/gl3.h>
#include <emscripten.h>
#include <emscripten/threading.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// extension enumerations
#define BO1_GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define BO1_GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define BO1_GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#define BO1_GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT 0x8C4D
#define BO1_GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT 0x8C4E
#define BO1_GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT 0x8C4F
#define BO1_GL_COMPRESSED_RED_RGTC1_EXT 0x8DBB
#define BO1_GL_COMPRESSED_RED_GREEN_RGTC2_EXT 0x8DBD
#define BO1_GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#define BO1_GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#define BO1_GL_R16_EXT 0x822A
#define BO1_GL_RG16_EXT 0x822C
#define BO1_GL_RGBA16_EXT 0x805B
#define BO1_GL_R16_SNORM_EXT 0x8F98
#define BO1_GL_RG16_SNORM_EXT 0x8F99
#define BO1_GL_RGBA16_SNORM_EXT 0x8F9B

// bo1_web_lib.js
extern "C" {
int bo1_web_gl_create(void);
int bo1_web_gl_has(const char *name);
void bo1_web_gl_blend_indexed(int buf, int enable, int srcRGB, int dstRGB, int srcA, int dstA, int opRGB, int opA,
                              int mask);
int bo1_web_gl_clip_control(void);
// (Emscripten's WebGL 2 library has it; GLES 3's headers don't declare it)
void glGetBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, void *data);
}

namespace rhi
{
namespace
{
constexpr uint32_t PF_DEPTH24_STENCIL8 = 255; // MTLPixelFormatDepth24Unorm_Stencil8
constexpr uint32_t PF_X32_STENCIL8 = 261;     // MTLPixelFormatX32_Stencil8

// ---- formats ---------------------------------------------------------------------------------------------------
// What a texel of the format is in memory when the Direct3D layer uploads it, and how WebGL holds it.
enum Conv : uint8_t
{
    CV_NONE,
    CV_BGRA8,      // B, G, R, A bytes -> R, G, B, A
    CV_BGR10A2,    // the 10-bit red and blue fields swapped
    CV_U16_HALF,   // unsigned 16-bit normalized -> half float (no EXT_texture_norm16)
    CV_S16_HALF,   // signed 16-bit normalized -> half float
    CV_BC1,        // S3TC / RGTC blocks decoded (no compression extension): BC1-3 to RGBA8, BC4 to R8, BC5 to RG8
    CV_BC2,
    CV_BC3,
    CV_BC4,
    CV_BC5,
};

struct GlFormat
{
    uint32_t pf;
    GLenum internal, format, type;
    uint8_t blockBytes; // bytes per pixel, or per 4x4 block (of the Direct3D layer's memory)
    bool compressed, depth, stencil;
    Conv conv;
    // how the texels are read back (ReadTexture): the components and type WebGL reads this format's render targets as
    uint8_t readKind; // 0 none, 1 RGBA8 bytes, 2 RGBA float, 3 RGB10A2 words
};

std::vector<GlFormat> g_formats; // (the table for this browser's extensions: BuildFormats)

void BuildFormats(bool s3tc, bool s3tcSrgb, bool rgtc, bool norm16)
{
    g_formats = {
        {PF_A8, GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1, false, false, false, CV_NONE, 1},
        {PF_R8, GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1, false, false, false, CV_NONE, 1},
        {PF_R8_SNORM, GL_R8_SNORM, GL_RED, GL_BYTE, 1, false, false, false, CV_NONE, 0},
        {PF_R16F, GL_R16F, GL_RED, GL_HALF_FLOAT, 2, false, false, false, CV_NONE, 2},
        {PF_RG8, GL_RG8, GL_RG, GL_UNSIGNED_BYTE, 2, false, false, false, CV_NONE, 1},
        {PF_RG8_SNORM, GL_RG8_SNORM, GL_RG, GL_BYTE, 2, false, false, false, CV_NONE, 0},
        {PF_B5G6R5, GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 2, false, false, false, CV_NONE, 1},
        {PF_R32F, GL_R32F, GL_RED, GL_FLOAT, 4, false, false, false, CV_NONE, 2},
        {PF_RG16F, GL_RG16F, GL_RG, GL_HALF_FLOAT, 4, false, false, false, CV_NONE, 2},
        {PF_RGBA8, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, false, false, false, CV_NONE, 1},
        {PF_RGBA8_SRGB, GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, false, false, false, CV_NONE, 1},
        {PF_RGBA8_SNORM, GL_RGBA8_SNORM, GL_RGBA, GL_BYTE, 4, false, false, false, CV_NONE, 0},
        {PF_BGRA8, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, false, false, false, CV_BGRA8, 1},
        {PF_BGRA8_SRGB, GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, false, false, false, CV_BGRA8, 1},
        {PF_RGB10A2, GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, 4, false, false, false, CV_NONE, 3},
        {PF_BGR10A2, GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, 4, false, false, false, CV_BGR10A2, 3},
        {PF_RG32F, GL_RG32F, GL_RG, GL_FLOAT, 8, false, false, false, CV_NONE, 2},
        {PF_RGBA16F, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 8, false, false, false, CV_NONE, 2},
        {PF_RGBA32F, GL_RGBA32F, GL_RGBA, GL_FLOAT, 16, false, false, false, CV_NONE, 2},
        {PF_D16, GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, 2, false, true, false, CV_NONE, 0},
        {PF_D32F, GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, 4, false, true, false, CV_NONE, 0},
        {PF_D32F_S8, GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, 8, false, true, true,
         CV_NONE, 0},
        {PF_DEPTH24_STENCIL8, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 4, false, true, true, CV_NONE,
         0},
    };
    if (norm16)
    {
        g_formats.push_back({PF_R16, BO1_GL_R16_EXT, GL_RED, GL_UNSIGNED_SHORT, 2, false, false, false, CV_NONE, 2});
        g_formats.push_back({PF_RG16, BO1_GL_RG16_EXT, GL_RG, GL_UNSIGNED_SHORT, 4, false, false, false, CV_NONE, 2});
        g_formats.push_back({PF_RGBA16, BO1_GL_RGBA16_EXT, GL_RGBA, GL_UNSIGNED_SHORT, 8, false, false, false, CV_NONE, 2});
        g_formats.push_back({PF_RG16_SNORM, BO1_GL_RG16_SNORM_EXT, GL_RG, GL_SHORT, 4, false, false, false, CV_NONE, 0});
        g_formats.push_back({PF_RGBA16_SNORM, BO1_GL_RGBA16_SNORM_EXT, GL_RGBA, GL_SHORT, 8, false, false, false, CV_NONE, 0});
    }
    else
    {
        g_formats.push_back({PF_R16, GL_R16F, GL_RED, GL_HALF_FLOAT, 2, false, false, false, CV_U16_HALF, 2});
        g_formats.push_back({PF_RG16, GL_RG16F, GL_RG, GL_HALF_FLOAT, 4, false, false, false, CV_U16_HALF, 2});
        g_formats.push_back({PF_RGBA16, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 8, false, false, false, CV_U16_HALF, 2});
        g_formats.push_back({PF_RG16_SNORM, GL_RG16F, GL_RG, GL_HALF_FLOAT, 4, false, false, false, CV_S16_HALF, 2});
        g_formats.push_back({PF_RGBA16_SNORM, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 8, false, false, false, CV_S16_HALF, 2});
    }
    if (s3tc)
    {
        g_formats.push_back({PF_BC1, BO1_GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 0, 0, 8, true, false, false, CV_NONE, 0});
        g_formats.push_back({PF_BC2, BO1_GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, 0, 0, 16, true, false, false, CV_NONE, 0});
        g_formats.push_back({PF_BC3, BO1_GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 0, 0, 16, true, false, false, CV_NONE, 0});
    }
    else
    {
        g_formats.push_back({PF_BC1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 8, true, false, false, CV_BC1, 0});
        g_formats.push_back({PF_BC2, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 16, true, false, false, CV_BC2, 0});
        g_formats.push_back({PF_BC3, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 16, true, false, false, CV_BC3, 0});
    }
    if (s3tc && s3tcSrgb)
    {
        g_formats.push_back({PF_BC1_SRGB, BO1_GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT, 0, 0, 8, true, false, false, CV_NONE, 0});
        g_formats.push_back({PF_BC2_SRGB, BO1_GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT, 0, 0, 16, true, false, false, CV_NONE, 0});
        g_formats.push_back({PF_BC3_SRGB, BO1_GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT, 0, 0, 16, true, false, false, CV_NONE, 0});
    }
    else
    {
        g_formats.push_back({PF_BC1_SRGB, GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, 8, true, false, false, CV_BC1, 0});
        g_formats.push_back({PF_BC2_SRGB, GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, 16, true, false, false, CV_BC2, 0});
        g_formats.push_back({PF_BC3_SRGB, GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, 16, true, false, false, CV_BC3, 0});
    }
    if (rgtc)
    {
        g_formats.push_back({PF_BC4, BO1_GL_COMPRESSED_RED_RGTC1_EXT, 0, 0, 8, true, false, false, CV_NONE, 0});
        g_formats.push_back({PF_BC5, BO1_GL_COMPRESSED_RED_GREEN_RGTC2_EXT, 0, 0, 16, true, false, false, CV_NONE, 0});
    }
    else
    {
        g_formats.push_back({PF_BC4, GL_R8, GL_RED, GL_UNSIGNED_BYTE, 8, true, false, false, CV_BC4, 0});
        g_formats.push_back({PF_BC5, GL_RG8, GL_RG, GL_UNSIGNED_BYTE, 16, true, false, false, CV_BC5, 0});
    }
}

const GlFormat *FindGlFormat(uint32_t pf)
{
    for (const GlFormat &f : g_formats)
        if (f.pf == pf)
            return &f;
    return nullptr;
}

// (in the Direct3D layer's memory)
uint32_t RowBytes(const GlFormat *f, uint32_t w) { return f->compressed ? ((w + 3) / 4) * f->blockBytes : w * f->blockBytes; }
uint32_t RowCount(const GlFormat *f, uint32_t h) { return f->compressed ? (h + 3) / 4 : h; }
size_t AlignUp(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }
bool Decoded(const GlFormat *f) { return f->conv >= CV_BC1; }
// bytes per texel WebGL is given (uncompressed formats, converted ones included)
uint32_t GlTexelBytes(const GlFormat *f)
{
    switch (f->conv)
    {
    case CV_U16_HALF:
    case CV_S16_HALF: return f->blockBytes; // (16-bit to 16-bit)
    case CV_BC1:
    case CV_BC2:
    case CV_BC3: return 4;
    case CV_BC4: return 1;
    case CV_BC5: return 2;
    default: return f->blockBytes;
    }
}

// Metal enumerations -> OpenGL
const GLenum kCompare[8] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
const GLenum kStencilOp[8] = {GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP};
// (WebGL has no dual-source blending: the SRC1 factors fall back to the source's own)
const GLenum kBlendFactor[19] = {GL_ZERO,
                                 GL_ONE,
                                 GL_SRC_COLOR,
                                 GL_ONE_MINUS_SRC_COLOR,
                                 GL_SRC_ALPHA,
                                 GL_ONE_MINUS_SRC_ALPHA,
                                 GL_DST_COLOR,
                                 GL_ONE_MINUS_DST_COLOR,
                                 GL_DST_ALPHA,
                                 GL_ONE_MINUS_DST_ALPHA,
                                 GL_SRC_ALPHA_SATURATE,
                                 GL_CONSTANT_COLOR,
                                 GL_ONE_MINUS_CONSTANT_COLOR,
                                 GL_CONSTANT_ALPHA,
                                 GL_ONE_MINUS_CONSTANT_ALPHA,
                                 GL_SRC_COLOR,
                                 GL_ONE_MINUS_SRC_COLOR,
                                 GL_SRC_ALPHA,
                                 GL_ONE_MINUS_SRC_ALPHA};
const GLenum kBlendOp[5] = {GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX};
// (no mirror-clamp or border addressing: the nearest WebGL has)
const GLenum kAddress[6] = {GL_CLAMP_TO_EDGE, GL_MIRRORED_REPEAT, GL_REPEAT, GL_MIRRORED_REPEAT,
                            GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE};
const GLenum kPrim[5] = {GL_POINTS, GL_LINES, GL_LINE_STRIP, GL_TRIANGLES, GL_TRIANGLE_STRIP};

GLenum Cmp(uint8_t v) { return kCompare[v & 7]; }
GLenum BlendFactorGL(uint8_t v) { return v < 19 ? kBlendFactor[v] : GL_ONE; }
GLenum BlendOpGL(uint8_t v) { return v < 5 ? kBlendOp[v] : GL_FUNC_ADD; }

struct VertexFormatGL
{
    GLint size;
    GLenum type;
    GLboolean normalized;
    bool bgra; // D3DCOLOR: the vertex shader swaps it
};
bool VertexFormatToGL(uint32_t f, VertexFormatGL *out)
{
    switch (f)
    {
    case VF_UCHAR4: *out = {4, GL_UNSIGNED_BYTE, GL_FALSE, false}; return true;
    case VF_UCHAR4_NORM: *out = {4, GL_UNSIGNED_BYTE, GL_TRUE, false}; return true;
    case VF_SHORT2: *out = {2, GL_SHORT, GL_FALSE, false}; return true;
    case VF_SHORT4: *out = {4, GL_SHORT, GL_FALSE, false}; return true;
    case VF_SHORT2_NORM: *out = {2, GL_SHORT, GL_TRUE, false}; return true;
    case VF_SHORT4_NORM: *out = {4, GL_SHORT, GL_TRUE, false}; return true;
    case VF_USHORT2_NORM: *out = {2, GL_UNSIGNED_SHORT, GL_TRUE, false}; return true;
    case VF_USHORT4_NORM: *out = {4, GL_UNSIGNED_SHORT, GL_TRUE, false}; return true;
    case VF_HALF2: *out = {2, GL_HALF_FLOAT, GL_FALSE, false}; return true;
    case VF_HALF4: *out = {4, GL_HALF_FLOAT, GL_FALSE, false}; return true;
    case VF_FLOAT: *out = {1, GL_FLOAT, GL_FALSE, false}; return true;
    case VF_FLOAT2: *out = {2, GL_FLOAT, GL_FALSE, false}; return true;
    case VF_FLOAT3: *out = {3, GL_FLOAT, GL_FALSE, false}; return true;
    case VF_FLOAT4: *out = {4, GL_FLOAT, GL_FALSE, false}; return true;
    case VF_INT1010102_NORM: *out = {4, GL_INT_2_10_10_10_REV, GL_TRUE, false}; return true;
    case VF_UINT1010102_NORM: *out = {4, GL_UNSIGNED_INT_2_10_10_10_REV, GL_TRUE, false}; return true;
    case VF_UCHAR4_NORM_BGRA: *out = {4, GL_UNSIGNED_BYTE, GL_TRUE, true}; return true;
    default: return false;
    }
}

// ---- sampling swizzles (no texture swizzle in WebGL: the shaders apply them, bo1_swz) ---------------------------
// The swizzles of the Direct3D layer's formats (d3d9_format.cpp) as codes: R, G, B, A / 0 / 1 selectors per channel.
struct SwizzleCode
{
    uint8_t sel[4];
};
const SwizzleCode kSwizzleCodes[9] = {
    {{SW_RED, SW_GREEN, SW_BLUE, SW_ALPHA}}, // 0 identity
    {{SW_RED, SW_GREEN, SW_BLUE, SW_ONE}},   // 1 X8R8G8B8 and the other X formats
    {{SW_RED, SW_GREEN, SW_ONE, SW_ONE}},    // 2 G16R16, V8U8, G16R16F, G32R32F
    {{SW_RED, SW_RED, SW_RED, SW_ONE}},      // 3 L8, L16
    {{SW_RED, SW_RED, SW_RED, SW_GREEN}},    // 4 A8L8, A4L4
    {{SW_RED, SW_ONE, SW_ONE, SW_ONE}},      // 5 R16F, R32F
    {{SW_RED, SW_RED, SW_RED, SW_RED}},      // 6 ATI1
    {{SW_GREEN, SW_RED, SW_ONE, SW_ONE}},    // 7 ATI2
    {{SW_ZERO, SW_ZERO, SW_ZERO, SW_RED}},   // 8 A8 (stored as R8)
};

uint8_t SwizzleCodeFor(uint32_t format, const uint8_t *swizzle)
{
    static const uint8_t identity[4] = {SW_RED, SW_GREEN, SW_BLUE, SW_ALPHA};
    const uint8_t *s = swizzle ? swizzle : identity;
    uint8_t sel[4];
    for (int i = 0; i < 4; ++i)
    {
        sel[i] = s[i];
        if (format == PF_A8 && sel[i] >= SW_RED)
            sel[i] = sel[i] == SW_ALPHA ? SW_RED : SW_ZERO; // (A8 lives in R8)
    }
    for (uint8_t c = 0; c < 9; ++c)
        if (memcmp(kSwizzleCodes[c].sel, sel, 4) == 0)
            return c;
    return 0;
}
} // namespace

// ======================================================================================================================
// objects
// ======================================================================================================================
struct TransientChunk;

struct Texture
{
    GLuint id = 0;
    GLenum target = GL_TEXTURE_2D;
    uint32_t format = 0;
    const GlFormat *gf = nullptr;
    TextureType type = TEX_2D;
    uint32_t width = 1, height = 1, depth = 1, levels = 1, samples = 1;
    uint8_t swizzle = 0;      // the sampling swizzle's code (kSwizzleCodes)
    Texture *base = nullptr;  // a view: the texture it shares (its own GL texture is the base's)
    std::vector<Texture *> views;
};

struct Buffer
{
    size_t length = 0;
    uint8_t *cpu = nullptr; // NoCopy: the caller's memory; otherwise our own
    bool noCopy = false;
    bool owned = false; // (cpu is ours to free: CreateBuffer)
    void (*release)(void *, size_t, void *) = nullptr;
    void *user = nullptr;
    TransientChunk *chunk = nullptr; // scratch memory: the chunk this buffer is
    // the GL copies: for vertex and uniform data, and for indices (WebGL buffers have one kind for good)
    GLuint vbo = 0, ibo = 0;
    // NoCopy: ranges the CPU wrote that the GL copies do not have yet (under G->uploadLock)
    std::vector<std::pair<size_t, size_t>> dirty;
    bool queued = false;
};

struct ShaderObj
{
    GLuint id = 0;
    bool pixel = false;
    bool failed = false;
    std::string source; // until compiled
    std::vector<int> inputs; // vertex shaders: the input registers (layout locations) the source declares
    ~ShaderObj();
};

struct Library
{
    std::shared_ptr<ShaderObj> sh;
};

struct Function
{
    std::shared_ptr<ShaderObj> sh;
};

struct Pipeline
{
    GLuint program = 0;
    bool linked = false, failed = false;
    std::shared_ptr<ShaderObj> vs, fs;
    PipelineDesc desc{};
    uint32_t unitMask = 0;    // texture units the program samples
    uint32_t bindingMask = 0; // vertex buffer bindings (buffer index - 1) the attributes read
    uint16_t attrMask = 0;    // attributes read from buffers
    uint16_t bgraMask = 0;    // attributes that are D3DCOLOR (bo1_misc.x)
    VertexFormatGL attrFormat[MAX_VERTEX_ATTRIBUTES];
    GLint blockSize[4] = {}; // the uniform blocks' sizes (UBO_*), 0: not used
};

struct DepthStencil
{
    DepthStencilDesc d;
};

struct Sampler
{
    SamplerDesc desc{};
    GLuint id = 0;      // made at first use
    GLuint nearest = 0; // the same with point filtering, for depth textures sampled without comparison
};

struct Visibility
{
    bool active = false;
};

struct TransientChunk
{
    Buffer buffer;
    size_t size = 0;
    size_t used = 0;
    size_t uploadedV = 0, uploadedI = 0; // how much of [0, used) the GL copies have
    uint64_t frame = 0;                  // last frame that used it
};

namespace
{
// ======================================================================================================================
// global state
// ======================================================================================================================
constexpr size_t kTransientChunkSize = 4u << 20;
constexpr uint32_t kMaxFragmentTextures = 16;
constexpr uint32_t kMaxVertexTextures = 4;
constexpr uint32_t kVertexTextureUnitBase = 16; // the GLSL profile's units for vertex shader samplers
constexpr uint32_t kScratchUnit = 31;           // where textures are bound to be made and filled
constexpr int kFramesBeforeReuse = 3;           // scratch memory is reused this many frames later

enum
{
    UBO_VS = 0,
    UBO_PS = 1,
    UBO_VSFIX = 2,
    UBO_MISC = 3,
    UBO_COUNT = 4
};
const char *const kBlockNames[UBO_COUNT] = {"bo1_vs_uniforms", "bo1_ps_uniforms", "bo1_vsfix_block", "bo1_misc_block"};

enum Cap
{
    CAP_CULL,
    CAP_DEPTH,
    CAP_STENCIL,
    CAP_SCISSOR,
    CAP_OFFSET_FILL,
    CAP_A2C,
    CAP_BLEND,
    CAP_COUNT
};
const GLenum kCapEnum[CAP_COUNT] = {GL_CULL_FACE,          GL_DEPTH_TEST, GL_STENCIL_TEST, GL_SCISSOR_TEST,
                                    GL_POLYGON_OFFSET_FILL, GL_SAMPLE_ALPHA_TO_COVERAGE, GL_BLEND};

// what the GL context has (the shadow the setters compare against)
struct GlState
{
    GLuint program = 0, vao = 0, drawFbo = 0, readFbo = 0, arrayBuffer = ~0u, copyBuffer = ~0u;
    int caps[CAP_COUNT];
    int colorMask = -1;
    GLenum blend[6] = {}; // srcRGB, dstRGB, srcA, dstA, opRGB, opA
    bool indexedBlend = false;
    GLenum depthFunc = 0;
    int depthMask = -1;
    GLenum stencilFunc[2] = {0, 0};
    GLint stencilRef[2] = {-1, -1};
    GLuint stencilReadMask[2] = {0, 0};
    GLenum stencilOps[2][3] = {};
    GLint stencilWriteMask = -1;
    GLenum cullFace = 0, frontFace = 0;
    float offsetFactor = NAN, offsetUnits = NAN;
    GLint viewport[4] = {-1, -1, -1, -1};
    float depthRange[2] = {NAN, NAN};
    GLint scissor[4] = {-1, -1, -1, -1};
    float blendColor[4] = {NAN, NAN, NAN, NAN};
    GLuint tex[32] = {}, samp[32] = {};
    GLenum texTarget[32] = {};
    GLuint activeUnit = ~0u;
    GLuint ubo[UBO_COUNT] = {};
    GLintptr uboOffset[UBO_COUNT] = {};
    GLsizeiptr uboSize[UBO_COUNT] = {};
    // the main vertex array object
    struct Attr
    {
        int enabled = -1;
        GLuint buffer = ~0u;
        GLint size = 0;
        GLenum type = 0;
        GLboolean normalized = 0;
        GLsizei stride = -1;
        GLintptr offset = -1;
        GLuint divisor = ~0u;
    } attr[MAX_VERTEX_ATTRIBUTES];
    GLuint elementBuffer = ~0u;

    GlState()
    {
        for (int &c : caps)
            c = -1;
    }
};

// what the Direct3D layer asked for
struct WantState
{
    Pipeline *pipeline = nullptr;
    DepthStencil *depthStencil = nullptr;
    uint32_t stencilRef = 0;
    uint8_t cull = 0, winding = 0, fill = 0;
    float depthBias = 0, slopeScale = 0, biasClamp = 0;
    double viewport[6] = {0, 0, 1, 1, 0, 1};
    bool scissorEnable = false;
    int sx = 0, sy = 0, sw = 0, sh = 0;
    float blendColor[4] = {0, 0, 0, 0};
};

struct BoundBuffer
{
    Buffer *buf = nullptr;
    uint32_t offset = 0;
    uint32_t size = 0; // (uniform blocks)
};

struct FboKey
{
    uint64_t v[10];
    bool operator==(const FboKey &o) const { return memcmp(v, o.v, sizeof(v)) == 0; }
};
struct FboKeyHash
{
    size_t operator()(const FboKey &k) const
    {
        uint64_t h = 0xCBF29CE484222325ull;
        for (uint64_t x : k.v)
            h = (h ^ x) * 0x100000001B3ull;
        return (size_t)h;
    }
};

struct UtilProgram
{
    GLuint program = 0;
    GLint rect = -1, flip = -1, pts = -1;
};

struct Globals
{
    WebSurface *surface = nullptr;
    int context = 0;
    pthread_t renderThread;

    // capabilities
    DeviceInfo info{};
    GLint uboAlign = 256;
    float maxAnisotropy = 1;
    bool clipControl = false;      // EXT_clip_control (else the vertex shaders do it)
    bool drawBuffersIndexed = false;
    bool floatLinear = false;
    bool colorBufferFloat = false;
    bool norm16 = false;
    GLenum frontForCW = GL_CW; // the GL front-face winding of a primitive Metal calls clockwise (the self-test)

    // "command buffers": serials, all complete as soon as they are submitted (WebGL copies what it is given)
    uint64_t nextSerial = 1;
    uint64_t recordingSerial = 0;
    uint64_t lastCommittedSerial = 0;

    // render targets
    RenderTargets rt{};
    uint32_t rtWidth = 1, rtHeight = 1;
    bool rtDirty = true;
    GLuint rtFbo = 0;
    std::unordered_map<FboKey, GLuint, FboKeyHash> fbos;

    WantState want;
    GlState have;
    BoundBuffer vbuf[MAX_VERTEX_BUFFERS];
    BoundBuffer ubo[UBO_COUNT];
    Texture *vtex[kMaxVertexTextures] = {};
    Texture *ftex[kMaxFragmentTextures] = {};
    Sampler *vsamp[kMaxVertexTextures] = {};
    Sampler *fsamp[kMaxFragmentTextures] = {};
    DepthStencil defaultDS{};
    GLuint mainVao = 0, emptyVao = 0;
    Buffer *zeroUniforms = nullptr; // bo1_vsfix before the layer sets it, and short uniform blocks
    uint32_t miscCache[8] = {~0u}; // the last bo1_misc_block contents
    BoundBuffer miscBlock;

    // writes to NoCopy buffers waiting for the next draw
    std::mutex uploadLock;
    std::vector<Buffer *> dirtyBuffers;

    // scratch memory
    std::vector<std::unique_ptr<TransientChunk>> chunks;
    TransientChunk *chunk = nullptr;

    // work other threads left for the render thread (in the order they asked)
    std::mutex deferLock;
    std::vector<std::function<void()>> deferred;
    std::atomic<bool> haveDeferred{false};

    // utilities
    UtilProgram blit, present;
    GLuint linearClamp = 0, pointClamp = 0;
    GLuint gammaLut = 0;
    uint16_t gammaCache[3 * 256];
    bool vsync = true;

    // frames: Present draws the back buffer (through the gamma ramp) into frameTex and reads that back into a pixel
    // buffer; the next Present copies those pixels into a slot the page draws from (see web_shared.h). (A frame handed
    // to the page as an ImageBitmap is never freed while the thread that made it doesn't return to the browser's event
    // loop, which the render thread never does.)
    uint64_t frameIndex = 0;
    GLuint frameTex = 0, frameFbo = 0;
    int frameW = 0, frameH = 0;
    struct Readback
    {
        GLuint pbo = 0;
        size_t capacity = 0;
        GLsync fence = nullptr;
        int w = 0, h = 0, aspectW = 0, aspectH = 0, fill = 1;
        bool full = false;
    } readback[2];
    uint8_t *slot[3] = {};
    size_t slotCapacity[3] = {};

    // depth buffers standing in for a bigger (or smaller) one bound with a color target (FboFor)
    struct ScratchDepth
    {
        uint32_t w, h, format;
        GLuint tex;
    };
    std::vector<ScratchDepth> scratchDepth;
};

Globals *G;

// which thread has the context
thread_local bool t_render = false;

void SetErr(char *err, size_t errSize, const char *msg)
{
    if (err && errSize)
        snprintf(err, errSize, "%s", msg ? msg : "unknown error");
}

void LogOnce(const char *key, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void LogOnce(const char *key, const char *fmt, ...)
{
    static std::mutex m;
    static std::set<std::string> seen;
    {
        std::lock_guard<std::mutex> lock(m);
        if (!seen.insert(key).second)
            return;
    }
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[webgl] %s\n", buf);
}

bool OnRenderThread() { return t_render; }

// work for the render thread, done at its next call (now, on the render thread)
void Defer(std::function<void()> fn)
{
    if (OnRenderThread())
    {
        fn();
        return;
    }
    std::lock_guard<std::mutex> lock(G->deferLock);
    G->deferred.push_back(std::move(fn));
    G->haveDeferred.store(true, std::memory_order_release);
}

void RunDeferred()
{
    if (!G->haveDeferred.load(std::memory_order_acquire))
        return;
    std::vector<std::function<void()>> list;
    {
        std::lock_guard<std::mutex> lock(G->deferLock);
        list.swap(G->deferred);
        G->haveDeferred.store(false, std::memory_order_relaxed);
    }
    for (auto &fn : list)
        fn();
}

// the render thread with its context (after the work other threads left for it); false (logged once) elsewhere
bool RenderContext(const char *what)
{
    if (!G)
        return false;
    if (!t_render)
    {
        LogOnce(what, "%s called off the render thread: skipped", what);
        return false;
    }
    RunDeferred();
    return true;
}

// ======================================================================================================================
// state setters (compare with the shadow)
// ======================================================================================================================
void SetCap(Cap c, bool on)
{
    if (G->have.caps[c] == (int)on)
        return;
    G->have.caps[c] = on;
    if (on)
        glEnable(kCapEnum[c]);
    else
        glDisable(kCapEnum[c]);
}

void UseProgram(GLuint p)
{
    if (G->have.program != p)
    {
        G->have.program = p;
        glUseProgram(p);
    }
}

void BindVao(GLuint v)
{
    if (G->have.vao != v)
    {
        G->have.vao = v;
        glBindVertexArray(v);
        // (the element buffer binding is the VAO's)
        G->have.elementBuffer = ~0u;
    }
}

void BindDrawFbo(GLuint f)
{
    if (G->have.drawFbo != f)
    {
        G->have.drawFbo = f;
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, f);
    }
}

void BindReadFbo(GLuint f)
{
    if (G->have.readFbo != f)
    {
        G->have.readFbo = f;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, f);
    }
}

void BindArrayBuffer(GLuint b)
{
    if (G->have.arrayBuffer != b)
    {
        G->have.arrayBuffer = b;
        glBindBuffer(GL_ARRAY_BUFFER, b);
    }
}

void BindCopyBuffer(GLuint b)
{
    if (G->have.copyBuffer != b)
    {
        G->have.copyBuffer = b;
        glBindBuffer(GL_COPY_WRITE_BUFFER, b);
    }
}

void SetElementBuffer(GLuint b)
{
    if (G->have.elementBuffer != b)
    {
        G->have.elementBuffer = b;
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b);
    }
}

// Metal's write mask: R 8, G 4, B 2, A 1
void SetColorMask(int mask)
{
    if (G->have.colorMask == mask && !G->have.indexedBlend)
        return;
    G->have.colorMask = mask;
    glColorMask((mask & 8) != 0, (mask & 4) != 0, (mask & 2) != 0, (mask & 1) != 0);
}

void SetBlend(GLenum sRGB, GLenum dRGB, GLenum sA, GLenum dA, GLenum opRGB, GLenum opA)
{
    GLenum *b = G->have.blend;
    if (b[0] != sRGB || b[1] != dRGB || b[2] != sA || b[3] != dA || G->have.indexedBlend)
    {
        b[0] = sRGB, b[1] = dRGB, b[2] = sA, b[3] = dA;
        glBlendFuncSeparate(sRGB, dRGB, sA, dA);
    }
    if (b[4] != opRGB || b[5] != opA || G->have.indexedBlend)
    {
        b[4] = opRGB, b[5] = opA;
        glBlendEquationSeparate(opRGB, opA);
    }
}

void SetDepth(GLenum func, bool write)
{
    if (G->have.depthFunc != func)
    {
        G->have.depthFunc = func;
        glDepthFunc(func);
    }
    if (G->have.depthMask != (int)write)
    {
        G->have.depthMask = write;
        glDepthMask(write ? GL_TRUE : GL_FALSE);
    }
}

void SetStencilWriteMask(GLuint mask)
{
    if (G->have.stencilWriteMask != (GLint)mask)
    {
        G->have.stencilWriteMask = (GLint)mask;
        glStencilMaskSeparate(GL_FRONT_AND_BACK, mask);
    }
}

void SetStencilFace(int face, GLenum func, GLint ref, GLuint readMask, GLenum sfail, GLenum dpfail, GLenum dppass)
{
    const GLenum f = face ? GL_BACK : GL_FRONT;
    GlState &h = G->have;
    if (h.stencilFunc[face] != func || h.stencilRef[face] != ref || h.stencilReadMask[face] != readMask)
    {
        h.stencilFunc[face] = func;
        h.stencilRef[face] = ref;
        h.stencilReadMask[face] = readMask;
        glStencilFuncSeparate(f, func, ref, readMask);
    }
    if (h.stencilOps[face][0] != sfail || h.stencilOps[face][1] != dpfail || h.stencilOps[face][2] != dppass)
    {
        h.stencilOps[face][0] = sfail;
        h.stencilOps[face][1] = dpfail;
        h.stencilOps[face][2] = dppass;
        glStencilOpSeparate(f, sfail, dpfail, dppass);
    }
}

void SetCullFace(GLenum f)
{
    if (G->have.cullFace != f)
    {
        G->have.cullFace = f;
        glCullFace(f);
    }
}

void SetFrontFace(GLenum f)
{
    if (G->have.frontFace != f)
    {
        G->have.frontFace = f;
        glFrontFace(f);
    }
}

void SetPolygonOffset(float factor, float units)
{
    if (G->have.offsetFactor != factor || G->have.offsetUnits != units)
    {
        G->have.offsetFactor = factor;
        G->have.offsetUnits = units;
        glPolygonOffset(factor, units);
    }
}

void SetViewportGL(float x, float y, float w, float h, float n, float f)
{
    const GLint v[4] = {(GLint)lroundf(x), (GLint)lroundf(y), std::max((GLint)lroundf(w), 0),
                        std::max((GLint)lroundf(h), 0)};
    if (memcmp(G->have.viewport, v, sizeof(v)) != 0)
    {
        memcpy(G->have.viewport, v, sizeof(v));
        glViewport(v[0], v[1], v[2], v[3]);
    }
    if (G->have.depthRange[0] != n || G->have.depthRange[1] != f)
    {
        G->have.depthRange[0] = n;
        G->have.depthRange[1] = f;
        glDepthRangef(n, f);
    }
}

void SetScissorRect(GLint x, GLint y, GLint w, GLint h)
{
    GLint *s = G->have.scissor;
    if (s[0] != x || s[1] != y || s[2] != w || s[3] != h)
    {
        s[0] = x, s[1] = y, s[2] = w, s[3] = h;
        glScissor(x, y, w, h);
    }
}

void SetBlendColorGL(const float c[4])
{
    if (memcmp(G->have.blendColor, c, sizeof(float) * 4) != 0)
    {
        memcpy(G->have.blendColor, c, sizeof(float) * 4);
        glBlendColor(c[0], c[1], c[2], c[3]);
    }
}

void ActiveUnit(GLuint unit)
{
    if (G->have.activeUnit != unit)
    {
        G->have.activeUnit = unit;
        glActiveTexture(GL_TEXTURE0 + unit);
    }
}

void BindTextureUnit(uint32_t unit, GLenum target, GLuint tex)
{
    GlState &h = G->have;
    if (h.tex[unit] == tex && (h.texTarget[unit] == target || !tex))
        return;
    ActiveUnit(unit);
    // (a unit has a binding per target: the old one is taken off when the target changes)
    if (h.tex[unit] && h.texTarget[unit] != target)
        glBindTexture(h.texTarget[unit], 0);
    glBindTexture(target, tex);
    h.tex[unit] = tex;
    h.texTarget[unit] = target;
}

void BindSamplerUnit(uint32_t unit, GLuint s)
{
    if (G->have.samp[unit] != s)
    {
        G->have.samp[unit] = s;
        glBindSampler(unit, s);
    }
}

void BindUniformBuffer(uint32_t binding, GLuint buf, GLintptr offset, GLsizeiptr size)
{
    GlState &h = G->have;
    if (h.ubo[binding] != buf || h.uboOffset[binding] != offset || h.uboSize[binding] != size)
    {
        h.ubo[binding] = buf;
        h.uboOffset[binding] = offset;
        h.uboSize[binding] = size;
        glBindBufferRange(GL_UNIFORM_BUFFER, binding, buf, offset, size);
    }
}

// a deleted name may come back for a new object: nothing in the shadow may still name it
void ForgetTextureName(GLuint id)
{
    for (GLuint &t : G->have.tex)
        if (t == id)
            t = 0;
}

void ForgetBufferName(GLuint id)
{
    GlState &h = G->have;
    for (uint32_t i = 0; i < UBO_COUNT; ++i)
        if (h.ubo[i] == id)
        {
            h.ubo[i] = 0;
            h.uboSize[i] = 0;
        }
    for (GlState::Attr &a : h.attr)
        if (a.buffer == id)
            a.buffer = ~0u;
    if (h.elementBuffer == id)
        h.elementBuffer = ~0u;
    if (h.arrayBuffer == id)
        h.arrayBuffer = ~0u;
    if (h.copyBuffer == id)
        h.copyBuffer = ~0u;
}

void ForgetSamplerName(GLuint id)
{
    for (GLuint &s : G->have.samp)
        if (s == id)
            s = 0;
}
} // namespace

ShaderObj::~ShaderObj()
{
    if (!id || !G)
        return;
    const GLuint shader = id;
    Defer([shader] { glDeleteShader(shader); });
}

namespace
{
// ======================================================================================================================
// "command buffers"
// ======================================================================================================================
void EnsureCmdBuf()
{
    if (!G->recordingSerial)
        G->recordingSerial = G->nextSerial++;
}

void CommitCurrent()
{
    if (!G->recordingSerial)
        return;
    glFlush();
    G->lastCommittedSerial = G->recordingSerial;
    G->recordingSerial = 0;
}

// ======================================================================================================================
// buffers: CPU memory with GL copies
// ======================================================================================================================
// the GL copy of a buffer for vertex and uniform data, made (with the buffer's bytes) the first time it is needed
GLuint EnsureVbo(Buffer *b)
{
    if (!b->vbo)
    {
        glGenBuffers(1, &b->vbo);
        // (the copy-write target first: the buffer becomes one WebGL never lets hold indices)
        BindCopyBuffer(b->vbo);
        glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)std::max<size_t>(b->length, 16), b->chunk ? nullptr : b->cpu,
                     GL_DYNAMIC_DRAW);
    }
    return b->vbo;
}

// the same for indices
GLuint EnsureIbo(Buffer *b)
{
    if (!b->ibo)
    {
        glGenBuffers(1, &b->ibo);
        // (bound as an element array once, in the main vertex array object, which binds it there anyway)
        BindVao(G->mainVao);
        SetElementBuffer(b->ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)std::max<size_t>(b->length, 16), b->chunk ? nullptr : b->cpu,
                     GL_DYNAMIC_DRAW);
    }
    return b->ibo;
}

void UploadRange(GLuint glbuf, size_t offset, size_t length, const void *data)
{
    BindCopyBuffer(glbuf);
    glBufferSubData(GL_COPY_WRITE_BUFFER, (GLintptr)offset, (GLsizeiptr)length, data);
}

// the written ranges of NoCopy buffers into their GL copies (before a draw)
void FlushPendingUploads()
{
    std::vector<Buffer *> list;
    std::lock_guard<std::mutex> lock(G->uploadLock);
    if (G->dirtyBuffers.empty())
        return;
    list.swap(G->dirtyBuffers);
    for (Buffer *b : list)
    {
        b->queued = false;
        for (const auto &r : b->dirty)
        {
            if (b->vbo)
                UploadRange(b->vbo, r.first, r.second - r.first, b->cpu + r.first);
            if (b->ibo)
                UploadRange(b->ibo, r.first, r.second - r.first, b->cpu + r.first);
        }
        b->dirty.clear();
    }
}

// scratch memory used since the last upload into the GL copy of the given kind
void SyncChunk(TransientChunk *c, bool index)
{
    size_t &uploaded = index ? c->uploadedI : c->uploadedV;
    if (uploaded >= c->used)
        return;
    const GLuint glbuf = index ? EnsureIbo(&c->buffer) : EnsureVbo(&c->buffer);
    UploadRange(glbuf, uploaded, c->used - uploaded, c->buffer.cpu + uploaded);
    uploaded = c->used;
}

// a buffer's GL copy of the given kind, up to date
GLuint GlBufferFor(Buffer *b, bool index)
{
    if (b->chunk)
    {
        SyncChunk(b->chunk, index);
        b->chunk->frame = G->frameIndex;
    }
    return index ? EnsureIbo(b) : EnsureVbo(b);
}

void *Transient(size_t length, size_t alignment, Buffer **buf, uint32_t *offset)
{
    EnsureCmdBuf();
    alignment = std::max<size_t>(alignment, 16);
    TransientChunk *c = G->chunk;
    if (!c || AlignUp(c->used, alignment) + length > c->size)
    {
        c = nullptr;
        for (auto &ch : G->chunks)
        {
            if (ch.get() != G->chunk && ch->frame + kFramesBeforeReuse <= G->frameIndex && ch->size >= length)
            {
                c = ch.get();
                break;
            }
        }
        if (!c)
        {
            auto ch = std::make_unique<TransientChunk>();
            ch->size = std::max(kTransientChunkSize, AlignUp(length, 65536));
            ch->buffer.cpu = (uint8_t *)aligned_alloc(256, ch->size);
            ch->buffer.length = ch->size;
            ch->buffer.chunk = ch.get();
            c = ch.get();
            G->chunks.push_back(std::move(ch));
        }
        c->used = 0;
        c->uploadedV = c->uploadedI = 0;
        G->chunk = c;
    }
    const size_t off = AlignUp(c->used, alignment);
    c->used = off + length;
    c->frame = G->frameIndex;
    *buf = &c->buffer;
    *offset = (uint32_t)off;
    return c->buffer.cpu + off;
}

// ======================================================================================================================
// textures: made (and filled) on the render thread
// ======================================================================================================================
bool HasStencil(uint32_t f) { return f == PF_D32F_S8 || f == PF_DEPTH24_STENCIL8; }
bool IsDepthFormat(uint32_t f) { return f == PF_D16 || f == PF_D32F || HasStencil(f) || f == PF_X32_STENCIL8; }
bool IsCompressed(uint32_t f) { return f >= PF_BC1 && f <= 143; }

Texture *Storage(Texture *t) { return t && t->base ? t->base : t; }

// the GL texture, made the first time it is needed
GLuint EnsureTexture(Texture *t)
{
    t = Storage(t);
    if (!t || t->id)
        return t ? t->id : 0;
    glGenTextures(1, &t->id);
    BindTextureUnit(kScratchUnit, t->target, t->id);
    const GlFormat *f = t->gf;
    if (t->type == TEX_3D)
        glTexStorage3D(GL_TEXTURE_3D, (GLsizei)t->levels, f->internal, (GLsizei)t->width, (GLsizei)t->height,
                       (GLsizei)t->depth);
    else
        glTexStorage2D(t->target, (GLsizei)t->levels, f->internal, (GLsizei)t->width, (GLsizei)t->height);
    glTexParameteri(t->target, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(t->target, GL_TEXTURE_MAX_LEVEL, (GLint)t->levels - 1);
    // (glGetError waits for the GPU process: only when asked for)
    static const bool check = getenv("BO1_GL_DEBUG") != nullptr;
    const GLenum err = check ? glGetError() : GL_NO_ERROR;
    if (err != GL_NO_ERROR)
    {
        char key[48];
        snprintf(key, sizeof(key), "texstorage%u_%x", t->format, err);
        LogOnce(key, "cannot make a %ux%u texture of format %u (GL error 0x%x)", t->width, t->height, t->format, err);
    }
    return t->id;
}

// ---- conversions of uploaded texels -----------------------------------------------------------------------------
uint16_t FloatToHalf(float f)
{
    uint32_t x;
    memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000;
    int32_t e = (int32_t)((x >> 23) & 0xFF) - 127 + 15;
    uint32_t m = x & 0x7FFFFF;
    if (e <= 0)
    {
        if (e < -10)
            return (uint16_t)sign;
        m = (m | 0x800000) >> (1 - e);
        return (uint16_t)(sign | ((m + 0x1000) >> 13));
    }
    if (e >= 31)
        return (uint16_t)(sign | 0x7C00);
    return (uint16_t)(sign | ((uint32_t)e << 10) | ((m + 0x1000) >> 13));
}

float HalfToFloat(uint16_t h)
{
    const uint32_t sign = (uint32_t)(h & 0x8000) << 16;
    uint32_t e = (h >> 10) & 0x1F, m = h & 0x3FF;
    uint32_t x;
    if (e == 0)
    {
        if (!m)
            x = sign;
        else
        {
            e = 127 - 15 + 1;
            while (!(m & 0x400))
            {
                m <<= 1;
                --e;
            }
            x = sign | (e << 23) | ((m & 0x3FF) << 13);
        }
    }
    else if (e == 31)
        x = sign | 0x7F800000 | (m << 13);
    else
        x = sign | ((e - 15 + 127) << 23) | (m << 13);
    float f;
    memcpy(&f, &x, 4);
    return f;
}

// ---- S3TC / RGTC decoding (browsers without the extensions) ------------------------------------------------------
void DecodeColorBlock(const uint8_t *b, uint8_t out[16][4], bool dxt1)
{
    const uint16_t c0 = (uint16_t)(b[0] | (b[1] << 8)), c1 = (uint16_t)(b[2] | (b[3] << 8));
    uint8_t pal[4][4];
    auto expand = [](uint16_t c, uint8_t *p) {
        p[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
        p[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
        p[2] = (uint8_t)((c & 31) * 255 / 31);
        p[3] = 255;
    };
    expand(c0, pal[0]);
    expand(c1, pal[1]);
    if (!dxt1 || c0 > c1)
    {
        for (int k = 0; k < 3; ++k)
        {
            pal[2][k] = (uint8_t)((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = (uint8_t)((pal[0][k] + 2 * pal[1][k]) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    }
    else
    {
        for (int k = 0; k < 3; ++k)
        {
            pal[2][k] = (uint8_t)((pal[0][k] + pal[1][k]) / 2);
            pal[3][k] = 0;
        }
        pal[2][3] = 255;
        pal[3][3] = 0;
    }
    const uint32_t bits = (uint32_t)b[4] | ((uint32_t)b[5] << 8) | ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 24);
    for (int i = 0; i < 16; ++i)
        memcpy(out[i], pal[(bits >> (2 * i)) & 3], 4);
}

void DecodeAlphaBlock(const uint8_t *b, uint8_t out[16])
{
    uint8_t a[8];
    a[0] = b[0];
    a[1] = b[1];
    if (a[0] > a[1])
        for (int i = 1; i < 7; ++i)
            a[i + 1] = (uint8_t)(((7 - i) * a[0] + i * a[1]) / 7);
    else
    {
        for (int i = 1; i < 5; ++i)
            a[i + 1] = (uint8_t)(((5 - i) * a[0] + i * a[1]) / 5);
        a[6] = 0;
        a[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i)
        bits |= (uint64_t)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; ++i)
        out[i] = a[(bits >> (3 * i)) & 7];
}

// blocks (rowPitch bytes per row of blocks) of a w x h region -> tightly packed texels of the GL format
void DecodeBlocks(Conv conv, const uint8_t *src, uint32_t rowPitch, uint32_t w, uint32_t h, std::vector<uint8_t> &out)
{
    const uint32_t texel = conv <= CV_BC3 ? 4 : conv == CV_BC4 ? 1 : 2;
    out.assign((size_t)w * h * texel, 0);
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    const uint32_t blockBytes = conv == CV_BC1 || conv == CV_BC4 ? 8 : 16;
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx)
        {
            const uint8_t *b = src + (size_t)by * rowPitch + (size_t)bx * blockBytes;
            uint8_t rgba[16][4];
            uint8_t ch0[16], ch1[16];
            switch (conv)
            {
            case CV_BC1: DecodeColorBlock(b, rgba, true); break;
            case CV_BC2:
                DecodeColorBlock(b + 8, rgba, false);
                for (int i = 0; i < 16; ++i)
                    rgba[i][3] = (uint8_t)(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
                break;
            case CV_BC3:
                DecodeColorBlock(b + 8, rgba, false);
                DecodeAlphaBlock(b, ch0);
                for (int i = 0; i < 16; ++i)
                    rgba[i][3] = ch0[i];
                break;
            case CV_BC4: DecodeAlphaBlock(b, ch0); break;
            default:
                DecodeAlphaBlock(b, ch0);
                DecodeAlphaBlock(b + 8, ch1);
                break;
            }
            for (uint32_t py = 0; py < 4; ++py)
                for (uint32_t px = 0; px < 4; ++px)
                {
                    const uint32_t x = bx * 4 + px, y = by * 4 + py;
                    if (x >= w || y >= h)
                        continue;
                    const int i = (int)(py * 4 + px);
                    uint8_t *d = out.data() + ((size_t)y * w + x) * texel;
                    if (texel == 4)
                        memcpy(d, rgba[i], 4);
                    else if (texel == 1)
                        d[0] = ch0[i];
                    else
                    {
                        d[0] = ch0[i];
                        d[1] = ch1[i];
                    }
                }
        }
}

// rows of the Direct3D layer's texels -> tightly packed texels as WebGL takes them (converted where needed)
const uint8_t *PrepareTexels(const GlFormat *f, const uint8_t *src, uint32_t w, uint32_t h, uint32_t d,
                             uint32_t rowPitch, size_t imageStride, std::vector<uint8_t> &tmp)
{
    const uint32_t rows = RowCount(f, h);
    const uint32_t tight = RowBytes(f, w);
    if (Decoded(f))
    {
        std::vector<uint8_t> one;
        tmp.clear();
        for (uint32_t k = 0; k < d; ++k)
        {
            DecodeBlocks(f->conv, src + k * imageStride, rowPitch, w, h, one);
            tmp.insert(tmp.end(), one.begin(), one.end());
        }
        return tmp.data();
    }
    const bool tightRows = rowPitch == tight && (d == 1 || imageStride == (size_t)tight * rows);
    if (tightRows && f->conv == CV_NONE)
        return src;
    tmp.resize((size_t)tight * rows * d);
    for (uint32_t k = 0; k < d; ++k)
        for (uint32_t r = 0; r < rows; ++r)
            memcpy(tmp.data() + ((size_t)k * rows + r) * tight, src + k * imageStride + (size_t)r * rowPitch, tight);
    uint8_t *p = tmp.data();
    const size_t n = tmp.size();
    switch (f->conv)
    {
    case CV_BGRA8:
        for (size_t i = 0; i + 3 < n; i += 4)
            std::swap(p[i], p[i + 2]);
        break;
    case CV_BGR10A2:
        for (size_t i = 0; i + 3 < n; i += 4)
        {
            uint32_t v;
            memcpy(&v, p + i, 4);
            v = (v & 0xC00FFC00u) | ((v & 0x3FFu) << 20) | ((v >> 20) & 0x3FFu);
            memcpy(p + i, &v, 4);
        }
        break;
    case CV_U16_HALF:
        for (size_t i = 0; i + 1 < n; i += 2)
        {
            uint16_t v;
            memcpy(&v, p + i, 2);
            v = FloatToHalf(v / 65535.0f);
            memcpy(p + i, &v, 2);
        }
        break;
    case CV_S16_HALF:
        for (size_t i = 0; i + 1 < n; i += 2)
        {
            int16_t v;
            memcpy(&v, p + i, 2);
            const uint16_t hv = FloatToHalf(std::max(v / 32767.0f, -1.0f));
            memcpy(p + i, &hv, 2);
        }
        break;
    default: break;
    }
    return p;
}

void Upload(Texture *t, uint32_t slice, uint32_t level, uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h,
            uint32_t d, const void *data, uint32_t rowPitch, uint32_t slicePitch)
{
    t = Storage(t);
    const GlFormat *f = t->gf;
    const uint32_t rows = RowCount(f, h);
    const size_t imageBytes = (size_t)rowPitch * rows;
    const size_t imageStride = t->type == TEX_3D ? (slicePitch ? slicePitch : imageBytes) : imageBytes;
    std::vector<uint8_t> tmp;
    const uint8_t *src = PrepareTexels(f, (const uint8_t *)data, w, h, d, rowPitch, imageStride, tmp);
    BindTextureUnit(kScratchUnit, t->target, EnsureTexture(t));
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    const GLenum target = t->type == TEX_CUBE ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + slice : t->target;
    if (f->compressed && !Decoded(f))
    {
        const GLsizei imageSize = (GLsizei)((size_t)RowBytes(f, w) * rows * d);
        if (t->type == TEX_3D)
            glCompressedTexSubImage3D(GL_TEXTURE_3D, (GLint)level, (GLint)x, (GLint)y, (GLint)z, (GLsizei)w, (GLsizei)h,
                                      (GLsizei)d, f->internal, imageSize, src);
        else
            glCompressedTexSubImage2D(target, (GLint)level, (GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, f->internal,
                                      imageSize, src);
    }
    else if (t->type == TEX_3D)
        glTexSubImage3D(GL_TEXTURE_3D, (GLint)level, (GLint)x, (GLint)y, (GLint)z, (GLsizei)w, (GLsizei)h, (GLsizei)d,
                        f->format, f->type, src);
    else
        glTexSubImage2D(target, (GLint)level, (GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, f->format, f->type, src);
}

void ZeroRows(void *dst, uint32_t rows, uint32_t rowPitch, uint32_t tight)
{
    for (uint32_t r = 0; r < rows; ++r)
        memset((uint8_t *)dst + (size_t)r * rowPitch, 0, std::min(tight, rowPitch));
}

// ======================================================================================================================
// framebuffers
// ======================================================================================================================
void AttachTexture(GLenum fboTarget, GLenum attachment, Texture *t, uint32_t level, uint32_t slice)
{
    t = Storage(t);
    const GLuint id = EnsureTexture(t);
    if (t->type == TEX_3D)
        glFramebufferTextureLayer(fboTarget, attachment, id, (GLint)level, (GLint)slice);
    else if (t->type == TEX_CUBE)
        glFramebufferTexture2D(fboTarget, attachment, GL_TEXTURE_CUBE_MAP_POSITIVE_X + slice, id, (GLint)level);
    else
        glFramebufferTexture2D(fboTarget, attachment, GL_TEXTURE_2D, id, (GLint)level);
}

FboKey KeyFor(const RenderTargets &rt)
{
    FboKey k;
    for (int i = 0; i < 4; ++i)
    {
        k.v[i * 2] = (uint64_t)(uintptr_t)Storage(rt.color[i]);
        k.v[i * 2 + 1] = rt.color[i] ? ((uint64_t)rt.colorLevel[i] | ((uint64_t)rt.colorSlice[i] << 32)) : 0;
    }
    k.v[8] = (uint64_t)(uintptr_t)Storage(rt.depth);
    k.v[9] = rt.depth ? ((uint64_t)rt.depthLevel | ((uint64_t)rt.depthSlice << 32)) : 0;
    return k;
}

// a depth texture of a size and format of its own, kept for the next pass that needs one (FboFor)
GLuint ScratchDepthFor(uint32_t w, uint32_t h, const Texture *like)
{
    for (const Globals::ScratchDepth &s : G->scratchDepth)
        if (s.w == w && s.h == h && s.format == like->format)
            return s.tex;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    BindTextureUnit(kScratchUnit, GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, like->gf->internal, (GLsizei)w, (GLsizei)h);
    G->scratchDepth.push_back({w, h, like->format, tex});
    return tex;
}

// the framebuffer object for a set of attachments (cached until one of its textures goes); 0 when it has none
GLuint FboFor(const RenderTargets &rt)
{
    bool any = false;
    for (int i = 0; i < 4; ++i)
        any |= rt.color[i] != nullptr;
    any |= rt.depth != nullptr;
    if (!any)
        return 0;
    const FboKey key = KeyFor(rt);
    auto it = G->fbos.find(key);
    if (it != G->fbos.end())
        return it->second;
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    BindDrawFbo(fbo);
    GLenum bufs[4];
    int count = 0;
    for (int i = 0; i < 4; ++i)
    {
        bufs[i] = GL_NONE;
        if (Texture *t = rt.color[i])
        {
            AttachTexture(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, t, rt.colorLevel[i], rt.colorSlice[i]);
            bufs[i] = GL_COLOR_ATTACHMENT0 + i;
            count = i + 1;
        }
    }
    glDrawBuffers(std::max(count, 1), bufs);
    if (Texture *d = Storage(rt.depth))
    {
        const GLenum attachment = HasStencil(d->format) ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
        // WebGL wants all of a framebuffer's attachments the same size, where Direct3D 9 draws into the top-left
        // corner of a depth buffer bigger than the color target. Such a depth buffer is replaced by one of the
        // target's size, whose contents start undefined (the passes that do this clear it, or don't test depth).
        uint32_t cw = 0, ch = 0;
        for (int i = 0; i < 4 && !cw; ++i)
        {
            if (const Texture *c = Storage(rt.color[i]))
            {
                cw = std::max<uint32_t>(c->width >> rt.colorLevel[i], 1);
                ch = std::max<uint32_t>(c->height >> rt.colorLevel[i], 1);
            }
        }
        const uint32_t dw = std::max<uint32_t>(d->width >> rt.depthLevel, 1);
        const uint32_t dh = std::max<uint32_t>(d->height >> rt.depthLevel, 1);
        if (cw && (cw != dw || ch != dh) && d->type == TEX_2D)
        {
            char key2[64];
            snprintf(key2, sizeof(key2), "depthsize%ux%u_%ux%u", cw, ch, dw, dh);
            LogOnce(key2, "a %ux%u depth buffer with a %ux%u color target: drawn with a depth buffer of the target's size",
                    dw, dh, cw, ch);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, attachment, GL_TEXTURE_2D, ScratchDepthFor(cw, ch, d), 0);
        }
        else
            AttachTexture(GL_DRAW_FRAMEBUFFER, attachment, d, rt.depthLevel, rt.depthSlice);
    }
    const GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        char key2[64];
        snprintf(key2, sizeof(key2), "fbo%x_%u_%u", status, rt.color[0] ? rt.color[0]->format : 0,
                 rt.depth ? rt.depth->format : 0);
        LogOnce(key2, "render targets not usable together (framebuffer status 0x%x): color %u/%u/%u/%u depth %u", status,
                rt.color[0] ? rt.color[0]->format : 0, rt.color[1] ? rt.color[1]->format : 0,
                rt.color[2] ? rt.color[2]->format : 0, rt.color[3] ? rt.color[3]->format : 0,
                rt.depth ? rt.depth->format : 0);
    }
    G->fbos.emplace(key, fbo);
    return fbo;
}

GLuint FboForTexture(Texture *t, uint32_t level, uint32_t slice)
{
    RenderTargets rt{};
    if (IsDepthFormat(t->format))
    {
        rt.depth = t;
        rt.depthLevel = level;
        rt.depthSlice = slice;
    }
    else
    {
        rt.color[0] = t;
        rt.colorLevel[0] = level;
        rt.colorSlice[0] = slice;
    }
    return FboFor(rt);
}

void ForgetFbosOf(Texture *t)
{
    const uint64_t p = (uint64_t)(uintptr_t)Storage(t);
    for (auto it = G->fbos.begin(); it != G->fbos.end();)
    {
        const FboKey &k = it->first;
        if (k.v[0] == p || k.v[2] == p || k.v[4] == p || k.v[6] == p || k.v[8] == p)
        {
            if (G->rtFbo == it->second)
            {
                G->rtFbo = 0;
                G->rtDirty = true;
            }
            if (G->have.drawFbo == it->second)
                BindDrawFbo(0);
            if (G->have.readFbo == it->second)
                BindReadFbo(0);
            glDeleteFramebuffers(1, &it->second);
            it = G->fbos.erase(it);
        }
        else
            ++it;
    }
}

void ComputeTargetSize()
{
    Texture *t = nullptr;
    uint32_t level = 0;
    for (int i = 0; i < 4 && !t; ++i)
    {
        if (G->rt.color[i])
        {
            t = G->rt.color[i];
            level = G->rt.colorLevel[i];
        }
    }
    if (!t && G->rt.depth)
    {
        t = G->rt.depth;
        level = G->rt.depthLevel;
    }
    if (!t)
        return;
    G->rtWidth = std::max<uint32_t>(t->width >> level, 1);
    G->rtHeight = std::max<uint32_t>(t->height >> level, 1);
}

GLuint BindRenderTargets()
{
    if (G->rtDirty || !G->rtFbo)
    {
        G->rtFbo = FboFor(G->rt);
        G->rtDirty = false;
    }
    BindDrawFbo(G->rtFbo);
    return G->rtFbo;
}

// clears, blits and utility draws: no scissor, every channel written
void OpenMasks()
{
    SetCap(CAP_SCISSOR, false);
    SetColorMask(15);
    SetDepth(G->have.depthFunc ? G->have.depthFunc : GL_ALWAYS, true);
    SetStencilWriteMask(0xFF);
}

bool ScissorEmpty(const WantState &s)
{
    if (!s.scissorEnable)
        return false;
    const int x0 = std::clamp(s.sx, 0, (int)G->rtWidth);
    const int y0 = std::clamp(s.sy, 0, (int)G->rtHeight);
    const int x1 = std::clamp(s.sx + s.sw, x0, (int)G->rtWidth);
    const int y1 = std::clamp(s.sy + s.sh, y0, (int)G->rtHeight);
    return x1 <= x0 || y1 <= y0;
}

// ======================================================================================================================
// shaders: the BO1 GLSL profile's output as GLSL ES 3.00
// ======================================================================================================================
// What every shader starts with: the version and precision, the Metal-style type names the profile's emitters use,
// and the per-draw table of the units' sampling swizzles (with the D3DCOLOR vertex inputs in bo1_misc.x).
const char kShaderHeader[] =
    "#version 300 es\n"
    "precision highp float;\nprecision highp int;\nprecision highp sampler2D;\nprecision highp sampler3D;\n"
    "precision highp samplerCube;\nprecision highp sampler2DShadow;\n"
    "#define float2 vec2\n#define float3 vec3\n#define float4 vec4\n"
    "#define int2 ivec2\n#define int3 ivec3\n#define int4 ivec4\n"
    "#define bool2 bvec2\n#define bool3 bvec3\n#define bool4 bvec4\n"
    "#define FLT_MAX 3.402823466e+38\n"
    "layout(std140) uniform bo1_misc_block { uvec4 bo1_swzcodes; uvec4 bo1_misc; };\n"
    "vec4 bo1_swz(vec4 c, int i) {\n"
    "    uint code = (bo1_swzcodes[i >> 3] >> uint((i & 7) * 4)) & 15u;\n"
    "    if (code == 0u) return c;\n"
    "    if (code == 1u) return vec4(c.rgb, 1.0);\n"
    "    if (code == 2u) return vec4(c.rg, 1.0, 1.0);\n"
    "    if (code == 3u) return vec4(c.rrr, 1.0);\n"
    "    if (code == 4u) return c.rrrg;\n"
    "    if (code == 5u) return vec4(c.r, 1.0, 1.0, 1.0);\n"
    "    if (code == 6u) return c.rrrr;\n"
    "    if (code == 7u) return vec4(c.g, c.r, 1.0, 1.0);\n"
    "    return vec4(0.0, 0.0, 0.0, c.r);\n"
    "}\n";

// The sampling helpers with the unit's swizzle (inserted after the profile's helpers, which they call). Depth
// textures (sampler2DShadow) have no swizzle.
const char kSwizzleWrappersPS[] =
    "vec4 bo1_tex2d_w(int i, sampler2D s, vec4 c, int kind, int mode) { return bo1_swz(bo1_tex2d(s, c, kind, mode), i); }\n"
    "vec4 bo1_tex2d_w(int i, sampler2DShadow s, vec4 c, int kind, int mode) { return bo1_tex2d(s, c, kind, mode); }\n"
    "vec4 bo1_tex2dgrad_w(int i, sampler2D s, vec4 c, vec4 dx, vec4 dy, int mode) { return bo1_swz(bo1_tex2dgrad(s, c, dx, dy, mode), i); }\n"
    "vec4 bo1_tex2dgrad_w(int i, sampler2DShadow s, vec4 c, vec4 dx, vec4 dy, int mode) { return bo1_tex2dgrad(s, c, dx, dy, mode); }\n"
    "vec4 bo1_texcube_w(int i, samplerCube s, vec4 c, int kind, int mode) { return bo1_swz(bo1_texcube(s, c, kind, mode), i); }\n"
    "vec4 bo1_texcubegrad_w(int i, samplerCube s, vec4 c, vec4 dx, vec4 dy, int mode) { return bo1_swz(bo1_texcubegrad(s, c, dx, dy, mode), i); }\n"
    "vec4 bo1_tex3d_w(int i, sampler3D s, vec4 c, int kind, int mode) { return bo1_swz(bo1_tex3d(s, c, kind, mode), i); }\n"
    "vec4 bo1_tex3dgrad_w(int i, sampler3D s, vec4 c, vec4 dx, vec4 dy, int mode) { return bo1_swz(bo1_tex3dgrad(s, c, dx, dy, mode), i); }\n";
const char kSwizzleWrappersVS[] =
    "vec4 bo1_vtex2d_w(int i, sampler2D s, vec4 c, int mode) { return bo1_swz(bo1_vtex2d(s, c, mode), 16 + i); }\n"
    "vec4 bo1_vtex2d_w(int i, sampler2DShadow s, vec4 c, int mode) { return bo1_vtex2d(s, c, mode); }\n"
    "vec4 bo1_vtexcube_w(int i, samplerCube s, vec4 c, int mode) { return bo1_swz(bo1_vtexcube(s, c, mode), 16 + i); }\n"
    "vec4 bo1_vtex3d_w(int i, sampler3D s, vec4 c, int mode) { return bo1_swz(bo1_vtex3d(s, c, mode), 16 + i); }\n";

// in s, every occurrence of from replaced by to
void ReplaceAll(std::string &s, const char *from, const char *to)
{
    const size_t n = strlen(from), m = strlen(to);
    for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + m))
        s.replace(p, n, to);
}

// The profile's output made GLSL ES: no layout bindings (set by name after linking), the sampling helpers with the
// units' swizzles, and in vertex shaders a main() of ours around the profile's: it swaps the D3DCOLOR inputs and,
// without EXT_clip_control, turns clip space as glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE) would.
std::string AdaptSource(ShaderObj *s)
{
    std::string src = s->source;
    ReplaceAll(src, "layout(std140, binding = 0)", "layout(std140)");
    ReplaceAll(src, "layout(std140, binding = 1)", "layout(std140)");
    ReplaceAll(src, "layout(std140, binding = 2)", "layout(std140)");
    // samplers: "layout(binding = N) uniform TYPE NAME;"
    for (size_t p = src.find("layout(binding = "); p != std::string::npos; p = src.find("layout(binding = ", p))
    {
        const size_t close = src.find(") ", p);
        if (close == std::string::npos)
            break;
        src.erase(p, close + 2 - p);
    }
    // sampling calls: NAME(ps_sN, -> NAME_w(N, ps_sN,
    static const char *const fns[] = {"bo1_tex2dgrad", "bo1_tex2d", "bo1_texcubegrad", "bo1_texcube", "bo1_tex3dgrad",
                                      "bo1_tex3d", "bo1_vtex2d", "bo1_vtexcube", "bo1_vtex3d"};
    for (const char *fn : fns)
    {
        const std::string call = std::string(fn) + "(";
        for (size_t p = src.find(call); p != std::string::npos; p = src.find(call, p + 1))
        {
            const size_t arg = p + call.size();
            if (src.compare(arg, 4, "ps_s") != 0 && src.compare(arg, 4, "vs_s") != 0)
                continue;
            size_t e = arg + 4;
            while (e < src.size() && isdigit((unsigned char)src[e]))
                ++e;
            if (e == arg + 4 || src[e] != ',')
                continue;
            const std::string reg = src.substr(arg + 4, e - arg - 4);
            src.replace(p, call.size(), std::string(fn) + "_w(" + reg + ", ");
        }
    }
    // the wrappers after the profile's helpers
    const size_t helpers = src.find("#ifndef BO1_HELPERS");
    if (helpers != std::string::npos)
    {
        const size_t end = src.find("#endif", helpers);
        if (end != std::string::npos)
        {
            const size_t eol = src.find('\n', end);
            src.insert(eol == std::string::npos ? src.size() : eol + 1, s->pixel ? kSwizzleWrappersPS : kSwizzleWrappersVS);
        }
    }
    std::string out = kShaderHeader;
    if (!s->pixel)
    {
        // inputs: "layout(location = N) in float4 NAME;" -> the attribute under another name, NAME a global main sets
        std::string assigns;
        for (size_t p = src.find("layout(location = "); p != std::string::npos; p = src.find("layout(location = ", p + 1))
        {
            const size_t in = src.find(") in float4 ", p);
            const size_t semi = in == std::string::npos ? in : src.find(';', in);
            if (in == std::string::npos || semi == std::string::npos || src.find('\n', p) < semi)
                continue;
            const int loc = atoi(src.c_str() + p + strlen("layout(location = "));
            const std::string name = src.substr(in + strlen(") in float4 "), semi - in - strlen(") in float4 "));
            const std::string decl = "layout(location = " + std::to_string(loc) + ") in float4 bo1_in_" + name +
                                     ";\nfloat4 " + name + ";";
            src.replace(p, semi + 1 - p, decl);
            char line[256];
            snprintf(line, sizeof(line), "    %s = ((bo1_misc.x >> %du) & 1u) != 0u ? bo1_in_%s.zyxw : bo1_in_%s;\n",
                     name.c_str(), loc, name.c_str(), name.c_str());
            assigns += line;
        }
        out += "#define main bo1_main\n";
        out += src;
        out += "\n#undef main\nvoid main()\n{\n";
        out += assigns;
        out += "    bo1_main();\n";
        if (!G->clipControl)
            out += "    gl_Position.y = -gl_Position.y;\n    gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;\n";
        out += "}\n";
    }
    else
        out += src;
    return out;
}

bool CompileShader(ShaderObj *s, char *err, size_t errSize)
{
    if (s->id)
        return true;
    if (s->failed)
        return false;
    const std::string src = AdaptSource(s);
    s->id = glCreateShader(s->pixel ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
    const char *p = src.c_str();
    glShaderSource(s->id, 1, &p, nullptr);
    glCompileShader(s->id);
    GLint ok = 0;
    glGetShaderiv(s->id, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        GLint len = 0;
        glGetShaderiv(s->id, GL_INFO_LOG_LENGTH, &len);
        std::string log((size_t)std::max(len, 1), '\0');
        glGetShaderInfoLog(s->id, (GLsizei)log.size(), nullptr, &log[0]);
        SetErr(err, errSize, log.c_str());
        if (getenv("BO1_GL_DUMP_FAILED"))
            fprintf(stderr, "[webgl] failed shader:\n%s\n", src.c_str());
        glDeleteShader(s->id);
        s->id = 0;
        s->failed = true;
        return false;
    }
    std::string().swap(s->source);
    return true;
}

GLuint LinkProgram(GLuint vs, GLuint fs, char *err, size_t errSize)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    glDetachShader(p, vs);
    glDetachShader(p, fs);
    if (!ok)
    {
        GLint len = 0;
        glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
        std::string log((size_t)std::max(len, 1), '\0');
        glGetProgramInfoLog(p, (GLsizei)log.size(), nullptr, &log[0]);
        SetErr(err, errSize, log.c_str());
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

// After linking: the bindings the GLSL 4.50 source had in layout qualifiers, by name (uniform blocks bo1_vs_uniforms,
// bo1_ps_uniforms, bo1_vsfix_block, bo1_misc_block; samplers ps_sN at unit N, vs_sN at 16 + N), and what the program
// samples and reads.
void BindProgramInterface(Pipeline *pl)
{
    const GLuint p = pl->program;
    for (uint32_t b = 0; b < UBO_COUNT; ++b)
    {
        const GLuint index = glGetUniformBlockIndex(p, kBlockNames[b]);
        if (index == GL_INVALID_INDEX)
            continue;
        glUniformBlockBinding(p, index, b);
        GLint size = 0;
        glGetActiveUniformBlockiv(p, index, GL_UNIFORM_BLOCK_DATA_SIZE, &size);
        pl->blockSize[b] = std::max(size, 16);
    }
    UseProgram(p);
    GLint n = 0;
    glGetProgramiv(p, GL_ACTIVE_UNIFORMS, &n);
    uint32_t mask = 0;
    for (GLint i = 0; i < n; ++i)
    {
        char name[256];
        GLint size = 0;
        GLenum type = 0;
        glGetActiveUniform(p, (GLuint)i, sizeof(name), nullptr, &size, &type, name);
        if (type != GL_SAMPLER_2D && type != GL_SAMPLER_2D_SHADOW && type != GL_SAMPLER_CUBE && type != GL_SAMPLER_3D)
            continue;
        int unit = -1;
        if (!strncmp(name, "ps_s", 4))
            unit = atoi(name + 4);
        else if (!strncmp(name, "vs_s", 4))
            unit = (int)kVertexTextureUnitBase + atoi(name + 4);
        const GLint loc = glGetUniformLocation(p, name);
        if (unit < 0 || unit >= 31 || loc < 0)
            continue;
        glUniform1i(loc, unit);
        mask |= 1u << unit;
    }
    pl->unitMask = mask;
}

bool LinkPipeline(Pipeline *p, char *err, size_t errSize)
{
    if (p->linked)
        return true;
    if (p->failed)
        return false;
    char e2[2048] = "";
    if (!p->vs || !p->fs)
    {
        SetErr(err, errSize, "a pipeline needs a vertex and a pixel shader");
        p->failed = true;
        return false;
    }
    if (!CompileShader(p->vs.get(), e2, sizeof(e2)) || !CompileShader(p->fs.get(), e2, sizeof(e2)))
    {
        SetErr(err, errSize, e2[0] ? e2 : "a shader of the pipeline did not compile");
        p->failed = true;
        return false;
    }
    p->program = LinkProgram(p->vs->id, p->fs->id, err, errSize);
    if (!p->program)
    {
        p->failed = true;
        return false;
    }
    BindProgramInterface(p);
    p->vs.reset();
    p->fs.reset();
    p->linked = true;
    return true;
}

// ---- the renderer's own programs ----
// a full-viewport triangle; rect: source uv at the viewport's top-left (xy) and bottom-right (zw); flip.x: 1 into a
// render target (row 0 at the top), -1 into the canvas (row 0 at the bottom)
const char kUtilVS[] = "uniform vec4 rect;\n"
                       "uniform vec4 flip;\n"
                       "out vec2 uv;\n"
                       "void main()\n"
                       "{\n"
                       "    vec2 t = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
                       "    gl_Position = vec4(t.x * 2.0 - 1.0, (1.0 - t.y * 2.0) * flip.x, 0.0, 1.0);\n"
                       "    uv = mix(rect.xy, rect.zw, t);\n"
                       "    BO1_CLIP\n"
                       "}\n";
const char kBlitFS[] = "uniform sampler2D src;\n"
                       "in vec2 uv;\n"
                       "layout(location = 0) out vec4 o;\n"
                       "void main() { o = texture(src, uv); }\n";
// back buffer -> canvas through the gamma ramp (256 entries per channel, interpolated)
const char kPresentFS[] = "uniform sampler2D src;\n"
                          "uniform sampler2D lut;\n"
                          "in vec2 uv;\n"
                          "layout(location = 0) out vec4 o;\n"
                          "void main()\n"
                          "{\n"
                          "    vec3 c = clamp(texture(src, uv).rgb, 0.0, 1.0);\n"
                          "    vec3 lc = (c * 255.0 + 0.5) / 256.0;\n"
                          "    o = vec4(texture(lut, vec2(lc.r, 0.5)).r, texture(lut, vec2(lc.g, 0.5)).g,\n"
                          "             texture(lut, vec2(lc.b, 0.5)).b, 1.0);\n"
                          "}\n";
// the self-test: a triangle at the given clip-space positions, writing (1, gl_FragCoord.y / 4, 0, 1)
const char kTestVS[] = "uniform vec4 pts[3];\n"
                       "void main() { gl_Position = vec4(pts[gl_VertexID].xy, 0.0, 1.0); BO1_CLIP }\n";
const char kTestFS[] = "layout(location = 0) out vec4 o;\n"
                       "void main() { o = vec4(1.0, gl_FragCoord.y / 4.0, 0.0, 1.0); }\n";

UtilProgram BuildProgram(const char *vsBody, const char *fsBody, const char *name)
{
    UtilProgram u;
    const std::string head = "#version 300 es\nprecision highp float;\nprecision highp int;\n";
    std::string vsSrc = head + vsBody;
    ReplaceAll(vsSrc, "BO1_CLIP",
               G->clipControl ? "" : "gl_Position.y = -gl_Position.y; gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;");
    const std::string fsSrc = head + fsBody;
    auto compile = [&](GLenum type, const std::string &src) -> GLuint {
        GLuint s = glCreateShader(type);
        const char *p = src.c_str();
        glShaderSource(s, 1, &p, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            char log[2048] = "";
            glGetShaderInfoLog(s, sizeof(log), nullptr, log);
            fprintf(stderr, "[webgl] %s shader: %s\n", name, log);
            glDeleteShader(s);
            return 0;
        }
        return s;
    };
    GLuint vs = compile(GL_VERTEX_SHADER, vsSrc), fs = compile(GL_FRAGMENT_SHADER, fsSrc);
    if (vs && fs)
    {
        char err[2048] = "";
        u.program = LinkProgram(vs, fs, err, sizeof(err));
        if (!u.program)
            fprintf(stderr, "[webgl] %s program: %s\n", name, err);
    }
    if (vs)
        glDeleteShader(vs);
    if (fs)
        glDeleteShader(fs);
    if (u.program)
    {
        UseProgram(u.program);
        u.rect = glGetUniformLocation(u.program, "rect");
        u.flip = glGetUniformLocation(u.program, "flip");
        u.pts = glGetUniformLocation(u.program, "pts");
        const GLint src = glGetUniformLocation(u.program, "src");
        const GLint lut = glGetUniformLocation(u.program, "lut");
        if (src >= 0)
            glUniform1i(src, 0);
        if (lut >= 0)
            glUniform1i(lut, 1);
    }
    return u;
}

// ======================================================================================================================
// utility draws
// ======================================================================================================================
void UtilityState(const UtilProgram &u, GLuint fbo)
{
    BindDrawFbo(fbo);
    UseProgram(u.program);
    BindVao(G->emptyVao);
    SetCap(CAP_CULL, false);
    SetCap(CAP_DEPTH, false);
    SetCap(CAP_STENCIL, false);
    SetCap(CAP_SCISSOR, false);
    SetCap(CAP_OFFSET_FILL, false);
    SetCap(CAP_A2C, false);
    SetCap(CAP_BLEND, false);
    SetColorMask(15);
}

void DrawFullViewport(const UtilProgram &u, const float rect[4], float flipY)
{
    glUniform4fv(u.rect, 1, rect);
    const float flip[4] = {flipY, 0, 0, 0};
    glUniform4fv(u.flip, 1, flip);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

// ======================================================================================================================
// the self-test: how this WebGL implementation orients what is drawn
// ======================================================================================================================
void SelfTest()
{
    UtilProgram prog = BuildProgram(kTestVS, kTestFS, "self-test");
    if (!prog.program)
        return;
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    BindTextureUnit(kScratchUnit, GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 4, 4);
    glGenFramebuffers(1, &fbo);
    BindDrawFbo(fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    BindReadFbo(fbo);
    UtilityState(prog, fbo);
    SetViewportGL(0, 0, 4, 4, 0, 1);
    const float black[4] = {0, 0, 0, 0};
    uint8_t px[4 * 4 * 4];
    auto run = [&](const float pts[12]) {
        glClearBufferfv(GL_COLOR, 0, black);
        glUniform4fv(prog.pts, 3, pts);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, px);
    };
    // 1. the upper half of clip space (y > 0) must land in rows 0-1 (the top of a Direct3D render target), and
    //    gl_FragCoord.y must count rows from row 0
    const float upper[12] = {-1, 0, 0, 1, 3, 0, 0, 1, -1, 4, 0, 1};
    run(upper);
    const bool top = px[0] == 255 && px[3 * 16] == 0;
    const int fragY = px[1];
    if (!top)
        fprintf(stderr, "[webgl] warning: +y does not land at row 0 (row 0 %u, row 3 %u): the picture will be upside "
                        "down\n",
                px[0], px[3 * 16]);
    // 2. which GL winding a triangle has that is clockwise as seen (clip space, y up: top-left, right, bottom-left)
    SetCap(CAP_CULL, true);
    SetCullFace(GL_BACK);
    SetFrontFace(GL_CCW);
    const float cw[12] = {-1, 3, 0, 1, 3, -1, 0, 1, -1, -1, 0, 1};
    run(cw);
    const bool drawn = px[0] == 255;
    G->frontForCW = drawn ? GL_CCW : GL_CW;
    SetCap(CAP_CULL, false);
    fprintf(stderr, "[webgl] self-test: +y at row 0 %s, gl_FragCoord.y at row 0 %d/255, clockwise is GL %s (%s)\n",
            top ? "yes" : "no", fragY, drawn ? "CCW" : "CW", G->clipControl ? "EXT_clip_control" : "in the shaders");
    BindDrawFbo(0);
    BindReadFbo(0);
    UseProgram(0);
    glDeleteFramebuffers(1, &fbo);
    ForgetTextureName(tex);
    glDeleteTextures(1, &tex);
    glDeleteProgram(prog.program);
}

bool BuildUtilities(char *err, size_t errSize)
{
    glGenVertexArrays(1, &G->mainVao);
    glGenVertexArrays(1, &G->emptyVao);
    BindVao(G->mainVao);

    G->blit = BuildProgram(kUtilVS, kBlitFS, "blit");
    G->present = BuildProgram(kUtilVS, kPresentFS, "present");
    if (!G->blit.program || !G->present.program)
    {
        SetErr(err, errSize, "the WebGL renderer's own shaders did not compile");
        return false;
    }

    glGenSamplers(1, &G->linearClamp);
    glSamplerParameteri(G->linearClamp, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(G->linearClamp, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(G->linearClamp, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(G->linearClamp, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenSamplers(1, &G->pointClamp);
    glSamplerParameteri(G->pointClamp, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(G->pointClamp, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glSamplerParameteri(G->pointClamp, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(G->pointClamp, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    G->zeroUniforms = new Buffer();
    G->zeroUniforms->length = 16384;
    G->zeroUniforms->cpu = (uint8_t *)calloc(1, G->zeroUniforms->length);
    G->ubo[UBO_VSFIX] = {G->zeroUniforms, 0, 16};

    // the gamma ramp: half floats (16-bit normalized textures are an extension in WebGL)
    glGenTextures(1, &G->gammaLut);
    BindTextureUnit(kScratchUnit, GL_TEXTURE_2D, G->gammaLut);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, 256, 1);
    uint16_t identity[256 * 4];
    for (int i = 0; i < 256; ++i)
    {
        const uint16_t v = (uint16_t)(i * 257);
        identity[i * 4 + 0] = identity[i * 4 + 1] = identity[i * 4 + 2] = FloatToHalf(i / 255.0f);
        identity[i * 4 + 3] = FloatToHalf(1.0f);
        G->gammaCache[i] = G->gammaCache[256 + i] = G->gammaCache[512 + i] = v;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_HALF_FLOAT, identity);

    G->defaultDS.d.depthFunc = 7; // always, no writes
    return true;
}

bool EnvFlag(const char *name)
{
    const char *e = getenv(name);
    return e && *e && strcmp(e, "0") != 0;
}
} // namespace

// ======================================================================================================================
// device
// ======================================================================================================================
bool Init(void *layer, char *err, size_t errSize)
{
    if (G)
        return true;
    G = new Globals();
    G->surface = (WebSurface *)layer;
    G->context = bo1_web_gl_create();
    if (G->context <= 0)
    {
        SetErr(err, errSize, "this browser has no WebGL 2 for the game's thread (OffscreenCanvas)");
        delete G;
        G = nullptr;
        return false;
    }
    t_render = true;
    G->renderThread = pthread_self();

    // capabilities
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version = (const char *)glGetString(GL_VERSION);
    snprintf(G->info.name, sizeof(G->info.name), "%s", renderer ? renderer : "WebGL 2");
    GLint maxTex = 4096;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
    G->info.maxTextureSize = (uint32_t)maxTex;
    G->info.supportsDepth24 = false;
    G->info.supportsBC = true; // (decoded on the CPU where the browser cannot sample them: BuildFormats)
    const bool s3tc = bo1_web_gl_has("WEBGL_compressed_texture_s3tc") != 0;
    const bool s3tcSrgb = bo1_web_gl_has("WEBGL_compressed_texture_s3tc_srgb") != 0;
    const bool rgtc = bo1_web_gl_has("EXT_texture_compression_rgtc") != 0;
    G->norm16 = bo1_web_gl_has("EXT_texture_norm16") != 0;
    G->colorBufferFloat = bo1_web_gl_has("EXT_color_buffer_float") != 0;
    G->floatLinear = bo1_web_gl_has("OES_texture_float_linear") != 0;
    G->drawBuffersIndexed = bo1_web_gl_has("OES_draw_buffers_indexed") != 0;
    BuildFormats(s3tc, s3tcSrgb, rgtc, G->norm16);
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &G->uboAlign);
    G->uboAlign = std::max(G->uboAlign, 16);
    if (bo1_web_gl_has("EXT_texture_filter_anisotropic"))
        glGetFloatv(BO1_GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &G->maxAnisotropy);
    G->maxAnisotropy = std::max(G->maxAnisotropy, 1.0f);
    G->clipControl = !EnvFlag("BO1_WEBGL_NO_CLIP_CONTROL") && bo1_web_gl_clip_control() != 0;
    fprintf(stderr, "[webgl] %s, %s; S3TC %s, RGTC %s, 16-bit normalized %s, float render targets %s\n",
            renderer ? renderer : "?", version ? version : "?", s3tc ? "yes" : "decoded on the CPU",
            rgtc ? "yes" : "decoded on the CPU", G->norm16 ? "yes" : "as half floats", G->colorBufferFloat ? "yes" : "no");

    // fixed state
    SetCap(CAP_DEPTH, true);
    if (!BuildUtilities(err, errSize))
    {
        Shutdown();
        return false;
    }
    SelfTest();
    return true;
}

void Shutdown()
{
    if (!G)
        return;
    if (t_render)
        CommitCurrent();
    for (uint8_t *p : G->slot)
        free(p); // (the page stops drawing them: the game is quitting)
    delete G;
    G = nullptr;
    t_render = false;
}

void GetDeviceInfo(DeviceInfo *info)
{
    memset(info, 0, sizeof(*info));
    if (G)
        *info = G->info;
}

bool SupportsSampleCount(uint32_t count) { return G && count == 1; }

bool SupportsFormat(uint32_t pixelFormat, uint32_t usage)
{
    if (!G)
        return false;
    const GlFormat *f = FindGlFormat(pixelFormat);
    if (!f)
        return false;
    if (f->compressed)
        return !(usage & USAGE_RENDER_TARGET);
    if (usage & USAGE_RENDER_TARGET)
    {
        switch (pixelFormat)
        {
        case PF_RGBA8_SNORM:
        case PF_R8_SNORM:
        case PF_RG8_SNORM:
        case PF_RG16_SNORM:
        case PF_RGBA16_SNORM: return false;
        case PF_R16F:
        case PF_RG16F:
        case PF_RGBA16F:
        case PF_R32F:
        case PF_RG32F:
        case PF_RGBA32F: return G->colorBufferFloat;
        case PF_R16:
        case PF_RG16:
        case PF_RGBA16: return G->norm16 || G->colorBufferFloat;
        default: break;
        }
    }
    return true;
}

void SetLayer(void *layer)
{
    if (G && layer)
        G->surface = (WebSurface *)layer;
}

void SetDrawableSize(uint32_t, uint32_t) {} // (the canvas is the page's size: Present scales into it)

void SetDisplaySync(bool vsync)
{
    if (G)
        G->vsync = vsync;
}

// ======================================================================================================================
// textures
// ======================================================================================================================
Texture *CreateTexture(const TextureDesc &desc)
{
    if (!G)
        return nullptr;
    const GlFormat *f = FindGlFormat(desc.format);
    if (!f)
    {
        char key[32];
        snprintf(key, sizeof(key), "fmt%u", desc.format);
        LogOnce(key, "texture format %u has no WebGL equivalent", desc.format);
        return nullptr;
    }
    if (desc.sampleCount > 1)
        LogOnce("msaa", "multisampled textures are made single-sampled (WebGL renders without multisampling here)");
    Texture *t = new Texture();
    t->format = desc.format;
    t->gf = f;
    t->type = desc.type == TEX_2D_MS ? TEX_2D : desc.type;
    t->width = std::max<uint32_t>(desc.width, 1);
    t->height = desc.type == TEX_CUBE ? t->width : std::max<uint32_t>(desc.height, 1);
    t->depth = desc.type == TEX_3D ? std::max<uint32_t>(desc.depth, 1) : 1;
    t->levels = std::max<uint32_t>(desc.mipLevels, 1);
    t->samples = 1;
    t->swizzle = SwizzleCodeFor(desc.format, nullptr);
    switch (t->type)
    {
    case TEX_CUBE: t->target = GL_TEXTURE_CUBE_MAP; break;
    case TEX_3D: t->target = GL_TEXTURE_3D; break;
    default: t->target = GL_TEXTURE_2D; break;
    }
    // (the GL texture is made by the render thread when it first needs it: EnsureTexture)
    return t;
}

Texture *CreateTextureView(Texture *base, uint32_t format, const uint8_t *swizzle)
{
    if (!base || !G)
        return nullptr;
    base = Storage(base);
    const GlFormat *f = FindGlFormat(format);
    if (!f)
        return nullptr;
    // (no texture views in WebGL: the same texture, sampled with the view's swizzle; another format is sampled as the
    // base's)
    Texture *t = new Texture(*base);
    t->views.clear();
    t->id = 0;
    t->base = base;
    t->format = base->format;
    t->swizzle = SwizzleCodeFor(base->format, swizzle);
    if (format != base->format)
    {
        char key[48];
        snprintf(key, sizeof(key), "view%u_%u", base->format, format);
        LogOnce(key, "a view of format %u as format %u is sampled as its own format", base->format, format);
    }
    base->views.push_back(t);
    return t;
}

void ReleaseTexture(Texture *tex)
{
    if (!tex || !G)
        return;
    Defer([tex] {
        for (int i = 0; i < 4; ++i)
            if (G->rt.color[i] == tex)
            {
                G->rt.color[i] = nullptr;
                G->rtDirty = true;
            }
        if (G->rt.depth == tex)
        {
            G->rt.depth = nullptr;
            G->rtDirty = true;
        }
        for (Texture *&t : G->vtex)
            if (t == tex)
                t = nullptr;
        for (Texture *&t : G->ftex)
            if (t == tex)
                t = nullptr;
        if (tex->base)
        {
            auto &v = tex->base->views;
            v.erase(std::remove(v.begin(), v.end(), tex), v.end());
            delete tex;
            return;
        }
        // (views of it that are still there now share nothing)
        for (Texture *v : tex->views)
            v->base = nullptr;
        ForgetFbosOf(tex);
        if (tex->id)
        {
            ForgetTextureName(tex->id);
            glDeleteTextures(1, &tex->id);
        }
        delete tex;
    });
}

void GetTextureSize(Texture *tex, uint32_t *w, uint32_t *h, uint32_t *d)
{
    if (w)
        *w = tex ? tex->width : 0;
    if (h)
        *h = tex ? tex->height : 0;
    if (d)
        *d = tex ? tex->depth : 0;
}

uint32_t TextureFormat(Texture *tex) { return tex ? tex->format : 0; }

void UploadTexture(Texture *tex, uint32_t slice, uint32_t level, uint32_t x, uint32_t y, uint32_t z, uint32_t w,
                   uint32_t h, uint32_t d, const void *data, uint32_t rowPitch, uint32_t slicePitch, bool)
{
    if (!tex || !data || !w || !h || !G)
        return;
    d = std::max<uint32_t>(d, 1);
    if (OnRenderThread())
    {
        RunDeferred();
        EnsureCmdBuf();
        Upload(tex, slice, level, x, y, z, w, h, d, data, rowPitch, slicePitch);
        return;
    }
    // (another thread: the texels are copied and uploaded by the render thread at its next call, before any draw)
    const GlFormat *f = Storage(tex)->gf;
    const size_t imageBytes = (size_t)rowPitch * RowCount(f, h);
    const size_t stride = Storage(tex)->type == TEX_3D ? (slicePitch ? slicePitch : imageBytes) : imageBytes;
    auto copy = std::make_shared<std::vector<uint8_t>>((const uint8_t *)data, (const uint8_t *)data + stride * (d - 1) + imageBytes);
    Defer([=] { Upload(tex, slice, level, x, y, z, w, h, d, copy->data(), rowPitch, (uint32_t)stride); });
}

namespace
{
// reads w x h texels of a color texture's level into dst (the Direct3D layer's layout)
void Download(Texture *t, uint32_t slice, uint32_t level, uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h,
              void *dst, uint32_t rowPitch)
{
    t = Storage(t);
    const GlFormat *f = t->gf;
    const uint32_t tight = RowBytes(f, w);
    if (!f->readKind || !EnsureTexture(t))
    {
        ZeroRows(dst, h, rowPitch, tight);
        return;
    }
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    BindReadFbo(fbo);
    if (t->type == TEX_3D)
        glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, t->id, (GLint)level, (GLint)z);
    else
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               t->type == TEX_CUBE ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + slice : GL_TEXTURE_2D, t->id,
                               (GLint)level);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    const size_t n = (size_t)w * h;
    if (f->readKind == 1 || f->readKind == 3)
    {
        std::vector<uint32_t> px(n);
        glReadPixels((GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, GL_RGBA,
                     f->readKind == 1 ? GL_UNSIGNED_BYTE : GL_UNSIGNED_INT_2_10_10_10_REV, px.data());
        for (uint32_t r = 0; r < h; ++r)
        {
            uint8_t *out = (uint8_t *)dst + (size_t)r * rowPitch;
            for (uint32_t c = 0; c < w; ++c)
            {
                uint32_t v = px[(size_t)r * w + c];
                const uint8_t *b = (const uint8_t *)&v;
                switch (f->pf)
                {
                case PF_A8:
                case PF_R8: out[c] = b[0]; break;
                case PF_RG8:
                    out[c * 2] = b[0];
                    out[c * 2 + 1] = b[1];
                    break;
                case PF_B5G6R5:
                {
                    const uint16_t s = (uint16_t)(((b[0] >> 3) << 11) | ((b[1] >> 2) << 5) | (b[2] >> 3));
                    memcpy(out + c * 2, &s, 2);
                    break;
                }
                case PF_BGRA8:
                case PF_BGRA8_SRGB:
                    out[c * 4] = b[2];
                    out[c * 4 + 1] = b[1];
                    out[c * 4 + 2] = b[0];
                    out[c * 4 + 3] = b[3];
                    break;
                case PF_BGR10A2:
                    v = (v & 0xC00FFC00u) | ((v & 0x3FFu) << 20) | ((v >> 20) & 0x3FFu);
                    memcpy(out + c * 4, &v, 4);
                    break;
                default: memcpy(out + c * 4, &v, 4); break;
                }
            }
        }
    }
    else
    {
        std::vector<float> px(n * 4);
        glReadPixels((GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_FLOAT, px.data());
        const int comps = f->format == GL_RED ? 1 : f->format == GL_RG ? 2 : 4;
        for (uint32_t r = 0; r < h; ++r)
        {
            uint8_t *out = (uint8_t *)dst + (size_t)r * rowPitch;
            for (uint32_t c = 0; c < w; ++c)
                for (int k = 0; k < comps; ++k)
                {
                    const float v = px[((size_t)r * w + c) * 4 + k];
                    if (f->type == GL_FLOAT)
                        memcpy(out + (c * comps + k) * 4, &v, 4);
                    else if (f->conv == CV_U16_HALF || f->type == GL_UNSIGNED_SHORT)
                    {
                        const uint16_t u = (uint16_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 65535.0f);
                        memcpy(out + (c * comps + k) * 2, &u, 2);
                    }
                    else
                    {
                        const uint16_t hv = FloatToHalf(v);
                        memcpy(out + (c * comps + k) * 2, &hv, 2);
                    }
                }
        }
    }
    BindReadFbo(0);
    glDeleteFramebuffers(1, &fbo);
}
} // namespace

void ReadTexture(Texture *tex, uint32_t slice, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                 void *dst, uint32_t rowPitch)
{
    if (!tex || !dst || !w || !h)
        return;
    const uint32_t rows = RowCount(Storage(tex)->gf, h);
    const uint32_t tight = RowBytes(Storage(tex)->gf, w);
    if (IsDepthFormat(tex->format) || !RenderContext("ReadTexture"))
    {
        ZeroRows(dst, rows, rowPitch, tight);
        return;
    }
    Download(tex, slice, level, x, y, 0, w, h, dst, rowPitch);
}

void ReadTexture3D(Texture *tex, uint32_t level, uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h, uint32_t d,
                   void *dst, uint32_t rowPitch, uint32_t slicePitch)
{
    if (!tex || !dst || !w || !h || !d || !RenderContext("ReadTexture3D"))
        return;
    for (uint32_t k = 0; k < d; ++k)
        Download(tex, 0, level, x, y, z + k, w, h, (uint8_t *)dst + (size_t)k * slicePitch, rowPitch);
}

void GenerateMipmaps(Texture *tex)
{
    if (!tex || !G)
        return;
    Defer([tex] {
        Texture *t = Storage(tex);
        if (!t || t->levels < 2 || IsCompressed(t->format) || IsDepthFormat(t->format))
            return;
        EnsureCmdBuf();
        BindTextureUnit(kScratchUnit, t->target, EnsureTexture(t));
        glGenerateMipmap(t->target);
    });
}

namespace
{
// a rectangle of one texture's level into another's through framebuffers (scaled when the sizes differ)
void BlitRect(Texture *src, uint32_t srcSlice, uint32_t srcLevel, int sx0, int sy0, int sx1, int sy1, Texture *dst,
              uint32_t dstSlice, uint32_t dstLevel, int dx0, int dy0, int dx1, int dy1, bool linear)
{
    src = Storage(src);
    dst = Storage(dst);
    if (!src || !dst || IsCompressed(src->format) || IsCompressed(dst->format))
    {
        LogOnce("blitcompressed", "a copy between compressed textures is skipped");
        return;
    }
    EnsureCmdBuf();
    const bool depth = IsDepthFormat(src->format);
    if (depth != IsDepthFormat(dst->format))
        return;
    GLuint readFbo = 0;
    glGenFramebuffers(1, &readFbo);
    BindReadFbo(readFbo);
    const GLenum attach = depth ? (HasStencil(src->format) ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT)
                                : GL_COLOR_ATTACHMENT0;
    AttachTexture(GL_READ_FRAMEBUFFER, attach, src, srcLevel, srcSlice);
    if (!depth)
        glReadBuffer(GL_COLOR_ATTACHMENT0);
    const GLuint drawFbo = FboForTexture(dst, dstLevel, dstSlice);
    BindDrawFbo(drawFbo);
    OpenMasks();
    GLbitfield mask = GL_COLOR_BUFFER_BIT;
    if (depth)
        mask = GL_DEPTH_BUFFER_BIT | (HasStencil(src->format) && HasStencil(dst->format) ? GL_STENCIL_BUFFER_BIT : 0);
    glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, linear && !depth ? GL_LINEAR : GL_NEAREST);
    BindReadFbo(0);
    glDeleteFramebuffers(1, &readFbo);
}
} // namespace

void CopyTexture(Texture *src, uint32_t srcSlice, uint32_t srcLevel, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h,
                 Texture *dst, uint32_t dstSlice, uint32_t dstLevel, uint32_t dx, uint32_t dy)
{
    if (!src || !dst || !w || !h || !G)
        return;
    Defer([=] {
        BlitRect(src, srcSlice, srcLevel, (int)sx, (int)sy, (int)(sx + w), (int)(sy + h), dst, dstSlice, dstLevel, (int)dx,
                 (int)dy, (int)(dx + w), (int)(dy + h), false);
    });
}

void ResolveTexture(Texture *src, Texture *dst, uint32_t dstSlice, uint32_t dstLevel)
{
    if (!src || !dst || !G)
        return;
    // (nothing is multisampled here: a copy)
    const uint32_t w = std::min(src->width, std::max<uint32_t>(dst->width >> dstLevel, 1));
    const uint32_t h = std::min(src->height, std::max<uint32_t>(dst->height >> dstLevel, 1));
    CopyTexture(src, 0, 0, 0, 0, w, h, dst, dstSlice, dstLevel, 0, 0);
}

void BlitTexture(Texture *src, uint32_t srcSlice, uint32_t srcLevel, int sx0, int sy0, int sx1, int sy1, Texture *dst,
                 uint32_t dstSlice, uint32_t dstLevel, int dx0, int dy0, int dx1, int dy1, bool linear)
{
    if (!src || !dst || sx1 <= sx0 || sy1 <= sy0 || dx1 <= dx0 || dy1 <= dy0 || !G)
        return;
    if (IsDepthFormat(dst->format) || IsDepthFormat(src->format))
    {
        // depth copies: same size only (Direct3D 9 allows nothing else)
        if (src->format != dst->format || sx1 - sx0 != dx1 - dx0 || sy1 - sy0 != dy1 - dy0)
            return;
    }
    Defer([=] { BlitRect(src, srcSlice, srcLevel, sx0, sy0, sx1, sy1, dst, dstSlice, dstLevel, dx0, dy0, dx1, dy1, linear); });
}

void FillTexture(Texture *dst, uint32_t slice, uint32_t level, int x0, int y0, int x1, int y1, const float color[4])
{
    if (!dst || IsDepthFormat(dst->format) || IsCompressed(dst->format) || !G)
        return;
    if (dst->format == PF_RGBA8_SNORM || dst->format == PF_R8_SNORM || dst->format == PF_RG8_SNORM)
        return; // (not renderable in WebGL)
    const int lw = (int)std::max<uint32_t>(dst->width >> level, 1);
    const int lh = (int)std::max<uint32_t>(dst->height >> level, 1);
    x0 = std::clamp(x0, 0, lw);
    y0 = std::clamp(y0, 0, lh);
    x1 = std::clamp(x1, x0, lw);
    y1 = std::clamp(y1, y0, lh);
    if (x1 <= x0 || y1 <= y0)
        return;
    const float c[4] = {color[0], color[1], color[2], color[3]};
    Defer([=] {
        Texture *t = Storage(dst);
        if (!t)
            return;
        if ((t->gf->type == GL_FLOAT || t->gf->type == GL_HALF_FLOAT) && !G->colorBufferFloat)
            return;
        EnsureCmdBuf();
        BindDrawFbo(FboForTexture(t, level, slice));
        OpenMasks();
        SetCap(CAP_SCISSOR, true);
        SetScissorRect(x0, y0, x1 - x0, y1 - y0);
        glClearBufferfv(GL_COLOR, 0, c);
    });
}

// ======================================================================================================================
// buffers
// ======================================================================================================================
Buffer *CreateBufferNoCopy(void *ptr, size_t length, void (*release)(void *ptr, size_t len, void *user), void *user)
{
    if (!G || !ptr)
        return nullptr;
    Buffer *b = new Buffer();
    b->noCopy = true;
    b->cpu = (uint8_t *)ptr;
    b->length = length;
    b->release = release;
    b->user = user;
    // (the GL copies are made from these bytes when first drawn from: GlBufferFor)
    return b;
}

Buffer *CreateBuffer(size_t length)
{
    if (!G)
        return nullptr;
    Buffer *b = new Buffer();
    b->length = std::max<size_t>(length, 16);
    b->cpu = (uint8_t *)calloc(1, b->length);
    b->owned = true;
    b->noCopy = true; // (its writes reach the GL copies the same way: BufferWritten)
    return b;
}

void *BufferContents(Buffer *buf) { return buf ? buf->cpu : nullptr; }

void BufferWritten(Buffer *buf, size_t offset, size_t length)
{
    if (!buf || !length || !G || offset >= buf->length || buf->chunk)
        return;
    const size_t end = std::min(buf->length, offset + length);
    std::lock_guard<std::mutex> lock(G->uploadLock);
    // (a gap of a few KiB is cheaper to copy along than to upload separately)
    if (!buf->dirty.empty() && offset <= buf->dirty.back().second + 4096 && end + 4096 >= buf->dirty.back().first)
    {
        auto &r = buf->dirty.back();
        r.first = std::min(r.first, offset);
        r.second = std::max(r.second, end);
    }
    else if (buf->dirty.size() >= 64)
    {
        // (many scattered writes: one range over them all)
        auto &r = buf->dirty.back();
        for (const auto &d : buf->dirty)
        {
            r.first = std::min(r.first, d.first);
            r.second = std::max(r.second, d.second);
        }
        r.first = std::min(r.first, offset);
        r.second = std::max(r.second, end);
        buf->dirty.erase(buf->dirty.begin(), buf->dirty.end() - 1);
    }
    else
        buf->dirty.emplace_back(offset, end);
    if (!buf->queued)
    {
        buf->queued = true;
        G->dirtyBuffers.push_back(buf);
    }
}

void ReleaseBuffer(Buffer *buf)
{
    if (!buf || !G)
        return;
    {
        std::lock_guard<std::mutex> lock(G->uploadLock);
        if (buf->queued)
            G->dirtyBuffers.erase(std::remove(G->dirtyBuffers.begin(), G->dirtyBuffers.end(), buf),
                                  G->dirtyBuffers.end());
        buf->queued = false;
        buf->dirty.clear();
    }
    // (the GL copies have the bytes: the caller's memory is free at once)
    if (buf->release)
        buf->release(buf->cpu, buf->length, buf->user);
    else if (buf->owned)
        free(buf->cpu);
    buf->cpu = nullptr;
    Defer([buf] {
        for (BoundBuffer &b : G->vbuf)
            if (b.buf == buf)
                b = BoundBuffer{};
        for (BoundBuffer &b : G->ubo)
            if (b.buf == buf)
                b = BoundBuffer{};
        for (GLuint id : {buf->vbo, buf->ibo})
            if (id)
            {
                ForgetBufferName(id);
                glDeleteBuffers(1, &id);
            }
        delete buf;
    });
}

// ======================================================================================================================
// shaders
// ======================================================================================================================
Library *CompileLibrary(const char *source, char *err, size_t errSize)
{
    if (!G || !source)
    {
        SetErr(err, errSize, "no renderer");
        return nullptr;
    }
    auto sh = std::make_shared<ShaderObj>();
    sh->pixel = strstr(source, "#define BO1_STAGE_PS") != nullptr;
    sh->source = source;
    // (compiled by the render thread when a pipeline with it is first drawn: LinkPipeline)
    Library *l = new Library();
    l->sh = std::move(sh);
    return l;
}

Function *GetFunction(Library *lib, const char *)
{
    if (!lib || !lib->sh)
        return nullptr;
    Function *f = new Function();
    f->sh = lib->sh;
    return f;
}

void ReleaseLibrary(Library *lib) { delete lib; }
void ReleaseFunction(Function *fn) { delete fn; }

// ======================================================================================================================
// state objects
// ======================================================================================================================
Pipeline *CreatePipeline(const PipelineDesc &desc, char *err, size_t errSize)
{
    if (!G || !desc.vertexFunction || !desc.fragmentFunction)
    {
        SetErr(err, errSize, "a pipeline needs a vertex and a pixel shader");
        return nullptr;
    }
    Pipeline *p = new Pipeline();
    p->desc = desc;
    p->desc.vertexFunction = p->desc.fragmentFunction = nullptr;
    p->vs = desc.vertexFunction->sh;
    p->fs = desc.fragmentFunction->sh;
    for (uint32_t i = 0; i < MAX_VERTEX_ATTRIBUTES; ++i)
    {
        const VertexAttributeDesc &a = desc.attributes[i];
        if (a.format == VF_INVALID || a.bufferIndex < VERTEX_BUFFER_BASE || a.bufferIndex >= VERTEX_BUFFER_BASE + 16)
            continue; // (VERTEX_BUFFER_ZERO: a disabled attribute reads (0, 0, 0, 1))
        if (!VertexFormatToGL(a.format, &p->attrFormat[i]))
        {
            char key[32];
            snprintf(key, sizeof(key), "vf%u", a.format);
            LogOnce(key, "vertex format %u has no WebGL equivalent", a.format);
            continue;
        }
        p->attrMask |= (uint16_t)(1u << i);
        if (p->attrFormat[i].bgra)
            p->bgraMask |= (uint16_t)(1u << i);
        p->bindingMask |= 1u << (a.bufferIndex - VERTEX_BUFFER_BASE);
    }
    // (linked by the render thread at its first draw: PrepareDraw)
    return p;
}

void ReleasePipeline(Pipeline *p)
{
    if (!p || !G)
        return;
    Defer([p] {
        if (G->want.pipeline == p)
            G->want.pipeline = nullptr;
        if (p->program)
        {
            if (G->have.program == p->program)
                UseProgram(0);
            glDeleteProgram(p->program);
        }
        delete p;
    });
}

DepthStencil *CreateDepthStencil(const DepthStencilDesc &desc)
{
    DepthStencil *d = new DepthStencil();
    d->d = desc;
    return d;
}

Sampler *CreateSampler(const SamplerDesc &desc)
{
    if (!G)
        return nullptr;
    Sampler *s = new Sampler();
    s->desc = desc;
    return s; // (the GL sampler is made at first use: SamplerId)
}

namespace
{
GLuint MakeSampler(const SamplerDesc &desc, bool nearest)
{
    GLuint id = 0;
    glGenSamplers(1, &id);
    const bool minLinear = desc.minFilter == 1 && !nearest;
    GLenum minFilter;
    switch (desc.mipFilter)
    {
    case 1: minFilter = minLinear ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST; break;
    case 2: minFilter = minLinear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR; break;
    default: minFilter = minLinear ? GL_LINEAR : GL_NEAREST; break;
    }
    if (nearest && desc.mipFilter)
        minFilter = GL_NEAREST_MIPMAP_NEAREST;
    glSamplerParameteri(id, GL_TEXTURE_MIN_FILTER, (GLint)minFilter);
    glSamplerParameteri(id, GL_TEXTURE_MAG_FILTER, desc.magFilter == 1 && !nearest ? GL_LINEAR : GL_NEAREST);
    const uint8_t modes[3] = {desc.addressU, desc.addressV, desc.addressW};
    const GLenum wraps[3] = {GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T, GL_TEXTURE_WRAP_R};
    for (int i = 0; i < 3; ++i)
        glSamplerParameteri(id, wraps[i], (GLint)kAddress[modes[i] < 6 ? modes[i] : 0]);
    if (desc.maxAnisotropy > 1 && G->maxAnisotropy > 1 && !nearest)
        glSamplerParameterf(id, BO1_GL_TEXTURE_MAX_ANISOTROPY_EXT, std::min<float>(desc.maxAnisotropy, G->maxAnisotropy));
    if (desc.compareFunc)
    {
        glSamplerParameteri(id, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glSamplerParameteri(id, GL_TEXTURE_COMPARE_FUNC, (GLint)Cmp(desc.compareFunc));
    }
    glSamplerParameterf(id, GL_TEXTURE_MIN_LOD, desc.lodMinClamp);
    glSamplerParameterf(id, GL_TEXTURE_MAX_LOD, desc.lodMaxClamp);
    return id;
}

// the sampler for a texture: point filtering where WebGL cannot filter the texture (a depth texture read without
// comparison; a 32-bit float one without OES_texture_float_linear)
GLuint SamplerId(Sampler *s, Texture *t)
{
    if (!s)
        return 0;
    bool nearest = false;
    if (t)
    {
        if (IsDepthFormat(t->format) && !s->desc.compareFunc)
            nearest = true;
        else if (!G->floatLinear && Storage(t)->gf && Storage(t)->gf->type == GL_FLOAT && !IsDepthFormat(t->format))
            nearest = true;
    }
    GLuint &id = nearest ? s->nearest : s->id;
    if (!id)
        id = MakeSampler(s->desc, nearest);
    return id;
}
} // namespace

// ======================================================================================================================
// passes and draws
// ======================================================================================================================
void SetRenderTargets(const RenderTargets &rt)
{
    if (!G || memcmp(&rt, &G->rt, sizeof(rt)) == 0)
        return;
    G->rt = rt;
    G->rtDirty = true;
    ComputeTargetSize();
}

void Clear(uint32_t flags, const float color[4], float depth, uint32_t stencil, int x, int y, int w, int h)
{
    if (!RenderContext("Clear"))
        return;
    for (int i = 0; i < 4; ++i)
        if (!G->rt.color[i])
            flags &= ~(CLEAR_COLOR0 << i);
    if (!G->rt.depth)
        flags &= ~(CLEAR_DEPTH | CLEAR_STENCIL);
    else if (!HasStencil(G->rt.depth->format))
        flags &= ~CLEAR_STENCIL;
    if (!flags)
        return;
    EnsureCmdBuf();
    if (!BindRenderTargets())
        return;
    OpenMasks();
    const bool whole = w <= 0 || h <= 0 || (x <= 0 && y <= 0 && x + w >= (int)G->rtWidth && y + h >= (int)G->rtHeight);
    if (!whole)
    {
        const int x0 = std::clamp(x, 0, (int)G->rtWidth);
        const int y0 = std::clamp(y, 0, (int)G->rtHeight);
        const int x1 = std::clamp(x + w, x0, (int)G->rtWidth);
        const int y1 = std::clamp(y + h, y0, (int)G->rtHeight);
        if (x1 <= x0 || y1 <= y0)
            return;
        SetCap(CAP_SCISSOR, true);
        SetScissorRect(x0, y0, x1 - x0, y1 - y0);
    }
    for (int i = 0; i < 4; ++i)
        if (flags & (CLEAR_COLOR0 << i))
            glClearBufferfv(GL_COLOR, i, color);
    const bool d = (flags & CLEAR_DEPTH) != 0, s = (flags & CLEAR_STENCIL) != 0;
    if (d && s)
        glClearBufferfi(GL_DEPTH_STENCIL, 0, depth, (GLint)stencil);
    else if (d)
        glClearBufferfv(GL_DEPTH, 0, &depth);
    else if (s)
    {
        const GLint sv = (GLint)stencil;
        glClearBufferiv(GL_STENCIL, 0, &sv);
    }
}

void SetPipeline(Pipeline *p)
{
    if (G)
        G->want.pipeline = p;
}
void SetDepthStencil(DepthStencil *ds)
{
    if (G)
        G->want.depthStencil = ds;
}
void SetStencilReference(uint32_t ref)
{
    if (G)
        G->want.stencilRef = ref;
}

void SetRasterState(uint8_t cullMode, uint8_t frontWinding, uint8_t fillMode)
{
    if (!G)
        return;
    G->want.cull = cullMode;
    G->want.winding = frontWinding;
    G->want.fill = fillMode;
}

void SetDepthBias(float depthBias, float slopeScale, float clamp)
{
    if (!G)
        return;
    G->want.depthBias = depthBias;
    G->want.slopeScale = slopeScale;
    G->want.biasClamp = clamp;
}

void SetViewport(double x, double y, double w, double h, double znear, double zfar)
{
    if (!G)
        return;
    double *v = G->want.viewport;
    v[0] = x, v[1] = y, v[2] = w, v[3] = h, v[4] = znear, v[5] = zfar;
}

void SetScissor(bool enable, int x, int y, int w, int h)
{
    if (!G)
        return;
    G->want.scissorEnable = enable;
    G->want.sx = x;
    G->want.sy = y;
    G->want.sw = w;
    G->want.sh = h;
}

void SetBlendColor(float r, float g, float b, float a)
{
    if (!G)
        return;
    G->want.blendColor[0] = r;
    G->want.blendColor[1] = g;
    G->want.blendColor[2] = b;
    G->want.blendColor[3] = a;
}

void SetVertexBuffer(uint32_t index, Buffer *buf, uint32_t offset)
{
    if (!G || index >= MAX_VERTEX_BUFFERS || index == VERTEX_BUFFER_ZERO)
        return;
    G->vbuf[index].buf = buf;
    G->vbuf[index].offset = offset;
}

namespace
{
void SetBytes(BoundBuffer &b, const void *data, size_t length, size_t alignment)
{
    Buffer *tb = nullptr;
    uint32_t off = 0;
    b.buf = nullptr;
    const size_t size = AlignUp(length, 16);
    uint8_t *p = (uint8_t *)Transient(size, alignment, &tb, &off);
    memcpy(p, data, length);
    if (size > length)
        memset(p + length, 0, size - length);
    b.buf = tb;
    b.offset = off;
    b.size = (uint32_t)size;
}
} // namespace

void SetVertexBytes(uint32_t index, const void *data, size_t length)
{
    if (!G || !data || !length || !RenderContext("SetVertexBytes"))
        return;
    if (index == 0)
        SetBytes(G->ubo[UBO_VS], data, length, (size_t)G->uboAlign);
    else if (index == VERTEX_FIXUP)
        SetBytes(G->ubo[UBO_VSFIX], data, length, (size_t)G->uboAlign);
    else if (index < MAX_VERTEX_BUFFERS && index != VERTEX_BUFFER_ZERO)
        SetBytes(G->vbuf[index], data, length, 16);
}

void SetFragmentBytes(uint32_t index, const void *data, size_t length)
{
    if (!G || index != 0 || !data || !length || !RenderContext("SetFragmentBytes"))
        return;
    SetBytes(G->ubo[UBO_PS], data, length, (size_t)G->uboAlign);
}

void SetVertexTexture(uint32_t index, Texture *tex)
{
    if (G && index < kMaxVertexTextures)
        G->vtex[index] = tex;
}

void SetFragmentTexture(uint32_t index, Texture *tex)
{
    if (G && index < kMaxFragmentTextures)
        G->ftex[index] = tex;
}

void SetVertexSampler(uint32_t index, Sampler *s)
{
    if (G && index < kMaxVertexTextures)
        G->vsamp[index] = s;
}

void SetFragmentSampler(uint32_t index, Sampler *s)
{
    if (G && index < kMaxFragmentTextures)
        G->fsamp[index] = s;
}

namespace
{
// one attribute of the main vertex array object (compared with the shadow)
void SetAttribute(uint32_t i, GLuint buffer, const VertexFormatGL &fmt, GLsizei stride, GLintptr offset, GLuint divisor)
{
    GlState::Attr &a = G->have.attr[i];
    if (a.enabled != 1)
    {
        a.enabled = 1;
        glEnableVertexAttribArray(i);
    }
    if (a.buffer != buffer || a.size != fmt.size || a.type != fmt.type || a.normalized != fmt.normalized ||
        a.stride != stride || a.offset != offset)
    {
        BindArrayBuffer(buffer);
        glVertexAttribPointer(i, fmt.size, fmt.type, fmt.normalized, stride, (const void *)offset);
        a.buffer = buffer;
        a.size = fmt.size;
        a.type = fmt.type;
        a.normalized = fmt.normalized;
        a.stride = stride;
        a.offset = offset;
    }
    if (a.divisor != divisor)
    {
        a.divisor = divisor;
        glVertexAttribDivisor(i, divisor);
    }
}

void DisableAttribute(uint32_t i)
{
    GlState::Attr &a = G->have.attr[i];
    if (a.enabled != 0)
    {
        a.enabled = 0;
        glDisableVertexAttribArray(i);
        glVertexAttrib4f(i, 0, 0, 0, 1);
    }
}

// a uniform block of at least the size the program declares (WebGL refuses the draw otherwise)
void BindBlock(uint32_t k, const BoundBuffer &u, GLint need)
{
    if (!need)
        return;
    if (!u.buf || u.size < (uint32_t)need)
    {
        // (shorter than the program's: zeros after what there is)
        Buffer *tb = nullptr;
        uint32_t off = 0;
        uint8_t *p = (uint8_t *)Transient(AlignUp((size_t)need, 16), (size_t)G->uboAlign, &tb, &off);
        memset(p, 0, (size_t)need);
        if (u.buf && u.buf->cpu)
            memcpy(p, u.buf->cpu + u.offset, std::min<size_t>(u.size, (size_t)need));
        BindUniformBuffer(k, GlBufferFor(tb, false), off, AlignUp((size_t)need, 16));
        return;
    }
    BindUniformBuffer(k, GlBufferFor(u.buf, false), u.offset, u.size);
}

bool PrepareDraw(int32_t baseVertex)
{
    if (!RenderContext("Draw"))
        return false;
    Pipeline *p = G->want.pipeline;
    if (!p)
        return false;
    if (!p->linked)
    {
        char err[2048] = "";
        if (!LinkPipeline(p, err, sizeof(err)))
        {
            LogOnce(err, "pipeline failed: %s", err);
            return false;
        }
    }
    if (ScissorEmpty(G->want))
        return false;
    EnsureCmdBuf();
    if (!BindRenderTargets())
        return false;
    FlushPendingUploads();
    const WantState &w = G->want;

    // the pipeline: program, blending, write masks
    UseProgram(p->program);
    BindVao(G->mainVao);
    {
        const BlendDesc &b0 = p->desc.blend[0];
        bool sameAll = true;
        for (int i = 1; i < 4; ++i)
            if (p->desc.colorFormats[i] && memcmp(&p->desc.blend[i], &b0, sizeof(BlendDesc)) != 0)
                sameAll = false;
        if (sameAll || !G->drawBuffersIndexed)
        {
            if (!sameAll)
                LogOnce("mrtblend", "render targets with different blending: the first one's applies to all "
                                    "(no OES_draw_buffers_indexed)");
            const bool on = b0.enable && p->desc.colorFormats[0] != 0;
            SetCap(CAP_BLEND, on);
            if (on)
                SetBlend(BlendFactorGL(b0.srcRGB), BlendFactorGL(b0.dstRGB), BlendFactorGL(b0.srcAlpha),
                         BlendFactorGL(b0.dstAlpha), BlendOpGL(b0.opRGB), BlendOpGL(b0.opAlpha));
            SetColorMask(b0.writeMask);
            G->have.indexedBlend = false;
        }
        else
        {
            for (int i = 0; i < 4; ++i)
            {
                const BlendDesc &b = p->desc.blend[i];
                bo1_web_gl_blend_indexed(i, b.enable && p->desc.colorFormats[i] != 0, (int)BlendFactorGL(b.srcRGB),
                                         (int)BlendFactorGL(b.dstRGB), (int)BlendFactorGL(b.srcAlpha),
                                         (int)BlendFactorGL(b.dstAlpha), (int)BlendOpGL(b.opRGB), (int)BlendOpGL(b.opAlpha),
                                         b.writeMask);
            }
            G->have.indexedBlend = true;
            G->have.caps[CAP_BLEND] = -1;
            G->have.colorMask = -1;
        }
    }
    SetCap(CAP_A2C, p->desc.alphaToCoverage != 0);

    // depth and stencil
    const DepthStencilDesc &ds = (w.depthStencil ? w.depthStencil : &G->defaultDS)->d;
    SetCap(CAP_DEPTH, true);
    SetDepth(Cmp(ds.depthFunc), ds.depthWrite != 0);
    SetCap(CAP_STENCIL, ds.stencilEnable != 0);
    if (ds.stencilEnable)
    {
        const StencilFaceDesc *faces[2] = {&ds.front, &ds.back};
        for (int f = 0; f < 2; ++f)
            SetStencilFace(f, Cmp(faces[f]->func), (GLint)w.stencilRef, ds.readMask, kStencilOp[faces[f]->failOp & 7],
                           kStencilOp[faces[f]->depthFailOp & 7], kStencilOp[faces[f]->passOp & 7]);
        SetStencilWriteMask(ds.writeMask);
    }

    // rasterizer (no wireframe in WebGL)
    SetCap(CAP_CULL, w.cull != 0);
    if (w.cull)
        SetCullFace(w.cull == 1 ? GL_FRONT : GL_BACK);
    const GLenum cwFront = G->frontForCW;
    SetFrontFace(w.winding == 0 ? cwFront : (cwFront == GL_CW ? GL_CCW : GL_CW));
    const bool bias = w.slopeScale != 0 || w.depthBias != 0;
    SetCap(CAP_OFFSET_FILL, bias);
    if (bias)
        SetPolygonOffset(w.slopeScale, w.depthBias);
    SetViewportGL((float)w.viewport[0], (float)w.viewport[1], (float)w.viewport[2], (float)w.viewport[3],
                  (float)w.viewport[4], (float)w.viewport[5]);
    if (w.scissorEnable)
    {
        const int x0 = std::clamp(w.sx, 0, (int)G->rtWidth);
        const int y0 = std::clamp(w.sy, 0, (int)G->rtHeight);
        const int x1 = std::clamp(w.sx + w.sw, x0, (int)G->rtWidth);
        const int y1 = std::clamp(w.sy + w.sh, y0, (int)G->rtHeight);
        SetCap(CAP_SCISSOR, true);
        SetScissorRect(x0, y0, x1 - x0, y1 - y0);
    }
    else
        SetCap(CAP_SCISSOR, false);
    SetBlendColorGL(w.blendColor);

    // vertex input: each attribute pointed at its stream (moved by the base vertex: WebGL has no base vertex)
    for (uint32_t i = 0; i < MAX_VERTEX_ATTRIBUTES; ++i)
    {
        if (!(p->attrMask & (1u << i)))
        {
            DisableAttribute(i);
            continue;
        }
        const VertexAttributeDesc &a = p->desc.attributes[i];
        const VertexLayoutDesc &l = p->desc.layouts[a.bufferIndex];
        const BoundBuffer &vb = G->vbuf[a.bufferIndex];
        if (!vb.buf || !vb.buf->cpu)
        {
            DisableAttribute(i);
            continue;
        }
        const GLsizei stride = (GLsizei)l.stride;
        GLintptr offset = (GLintptr)vb.offset + (GLintptr)a.offset;
        GLuint divisor = 0;
        if (l.stepFunction == 0)
            divisor = 0x7FFFFFFF; // (constant: one element for every vertex)
        else if (l.stepFunction == 2)
            divisor = std::max<uint32_t>(l.stepRate, 1);
        else
            offset += (GLintptr)baseVertex * stride;
        if (offset < 0)
        {
            LogOnce("negbase", "a draw with a negative base vertex before its stream's start is skipped");
            return false;
        }
        SetAttribute(i, GlBufferFor(vb.buf, false), p->attrFormat[i], stride, offset, divisor);
    }

    // uniforms
    for (uint32_t k = 0; k < UBO_MISC; ++k)
        BindBlock(k, G->ubo[k], p->blockSize[k]);

    // textures and samplers of the units the program samples, and their swizzles (bo1_misc_block)
    uint32_t misc[8] = {};
    for (uint32_t m = p->unitMask; m;)
    {
        const uint32_t unit = (uint32_t)__builtin_ctz(m);
        m &= m - 1;
        Texture *t = nullptr;
        Sampler *s = nullptr;
        if (unit < kMaxFragmentTextures)
        {
            t = G->ftex[unit];
            s = G->fsamp[unit];
        }
        else if (unit >= kVertexTextureUnitBase && unit < kVertexTextureUnitBase + kMaxVertexTextures)
        {
            t = G->vtex[unit - kVertexTextureUnitBase];
            s = G->vsamp[unit - kVertexTextureUnitBase];
        }
        Texture *st = Storage(t);
        // (WebGL refuses a draw that samples a texture it renders into: Direct3D 9 leaves the result undefined)
        if (st)
        {
            bool target = Storage(G->rt.depth) == st;
            for (int i = 0; i < 4; ++i)
                target |= Storage(G->rt.color[i]) == st;
            if (target)
            {
                LogOnce("feedback", "a draw samples a texture it renders into: the texture is left out");
                st = nullptr;
                t = nullptr;
            }
        }
        BindTextureUnit(unit, st ? st->target : GL_TEXTURE_2D, st ? EnsureTexture(st) : 0);
        BindSamplerUnit(unit, SamplerId(s, t));
        if (t && unit < 32)
            misc[unit >> 3] |= (uint32_t)t->swizzle << ((unit & 7) * 4);
    }
    misc[4] = p->bgraMask;
    if (p->blockSize[UBO_MISC])
    {
        if (memcmp(misc, G->miscCache, sizeof(misc)) != 0 || !G->miscBlock.buf ||
            G->miscBlock.buf->chunk->frame + kFramesBeforeReuse <= G->frameIndex)
        {
            memcpy(G->miscCache, misc, sizeof(misc));
            SetBytes(G->miscBlock, misc, sizeof(misc), (size_t)G->uboAlign);
        }
        BindBlock(UBO_MISC, G->miscBlock, p->blockSize[UBO_MISC]);
    }
    return true;
}
} // namespace

void Draw(PrimitiveType prim, uint32_t vertexStart, uint32_t vertexCount)
{
    if (!vertexCount || !PrepareDraw(0))
        return;
    glDrawArrays(kPrim[prim < 5 ? prim : 3], (GLint)vertexStart, (GLsizei)vertexCount);
}

void DrawIndexed(PrimitiveType prim, uint32_t indexCount, bool index32, Buffer *indexBuffer, uint32_t indexOffset,
                 int32_t baseVertex)
{
    if (!indexCount || !indexBuffer || !indexBuffer->cpu || !PrepareDraw(baseVertex))
        return;
    const GLuint ibo = GlBufferFor(indexBuffer, true);
    BindVao(G->mainVao);
    SetElementBuffer(ibo);
    glDrawElements(kPrim[prim < 5 ? prim : 3], (GLsizei)indexCount, index32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,
                   (const void *)(uintptr_t)indexOffset);
}

void *AllocTransient(size_t length, size_t alignment, Buffer **buf, uint32_t *offset)
{
    if (!G || !RenderContext("AllocTransient"))
    {
        static std::vector<uint8_t> sink;
        sink.resize(std::max(sink.size(), length));
        *buf = nullptr;
        *offset = 0;
        return sink.data();
    }
    return Transient(length, alignment, buf, offset);
}

// ======================================================================================================================
// occlusion queries: everything counts as visible (see the top of the file)
// ======================================================================================================================
Visibility *CreateVisibility() { return new Visibility(); }
void ReleaseVisibility(Visibility *v) { delete v; }
void BeginVisibility(Visibility *v)
{
    if (v)
        v->active = true;
}
void EndVisibility(Visibility *v)
{
    if (v)
        v->active = false;
}
bool GetVisibilityResult(Visibility *v, uint64_t *count, bool, bool)
{
    if (!v || v->active)
        return false;
    if (count)
        *count = 1u << 20;
    return true;
}

// ======================================================================================================================
// submission
// ======================================================================================================================
uint64_t CurrentSerial()
{
    if (!G)
        return 0;
    return G->recordingSerial ? G->recordingSerial : G->lastCommittedSerial;
}

// (WebGL takes copies of what it is given: everything submitted is done as far as the caller's memory goes)
uint64_t CompletedSerial() { return G ? G->lastCommittedSerial : 0; }

void Flush()
{
    if (!RenderContext("Flush"))
        return;
    CommitCurrent();
}

void WaitForSerial(uint64_t serial)
{
    if (!G || serial == 0 || !RenderContext("WaitForSerial"))
        return;
    if (serial == G->recordingSerial)
        CommitCurrent();
}

double LastFrameGpuTime() { return 0.0; }

void Present(Texture *backBuffer, const uint16_t *gammaRamp)
{
    if (!RenderContext("Present"))
        return;
    EnsureCmdBuf();
    if (gammaRamp && memcmp(gammaRamp, G->gammaCache, sizeof(G->gammaCache)) != 0)
    {
        memcpy(G->gammaCache, gammaRamp, sizeof(G->gammaCache));
        uint16_t texels[256 * 4];
        for (int i = 0; i < 256; ++i)
        {
            texels[i * 4 + 0] = FloatToHalf(gammaRamp[i] / 65535.0f);
            texels[i * 4 + 1] = FloatToHalf(gammaRamp[256 + i] / 65535.0f);
            texels[i * 4 + 2] = FloatToHalf(gammaRamp[512 + i] / 65535.0f);
            texels[i * 4 + 3] = FloatToHalf(1.0f);
        }
        BindTextureUnit(kScratchUnit, GL_TEXTURE_2D, G->gammaLut);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_HALF_FLOAT, texels);
    }

    bo1web::SharedState &sh = bo1web::Shared();
    Globals::Readback &cur = G->readback[G->frameIndex & 1];
    Globals::Readback &prev = G->readback[(G->frameIndex & 1) ^ 1];
    Texture *bb = Storage(backBuffer);
    if (bb && EnsureTexture(bb) && !cur.full)
    {
        // the back buffer through the gamma ramp, at its own size (the page scales it to its canvas)
        const int w = (int)bb->width, h = (int)bb->height;
        if (w != G->frameW || h != G->frameH || !G->frameTex)
        {
            if (G->frameTex)
            {
                ForgetTextureName(G->frameTex);
                glDeleteTextures(1, &G->frameTex);
            }
            if (!G->frameFbo)
                glGenFramebuffers(1, &G->frameFbo);
            glGenTextures(1, &G->frameTex);
            BindTextureUnit(kScratchUnit, GL_TEXTURE_2D, G->frameTex);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
            BindDrawFbo(G->frameFbo);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, G->frameTex, 0);
            G->frameW = w;
            G->frameH = h;
        }
        UtilityState(G->present, G->frameFbo);
        SetViewportGL(0, 0, (float)w, (float)h, 0, 1);
        BindTextureUnit(0, GL_TEXTURE_2D, bb->id);
        BindSamplerUnit(0, G->pointClamp);
        BindTextureUnit(1, GL_TEXTURE_2D, G->gammaLut);
        BindSamplerUnit(1, G->linearClamp);
        const float rect[4] = {0, 0, 1, 1};
        // (flipped: the rows read back bottom first, as the page's texture wants them)
        DrawFullViewport(G->present, rect, -1.0f);

        // into this frame's pixel buffer, without waiting for the GPU
        const size_t bytes = (size_t)w * h * 4;
        if (!cur.pbo)
            glGenBuffers(1, &cur.pbo);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, cur.pbo);
        if (cur.capacity < bytes)
        {
            glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)bytes, nullptr, GL_STREAM_READ);
            cur.capacity = bytes;
        }
        BindReadFbo(G->frameFbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        cur.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        cur.w = w;
        cur.h = h;
        // the shape to show it at: the game's resolution (its window's client size), stretched or not
        const WebSurface *s = G->surface;
        const int lw = s ? s->logicalW.load(std::memory_order_relaxed) : 0;
        const int lh = s ? s->logicalH.load(std::memory_order_relaxed) : 0;
        cur.aspectW = lw > 0 && lh > 0 ? lw : w;
        cur.aspectH = lw > 0 && lh > 0 ? lh : h;
        cur.fill = !s || s->fill.load(std::memory_order_relaxed) ? 1 : 0;
        cur.full = true;
    }
    CommitCurrent();
    glFlush();
    ++G->frameIndex;

    // the frame before this one to the page: its pixels have had the time this frame took to come back
    if (!prev.full)
        return;
    const int32_t frame = sh.framesPresented.load(std::memory_order_relaxed) + 1;
    const int slot = frame % 3;
    const size_t bytes = (size_t)prev.w * prev.h * 4;
    if (G->slotCapacity[slot] < bytes)
    {
        free(G->slot[slot]);
        G->slot[slot] = (uint8_t *)malloc(bytes);
        G->slotCapacity[slot] = G->slot[slot] ? bytes : 0;
    }
    if (G->slot[slot])
    {
        glClientWaitSync(prev.fence, 0, 0);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, prev.pbo);
        glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)bytes, G->slot[slot]);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
    glDeleteSync(prev.fence);
    prev.fence = nullptr;
    prev.full = false;
    if (!G->slot[slot])
        return;
    sh.framePixels[slot] = (int32_t)(uintptr_t)G->slot[slot];
    sh.frameWidth[slot] = prev.w;
    sh.frameHeight[slot] = prev.h;
    sh.frameAspectW = prev.aspectW;
    sh.frameAspectH = prev.aspectH;
    sh.frameFill = prev.fill;
    sh.framesPresented.store(frame, std::memory_order_release);

    // pacing: at most one frame waiting for the page (which shows one per display refresh); without v-sync the page
    // shows the latest and drops the rest
    if (G->vsync)
    {
        const double start = emscripten_get_now();
        for (;;)
        {
            const int32_t shown = sh.framesDisplayed.load(std::memory_order_acquire);
            if (frame - shown <= 1)
                break;
            const double left = 100.0 - (emscripten_get_now() - start);
            if (left <= 0)
                break; // (the page is hidden: the game keeps running, slowly)
            emscripten_futex_wait(&sh.framesDisplayed, (uint32_t)shown, left);
        }
    }
}
} // namespace rhi
