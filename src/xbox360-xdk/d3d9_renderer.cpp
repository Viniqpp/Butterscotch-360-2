#include <xtl.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <xgraphics.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

// Core headers — compiled as C++ alongside the .c files (via /TP flag)
#include "utils.h"
#include "text_utils.h"
#include "d3d9_renderer.h"

extern "C" unsigned long __cdecl DbgPrint(const char* format, ...);

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

static const char* g_psSource =
    "sampler2D s0 : register(s0);\n"
    "struct PS_IN { float2 Tex : TEXCOORD0; float4 Col : TEXCOORD1; };\n"
    "float4 main(PS_IN i) : COLOR0 {\n"
    "  return tex2D(s0, i.Tex) * i.Col;\n"
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

static void flushBatch(D3D9Renderer* dr) {
    if (!dr) return;
    if (!dr->sceneActive) { dr->quadCount = 0; return; }
    if (!dr->vertexData || !dr->triangleData || !dr->pd3dDevice || dr->quadCount == 0) return;

    IDirect3DDevice9* dev = Dev(dr);
    IDirect3DBaseTexture9* texture = dr->whiteTexture
        ? (IDirect3DBaseTexture9*)dr->whiteTexture : NULL;
    if (dr->currentTextureIndex >= 0 &&
        (uint32_t)dr->currentTextureIndex < dr->textureCount &&
        dr->textures[dr->currentTextureIndex]) {
        texture = (IDirect3DBaseTexture9*)dr->textures[dr->currentTextureIndex];
    }

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
    if (FAILED(hr)) DbgPrint("BS: D3D9 quad batch flush failed: hr=0x%08X\n", (unsigned)hr);
    dr->quadCount = 0;
}

static void ensureTexture(D3D9Renderer* dr, int32_t textureIndex) {
    if (dr->currentTextureIndex != textureIndex) {
        flushBatch(dr);
        dr->currentTextureIndex = textureIndex;
    }
}

// Public texture handles are TPAG handles (index + 1), not raw TXTR page IDs.
static int32_t d3d9TextureHandleToPage(D3D9Renderer* dr, uint32_t textureHandle) {
    if (!dr || !dr->base.dataWin || textureHandle == 0) return -1;
    DataWin* dw = dr->base.dataWin;
    uint32_t tpagIndex = textureHandle - 1;
    if (tpagIndex >= dw->tpag.count) return -1;
    int32_t page = dw->tpag.items[tpagIndex].texturePageId;
    if (page < 0 || (uint32_t)page >= dr->textureCount ||
        !dr->textures || !dr->textures[page]) return -1;
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

// Transform a game-space point to screen-space pixels (with pillarbox centering)
static inline void transformPoint(D3D9Renderer* dr, float inX, float inY, float* outX, float* outY) {
    // Game-space: view → port mapping
    float gx = dr->portOffsetX + (inX - dr->offsetX) * dr->portScaleX;
    float gy = dr->portOffsetY + (inY - dr->offsetY) * dr->portScaleY;
    // Screen-space: uniform scale + center offset
    *outX = gx * dr->renderScale + dr->renderOffsetX;
    *outY = gy * dr->renderScale + dr->renderOffsetY;
}

// ===[ Vtable Implementations ]===

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
    dr->maxTextureBytes = 128u * 1024u * 1024u;
    renderer->blendFactors.src = bm_src_alpha;
    renderer->blendFactors.dst = bm_inv_src_alpha;
    renderer->blendFactors.srcAlpha = bm_src_alpha;
    renderer->blendFactors.dstAlpha = bm_inv_src_alpha;

    // Allocate CPU vertex staging buffer
    dr->vertexData = (uint8_t*)malloc(D3D9_MAX_QUADS * D3D9_VERTS_PER_QUAD * sizeof(SpriteVertex));
    if (!dr->vertexData) {
        OutputDebugStringA("D3D9: vertex staging allocation failed\n");
        return;
    }
    dr->triangleData = malloc(D3D9_MAX_QUADS * 6u * sizeof(SpriteVertex));
    if (!dr->triangleData) {
        OutputDebugStringA("D3D9: triangle staging allocation failed\n");
        return;
    }
    dr->primitiveVertexCapacity = D3D9_MAX_PRIMITIVE_VERTICES;
    dr->primitiveVertexData = malloc((size_t)dr->primitiveVertexCapacity * sizeof(SpriteVertex));
    if (!dr->primitiveVertexData) {
        dr->primitiveVertexCapacity = 0;
        OutputDebugStringA("D3D9: primitive vertex allocation failed; immediate primitives disabled\n");
    }

    // Compile shaders from source
    ID3DXBuffer* pCode = NULL;
    ID3DXBuffer* pErr = NULL;

    HRESULT hr = D3DXCompileShader(g_vsSource, (UINT)strlen(g_vsSource),
                                   NULL, NULL, "main", "vs_2_0", 0, &pCode, &pErr, NULL);
    if (FAILED(hr) || !pCode) {
        OutputDebugStringA("VS compile failed: ");
        if (pErr) OutputDebugStringA((const char*)pErr->GetBufferPointer());
        if (pErr) pErr->Release();
        if (pCode) pCode->Release();
        return;
    }
    hr = dev->CreateVertexShader((const DWORD*)pCode->GetBufferPointer(),
                                 (IDirect3DVertexShader9**)&dr->pVertexShader);
    pCode->Release();
    if (pErr) { pErr->Release(); pErr = NULL; }
    if (FAILED(hr) || !dr->pVertexShader) {
        OutputDebugStringA("D3D9: CreateVertexShader failed\n");
        return;
    }

    hr = D3DXCompileShader(g_psSource, (UINT)strlen(g_psSource),
                           NULL, NULL, "main", "ps_2_0", 0, &pCode, &pErr, NULL);
    if (FAILED(hr) || !pCode) {
        OutputDebugStringA("PS compile failed: ");
        if (pErr) OutputDebugStringA((const char*)pErr->GetBufferPointer());
        if (pErr) pErr->Release();
        if (pCode) pCode->Release();
        return;
    }
    hr = dev->CreatePixelShader((const DWORD*)pCode->GetBufferPointer(),
                                (IDirect3DPixelShader9**)&dr->pPixelShader);
    pCode->Release();
    if (pErr) { pErr->Release(); pErr = NULL; }
    if (FAILED(hr) || !dr->pPixelShader) {
        OutputDebugStringA("D3D9: CreatePixelShader failed\n");
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
        OutputDebugStringA("D3D9: CreateVertexDeclaration failed\n");
        return;
    }

    // Create 1x1 white texture for primitives.
    IDirect3DTexture9* whiteTex = NULL;
    hr = dev->CreateTexture(1, 1, 1, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_DEFAULT, &whiteTex, NULL);
    if (FAILED(hr) || !whiteTex) {
        OutputDebugStringA("D3D9: white texture creation failed\n");
        return;
    }
    D3DLOCKED_RECT lr;
    hr = whiteTex->LockRect(0, &lr, NULL, 0);
    if (FAILED(hr) || !lr.pBits) {
        whiteTex->Release();
        OutputDebugStringA("D3D9: white texture lock failed\n");
        return;
    }
    *(DWORD*)lr.pBits = 0xFFFFFFFF;
    hr = whiteTex->UnlockRect(0);
    if (FAILED(hr)) {
        whiteTex->Release();
        OutputDebugStringA("D3D9: white texture unlock failed\n");
        return;
    }
    dr->whiteTexture = whiteTex;

    // Load TXTR pages: decode PNG to RGBA, upload as D3D textures
    dr->textureCount = dataWin->txtr.count;
    if (dr->textureCount > 0) {
        dr->textures = (void**)calloc(dr->textureCount, sizeof(void*));
        dr->textureWidths = (int32_t*)calloc(dr->textureCount, sizeof(int32_t));
        dr->textureHeights = (int32_t*)calloc(dr->textureCount, sizeof(int32_t));
        if (!dr->textures || !dr->textureWidths || !dr->textureHeights) {
            OutputDebugStringA("D3D9: texture table allocation failed\n");
            return;
        }
    }

    for (uint32_t i = 0; i < dr->textureCount; i++) {
        Texture* txtr = &dataWin->txtr.textures[i];
        if (!txtr->blobData || txtr->blobSize == 0 || txtr->blobSize > 32u * 1024u * 1024u) {
            DbgPrint("BS: invalid or oversized TXTR blob for page %u (size=%u)\n", i, txtr->blobSize);
            continue;
        }
        int expectedW = 0, expectedH = 0;
        if (!stbi_info_from_memory(txtr->blobData, (int)txtr->blobSize,
                                   &expectedW, &expectedH, NULL)) {
            DbgPrint("BS: could not inspect TXTR page %u before decode\n", i);
            continue;
        }
        uint64_t pixelBytes = (uint64_t)expectedW * (uint64_t)expectedH * 4u;
        if (expectedW <= 0 || expectedH <= 0 || expectedW > 8192 || expectedH > 8192 ||
            pixelBytes == 0 || pixelBytes > 64u * 1024u * 1024u) {
            DbgPrint("BS: invalid or oversized TXTR dimensions for page %u (%d x %d)\n",
                     i, expectedW, expectedH);
            continue;
        }

        int w = 0, h = 0;
        uint8_t* pixels = stbi_load_from_memory(txtr->blobData, (int)txtr->blobSize, &w, &h, NULL, 4);
        if (!pixels) {
            OutputDebugStringA("Failed to decode TXTR page\n");
            continue;
        }
        if (w != expectedW || h != expectedH) {
            DbgPrint("BS: TXTR page %u dimensions changed during decode (%d x %d, expected %d x %d)\n",
                     i, w, h, expectedW, expectedH);
            stbi_image_free(pixels);
            continue;
        }

        dr->textureWidths[i] = w;
        dr->textureHeights[i] = h;

        // Xbox 360 textures are uploaded through a linear surface. Keep a
        // strict aggregate budget so malformed WADs cannot exhaust unified RAM.
        IDirect3DTexture9* linearTex = NULL;
        HRESULT textureHr = dev->CreateTexture(w, h, 1, 0, D3DFMT_LIN_A8R8G8B8,
                                               D3DPOOL_DEFAULT, &linearTex, NULL);
        if (SUCCEEDED(textureHr) && linearTex) {
            D3DLOCKED_RECT lr;
            textureHr = linearTex->LockRect(0, &lr, NULL, 0);
            if (SUCCEEDED(textureHr) && lr.pBits) {
                uint64_t allocationBytes = lr.Pitch > 0 ? (uint64_t)lr.Pitch * (uint64_t)h : 0;
                if (lr.Pitch < w * 4 || allocationBytes == 0 ||
                    allocationBytes > dr->maxTextureBytes ||
                    dr->textureBytesAllocated > dr->maxTextureBytes - (uint32_t)allocationBytes) {
                    textureHr = E_OUTOFMEMORY;
                    HRESULT unlockHr = linearTex->UnlockRect(0);
                    if (FAILED(unlockHr)) textureHr = unlockHr;
                } else {
                    for (int y2 = 0; y2 < h; y2++) {
                        uint8_t* src = pixels + (size_t)y2 * (size_t)w * 4u;
                        DWORD* dst = (DWORD*)((uint8_t*)lr.pBits + (size_t)y2 * (size_t)lr.Pitch);
                        for (int x2 = 0; x2 < w; x2++) {
                            uint8_t r = src[x2 * 4 + 0];
                            uint8_t g = src[x2 * 4 + 1];
                            uint8_t b = src[x2 * 4 + 2];
                            uint8_t a = src[x2 * 4 + 3];
                            if (a == 0) { r = 0; g = 0; b = 0; }
                            dst[x2] = D3DCOLOR_ARGB(a, r, g, b);
                        }
                    }
                    textureHr = linearTex->UnlockRect(0);
                    if (SUCCEEDED(textureHr)) {
                        dr->textures[i] = linearTex;
                        dr->textureBytesAllocated += (uint32_t)allocationBytes;
                        linearTex = NULL;
                    }
                }
            } else if (SUCCEEDED(textureHr)) {
                HRESULT unlockHr = linearTex->UnlockRect(0);
                if (FAILED(unlockHr)) textureHr = unlockHr;
            }
        } else if (SUCCEEDED(textureHr)) {
            textureHr = E_FAIL;
        }

        if (linearTex) linearTex->Release();
        if (!dr->textures[i]) {
            DbgPrint("BS: D3D9 texture upload failed for page %u (hr=0x%08X)\n", i, (unsigned)textureHr);
        }
        stbi_image_free(pixels);
    }
    dr->originalTexturePageCount = dataWin->txtr.count;
    dr->originalTpagCount = dataWin->tpag.count;
    dr->originalSpriteCount = dataWin->sprt.count;

    dr->currentTextureIndex = -1;
    dr->quadCount = 0;
    dr->initialized = true;
    DbgPrint("BS: D3D9 renderer initialized (%u texture pages)\n", dr->textureCount);
}

static void d3d9Destroy(Renderer* renderer) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    if (dr->textures) {
        for (uint32_t i = 0; i < dr->textureCount; i++) {
            if (dr->textures[i]) ((IDirect3DTexture9*)dr->textures[i])->Release();
        }
    }
    free(dr->textures);
    free(dr->textureWidths);
    free(dr->textureHeights);
    free(dr->vertexData);
    free(dr->triangleData);
    free(dr->primitiveVertexData);
    if (dr->whiteTexture) ((IDirect3DTexture9*)dr->whiteTexture)->Release();
    if (dr->pVertexShader) ((IDirect3DVertexShader9*)dr->pVertexShader)->Release();
    if (dr->pPixelShader) ((IDirect3DPixelShader9*)dr->pPixelShader)->Release();
    if (dr->pVertexDecl) ((IDirect3DVertexDeclaration9*)dr->pVertexDecl)->Release();
    free(dr);
}

static void d3d9BeginFrame(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    IDirect3DDevice9* dev = Dev(dr);
    if (!dr->initialized || !dev || gameW <= 0 || gameH <= 0 || windowW <= 0 || windowH <= 0) return;

    dr->gameW = gameW;
    dr->gameH = gameH;
    dr->screenW = windowW;
    dr->screenH = windowH;

    // Compute uniform scale to fit game in screen with letterboxing
    float scaleX = (float)windowW / (float)gameW;
    float scaleY = (float)windowH / (float)gameH;
    dr->renderScale = (scaleX < scaleY) ? scaleX : scaleY;
    dr->renderOffsetX = ((float)windowW - (float)gameW * dr->renderScale) * 0.5f;
    dr->renderOffsetY = ((float)windowH - (float)gameH * dr->renderScale) * 0.5f;

    // Debug: print once
    static bool printedOnce = false;
    if (!printedOnce) {
        DbgPrint("BS: renderScale=%d/1000 offsetX=%d offsetY=%d gameW=%d gameH=%d screenW=%d screenH=%d\n",
            (int)(dr->renderScale * 1000), (int)dr->renderOffsetX, (int)dr->renderOffsetY,
            gameW, gameH, windowW, windowH);
        printedOnce = true;
    }

    dr->skipNextClearScreen = false;
    dr->sceneActive = false;
    HRESULT sceneHr = dev->BeginScene();
    if (FAILED(sceneHr)) {
        DbgPrint("BS: D3D9 BeginScene failed: hr=0x%08X\\n", (unsigned)sceneHr);
        return;
    }
    dr->sceneActive = true;

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
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
}

static void d3d9EndFrame(Renderer* renderer) {
    // The platform owns EndScene/Present after the GUI pass.
    flushBatch((D3D9Renderer*)renderer);
}

static void d3d9BeginView(Renderer* renderer, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH,
                           int32_t portX, int32_t portY, int32_t portW, int32_t portH, float viewAngle) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    IDirect3DDevice9* dev = Dev(dr);
    (void)viewAngle; // TODO: support rotated views
    if (!dr->initialized || !dev || viewW <= 0 || viewH <= 0) return;

    D3DVIEWPORT9 vp;
    vp.X = 0;
    vp.Y = 0;
    vp.Width = (DWORD)dr->screenW;
    vp.Height = (DWORD)dr->screenH;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    dev->SetViewport(&vp);

    // Store view transform for point mapping
    dr->offsetX = (float)viewX;
    dr->offsetY = (float)viewY;
    dr->portScaleX = (float)portW / (float)viewW;
    dr->portScaleY = (float)portH / (float)viewH;
    dr->portOffsetX = (float)portX;
    dr->portOffsetY = (float)portY;

    // No projection matrix needed — vertices are pre-transformed screen coords.
    // D3DRS_VIEWPORTENABLE=FALSE means the GPU uses positions directly as pixels.
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
    if (!dr->textures[texPageId]) return;

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
    if (!dr->textures[texPageId]) return;

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
    if (page < 0 || (uint32_t)page >= dr->textureCount || !dr->textures[page]) return;
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
    if (page < 0 || (uint32_t)page >= dr->textureCount || !dr->textures[page] ||
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

static void d3d9DrawText(Renderer* renderer, const char* text, float x, float y,
                          float xscale, float yscale, float angleDeg) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    DataWin* dw = renderer->dataWin;
    int32_t fontIndex = renderer->drawFont;
    if (0 > fontIndex || (uint32_t)fontIndex >= dw->font.count) return;

    Font* font = &dw->font.fonts[fontIndex];
    uint32_t color = renderer->drawColor;
    float alpha = renderer->drawAlpha;

    // Resolve font texture page
    int32_t fontTpagIndex = DataWin_resolveTPAG(dw, font->textureOffset);
    if (0 > fontTpagIndex) return;

    TexturePageItem* fontTpag = &dw->tpag.items[fontTpagIndex];
    int16_t pageId = fontTpag->texturePageId;
    if (0 > pageId || dr->textureCount <= (uint32_t)pageId) return;
    if (!dr->textures[pageId]) return;

    float texW = (float)dr->textureWidths[pageId];
    float texH = (float)dr->textureHeights[pageId];
    if (texW <= 0 || texH <= 0) return;

    float cr, cg, cb, ca;
    bgrToFloatColor(color, alpha, &cr, &cg, &cb, &ca);

    // Preprocess: convert # to \n (and \# to literal #)
    char* processed = TextUtils_preprocessGmlText(text);
    int32_t textLen = (int32_t)strlen(processed);

    // Count lines
    int32_t lineCount = TextUtils_countLines(processed, textLen);

    // Vertical alignment offset
    float totalHeight = (float)lineCount * (float)font->emSize;
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

    ensureTexture(dr, (int32_t)pageId);

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

            // Compute UVs from glyph position in the font's atlas
            float u0 = (float)(fontTpag->sourceX + glyph->sourceX) / texW;
            float v0 = (float)(fontTpag->sourceY + glyph->sourceY) / texH;
            float u1 = (float)(fontTpag->sourceX + glyph->sourceX + glyph->sourceWidth) / texW;
            float v1 = (float)(fontTpag->sourceY + glyph->sourceY + glyph->sourceHeight) / texH;

            // Local quad position
            float localX0 = cursorX + glyph->offset;
            float localY0 = cursorY;
            float localX1 = localX0 + (float)glyph->sourceWidth;
            float localY1 = localY0 + (float)glyph->sourceHeight;

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
            for (int i = 0; i < 4; i++) {
                transformPoint(dr, x + cx[i], y + cy[i], &screenX, &screenY);
                v[i].x = screenX;
                v[i].y = screenY;
                v[i].z = 0.0f;
                v[i].w = 1.0f;
                v[i].r = cr; v[i].g = cg; v[i].b = cb; v[i].a = ca;
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

        cursorY += (float)font->emSize;

        // Advance past the newline
        if (textLen > lineEnd) {
            lineStart = TextUtils_skipNewline(processed, lineEnd, textLen);
        } else {
            lineStart = lineEnd;
        }
    }

    free(processed);
}

static void d3d9Flush(Renderer* renderer) {
    flushBatch((D3D9Renderer*)renderer);
}

static int32_t d3d9CreateSpriteFromSurface(Renderer* renderer, int32_t x, int32_t y,
                                            int32_t w, int32_t h, bool removeback,
                                            bool smooth, int32_t xorig, int32_t yorig) {
    (void)renderer; (void)x; (void)y; (void)w; (void)h;
    (void)removeback; (void)smooth; (void)xorig; (void)yorig;
    // TODO: implement surface capture
    return -1;
}

static void d3d9DeleteSprite(Renderer* renderer, int32_t spriteIndex) {
    (void)renderer; (void)spriteIndex;
    // TODO: implement dynamic sprite deletion
}

// Modern RendererVtable includes surfaceID; keep the legacy helper's narrower
// signature behind an exact-contract adapter.
static int32_t d3d9CreateSpriteFromSurfaceModern(Renderer* renderer, int32_t surfaceID,
                                                  int32_t x, int32_t y, int32_t w, int32_t h,
                                                  bool removeback, bool smooth,
                                                  int32_t xorig, int32_t yorig) {
    (void)surfaceID;
    return d3d9CreateSpriteFromSurface(renderer, x, y, w, h,
                                       removeback, smooth, xorig, yorig);
}

// ===[ Modern renderer-contract adapters ]===

// GUI coordinates are independent of the last room view. The platform presents
// after this pass, so these hooks must never end or present the D3D scene.
static void d3d9EndFrameInit(Renderer* renderer) { flushBatch((D3D9Renderer*)renderer); }
static void d3d9BeginGUI(Renderer* renderer, int32_t guiW, int32_t guiH,
                         int32_t portX, int32_t portY, int32_t portW, int32_t portH,
                         int32_t targetSurfaceId) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    (void)targetSurfaceId; // User surfaces are not implemented by this backend yet.
    flushBatch(dr);
    dr->offsetX = dr->offsetY = 0.0f;
    dr->portOffsetX = (float)portX;
    dr->portOffsetY = (float)portY;
    dr->portScaleX = guiW > 0 ? (float)portW / (float)guiW : 1.0f;
    dr->portScaleY = guiH > 0 ? (float)portH / (float)guiH : 1.0f;
}
static void d3d9SetGuiProjection(Renderer* renderer, int32_t guiW, int32_t guiH,
                                 int32_t portW, int32_t portH, bool renderingToUserSurface) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    (void)renderingToUserSurface;
    flushBatch(dr);
    dr->offsetX = dr->offsetY = 0.0f;
    dr->portScaleX = guiW > 0 ? (float)portW / (float)guiW : 1.0f;
    dr->portScaleY = guiH > 0 ? (float)portH / (float)guiH : 1.0f;
}
static void d3d9EndGUI(Renderer* renderer) { flushBatch((D3D9Renderer*)renderer); }

static void d3d9DrawSpritePos(Renderer* renderer, int32_t tpagIndex,
                              float x1, float y1, float x2, float y2,
                              float x3, float y3, float x4, float y4, float alpha) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    DataWin* dw = renderer->dataWin;
    if (!dw || tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int32_t page = tpag->texturePageId;
    if (page < 0 || (uint32_t)page >= dr->textureCount || !dr->textures[page]) return;
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
    (void)lineSeparation;
    d3d9DrawText(renderer, text, x, y, xs, ys, angle);
}
static void d3d9DrawTextColorModern(Renderer* renderer, const char* text, float x, float y,
                                   float xs, float ys, float angle, int32_t c1, int32_t c2,
                                   int32_t c3, int32_t c4, float alpha, float lineSeparation) {
    (void)c2; (void)c3; (void)c4; (void)lineSeparation;
    uint32_t oldColor = renderer->drawColor;
    float oldAlpha = renderer->drawAlpha;
    renderer->drawColor = (uint32_t)c1;
    renderer->drawAlpha = alpha;
    d3d9DrawText(renderer, text, x, y, xs, ys, angle);
    renderer->drawColor = oldColor;
    renderer->drawAlpha = oldAlpha;
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
static void d3d9DrawSurfaceTiled(Renderer* renderer, int32_t surfaceID, float x, float y,
                                float xs, float ys, float roomW, float roomH, uint32_t color, float alpha) {
    (void)renderer; (void)surfaceID; (void)x; (void)y; (void)xs; (void)ys;
    (void)roomW; (void)roomH; (void)color; (void)alpha;
}
static void d3d9DrawSurface(Renderer* renderer, int32_t surfaceID, int32_t sl, int32_t st,
                            int32_t sw, int32_t sh, float x, float y, float xs, float ys,
                            float angle, uint32_t color, float alpha) {
    // The application surface aliases the host framebuffer, so compositing it
    // back onto itself is intentionally a no-op. Other surface IDs are unsupported.
    (void)renderer; (void)surfaceID; (void)sl; (void)st; (void)sw; (void)sh;
    (void)x; (void)y; (void)xs; (void)ys; (void)angle; (void)color; (void)alpha;
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
        if (page >= 0) texture = (IDirect3DBaseTexture9*)dr->textures[page];
        HRESULT hr = Dev(dr)->SetTexture(0, texture);
        if (SUCCEEDED(hr)) {
            hr = Dev(dr)->DrawPrimitiveUP(type, primitiveCount,
                dr->primitiveVertexData, sizeof(SpriteVertex));
        }
        if (FAILED(hr)) DbgPrint("BS: primitive DrawPrimitiveUP failed: hr=0x%08X\\n", (unsigned)hr);
    }

    dr->primitiveActive = false;
    dr->primitiveVertexCount = 0;
    dr->primitiveTexture = 0;
    dr->currentTextureIndex = -1;
}

// Unsupported surface/shader operations fail explicitly instead of dispatching
// through a mismatched function pointer. These are capability gaps, not success.
static int32_t d3d9CreateSurface(Renderer* r, int32_t w, int32_t h) { (void)r; (void)w; (void)h; return -1; }
static bool d3d9SurfaceExists(Renderer* r, int32_t id) {
    (void)r;
    // The application surface is represented by the host framebuffer on this
    // backend; user-created offscreen surfaces are not implemented.
    return id == APPLICATION_SURFACE_ID;
}
static bool d3d9SetRenderTarget(Renderer* r, int32_t id, bool implicit) {
    (void)implicit;
    if (id != APPLICATION_SURFACE_ID && id != RENDER_TARGET_HOST_FRAMEBUFFER) return false;
    flushBatch((D3D9Renderer*)r);
    return true;
}
static int32_t d3d9EnsureApplicationSurface(Renderer* r, int32_t w, int32_t h) {
    (void)r;
    return w > 0 && h > 0 ? APPLICATION_SURFACE_ID : -1;
}
static float d3d9GetSurfaceWidth(Renderer* r, int32_t id) {
    if (id != APPLICATION_SURFACE_ID) return 0.0f;
    return (float)((D3D9Renderer*)r)->gameW;
}
static float d3d9GetSurfaceHeight(Renderer* r, int32_t id) {
    if (id != APPLICATION_SURFACE_ID) return 0.0f;
    return (float)((D3D9Renderer*)r)->gameH;
}
static void d3d9SurfaceResize(Renderer* r, int32_t id, int32_t w, int32_t h) { (void)r; (void)id; (void)w; (void)h; }
static void d3d9SurfaceFree(Renderer* r, int32_t id) { (void)r; (void)id; }
static void d3d9SurfaceCopy(Renderer* r, int32_t d, int32_t dx, int32_t dy, int32_t src, int32_t sx, int32_t sy, int32_t w, int32_t h, bool part) {
    (void)r; (void)d; (void)dx; (void)dy; (void)src; (void)sx; (void)sy; (void)w; (void)h; (void)part;
}
static bool d3d9SurfaceGetPixels(Renderer* r, int32_t id, uint8_t* rgba) { (void)r; (void)id; (void)rgba; return false; }
static uint32_t d3d9SpriteGetTexture(Renderer* r, int32_t tpagIndex) {
    D3D9Renderer* dr = (D3D9Renderer*)r;
    DataWin* dw = r->dataWin;
    if (!dw || tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) return 0;
    int32_t page = dw->tpag.items[tpagIndex].texturePageId;
    if (page < 0 || (uint32_t)page >= dr->textureCount || !dr->textures[page]) return 0;
    return (uint32_t)tpagIndex + 1;
}
static uint32_t d3d9SurfaceGetTexture(Renderer* r, int32_t i) { (void)r; (void)i; return 0; }
static float d3d9TextureGetTexelWidth(Renderer* r, uint32_t texID) {
    DataWin* dw = r->dataWin;
    if (!dw || texID == 0 || texID - 1 >= dw->tpag.count) return 0.0f;
    D3D9Renderer* dr = (D3D9Renderer*)r;
    int32_t page = dw->tpag.items[texID - 1].texturePageId;
    if (page < 0 || (uint32_t)page >= dr->textureCount || !dr->textures[page] || dr->textureWidths[page] <= 0) return 0.0f;
    return 1.0f / (float)dr->textureWidths[page];
}
static float d3d9TextureGetTexelHeight(Renderer* r, uint32_t texID) {
    DataWin* dw = r->dataWin;
    if (!dw || texID == 0 || texID - 1 >= dw->tpag.count) return 0.0f;
    D3D9Renderer* dr = (D3D9Renderer*)r;
    int32_t page = dw->tpag.items[texID - 1].texturePageId;
    if (page < 0 || (uint32_t)page >= dr->textureCount || !dr->textures[page] || dr->textureHeights[page] <= 0) return 0.0f;
    return 1.0f / (float)dr->textureHeights[page];
}
static bool d3d9TextureGetUVs(Renderer* r, uint32_t texID, float* uv) {
    DataWin* dw = r->dataWin;
    if (!dw || !uv || texID == 0 || texID - 1 >= dw->tpag.count) return false;
    D3D9Renderer* dr = (D3D9Renderer*)r;
    TexturePageItem* t = &dw->tpag.items[texID - 1];
    int32_t page = t->texturePageId;
    if (page < 0 || (uint32_t)page >= dr->textureCount || !dr->textures[page] ||
        dr->textureWidths[page] <= 0 || dr->textureHeights[page] <= 0) return false;
    float w = (float)dr->textureWidths[page], h = (float)dr->textureHeights[page];
    uv[0] = (float)t->sourceX / w;
    uv[1] = (float)t->sourceY / h;
    uv[2] = (float)(t->sourceX + t->sourceWidth) / w;
    uv[3] = (float)(t->sourceY + t->sourceHeight) / h;
    return true;
}
static void d3d9TextureSetStage(Renderer* r, int32_t slot, uint32_t texID) {
    if (slot != 0) return;
    D3D9Renderer* dr = (D3D9Renderer*)r;
    ensureTexture(dr, d3d9TextureHandleToPage(dr, texID));
}
static void d3d9GpuSetShader(Renderer* r, int32_t i) { (void)r; (void)i; }
static void d3d9GpuResetShader(Renderer* r) { (void)r; }
static int32_t d3d9ShaderGetUniform(Renderer* r, int32_t s, char* n) { (void)r; (void)s; (void)n; return -1; }
static int32_t d3d9ShaderGetSamplerIndex(Renderer* r, int32_t s, char* n) { (void)r; (void)s; (void)n; return -1; }
static void d3d9ShaderSetUniformF(Renderer* r, int32_t h, int32_t c, float a, float b, float d, float e) { (void)r; (void)h; (void)c; (void)a; (void)b; (void)d; (void)e; }
static void d3d9ShaderSetUniformI(Renderer* r, int32_t h, int32_t c, int32_t a, int32_t b, int32_t d, int32_t e) { (void)r; (void)h; (void)c; (void)a; (void)b; (void)d; (void)e; }
static bool d3d9ShaderIsCompiled(Renderer* r, int32_t i) { (void)r; (void)i; return false; }
static bool d3d9ShadersSupported(void) { return false; }

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
        case bm_subtract:
            src = D3DBLEND_SRCALPHA;
            dst = D3DBLEND_ONE;
            op = D3DBLENDOP_SUBTRACT;
            break;
        case bm_reverse_subtract:
            src = D3DBLEND_SRCALPHA;
            dst = D3DBLEND_ONE;
            op = D3DBLENDOP_REVSUBTRACT;
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
    dr->pd3dDevice = pd3dDevice;
    dr->currentTextureIndex = -1;
    return (Renderer*)dr;
}
