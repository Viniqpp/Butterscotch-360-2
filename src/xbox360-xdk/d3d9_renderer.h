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

    // Dynamic sprite tracking
    uint32_t originalTexturePageCount;
    uint32_t originalTpagCount;
    uint32_t originalSpriteCount;
} D3D9Renderer;

Renderer* D3D9Renderer_create(void* pd3dDevice);
