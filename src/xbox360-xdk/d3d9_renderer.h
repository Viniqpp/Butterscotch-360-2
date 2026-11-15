#pragma once

#include "renderer.h"

// Maximum quads per batch before flushing
#define D3D9_MAX_QUADS 2048
#define D3D9_VERTS_PER_QUAD 4
#define D3D9_INDICES_PER_QUAD 6
#define D3D9_MAX_PRIMITIVE_VERTICES 8192

// Vertex: position(4f), texcoord(2f), color(4f)
#define D3D9_VERTEX_STRIDE 40

typedef struct {
    Renderer base; // Must be first field

    void* pd3dDevice; // IDirect3DDevice9* (opaque in C header)

    // Shader programs (compiled at init from HLSL source)
    void* pVertexShader;
    void* pPixelShader;
    void* pVertexDecl;
    bool initialized; // true only after required D3D resources are ready
    bool sceneActive; // BeginScene succeeded and platform has not ended it yet
    bool skipNextClearScreen; // main loop clears before Draw Pre; suppress Runner_drawViews' duplicate clear
    int32_t blendMode;

    // Sprite batch state
    int32_t quadCount;
    int32_t currentTextureIndex;
    uint8_t* vertexData; // CPU-side staging (D3D9_MAX_QUADS * D3D9_VERTS_PER_QUAD * D3D9_VERTEX_STRIDE)
    void* triangleData; // expanded triangle-list staging (6 vertices per quad)
    void* primitiveVertexData;
    uint32_t primitiveVertexCount;
    uint32_t primitiveVertexCapacity;
    int32_t primitiveType;
    int32_t primitiveTexture;
    bool primitiveActive;

    // Textures loaded from TXTR pages (decoded PNG -> D3D textures)
    void** textures;     // IDirect3DTexture9*[]
    int32_t* textureWidths;
    int32_t* textureHeights;
    uint32_t textureCount;
    uint32_t textureBytesAllocated;
    uint32_t maxTextureBytes;
    // Pages load on first use and the least recently used ones are evicted to
    // stay within maxTextureBytes (Deltarune chapters exceed it when preloaded).
    uint32_t* pageLastUse; // frameCounter value of the last draw using the page
    uint32_t* pageBytes;
    uint8_t* pageFailed;   // decode/upload failed: do not retry every draw
    uint32_t frameCounter;
    void* loader;          // D3D9Loader: background page decoding
    float fog[4];          // gpu_set_fog colour, [3] = 1 while enabled
    bool texRepeat;        // gpu_set_texrepeat
    bool compressTextures; // CONFIG.JSN "compressTextures": DXT5 for every page (default: only under pressure)

    // 1x1 white texture for primitives
    void* whiteTexture;

    // World -> back-buffer pixel transform: viewport(NDC) * gmlMatrices[WORLD_VIEW_PROJECTION].
    // Rebuilt whenever the viewport or any GML matrix changes.
    Matrix4f screenTransform;
    float viewportX, viewportY, viewportW, viewportH; // back-buffer pixels
    bool hostTargetFlipY;    // GUI pass bound to the back buffer: clip -1 is the top row
    bool guiActive;          // inside beginGUI..endGUI (beginGUI re-enters on surface_reset_target)
    Matrix4f savedGuiWorld;  // world matrix restored by endGUI, as upstream does

    // Frame dimensions
    int32_t gameW, gameH;

    // Letterbox: uniform-scaled render area within screen
    float renderScale;   // uniform scale factor
    float renderOffsetX; // pixel offset for centering
    float renderOffsetY;
    int32_t windowW, windowH;

    // Surfaces (application_surface included): an EDRAM render target plus
    // the texture it is resolved into, per surface id.
    void* surfaces;      // stb_ds array of D3D9Surface
    int32_t appSurface;  // id of application_surface, -1 = draw to the back buffer
    int32_t boundTarget; // surface id being drawn to, -1 = back buffer
    void* hostCopy;      // IDirect3DTexture9*: back buffer kept while a surface is bound mid-frame
    bool hostDirty;      // back buffer drawn to this frame
    bool hostHasCopy;

    // GameMaker shaders, precompiled offline (tools/xdk/translate_shaders.py)
    // into game:\shaders\<name>.xps; NULL entries fall back to the default shader.
    void** userPixelShaders;   // IDirect3DPixelShader9*[shdr.count]
    void** userConstantTables; // ID3DXConstantTable*[shdr.count]
    uint32_t userShaderCount;
    void* uniformHandles;      // stb_ds array of { shader, D3DXHANDLE } (D3D9UniformRef)

    // Dynamic sprite tracking
    uint32_t originalTexturePageCount;
    uint32_t originalTpagCount;
    uint32_t originalSpriteCount;
} D3D9Renderer;

Renderer* D3D9Renderer_create(void* pd3dDevice);
void D3D9Renderer_setTexRepeat(Renderer* r, bool repeat);
bool D3D9Renderer_getTexRepeat(Renderer* r);

// Folder holding the precompiled <name>.xps shaders: "shaders\" next to data.win.
extern char g_d3d9ShaderDir[256];

// data.win the loader threads read texture blobs from (and key the cache on).
extern char g_d3d9DataWinPath[256];
