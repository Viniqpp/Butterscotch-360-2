#include <xtl.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <xgraphics.h>
#include <xcompress.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

// Core headers — compiled as C++ alongside the .c files (via /TP flag)
#include "utils.h"
#include "text_utils.h"
#include "d3d9_renderer.h"
#include "d3d9_transform.h"
#include "runner.h"
#include "vm_builtins.h"
#include "stb_ds.h"
#include "image_decoder.h"


#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// ===[ Vertex Format ]===
// Uses FLOAT4 position (pre-transformed screen coords, z=0, w=1)
// and FLOAT4 color to avoid D3DCOLOR endianness issues on Xbox 360.
struct SpriteVertex {
    float x, y, z, w; // position (screen-space, z=0, w=1)
    float u, v;        // texcoord
    float r, g, b, a;  // color as floats
};

// ===[ HLSL Shader Source ]===
// Vertex shader: simple pass-through for pre-transformed screen-space vertices.
// Position is already in screen pixels with z=0, w=1.
// With D3DRS_VIEWPORTENABLE=FALSE, the GPU uses these directly.
static const char* g_vsSource =
    "struct VS_IN  { float4 Pos : POSITION; float2 Tex : TEXCOORD0; float4 Col : TEXCOORD1; };\n"
    "struct VS_OUT { float4 Pos : POSITION; float2 Tex : TEXCOORD0; float4 Col : TEXCOORD1; };\n"
    "VS_OUT main(VS_IN i) {\n"
    "  VS_OUT o;\n"
    "  o.Pos = i.Pos;\n"
    "  o.Tex = i.Tex;\n"
    "  o.Col = i.Col;\n"
    "  return o;\n"
    "}\n";

// gpu_set_fog: c223 = fog colour, .a = 1 while fog is on (RGB replaced, alpha kept).
// c223 sits above the registers translated GameMaker shaders use, so it survives shader switches.
#define D3D9_FOG_REGISTER 223
static const char* g_psSource =
    "sampler2D s0 : register(s0);\n"
    "float4 fog : register(c223);\n"
    "struct PS_IN { float2 Tex : TEXCOORD0; float4 Col : TEXCOORD1; };\n"
    "float4 main(PS_IN i) : COLOR0 {\n"
    "  float4 c = tex2D(s0, i.Tex) * i.Col;\n"
    "  c.rgb = lerp(c.rgb, fog.rgb, fog.a);\n"
    "  return c;\n"
    "}\n";

// ===[ Helpers ]===

static inline void setVertex(SpriteVertex* sv, float px, float py, float tu, float tv,
                              float cr, float cg, float cb, float ca) {
    sv->x = px; sv->y = py; sv->z = 0.0f; sv->w = 1.0f;
    sv->u = tu; sv->v = tv;
    sv->r = cr; sv->g = cg; sv->b = cb; sv->a = ca;
}

static inline IDirect3DDevice9* Dev(D3D9Renderer* r) {
    return (IDirect3DDevice9*)r->pd3dDevice;
}

// Convert Butterscotch BGR color + alpha to float RGBA
static inline void bgrToFloatColor(uint32_t bgr, float alpha, float* outR, float* outG, float* outB, float* outA) {
    *outR = (float)(bgr & 0xFF) / 255.0f;
    *outG = (float)((bgr >> 8) & 0xFF) / 255.0f;
    *outB = (float)((bgr >> 16) & 0xFF) / 255.0f;
    *outA = alpha;
}

// ===[ Batch Flush ]===

typedef struct {
    void* rt;  // IDirect3DSurface9* (EDRAM)
    void* tex; // IDirect3DTexture9* the target is resolved into
    int32_t w, h, allocW, allocH;
    bool exists;
    bool hasContent; // tex holds the last resolved image
} D3D9Surface;

// surface_get_texture handles live above the TPAG handle range (TPAG index + 1).
#define D3D9_SURFACE_HANDLE 0x40000000u

static D3D9Surface* d3d9Surface(D3D9Renderer* dr, int32_t id) {
    D3D9Surface* s = (D3D9Surface*)dr->surfaces;
    return id >= 0 && id < (int32_t)arrlen(s) && s[id].exists ? &s[id] : NULL;
}

// Batch texture index: >= 0 texture page, -1 white, <= -2 surface (-2 - id).
static void* d3d9TextureForIndex(D3D9Renderer* dr, int32_t index) {
    if (index >= 0) return (uint32_t)index < dr->textureCount ? dr->textures[index] : NULL;
    if (index == -1) return dr->whiteTexture;
    D3D9Surface* s = d3d9Surface(dr, -2 - index);
    return s && s->hasContent ? s->tex : dr->whiteTexture;
}

static void flushBatch(D3D9Renderer* dr) {
    if (!dr) return;
    if (!dr->sceneActive) { dr->quadCount = 0; return; }
    if (!dr->vertexData || !dr->triangleData || !dr->pd3dDevice || dr->quadCount == 0) return;

    IDirect3DDevice9* dev = Dev(dr);
    void* chosen = d3d9TextureForIndex(dr, dr->currentTextureIndex);
    IDirect3DBaseTexture9* texture = (IDirect3DBaseTexture9*)(chosen ? chosen : dr->whiteTexture);

    HRESULT hr = dev->SetTexture(0, texture);
    if (SUCCEEDED(hr)) {
        // Use the standard D3D triangle-list topology rather than relying on
        // the non-portable QUADLIST primitive type.
        SpriteVertex* source = (SpriteVertex*)dr->vertexData;
        SpriteVertex* triangles = (SpriteVertex*)dr->triangleData;
        for (int32_t i = 0; i < dr->quadCount; ++i) {
            SpriteVertex* q = source + i * D3D9_VERTS_PER_QUAD;
            SpriteVertex* t = triangles + i * 6;
            t[0] = q[0]; t[1] = q[1]; t[2] = q[2];
            t[3] = q[0]; t[4] = q[2]; t[5] = q[3];
        }
        hr = dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, dr->quadCount * 2,
                                  triangles, sizeof(SpriteVertex));
    }
    if (FAILED(hr)) logInfo("BS: D3D9 quad batch flush failed: hr=0x%08X\n", (unsigned)hr);
    dr->quadCount = 0;
}

static void ensureTexture(D3D9Renderer* dr, int32_t textureIndex) {
    if (dr->currentTextureIndex != textureIndex) {
        flushBatch(dr);
        dr->currentTextureIndex = textureIndex;
    }
}

// Public texture handles are TPAG handles (index + 1), not raw TXTR page IDs.
static void* d3d9Page(D3D9Renderer* dr, int32_t page);
static void* d3d9PageEx(D3D9Renderer* dr, int32_t page, bool wait);

static int32_t d3d9TextureHandleToPage(D3D9Renderer* dr, uint32_t textureHandle) {
    if (textureHandle >= D3D9_SURFACE_HANDLE) { // surface_get_texture
        int32_t id = (int32_t)(textureHandle - D3D9_SURFACE_HANDLE);
        return d3d9Surface(dr, id) ? -2 - id : -1;
    }
    if (!dr || !dr->base.dataWin || textureHandle == 0) return -1;
    DataWin* dw = dr->base.dataWin;
    uint32_t tpagIndex = textureHandle - 1;
    if (tpagIndex >= dw->tpag.count) return -1;
    int32_t page = dw->tpag.items[tpagIndex].texturePageId;
    if (page < 0 || (uint32_t)page >= dr->textureCount ||
        !d3d9PageEx(dr, page, true)) return -1;
    return page;
}

static SpriteVertex* allocQuad(D3D9Renderer* dr) {
    if (dr->quadCount >= D3D9_MAX_QUADS) {
        flushBatch(dr);
    }
    SpriteVertex* v = (SpriteVertex*)(dr->vertexData + dr->quadCount * D3D9_VERTS_PER_QUAD * sizeof(SpriteVertex));
    dr->quadCount++;
    return v;
}

// Transform a world-space point to back-buffer pixels through the active GML matrices.
// shortcut: ignores clip-space w, so d3d perspective projections draw as orthographic; add a w-divide if a 3D game needs it.
// Full homogeneous transform: d3d_set_projection / perspective cameras give w != 1.
// shortcut: the divide is done on the CPU, so textures under perspective are
// interpolated affinely and points behind the camera are not clipped.
static inline void transformPoint3(D3D9Renderer* dr, float x, float y, float z, float* outX, float* outY) {
    const float* m = dr->screenTransform.m;
    float w = m[3] * x + m[7] * y + m[11] * z + m[15];
    float sx = m[0] * x + m[4] * y + m[8] * z + m[12];
    float sy = m[1] * x + m[5] * y + m[9] * z + m[13];
    if (w != 1.0f && fabsf(w) > 1e-6f) { sx /= w; sy /= w; }
    *outX = sx;
    *outY = sy;
}

static inline void transformPoint(D3D9Renderer* dr, float inX, float inY, float* outX, float* outY) {
    transformPoint3(dr, inX, inY, 0.0f, outX, outY);
}

// screenTransform = NDC->viewport pixels * WVP; see d3d9_transform.h for the Y convention.
static void d3d9RebuildTransform(D3D9Renderer* dr) {
    D3D9Transform_build(&dr->screenTransform, &dr->base.gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION],
                        dr->viewportX, dr->viewportY, dr->viewportW, dr->viewportH, dr->hostTargetFlipY);
}

// Viewport given in application-surface (game) coordinates; the application
// surface aliases the letterboxed region of the back buffer on this backend.
static void d3d9SetGameViewport(D3D9Renderer* dr, float x, float y, float w, float h) {
    dr->viewportX = x * dr->renderScale + dr->renderOffsetX;
    dr->viewportY = y * dr->renderScale + dr->renderOffsetY;
    dr->viewportW = w * dr->renderScale;
    dr->viewportH = h * dr->renderScale;
}

static void d3d9SetProjection(D3D9Renderer* dr, const Matrix4f* view, const Matrix4f* projection) {
    flushBatch(dr);
    Renderer_applyProjection(&dr->base, view, projection);
    d3d9RebuildTransform(dr);
}

static bool d3d9BindTarget(D3D9Renderer* dr, int32_t id);
static void d3d9ReleaseSurfaceObjects(D3D9Renderer* dr, D3D9Surface* s);
static void d3d9BlitTexture(D3D9Renderer* dr, void* tex, int32_t allocW, int32_t allocH,
                            int32_t sx, int32_t sy, int32_t w, int32_t h, float dx, float dy);
static int32_t d3d9CreateSurface(Renderer* r, int32_t w, int32_t h);
static void d3d9FreeSurfaces(D3D9Renderer* dr);
static void d3d9LoadUserShaders(D3D9Renderer* dr, DataWin* dw);
static void d3d9ReleaseUserShaders(D3D9Renderer* dr);

// Uniform scale + centring that fits the game into the back buffer.
static void d3d9ComputeLetterbox(D3D9Renderer* dr) {
    float scaleX = (float)dr->windowW / (float)dr->gameW;
    float scaleY = (float)dr->windowH / (float)dr->gameH;
    dr->renderScale = (scaleX < scaleY) ? scaleX : scaleY;
    dr->renderOffsetX = ((float)dr->windowW - (float)dr->gameW * dr->renderScale) * 0.5f;
    dr->renderOffsetY = ((float)dr->windowH - (float)dr->gameH * dr->renderScale) * 0.5f;
}

// Draws `texture` as a quad with screen-space corners x[i],y[i] (TL, TR, BR, BL).
static void d3d9DrawTextureQuad(D3D9Renderer* dr, void* texture, const float* x, const float* y,
                                float u0, float v0, float u1, float v1, uint32_t color, float alpha) {
    if (!dr->sceneActive || !texture) return;
    flushBatch(dr);
    float r, g, b, a;
    bgrToFloatColor(color, alpha, &r, &g, &b, &a);
    SpriteVertex q[4];
    setVertex(&q[0], x[0], y[0], u0, v0, r, g, b, a);
    setVertex(&q[1], x[1], y[1], u1, v0, r, g, b, a);
    setVertex(&q[2], x[2], y[2], u1, v1, r, g, b, a);
    setVertex(&q[3], x[3], y[3], u0, v1, r, g, b, a);
    SpriteVertex tri[6] = { q[0], q[1], q[2], q[0], q[2], q[3] };
    IDirect3DDevice9* dev = Dev(dr);
    dev->SetTexture(0, (IDirect3DBaseTexture9*)texture);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, tri, sizeof(SpriteVertex));
    dr->currentTextureIndex = -1; // next batch rebinds its own texture
}

// Default ortho over the whole application surface, as a freshly bound target has.
static void d3d9ResetToGameArea(D3D9Renderer* dr) {
    Matrix4f view, projection;
    Matrix4f_identity(&view);
    Matrix4f_viewProjection(&projection, 0.0f, 0.0f, (float)dr->gameW, (float)dr->gameH, 0.0f);
    dr->hostTargetFlipY = false;
    d3d9SetGameViewport(dr, 0.0f, 0.0f, (float)dr->gameW, (float)dr->gameH);
    d3d9SetProjection(dr, &view, &projection);
}

// Mirrors upstream GLCommon_beginGUI/setGuiProjection: the GUI camera is
// published to the runner because core d3d_* code keys off GUI_CAMERA.
static void d3d9ApplyGuiCamera(D3D9Renderer* dr, int32_t guiW, int32_t guiH) {
    Renderer* r = &dr->base;
    Matrix4f view, projection;
    D3D9Transform_guiCamera(&view, &projection, (float)guiW, (float)guiH);
    r->cameraCurrent = GUI_CAMERA;
    if (r->runner) {
        GMLCamera* camera = &r->runner->guiCamera;
        camera->allocated = true;
        camera->viewX = 0.0;
        camera->viewY = 0.0;
        camera->viewWidth = guiW;
        camera->viewHeight = guiH;
        camera->borderX = 0;
        camera->borderY = 0;
        camera->speedX = 0;
        camera->speedY = 0;
        camera->objectId = -1;
        camera->viewAngle = 0;
        camera->viewMatrix = view;
        camera->projectionMatrix = projection;
    }
    d3d9SetProjection(dr, &view, &projection);
}

// ===[ Vtable Implementations ]===

// Re-encodes a resident full-colour page as DXT5 in place (a quarter of the memory).
// Used when every page in memory is needed by the current scene.
static bool d3d9SqueezePage(D3D9Renderer* dr, uint32_t p) {
    int w = dr->textureWidths[p], h = dr->textureHeights[p];
    IDirect3DTexture9* old = (IDirect3DTexture9*)dr->textures[p];
    IDirect3DTexture9* tex = NULL;
    D3DLOCKED_RECT src, dst;
    if (w % 4 || h % 4 ||
        FAILED(Dev(dr)->CreateTexture(w, h, 1, 0, D3DFMT_LIN_DXT5, D3DPOOL_DEFAULT, &tex, NULL)) || !tex)
        return false;
    bool ok = false;
    if (SUCCEEDED(old->LockRect(0, &src, NULL, D3DLOCK_READONLY))) {
        if (SUCCEEDED(tex->LockRect(0, &dst, NULL, 0))) {
            ok = SUCCEEDED(XGCompressSurface(dst.pBits, dst.Pitch, w, h, D3DFMT_LIN_DXT5, NULL,
                                             src.pBits, src.Pitch, D3DFMT_LIN_A8R8G8B8, NULL, 0, 0.0f));
            tex->UnlockRect(0);
        }
        old->UnlockRect(0);
    }
    if (!ok) { tex->Release(); return false; }
    old->Release(); // caller already waited for the GPU
    dr->textures[p] = tex;
    dr->textureBytesAllocated -= dr->pageBytes[p];
    dr->pageBytes[p] = (uint32_t)dst.Pitch * (uint32_t)(h / 4);
    dr->textureBytesAllocated += dr->pageBytes[p];
    return true;
}

// Frees least-recently-used pages not drawn this frame until `bytes` fit under `limit`;
// when every page is in use, compresses full-colour ones in place instead.
static bool d3d9MakeRoom(D3D9Renderer* dr, uint32_t bytes, uint32_t limit) {
    bool idled = false;
    while (dr->textureBytesAllocated + bytes > limit) {
        int32_t victim = -1, squeeze = -1;
        for (uint32_t p = 0; p < dr->originalTexturePageCount; p++) {
            if (!dr->textures[p]) continue;
            if (dr->pageLastUse[p] != dr->frameCounter &&
                (victim < 0 || dr->pageLastUse[p] < dr->pageLastUse[victim])) victim = (int32_t)p;
            if (dr->pageBytes[p] >= (uint32_t)dr->textureWidths[p] * (uint32_t)dr->textureHeights[p] * 4u &&
                (squeeze < 0 || dr->pageBytes[p] > dr->pageBytes[squeeze])) squeeze = (int32_t)p;
        }
        if (victim < 0 && squeeze < 0) return false;
        if (!idled) { // Xenon does not track resource lifetime: let the GPU finish first
            flushBatch(dr);
            Dev(dr)->BlockUntilIdle();
            idled = true;
        }
        if (victim < 0) {
            if (!d3d9SqueezePage(dr, (uint32_t)squeeze)) return false;
            logInfo("D3D9: compressed resident page %d to make room\n", squeeze);
            continue;
        }
        ((IDirect3DTexture9*)dr->textures[victim])->Release();
        dr->textures[victim] = NULL;
        dr->textureBytesAllocated -= dr->pageBytes[victim];
        dr->pageBytes[victim] = 0;
    }
    return true;
}

// ===[ Texture pages ]===
// Pages are decoded (PNG / QOI / BZip2+QOI) on worker threads into their final
// upload format, so the frame never waits for a decode: a page that is not
// ready yet is skipped for a frame or two. Decoded pages are also kept in a
// lossless LZX cache on disk (game:\texcache_<size>\ or, when the game folder is
// read-only as in Xenia, the cache: utility drive), which loads several times
// faster than decoding. Idle workers fill that cache in the background.

#define D3D9_LOADER_THREADS 3 // Xenon hardware threads 2-4; 0 runs the game, 1 and 5 stay free for audio/system
#define D3D9_PREFETCH 0x80000000u // queue entry flag: only build the cache for this page
#define D3D9_CACHE_MAGIC 0x42534331u // "BSC1"

char g_d3d9DataWinPath[256];
// CONFIG.JSN "traceSurfaces": N logs the next N surface operations (debugging aid).
int g_d3d9TraceSurfaces;
char g_d3d9TraceRoom[64]; // "traceRoom": only trace while this room is current
static bool d3d9TraceOn(D3D9Renderer* dr) {
    if (g_d3d9TraceSurfaces <= 0) return false;
    if (g_d3d9TraceRoom[0]) {
        Runner* runner = dr->base.runner;
        if (!runner || !runner->currentRoom || !runner->currentRoom->name ||
            strcmp(runner->currentRoom->name, g_d3d9TraceRoom)) return false;
    }
    g_d3d9TraceSurfaces--;
    return true;
}
#define SURF_TRACE(...) do { if (d3d9TraceOn(dr)) logInfo(__VA_ARGS__); } while (0)

enum { D3D9_PAGE_IDLE, D3D9_PAGE_QUEUED, D3D9_PAGE_READY, D3D9_PAGE_BAD };

typedef struct {
    CRITICAL_SECTION lock;
    HANDLE wake; // semaphore: one count per queue entry
    HANDLE threads[D3D9_LOADER_THREADS];
    volatile bool quit;
    bool qoiTitle; // GMS 2022.5+: BZip2+QOI blobs
    DataWin* dw;
    char cacheDir[256]; // empty: no writable cache
    uint32_t* queue; // ring: on-demand pages at the head, prefetch entries at the tail
    uint32_t head, count, cap;
    uint8_t* state;
    uint8_t* dxt;    // requested format per page
    uint8_t** out;   // decoded page in upload format
    int32_t* outW;
    int32_t* outH;
} D3D9Loader;

// Turns 0xAARRGGBB words into DXT5 blocks (w*h bytes); NULL if it cannot.
// Transparent texels first take the last visible colour in their row, or DXT
// colour blocks bleed black into sprite edges. Modifies `words`.
static uint8_t* d3d9ToDxt(uint8_t* words, int w, int h) {
    if (w % 4 || h % 4) return NULL;
    DWORD* px = (DWORD*)words;
    for (int y = 0; y < h; y++) {
        DWORD carry = 0;
        for (int x = 0; x < w; x++) {
            DWORD* p = px + (size_t)y * w + x;
            if (*p >> 24) carry = *p; else *p = carry & 0x00FFFFFF;
        }
    }
    uint8_t* blocks = (uint8_t*)malloc((size_t)w * h);
    if (blocks && FAILED(XGCompressSurface(blocks, (UINT)w * 4, w, h, D3DFMT_LIN_DXT5, NULL,
                                           words, (UINT)w * 4, D3DFMT_LIN_A8R8G8B8, NULL, 0, 0.0f))) {
        free(blocks);
        blocks = NULL;
    }
    return blocks;
}

// Decodes a TXTR blob into 0xAARRGGBB words (any thread).
static uint8_t* d3d9DecodePage(const uint8_t* blob, uint32_t size, bool qoiTitle, int* outW, int* outH) {
    int w = 0, h = 0;
    uint8_t* rgba = ImageDecoder_decodeToRgba((uint8_t*)blob, size, qoiTitle, &w, &h);
    if (!rgba) return NULL;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) { free(rgba); return NULL; }
    *outW = w; *outH = h;
    DWORD* words = (DWORD*)rgba; // in place: RGBA bytes -> 0xAARRGGBB words
    for (size_t k = 0, n = (size_t)w * h; k < n; k++) {
        uint8_t* p = rgba + k * 4;
        words[k] = p[3] ? D3DCOLOR_ARGB(p[3], p[0], p[1], p[2]) : 0;
    }
    return rgba;
}

// GPU half (main thread): copies a decoded page into a new texture. Negative w = DXT5 blocks.
static bool d3d9UploadPage(D3D9Renderer* dr, uint32_t i, uint8_t* data, int w, int h) {
    bool dxt = w < 0;
    if (dxt) w = -w;
    uint32_t bytes = dxt ? (uint32_t)w * (uint32_t)h : (uint32_t)w * (uint32_t)h * 4u;
    // Full-size page does not fit beside everything drawn this frame: compress it.
    if (!dxt && !d3d9MakeRoom(dr, bytes, dr->maxTextureBytes)) {
        uint8_t* blocks = d3d9ToDxt(data, w, h);
        if (blocks) {
            bool ok = d3d9UploadPage(dr, i, blocks, -w, h);
            free(blocks);
            return ok;
        }
    }
    if (!d3d9MakeRoom(dr, bytes, dr->maxTextureBytes)) {
        logWarn("D3D9: no room for texture page %u (%u MB in use, all drawn this frame)\n",
                i, dr->textureBytesAllocated >> 20);
        return false;
    }
    IDirect3DTexture9* tex = NULL;
    D3DLOCKED_RECT lr;
    HRESULT hr = Dev(dr)->CreateTexture(w, h, 1, 0, dxt ? D3DFMT_LIN_DXT5 : D3DFMT_LIN_A8R8G8B8,
                                        D3DPOOL_DEFAULT, &tex, NULL);
    if (SUCCEEDED(hr) && tex) hr = tex->LockRect(0, &lr, NULL, 0);
    if (FAILED(hr) || !tex) {
        logWarn("D3D9: texture upload failed for page %u (hr=0x%08X)\n", i, (unsigned)hr);
        if (tex) tex->Release();
        return false;
    }
    uint32_t rows = dxt ? (uint32_t)h / 4u : (uint32_t)h, rowBytes = (uint32_t)w * 4u;
    for (uint32_t y = 0; y < rows; y++)
        memcpy((uint8_t*)lr.pBits + (size_t)y * lr.Pitch, data + (size_t)y * rowBytes, rowBytes);
    tex->UnlockRect(0);
    dr->textures[i] = tex;
    dr->textureWidths[i] = w;
    dr->textureHeights[i] = h;
    dr->pageBytes[i] = (uint32_t)lr.Pitch * rows;
    dr->textureBytesAllocated += dr->pageBytes[i];
    return true;
}

// DXT is lossy, so pages stay full quality while they fit in 3/4 of the budget;
// only frames that need more (e.g. Deltarune Ch5 battles) get compressed pages.
// "compressTextures": true compresses everything.
static bool d3d9WantDxt(D3D9Renderer* dr, Texture* t) {
    if (dr->compressTextures) return true;
    if (t->textureWidth <= 0 || t->textureHeight <= 0) return false; // size unknown before decode
    // Compress only when the page cannot fit uncompressed beside the pages the
    // current scene keeps drawing (used this frame or the last one): evicting
    // those would just reload them next frame.
    uint32_t pinned = 0;
    for (uint32_t p = 0; p < dr->originalTexturePageCount; p++)
        if (dr->textures[p] && dr->pageLastUse[p] + 1 >= dr->frameCounter) pinned += dr->pageBytes[p];
    // 3/4: the last quarter stays free for compressed pages once a scene outgrows the budget.
    return pinned + (uint32_t)t->textureWidth * (uint32_t)t->textureHeight * 4u > dr->maxTextureBytes / 4 * 3;
}

// Synchronous load, used when no worker threads could be started.
static bool d3d9LoadPage(D3D9Renderer* dr, uint32_t i) {
    DataWin* dw = dr->base.dataWin;
    Texture* t = &dw->txtr.textures[i];
    DataWin_loadTxtrIfNeeded(dw, i);
    if (!t->blobData || t->blobSize == 0 || t->blobSize > 32u * 1024u * 1024u) {
        logInfo("BS: invalid or oversized TXTR blob for page %u (size=%u)\n", i, t->blobSize);
        return false;
    }
    int w = 0, h = 0;
    uint8_t* data = d3d9DecodePage(t->blobData, t->blobSize,
                                   DataWin_isVersionAtLeast(dw, 2022, 5, 0, 0), &w, &h);
    if (dw->lazyLoadTextures) { free(t->blobData); t->blobData = NULL; }
    if (!data) { logWarn("BS: could not decode TXTR page %u\n", i); return false; }
    if (d3d9WantDxt(dr, t)) {
        uint8_t* blocks = d3d9ToDxt(data, w, h);
        if (blocks) { free(data); data = blocks; w = -w; }
    }
    bool ok = d3d9UploadPage(dr, i, data, w, h);
    free(data);
    return ok;
}

// ---- Disk cache: { magic, w, h, packed size } + LZX(0xAARRGGBB words) ----

static uint8_t* d3d9CacheRead(D3D9Loader* L, XMEMDECOMPRESSION_CONTEXT dctx, uint32_t page, int* w, int* h) {
    char path[300];
    sprintf(path, "%sp%u.bin", L->cacheDir, page);
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    DWORD hdr[4], got = 0;
    uint8_t *packed = NULL, *data = NULL;
    if (ReadFile(f, hdr, sizeof(hdr), &got, NULL) && got == sizeof(hdr) && hdr[0] == D3D9_CACHE_MAGIC &&
        hdr[1] - 1u < 8192u && hdr[2] - 1u < 8192u && hdr[3] <= 64u * 1024u * 1024u &&
        (packed = (uint8_t*)malloc(hdr[3])) != NULL &&
        ReadFile(f, packed, hdr[3], &got, NULL) && got == hdr[3]) {
        SIZE_T size = (SIZE_T)hdr[1] * hdr[2] * 4u;
        data = (uint8_t*)malloc(size);
        SIZE_T outSize = size;
        if (data && (FAILED(XMemDecompress(dctx, data, &outSize, packed, hdr[3])) || outSize != size)) {
            free(data);
            data = NULL;
        }
        *w = (int)hdr[1];
        *h = (int)hdr[2];
    }
    free(packed);
    CloseHandle(f);
    return data;
}

static void d3d9CacheWrite(D3D9Loader* L, XMEMCOMPRESSION_CONTEXT cctx, uint32_t page,
                           const uint8_t* data, int w, int h) {
    SIZE_T size = (SIZE_T)w * h * 4u, packedSize = size + size / 8 + 4096; // LZX worst case grows a little
    uint8_t* packed = (uint8_t*)malloc(packedSize);
    if (!packed) return;
    if (SUCCEEDED(XMemCompress(cctx, packed, &packedSize, data, size))) {
        char tmp[300], path[300];
        sprintf(tmp, "%sp%u.tmp", L->cacheDir, page);
        sprintf(path, "%sp%u.bin", L->cacheDir, page);
        HANDLE f = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD hdr[4] = { D3D9_CACHE_MAGIC, (DWORD)w, (DWORD)h, (DWORD)packedSize }, put = 0, put2 = 0;
            bool ok = WriteFile(f, hdr, sizeof(hdr), &put, NULL) && WriteFile(f, packed, (DWORD)packedSize, &put2, NULL) &&
                      put == sizeof(hdr) && put2 == packedSize;
            CloseHandle(f);
            // written under a temporary name so a crash never leaves a truncated .bin
            if (!ok || !MoveFileA(tmp, path)) DeleteFileA(tmp);
        }
    }
    free(packed);
}

static bool d3d9CacheExists(D3D9Loader* L, uint32_t page) {
    char path[300];
    sprintf(path, "%sp%u.bin", L->cacheDir, page);
    return GetFileAttributesA(path) != (DWORD)-1;
}

// Reads a page's compressed blob with the worker's own data.win handle (the
// runner's lazy-load FILE* belongs to the main thread).
static uint8_t* d3d9ReadBlobAsync(D3D9Loader* L, HANDLE file, uint32_t page, uint32_t* size) {
    Texture* t = &L->dw->txtr.textures[page];
    *size = t->blobSize;
    if (t->blobSize == 0 || t->blobSize > 32u * 1024u * 1024u) return NULL;
    uint8_t* blob = (uint8_t*)malloc(t->blobSize);
    if (!blob) return NULL;
    if (!L->dw->lazyLoadTextures && t->blobData) { // whole data.win already in memory: never freed
        memcpy(blob, t->blobData, t->blobSize);
        return blob;
    }
    OVERLAPPED at;
    memset(&at, 0, sizeof(at));
    at.Offset = t->blobOffset;
    DWORD got = 0;
    if (file == INVALID_HANDLE_VALUE || t->blobOffset == 0 ||
        (!ReadFile(file, blob, t->blobSize, &got, &at) && GetLastError() != ERROR_IO_PENDING) ||
        (!got && !GetOverlappedResult(file, &at, &got, TRUE)) || got != t->blobSize) {
        free(blob);
        return NULL;
    }
    return blob;
}

static DWORD WINAPI d3d9LoaderThread(LPVOID arg) {
    D3D9Loader* L = (D3D9Loader*)arg;
    HANDLE file = CreateFileA(g_d3d9DataWinPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    XMEMCOMPRESSION_CONTEXT cctx = NULL;
    XMEMDECOMPRESSION_CONTEXT dctx = NULL;
    if (L->cacheDir[0]) {
        XMemCreateCompressionContext(XMEMCODEC_LZX, NULL, 0, &cctx);
        XMemCreateDecompressionContext(XMEMCODEC_LZX, NULL, 0, &dctx);
    }
    for (;;) {
        WaitForSingleObject(L->wake, INFINITE);
        if (L->quit) break;
        EnterCriticalSection(&L->lock);
        uint32_t entry = L->queue[L->head];
        L->head = (L->head + 1) % L->cap;
        L->count--;
        uint32_t page = entry & ~D3D9_PREFETCH;
        bool prefetch = (entry & D3D9_PREFETCH) != 0;
        bool dxt = L->dxt[page] != 0;
        LeaveCriticalSection(&L->lock);
        if (prefetch && (!cctx || d3d9CacheExists(L, page))) continue;

        int w = 0, h = 0;
        uint8_t* data = dctx ? d3d9CacheRead(L, dctx, page, &w, &h) : NULL;
        if (!data) {
            uint32_t size = 0;
            uint8_t* blob = d3d9ReadBlobAsync(L, file, page, &size);
            if (blob) data = d3d9DecodePage(blob, size, L->qoiTitle, &w, &h);
            free(blob);
            if (data && cctx && !d3d9CacheExists(L, page)) d3d9CacheWrite(L, cctx, page, data, w, h);
        }
        if (prefetch) { free(data); continue; }
        if (data && dxt) {
            uint8_t* blocks = d3d9ToDxt(data, w, h);
            if (blocks) { free(data); data = blocks; w = -w; }
        }
        EnterCriticalSection(&L->lock);
        L->out[page] = data;
        L->outW[page] = w;
        L->outH[page] = h;
        L->state[page] = data ? D3D9_PAGE_READY : D3D9_PAGE_BAD;
        LeaveCriticalSection(&L->lock);
    }
    if (cctx) XMemDestroyCompressionContext(cctx);
    if (dctx) XMemDestroyDecompressionContext(dctx);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return 0;
}

// Must be called with L->lock held.
static void d3d9LoaderPush(D3D9Loader* L, uint32_t entry, bool front) {
    if (L->count == L->cap) return; // cannot happen: at most one on-demand and one prefetch entry per page
    if (front) {
        L->head = (L->head + L->cap - 1) % L->cap;
        L->queue[L->head] = entry;
    } else {
        L->queue[(L->head + L->count) % L->cap] = entry;
    }
    L->count++;
    ReleaseSemaphore(L->wake, 1, NULL);
}

// Picks a writable cache folder for this data.win: next to it, else the cache: drive.
static void d3d9PickCacheDir(D3D9Loader* L) {
    char dir[256];
    DWORD size = 0;
    HANDLE f = CreateFileA(g_d3d9DataWinPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    size = GetFileSize(f, NULL); // a different data.win (game update) gets a fresh cache
    CloseHandle(f);
    const char* slash = strrchr(g_d3d9DataWinPath, '\\');
    size_t dirLen = slash ? (size_t)(slash - g_d3d9DataWinPath + 1) : 0;
    if (dirLen + 24 >= sizeof(dir)) return;
    memcpy(dir, g_d3d9DataWinPath, dirLen);
    sprintf(dir + dirLen, "texcache_%08X\\", (unsigned)size);
    for (int attempt = 0; attempt < 2; attempt++) {
        char bare[256]; // Xenia rejects a trailing backslash on CreateDirectory
        strcpy(bare, dir);
        bare[strlen(bare) - 1] = 0;
        CreateDirectoryA(bare, NULL);
        char probe[300];
        sprintf(probe, "%sprobe.tmp", dir);
        HANDLE p = CreateFileA(probe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (p != INVALID_HANDLE_VALUE) {
            CloseHandle(p);
            DeleteFileA(probe);
            strcpy(L->cacheDir, dir);
            logInfo("D3D9: texture cache at %s\n", dir);
            return;
        }
        // Read-only game folder (Xenia): the title's utility drive. The name keys on the
        // folder path too, so each Deltarune chapter keeps its own pages.
        // Xenia (mount_cache = true) already provides cache: and may fail the mount call itself.
        if (attempt == 0) XMountUtilityDrive(FALSE, 16384, 65536);
        uint32_t hash = 2166136261u;
        for (const char* c = g_d3d9DataWinPath; *c; c++) hash = (hash ^ (uint8_t)*c) * 16777619u;
        sprintf(dir, "cache:\\bs_%08X_%08X\\", (unsigned)hash, (unsigned)size);
    }
    logWarn("%s", "D3D9: no writable texture cache; pages are decoded every time\n");
}

static void d3d9LoaderStart(D3D9Renderer* dr, DataWin* dw) {
    uint32_t n = dw->txtr.count;
    if (n == 0) return;
    D3D9Loader* L = (D3D9Loader*)calloc(1, sizeof(D3D9Loader));
    if (!L) return;
    L->dw = dw;
    L->qoiTitle = DataWin_isVersionAtLeast(dw, 2022, 5, 0, 0);
    L->cap = n * 2;
    L->queue = (uint32_t*)calloc(L->cap, sizeof(uint32_t));
    L->state = (uint8_t*)calloc(n, 1);
    L->dxt = (uint8_t*)calloc(n, 1);
    L->out = (uint8_t**)calloc(n, sizeof(uint8_t*));
    L->outW = (int32_t*)calloc(n, sizeof(int32_t));
    L->outH = (int32_t*)calloc(n, sizeof(int32_t));
    L->wake = CreateSemaphore(NULL, 0, (LONG)L->cap + D3D9_LOADER_THREADS, NULL);
    if (!L->queue || !L->state || !L->dxt || !L->out || !L->outW || !L->outH || !L->wake) {
        logWarn("%s", "D3D9: texture loader unavailable, decoding on the main thread\n");
        if (L->wake) CloseHandle(L->wake);
        free(L->queue); free(L->state); free(L->dxt); free(L->out); free(L->outW); free(L->outH); free(L);
        return;
    }
    InitializeCriticalSection(&L->lock);
    d3d9PickCacheDir(L);
    if (L->cacheDir[0]) { // build the whole cache in the background, page order
        EnterCriticalSection(&L->lock);
        for (uint32_t i = 0; i < n; i++) d3d9LoaderPush(L, i | D3D9_PREFETCH, false);
        LeaveCriticalSection(&L->lock);
    }
    for (int k = 0; k < D3D9_LOADER_THREADS; k++) {
        // 256 KB stack: the BZip2 decoder keeps sizeable locals
        L->threads[k] = CreateThread(NULL, 256 * 1024, d3d9LoaderThread, L, CREATE_SUSPENDED, NULL);
        if (L->threads[k]) {
            XSetThreadProcessor(L->threads[k], 2 + k);
            ResumeThread(L->threads[k]);
        }
    }
    dr->loader = L;
}

static void d3d9LoaderStop(D3D9Renderer* dr) {
    D3D9Loader* L = (D3D9Loader*)dr->loader;
    if (!L) return;
    L->quit = true;
    ReleaseSemaphore(L->wake, D3D9_LOADER_THREADS, NULL);
    for (int k = 0; k < D3D9_LOADER_THREADS; k++) {
        if (!L->threads[k]) continue;
        WaitForSingleObject(L->threads[k], INFINITE);
        CloseHandle(L->threads[k]);
    }
    for (uint32_t i = 0; i < L->cap / 2; i++) free(L->out[i]);
    CloseHandle(L->wake);
    DeleteCriticalSection(&L->lock);
    free(L->queue); free(L->state); free(L->dxt); free(L->out); free(L->outW); free(L->outH); free(L);
    dr->loader = NULL;
}

// Queues the page (ahead of background work) and uploads it once decoded. With
// `wait`, blocks until it is ready (size queries and primitives cannot skip a frame).
static void d3d9LoaderRequest(D3D9Renderer* dr, uint32_t i, bool wait) {
    D3D9Loader* L = (D3D9Loader*)dr->loader;
    EnterCriticalSection(&L->lock);
    uint8_t st = L->state[i];
    if (st == D3D9_PAGE_IDLE) {
        LeaveCriticalSection(&L->lock);
        bool dxt = d3d9WantDxt(dr, &dr->base.dataWin->txtr.textures[i]); // may evict pages: outside the lock
        EnterCriticalSection(&L->lock);
        L->dxt[i] = dxt;
        L->state[i] = st = D3D9_PAGE_QUEUED;
        d3d9LoaderPush(L, i, true);
    }
    LeaveCriticalSection(&L->lock);
    while (st == D3D9_PAGE_QUEUED) {
        if (!wait) return;
        Sleep(1);
        EnterCriticalSection(&L->lock);
        st = L->state[i];
        LeaveCriticalSection(&L->lock);
    }
    // READY or BAD: the workers are done with this page
    EnterCriticalSection(&L->lock);
    uint8_t* data = L->out[i];
    int w = L->outW[i], h = L->outH[i];
    L->out[i] = NULL;
    L->state[i] = D3D9_PAGE_IDLE;
    LeaveCriticalSection(&L->lock);
    if (!data) {
        logWarn("BS: could not decode TXTR page %u\n", i);
        dr->pageFailed[i] = 1;
    } else if (!d3d9UploadPage(dr, i, data, w, h)) {
        dr->pageFailed[i] = 2; // no room right now: retried shortly (d3d9PageEx)
    }
    free(data);
}

// Every page lookup goes through here: loads on first use and marks it used this frame.
// Returns NULL while the page is still decoding unless `wait` is set.
// shortcut: a page that fails to decode stays missing until restart.
static void* d3d9PageEx(D3D9Renderer* dr, int32_t page, bool wait) {
    if (page < 0 || !dr->textures || (uint32_t)page >= dr->textureCount) return NULL;
    dr->pageLastUse[page] = dr->frameCounter;
    // Lacked room earlier: retry twice a second, by then it may get a DXT slot or space.
    if (dr->pageFailed[page] == 2 && dr->frameCounter % 30 == 0) dr->pageFailed[page] = 0;
    if (dr->textures[page] || dr->pageFailed[page] ||
        (uint32_t)page >= dr->originalTexturePageCount) return dr->textures[page];
    if (dr->loader) d3d9LoaderRequest(dr, (uint32_t)page, wait);
    else if (!d3d9LoadPage(dr, (uint32_t)page)) dr->pageFailed[page] = 1;
    return dr->textures[page];
}

static void* d3d9Page(D3D9Renderer* dr, int32_t page) {
    return d3d9PageEx(dr, page, false);
}

static void d3d9Init(Renderer* renderer, DataWin* dataWin) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    IDirect3DDevice9* dev = Dev(dr);
    renderer->dataWin = dataWin;
    dr->initialized = false;
    if (!dev || !dataWin) return;
    dr->gameW = (int32_t)dataWin->gen8.defaultWindowWidth;
    dr->gameH = (int32_t)dataWin->gen8.defaultWindowHeight;
    dr->blendMode = bm_normal;
    dr->textureBytesAllocated = 0;
    dr->maxTextureBytes = 256u * 1024u * 1024u; // Undertale needs ~170 MB; 512 MB console RAM
    renderer->blendFactors.src = bm_src_alpha;
    renderer->blendFactors.dst = bm_inv_src_alpha;
    renderer->blendFactors.srcAlpha = bm_src_alpha;
    renderer->blendFactors.dstAlpha = bm_inv_src_alpha;
    // All vertices go through WORLD_VIEW_PROJECTION; a zeroed matrix would collapse every draw.
    for (int i = 0; i < MATRICES_MAX; i++) Matrix4f_identity(&renderer->gmlMatrices[i]);
    Matrix4f_identity(&dr->screenTransform);

    // Allocate CPU vertex staging buffer
    dr->vertexData = (uint8_t*)malloc(D3D9_MAX_QUADS * D3D9_VERTS_PER_QUAD * sizeof(SpriteVertex));
    if (!dr->vertexData) {
        logWarn("%s", "D3D9: vertex staging allocation failed\n");
        return;
    }
    dr->triangleData = malloc(D3D9_MAX_QUADS * 6u * sizeof(SpriteVertex));
    if (!dr->triangleData) {
        logWarn("%s", "D3D9: triangle staging allocation failed\n");
        return;
    }
    dr->primitiveVertexCapacity = D3D9_MAX_PRIMITIVE_VERTICES;
    dr->primitiveVertexData = malloc((size_t)dr->primitiveVertexCapacity * sizeof(SpriteVertex));
    if (!dr->primitiveVertexData) {
        dr->primitiveVertexCapacity = 0;
        logWarn("%s", "D3D9: primitive vertex allocation failed; immediate primitives disabled\n");
    }

    // Compile shaders from source
    ID3DXBuffer* pCode = NULL;
    ID3DXBuffer* pErr = NULL;

    HRESULT hr = D3DXCompileShader(g_vsSource, (UINT)strlen(g_vsSource),
                                   NULL, NULL, "main", "vs_2_0", 0, &pCode, &pErr, NULL);
    if (FAILED(hr) || !pCode) {
        logWarn("%s", "VS compile failed: ");
        if (pErr) logWarn("%s", (const char*)pErr->GetBufferPointer());
        if (pErr) pErr->Release();
        if (pCode) pCode->Release();
        return;
    }
    hr = dev->CreateVertexShader((const DWORD*)pCode->GetBufferPointer(),
                                 (IDirect3DVertexShader9**)&dr->pVertexShader);
    pCode->Release();
    if (pErr) { pErr->Release(); pErr = NULL; }
    if (FAILED(hr) || !dr->pVertexShader) {
        logWarn("%s", "D3D9: CreateVertexShader failed\n");
        return;
    }

    hr = D3DXCompileShader(g_psSource, (UINT)strlen(g_psSource),
                           NULL, NULL, "main", "ps_2_0", 0, &pCode, &pErr, NULL);
    if (FAILED(hr) || !pCode) {
        logWarn("%s", "PS compile failed: ");
        if (pErr) logWarn("%s", (const char*)pErr->GetBufferPointer());
        if (pErr) pErr->Release();
        if (pCode) pCode->Release();
        return;
    }
    hr = dev->CreatePixelShader((const DWORD*)pCode->GetBufferPointer(),
                                (IDirect3DPixelShader9**)&dr->pPixelShader);
    pCode->Release();
    if (pErr) { pErr->Release(); pErr = NULL; }
    if (FAILED(hr) || !dr->pPixelShader) {
        logWarn("%s", "D3D9: CreatePixelShader failed\n");
        return;
    }

    // Create vertex declaration
    static const D3DVERTEXELEMENT9 decl[] = {
        { 0,  0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        { 0, 24, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
        D3DDECL_END()
    };
    hr = dev->CreateVertexDeclaration(decl, (IDirect3DVertexDeclaration9**)&dr->pVertexDecl);
    if (FAILED(hr) || !dr->pVertexDecl) {
        logWarn("%s", "D3D9: CreateVertexDeclaration failed\n");
        return;
    }

    // Create 1x1 white texture for primitives.
    IDirect3DTexture9* whiteTex = NULL;
    hr = dev->CreateTexture(1, 1, 1, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_DEFAULT, &whiteTex, NULL);
    if (FAILED(hr) || !whiteTex) {
        logWarn("%s", "D3D9: white texture creation failed\n");
        return;
    }
    D3DLOCKED_RECT lr;
    hr = whiteTex->LockRect(0, &lr, NULL, 0);
    if (FAILED(hr) || !lr.pBits) {
        whiteTex->Release();
        logWarn("%s", "D3D9: white texture lock failed\n");
        return;
    }
    *(DWORD*)lr.pBits = 0xFFFFFFFF;
    hr = whiteTex->UnlockRect(0);
    if (FAILED(hr)) {
        whiteTex->Release();
        logWarn("%s", "D3D9: white texture unlock failed\n");
        return;
    }
    dr->whiteTexture = whiteTex;

    // Load TXTR pages: decode PNG to RGBA, upload as D3D textures
    dr->textureCount = dataWin->txtr.count;
    if (dr->textureCount > 0) {
        dr->textures = (void**)calloc(dr->textureCount, sizeof(void*));
        dr->textureWidths = (int32_t*)calloc(dr->textureCount, sizeof(int32_t));
        dr->textureHeights = (int32_t*)calloc(dr->textureCount, sizeof(int32_t));
        dr->pageLastUse = (uint32_t*)calloc(dr->textureCount, sizeof(uint32_t));
        dr->pageBytes = (uint32_t*)calloc(dr->textureCount, sizeof(uint32_t));
        dr->pageFailed = (uint8_t*)calloc(dr->textureCount, 1);
        if (!dr->textures || !dr->textureWidths || !dr->textureHeights ||
            !dr->pageLastUse || !dr->pageBytes || !dr->pageFailed) {
            logWarn("%s", "D3D9: texture table allocation failed\n");
            return;
        }
    }

    dr->originalTexturePageCount = dataWin->txtr.count;
    dr->originalTpagCount = dataWin->tpag.count;
    dr->originalSpriteCount = dataWin->sprt.count;
    d3d9LoaderStart(dr, dataWin);

    dr->currentTextureIndex = -1;
    dr->quadCount = 0;
    d3d9LoadUserShaders(dr, dataWin);
    dr->initialized = true;
    logInfo("BS: D3D9 renderer initialized (%u texture pages)\n", dr->textureCount);
}

static void d3d9Destroy(Renderer* renderer) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    VMBuiltins_resetD3D(renderer); // frees renderer->d3d, as every upstream renderer does
    d3d9LoaderStop(dr); // workers read the blobs freed below
    if (dr->textures) {
        for (uint32_t i = 0; i < dr->textureCount; i++) {
            if (dr->textures[i]) ((IDirect3DTexture9*)dr->textures[i])->Release();
        }
    }
    free(dr->textures);
    free(dr->textureWidths);
    free(dr->textureHeights);
    free(dr->pageLastUse);
    free(dr->pageBytes);
    free(dr->pageFailed);
    free(dr->vertexData);
    free(dr->triangleData);
    free(dr->primitiveVertexData);
    if (dr->whiteTexture) ((IDirect3DTexture9*)dr->whiteTexture)->Release();
    if (dr->pVertexShader) ((IDirect3DVertexShader9*)dr->pVertexShader)->Release();
    if (dr->pPixelShader) ((IDirect3DPixelShader9*)dr->pPixelShader)->Release();
    if (dr->pVertexDecl) ((IDirect3DVertexDeclaration9*)dr->pVertexDecl)->Release();
    d3d9FreeSurfaces(dr);
    d3d9ReleaseUserShaders(dr);
    free(dr);
}

static void d3d9BeginFrame(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    IDirect3DDevice9* dev = Dev(dr);
    if (!dr->initialized || !dev || gameW <= 0 || gameH <= 0 || windowW <= 0 || windowH <= 0) return;

    dr->gameW = gameW;
    dr->gameH = gameH;
    dr->windowW = windowW;
    dr->windowH = windowH;
    d3d9ComputeLetterbox(dr);
    // On the application surface the game draws at its own resolution;
    // the letterbox is applied once, when the surface is blitted.
    if (d3d9Surface(dr, dr->appSurface)) {
        dr->renderScale = 1.0f;
        dr->renderOffsetX = dr->renderOffsetY = 0.0f;
    }
    dr->guiActive = false;
    dr->frameCounter++;
    d3d9ResetToGameArea(dr);

    // Debug: print once
    static bool printedOnce = false;
    if (!printedOnce) {
        logInfo("BS: renderScale=%d/1000 offsetX=%d offsetY=%d gameW=%d gameH=%d screenW=%d screenH=%d\n",
            (int)(dr->renderScale * 1000), (int)dr->renderOffsetX, (int)dr->renderOffsetY,
            gameW, gameH, windowW, windowH);
        printedOnce = true;
    }

    dr->skipNextClearScreen = false;
    dr->sceneActive = false;
    dr->quadCount = 0;
    dr->currentTextureIndex = -1;
    dr->primitiveActive = false;
    dr->primitiveVertexCount = 0;
    dr->primitiveTexture = 0;
    HRESULT sceneHr = dev->BeginScene();
    if (FAILED(sceneHr)) {
        logInfo("BS: D3D9 BeginScene failed: hr=0x%08X\n", (unsigned)sceneHr);
        return;
    }
    dr->sceneActive = true;
    dr->boundTarget = -1; // Present leaves the back buffer bound
    dr->hostDirty = false;
    dr->hostHasCopy = false;
    if (d3d9Surface(dr, dr->appSurface)) d3d9BindTarget(dr, dr->appSurface);

    // Set shared render state
    dev->SetVertexShader((IDirect3DVertexShader9*)dr->pVertexShader);
    dev->SetPixelShader((IDirect3DPixelShader9*)dr->pPixelShader);
    dev->SetVertexDeclaration((IDirect3DVertexDeclaration9*)dr->pVertexDecl);


    // Reset per-frame GPU state so the getters and actual device state agree
    // even when the previous frame ended with custom blend/alpha/color masks.
    dr->blendMode = bm_normal;
    renderer->blendFactors.src = bm_src_alpha;
    renderer->blendFactors.dst = bm_inv_src_alpha;
    renderer->blendFactors.srcAlpha = bm_src_alpha;
    renderer->blendFactors.dstAlpha = bm_inv_src_alpha;

    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHAREF, 0);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE,
        D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
        D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);

    // No depth testing for 2D
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

    D3DVIEWPORT9 vp;
    vp.X = 0;
    vp.Y = 0;
    vp.Width = (DWORD)windowW;
    vp.Height = (DWORD)windowH;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    dev->SetViewport(&vp);

    // Disable viewport transform — we use pre-transformed screen-space vertices
    dev->SetRenderState(D3DRS_VIEWPORTENABLE, FALSE);

    // Point filtering — pixel-perfect for 2D sprite games like Undertale
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, (dr->texRepeat ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP));
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, (dr->texRepeat ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP));
}

static void d3d9EndFrame(Renderer* renderer) {
    // The platform owns EndScene/Present after the GUI pass.
    flushBatch((D3D9Renderer*)renderer);
}

static void d3d9BeginView(Renderer* renderer, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH,
                           int32_t portX, int32_t portY, int32_t portW, int32_t portH, float viewAngle) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    IDirect3DDevice9* dev = Dev(dr);
    if (!dr->initialized || !dev || viewW <= 0 || viewH <= 0) return;

    // The D3D viewport stays full-screen (D3DRS_VIEWPORTENABLE=FALSE); the
    // port and camera live in screenTransform. Like upstream GLCommon_beginView,
    // apply the view's camera matrices (they carry view_angle), then let an
    // active d3d_start override them.
    dr->hostTargetFlipY = false;
    d3d9SetGameViewport(dr, (float)portX, (float)portY, (float)portW, (float)portH);
    Runner* runner = renderer->runner;
    GMLCamera* camera = NULL;
    if (runner) {
        int32_t viewCurrent = runner->viewsEnabled ? runner->viewCurrent : 0;
        renderer->cameraCurrent = runner->views[viewCurrent].cameraId;
        camera = Runner_getCameraById(runner, renderer->cameraCurrent);
    }
    if (camera) {
        d3d9SetProjection(dr, &camera->viewMatrix, &camera->projectionMatrix);
    } else {
        Matrix4f view, projection;
        Matrix4f_identity(&view);
        Matrix4f_viewProjection(&projection, (float)viewX, (float)viewY,
                                (float)viewW, (float)viewH, viewAngle);
        d3d9SetProjection(dr, &view, &projection);
    }
    VMBuiltins_applyD3DDefaultView(renderer, (float)viewX, (float)viewY,
                                   (float)viewW, (float)viewH, viewAngle);
}

// Runner_drawViews (surface views) and camera_apply / d3d_set_projection_*.
static void d3d9ApplyProjection(Renderer* renderer, const Matrix4f* viewMatrix,
                                const Matrix4f* projectionMatrix) {
    if (!viewMatrix || !projectionMatrix) return;
    d3d9SetProjection((D3D9Renderer*)renderer, viewMatrix, projectionMatrix);
}

// matrix_set / sequences / d3d_transform_*: any of VIEW, PROJECTION, WORLD.
static void d3d9SetMatrix(Renderer* renderer, int32_t matrixType, Matrix4f matrix) {
    if (matrixType < 0 || matrixType >= MATRICES_MAX) return;
    // Sequences set the world matrix per node; skip redundant flushes like upstream glSetMatrix.
    if (memcmp(&renderer->gmlMatrices[matrixType], &matrix, sizeof(Matrix4f)) == 0) return;
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    flushBatch(dr);
    renderer->gmlMatrices[matrixType] = matrix;
    // Renderer_applyProjection copies its inputs before writing, so aliasing is safe.
    Renderer_applyProjection(renderer, &renderer->gmlMatrices[MATRIX_VIEW],
                             &renderer->gmlMatrices[MATRIX_PROJECTION]);
    d3d9RebuildTransform(dr);
}

static void d3d9EndView(Renderer* renderer) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    flushBatch(dr);
}

// ===[ Sprite Drawing ]===

static void d3d9DrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y,
                            float originX, float originY, float xscale, float yscale,
                            float angleDeg, uint32_t color, float alpha) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || (uint32_t)tpagIndex >= dw->tpag.count) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int32_t texPageId = tpag->texturePageId;
    if (0 > texPageId || (uint32_t)texPageId >= dr->textureCount) return;
    if (!d3d9Page(dr, texPageId)) return;

    ensureTexture(dr, texPageId);

    float texW = (float)dr->textureWidths[texPageId];
    float texH = (float)dr->textureHeights[texPageId];
    if (texW <= 0 || texH <= 0) return;

    // UV coordinates on the texture atlas
    float u0 = (float)tpag->sourceX / texW;
    float v0 = (float)tpag->sourceY / texH;
    float u1 = (float)(tpag->sourceX + tpag->sourceWidth) / texW;
    float v1 = (float)(tpag->sourceY + tpag->sourceHeight) / texH;

    // Quad corners in local space (before transform)
    float localX0 = (float)tpag->targetX - originX;
    float localY0 = (float)tpag->targetY - originY;
    float localX1 = localX0 + (float)tpag->sourceWidth;
    float localY1 = localY0 + (float)tpag->sourceHeight;

    // Scale
    localX0 *= xscale; localY0 *= yscale;
    localX1 *= xscale; localY1 *= yscale;

    float cr, cg, cb, ca;
    bgrToFloatColor(color, alpha, &cr, &cg, &cb, &ca);

    // Build 4 corners
    float cx[4], cy[4];
    if (angleDeg != 0.0f) {
        float rad = angleDeg * (3.14159265f / 180.0f);
        float cosA = cosf(rad);
        float sinA = sinf(rad);

        float lx[4] = { localX0, localX1, localX1, localX0 };
        float ly[4] = { localY0, localY0, localY1, localY1 };
        for (int i = 0; i < 4; i++) {
            cx[i] = lx[i] * cosA - ly[i] * sinA;
            cy[i] = lx[i] * sinA + ly[i] * cosA;
        }
    } else {
        cx[0] = localX0; cy[0] = localY0;
        cx[1] = localX1; cy[1] = localY0;
        cx[2] = localX1; cy[2] = localY1;
        cx[3] = localX0; cy[3] = localY1;
    }

    // Transform to screen space
    SpriteVertex* v = allocQuad(dr);
    float sx, sy;
    for (int i = 0; i < 4; i++) {
        transformPoint(dr, x + cx[i], y + cy[i], &sx, &sy);
        v[i].x = sx;
        v[i].y = sy;
        v[i].z = 0.0f;
        v[i].w = 1.0f;
        v[i].r = cr; v[i].g = cg; v[i].b = cb; v[i].a = ca;
    }
    v[0].u = u0; v[0].v = v0;
    v[1].u = u1; v[1].v = v0;
    v[2].u = u1; v[2].v = v1;
    v[3].u = u0; v[3].v = v1;
}

static void d3d9DrawSpritePart(Renderer* renderer, int32_t tpagIndex,
                                int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH,
                                float x, float y, float xscale, float yscale,
                                uint32_t color, float alpha) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || (uint32_t)tpagIndex >= dw->tpag.count) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int32_t texPageId = tpag->texturePageId;
    if (0 > texPageId || (uint32_t)texPageId >= dr->textureCount) return;
    if (!d3d9Page(dr, texPageId)) return;

    ensureTexture(dr, texPageId);

    float texW = (float)dr->textureWidths[texPageId];
    float texH = (float)dr->textureHeights[texPageId];
    if (texW <= 0 || texH <= 0) return;

    float u0 = (float)(tpag->sourceX + srcOffX) / texW;
    float v0 = (float)(tpag->sourceY + srcOffY) / texH;
    float u1 = (float)(tpag->sourceX + srcOffX + srcW) / texW;
    float v1 = (float)(tpag->sourceY + srcOffY + srcH) / texH;

    float drawW = (float)srcW * xscale;
    float drawH = (float)srcH * yscale;

    float cr, cg, cb, ca;
    bgrToFloatColor(color, alpha, &cr, &cg, &cb, &ca);

    SpriteVertex* v = allocQuad(dr);
    float x0, y0, x1, y1;
    transformPoint(dr, x, y, &x0, &y0);
    transformPoint(dr, x + drawW, y + drawH, &x1, &y1);

    setVertex(&v[0], x0, y0, u0, v0, cr, cg, cb, ca);
    setVertex(&v[1], x1, y0, u1, v0, cr, cg, cb, ca);
    setVertex(&v[2], x1, y1, u1, v1, cr, cg, cb, ca);
    setVertex(&v[3], x0, y1, u0, v1, cr, cg, cb, ca);
}

static void d3d9DrawLine(Renderer* renderer, float x1, float y1, float x2, float y2,
                           float width, uint32_t color, float alpha);
static void d3d9DrawLineColor(Renderer* renderer, float x1, float y1, float x2, float y2,
                              float width, uint32_t color1, uint32_t color2, float alpha);

// Exact modern drawSpritePart contract. The pivot is in game coordinates;
// the source rectangle remains floating-point through UV calculation.
static void d3d9DrawSpritePartModern(Renderer* renderer, int32_t tpagIndex,
                                     float srcOffX, float srcOffY, float srcW, float srcH,
                                     float x, float y, float xscale, float yscale,
                                     float angleDeg, float pivotX, float pivotY,
                                     uint32_t color, float alpha) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    DataWin* dw = renderer->dataWin;
    if (!dw || tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int32_t page = tpag->texturePageId;
    if (page < 0 || (uint32_t)page >= dr->textureCount || !d3d9Page(dr, page)) return;
    if (srcW <= 0.0f || srcH <= 0.0f) return;

    ensureTexture(dr, page);
    float texW = (float)dr->textureWidths[page], texH = (float)dr->textureHeights[page];
    if (texW <= 0.0f || texH <= 0.0f) return;
    float u0 = (tpag->sourceX + srcOffX) / texW;
    float v0 = (tpag->sourceY + srcOffY) / texH;
    float u1 = (tpag->sourceX + srcOffX + srcW) / texW;
    float v1 = (tpag->sourceY + srcOffY + srcH) / texH;

    float px[4] = { x, x + srcW * xscale, x + srcW * xscale, x };
    float py[4] = { y, y, y + srcH * yscale, y + srcH * yscale };
    float rad = angleDeg * (3.14159265f / 180.0f);
    float cs = cosf(rad), sn = sinf(rad);
    SpriteVertex* v = allocQuad(dr);
    float cr, cg, cb, ca;
    bgrToFloatColor(color, alpha, &cr, &cg, &cb, &ca);
    float uvx[4] = { u0, u1, u1, u0 };
    float uvy[4] = { v0, v0, v1, v1 };
    for (int i = 0; i < 4; i++) {
        float dx = px[i] - pivotX, dy = py[i] - pivotY;
        float rx = pivotX + dx * cs - dy * sn;
        float ry = pivotY + dx * sn + dy * cs;
        float sx, sy;
        transformPoint(dr, rx, ry, &sx, &sy);
        setVertex(&v[i], sx, sy, uvx[i], uvy[i], cr, cg, cb, ca);
    }
}

static void d3d9DrawSpritePartColorModern(Renderer* renderer, int32_t tpagIndex,
                                          float srcOffX, float srcOffY, float srcW, float srcH,
                                          float x, float y, float xscale, float yscale,
                                          float angleDeg, float pivotX, float pivotY,
                                          uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4,
                                          float alpha) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    DataWin* dw = renderer->dataWin;
    if (!dw || tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int32_t page = tpag->texturePageId;
    if (page < 0 || (uint32_t)page >= dr->textureCount || !d3d9Page(dr, page) ||
        srcW <= 0.0f || srcH <= 0.0f) return;

    ensureTexture(dr, page);
    float texW = (float)dr->textureWidths[page], texH = (float)dr->textureHeights[page];
    if (texW <= 0.0f || texH <= 0.0f) return;
    float u[4] = {
        (tpag->sourceX + srcOffX) / texW,
        (tpag->sourceX + srcOffX + srcW) / texW,
        (tpag->sourceX + srcOffX + srcW) / texW,
        (tpag->sourceX + srcOffX) / texW
    };
    float vv[4] = {
        (tpag->sourceY + srcOffY) / texH,
        (tpag->sourceY + srcOffY) / texH,
        (tpag->sourceY + srcOffY + srcH) / texH,
        (tpag->sourceY + srcOffY + srcH) / texH
    };
    float px[4] = { x, x + srcW * xscale, x + srcW * xscale, x };
    float py[4] = { y, y, y + srcH * yscale, y + srcH * yscale };
    uint32_t colors[4] = { c1, c2, c3, c4 };
    float rad = angleDeg * (3.14159265f / 180.0f);
    float cs = cosf(rad), sn = sinf(rad);
    SpriteVertex* v = allocQuad(dr);
    for (int i = 0; i < 4; i++) {
        float dx = px[i] - pivotX, dy = py[i] - pivotY;
        float sx, sy;
        transformPoint(dr, pivotX + dx * cs - dy * sn,
                       pivotY + dx * sn + dy * cs, &sx, &sy);
        float cr, cg, cb, ca;
        bgrToFloatColor(colors[i], alpha, &cr, &cg, &cb, &ca);
        setVertex(&v[i], sx, sy, u[i], vv[i], cr, cg, cb, ca);
    }
}

static void d3d9DrawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2,
                               uint32_t color, float alpha, bool outline) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;

    if (outline) {
        // Draw 4 lines as thin rectangles
        float lw = 1.0f;
        d3d9DrawRectangle(renderer, x1, y1, x2, y1 + lw, color, alpha, false); // top
        d3d9DrawRectangle(renderer, x1, y2 - lw, x2, y2, color, alpha, false); // bottom
        d3d9DrawRectangle(renderer, x1, y1, x1 + lw, y2, color, alpha, false); // left
        d3d9DrawRectangle(renderer, x2 - lw, y1, x2, y2, color, alpha, false); // right
        return;
    }

    ensureTexture(dr, -1); // white texture

    float cr, cg, cb, ca;
    bgrToFloatColor(color, alpha, &cr, &cg, &cb, &ca);
    SpriteVertex* v = allocQuad(dr);

    float sx0, sy0, sx1, sy1;
    transformPoint(dr, x1, y1, &sx0, &sy0);
    transformPoint(dr, x2, y2, &sx1, &sy1);

    setVertex(&v[0], sx0, sy0, 0, 0, cr, cg, cb, ca);
    setVertex(&v[1], sx1, sy0, 1, 0, cr, cg, cb, ca);
    setVertex(&v[2], sx1, sy1, 1, 1, cr, cg, cb, ca);
    setVertex(&v[3], sx0, sy1, 0, 1, cr, cg, cb, ca);
}

static void d3d9DrawLine(Renderer* renderer, float x1, float y1, float x2, float y2,
                          float width, uint32_t color, float alpha) {
    // Draw line as a thin rotated rectangle
    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) return;

    float nx = -dy / len * width * 0.5f;
    float ny = dx / len * width * 0.5f;

    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    ensureTexture(dr, -1);
    float cr, cg, cb, ca;
    bgrToFloatColor(color, alpha, &cr, &cg, &cb, &ca);

    SpriteVertex* v = allocQuad(dr);
    float sx, sy;

    transformPoint(dr, x1 + nx, y1 + ny, &sx, &sy); setVertex(&v[0], sx, sy, 0, 0, cr, cg, cb, ca);
    transformPoint(dr, x2 + nx, y2 + ny, &sx, &sy); setVertex(&v[1], sx, sy, 1, 0, cr, cg, cb, ca);
    transformPoint(dr, x2 - nx, y2 - ny, &sx, &sy); setVertex(&v[2], sx, sy, 1, 1, cr, cg, cb, ca);
    transformPoint(dr, x1 - nx, y1 - ny, &sx, &sy); setVertex(&v[3], sx, sy, 0, 1, cr, cg, cb, ca);
}

static void d3d9DrawLineColor(Renderer* renderer, float x1, float y1, float x2, float y2,
                               float width, uint32_t color1, uint32_t color2, float alpha) {
    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) return;

    float nx = -dy / len * width * 0.5f;
    float ny = dx / len * width * 0.5f;

    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    ensureTexture(dr, -1);
    float c1r, c1g, c1b, c1a;
    bgrToFloatColor(color1, alpha, &c1r, &c1g, &c1b, &c1a);
    float c2r, c2g, c2b, c2a;
    bgrToFloatColor(color2, alpha, &c2r, &c2g, &c2b, &c2a);

    SpriteVertex* v = allocQuad(dr);
    float sx, sy;

    transformPoint(dr, x1 + nx, y1 + ny, &sx, &sy); setVertex(&v[0], sx, sy, 0, 0, c1r, c1g, c1b, c1a);
    transformPoint(dr, x2 + nx, y2 + ny, &sx, &sy); setVertex(&v[1], sx, sy, 1, 0, c2r, c2g, c2b, c2a);
    transformPoint(dr, x2 - nx, y2 - ny, &sx, &sy); setVertex(&v[2], sx, sy, 1, 1, c2r, c2g, c2b, c2a);
    transformPoint(dr, x1 - nx, y1 - ny, &sx, &sy); setVertex(&v[3], sx, sy, 0, 1, c1r, c1g, c1b, c1a);
}

// Bilinear gradient across one text line: c[0]=top-left, c[1]=top-right, c[2]=bottom-right, c[3]=bottom-left.
static void d3d9GradientColor(const uint32_t* c, float tx, float ty, float alpha,
                              float* r, float* g, float* b, float* a) {
    if (tx < 0.0f) tx = 0.0f; else if (tx > 1.0f) tx = 1.0f;
    if (ty < 0.0f) ty = 0.0f; else if (ty > 1.0f) ty = 1.0f;
    float ch[3];
    for (int s = 0; s < 3; s++) {
        float tl = (float)((c[0] >> (s * 8)) & 0xFF), tr = (float)((c[1] >> (s * 8)) & 0xFF);
        float br = (float)((c[2] >> (s * 8)) & 0xFF), bl = (float)((c[3] >> (s * 8)) & 0xFF);
        float top = tl + (tr - tl) * tx, bottom = bl + (br - bl) * tx;
        ch[s] = (top + (bottom - top) * ty) / 255.0f;
    }
    *r = ch[0]; *g = ch[1]; *b = ch[2]; *a = alpha;
}

// lineSeparation < 0 uses the font's em size; corners == NULL draws in drawColor.
static void d3d9DrawText(Renderer* renderer, const char* text, float x, float y,
                          float xscale, float yscale, float angleDeg,
                          float lineSeparation, const uint32_t* corners, float alpha) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    DataWin* dw = renderer->dataWin;
    int32_t fontIndex = renderer->drawFont;
    if (!text || 0 > fontIndex || (uint32_t)fontIndex >= dw->font.count) return;

    Font* font = &dw->font.fonts[fontIndex];
    uint32_t flat[4] = { renderer->drawColor, renderer->drawColor, renderer->drawColor, renderer->drawColor };
    if (!corners) corners = flat;
    float lineHeight = lineSeparation >= 0.0f ? lineSeparation : (float)font->emSize;

    // Sprite fonts (font_add_sprite; all of Deltarune's) take glyph N from frame N
    // of a sprite, possibly on different pages. Regular fonts use one atlas region.
    Sprite* spriteFont = NULL;
    TexturePageItem* fontTpag = NULL;
    int32_t pageId = -1;
    float texW = 0.0f, texH = 0.0f;
    if (font->isSpriteFont) {
        if (font->spriteIndex < 0 || (uint32_t)font->spriteIndex >= dw->sprt.count) return;
        spriteFont = &dw->sprt.sprites[font->spriteIndex];
    } else {
        if (0 > font->tpagIndex || (uint32_t)font->tpagIndex >= dw->tpag.count) return;
        fontTpag = &dw->tpag.items[font->tpagIndex];
        pageId = fontTpag->texturePageId;
        if (!d3d9Page(dr, pageId)) return;
        texW = (float)dr->textureWidths[pageId];
        texH = (float)dr->textureHeights[pageId];
        if (texW <= 0 || texH <= 0) return;
    }

    // The core already converted GML '#' newlines before calling drawText (as for upstream renderers).
    const char* processed = text;
    int32_t textLen = (int32_t)strlen(processed);

    // Count lines
    int32_t lineCount = TextUtils_countLines(processed, textLen);

    // Vertical alignment offset
    float totalHeight = (float)lineCount * lineHeight;
    float valignOffset = 0;
    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    float fontScaleX = xscale * font->scaleX;
    float fontScaleY = yscale * font->scaleY;

    // Build rotation transform (if needed)
    float cosA = 1.0f, sinA = 0.0f;
    bool hasRotation = (angleDeg != 0.0f);
    if (hasRotation) {
        float rad = -angleDeg * (3.14159265f / 180.0f);
        cosA = cosf(rad);
        sinA = sinf(rad);
    }

    // Iterate through lines
    float cursorY = valignOffset;
    int32_t lineStart = 0;

    for (int32_t lineIdx = 0; lineCount > lineIdx; lineIdx++) {
        // Find end of current line
        int32_t lineEnd = lineStart;
        while (textLen > lineEnd && !TextUtils_isNewlineChar(processed[lineEnd])) {
            lineEnd++;
        }
        int32_t lineLen = lineEnd - lineStart;

        // Horizontal alignment offset for this line
        float lineWidth = TextUtils_measureLineWidth(font, processed + lineStart, lineLen);
        float halignOffset = 0;
        if (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset;

        // Render each glyph
        int32_t pos = 0;
        while (lineLen > pos) {
            uint16_t ch = TextUtils_decodeUtf8(processed + lineStart, lineLen, &pos);
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            if (!glyph) continue;
            if (glyph->sourceWidth == 0 || glyph->sourceHeight == 0) {
                cursorX += glyph->shift;
                continue;
            }

            float u0, v0, u1, v1, glyphW, glyphH;
            float localX0 = cursorX + glyph->offset;
            float localY0 = cursorY;
            int32_t glyphPage = pageId;
            if (spriteFont) {
                int32_t frame = (int32_t)(glyph - font->glyphs);
                int32_t ti = frame >= 0 && frame < (int32_t)spriteFont->textureCount ? spriteFont->tpagIndices[frame] : -1;
                TexturePageItem* gt = ti >= 0 && (uint32_t)ti < dw->tpag.count ? &dw->tpag.items[ti] : NULL;
                glyphPage = gt ? gt->texturePageId : -1;
                if (!gt || !d3d9Page(dr, glyphPage)) { cursorX += glyph->shift; continue; }
                float tw = (float)dr->textureWidths[glyphPage], th = (float)dr->textureHeights[glyphPage];
                u0 = gt->sourceX / tw;
                v0 = gt->sourceY / th;
                u1 = (gt->sourceX + gt->sourceWidth) / tw;
                v1 = (gt->sourceY + gt->sourceHeight) / th;
                glyphW = gt->sourceWidth;
                glyphH = gt->sourceHeight;
                localY0 += (float)gt->targetY - (float)font->spriteOriginYAdjust;
            } else {
                u0 = (float)(fontTpag->sourceX + glyph->sourceX) / texW;
                v0 = (float)(fontTpag->sourceY + glyph->sourceY) / texH;
                u1 = (float)(fontTpag->sourceX + glyph->sourceX + glyph->sourceWidth) / texW;
                v1 = (float)(fontTpag->sourceY + glyph->sourceY + glyph->sourceHeight) / texH;
                glyphW = glyph->sourceWidth;
                glyphH = glyph->sourceHeight;
            }
            ensureTexture(dr, glyphPage);
            float localX1 = localX0 + glyphW;
            float localY1 = localY0 + glyphH;

            // Scale
            float sx0 = localX0 * fontScaleX;
            float sy0 = localY0 * fontScaleY;
            float sx1 = localX1 * fontScaleX;
            float sy1 = localY1 * fontScaleY;

            // Build 4 corners (with optional rotation)
            float cx[4], cy[4];
            if (hasRotation) {
                float lx[4] = { sx0, sx1, sx1, sx0 };
                float ly[4] = { sy0, sy0, sy1, sy1 };
                for (int i = 0; i < 4; i++) {
                    cx[i] = lx[i] * cosA - ly[i] * sinA;
                    cy[i] = lx[i] * sinA + ly[i] * cosA;
                }
            } else {
                cx[0] = sx0; cy[0] = sy0;
                cx[1] = sx1; cy[1] = sy0;
                cx[2] = sx1; cy[2] = sy1;
                cx[3] = sx0; cy[3] = sy1;
            }

            SpriteVertex* v = allocQuad(dr);
            float screenX, screenY;
            float gx[4] = { localX0, localX1, localX1, localX0 };
            float gy[4] = { localY0, localY0, localY1, localY1 };
            for (int i = 0; i < 4; i++) {
                transformPoint(dr, x + cx[i], y + cy[i], &screenX, &screenY);
                v[i].x = screenX;
                v[i].y = screenY;
                v[i].z = 0.0f;
                v[i].w = 1.0f;
                float tx = lineWidth > 0.0f ? (gx[i] - halignOffset) / lineWidth : 0.0f;
                float ty = lineHeight > 0.0f ? (gy[i] - cursorY) / lineHeight : 0.0f;
                d3d9GradientColor(corners, tx, ty, alpha, &v[i].r, &v[i].g, &v[i].b, &v[i].a);
            }
            v[0].u = u0; v[0].v = v0;
            v[1].u = u1; v[1].v = v0;
            v[2].u = u1; v[2].v = v1;
            v[3].u = u0; v[3].v = v1;

            // Advance cursor (shift + kerning)
            cursorX += glyph->shift;
            if (lineLen > pos) {
                int32_t savedPos = pos;
                uint16_t nextCh = TextUtils_decodeUtf8(processed + lineStart, lineLen, &pos);
                pos = savedPos;
                cursorX += TextUtils_getKerningOffset(glyph, nextCh);
            }
        }

        cursorY += lineHeight;

        // Advance past the newline
        if (textLen > lineEnd) {
            lineStart = TextUtils_skipNewline(processed, lineEnd, textLen);
        } else {
            lineStart = lineEnd;
        }
    }
}

static void d3d9Flush(Renderer* renderer) {
    flushBatch((D3D9Renderer*)renderer);
}

// Grows the page tables by one; dynamic pages sit past originalTexturePageCount
// and are never evicted (d3d9MakeRoom only scans the data.win pages).
static int32_t d3d9AddPage(D3D9Renderer* dr, void* tex, int32_t w, int32_t h) {
    uint32_t n = dr->textureCount + 1;
    void* p;
    if (!(p = realloc(dr->textures, n * sizeof(void*)))) return -1;          dr->textures = (void**)p;
    if (!(p = realloc(dr->textureWidths, n * sizeof(int32_t)))) return -1;   dr->textureWidths = (int32_t*)p;
    if (!(p = realloc(dr->textureHeights, n * sizeof(int32_t)))) return -1;  dr->textureHeights = (int32_t*)p;
    if (!(p = realloc(dr->pageLastUse, n * sizeof(uint32_t)))) return -1;    dr->pageLastUse = (uint32_t*)p;
    if (!(p = realloc(dr->pageBytes, n * sizeof(uint32_t)))) return -1;      dr->pageBytes = (uint32_t*)p;
    if (!(p = realloc(dr->pageFailed, n))) return -1;                        dr->pageFailed = (uint8_t*)p;
    uint32_t page = dr->textureCount;
    dr->textures[page] = tex;
    dr->textureWidths[page] = w;
    dr->textureHeights[page] = h;
    dr->pageLastUse[page] = dr->frameCounter;
    dr->pageBytes[page] = 0;
    dr->pageFailed[page] = 0;
    dr->textureCount = n;
    return (int32_t)page;
}

// Copies the region on the GPU: bind a scratch target, draw the source region
// into it, switch back (which resolves it), then keep its texture as a page.
// shortcut: removeback, smooth and sprite_delete of these sprites are ignored.
static int32_t d3d9CreateSpriteFromSurfaceModern(Renderer* renderer, int32_t surfaceID,
                                                  int32_t x, int32_t y, int32_t w, int32_t h,
                                                  bool removeback, bool smooth,
                                                  int32_t xorig, int32_t yorig) {
    (void)removeback; (void)smooth;
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    DataWin* dw = renderer->dataWin;
    if (w <= 0 || h <= 0 || !dw || !d3d9Surface(dr, surfaceID)) return -1;
    int32_t prev = dr->boundTarget;
    int32_t tmp = d3d9CreateSurface(renderer, w, h);
    if (tmp < 0) return -1;
    if (!d3d9BindTarget(dr, tmp)) { // leaving the source here resolves its latest drawing
        d3d9ReleaseSurfaceObjects(dr, d3d9Surface(dr, tmp));
        return -1;
    }
    D3D9Surface* src = d3d9Surface(dr, surfaceID);
    d3d9BlitTexture(dr, src->tex, src->allocW, src->allocH, x, y, w, h, 0.0f, 0.0f);
    d3d9BindTarget(dr, prev);
    D3D9Surface* s = d3d9Surface(dr, tmp);
    int32_t page = d3d9AddPage(dr, s->tex, s->allocW, s->allocH);
    if (page < 0) {
        d3d9ReleaseSurfaceObjects(dr, s);
        return -1;
    }
    ((IDirect3DSurface9*)s->rt)->Release(); // the page owns the texture now
    memset(s, 0, sizeof(*s));

    uint32_t t = dw->tpag.count;
    for (uint32_t i = dr->originalTpagCount; i < dw->tpag.count; i++)
        if (dw->tpag.items[i].texturePageId == -1) { t = i; break; }
    if (t == dw->tpag.count) {
        TexturePageItem* items = (TexturePageItem*)realloc(dw->tpag.items, (t + 1) * sizeof(TexturePageItem));
        if (!items) return -1;
        dw->tpag.items = items;
        dw->tpag.count++;
    }
    TexturePageItem* tpag = &dw->tpag.items[t];
    memset(tpag, 0, sizeof(*tpag));
    tpag->sourceWidth = tpag->targetWidth = tpag->boundingWidth = (uint16_t)w;
    tpag->sourceHeight = tpag->targetHeight = tpag->boundingHeight = (uint16_t)h;
    tpag->texturePageId = (int16_t)page;

    uint32_t si = dw->sprt.count;
    for (uint32_t i = dr->originalSpriteCount; i < dw->sprt.count; i++)
        if (dw->sprt.sprites[i].textureCount == 0) { si = i; break; }
    if (si == dw->sprt.count) {
        Sprite* sprites = (Sprite*)realloc(dw->sprt.sprites, (si + 1) * sizeof(Sprite));
        if (!sprites) return -1;
        dw->sprt.sprites = sprites;
        dw->sprt.count++;
    }
    Sprite* sprite = &dw->sprt.sprites[si];
    memset(sprite, 0, sizeof(*sprite));
    int32_t* tpags = (int32_t*)malloc(sizeof(int32_t));
    if (!tpags) return -1;
    tpags[0] = (int32_t)t;
    sprite->name = "dynamic_sprite";
    sprite->width = (uint32_t)w;
    sprite->height = (uint32_t)h;
    sprite->originX = xorig;
    sprite->originY = yorig;
    sprite->textureCount = 1;
    sprite->tpagIndices = tpags;
    return (int32_t)si;
}

// Frees a sprite made at runtime (sprite_create_from_surface, sprite_duplicate):
// textureCount 0 marks the slot reusable. Data.win sprites are left alone.
// shortcut: the texture page behind a surface sprite is kept (duplicates may share it).
static void d3d9DeleteSprite(Renderer* renderer, int32_t spriteIndex) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    DataWin* dw = renderer->dataWin;
    if (!dw || spriteIndex < (int32_t)dr->originalSpriteCount || (uint32_t)spriteIndex >= dw->sprt.count) return;
    Sprite* s = &dw->sprt.sprites[spriteIndex];
    free(s->tpagIndices);
    s->tpagIndices = NULL;
    s->textureCount = 0;
}


// ===[ Modern renderer-contract adapters ]===

// GUI coordinates are independent of the last room view. The platform presents
// after this pass, so these hooks must never end or present the D3D scene.
// Views are done: put the application surface on screen, then Draw Post and
// GUI draw over it on the back buffer (as GameMaker does).
static void d3d9EndFrameInit(Renderer* renderer) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    flushBatch(dr);
    D3D9Surface* app = d3d9Surface(dr, dr->appSurface);
    if (!app || !dr->sceneActive) return;
    d3d9BindTarget(dr, -1); // resolves the application surface into its texture
    IDirect3DDevice9* dev = Dev(dr);
    // Clear first: the letterbox bars would otherwise show stale EDRAM.
    dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0);
    Runner* runner = renderer->runner;
    if (runner && runner->usingAppSurface && !runner->appSurfaceAutoDraw) return; // game draws it itself
    float x0 = dr->renderOffsetX, y0 = dr->renderOffsetY;
    float x1 = x0 + (float)app->w * dr->renderScale, y1 = y0 + (float)app->h * dr->renderScale;
    float xs[4] = { x0, x1, x1, x0 }, ys[4] = { y0, y0, y1, y1 };
    // One smooth upscale of the whole frame avoids uneven point-sampled rows at 1.5x.
    DWORD minFilter = D3DTEXF_POINT, magFilter = D3DTEXF_POINT;
    dev->GetSamplerState(0, D3DSAMP_MINFILTER, &minFilter);
    dev->GetSamplerState(0, D3DSAMP_MAGFILTER, &magFilter);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE); // the game image is opaque
    d3d9DrawTextureQuad(dr, app->tex, xs, ys, 0.0f, 0.0f,
                        (float)app->w / app->allocW, (float)app->h / app->allocH, 0xFFFFFF, 1.0f);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, minFilter);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, magFilter);
}
// Like upstream: keeps the viewport, re-applies the GUI camera. User surfaces
// are not implemented here, so the upstream user-surface flip never applies.
static void d3d9SetGuiProjection(Renderer* renderer, int32_t guiW, int32_t guiH,
                                 int32_t portW, int32_t portH, bool renderingToUserSurface) {
    (void)portW; (void)portH;
    // Upstream flips the GUI projection itself when drawing into a user surface.
    ((D3D9Renderer*)renderer)->hostTargetFlipY = !renderingToUserSurface;
    d3d9ApplyGuiCamera((D3D9Renderer*)renderer, guiW, guiH);
}
// GUI/Pre/Post ports arrive in window (back-buffer) pixels. As in upstream
// GLCommon_beginGUI the GUI is letterboxed by the viewport, not the projection.
static void d3d9BeginGUI(Renderer* renderer, int32_t guiW, int32_t guiH,
                         int32_t portX, int32_t portY, int32_t portW, int32_t portH,
                         int32_t targetSurfaceId) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    flushBatch(dr);
    // surface_reset_target re-enters beginGUI; save the room's world matrix only once.
    if (!dr->guiActive) {
        dr->savedGuiWorld = renderer->gmlMatrices[MATRIX_WORLD];
        dr->guiActive = true;
    }
    Matrix4f_identity(&renderer->gmlMatrices[MATRIX_WORLD]);

    int32_t x, y, w, h;
    D3D9Transform_letterbox(guiW, guiH, portW, portH, &x, &y, &w, &h);
    d3d9BindTarget(dr, targetSurfaceId);
    D3D9Surface* target = d3d9Surface(dr, targetSurfaceId);
    if (target) { // surface passes (e.g. Draw Pre into application_surface) fill the surface
        x = y = 0; w = target->w; h = target->h; portX = portY = 0;
    }
    dr->viewportX = (float)(portX + x);
    dr->viewportY = (float)(portY + y);
    dr->viewportW = (float)w;
    dr->viewportH = (float)h;
    // The GUI camera puts the top at clip -1 on every target (see d3d9_transform.h).
    dr->hostTargetFlipY = true;
    d3d9ApplyGuiCamera(dr, guiW, guiH);
}
static void d3d9EndGUI(Renderer* renderer) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    flushBatch(dr);
    if (dr->guiActive) {
        dr->guiActive = false;
        renderer->gmlMatrices[MATRIX_WORLD] = dr->savedGuiWorld;
    }
    dr->hostTargetFlipY = false;
    Renderer_applyProjection(renderer, &renderer->gmlMatrices[MATRIX_VIEW],
                             &renderer->gmlMatrices[MATRIX_PROJECTION]);
    d3d9RebuildTransform(dr);
}

static void d3d9DrawSpritePos(Renderer* renderer, int32_t tpagIndex,
                              float x1, float y1, float x2, float y2,
                              float x3, float y3, float x4, float y4, float alpha) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    DataWin* dw = renderer->dataWin;
    if (!dw || tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int32_t page = tpag->texturePageId;
    if (page < 0 || (uint32_t)page >= dr->textureCount || !d3d9Page(dr, page)) return;
    ensureTexture(dr, page);
    float tw = (float)dr->textureWidths[page], th = (float)dr->textureHeights[page];
    if (tw <= 0.0f || th <= 0.0f) return;
    float cr, cg, cb, ca;
    bgrToFloatColor(0xFFFFFF, alpha, &cr, &cg, &cb, &ca);
    SpriteVertex* v = allocQuad(dr);
    float sx, sy;
    transformPoint(dr, x1, y1, &sx, &sy);
    setVertex(&v[0], sx, sy, (float)tpag->sourceX / tw, (float)tpag->sourceY / th, cr, cg, cb, ca);
    transformPoint(dr, x2, y2, &sx, &sy);
    setVertex(&v[1], sx, sy, (float)(tpag->sourceX + tpag->sourceWidth) / tw, (float)tpag->sourceY / th, cr, cg, cb, ca);
    transformPoint(dr, x3, y3, &sx, &sy);
    setVertex(&v[2], sx, sy, (float)(tpag->sourceX + tpag->sourceWidth) / tw, (float)(tpag->sourceY + tpag->sourceHeight) / th, cr, cg, cb, ca);
    transformPoint(dr, x4, y4, &sx, &sy);
    setVertex(&v[3], sx, sy, (float)tpag->sourceX / tw, (float)(tpag->sourceY + tpag->sourceHeight) / th, cr, cg, cb, ca);
}
static void d3d9DrawRectangleColor(Renderer* renderer, float x1, float y1, float x2, float y2,
                                   uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4,
                                   float alpha, bool outline) {
    if (outline) {
        d3d9DrawLineColor(renderer, x1, y1, x2, y1, 1.0f, c1, c2, alpha);
        d3d9DrawLineColor(renderer, x2, y1, x2, y2, 1.0f, c2, c3, alpha);
        d3d9DrawLineColor(renderer, x2, y2, x1, y2, 1.0f, c3, c4, alpha);
        d3d9DrawLineColor(renderer, x1, y2, x1, y1, 1.0f, c4, c1, alpha);
        return;
    }
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    ensureTexture(dr, -1);
    SpriteVertex* v = allocQuad(dr);
    float x, y, r, g, b, a;
    transformPoint(dr, x1, y1, &x, &y);
    bgrToFloatColor(c1, alpha, &r, &g, &b, &a); setVertex(&v[0], x, y, 0, 0, r, g, b, a);
    transformPoint(dr, x2, y1, &x, &y);
    bgrToFloatColor(c2, alpha, &r, &g, &b, &a); setVertex(&v[1], x, y, 1, 0, r, g, b, a);
    transformPoint(dr, x2, y2, &x, &y);
    bgrToFloatColor(c3, alpha, &r, &g, &b, &a); setVertex(&v[2], x, y, 1, 1, r, g, b, a);
    transformPoint(dr, x1, y2, &x, &y);
    bgrToFloatColor(c4, alpha, &r, &g, &b, &a); setVertex(&v[3], x, y, 0, 1, r, g, b, a);
}
static void d3d9DrawTriangle(Renderer* renderer, float x1, float y1, float x2, float y2,
                             float x3, float y3, uint32_t c1, uint32_t c2, uint32_t c3,
                             float alpha, bool outline) {
    if (outline) {
        d3d9DrawLine(renderer, x1, y1, x2, y2, 1.0f, c1, alpha);
        d3d9DrawLine(renderer, x2, y2, x3, y3, 1.0f, c2, alpha);
        d3d9DrawLine(renderer, x3, y3, x1, y1, 1.0f, c3, alpha);
        return;
    }
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    if (!dr->sceneActive) return;
    flushBatch(dr);
    IDirect3DDevice9* dev = Dev(dr);
    dev->SetTexture(0, (IDirect3DBaseTexture9*)dr->whiteTexture);
    SpriteVertex v[3];
    float x, y, r, g, b, a;
    transformPoint(dr, x1, y1, &x, &y); bgrToFloatColor(c1, alpha, &r, &g, &b, &a);
    setVertex(&v[0], x, y, 0, 0, r, g, b, a);
    transformPoint(dr, x2, y2, &x, &y); bgrToFloatColor(c2, alpha, &r, &g, &b, &a);
    setVertex(&v[1], x, y, 0, 0, r, g, b, a);
    transformPoint(dr, x3, y3, &x, &y); bgrToFloatColor(c3, alpha, &r, &g, &b, &a);
    setVertex(&v[2], x, y, 0, 0, r, g, b, a);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(SpriteVertex));
    dr->currentTextureIndex = -1;
}
static void d3d9DrawTextModern(Renderer* renderer, const char* text, float x, float y,
                               float xs, float ys, float angle, float lineSeparation) {
    d3d9DrawText(renderer, text, x, y, xs, ys, angle, lineSeparation, NULL, renderer->drawAlpha);
}
static void d3d9DrawTextColorModern(Renderer* renderer, const char* text, float x, float y,
                                   float xs, float ys, float angle, int32_t c1, int32_t c2,
                                   int32_t c3, int32_t c4, float alpha, float lineSeparation) {
    uint32_t corners[4] = { (uint32_t)c1, (uint32_t)c2, (uint32_t)c3, (uint32_t)c4 };
    d3d9DrawText(renderer, text, x, y, xs, ys, angle, lineSeparation, corners, alpha);
}
static void d3d9DrawSpriteTiled(Renderer* renderer, int32_t tpagIndex, float originX, float originY,
                                float x, float y, float xs, float ys, bool tileX, bool tileY,
                                float roomW, float roomH, uint32_t color, float alpha) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    if (!renderer->dataWin || tpagIndex < 0 || (uint32_t)tpagIndex >= renderer->dataWin->tpag.count ||
        xs == 0.0f || ys == 0.0f) return;
    TexturePageItem* t = &renderer->dataWin->tpag.items[tpagIndex];
    float stepX = (float)t->sourceWidth * (xs < 0 ? -xs : xs);
    float stepY = (float)t->sourceHeight * (ys < 0 ? -ys : ys);
    if (stepX <= 0.0f || stepY <= 0.0f) return;
    int nx = 1, ny = 1;
    if (tileX && roomW > 0.0f) {
        float countX = roomW / stepX;
        nx = countX >= 127.0f ? 128 : (int)countX + 1;
    }
    if (tileY && roomH > 0.0f) {
        float countY = roomH / stepY;
        ny = countY >= 127.0f ? 128 : (int)countY + 1;
    }
    for (int iy = 0; iy < ny; iy++) for (int ix = 0; ix < nx; ix++) {
        float dx = x + (tileX ? ix * stepX : 0.0f);
        float dy = y + (tileY ? iy * stepY : 0.0f);
        d3d9DrawSpritePart(renderer, tpagIndex, 0, 0, t->sourceWidth, t->sourceHeight,
                           dx - originX * xs, dy - originY * ys, xs, ys, color, alpha);
    }
    (void)dr;
}
static void d3d9DrawSurface(Renderer* renderer, int32_t surfaceID, int32_t sl, int32_t st,
                            int32_t sw, int32_t sh, float x, float y, float xs, float ys,
                            float angle, uint32_t color, float alpha) {
    // A surface cannot be drawn onto itself; one never resolved is still empty.
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    D3D9Surface* surf = d3d9Surface(dr, surfaceID);
    SURF_TRACE("SURF draw id=%d bound=%d content=%d src=%d,%d %dx%d at %.0f,%.0f scale %.2f,%.2f alpha=%.2f frame=%u\n",
               surfaceID, dr->boundTarget, surf ? surf->hasContent : -1, sl, st, sw, sh, x, y, xs, ys, alpha,
               dr->frameCounter);
    if (!surf || !surf->hasContent || surfaceID == dr->boundTarget) return;
    if (sw < 0) sw = surf->w - sl; // the core passes -1 for "the whole surface" (draw_surface, _ext, _stretched)
    if (sh < 0) sh = surf->h - st;
    if (sw <= 0 || sh <= 0) return;
    float px[4] = { 0.0f, sw * xs, sw * xs, 0.0f };
    float py[4] = { 0.0f, 0.0f, sh * ys, sh * ys };
    float rad = angle * (3.14159265f / 180.0f), cs = cosf(rad), sn = sinf(rad);
    float sx[4], sy[4];
    for (int i = 0; i < 4; i++) // rotate about (x, y) like the sprite paths
        transformPoint(dr, x + px[i] * cs - py[i] * sn, y + px[i] * sn + py[i] * cs, &sx[i], &sy[i]);
    d3d9DrawTextureQuad(dr, surf->tex, sx, sy,
                        (float)sl / surf->allocW, (float)st / surf->allocH,
                        (float)(sl + sw) / surf->allocW, (float)(st + sh) / surf->allocH, color, alpha);
}
static void d3d9DrawSurfaceTiled(Renderer* renderer, int32_t surfaceID, float x, float y,
                                float xs, float ys, float roomW, float roomH, uint32_t color, float alpha) {
    D3D9Surface* surf = d3d9Surface((D3D9Renderer*)renderer, surfaceID);
    if (!surf) return;
    float stepX = surf->w * fabsf(xs), stepY = surf->h * fabsf(ys);
    if (stepX < 1.0f || stepY < 1.0f) return;
    // Tile grid aligned to (x, y), covering the room (draw_surface_tiled).
    for (float ty = y - ceilf(y / stepY) * stepY; ty < roomH; ty += stepY)
        for (float tx = x - ceilf(x / stepX) * stepX; tx < roomW; tx += stepX)
            d3d9DrawSurface(renderer, surfaceID, 0, 0, surf->w, surf->h, tx, ty, xs, ys, 0.0f, color, alpha);
}
// draw_surface_general: same surface semantics as drawSurface (called unguarded by the core).
static void d3d9DrawSurfaceColor(Renderer* renderer, int32_t surfaceID, int32_t sl, int32_t st,
                                 int32_t sw, int32_t sh, float x, float y, float xs, float ys,
                                 float angle, uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4,
                                 float alpha) {
    (void)c2; (void)c3; (void)c4;
    d3d9DrawSurface(renderer, surfaceID, sl, st, sw, sh, x, y, xs, ys, angle, c1, alpha);
}

static void d3d9PrimitiveBegin(Renderer* renderer, int32_t primitiveType) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    if (!dr->initialized || !dr->primitiveVertexData || dr->primitiveVertexCapacity == 0) return;
    flushBatch(dr);
    dr->primitiveType = primitiveType;
    dr->primitiveTexture = 0;
    dr->primitiveVertexCount = 0;
    dr->primitiveActive = true;
}

static void d3d9PrimitiveBeginTexture(Renderer* renderer, int32_t primitiveType, int32_t texture) {
    d3d9PrimitiveBegin(renderer, primitiveType);
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    if (dr->primitiveActive) {
        dr->primitiveTexture = texture > 0 ? texture : 0;
        ensureTexture(dr, d3d9TextureHandleToPage(dr, (uint32_t)dr->primitiveTexture));
    }
}

static void d3d9DrawVertex(Renderer* renderer, float x, float y, float z,
                           uint32_t color, float alpha, float u, float v) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    if (!dr->primitiveActive || !dr->primitiveVertexData ||
        dr->primitiveVertexCount >= dr->primitiveVertexCapacity) return;

    SpriteVertex* vertices = (SpriteVertex*)dr->primitiveVertexData;
    SpriteVertex* vertex = &vertices[dr->primitiveVertexCount++];
    float sx, sy, cr, cg, cb, ca;
    transformPoint(dr, x, y, &sx, &sy);
    bgrToFloatColor(color, alpha, &cr, &cg, &cb, &ca);
    setVertex(vertex, sx, sy, u, v, cr, cg, cb, ca);
    vertex->z = z;
}

static void d3d9PrimitiveEnd(Renderer* renderer) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    if (!dr->primitiveActive) return;
    flushBatch(dr);

    D3DPRIMITIVETYPE type;
    uint32_t primitiveCount = 0;
    switch (dr->primitiveType) {
        case PRIMITIVE_POINTS:
            type = D3DPT_POINTLIST;
            primitiveCount = dr->primitiveVertexCount;
            break;
        case PRIMITIVE_LINES:
            type = D3DPT_LINELIST;
            primitiveCount = dr->primitiveVertexCount / 2;
            break;
        case PRIMITIVE_LINE_STRIP:
            type = D3DPT_LINESTRIP;
            primitiveCount = dr->primitiveVertexCount > 1 ? dr->primitiveVertexCount - 1 : 0;
            break;
        case PRIMITIVE_TRIANGLES:
            type = D3DPT_TRIANGLELIST;
            primitiveCount = dr->primitiveVertexCount / 3;
            break;
        case PRIMITIVE_TRIANGLE_STRIP:
            type = D3DPT_TRIANGLESTRIP;
            primitiveCount = dr->primitiveVertexCount > 2 ? dr->primitiveVertexCount - 2 : 0;
            break;
        case PRIMITIVE_TRIANGLE_FAN:
            type = D3DPT_TRIANGLEFAN;
            primitiveCount = dr->primitiveVertexCount > 2 ? dr->primitiveVertexCount - 2 : 0;
            break;
        default:
            primitiveCount = 0;
            type = D3DPT_TRIANGLELIST;
            break;
    }

    if (primitiveCount > 0 && dr->sceneActive) {
        int32_t page = d3d9TextureHandleToPage(dr, (uint32_t)dr->primitiveTexture);
        IDirect3DBaseTexture9* texture = dr->whiteTexture
            ? (IDirect3DBaseTexture9*)dr->whiteTexture : NULL;
        void* chosen = d3d9TextureForIndex(dr, page);
        if (chosen) texture = (IDirect3DBaseTexture9*)chosen;
        HRESULT hr = Dev(dr)->SetTexture(0, texture);
        if (SUCCEEDED(hr)) {
            hr = Dev(dr)->DrawPrimitiveUP(type, primitiveCount,
                dr->primitiveVertexData, sizeof(SpriteVertex));
        }
        if (FAILED(hr)) logInfo("BS: primitive DrawPrimitiveUP failed: hr=0x%08X\n", (unsigned)hr);
    }

    dr->primitiveActive = false;
    dr->primitiveVertexCount = 0;
    dr->primitiveTexture = 0;
    dr->currentTextureIndex = -1;
}

// vertex_submit: decode the buffer on the CPU into the primitive path (positions go
// through screenTransform like every other draw). Lists are drawn in chunks.
static void d3d9DrawVertexBuffer(Renderer* renderer, VertexBuffer* vb, int32_t primitive,
                                 int32_t texture, int32_t offset, int32_t count) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    if (!vb || !vb->data || !vb->format || vb->vertexSize == 0 || !dr->primitiveVertexCapacity) return;
    const VertexFormat* f = vb->format;
    int32_t pos = -1, col = -1, uv = -1;
    for (int32_t e = 0; e < (int32_t)f->numElements; e++) {
        const VertexElement* el = &f->elements[e];
        if (el->usage == VERTEX_USAGE_POSITION && pos < 0) pos = e;
        else if (el->usage == VERTEX_USAGE_COLOR && col < 0) col = e;
        else if (el->usage == VERTEX_USAGE_TEXCOORD && uv < 0) uv = e;
    }
    if (pos < 0) return;
    int32_t total = (int32_t)(vb->size / vb->vertexSize);
    if (offset < 0) offset = 0;
    if (count < 0 || offset + count > total) count = total - offset;
    if (count <= 0) return;

    // Lists split cleanly at multiples of 6; strips and fans cannot be split.
    // shortcut: strips/fans beyond D3D9_MAX_PRIMITIVE_VERTICES are truncated.
    bool splittable = primitive == PRIMITIVE_POINTS || primitive == PRIMITIVE_LINES ||
                      primitive == PRIMITIVE_TRIANGLES;
    int32_t chunk = (int32_t)(dr->primitiveVertexCapacity / 6u * 6u);
    if (!splittable && count > chunk) count = chunk;

    while (count > 0) {
        int32_t n = count < chunk ? count : chunk;
        flushBatch(dr);
        SpriteVertex* out = (SpriteVertex*)dr->primitiveVertexData;
        for (int32_t i = 0; i < n; i++) {
            const uint8_t* v = vb->data + (size_t)(offset + i) * vb->vertexSize;
            const float* p = (const float*)(v + f->elements[pos].offset);
            float sx, sy, u = 0.0f, t = 0.0f, r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
            VertexType pt = f->elements[pos].type;
            transformPoint3(dr, p[0], pt == VERTEX_TYPE_FLOAT1 ? 0.0f : p[1],
                            pt == VERTEX_TYPE_FLOAT3 || pt == VERTEX_TYPE_FLOAT4 ? p[2] : 0.0f, &sx, &sy);
            if (uv >= 0) {
                const float* q = (const float*)(v + f->elements[uv].offset);
                u = q[0]; t = q[1];
            }
            if (col >= 0) { // vertex_colour stores a native uint32: BGR | alpha << 24
                uint32_t c = *(const uint32_t*)(v + f->elements[col].offset);
                bgrToFloatColor(c & 0xFFFFFFu, (float)(c >> 24) / 255.0f, &r, &g, &b, &a);
            }
            setVertex(&out[i], sx, sy, u, t, r, g, b, a);
        }
        dr->primitiveVertexCount = (uint32_t)n;
        dr->primitiveType = primitive;
        dr->primitiveTexture = texture < 0 ? 0 : texture;
        dr->primitiveActive = true;
        d3d9PrimitiveEnd(renderer);
        offset += n;
        count -= n;
    }
}

// ===[ Surfaces ]===
// Every surface (application_surface included) is an EDRAM render target plus a
// texture it is resolved into (design after Butterscotch360-Next's notes).
// EDRAM only holds the bound target, so d3d9BindTarget resolves the target it
// leaves and redraws the last contents of the one it enters.

static bool d3d9CreateSurfaceObjects(D3D9Renderer* dr, D3D9Surface* s, int32_t w, int32_t h) {
    IDirect3DDevice9* dev = Dev(dr);
    int32_t allocW = (w + 7) & ~7, allocH = (h + 7) & ~7; // same alignment as the Refresh hotfix
    IDirect3DTexture9* tex = NULL;
    IDirect3DSurface9* rt = NULL;
    // shortcut: targets larger than EDRAM (10 MB) fail here; predicated tiling would lift that.
    HRESULT hr = dev->CreateTexture((UINT)allocW, (UINT)allocH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, NULL);
    if (SUCCEEDED(hr) && tex) {
        // All targets share EDRAM from tile 0: only one is bound at a time and
        // d3d9BindTarget resolves it out before switching. Without this, D3D gives
        // each its own EDRAM slice and creation fails once the 10 MB is used.
        D3DSURFACE_PARAMETERS edram;
        memset(&edram, 0, sizeof(edram));
        hr = dev->CreateRenderTarget((UINT)allocW, (UINT)allocH, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, &edram);
    }
    if (FAILED(hr) || !tex || !rt) {
        logWarn("D3D9: surface %dx%d unavailable (hr=0x%08X)\n", w, h, (unsigned)hr);
        if (tex) tex->Release();
        if (rt) rt->Release();
        return false;
    }
    s->tex = tex;
    s->rt = rt;
    s->w = w;
    s->h = h;
    s->allocW = allocW;
    s->allocH = allocH;
    s->exists = true;
    s->hasContent = false;
    return true;
}

static void d3d9ReleaseSurfaceObjects(D3D9Renderer* dr, D3D9Surface* s) {
    if (s->tex || s->rt) {
        if (dr->sceneActive) { // the GPU may still sample it: Xenon does not track lifetimes
            flushBatch(dr);
            Dev(dr)->BlockUntilIdle();
        }
        if (s->rt) ((IDirect3DSurface9*)s->rt)->Release();
        if (s->tex) ((IDirect3DTexture9*)s->tex)->Release();
    }
    memset(s, 0, sizeof(*s));
}

static void d3d9FreeSurfaces(D3D9Renderer* dr) {
    D3D9Surface* s = (D3D9Surface*)dr->surfaces;
    for (int32_t i = 0; i < (int32_t)arrlen(s); i++) d3d9ReleaseSurfaceObjects(dr, &s[i]);
    arrfree(s);
    dr->surfaces = NULL;
    if (dr->hostCopy) ((IDirect3DTexture9*)dr->hostCopy)->Release();
    dr->hostCopy = NULL;
}

// Opaque copy of a texture region onto the bound target at pixel (dx, dy), with
// the default shader and full colour writes (restores and surface_copy).
static void d3d9BlitTexture(D3D9Renderer* dr, void* tex, int32_t allocW, int32_t allocH,
                            int32_t sx, int32_t sy, int32_t w, int32_t h, float dx, float dy) {
    IDirect3DDevice9* dev = Dev(dr);
    DWORD blend = TRUE, colorWrite = 0xF;
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
    dev->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite);
    IDirect3DPixelShader9* shader = NULL;
    dev->GetPixelShader(&shader);
    flushBatch(dr);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetPixelShader((IDirect3DPixelShader9*)dr->pPixelShader);
    static const float noFog[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    dev->SetPixelShaderConstantF(D3D9_FOG_REGISTER, noFog, 1);
    float xs[4] = { dx, dx + w, dx + w, dx }, ys[4] = { dy, dy, dy + h, dy + h };
    d3d9DrawTextureQuad(dr, tex, xs, ys, (float)sx / allocW, (float)sy / allocH,
                        (float)(sx + w) / allocW, (float)(sy + h) / allocH, 0xFFFFFF, 1.0f);
    dev->SetPixelShader(shader);
    dev->SetPixelShaderConstantF(D3D9_FOG_REGISTER, dr->fog, 1);
    if (shader) shader->Release();
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, colorWrite);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, blend);
}

// Switches the render target: id >= 0 is a surface, -1 the back buffer.
static bool d3d9BindTarget(D3D9Renderer* dr, int32_t id) {
    D3D9Surface* next = d3d9Surface(dr, id);
    if (id != -1 && !next) return false;
    if (id == dr->boundTarget) return true;
    if (!dr->sceneActive) return false;
    IDirect3DDevice9* dev = Dev(dr);
    flushBatch(dr);
    int32_t hostW = (dr->windowW + 7) & ~7, hostH = (dr->windowH + 7) & ~7;
    D3D9Surface* current = d3d9Surface(dr, dr->boundTarget);
    if (current) {
        dev->Resolve(D3DRESOLVE_RENDERTARGET0, NULL, (IDirect3DBaseTexture9*)current->tex,
                     NULL, 0, 0, NULL, 1.0f, 0, NULL);
        current->hasContent = true;
    } else if (dr->boundTarget == -1 && dr->hostDirty) { // e.g. surface drawing inside Draw GUI
        if (!dr->hostCopy)
            dev->CreateTexture((UINT)hostW, (UINT)hostH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                               (IDirect3DTexture9**)&dr->hostCopy, NULL);
        if (dr->hostCopy) {
            dev->Resolve(D3DRESOLVE_RENDERTARGET0, NULL, (IDirect3DBaseTexture9*)dr->hostCopy,
                         NULL, 0, 0, NULL, 1.0f, 0, NULL);
            dr->hostHasCopy = true;
        }
    }
    if (next) {
        dev->SetRenderTarget(0, (IDirect3DSurface9*)next->rt);
    } else {
        IDirect3DSurface9* backBuffer = NULL;
        if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) && backBuffer) {
            dev->SetRenderTarget(0, backBuffer);
            backBuffer->Release();
        }
    }
    dr->boundTarget = id;
    if (next) { // surfaces are drawn at their own resolution; only the back buffer is letterboxed
        dr->renderScale = 1.0f;
        dr->renderOffsetX = dr->renderOffsetY = 0.0f;
        if (next->hasContent)
            d3d9BlitTexture(dr, next->tex, next->allocW, next->allocH, 0, 0, next->w, next->h, 0.0f, 0.0f);
    } else {
        d3d9ComputeLetterbox(dr);
        dr->hostDirty = true;
        if (dr->hostHasCopy)
            d3d9BlitTexture(dr, dr->hostCopy, hostW, hostH, 0, 0, dr->windowW, dr->windowH, 0.0f, 0.0f);
    }
    return true;
}

static int32_t d3d9CreateSurface(Renderer* r, int32_t w, int32_t h) {
    D3D9Renderer* dr = (D3D9Renderer*)r;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return -1;
    D3D9Surface* s = (D3D9Surface*)dr->surfaces;
    int32_t id = 0;
    while (id < (int32_t)arrlen(s) && s[id].exists) id++;
    if (id == (int32_t)arrlen(s)) {
        D3D9Surface blank;
        memset(&blank, 0, sizeof(blank));
        arrput(s, blank);
        dr->surfaces = s;
    }
    bool made = d3d9CreateSurfaceObjects(dr, &s[id], w, h);
    SURF_TRACE("SURF create %dx%d -> %d\n", w, h, made ? id : -1);
    return made ? id : -1;
}
static bool d3d9SurfaceExists(Renderer* r, int32_t id) {
    return d3d9Surface((D3D9Renderer*)r, id) != NULL;
}
static bool d3d9SetRenderTarget(Renderer* r, int32_t id, bool implicit) {
    D3D9Renderer* dr = (D3D9Renderer*)r;
    SURF_TRACE("SURF target %d (implicit=%d) from %d\n", id, implicit, dr->boundTarget);
    if (!d3d9BindTarget(dr, id)) return false;
    D3D9Surface* surf = d3d9Surface(dr, id);
    if (!surf) return true; // the back buffer: GUI passes re-apply their camera via beginGUI
    // Like upstream glSetRenderTarget: the view's own surface (or the application
    // surface on an implicit restore) uses the view camera, others the full surface.
    dr->hostTargetFlipY = false;
    d3d9SetGameViewport(dr, 0.0f, 0.0f, (float)surf->w, (float)surf->h);
    Runner* runner = r->runner;
    RuntimeView* view = runner ? &runner->views[runner->viewsEnabled ? runner->viewCurrent : 0] : NULL;
    GMLCamera* camera = NULL;
    if (view && ((implicit && id == dr->appSurface) || id == view->surfaceId)) {
        r->cameraCurrent = view->cameraId;
        camera = Runner_getCameraById(runner, view->cameraId);
    }
    if (camera) {
        d3d9SetProjection(dr, &camera->viewMatrix, &camera->projectionMatrix);
        return true;
    }
    Matrix4f viewMatrix, projection;
    Matrix4f_identity(&viewMatrix);
    Matrix4f_viewProjection(&projection, 0.0f, 0.0f, (float)surf->w, (float)surf->h, 0.0f);
    r->cameraCurrent = SURFACE_CAMERA;
    if (runner) {
        GMLCamera* sc = &runner->surfaceCamera;
        sc->allocated = true;
        sc->viewX = sc->viewY = 0.0f;
        sc->viewWidth = surf->w;
        sc->viewHeight = surf->h;
        sc->viewAngle = 0.0f;
        sc->objectId = -1;
        sc->viewMatrix = viewMatrix;
        sc->projectionMatrix = projection;
    }
    d3d9SetProjection(dr, &viewMatrix, &projection);
    return true;
}
// Called by Runner_beginFrame before beginFrame. application_surface is an
// ordinary surface id; if it cannot be created the game draws to the back buffer.
static int32_t d3d9EnsureApplicationSurface(Renderer* r, int32_t w, int32_t h) {
    D3D9Renderer* dr = (D3D9Renderer*)r;
    D3D9Surface* app = d3d9Surface(dr, dr->appSurface);
    if (w <= 0 || h <= 0 || (app && app->w == w && app->h == h)) return dr->appSurface;
    if (app) {
        d3d9ReleaseSurfaceObjects(dr, app);
        if (!d3d9CreateSurfaceObjects(dr, app, w, h)) dr->appSurface = -1;
    } else {
        dr->appSurface = d3d9CreateSurface(r, w, h);
    }
    if (dr->appSurface >= 0) logInfo("D3D9: application_surface %dx%d (surface %d)\n", w, h, dr->appSurface);
    return dr->appSurface;
}
static float d3d9GetSurfaceWidth(Renderer* r, int32_t id) {
    D3D9Surface* s = d3d9Surface((D3D9Renderer*)r, id);
    return s ? (float)s->w : 0.0f;
}
static float d3d9GetSurfaceHeight(Renderer* r, int32_t id) {
    D3D9Surface* s = d3d9Surface((D3D9Renderer*)r, id);
    return s ? (float)s->h : 0.0f;
}
static void d3d9SurfaceResize(Renderer* r, int32_t id, int32_t w, int32_t h) {
    D3D9Renderer* dr = (D3D9Renderer*)r;
    D3D9Surface* s = d3d9Surface(dr, id);
    // shortcut: a surface cannot be resized while it is the bound target.
    if (!s || w <= 0 || h <= 0 || (s->w == w && s->h == h) || id == dr->boundTarget) return;
    d3d9ReleaseSurfaceObjects(dr, s);
    d3d9CreateSurfaceObjects(dr, s, w, h); // contents are lost, as in GameMaker
}
static void d3d9SurfaceFree(Renderer* r, int32_t id) {
    D3D9Renderer* dr = (D3D9Renderer*)r;
    D3D9Surface* s = d3d9Surface(dr, id);
    if (!s) return;
    if (id == dr->boundTarget) d3d9BindTarget(dr, -1);
    d3d9ReleaseSurfaceObjects(dr, s);
    if (id == dr->appSurface) dr->appSurface = -1;
}
// surface_copy / surface_copy_part: opaque copy of src's contents into dest.
static void d3d9SurfaceCopy(Renderer* r, int32_t dest, int32_t dx, int32_t dy, int32_t src,
                            int32_t sx, int32_t sy, int32_t w, int32_t h, bool part) {
    D3D9Renderer* dr = (D3D9Renderer*)r;
    D3D9Surface* from = d3d9Surface(dr, src);
    if (!from || dest == src || !d3d9Surface(dr, dest)) return;
    int32_t previous = dr->boundTarget;
    if (!d3d9BindTarget(dr, dest)) return; // also resolves `src` if it was bound
    if (!part) { sx = sy = 0; w = from->w; h = from->h; }
    if (from->hasContent && w > 0 && h > 0)
        d3d9BlitTexture(dr, from->tex, from->allocW, from->allocH, sx, sy, w, h, (float)dx, (float)dy);
    d3d9BindTarget(dr, previous);
}
// shortcut: no CPU readback (surface_getpixel, buffer_get_surface); resolved
// textures are tiled in Xenon memory and would need XGUntileSurface.
static bool d3d9SurfaceGetPixels(Renderer* r, int32_t id, uint8_t* rgba) { (void)r; (void)id; (void)rgba; return false; }
static uint32_t d3d9SpriteGetTexture(Renderer* r, int32_t tpagIndex) {
    D3D9Renderer* dr = (D3D9Renderer*)r;
    DataWin* dw = r->dataWin;
    if (!dw || tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) return 0;
    int32_t page = dw->tpag.items[tpagIndex].texturePageId;
    if (page < 0 || (uint32_t)page >= dr->textureCount || !d3d9PageEx(dr, page, true)) return 0;
    return (uint32_t)tpagIndex + 1;
}
static uint32_t d3d9SurfaceGetTexture(Renderer* r, int32_t i) {
    return d3d9Surface((D3D9Renderer*)r, i) ? D3D9_SURFACE_HANDLE + (uint32_t)i : 0;
}
static float d3d9TextureGetTexelWidth(Renderer* r, uint32_t texID) {
    if (texID >= D3D9_SURFACE_HANDLE) {
        D3D9Surface* surf = d3d9Surface((D3D9Renderer*)r, (int32_t)(texID - D3D9_SURFACE_HANDLE));
        return surf ? 1.0f / (float)surf->allocW : 0.0f;
    }
    DataWin* dw = r->dataWin;
    if (!dw || texID == 0 || texID - 1 >= dw->tpag.count) return 0.0f;
    D3D9Renderer* dr = (D3D9Renderer*)r;
    int32_t page = dw->tpag.items[texID - 1].texturePageId;
    if (page < 0 || (uint32_t)page >= dr->textureCount || !d3d9PageEx(dr, page, true) || dr->textureWidths[page] <= 0) return 0.0f;
    return 1.0f / (float)dr->textureWidths[page];
}
static float d3d9TextureGetTexelHeight(Renderer* r, uint32_t texID) {
    if (texID >= D3D9_SURFACE_HANDLE) {
        D3D9Surface* surf = d3d9Surface((D3D9Renderer*)r, (int32_t)(texID - D3D9_SURFACE_HANDLE));
        return surf ? 1.0f / (float)surf->allocH : 0.0f;
    }
    DataWin* dw = r->dataWin;
    if (!dw || texID == 0 || texID - 1 >= dw->tpag.count) return 0.0f;
    D3D9Renderer* dr = (D3D9Renderer*)r;
    int32_t page = dw->tpag.items[texID - 1].texturePageId;
    if (page < 0 || (uint32_t)page >= dr->textureCount || !d3d9PageEx(dr, page, true) || dr->textureHeights[page] <= 0) return 0.0f;
    return 1.0f / (float)dr->textureHeights[page];
}
static bool d3d9TextureGetUVs(Renderer* r, uint32_t texID, float* uv) {
    if (texID >= D3D9_SURFACE_HANDLE) {
        D3D9Surface* surf = d3d9Surface((D3D9Renderer*)r, (int32_t)(texID - D3D9_SURFACE_HANDLE));
        if (!surf || !uv) return false;
        uv[0] = uv[1] = 0.0f;
        uv[2] = (float)surf->w / surf->allocW;
        uv[3] = (float)surf->h / surf->allocH;
        return true;
    }
    DataWin* dw = r->dataWin;
    if (!dw || !uv || texID == 0 || texID - 1 >= dw->tpag.count) return false;
    D3D9Renderer* dr = (D3D9Renderer*)r;
    TexturePageItem* t = &dw->tpag.items[texID - 1];
    int32_t page = t->texturePageId;
    if (page < 0 || (uint32_t)page >= dr->textureCount || !d3d9PageEx(dr, page, true) ||
        dr->textureWidths[page] <= 0 || dr->textureHeights[page] <= 0) return false;
    float w = (float)dr->textureWidths[page], h = (float)dr->textureHeights[page];
    uv[0] = (float)t->sourceX / w;
    uv[1] = (float)t->sourceY / h;
    uv[2] = (float)(t->sourceX + t->sourceWidth) / w;
    uv[3] = (float)(t->sourceY + t->sourceHeight) / h;
    return true;
}
static void d3d9TextureSetStage(Renderer* r, int32_t slot, uint32_t texID) {
    D3D9Renderer* dr = (D3D9Renderer*)r;
    if (slot > 0 && slot < 16) { // extra samplers for shaders (palettes, LUTs)
        flushBatch(dr);
        int32_t page = d3d9TextureHandleToPage(dr, texID);
        IDirect3DDevice9* dev = Dev(dr);
        dev->SetTexture(slot, page != -1 ? (IDirect3DBaseTexture9*)d3d9TextureForIndex(dr, page) : NULL);
        dev->SetSamplerState(slot, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(slot, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(slot, D3DSAMP_ADDRESSU, (dr->texRepeat ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP));
        dev->SetSamplerState(slot, D3DSAMP_ADDRESSV, (dr->texRepeat ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP));
        return;
    }
    if (slot != 0) return;
    ensureTexture(dr, d3d9TextureHandleToPage(dr, texID));
}
// ===[ GameMaker shaders ]===
// Pixel shaders only: the fixed vertex shader already provides v_vTexcoord and
// v_vColour. Uniform handles index uniformHandles (shader + D3DX constant).
typedef struct { int32_t shader; D3DXHANDLE handle; } D3D9UniformRef;
char g_d3d9ShaderDir[256] = "game:\\shaders\\";

static void d3d9LoadUserShaders(D3D9Renderer* dr, DataWin* dw) {
    dr->userShaderCount = dw->shdr.count;
    if (dr->userShaderCount == 0) return;
    dr->userPixelShaders = (void**)calloc(dr->userShaderCount, sizeof(void*));
    dr->userConstantTables = (void**)calloc(dr->userShaderCount, sizeof(void*));
    if (!dr->userPixelShaders || !dr->userConstantTables) { dr->userShaderCount = 0; return; }
    uint32_t loaded = 0;
    for (uint32_t i = 0; i < dr->userShaderCount; i++) {
        const char* name = dw->shdr.shaders[i].name;
        char path[256];
        if (!name || snprintf(path, sizeof(path), "%s%s.xps", g_d3d9ShaderDir, name) >= (int)sizeof(path)) continue;
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        DWORD* code = size > 0 && size < 1024 * 1024 ? (DWORD*)malloc((size_t)size) : NULL;
        if (code && fread(code, 1, (size_t)size, f) == (size_t)size &&
            SUCCEEDED(Dev(dr)->CreatePixelShader(code, (IDirect3DPixelShader9**)&dr->userPixelShaders[i]))) {
            D3DXGetShaderConstantTable(code, (ID3DXConstantTable**)&dr->userConstantTables[i]);
            loaded++;
        }
        free(code);
        fclose(f);
    }
    logInfo("D3D9: loaded %u/%u precompiled shaders\n", loaded, dr->userShaderCount);
}

static void d3d9ReleaseUserShaders(D3D9Renderer* dr) {
    for (uint32_t i = 0; i < dr->userShaderCount; i++) {
        if (dr->userPixelShaders[i]) ((IDirect3DPixelShader9*)dr->userPixelShaders[i])->Release();
        if (dr->userConstantTables[i]) ((ID3DXConstantTable*)dr->userConstantTables[i])->Release();
    }
    free(dr->userPixelShaders);
    free(dr->userConstantTables);
    D3D9UniformRef* refs = (D3D9UniformRef*)dr->uniformHandles;
    arrfree(refs);
    dr->uniformHandles = NULL;
    dr->userShaderCount = 0;
}

static ID3DXConstantTable* d3d9ShaderTable(D3D9Renderer* dr, int32_t shader) {
    if (shader < 0 || (uint32_t)shader >= dr->userShaderCount) return NULL;
    return (ID3DXConstantTable*)dr->userConstantTables[shader];
}

static void d3d9GpuSetShader(Renderer* r, int32_t i) {
    D3D9Renderer* dr = (D3D9Renderer*)r;
    flushBatch(dr);
    IDirect3DPixelShader9* ps = i >= 0 && (uint32_t)i < dr->userShaderCount
        ? (IDirect3DPixelShader9*)dr->userPixelShaders[i] : NULL;
    Dev(dr)->SetPixelShader(ps ? ps : (IDirect3DPixelShader9*)dr->pPixelShader);
}
static void d3d9GpuResetShader(Renderer* r) {
    D3D9Renderer* dr = (D3D9Renderer*)r;
    flushBatch(dr);
    Dev(dr)->SetPixelShader((IDirect3DPixelShader9*)dr->pPixelShader);
}
static int32_t d3d9ShaderGetUniform(Renderer* r, int32_t s, char* n) {
    D3D9Renderer* dr = (D3D9Renderer*)r;
    ID3DXConstantTable* table = d3d9ShaderTable(dr, s);
    D3DXHANDLE handle = table && n ? table->GetConstantByName(NULL, n) : NULL;
    if (!handle) return -1;
    D3D9UniformRef* refs = (D3D9UniformRef*)dr->uniformHandles;
    for (int32_t k = 0; k < (int32_t)arrlen(refs); k++)
        if (refs[k].shader == s && refs[k].handle == handle) return k;
    D3D9UniformRef ref = { s, handle };
    arrput(refs, ref);
    dr->uniformHandles = refs;
    return (int32_t)arrlen(refs) - 1;
}
static int32_t d3d9ShaderGetSamplerIndex(Renderer* r, int32_t s, char* n) {
    ID3DXConstantTable* table = d3d9ShaderTable((D3D9Renderer*)r, s);
    D3DXHANDLE handle = table && n ? table->GetConstantByName(NULL, n) : NULL;
    return handle ? (int32_t)table->GetSamplerIndex(handle) : -1;
}
// Resolves a uniform handle to its constant table, flushing draws queued under the old value.
static ID3DXConstantTable* d3d9UniformTarget(D3D9Renderer* dr, int32_t h, D3DXHANDLE* outHandle) {
    D3D9UniformRef* refs = (D3D9UniformRef*)dr->uniformHandles;
    if (h < 0 || h >= (int32_t)arrlen(refs)) return NULL;
    ID3DXConstantTable* table = d3d9ShaderTable(dr, refs[h].shader);
    if (!table) return NULL;
    flushBatch(dr);
    *outHandle = refs[h].handle;
    return table;
}
static void d3d9ShaderSetUniformFArray(Renderer* r, int32_t h, float* values, uint32_t count) {
    D3D9Renderer* dr = (D3D9Renderer*)r;
    D3DXHANDLE handle;
    ID3DXConstantTable* table = d3d9UniformTarget(dr, h, &handle);
    if (table && values) table->SetFloatArray(Dev(dr), handle, values, count);
}
static void d3d9ShaderSetUniformF(Renderer* r, int32_t h, int32_t c, float a, float b, float d, float e) {
    float values[4] = { a, b, d, e };
    d3d9ShaderSetUniformFArray(r, h, values, (uint32_t)(c < 1 ? 1 : c > 4 ? 4 : c));
}
static void d3d9ShaderSetUniformI(Renderer* r, int32_t h, int32_t c, int32_t a, int32_t b, int32_t d, int32_t e) {
    D3D9Renderer* dr = (D3D9Renderer*)r;
    D3DXHANDLE handle;
    ID3DXConstantTable* table = d3d9UniformTarget(dr, h, &handle);
    INT values[4] = { a, b, d, e };
    if (table) table->SetIntArray(Dev(dr), handle, values, (UINT)(c < 1 ? 1 : c > 4 ? 4 : c));
}
static bool d3d9ShaderIsCompiled(Renderer* r, int32_t i) {
    D3D9Renderer* dr = (D3D9Renderer*)r;
    return i >= 0 && (uint32_t)i < dr->userShaderCount && dr->userPixelShaders[i] != NULL;
}
static bool d3d9ShadersSupported(void) { return true; }

static BlendFactors d3d9GpuGetBlendFactors(Renderer* r) {
    return r->blendFactors;
}
static int32_t d3d9GpuGetBlendMode(Renderer* r) {
    return ((D3D9Renderer*)r)->blendMode;
}
static bool d3d9GpuGetAlphaTestEnable(Renderer* r) {
    DWORD value = FALSE;
    Dev((D3D9Renderer*)r)->GetRenderState(D3DRS_ALPHATESTENABLE, &value);
    return value != FALSE;
}

static void d3d9GpuSetBlendEnable(Renderer* r, bool enable) {
    flushBatch((D3D9Renderer*)r);
    Dev((D3D9Renderer*)r)->SetRenderState(D3DRS_ALPHABLENDENABLE, enable ? TRUE : FALSE);
}
static bool d3d9GpuGetBlendEnable(Renderer* r) {
    DWORD value = FALSE;
    Dev((D3D9Renderer*)r)->GetRenderState(D3DRS_ALPHABLENDENABLE, &value);
    return value != FALSE;
}
static void d3d9GpuSetTexFilter(Renderer* r, bool enable) {
    flushBatch((D3D9Renderer*)r);
    IDirect3DDevice9* d = Dev((D3D9Renderer*)r);
    DWORD f = enable ? D3DTEXF_LINEAR : D3DTEXF_POINT;
    d->SetSamplerState(0, D3DSAMP_MINFILTER, f);
    d->SetSamplerState(0, D3DSAMP_MAGFILTER, f);
}
static void d3d9GpuSetAlphaTestEnable(Renderer* r, bool enable) {
    flushBatch((D3D9Renderer*)r);
    Dev((D3D9Renderer*)r)->SetRenderState(D3DRS_ALPHATESTENABLE, enable ? TRUE : FALSE);
}
static void d3d9GpuSetAlphaTestRef(Renderer* r, uint8_t ref) {
    flushBatch((D3D9Renderer*)r);
    Dev((D3D9Renderer*)r)->SetRenderState(D3DRS_ALPHAREF, ref);
}
static void d3d9GpuSetFog(Renderer* r, bool enable, uint32_t color, float start, float end) {
    (void)start; (void)end; // 2D fog is all-or-nothing, as in GameMaker
    D3D9Renderer* dr = (D3D9Renderer*)r;
    flushBatch(dr);
    bgrToFloatColor(color, enable ? 1.0f : 0.0f, &dr->fog[0], &dr->fog[1], &dr->fog[2], &dr->fog[3]);
    Dev(dr)->SetPixelShaderConstantF(D3D9_FOG_REGISTER, dr->fog, 1);
}

static void d3d9GpuSetColorWriteEnable(Renderer* r, bool red, bool green, bool blue, bool alpha) {
    flushBatch((D3D9Renderer*)r);
    DWORD mask = (red ? D3DCOLORWRITEENABLE_RED : 0) |
                 (green ? D3DCOLORWRITEENABLE_GREEN : 0) |
                 (blue ? D3DCOLORWRITEENABLE_BLUE : 0) |
                 (alpha ? D3DCOLORWRITEENABLE_ALPHA : 0);
    Dev((D3D9Renderer*)r)->SetRenderState(D3DRS_COLORWRITEENABLE, mask);
}
static void d3d9GpuGetColorWriteEnable(Renderer* r, bool* red, bool* green, bool* blue, bool* alpha) {
    DWORD mask = 0;
    Dev((D3D9Renderer*)r)->GetRenderState(D3DRS_COLORWRITEENABLE, &mask);
    if (red) *red = (mask & D3DCOLORWRITEENABLE_RED) != 0;
    if (green) *green = (mask & D3DCOLORWRITEENABLE_GREEN) != 0;
    if (blue) *blue = (mask & D3DCOLORWRITEENABLE_BLUE) != 0;
    if (alpha) *alpha = (mask & D3DCOLORWRITEENABLE_ALPHA) != 0;
}
static DWORD d3d9MapBlendFactor(int32_t factor) {
    switch (factor) {
        case bm_zero: return D3DBLEND_ZERO;
        case bm_one: return D3DBLEND_ONE;
        case bm_src_color: return D3DBLEND_SRCCOLOR;
        case bm_inv_src_color: return D3DBLEND_INVSRCCOLOR;
        case bm_src_alpha: return D3DBLEND_SRCALPHA;
        case bm_inv_src_alpha: return D3DBLEND_INVSRCALPHA;
        case bm_dest_alpha: return D3DBLEND_DESTALPHA;
        case bm_inv_dest_alpha: return D3DBLEND_INVDESTALPHA;
        case bm_dest_color: return D3DBLEND_DESTCOLOR;
        case bm_inv_dest_color: return D3DBLEND_INVDESTCOLOR;
        case bm_src_alpha_sat: return D3DBLEND_SRCALPHASAT;
        default: return D3DBLEND_SRCALPHA;
    }
}

static void d3d9GpuSetBlendMode(Renderer* r, int32_t mode) {
    if (mode == bm_complex) return;
    flushBatch((D3D9Renderer*)r);
    IDirect3DDevice9* d = Dev((D3D9Renderer*)r);
    DWORD src = D3DBLEND_SRCALPHA, dst = D3DBLEND_INVSRCALPHA;
    DWORD op = D3DBLENDOP_ADD;
    switch (mode) {
        case bm_add:
            dst = D3DBLEND_ONE;
            break;
        case bm_subtract: // GameMaker: destination - source (D3D's REVSUBTRACT)
            src = D3DBLEND_SRCALPHA;
            dst = D3DBLEND_ONE;
            op = D3DBLENDOP_REVSUBTRACT;
            break;
        case bm_reverse_subtract: // source - destination
            src = D3DBLEND_SRCALPHA;
            dst = D3DBLEND_ONE;
            op = D3DBLENDOP_SUBTRACT;
            break;
        case bm_max:
            src = D3DBLEND_ONE;
            dst = D3DBLEND_ONE;
            op = D3DBLENDOP_MAX;
            break;
        case bm_min:
            src = D3DBLEND_ONE;
            dst = D3DBLEND_ONE;
            op = D3DBLENDOP_MIN;
            break;
        default:
            break;
    }
    d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_BLENDOP, op);
    d->SetRenderState(D3DRS_SRCBLEND, src);
    d->SetRenderState(D3DRS_DESTBLEND, dst);
    D3D9Renderer* dr = (D3D9Renderer*)r;
    dr->blendMode = mode;
    if (mode == bm_min || mode == bm_max) {
        r->blendFactors.src = bm_one;
        r->blendFactors.dst = bm_one;
    } else {
        r->blendFactors.src = bm_src_alpha;
        r->blendFactors.dst = mode == bm_normal ? bm_inv_src_alpha : bm_one;
    }
    r->blendFactors.srcAlpha = r->blendFactors.src;
    r->blendFactors.dstAlpha = r->blendFactors.dst;
}

static void d3d9GpuSetBlendModeExt(Renderer* r, int32_t sf, int32_t df, int32_t sfa, int32_t dfa) {
    flushBatch((D3D9Renderer*)r);
    IDirect3DDevice9* d = Dev((D3D9Renderer*)r);
    DWORD src = d3d9MapBlendFactor(sf);
    DWORD dst = d3d9MapBlendFactor(df);
    DWORD srcAlpha = sfa >= bm_zero && sfa <= bm_src_alpha_sat
        ? d3d9MapBlendFactor(sfa) : src;
    DWORD dstAlpha = dfa >= bm_zero && dfa <= bm_src_alpha_sat
        ? d3d9MapBlendFactor(dfa) : dst;
    d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    d->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
    d->SetRenderState(D3DRS_SRCBLEND, src);
    d->SetRenderState(D3DRS_DESTBLEND, dst);
    d->SetRenderState(D3DRS_SRCBLENDALPHA, srcAlpha);
    d->SetRenderState(D3DRS_DESTBLENDALPHA, dstAlpha);
    ((D3D9Renderer*)r)->blendMode = bm_complex;
    r->blendFactors.src = sf;
    r->blendFactors.dst = df;
    r->blendFactors.srcAlpha = sfa;
    r->blendFactors.dstAlpha = dfa;
}
static void d3d9ClearScreen(Renderer* r, uint32_t color, float alpha) {
    D3D9Renderer* dr = (D3D9Renderer*)r;
    SURF_TRACE("SURF clear bound=%d color=%06X alpha=%.2f skip=%d\n", dr->boundTarget, color, alpha,
               dr->skipNextClearScreen);
    if (dr->skipNextClearScreen) {
        dr->skipNextClearScreen = false;
        return;
    }
    flushBatch(dr);
    float cr, cg, cb, ca;
    bgrToFloatColor(color, alpha, &cr, &cg, &cb, &ca);
    DWORD packed = D3DCOLOR_ARGB((DWORD)(ca * 255.0f), (DWORD)(cr * 255.0f),
                                  (DWORD)(cg * 255.0f), (DWORD)(cb * 255.0f));
    Dev((D3D9Renderer*)r)->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, packed, 1.0f, 0);
}

// Populate by field name. The modern contract has grown substantially; a
// positional initializer silently routes incompatible callbacks into slots.
static RendererVtable d3d9RendererVtable;
static bool d3d9RendererVtableInitialized = false;
static void d3d9InitializeRendererVtable(void) {
    if (d3d9RendererVtableInitialized) return;
    memset(&d3d9RendererVtable, 0, sizeof(d3d9RendererVtable));
    d3d9RendererVtable.init = d3d9Init;
    d3d9RendererVtable.destroy = d3d9Destroy;
    d3d9RendererVtable.beginFrame = d3d9BeginFrame;
    d3d9RendererVtable.endFrameInit = d3d9EndFrameInit;
    d3d9RendererVtable.endFrameEnd = d3d9EndFrame;
    d3d9RendererVtable.beginView = d3d9BeginView;
    d3d9RendererVtable.endView = d3d9EndView;
    d3d9RendererVtable.applyProjection = d3d9ApplyProjection;
    d3d9RendererVtable.setMatrix = d3d9SetMatrix;
    d3d9RendererVtable.drawSurfaceColor = d3d9DrawSurfaceColor;
    d3d9RendererVtable.beginGUI = d3d9BeginGUI;
    d3d9RendererVtable.setGuiProjection = d3d9SetGuiProjection;
    d3d9RendererVtable.endGUI = d3d9EndGUI;
    d3d9RendererVtable.drawSprite = d3d9DrawSprite;
    d3d9RendererVtable.drawSpritePart = d3d9DrawSpritePartModern;
    d3d9RendererVtable.drawSpritePartColor = d3d9DrawSpritePartColorModern;
    d3d9RendererVtable.drawTextUI = d3d9DrawTextColorModern;
    d3d9RendererVtable.drawSpritePos = d3d9DrawSpritePos;
    d3d9RendererVtable.drawRectangle = d3d9DrawRectangle;
    d3d9RendererVtable.drawRectangleColor = d3d9DrawRectangleColor;
    d3d9RendererVtable.drawLine = d3d9DrawLine;
    d3d9RendererVtable.drawTriangle = d3d9DrawTriangle;
    d3d9RendererVtable.drawLineColor = d3d9DrawLineColor;
    d3d9RendererVtable.drawText = d3d9DrawTextModern;
    d3d9RendererVtable.drawTextColor = d3d9DrawTextColorModern;
    d3d9RendererVtable.drawSpriteTiled = d3d9DrawSpriteTiled;
    d3d9RendererVtable.drawSurfaceTiled = d3d9DrawSurfaceTiled;
    d3d9RendererVtable.primitiveBegin = d3d9PrimitiveBegin;
    d3d9RendererVtable.primitiveBeginTexture = d3d9PrimitiveBeginTexture;
    d3d9RendererVtable.primitiveEnd = d3d9PrimitiveEnd;
    d3d9RendererVtable.drawVertex = d3d9DrawVertex;
    d3d9RendererVtable.flush = d3d9Flush;
    d3d9RendererVtable.clearScreen = d3d9ClearScreen;
    d3d9RendererVtable.createSpriteFromSurface = d3d9CreateSpriteFromSurfaceModern;
    d3d9RendererVtable.drawVertexBuffer = d3d9DrawVertexBuffer;
    d3d9RendererVtable.deleteSprite = d3d9DeleteSprite;
    d3d9RendererVtable.gpuGetBlendFactors = d3d9GpuGetBlendFactors;
    d3d9RendererVtable.gpuGetBlendMode = d3d9GpuGetBlendMode;
    d3d9RendererVtable.gpuSetBlendMode = d3d9GpuSetBlendMode;
    d3d9RendererVtable.gpuSetBlendModeExt = d3d9GpuSetBlendModeExt;
    d3d9RendererVtable.gpuSetBlendEnable = d3d9GpuSetBlendEnable;
    d3d9RendererVtable.gpuSetTexFilter = d3d9GpuSetTexFilter;
    d3d9RendererVtable.gpuGetBlendEnable = d3d9GpuGetBlendEnable;
    d3d9RendererVtable.gpuSetAlphaTestEnable = d3d9GpuSetAlphaTestEnable;
    d3d9RendererVtable.gpuGetAlphaTestEnable = d3d9GpuGetAlphaTestEnable;
    d3d9RendererVtable.gpuSetAlphaTestRef = d3d9GpuSetAlphaTestRef;
    d3d9RendererVtable.gpuSetColorWriteEnable = d3d9GpuSetColorWriteEnable;
    d3d9RendererVtable.gpuSetFog = d3d9GpuSetFog;
    d3d9RendererVtable.gpuGetColorWriteEnable = d3d9GpuGetColorWriteEnable;
    d3d9RendererVtable.createSurface = d3d9CreateSurface;
    d3d9RendererVtable.surfaceExists = d3d9SurfaceExists;
    d3d9RendererVtable.setRenderTarget = d3d9SetRenderTarget;
    d3d9RendererVtable.ensureApplicationSurface = d3d9EnsureApplicationSurface;
    d3d9RendererVtable.getSurfaceWidth = d3d9GetSurfaceWidth;
    d3d9RendererVtable.getSurfaceHeight = d3d9GetSurfaceHeight;
    d3d9RendererVtable.drawSurface = d3d9DrawSurface;
    d3d9RendererVtable.surfaceResize = d3d9SurfaceResize;
    d3d9RendererVtable.surfaceFree = d3d9SurfaceFree;
    d3d9RendererVtable.surfaceCopy = d3d9SurfaceCopy;
    d3d9RendererVtable.surfaceGetPixels = d3d9SurfaceGetPixels;
    d3d9RendererVtable.spriteGetTexture = d3d9SpriteGetTexture;
    d3d9RendererVtable.surfaceGetTexture = d3d9SurfaceGetTexture;
    d3d9RendererVtable.textureGetTexelWidth = d3d9TextureGetTexelWidth;
    d3d9RendererVtable.textureGetTexelHeight = d3d9TextureGetTexelHeight;
    d3d9RendererVtable.textureGetUVs = d3d9TextureGetUVs;
    d3d9RendererVtable.textureSetStage = d3d9TextureSetStage;
    d3d9RendererVtable.gpuSetShader = d3d9GpuSetShader;
    d3d9RendererVtable.gpuResetShader = d3d9GpuResetShader;
    d3d9RendererVtable.shaderGetUniform = d3d9ShaderGetUniform;
    d3d9RendererVtable.shaderGetSamplerIndex = d3d9ShaderGetSamplerIndex;
    d3d9RendererVtable.shaderSetUniformF = d3d9ShaderSetUniformF;
    d3d9RendererVtable.shaderSetUniformI = d3d9ShaderSetUniformI;
    d3d9RendererVtable.shaderSetUniformFArray = d3d9ShaderSetUniformFArray;
    d3d9RendererVtable.shaderIsCompiled = d3d9ShaderIsCompiled;
    d3d9RendererVtable.shadersSupported = d3d9ShadersSupported;
    d3d9RendererVtableInitialized = true;
}

// ===[ Public API ]===

Renderer* D3D9Renderer_create(void* pd3dDevice) {
    if (!pd3dDevice) return NULL;
    D3D9Renderer* dr = (D3D9Renderer*)calloc(1, sizeof(D3D9Renderer));
    if (!dr) return NULL;
    d3d9InitializeRendererVtable();
    dr->base.vtable = &d3d9RendererVtable;
    dr->base.drawColor = 0xFFFFFF;
    dr->base.drawAlpha = 1.0f;
    dr->base.drawFont = -1;
    dr->appSurface = -1;
    dr->compressTextures = false; // compress only under memory pressure
    dr->boundTarget = -1;
    dr->pd3dDevice = pd3dDevice;
    dr->currentTextureIndex = -1;
    return (Renderer*)dr;
}

// gpu_set_texrepeat (registered in xdk_builtins.cpp): wrap instead of clamp on every sampler.
void D3D9Renderer_setTexRepeat(Renderer* r, bool repeat) {
    D3D9Renderer* dr = (D3D9Renderer*)r;
    if (dr->texRepeat == repeat) return;
    flushBatch(dr);
    dr->texRepeat = repeat;
    DWORD mode = repeat ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP;
    for (DWORD s = 0; s < 4; s++) {
        Dev(dr)->SetSamplerState(s, D3DSAMP_ADDRESSU, mode);
        Dev(dr)->SetSamplerState(s, D3DSAMP_ADDRESSV, mode);
    }
}

bool D3D9Renderer_getTexRepeat(Renderer* r) {
    return ((D3D9Renderer*)r)->texRepeat;
}
