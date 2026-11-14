// Pure math for the D3D9 backend's CPU vertex transform. No D3D/XDK types,
// so tools/xdk/test_d3d9_transform.c can verify it on any host.
#pragma once

#include "matrix_math.h"

// Canonical GML clip space (as the core and upstream renderers build it):
//  - views, application_surface and user surfaces put clip +1 at the top row;
//  - GUI/Post passes on the host framebuffer put clip -1 at the top
//    (upstream GL relies on its global flipClipY for those).
// The XDK backend draws everything straight into the back buffer, so a pass
// aimed at the host framebuffer must flip to stay upright.
static inline void D3D9Transform_build(Matrix4f* out, const Matrix4f* worldViewProjection,
                                       float vpX, float vpY, float vpW, float vpH, bool flipY) {
    Matrix4f ndcToScreen;
    Matrix4f_identity(&ndcToScreen);
    ndcToScreen.m[0] = vpW * 0.5f;
    ndcToScreen.m[5] = (flipY ? 0.5f : -0.5f) * vpH;
    ndcToScreen.m[12] = vpX + vpW * 0.5f;
    ndcToScreen.m[13] = vpY + vpH * 0.5f;
    Matrix4f_multiply(out, &ndcToScreen, worldViewProjection);
}

// Same integer math as upstream GLCommon_computeLetterbox: fit contentW x contentH inside areaW x areaH, centred.
static inline void D3D9Transform_letterbox(int32_t contentW, int32_t contentH, int32_t areaW, int32_t areaH,
                                           int32_t* outX, int32_t* outY, int32_t* outW, int32_t* outH) {
    int32_t effW, effH;
    if (contentW <= 0 || contentH <= 0) { *outX = *outY = 0; *outW = areaW; *outH = areaH; return; }
    if ((contentW * areaH) / contentH < areaW) {
        effW = (contentW * areaH) / contentH;
        effH = areaH;
    } else {
        effW = areaW;
        effH = (contentH * areaW) / contentW;
    }
    *outX = (areaW - effW) / 2;
    *outY = (areaH - effH) / 2;
    *outW = effW;
    *outH = effH;
}

// The GUI camera every upstream renderer applies in beginGUI/setGuiProjection.
static inline void D3D9Transform_guiCamera(Matrix4f* view, Matrix4f* projection, float guiW, float guiH) {
    Matrix4f_identity(view);
    Matrix4f_LookAt(view, guiW * 0.5f, guiH * 0.5f, -16000.0f, guiW * 0.5f, guiH * 0.5f, 16000.0f, 0.0f, 1.0f, 0.0f);
    Matrix4f_Orthographic(projection, guiW, guiH, 32000.0f, 0.0f);
}
