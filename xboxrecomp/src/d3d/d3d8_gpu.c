/* d3d8_gpu.c -- the macOS GPU backend (OT_GPU): SDL3's GPU API, Metal underneath.
 *
 * It stands in for the Direct3D 11 layer (d3d8_device.c and friends) under the *unchanged* PGRAPH translator
 * (nv2a/nv2a_pgraph_d3d11.c): the translator calls an IDirect3DDevice8 and a few dozen d3d8_* helpers, and this
 * file answers them. See RENDERER_PLAN.md.
 *
 * Slice 1 (this file today): clears and untextured geometry. A draw is the title's vertex program already run on
 * the CPU (ProgVertex: screen x, y, z, 1/w, two colours, fog, four texture coordinates) or a pre-transformed
 * fixed-function vertex (converted to the same layout); the pixel shader outputs the vertex colour. Textures are
 * created and filled (the translator does that); Slice 2 samples stage 0 (modulated with the vertex colour); combiner
 * shaders are Slice 3.
 *
 * Frames are RECORDED while the translator runs on the pump thread -- state in a table, vertices in an arena, one op
 * per draw or clear -- and EXECUTED at present: upload the arena, replay the ops into a 640x480 colour + depth target
 * (a clear that covers the target starts a new render pass with a clear load op), blit the target letterboxed to the
 * window. While a movie plays the guest framebuffer is shown instead.
 */
#include "d3d8_xbox.h"
#include "../nv2a/nv2a_pgraph_d3d11.h"
#include "../nv2a/nv2a_hdtex.h"
#include "../nv2a/nv2a_psh.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TITLE_W 640
#define TITLE_H 480
#define E_NOTIMPL_ ((HRESULT)0x80004001)

/* ================================================================= */
/* device and swapchain, attached by the port (main thread)           */
/* ================================================================= */

extern void d3d8_host_target_size(unsigned *w, unsigned *h);
extern int d3d8_PresentBoxOn(void);
extern double d3d8_PresentBoxShape(void);
static uint64_t g_t_encode, g_t_acquire, g_t_submit;   /* where the backend's time goes (reported every 300 presents) */
static unsigned g_tw = TITLE_W, g_th = TITLE_H;     /* the scene target's size (d3d8_host_target_size) */
static SDL_GPUDevice *g_dev;
static SDL_Window    *g_win;
static SDL_GPUTexture *g_rt, *g_ds, *g_movie;
static SDL_GPUTextureFormat g_rt_fmt = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
static SDL_GPUTextureFormat g_ds_fmt = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
static SDL_GPUShader *g_vs, *g_fs;
typedef struct Tex Tex;
static int g_failed;
static int g_rt_cleared;      /* has the target been cleared at least once (first frame starts black) */

extern int nv2a_psh_msl;

int d3d8_gpu_attach(void *device, void *window)
{
    nv2a_psh_msl = 1;               /* the translator's combiner generator emits MSL for this backend */
    g_dev = (SDL_GPUDevice *)device;
    g_win = (SDL_Window *)window;
    return g_dev && g_win;
}

/* ================================================================= */
/* shaders (Metal Shading Language, compiled at run time)             */
/* ================================================================= */

static const char g_msl[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VSIn {\n"
    "  float4 pos [[attribute(0)]]; float4 d0 [[attribute(1)]]; float4 d1 [[attribute(2)]]; float fog [[attribute(3)]];\n"
    "  float4 t0 [[attribute(4)]]; float4 t1 [[attribute(5)]]; float4 t2 [[attribute(6)]]; float4 t3 [[attribute(7)]];\n"
    "};\n"
    "struct VSOut {\n"
    "  float4 pos [[position]]; float4 d0; float4 d1; float fog; float4 t0; float4 t1; float4 t2; float4 t3;\n"
    "};\n"
    "struct VSU { float4 screen; };\n"
    "vertex VSOut vs_main(VSIn i [[stage_in]], constant VSU &u [[buffer(0)]]) {\n"
    "  VSOut o;\n"
    /* pre-transformed vertices: undo the divide by w so the rasteriser interpolates perspective-correctly; rhw keeps
     * its sign, so a vertex behind the eye is clipped at the near plane */
    "  float w = 1.0 / i.pos.w;\n"
    "  o.pos = float4((i.pos.x / u.screen.x * 2.0 - 1.0) * w, (1.0 - i.pos.y / u.screen.y * 2.0) * w, i.pos.z * w, w);\n"
    "  o.d0 = i.d0; o.d1 = i.d1; o.fog = i.fog; o.t0 = i.t0; o.t1 = i.t1; o.t2 = i.t2; o.t3 = i.t3;\n"
    "  return o;\n"
    "}\n"
    /* atest.x: D3DCMP function (0 = test off), y: reference 0..1; tx.x: 1 = modulate with stage 0, tx.yz: 1 / texel
     * size for rect (texel-unit) coordinates, 1 otherwise */
    "struct FSU { float4 atest; float4 tx; };\n"
    "fragment float4 fs_color(VSOut i [[stage_in]], constant FSU &u [[buffer(0)]],\n"
    "                        texture2d<float> tex0 [[texture(0)]], sampler smp0 [[sampler(0)]]) {\n"
    "  float4 c = i.d0;\n"
    "  if (u.tx.x > 0.5) c *= tex0.sample(smp0, i.t0.xy * u.tx.yz);\n"
    "  int f = int(u.atest.x); float r = u.atest.y; bool pass = true;\n"
    "  if (f == 1) pass = false; else if (f == 2) pass = c.a < r; else if (f == 3) pass = c.a == r;\n"
    "  else if (f == 4) pass = c.a <= r; else if (f == 5) pass = c.a > r; else if (f == 6) pass = c.a != r;\n"
    "  else if (f == 7) pass = c.a >= r;\n"
    "  if (!pass) discard_fragment();\n"
    "  return c;\n"
    "}\n";

static SDL_GPUShader *make_shader(SDL_GPUShaderStage stage, const char *entry, Uint32 uniforms, Uint32 samplers)
{
    SDL_GPUShaderCreateInfo ci;
    SDL_GPUShader *s;
    memset(&ci, 0, sizeof ci);
    ci.code = (const Uint8 *)g_msl; ci.code_size = sizeof g_msl;
    ci.entrypoint = entry; ci.format = SDL_GPU_SHADERFORMAT_MSL; ci.stage = stage;
    ci.num_uniform_buffers = uniforms; ci.num_samplers = samplers;
    s = SDL_CreateGPUShader(g_dev, &ci);
    if (!s) fprintf(stderr, "[GPU] shader %s failed: %s\n", entry, SDL_GetError());
    return s;
}

/* ================================================================= */
/* device state (what SetRenderState / SetTextureStageState keep)     */
/* ================================================================= */

static DWORD g_rs[512];
static DWORD g_tss[4][64];
static DWORD g_fvf;
static int   g_wclip_on; static long g_wclip[4];
static void  state_defaults(void)
{
    memset(g_rs, 0, sizeof g_rs);
    g_rs[D3DRS_ZENABLE] = 1; g_rs[D3DRS_ZWRITEENABLE] = 1; g_rs[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
    g_rs[D3DRS_CULLMODE] = D3DCULL_CCW; g_rs[D3DRS_COLORWRITEENABLE] = 0xF;
    g_rs[D3DRS_SRCBLEND] = D3DBLEND_ONE; g_rs[D3DRS_DESTBLEND] = D3DBLEND_ZERO; g_rs[D3DRS_BLENDOP] = 1;
    g_rs[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS; g_rs[D3DRS_STENCILMASK] = 0xFFFFFFFFu; g_rs[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFFu;
    g_rs[D3DRS_STENCILFAIL] = g_rs[D3DRS_STENCILZFAIL] = g_rs[D3DRS_STENCILPASS] = 1;
    g_rs[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS; g_rs[D3DRS_FILLMODE] = 3;
    {   int st; memset(g_tss, 0, sizeof g_tss);
        for (st = 0; st < 4; st++) {
            g_tss[st][D3DTSS_ADDRESSU] = g_tss[st][D3DTSS_ADDRESSV] = 1;
            g_tss[st][D3DTSS_MAGFILTER] = g_tss[st][D3DTSS_MINFILTER] = 2;
        } }
}

void d3d8_SetWindowClip(int on, long x0, long y0, long x1, long y1)
{
    g_wclip_on = on; g_wclip[0] = x0; g_wclip[1] = y0; g_wclip[2] = x1; g_wclip[3] = y1;
}
int d3d8_WindowClipOn(void) { return g_wclip_on; }

/* ================================================================= */
/* pipelines                                                          */
/* ================================================================= */

typedef struct {
    uint64_t ps;                                  /* nv2a_psh_key of the combiner shader, 0 = the plain vertex-colour one */
    uint8_t blend_en, src, dst, op, cwrite;
    uint8_t z_en, z_wr, z_func;
    uint8_t st_en, st_func, st_rmask, st_wmask, st_fail, st_zfail, st_pass;
    uint8_t cull, ccw, clip, prim, wire;
} PKey;

typedef struct { PKey k; SDL_GPUGraphicsPipeline *p; } PCache;
static PCache g_pc[2048];
static int g_npc;

static SDL_GPUBlendFactor blend_f(DWORD b, int alpha)
{
    switch (b) {
    case D3DBLEND_ZERO:         return SDL_GPU_BLENDFACTOR_ZERO;
    case D3DBLEND_ONE:          return SDL_GPU_BLENDFACTOR_ONE;
    case D3DBLEND_SRCCOLOR:     return alpha ? SDL_GPU_BLENDFACTOR_SRC_ALPHA : SDL_GPU_BLENDFACTOR_SRC_COLOR;
    case D3DBLEND_INVSRCCOLOR:  return alpha ? SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
    case D3DBLEND_SRCALPHA:     return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    case D3DBLEND_INVSRCALPHA:  return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    case D3DBLEND_DESTALPHA:    return SDL_GPU_BLENDFACTOR_DST_ALPHA;
    case D3DBLEND_INVDESTALPHA: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
    case D3DBLEND_DESTCOLOR:    return alpha ? SDL_GPU_BLENDFACTOR_DST_ALPHA : SDL_GPU_BLENDFACTOR_DST_COLOR;
    case D3DBLEND_INVDESTCOLOR: return alpha ? SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA : SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
    case D3DBLEND_SRCALPHASAT:  return SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE;
    default:                    return SDL_GPU_BLENDFACTOR_ONE;
    }
}
static SDL_GPUBlendOp blend_o(DWORD o)
{
    switch (o) { case 2: return SDL_GPU_BLENDOP_SUBTRACT; case 3: return SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
                 case 4: return SDL_GPU_BLENDOP_MIN; case 5: return SDL_GPU_BLENDOP_MAX; default: return SDL_GPU_BLENDOP_ADD; }
}
static SDL_GPUCompareOp cmp_o(DWORD c)
{
    switch (c) {
    case D3DCMP_NEVER: return SDL_GPU_COMPAREOP_NEVER;          case D3DCMP_LESS: return SDL_GPU_COMPAREOP_LESS;
    case D3DCMP_EQUAL: return SDL_GPU_COMPAREOP_EQUAL;          case D3DCMP_LESSEQUAL: return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    case D3DCMP_GREATER: return SDL_GPU_COMPAREOP_GREATER;      case D3DCMP_NOTEQUAL: return SDL_GPU_COMPAREOP_NOT_EQUAL;
    case D3DCMP_GREATEREQUAL: return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    default: return SDL_GPU_COMPAREOP_ALWAYS;
    }
}
static SDL_GPUStencilOp stencil_o(DWORD o)
{
    switch (o) {
    case 2: return SDL_GPU_STENCILOP_ZERO; case 3: return SDL_GPU_STENCILOP_REPLACE;
    case 4: return SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP; case 5: return SDL_GPU_STENCILOP_DECREMENT_AND_CLAMP;
    case 6: return SDL_GPU_STENCILOP_INVERT; case 7: return SDL_GPU_STENCILOP_INCREMENT_AND_WRAP;
    case 8: return SDL_GPU_STENCILOP_DECREMENT_AND_WRAP; default: return SDL_GPU_STENCILOP_KEEP;
    }
}

static SDL_GPUShader *psh_shader(uint64_t key);
static int pipeline_has_psh(const PKey *k);

static SDL_GPUGraphicsPipeline *pipeline_for(const PKey *k)
{
    SDL_GPUGraphicsPipelineCreateInfo pi;
    SDL_GPUColorTargetDescription ct;
    SDL_GPUVertexBufferDescription vb;
    SDL_GPUVertexAttribute at[8];
    SDL_GPUGraphicsPipeline *p;
    int i;
    for (i = 0; i < g_npc; i++) if (!memcmp(&g_pc[i].k, k, sizeof *k)) return g_pc[i].p;
    {   /* OT_GPU_PIPELINE_CACHE_MAX=<n> (test): a smaller cache, to exercise the full path */
        static int cap = -1;
        if (cap < 0) { const char *e = getenv("OT_GPU_PIPELINE_CACHE_MAX"); cap = e ? atoi(e) : 0; }
        if (cap > 0 && cap < (int)(sizeof g_pc / sizeof g_pc[0]) && g_npc >= cap) goto cache_full;
    }
    if (g_npc >= (int)(sizeof g_pc / sizeof g_pc[0])) {   /* full: no compile, no leak -- the draw is skipped */
cache_full:
        static int told;
        if (!told) { told = 1; fprintf(stderr, "[GPU] pipeline cache full (%d): draws with a new state are skipped\n", g_npc); }
        return NULL;
    }

    memset(&pi, 0, sizeof pi); memset(&ct, 0, sizeof ct);
    memset(&vb, 0, sizeof vb); memset(at, 0, sizeof at);
    vb.slot = 0; vb.pitch = 116; vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    {   /* ProgVertex: pos, d0, d1, fog, t0..t3 */
        static const Uint32 off[8] = { 0, 16, 32, 48, 52, 68, 84, 100 };
        for (i = 0; i < 8; i++) {
            at[i].location = (Uint32)i; at[i].buffer_slot = 0; at[i].offset = off[i];
            at[i].format = (i == 3) ? SDL_GPU_VERTEXELEMENTFORMAT_FLOAT : SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        }
    }
    pi.vertex_shader = g_vs; pi.fragment_shader = k->ps ? psh_shader(k->ps) : g_fs;
    if (!pi.fragment_shader) pi.fragment_shader = g_fs;
    pi.vertex_input_state.vertex_buffer_descriptions = &vb; pi.vertex_input_state.num_vertex_buffers = 1;
    pi.vertex_input_state.vertex_attributes = at; pi.vertex_input_state.num_vertex_attributes = 8;
    switch (k->prim) {
    case D3DPT_POINTLIST:     pi.primitive_type = SDL_GPU_PRIMITIVETYPE_POINTLIST; break;
    case D3DPT_LINELIST:      pi.primitive_type = SDL_GPU_PRIMITIVETYPE_LINELIST; break;
    case D3DPT_LINESTRIP:     pi.primitive_type = SDL_GPU_PRIMITIVETYPE_LINESTRIP; break;
    case D3DPT_TRIANGLESTRIP: pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP; break;
    default:                  pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST; break;
    }
    pi.rasterizer_state.fill_mode = k->wire ? SDL_GPU_FILLMODE_LINE : SDL_GPU_FILLMODE_FILL;
    pi.rasterizer_state.cull_mode = k->cull == 1 ? SDL_GPU_CULLMODE_FRONT : k->cull == 2 ? SDL_GPU_CULLMODE_BACK : SDL_GPU_CULLMODE_NONE;
    pi.rasterizer_state.front_face = k->ccw ? SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE : SDL_GPU_FRONTFACE_CLOCKWISE;
    pi.rasterizer_state.enable_depth_clip = k->clip != 0;
    pi.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    pi.depth_stencil_state.enable_depth_test = k->z_en != 0;
    pi.depth_stencil_state.enable_depth_write = k->z_wr != 0;
    pi.depth_stencil_state.compare_op = cmp_o(k->z_func);
    pi.depth_stencil_state.enable_stencil_test = k->st_en != 0;
    pi.depth_stencil_state.compare_mask = k->st_rmask; pi.depth_stencil_state.write_mask = k->st_wmask;
    pi.depth_stencil_state.front_stencil_state.compare_op = cmp_o(k->st_func);
    pi.depth_stencil_state.front_stencil_state.fail_op = stencil_o(k->st_fail);
    pi.depth_stencil_state.front_stencil_state.depth_fail_op = stencil_o(k->st_zfail);
    pi.depth_stencil_state.front_stencil_state.pass_op = stencil_o(k->st_pass);
    pi.depth_stencil_state.back_stencil_state = pi.depth_stencil_state.front_stencil_state;
    ct.format = g_rt_fmt;
    ct.blend_state.enable_blend = k->blend_en != 0;
    ct.blend_state.src_color_blendfactor = blend_f(k->src, 0); ct.blend_state.dst_color_blendfactor = blend_f(k->dst, 0);
    ct.blend_state.src_alpha_blendfactor = blend_f(k->src, 1); ct.blend_state.dst_alpha_blendfactor = blend_f(k->dst, 1);
    ct.blend_state.color_blend_op = ct.blend_state.alpha_blend_op = blend_o(k->op);
    ct.blend_state.color_write_mask = (SDL_GPUColorComponentFlags)(((k->cwrite & 1) ? SDL_GPU_COLORCOMPONENT_R : 0) |
        ((k->cwrite & 2) ? SDL_GPU_COLORCOMPONENT_G : 0) | ((k->cwrite & 4) ? SDL_GPU_COLORCOMPONENT_B : 0) |
        ((k->cwrite & 8) ? SDL_GPU_COLORCOMPONENT_A : 0));
    ct.blend_state.enable_color_write_mask = true;
    pi.target_info.color_target_descriptions = &ct; pi.target_info.num_color_targets = 1;
    pi.target_info.depth_stencil_format = g_ds_fmt; pi.target_info.has_depth_stencil_target = true;
    p = SDL_CreateGPUGraphicsPipeline(g_dev, &pi);
    if (!p) fprintf(stderr, "[GPU] pipeline failed: %s\n", SDL_GetError());
    g_pc[g_npc].k = *k; g_pc[g_npc].p = p; g_npc++;     /* a failed creation is cached too (NULL): not retried every draw */
    return p;
}

static void key_from_state(PKey *k, int prim, int raster, int use_raster)
{
    memset(k, 0, sizeof *k);
    k->blend_en = g_rs[D3DRS_ALPHABLENDENABLE] != 0;
    k->src = (uint8_t)g_rs[D3DRS_SRCBLEND]; k->dst = (uint8_t)g_rs[D3DRS_DESTBLEND];
    k->op = (uint8_t)(g_rs[D3DRS_BLENDOP] ? g_rs[D3DRS_BLENDOP] : 1);
    k->cwrite = (uint8_t)(g_rs[D3DRS_COLORWRITEENABLE] & 0xF);
    k->z_en = g_rs[D3DRS_ZENABLE] != 0; k->z_wr = g_rs[D3DRS_ZWRITEENABLE] != 0; k->z_func = (uint8_t)g_rs[D3DRS_ZFUNC];
    k->st_en = g_rs[D3DRS_STENCILENABLE] != 0; k->st_func = (uint8_t)g_rs[D3DRS_STENCILFUNC];
    k->st_rmask = (uint8_t)g_rs[D3DRS_STENCILMASK]; k->st_wmask = (uint8_t)g_rs[D3DRS_STENCILWRITEMASK];
    k->st_fail = (uint8_t)g_rs[D3DRS_STENCILFAIL]; k->st_zfail = (uint8_t)g_rs[D3DRS_STENCILZFAIL];
    k->st_pass = (uint8_t)g_rs[D3DRS_STENCILPASS];
    k->prim = (uint8_t)prim;
    k->wire = g_rs[D3DRS_FILLMODE] == 2;
    if (use_raster) {   /* NV2A draws: bit 0 depth clip, bits 2:1 cull (0 none, 1 front, 2 back), bit 3 front face CCW */
        k->clip = (raster & 1) != 0; k->cull = (uint8_t)((raster >> 1) & 3); k->ccw = (raster & 8) != 0;
    } else {
        k->clip = 1; k->ccw = 0;
        k->cull = g_rs[D3DRS_CULLMODE] == D3DCULL_CW ? 1 : g_rs[D3DRS_CULLMODE] == D3DCULL_CCW ? 2 : 0;
    }
}

/* ================================================================= */
/* recording                                                          */
/* ================================================================= */

typedef struct { uint8_t min, mag, mip, au, av, maxmip, aniso, pad; float bias; } SKey;
enum { OP_DRAW = 1, OP_CLEAR = 2 };
typedef struct {
    int type;
    PKey key;
    uint32_t first, count;                 /* vertices in the arena */
    int scissor_on; long sc[4];
    float atest[4];
    float tx[4];
    Tex *tex[4];                           /* bound textures (a reference is held until the frame is presented) */
    SKey skey[4];
    int psh;                               /* 1: a combiner shader, `pc` holds its constants */
    float pc[sizeof(Nv2aPshConsts) / 4];
    uint32_t stencil_ref;
    /* clear */
    DWORD flags; float color[4]; float z; DWORD stencil; unsigned x0, y0, x1, y1;
} Op;

#define MAX_OPS   65536
#define ARENA_MAX (96u * 1024u * 1024u)
static Op      *g_ops;
static uint32_t g_nops;
static uint8_t *g_arena;
static uint32_t g_arena_used;               /* bytes */
static SDL_GPUBuffer *g_vbuf; static uint32_t g_vbuf_size;
static SDL_GPUTransferBuffer *g_tbuf; static uint32_t g_tbuf_size;
static unsigned g_dropped;

static uint32_t arena_add(const void *v, uint32_t n_verts, uint32_t stride, int convert_ffp)
{
    uint32_t first = g_arena_used / 116u, i;
    uint32_t bytes = n_verts * 116u;
    if (g_arena_used + bytes > ARENA_MAX) return (uint32_t)-1;
    if (!convert_ffp) {
        if (stride == 116u) memcpy(g_arena + g_arena_used, v, bytes);
        else for (i = 0; i < n_verts; i++) memcpy(g_arena + g_arena_used + i * 116u, (const uint8_t *)v + i * stride, 116u);
    } else {   /* OutputVertex: x, y, z, rhw, D3DCOLOR (ARGB), u, v  ->  ProgVertex */
        for (i = 0; i < n_verts; i++) {
            const float *s = (const float *)((const uint8_t *)v + i * stride);
            float *d = (float *)(g_arena + g_arena_used + i * 116u);
            uint32_t c; memcpy(&c, &s[4], 4);
            memset(d, 0, 116u);
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
            d[4] = ((c >> 16) & 255) / 255.0f; d[5] = ((c >> 8) & 255) / 255.0f; d[6] = (c & 255) / 255.0f; d[7] = ((c >> 24) & 255) / 255.0f;
            d[8] = d[9] = d[10] = d[11] = 1.0f;
            d[13] = s[5]; d[14] = s[6]; d[16] = 1.0f;     /* t0 = (u, v, 0, 1) */
        }
    }
    g_arena_used += bytes;
    return first;
}

static Op *op_new(int type)
{
    Op *o;
    if (g_nops >= MAX_OPS) { g_dropped++; return NULL; }
    o = &g_ops[g_nops++];
    memset(o, 0, sizeof *o);
    o->type = type;
    return o;
}

static void fill_alpha_test(Op *o)
{
    o->atest[0] = g_rs[D3DRS_ALPHATESTENABLE] ? (float)g_rs[D3DRS_ALPHAFUNC] : 0.0f;
    o->atest[1] = (float)(g_rs[D3DRS_ALPHAREF] & 0xFF) / 255.0f;
}

static void capture_tex(Op *o, const Nv2aPshConsts *pc);
static void record_draw(int prim, uint32_t nverts, const void *v, uint32_t stride, int ffp, int raster, int use_raster,
                        const Nv2aPshConsts *pc, uint64_t ps_key)
{
    Op *o; uint32_t first;
    if (!nverts || !g_ops) return;
    if (prim == D3DPT_TRIANGLEFAN) return;      /* the translator does not emit fans; Metal has none */
    first = arena_add(v, nverts, stride, ffp);
    if (first == (uint32_t)-1) { g_dropped++; return; }
    o = op_new(OP_DRAW);
    if (!o) return;
    key_from_state(&o->key, prim, raster, use_raster);
    if (pc && ps_key) { o->key.ps = ps_key; o->psh = 1; memcpy(o->pc, pc, sizeof o->pc); }
    o->first = first; o->count = nverts;
    o->scissor_on = g_wclip_on; memcpy(o->sc, g_wclip, sizeof o->sc);
    fill_alpha_test(o);
    capture_tex(o, pc);
    o->stencil_ref = g_rs[D3DRS_STENCILREF] & 0xFF;
}

static uint32_t prim_verts(int prim, uint32_t pc)
{
    switch (prim) {
    case D3DPT_POINTLIST: return pc;
    case D3DPT_LINELIST: return pc * 2;
    case D3DPT_LINESTRIP: return pc + 1;
    case D3DPT_TRIANGLELIST: return pc * 3;
    case D3DPT_TRIANGLESTRIP: return pc + 2;
    default: return 0;
    }
}

HRESULT d3d8_nv2a_draw(D3DPRIMITIVETYPE prim, UINT prim_count, const void *verts, UINT stride,
                       unsigned long long ps_key, const void *ps_consts, UINT ps_consts_size, int raster)
{
    record_draw((int)prim, prim_verts((int)prim, prim_count), verts, stride, 0, raster, 1,
                ps_consts_size >= sizeof(Nv2aPshConsts) ? (const Nv2aPshConsts *)ps_consts : NULL, ps_key);
    return 0;
}

static void clear_op(DWORD flags, DWORD color, float z, DWORD stencil, unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
    Op *o = op_new(OP_CLEAR);
    if (!o) return;
    o->flags = flags; o->z = z; o->stencil = stencil;
    o->color[0] = ((color >> 16) & 255) / 255.0f; o->color[1] = ((color >> 8) & 255) / 255.0f;
    o->color[2] = (color & 255) / 255.0f;         o->color[3] = ((color >> 24) & 255) / 255.0f;
    o->x0 = x0; o->y0 = y0; o->x1 = x1; o->y1 = y1;
    o->count = 0;
    if ((flags & 1) && !(x0 == 0 && y0 == 0 && x1 >= TITLE_W - 1 && y1 >= TITLE_H - 1)) {
        /* a colour rectangle: a quad in the clear colour (depth and stencil are cleared whole, as on the D3D11 layer) */
        float v[6][29]; int j;
        float xs[4] = { (float)x0, (float)x1 + 1.0f, (float)x0, (float)x1 + 1.0f };
        float ys[4] = { (float)y0, (float)y0, (float)y1 + 1.0f, (float)y1 + 1.0f };
        static const int qi[6] = { 0, 1, 2, 2, 1, 3 };
        memset(v, 0, sizeof v);
        for (j = 0; j < 6; j++) {
            v[j][0] = xs[qi[j]]; v[j][1] = ys[qi[j]]; v[j][2] = 0.5f; v[j][3] = 1.0f;
            v[j][4] = o->color[0]; v[j][5] = o->color[1]; v[j][6] = o->color[2]; v[j][7] = o->color[3];
        }
        o->first = arena_add(v, 6, 116, 0);
        if (o->first != (uint32_t)-1) o->count = 6;
    }
}

void d3d8_ClearRect(DWORD flags, D3DCOLOR color, float z, DWORD stencil,
                    unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
    clear_op(flags, color, z, stencil, x0, y0, x1, y1);
}

/* ================================================================= */
/* executing a frame                                                  */
/* ================================================================= */

static int ensure_targets(void)
{
    SDL_GPUTextureCreateInfo ti;
    if (g_rt) return 1;
    d3d8_host_target_size(&g_tw, &g_th);
    fprintf(stderr, "[GPU] scene target %ux%u (display shape %.3f)\n", g_tw, g_th, d3d8_HostAspect());
    memset(&ti, 0, sizeof ti);
    ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = g_rt_fmt; ti.width = g_tw; ti.height = g_th;
    ti.layer_count_or_depth = 1; ti.num_levels = 1; ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    g_rt = SDL_CreateGPUTexture(g_dev, &ti);
    ti.format = g_ds_fmt; ti.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    g_ds = SDL_CreateGPUTexture(g_dev, &ti);
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER; ti.width = TITLE_W; ti.height = TITLE_H;
    g_movie = SDL_CreateGPUTexture(g_dev, &ti);
    if (!g_rt || !g_ds || !g_movie) { fprintf(stderr, "[GPU] target creation failed: %s\n", SDL_GetError()); return 0; }
    return 1;
}

static int ensure_shaders(void)
{
    if (g_vs && g_fs) return 1;
    g_vs = make_shader(SDL_GPU_SHADERSTAGE_VERTEX, "vs_main", 1, 0);
    g_fs = make_shader(SDL_GPU_SHADERSTAGE_FRAGMENT, "fs_color", 1, 1);
    return g_vs && g_fs;
}

static SDL_GPURenderPass *begin_pass(SDL_GPUCommandBuffer *cmd, int clear_color, const float *col, int clear_depth, float z, int clear_stencil, Uint8 st)
{
    SDL_GPUColorTargetInfo ct; SDL_GPUDepthStencilTargetInfo dt;
    memset(&ct, 0, sizeof ct); memset(&dt, 0, sizeof dt);
    ct.texture = g_rt; ct.store_op = SDL_GPU_STOREOP_STORE;
    ct.load_op = clear_color ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    if (clear_color) { ct.clear_color.r = col[0]; ct.clear_color.g = col[1]; ct.clear_color.b = col[2]; ct.clear_color.a = col[3]; }
    dt.texture = g_ds; dt.store_op = SDL_GPU_STOREOP_STORE; dt.stencil_store_op = SDL_GPU_STOREOP_STORE;
    dt.load_op = clear_depth ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD; dt.clear_depth = z;
    dt.stencil_load_op = clear_stencil ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD; dt.clear_stencil = st;
    return SDL_BeginGPURenderPass(cmd, &ct, 1, &dt);
}

static void upload_arena(SDL_GPUCommandBuffer *cmd)
{
    SDL_GPUBufferCreateInfo bi; SDL_GPUTransferBufferCreateInfo ti;
    SDL_GPUCopyPass *cp; void *map;
    SDL_GPUTransferBufferLocation src; SDL_GPUBufferRegion dst;
    uint32_t need = g_arena_used;
    if (!need) return;
    if (need > g_vbuf_size) {
        uint32_t sz = (need + (4u << 20)) & ~((4u << 20) - 1u);
        if (g_vbuf) SDL_ReleaseGPUBuffer(g_dev, g_vbuf);
        if (g_tbuf) SDL_ReleaseGPUTransferBuffer(g_dev, g_tbuf);
        memset(&bi, 0, sizeof bi); bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX; bi.size = sz;
        memset(&ti, 0, sizeof ti); ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; ti.size = sz;
        g_vbuf = SDL_CreateGPUBuffer(g_dev, &bi); g_tbuf = SDL_CreateGPUTransferBuffer(g_dev, &ti);
        g_vbuf_size = g_tbuf ? sz : 0;
    }
    if (!g_vbuf || !g_tbuf) return;
    map = SDL_MapGPUTransferBuffer(g_dev, g_tbuf, true);
    if (!map) return;
    memcpy(map, g_arena, need);
    SDL_UnmapGPUTransferBuffer(g_dev, g_tbuf);
    cp = SDL_BeginGPUCopyPass(cmd);
    src.transfer_buffer = g_tbuf; src.offset = 0; dst.buffer = g_vbuf; dst.offset = 0; dst.size = need;
    SDL_UploadToGPUBuffer(cp, &src, &dst, true);
    SDL_EndGPUCopyPass(cp);
}

static void frame_release_textures(void);
static void upload_textures(SDL_GPUCommandBuffer *cmd);
static int  bind_samplers(SDL_GPURenderPass *pass, const Op *o);

static void replay_ops(SDL_GPUCommandBuffer *cmd)
{
    SDL_GPURenderPass *pass = NULL;
    float black[4] = { 0, 0, 0, 1 };
    uint32_t i;
    int first_pass = !g_rt_cleared;
    if (g_vbuf) { /* vertices live in g_vbuf for the whole frame */ }

    for (i = 0; i < g_nops; i++) {
        Op *o = &g_ops[i];
        if (o->type == OP_CLEAR) {
            int cc = (o->flags & 1) != 0, cz = (o->flags & 2) != 0, cs = (o->flags & 4) != 0;
            int full = o->x0 == 0 && o->y0 == 0 && o->x1 >= TITLE_W - 1 && o->y1 >= TITLE_H - 1;
            if (!cc && !cz && !cs) continue;
            if (pass) { SDL_EndGPURenderPass(pass); pass = NULL; }
            /* a colour clear that covers the target, and depth / stencil (cleared whole, as on the D3D11 layer) */
            pass = begin_pass(cmd, cc && full, o->color, cz, o->z, cs, (Uint8)o->stencil);
            if (cc && !full && o->count) {
                PKey k; SDL_GPUGraphicsPipeline *p; SDL_GPUBufferBinding bb; SDL_Rect all = { 0, 0, (int)g_tw, (int)g_th };
                float screen[4] = { TITLE_W, TITLE_H, 0, 0 }, fs[8] = { 0, 0, 0, 0, 0, 1, 1, 0 };
                memset(&k, 0, sizeof k); k.cwrite = 0xF; k.src = D3DBLEND_ONE; k.dst = D3DBLEND_ZERO; k.op = 1;
                k.z_func = D3DCMP_ALWAYS; k.st_func = D3DCMP_ALWAYS; k.prim = D3DPT_TRIANGLELIST;
                p = pipeline_for(&k);
                if (p && g_vbuf && bind_samplers(pass, o)) {
                    SDL_BindGPUGraphicsPipeline(pass, p);
                    SDL_SetGPUScissor(pass, &all);
                    SDL_PushGPUVertexUniformData(cmd, 0, screen, sizeof screen);
                    SDL_PushGPUFragmentUniformData(cmd, 0, fs, sizeof fs);
                    bb.buffer = g_vbuf; bb.offset = o->first * 116u;
                    SDL_BindGPUVertexBuffers(pass, 0, &bb, 1);
                    SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
                }
            }
            continue;
        }
        if (!pass) { pass = begin_pass(cmd, first_pass, black, first_pass, 1.0f, first_pass, 0); first_pass = 0; }
        {
            SDL_GPUGraphicsPipeline *p = pipeline_for(&o->key);
            SDL_GPUBufferBinding bb;
            float screen[4] = { TITLE_W, TITLE_H, 0, 0 };
            if (!p || !g_vbuf || !bind_samplers(pass, o)) continue;
            SDL_BindGPUGraphicsPipeline(pass, p);
            if (o->scissor_on) {
                float gsx = (float)g_tw / (float)TITLE_W, gsy = (float)g_th / (float)TITLE_H;   /* clip rectangles are in title pixels */
                SDL_Rect r; r.x = (int)(o->sc[0] * gsx); r.y = (int)(o->sc[1] * gsy);
                r.w = (int)((o->sc[2] + 1) * gsx) - r.x; r.h = (int)((o->sc[3] + 1) * gsy) - r.y;
                if (r.x < 0) { r.w += r.x; r.x = 0; } if (r.y < 0) { r.h += r.y; r.y = 0; }
                if (r.w < 0) r.w = 0; if (r.h < 0) r.h = 0;
                SDL_SetGPUScissor(pass, &r);
            } else {
                SDL_Rect r = { 0, 0, (int)g_tw, (int)g_th }; SDL_SetGPUScissor(pass, &r);
            }
            SDL_SetGPUStencilReference(pass, (Uint8)o->stencil_ref);
            SDL_PushGPUVertexUniformData(cmd, 0, screen, sizeof screen);
            if (o->psh && pipeline_has_psh(&o->key)) SDL_PushGPUFragmentUniformData(cmd, 0, o->pc, sizeof o->pc);
            else SDL_PushGPUFragmentUniformData(cmd, 0, o->atest, 2 * sizeof o->atest);   /* atest + tx */
            bb.buffer = g_vbuf; bb.offset = o->first * 116u;
            SDL_BindGPUVertexBuffers(pass, 0, &bb, 1);
            SDL_DrawGPUPrimitives(pass, o->count, 1, 0, 0);
        }
    }
    if (!pass) {   /* nothing drawn: the target still has to be cleared once, and presented */
        pass = begin_pass(cmd, first_pass, black, first_pass, 1.0f, first_pass, 0);
    }
    SDL_EndGPURenderPass(pass);
    g_rt_cleared = 1;
}

static void blit_to_window(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *src, int src_w, int src_h, double shape)
{
    SDL_GPUTexture *sc = NULL; Uint32 w = 0, h = 0;
    SDL_GPUBlitInfo bi;
    { uint64_t a0 = SDL_GetTicksNS(); int ok = SDL_WaitAndAcquireGPUSwapchainTexture(cmd, g_win, &sc, &w, &h); g_t_acquire += SDL_GetTicksNS() - a0; if (!ok || !sc || !w || !h) return; }
    memset(&bi, 0, sizeof bi);
    bi.source.texture = src; bi.source.w = (Uint32)src_w; bi.source.h = (Uint32)src_h;
    bi.destination.texture = sc;
    {   /* letterbox to the picture's shape */
        double ar = shape > 0.5 ? shape : 4.0 / 3.0, wa = (double)w / (double)h; Uint32 dw = w, dh = h;
        if (wa > ar) dw = (Uint32)((double)h * ar + 0.5); else dh = (Uint32)((double)w / ar + 0.5);
        bi.destination.x = (w - dw) / 2; bi.destination.y = (h - dh) / 2; bi.destination.w = dw; bi.destination.h = dh;
    }
    bi.load_op = SDL_GPU_LOADOP_CLEAR; bi.clear_color.a = 1.0f;
    bi.filter = SDL_GPU_FILTER_LINEAR;
    SDL_BlitGPUTexture(cmd, &bi);
}

static void reset_frame(void) { frame_release_textures(); g_nops = 0; g_arena_used = 0; }

/* OT_GPU_SHOT=<prefix> OT_GPU_SHOT_AT=300,900,...: after those scene presents, the 640x480 target is read back
 * and written as <prefix>_<n>.ppm. The way to look at the 3D without a screen capture. */
static int shot_wanted(unsigned scene_present, int movie)
{
    static const char *at[2]; static int init;
    const char *p = at[0];
    if (!init) { init = 1; at[0] = getenv("OT_GPU_SHOT") ? getenv("OT_GPU_SHOT_AT") : NULL;
                 at[1] = getenv("OT_GPU_SHOT") ? getenv("OT_GPU_SHOT_MOVIE_AT") : NULL; }
    p = at[movie ? 1 : 0];
    for (; p && *p; ) {
        char *e; unsigned long v = strtoul(p, &e, 10);
        if (e == p) break;
        if (v == scene_present) return 1;
        p = *e ? e + 1 : e;
    }
    return 0;
}

/* Read a texture back and write it: a PPM (the OT_GPU_SHOT test hook) or a PNG (the F12 screenshot). Submits `cmd`. */
static int capture_save(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *tex, unsigned w, unsigned h, int rgba_order,
                        const char *path, int png)
{
    SDL_GPUTransferBufferCreateInfo ti; SDL_GPUTransferBuffer *tb; SDL_GPUCopyPass *cp;
    SDL_GPUTextureRegion reg; SDL_GPUTextureTransferInfo dst; SDL_GPUFence *fence; void *map;
    int ok = 0; unsigned x, y;
    memset(&ti, 0, sizeof ti); ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; ti.size = w * h * 4;
    tb = SDL_CreateGPUTransferBuffer(g_dev, &ti);
    if (!tb) { SDL_SubmitGPUCommandBuffer(cmd); return 0; }
    cp = SDL_BeginGPUCopyPass(cmd);
    memset(&reg, 0, sizeof reg); memset(&dst, 0, sizeof dst);
    reg.texture = tex; reg.w = w; reg.h = h; reg.d = 1;
    dst.transfer_buffer = tb; dst.pixels_per_row = w; dst.rows_per_layer = h;
    SDL_DownloadFromGPUTexture(cp, &reg, &dst);
    SDL_EndGPUCopyPass(cp);
    fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence) { SDL_WaitForGPUFences(g_dev, true, &fence, 1); SDL_ReleaseGPUFence(g_dev, fence); }
    map = SDL_MapGPUTransferBuffer(g_dev, tb, false);
    if (map && png) {
        uint8_t *rgba = (uint8_t *)malloc((size_t)w * h * 4);
        if (rgba) {
            for (y = 0; y < h; y++)
                for (x = 0; x < w; x++) {
                    const uint8_t *px = (const uint8_t *)map + ((size_t)y * w + x) * 4;      /* B G R A, or R G B A for the movie */
                    uint8_t *o = rgba + ((size_t)y * w + x) * 4;
                    o[0] = rgba_order ? px[0] : px[2]; o[1] = px[1]; o[2] = rgba_order ? px[2] : px[0]; o[3] = 255;
                }
            {
                SDL_Surface *sf = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_RGBA32, rgba, (int)w * 4);
                ok = sf && SDL_SavePNG(sf, path);
                if (!ok) fprintf(stderr, "[GPU] screenshot %s failed: %s\n", path, SDL_GetError());
                if (sf) SDL_DestroySurface(sf);
            }
            free(rgba);
        }
    } else if (map) {
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", w, h);
            for (y = 0; y < h; y++)
                for (x = 0; x < w; x++) {
                    const uint8_t *px = (const uint8_t *)map + ((size_t)y * w + x) * 4;
                    uint8_t rgb[3] = { rgba_order ? px[0] : px[2], px[1], rgba_order ? px[2] : px[0] };
                    fwrite(rgb, 1, 3, f);
                }
            fclose(f); ok = 1;
        }
    }
    if (map) SDL_UnmapGPUTransferBuffer(g_dev, tb);
    SDL_ReleaseGPUTransferBuffer(g_dev, tb);
    return ok;
}

static void shot_save(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *tex, int rgba_order, unsigned n)
{
    char path[1024];
    snprintf(path, sizeof path, "%s_%s%u.ppm", getenv("OT_GPU_SHOT"), rgba_order ? "m" : "", n);
    if (capture_save(cmd, tex, rgba_order ? TITLE_W : g_tw, rgba_order ? TITLE_H : g_th, rgba_order, path, 0))
        fprintf(stderr, "[GPU] saved the %s at present %u to %s\n", rgba_order ? "movie frame" : "3D target", n, path);
}

/* F12 (the window asks): the next scene frame is written as a PNG at the scene target's size. */
static char g_shot_req_path[1024];
static volatile int g_shot_req;
void d3d8_gpu_request_screenshot(const char *path)
{
    if (!path) return;
    snprintf(g_shot_req_path, sizeof g_shot_req_path, "%s", path);
    g_shot_req = 1;
}

/* The scene (recorded draws) or a movie frame (RGBA from the guest framebuffer) reaches the window. */
static uint64_t g_busy_ns;
static unsigned g_npsh;
static void present_now_inner(const uint8_t *movie_rgba, unsigned mw, unsigned mh);
static void present_now(const uint8_t *movie_rgba, unsigned mw, unsigned mh)
{
    uint64_t b0 = SDL_GetTicksNS();
    present_now_inner(movie_rgba, mw, mh);
    g_busy_ns += SDL_GetTicksNS() - b0;
}
static void present_now_inner(const uint8_t *movie_rgba, unsigned mw, unsigned mh)
{
    SDL_GPUCommandBuffer *cmd;
    static unsigned frames; static int announced;
    if (g_failed || !g_dev || !g_win) { reset_frame(); return; }
    if (!ensure_shaders() || !ensure_targets()) { g_failed = 1; reset_frame(); return; }
    if (!announced) { announced = 1; fprintf(stderr, "[GPU] %s: first present (%s)\n", SDL_GetGPUDeviceDriver(g_dev), movie_rgba ? "movie" : "scene"); }

    cmd = SDL_AcquireGPUCommandBuffer(g_dev);
    if (!cmd) { reset_frame(); return; }

    if (movie_rgba && getenv("OT_GPU_TEST_BADMOVIE")) { mw = 320; mh = 240; }   /* test: exercise the wrong-size path */
    if (movie_rgba && (mw != TITLE_W || mh != TITLE_H)) {
        static int told;
        if (!told) { told = 1; fprintf(stderr, "[GPU] movie frame is %ux%u, not %dx%d: not shown\n", mw, mh, TITLE_W, TITLE_H); }
        movie_rgba = NULL; mw = mh = 0;       /* only the movie upload is skipped: the scene below is still presented */
    }
    if (movie_rgba && mw && mh) {
        SDL_GPUTransferBufferCreateInfo ti; SDL_GPUTransferBuffer *tb; void *map;
        SDL_GPUCopyPass *cp; SDL_GPUTextureTransferInfo src; SDL_GPUTextureRegion dst;
        memset(&ti, 0, sizeof ti); ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; ti.size = mw * mh * 4;
        tb = SDL_CreateGPUTransferBuffer(g_dev, &ti);
        map = tb ? SDL_MapGPUTransferBuffer(g_dev, tb, false) : NULL;
        if (map) {
            memcpy(map, movie_rgba, (size_t)mw * mh * 4);
            SDL_UnmapGPUTransferBuffer(g_dev, tb);
            cp = SDL_BeginGPUCopyPass(cmd);
            memset(&src, 0, sizeof src); memset(&dst, 0, sizeof dst);
            src.transfer_buffer = tb; src.pixels_per_row = mw; src.rows_per_layer = mh;
            dst.texture = g_movie; dst.w = mw; dst.h = mh; dst.d = 1;
            SDL_UploadToGPUTexture(cp, &src, &dst, false);
            SDL_EndGPUCopyPass(cp);
            blit_to_window(cmd, g_movie, (int)mw, (int)mh, 4.0 / 3.0);     /* movies keep their own 4:3 */
            { static unsigned movie_presents; movie_presents++;
              if (shot_wanted(movie_presents, 1)) { shot_save(cmd, g_movie, 1, movie_presents); cmd = NULL; } }
        }
        if (tb) SDL_ReleaseGPUTransferBuffer(g_dev, tb);
    } else {
        static unsigned scene_presents;
        { uint64_t e0 = SDL_GetTicksNS(); upload_arena(cmd); upload_textures(cmd); replay_ops(cmd); g_t_encode += SDL_GetTicksNS() - e0; }
        {   /* the scene's shape; a frame the translator marked as boxed (menus kept at the title's own shape) is squeezed
             * into a centred box of that shape */
            double a = d3d8_HostAspect(), box = d3d8_PresentBoxShape();
            blit_to_window(cmd, g_rt, (int)g_tw, (int)g_th, d3d8_PresentBoxOn() && a > box + 0.005 ? box : a);
        }
        scene_presents++;
        if (g_shot_req) {
            g_shot_req = 0;
            if (capture_save(cmd, g_rt, g_tw, g_th, 0, g_shot_req_path, 1)) fprintf(stderr, "[GPU] screenshot saved: %s\n", g_shot_req_path);
            cmd = NULL;
        } else if (shot_wanted(scene_presents, 0)) { shot_save(cmd, g_rt, 0, scene_presents); cmd = NULL; }
    }
    if (cmd) { uint64_t u0 = SDL_GetTicksNS(); SDL_SubmitGPUCommandBuffer(cmd); g_t_submit += SDL_GetTicksNS() - u0; }
    reset_frame();
    if ((frames + 1) % 300 == 0) {   /* presents per second and the time the game thread spent in present_now, every 300 presents */
        static uint64_t t0, busy_prev; uint64_t busy = g_busy_ns - busy_prev, now = SDL_GetTicksNS();
        if (t0) fprintf(stderr, "[GPU] %.1f presents/s (%.1f ms/present in the backend: encode %.1f, swapchain wait %.1f, submit %.1f), %u shaders, %d pipelines\n",
                        300e9 / (double)(now - t0), (double)busy / 300e6, (double)g_t_encode / 300e6, (double)g_t_acquire / 300e6,
                        (double)g_t_submit / 300e6, g_npsh, g_npc);
        {   /* the translator's time in draw_program (CPU vertex programs): per present */
            extern uint64_t pgraph_d3d11_draw_program_ns(uint64_t *calls, uint64_t *verts, uint64_t *vs_ns);
            static uint64_t pn, pc, pv, pvs; uint64_t c, v, vs, n = pgraph_d3d11_draw_program_ns(&c, &v, &vs);
            if (t0) fprintf(stderr, "[GPU]   draw_program %.1f ms/present, of it the vertex programs %.1f (%.0f draws, %.0f vertices per present)\n",
                            (double)(n - pn) / 300e6, (double)(vs - pvs) / 300e6, (double)(c - pc) / 300.0, (double)(v - pv) / 300.0);
            pn = n; pc = c; pv = v; pvs = vs;
        }
        g_t_encode = g_t_acquire = g_t_submit = 0;
        t0 = now; busy_prev = g_busy_ns;
    }
    if ((++frames % 600) == 0 && g_dropped) { fprintf(stderr, "[GPU] %u draws dropped (frame buffers full)\n", g_dropped); g_dropped = 0; }
}

/* called by d3d8_PresentFrame (d3d8_null.c): rgba = the movie frame, NULL = the 3D scene */
void (*d3d8_present_hook)(const uint8_t *rgba, unsigned w, unsigned h);
static void present_hook(const uint8_t *rgba, unsigned w, unsigned h) { present_now(rgba, w, h); }

/* ================================================================= */
/* the IDirect3DDevice8 the translator draws through                  */
/* ================================================================= */

static HRESULT unimpl(int slot)
{
    static unsigned seen[32]; static int n;
    int i;
    for (i = 0; i < n; i++) if (seen[i] == (unsigned)slot) return E_NOTIMPL_;
    if (n < 32) { seen[n++] = (unsigned)slot; fprintf(stderr, "[GPU] unimplemented device method (vtable slot %d)\n", slot); }
    return E_NOTIMPL_;
}
static HRESULT __stdcall u0(void) { return unimpl(0); }
static HRESULT __stdcall u1(void) { return unimpl(1); }
static HRESULT __stdcall u2(void) { return unimpl(2); }
static HRESULT __stdcall u3(void) { return unimpl(3); }
static HRESULT __stdcall u4(void) { return unimpl(4); }
static HRESULT __stdcall u5(void) { return unimpl(5); }
static HRESULT __stdcall u6(void) { return unimpl(6); }
static HRESULT __stdcall u7(void) { return unimpl(7); }
static HRESULT __stdcall u8(void) { return unimpl(8); }
static HRESULT __stdcall u9(void) { return unimpl(9); }
static HRESULT __stdcall u10(void) { return unimpl(10); }
static HRESULT __stdcall u11(void) { return unimpl(11); }
static HRESULT __stdcall u12(void) { return unimpl(12); }
static HRESULT __stdcall u13(void) { return unimpl(13); }
static HRESULT __stdcall u14(void) { return unimpl(14); }
static HRESULT __stdcall u15(void) { return unimpl(15); }
static HRESULT __stdcall u16(void) { return unimpl(16); }
static HRESULT __stdcall u17(void) { return unimpl(17); }
static HRESULT __stdcall u18(void) { return unimpl(18); }
static HRESULT __stdcall u19(void) { return unimpl(19); }
static HRESULT __stdcall u20(void) { return unimpl(20); }
static HRESULT __stdcall u21(void) { return unimpl(21); }
static HRESULT __stdcall u22(void) { return unimpl(22); }
static HRESULT __stdcall u23(void) { return unimpl(23); }
static HRESULT __stdcall u24(void) { return unimpl(24); }
static HRESULT __stdcall u25(void) { return unimpl(25); }
static HRESULT __stdcall u26(void) { return unimpl(26); }
static HRESULT __stdcall u27(void) { return unimpl(27); }
static HRESULT __stdcall u28(void) { return unimpl(28); }
static HRESULT __stdcall u29(void) { return unimpl(29); }
static HRESULT __stdcall u30(void) { return unimpl(30); }
static HRESULT __stdcall u31(void) { return unimpl(31); }
static HRESULT __stdcall u32(void) { return unimpl(32); }
static HRESULT __stdcall u33(void) { return unimpl(33); }
static HRESULT __stdcall u34(void) { return unimpl(34); }
static HRESULT __stdcall u35(void) { return unimpl(35); }
static HRESULT __stdcall u36(void) { return unimpl(36); }
static HRESULT __stdcall u37(void) { return unimpl(37); }
static HRESULT __stdcall u38(void) { return unimpl(38); }
static HRESULT __stdcall u39(void) { return unimpl(39); }
static HRESULT __stdcall u40(void) { return unimpl(40); }
static HRESULT __stdcall u41(void) { return unimpl(41); }
static HRESULT __stdcall u42(void) { return unimpl(42); }
static HRESULT __stdcall u43(void) { return unimpl(43); }
static HRESULT __stdcall u44(void) { return unimpl(44); }
static HRESULT __stdcall u45(void) { return unimpl(45); }
static HRESULT __stdcall u46(void) { return unimpl(46); }
static HRESULT __stdcall u47(void) { return unimpl(47); }
static HRESULT __stdcall u48(void) { return unimpl(48); }
static HRESULT __stdcall u49(void) { return unimpl(49); }
static HRESULT __stdcall u50(void) { return unimpl(50); }
static HRESULT __stdcall u51(void) { return unimpl(51); }
static HRESULT __stdcall u52(void) { return unimpl(52); }
static HRESULT __stdcall u53(void) { return unimpl(53); }
static HRESULT __stdcall u54(void) { return unimpl(54); }
static HRESULT __stdcall u55(void) { return unimpl(55); }
static HRESULT __stdcall u56(void) { return unimpl(56); }
static HRESULT __stdcall u57(void) { return unimpl(57); }
static HRESULT __stdcall u58(void) { return unimpl(58); }
static HRESULT __stdcall u59(void) { return unimpl(59); }
static HRESULT __stdcall u60(void) { return unimpl(60); }
static HRESULT __stdcall u61(void) { return unimpl(61); }
static HRESULT __stdcall u62(void) { return unimpl(62); }
static HRESULT __stdcall u63(void) { return unimpl(63); }
static HRESULT __stdcall u64(void) { return unimpl(64); }
static HRESULT __stdcall u65(void) { return unimpl(65); }
static HRESULT __stdcall u66(void) { return unimpl(66); }
static HRESULT __stdcall u67(void) { return unimpl(67); }
static HRESULT __stdcall u68(void) { return unimpl(68); }
static HRESULT __stdcall u69(void) { return unimpl(69); }
static HRESULT __stdcall u70(void) { return unimpl(70); }
static HRESULT __stdcall u71(void) { return unimpl(71); }
static HRESULT __stdcall u72(void) { return unimpl(72); }
static HRESULT __stdcall u73(void) { return unimpl(73); }
static HRESULT __stdcall u74(void) { return unimpl(74); }
static HRESULT __stdcall u75(void) { return unimpl(75); }
static HRESULT __stdcall u76(void) { return unimpl(76); }
static HRESULT __stdcall u77(void) { return unimpl(77); }
static HRESULT __stdcall u78(void) { return unimpl(78); }
static HRESULT __stdcall u79(void) { return unimpl(79); }
static HRESULT __stdcall u80(void) { return unimpl(80); }
static HRESULT __stdcall u81(void) { return unimpl(81); }
static HRESULT __stdcall u82(void) { return unimpl(82); }
static HRESULT __stdcall u83(void) { return unimpl(83); }
static HRESULT __stdcall u84(void) { return unimpl(84); }
static HRESULT __stdcall u85(void) { return unimpl(85); }
static HRESULT __stdcall u86(void) { return unimpl(86); }
static HRESULT __stdcall u87(void) { return unimpl(87); }
static HRESULT __stdcall u88(void) { return unimpl(88); }
static HRESULT __stdcall u89(void) { return unimpl(89); }
static HRESULT __stdcall u90(void) { return unimpl(90); }
static HRESULT __stdcall u91(void) { return unimpl(91); }
static HRESULT __stdcall u92(void) { return unimpl(92); }
static HRESULT __stdcall u93(void) { return unimpl(93); }
static HRESULT __stdcall u94(void) { return unimpl(94); }
static HRESULT __stdcall u95(void) { return unimpl(95); }
static HRESULT __stdcall u96(void) { return unimpl(96); }
static HRESULT __stdcall u97(void) { return unimpl(97); }
static HRESULT __stdcall u98(void) { return unimpl(98); }
static HRESULT __stdcall u99(void) { return unimpl(99); }
static HRESULT __stdcall u100(void) { return unimpl(100); }
static HRESULT __stdcall u101(void) { return unimpl(101); }
static HRESULT __stdcall u102(void) { return unimpl(102); }
static HRESULT __stdcall u103(void) { return unimpl(103); }
static HRESULT __stdcall u104(void) { return unimpl(104); }
static HRESULT __stdcall u105(void) { return unimpl(105); }
static HRESULT __stdcall u106(void) { return unimpl(106); }
static HRESULT __stdcall u107(void) { return unimpl(107); }
static HRESULT __stdcall u108(void) { return unimpl(108); }
static HRESULT __stdcall u109(void) { return unimpl(109); }
static HRESULT __stdcall u110(void) { return unimpl(110); }
static HRESULT __stdcall u111(void) { return unimpl(111); }
static HRESULT __stdcall u112(void) { return unimpl(112); }
static HRESULT __stdcall u113(void) { return unimpl(113); }
static HRESULT __stdcall u114(void) { return unimpl(114); }
static HRESULT __stdcall u115(void) { return unimpl(115); }
static HRESULT __stdcall u116(void) { return unimpl(116); }
static HRESULT __stdcall u117(void) { return unimpl(117); }
static HRESULT __stdcall u118(void) { return unimpl(118); }
static HRESULT __stdcall u119(void) { return unimpl(119); }
static HRESULT __stdcall u120(void) { return unimpl(120); }
static HRESULT __stdcall u121(void) { return unimpl(121); }
static HRESULT __stdcall u122(void) { return unimpl(122); }
static HRESULT __stdcall u123(void) { return unimpl(123); }
static HRESULT __stdcall u124(void) { return unimpl(124); }
static HRESULT __stdcall u125(void) { return unimpl(125); }
static HRESULT __stdcall u126(void) { return unimpl(126); }
static HRESULT __stdcall u127(void) { return unimpl(127); }

struct Tex {
    const IDirect3DTexture8Vtbl *lpVtbl;
    UINT w, h, levels; D3DFORMAT fmt; LONG ref;
    uint8_t *data[16]; UINT pitch[16]; UINT size[16];
    SDL_GPUTexture *gpu;                   /* created and filled at present */
    int dirty;                             /* the levels changed since the last upload */
    uint32_t stamp;                        /* frame that last listed this texture in g_ftex */
};

static ULONG __stdcall tex_addref(IDirect3DTexture8 *s) { return (ULONG)++((Tex *)s)->ref; }
static ULONG __stdcall tex_release(IDirect3DTexture8 *s)
{
    Tex *t = (Tex *)s; UINT i;
    if (--t->ref > 0) return (ULONG)t->ref;
    for (i = 0; i < 16; i++) free(t->data[i]);
    if (t->gpu && g_dev) SDL_ReleaseGPUTexture(g_dev, t->gpu);
    free(t);
    return 0;
}
static DWORD __stdcall tex_levelcount(IDirect3DTexture8 *s) { return ((Tex *)s)->levels; }
static HRESULT __stdcall tex_lock(IDirect3DTexture8 *s, UINT lvl, D3DLOCKED_RECT *lr, const RECT *r, DWORD fl)
{
    Tex *t = (Tex *)s; (void)r; (void)fl;
    if (lvl >= t->levels || !t->data[lvl]) return (HRESULT)0x8876086C;
    lr->pBits = t->data[lvl]; lr->Pitch = (INT)t->pitch[lvl];
    return 0;
}
static HRESULT __stdcall tex_unlock(IDirect3DTexture8 *s, UINT lvl) { (void)lvl; ((Tex *)s)->dirty = 1; return 0; }
static HRESULT __stdcall tex_leveldesc(IDirect3DTexture8 *s, UINT lvl, D3DSURFACE_DESC *d)
{
    Tex *t = (Tex *)s;
    memset(d, 0, sizeof *d);
    d->Width = t->w >> lvl ? t->w >> lvl : 1; d->Height = t->h >> lvl ? t->h >> lvl : 1; d->Format = t->fmt;
    return 0;
}

static IDirect3DTexture8Vtbl g_texvt;

static HRESULT __stdcall dev_createtexture(IDirect3DDevice8 *self, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture8 **out)
{
    Tex *t; UINT i, lw, lh;
    (void)self; (void)usage; (void)pool;
    if (!w || !h) return (HRESULT)0x8876086C;
    if (!levels) levels = 1;
    if (levels > 16) levels = 16;
    t = (Tex *)calloc(1, sizeof *t);
    if (!t) return (HRESULT)0x8007000E;
    t->dirty = 1; t->lpVtbl = &g_texvt; t->w = w; t->h = h; t->levels = levels; t->fmt = fmt; t->ref = 1;
    for (i = 0; i < levels; i++) {
        lw = w >> i; if (!lw) lw = 1;
        lh = h >> i; if (!lh) lh = 1;
        if (fmt == D3DFMT_DXT1 || fmt == D3DFMT_DXT3 || fmt == D3DFMT_DXT5) {
            UINT bw = (lw + 3) / 4, bh = (lh + 3) / 4, bs = fmt == D3DFMT_DXT1 ? 8 : 16;
            t->pitch[i] = bw * bs; t->size[i] = bw * bh * bs;
        } else { t->pitch[i] = lw * 4; t->size[i] = lw * lh * 4; }
        t->data[i] = (uint8_t *)calloc(1, t->size[i]);
    }
    *out = (IDirect3DTexture8 *)t;
    return 0;
}

static IDirect3DBaseTexture8 *g_bound[4];

/* ---- textures: CPU levels -> SDL_GPUTexture at present, sampled as stage 0 (Slice 2) ---------------------------- */

#define MAX_FTEX 8192
static Tex *g_ftex[MAX_FTEX];               /* textures the frame's ops refer to (each op holds a reference) */
static uint32_t g_nftex, g_frame_id = 1;
static SDL_GPUTexture *g_white;             /* 1x1 white: bound when a draw has no texture */

static void skey_from_tss(SKey *k, int st)
{
    const DWORD *ts = g_tss[st]; float bias;
    memset(k, 0, sizeof *k);
    memcpy(&bias, &ts[D3DTSS_MIPMAPLODBIAS], 4);
    k->min = (uint8_t)ts[D3DTSS_MINFILTER]; k->mag = (uint8_t)ts[D3DTSS_MAGFILTER]; k->mip = (uint8_t)ts[D3DTSS_MIPFILTER];
    k->au = (uint8_t)ts[D3DTSS_ADDRESSU]; k->av = (uint8_t)ts[D3DTSS_ADDRESSV];
    k->maxmip = (uint8_t)(ts[D3DTSS_MAXMIPLEVEL] > 15 ? 15 : ts[D3DTSS_MAXMIPLEVEL]);
    k->aniso = (uint8_t)(ts[D3DTSS_MAXANISOTROPY] > 16 ? 16 : ts[D3DTSS_MAXANISOTROPY]);
    k->bias = (bias == bias && bias > -16.0f && bias < 16.0f) ? bias : 0.0f;     /* NaN / absurd -> 0 */
}

static void capture_tex(Op *o, const Nv2aPshConsts *pc)
{
    int st, last = o->psh ? 3 : 0;
    o->tx[1] = o->tx[2] = 1.0f;
    for (st = 0; st <= last; st++) {
        Tex *t = (Tex *)g_bound[st];
        skey_from_tss(&o->skey[st], st);
        if (!t) continue;
        if (t->stamp != g_frame_id) {
            if (g_nftex >= MAX_FTEX) continue;
            g_ftex[g_nftex++] = t; t->stamp = g_frame_id;
        }
        t->ref++;
        o->tex[st] = t;
        if (st == 0) {
            o->tx[0] = 1.0f;
            if (!o->psh && pc && pc->tex_size[0][2] > 0.5f) {   /* linear image, plain shader: texel units */
                o->tx[1] = 1.0f / (pc->tex_size[0][0] > 0 ? pc->tex_size[0][0] : (float)t->w);
                o->tx[2] = 1.0f / (pc->tex_size[0][1] > 0 ? pc->tex_size[0][1] : (float)t->h);
            }
        }
    }
}

static void frame_release_textures(void)
{
    uint32_t i;
    for (i = 0; i < g_nops; i++) { int st; for (st = 0; st < 4; st++) if (g_ops[i].tex[st]) { tex_release((IDirect3DTexture8 *)g_ops[i].tex[st]); g_ops[i].tex[st] = NULL; } }
    g_nftex = 0; g_frame_id++;
}

static SDL_GPUSampler *sampler_for(const SKey *k)
{
    static struct { SKey k; SDL_GPUSampler *s; } c[256];
    static int n, told;
    SDL_GPUSamplerCreateInfo si; SDL_GPUSampler *s; int i;
    for (i = 0; i < n; i++) if (!memcmp(&c[i].k, k, sizeof *k)) return c[i].s;
    if (n >= 256) { if (!told) { told = 1; fprintf(stderr, "[GPU] sampler cache full: using the first sampler\n"); } return c[0].s; }
    memset(&si, 0, sizeof si);
    si.min_filter = k->min >= 2 ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    si.mag_filter = k->mag >= 2 ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    si.mipmap_mode = k->mip >= 2 ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = k->au == 1 ? SDL_GPU_SAMPLERADDRESSMODE_REPEAT : k->au == 2 ? SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT : SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    si.address_mode_v = k->av == 1 ? SDL_GPU_SAMPLERADDRESSMODE_REPEAT : k->av == 2 ? SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT : SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    si.address_mode_w = si.address_mode_u;
    si.mip_lod_bias = k->bias; si.min_lod = (float)k->maxmip; si.max_lod = k->mip ? 1000.0f : (float)k->maxmip;
    if (k->aniso > 1 && k->min >= 2) { si.enable_anisotropy = true; si.max_anisotropy = (float)k->aniso; }
    s = SDL_CreateGPUSampler(g_dev, &si);
    if (!s) fprintf(stderr, "[GPU] sampler failed: %s\n", SDL_GetError());
    c[n].k = *k; c[n].s = s; n++;
    return s;
}

static SDL_GPUTextureFormat tex_gpu_fmt(D3DFORMAT f)
{
    return f == D3DFMT_DXT1 ? SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM : f == D3DFMT_DXT3 ? SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM :
           f == D3DFMT_DXT5 ? SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM : SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
}

static void upload_textures(SDL_GPUCommandBuffer *cmd)
{
    SDL_GPUCopyPass *cp = NULL; uint32_t i;
    if (!g_white) {
        SDL_GPUTextureCreateInfo ti; SDL_GPUTransferBufferCreateInfo bi; SDL_GPUTransferBuffer *tb; void *map;
        memset(&ti, 0, sizeof ti);
        ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; ti.width = ti.height = 1;
        ti.layer_count_or_depth = 1; ti.num_levels = 1; ti.sample_count = SDL_GPU_SAMPLECOUNT_1; ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        g_white = SDL_CreateGPUTexture(g_dev, &ti);
        memset(&bi, 0, sizeof bi); bi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; bi.size = 4;
        tb = g_white ? SDL_CreateGPUTransferBuffer(g_dev, &bi) : NULL;
        map = tb ? SDL_MapGPUTransferBuffer(g_dev, tb, false) : NULL;
        if (map) {
            SDL_GPUTextureTransferInfo src; SDL_GPUTextureRegion dst;
            memset(map, 0xFF, 4); SDL_UnmapGPUTransferBuffer(g_dev, tb);
            cp = SDL_BeginGPUCopyPass(cmd);
            memset(&src, 0, sizeof src); memset(&dst, 0, sizeof dst);
            src.transfer_buffer = tb; src.pixels_per_row = 1; src.rows_per_layer = 1;
            dst.texture = g_white; dst.w = dst.h = dst.d = 1;
            SDL_UploadToGPUTexture(cp, &src, &dst, false);
        }
        if (tb) SDL_ReleaseGPUTransferBuffer(g_dev, tb);
    }
    for (i = 0; i < g_nftex; i++) {
        Tex *t = g_ftex[i];
        SDL_GPUTransferBufferCreateInfo bi; SDL_GPUTransferBuffer *tb; uint8_t *map; UINT l; uint32_t total = 0, off = 0;
        if (t->gpu && !t->dirty) continue;
        if (!t->gpu) {
            SDL_GPUTextureCreateInfo ti; SDL_GPUTextureFormat f = tex_gpu_fmt(t->fmt);
            static int told;
            if (!SDL_GPUTextureSupportsFormat(g_dev, f, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
                if (!told) { told = 1; fprintf(stderr, "[GPU] texture format %d (xbox %d) not supported by the device: drawn untextured\n", (int)f, (int)t->fmt); }
                t->dirty = 0; continue;
            }
            memset(&ti, 0, sizeof ti);
            ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = f; ti.width = t->w; ti.height = t->h;
            ti.layer_count_or_depth = 1; ti.num_levels = t->levels; ti.sample_count = SDL_GPU_SAMPLECOUNT_1; ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
            t->gpu = SDL_CreateGPUTexture(g_dev, &ti);
            if (!t->gpu) { fprintf(stderr, "[GPU] texture %ux%u fmt %d failed: %s\n", t->w, t->h, (int)t->fmt, SDL_GetError()); t->dirty = 0; continue; }
        }
        for (l = 0; l < t->levels; l++) total += t->size[l];
        memset(&bi, 0, sizeof bi); bi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; bi.size = total;
        tb = SDL_CreateGPUTransferBuffer(g_dev, &bi);
        map = tb ? (uint8_t *)SDL_MapGPUTransferBuffer(g_dev, tb, false) : NULL;
        if (!map) { if (tb) SDL_ReleaseGPUTransferBuffer(g_dev, tb); continue; }
        for (l = 0; l < t->levels; l++) { memcpy(map + off, t->data[l], t->size[l]); off += t->size[l]; }
        SDL_UnmapGPUTransferBuffer(g_dev, tb);
        if (!cp) cp = SDL_BeginGPUCopyPass(cmd);
        for (off = 0, l = 0; l < t->levels; l++) {
            SDL_GPUTextureTransferInfo src; SDL_GPUTextureRegion dst;
            UINT lw = t->w >> l ? t->w >> l : 1, lh = t->h >> l ? t->h >> l : 1;
            memset(&src, 0, sizeof src); memset(&dst, 0, sizeof dst);
            src.transfer_buffer = tb; src.offset = off; src.pixels_per_row = lw; src.rows_per_layer = lh;
            dst.texture = t->gpu; dst.mip_level = l; dst.w = lw; dst.h = lh; dst.d = 1;
            SDL_UploadToGPUTexture(cp, &src, &dst, false);
            off += t->size[l];
        }
        SDL_ReleaseGPUTransferBuffer(g_dev, tb);
        t->dirty = 0;
    }
    if (cp) SDL_EndGPUCopyPass(cp);
}

static int bind_samplers(SDL_GPURenderPass *pass, const Op *o)
{
    SDL_GPUTextureSamplerBinding b[4]; int st, n = (o->psh && o->key.ps && psh_shader(o->key.ps)) ? 4 : 1;
    for (st = 0; st < n; st++) {
        b[st].texture = (o->tex[st] && o->tex[st]->gpu) ? o->tex[st]->gpu : g_white;
        b[st].sampler = sampler_for(&o->skey[st]);
        if (!b[st].texture || !b[st].sampler) return 0;
    }
    SDL_BindGPUFragmentSamplers(pass, 0, b, (Uint32)n);
    return 1;
}

/* ---- combiner shaders: the translator hands over generated MSL per state key ---------------------------------- */

typedef struct { uint64_t key; SDL_GPUShader *s; int used; } PshEnt;
#define PSH_SLOTS 16384
static PshEnt *g_psh; static unsigned g_npsh, g_psh_fail;

static PshEnt *psh_find(uint64_t key, int insert)
{
    unsigned h = (unsigned)(key ^ (key >> 29)) & (PSH_SLOTS - 1), n;
    if (!g_psh) { g_psh = (PshEnt *)calloc(PSH_SLOTS, sizeof *g_psh); if (!g_psh) return NULL; }
    for (n = 0; n < PSH_SLOTS; n++, h = (h + 1) & (PSH_SLOTS - 1)) {
        if (g_psh[h].used && g_psh[h].key == key) return &g_psh[h];
        if (!g_psh[h].used) { if (!insert) return NULL; g_psh[h].used = 1; g_psh[h].key = key; return &g_psh[h]; }
    }
    return NULL;
}
static SDL_GPUShader *psh_shader(uint64_t key) { PshEnt *e = psh_find(key, 0); return e ? e->s : NULL; }
static int pipeline_has_psh(const PKey *k) { return k->ps && psh_shader(k->ps) != NULL; }

int d3d8_nv2a_has_ps(unsigned long long key) { return key == 0 || psh_find(key, 0) != NULL; }
int d3d8_nv2a_add_ps(unsigned long long key, const char *msl, int len)
{
    PshEnt *e = psh_find(key, 1);
    SDL_GPUShaderCreateInfo ci;
    if (!e) return 0;
    if (!g_dev) return 1;
    memset(&ci, 0, sizeof ci);
    ci.code = (const Uint8 *)msl; ci.code_size = (size_t)len + 1;
    ci.entrypoint = "fs_psh"; ci.format = SDL_GPU_SHADERFORMAT_MSL; ci.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    ci.num_uniform_buffers = 1; ci.num_samplers = 4;
    e->s = SDL_CreateGPUShader(g_dev, &ci);
    g_npsh++;
    if (!e->s) {                       /* the draw falls back to the plain vertex-colour shader */
        if (g_psh_fail++ < 4) {
            fprintf(stderr, "[GPU] combiner shader %016llX failed: %s\n", key, SDL_GetError());
            if (getenv("OT_GPU_PSH_DUMP_FAILED")) fprintf(stderr, "----\n%.*s\n----\n", len, msl);
        }
    }
    return 1;
}

static HRESULT __stdcall dev_settexture(IDirect3DDevice8 *s, DWORD st, IDirect3DBaseTexture8 *t) { (void)s; if (st < 4) g_bound[st] = t; return 0; }
static HRESULT __stdcall dev_gettexture(IDirect3DDevice8 *s, DWORD st, IDirect3DBaseTexture8 **t) { (void)s; if (t) *t = st < 4 ? g_bound[st] : NULL; return 0; }
static HRESULT __stdcall dev_setrs(IDirect3DDevice8 *s, D3DRENDERSTATETYPE st, DWORD v) { (void)s; if ((unsigned)st < 512) g_rs[st] = v; return 0; }
static HRESULT __stdcall dev_getrs(IDirect3DDevice8 *s, D3DRENDERSTATETYPE st, DWORD *v) { (void)s; *v = (unsigned)st < 512 ? g_rs[st] : 0; return 0; }
static HRESULT __stdcall dev_settss(IDirect3DDevice8 *s, DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD v) { (void)s; if (stage < 4 && (unsigned)t < 64) g_tss[stage][t] = v; return 0; }
static HRESULT __stdcall dev_gettss(IDirect3DDevice8 *s, DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD *v) { (void)s; *v = (stage < 4 && (unsigned)t < 64) ? g_tss[stage][t] : 0; return 0; }
static HRESULT __stdcall dev_scene(IDirect3DDevice8 *s) { (void)s; return 0; }
static HRESULT __stdcall dev_setvs(IDirect3DDevice8 *s, DWORD h) { (void)s; g_fvf = h; return 0; }
static ULONG __stdcall dev_ref(IDirect3DDevice8 *s) { (void)s; return 1; }
static HRESULT __stdcall dev_drawup(IDirect3DDevice8 *s, D3DPRIMITIVETYPE prim, UINT pc, const void *v, UINT stride)
{
    (void)s;
    /* fixed-function pre-transformed vertices (XYZRHW | DIFFUSE | TEX1): x y z rhw, colour, u v */
    record_draw((int)prim, prim_verts((int)prim, pc), v, stride, 1, 0, 0, NULL, 0);
    return 0;
}
static HRESULT __stdcall dev_clear(IDirect3DDevice8 *s, DWORD n, const D3DRECT *r, DWORD fl, D3DCOLOR c, float z, DWORD st)
{
    (void)s; (void)n; (void)r;
    clear_op(fl, c, z, st, 0, 0, TITLE_W - 1, TITLE_H - 1);
    return 0;
}
static HRESULT __stdcall dev_present(IDirect3DDevice8 *s, const RECT *a, const RECT *b, HWND w, void *d)
{
    (void)s; (void)a; (void)b; (void)w; (void)d;
    d3d8_PresentFrame();
    return 0;
}

static IDirect3DDevice8Vtbl g_devvt;
static IDirect3DDevice8 g_device = { &g_devvt };
static int g_inited;

static void gpu_init_tables(void)
{
    static void *const stub[128] = { (void *)u0, (void *)u1, (void *)u2, (void *)u3, (void *)u4, (void *)u5, (void *)u6, (void *)u7, (void *)u8, (void *)u9, (void *)u10, (void *)u11, (void *)u12, (void *)u13, (void *)u14, (void *)u15, (void *)u16, (void *)u17, (void *)u18, (void *)u19, (void *)u20, (void *)u21, (void *)u22, (void *)u23, (void *)u24, (void *)u25, (void *)u26, (void *)u27, (void *)u28, (void *)u29, (void *)u30, (void *)u31, (void *)u32, (void *)u33, (void *)u34, (void *)u35, (void *)u36, (void *)u37, (void *)u38, (void *)u39, (void *)u40, (void *)u41, (void *)u42, (void *)u43, (void *)u44, (void *)u45, (void *)u46, (void *)u47, (void *)u48, (void *)u49, (void *)u50, (void *)u51, (void *)u52, (void *)u53, (void *)u54, (void *)u55, (void *)u56, (void *)u57, (void *)u58, (void *)u59, (void *)u60, (void *)u61, (void *)u62, (void *)u63, (void *)u64, (void *)u65, (void *)u66, (void *)u67, (void *)u68, (void *)u69, (void *)u70, (void *)u71, (void *)u72, (void *)u73, (void *)u74, (void *)u75, (void *)u76, (void *)u77, (void *)u78, (void *)u79, (void *)u80, (void *)u81, (void *)u82, (void *)u83, (void *)u84, (void *)u85, (void *)u86, (void *)u87, (void *)u88, (void *)u89, (void *)u90, (void *)u91, (void *)u92, (void *)u93, (void *)u94, (void *)u95, (void *)u96, (void *)u97, (void *)u98, (void *)u99, (void *)u100, (void *)u101, (void *)u102, (void *)u103, (void *)u104, (void *)u105, (void *)u106, (void *)u107, (void *)u108, (void *)u109, (void *)u110, (void *)u111, (void *)u112, (void *)u113, (void *)u114, (void *)u115, (void *)u116, (void *)u117, (void *)u118, (void *)u119, (void *)u120, (void *)u121, (void *)u122, (void *)u123, (void *)u124, (void *)u125, (void *)u126, (void *)u127 };
    void **p = (void **)&g_devvt; size_t i, n = sizeof g_devvt / sizeof(void *);
    void **t = (void **)&g_texvt; size_t j, m = sizeof g_texvt / sizeof(void *);
    if (g_inited) return;
    g_inited = 1;
    for (i = 0; i < n && i < 128; i++) p[i] = stub[i];
    for (j = 0; j < m && j < 128; j++) t[j] = stub[j];
    g_devvt.AddRef = (void *)dev_ref; g_devvt.Release = (void *)dev_ref;
    g_devvt.SetRenderState = dev_setrs; g_devvt.GetRenderState = dev_getrs;
    g_devvt.SetTextureStageState = dev_settss; g_devvt.GetTextureStageState = dev_gettss;
    g_devvt.SetTexture = dev_settexture; g_devvt.GetTexture = dev_gettexture;
    g_devvt.BeginScene = dev_scene; g_devvt.EndScene = dev_scene;
    g_devvt.SetVertexShader = dev_setvs; g_devvt.DrawPrimitiveUP = dev_drawup;
    g_devvt.CreateTexture = dev_createtexture; g_devvt.Clear = dev_clear; g_devvt.Present = dev_present;
    g_texvt.AddRef = tex_addref; g_texvt.Release = tex_release; g_texvt.GetLevelCount = tex_levelcount;
    g_texvt.LockRect = tex_lock; g_texvt.UnlockRect = tex_unlock; g_texvt.GetLevelDesc = tex_leveldesc;
    state_defaults();
    g_ops = (Op *)calloc(MAX_OPS, sizeof(Op));
    g_arena = (uint8_t *)malloc(ARENA_MAX);
    d3d8_present_hook = present_hook;
}

static HRESULT __stdcall d3d_createdevice(IDirect3D8 *self, UINT adapter, DWORD type, HWND wnd, DWORD flags, D3DPRESENT_PARAMETERS *pp, IDirect3DDevice8 **out)
{
    (void)self; (void)adapter; (void)type; (void)wnd; (void)flags; (void)pp;
    gpu_init_tables();
    *out = &g_device;
    return 0;
}
IDirect3D8 *xbox_Direct3DCreate8(UINT sdk)
{
    static IDirect3D8Vtbl vt; static IDirect3D8 obj;
    (void)sdk;
    memset(&vt, 0, sizeof vt); vt.CreateDevice = d3d_createdevice; obj.lpVtbl = &vt;
    return &obj;
}
IDirect3DDevice8 *xbox_GetD3DDevice(void) { return g_inited ? &g_device : NULL; }

/* ================================================================= */
/* helpers the translator asks the host layer for                     */
/* ================================================================= */

unsigned d3d8_GetBackbufferWidth(void)  { return TITLE_W; }
unsigned d3d8_GetBackbufferHeight(void) { return TITLE_H; }
void   d3d8_GetGuestScale(float *sx, float *sy)    /* scene-target pixels per title pixel */
{
    unsigned w, h; d3d8_host_target_size(&w, &h);
    if (sx) *sx = (float)w / (float)TITLE_W;
    if (sy) *sy = (float)h / (float)TITLE_H;
}
void   d3d8_PlagShot(void) {}
unsigned d3d8_DebugPeekScene(float fx, float fy) { (void)fx; (void)fy; return 0; }
IDirect3DTexture8 *d3d8_PrevFrameTexture(void) { return NULL; }
int  d3d8_SoftShadowOn(void) { return 0; }
int d3d8_SoftShadowDraw(float shade, unsigned cmp, unsigned ref, unsigned rmask,
                        float fx0, float fy0, float fx1, float fy1)
{ (void)shade; (void)cmp; (void)ref; (void)rmask; (void)fx0; (void)fy0; (void)fx1; (void)fy1; return 0; }
int  d3d8_pump_cache_on(void) { return 0; }

/* pixel shaders: Slice 3. The translator generates the combiner source; the default colour shader stands in for all of it. */
/* vertex programs run on the CPU (no GPU vertex-program path) */
int  d3d8_nv2a_vsh_ready(const void *d) { (void)d; return 0; }
int  d3d8_nv2a_draw_program_gpu(const void *d) { (void)d; return 0; }
int  d3d8_points_gpu_on(void) { return 0; }
int  d3d8_points_check_on(void) { return 0; }
void d3d8_nv2a_points_expect(const void *v, unsigned n, unsigned stride) { (void)v; (void)n; (void)stride; }

/* occlusion: each query is a fake handle; reports complete at once with a visible count */
static int g_occ_next = 1;
int   d3d8_OcclusionBegin(void) { return g_occ_next++ & 0x7FFF; }
void  d3d8_OcclusionEnd(int h) { (void)h; }
void  d3d8_OcclusionRelease(int h) { (void)h; }

/* profiling and HD-texture hooks the translator links against; all off */
int g_gpuprof_on;
volatile double g_tex_upload_ms;
volatile long g_tex_uploads_log;
void gpuprof_set_phase(int phase, int group) { (void)phase; (void)group; }
int  hdtex_on(void) { return 0; }
uint64_t hdtex_budget(void) { return 0; }
HdJob *hdtex_request(const uint8_t *l0, uint32_t n0, uint32_t w, uint32_t h) { (void)l0; (void)n0; (void)w; (void)h; return NULL; }
int  hdtex_collect(HdJob *j, IDirect3DTexture8 **t, uint32_t *b) { (void)j; (void)t; (void)b; return 0; }
void hdtex_cancel(HdJob *j) { (void)j; }
void hdtex_note_evicted(uint32_t b, int over) { (void)b; (void)over; }
