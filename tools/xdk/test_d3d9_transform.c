// Host test for the XDK renderer's CPU vertex transform (src/xbox360-xdk/d3d9_transform.h).
// Needs no XDK:  gcc -std=c99 -Isrc tools/xdk/test_d3d9_transform.c -lm -o test_d3d9_transform
// (do NOT add -Isrc/xbox360-xdk: its stdbool.h/windows.h shims are XDK-only).
//
// Reference points come from three independent sources:
//  * the pre-migration D3D9 formula (screen = (port + (p - view) * port/view) * scale + offset),
//  * upstream GLCommon_computeLetterbox and GL's flipClipY convention,
//  * the core's own camera/d3d matrix builders (copied calls below).
#include <stdio.h>
#include "../../src/xbox360-xdk/d3d9_transform.h"

static int failures = 0;

static void expectPoint(const char* what, const Matrix4f* m, float x, float y, float ex, float ey) {
    float sx, sy;
    Matrix4f_transformPoint(m, x, y, &sx, &sy);
    if (fabsf(sx - ex) > 1e-2f || fabsf(sy - ey) > 1e-2f) {
        printf("FAIL %s: (%g,%g) -> (%g,%g), expected (%g,%g)\n", what, x, y, sx, sy, ex, ey);
        failures++;
    }
}

// Same calls as Runner_updateCameraViewSimple (src/runner.c).
static void cameraMatrices(Matrix4f* view, Matrix4f* proj, float vx, float vy, float vw, float vh, float angleDeg) {
    float x = vx + vw / 2, y = vy + vh / 2;
    Matrix4f_identity(view);
    Matrix4f_LookAt(view, x, y, -16000.0f, x, y, 16000.0f, 0.0f, 1.0f, 0.0f);
    Matrix4f_translate(view, x, y, 0.0f);
    Matrix4f_rotateZ(view, -angleDeg * (float)M_PI / 180.0f);
    Matrix4f_translate(view, -x, -y, 0.0f);
    Matrix4f_Orthographic(proj, vw, -vh, 32000.0f, 0.0f);
}

static void wvp(Matrix4f* out, const Matrix4f* world, const Matrix4f* view, const Matrix4f* proj) {
    Matrix4f wv;
    Matrix4f_multiply(&wv, view, world);   // same order as Renderer_applyProjection
    Matrix4f_multiply(out, proj, &wv);
}

// Back buffer 1280x720, game 640x480: renderScale 1.5, letterbox offset (160, 0).
static const float SCALE = 1.5f, OFFX = 160.0f, OFFY = 0.0f;

static void legacyPoint(float px, float py, float vx, float vy, float vw, float vh,
                        float portX, float portY, float portW, float portH, float* ox, float* oy) {
    *ox = (portX + (px - vx) * portW / vw) * SCALE + OFFX;
    *oy = (portY + (py - vy) * portH / vh) * SCALE + OFFY;
}

static void testViewMatchesLegacyRenderer(void) {
    const float ports[2][4] = { { 0, 0, 640, 480 }, { 320, 0, 320, 240 } };
    for (int p = 0; p < 2; p++) {
        const float* port = ports[p];
        Matrix4f world, view, proj, m, screen;
        Matrix4f_identity(&world);
        cameraMatrices(&view, &proj, 100, 50, 640, 480, 0);
        wvp(&m, &world, &view, &proj);
        D3D9Transform_build(&screen, &m, port[0] * SCALE + OFFX, port[1] * SCALE + OFFY,
                            port[2] * SCALE, port[3] * SCALE, false);
        const float pts[4][2] = { { 100, 50 }, { 740, 530 }, { 420, 290 }, { 133.25f, 401.5f } };
        for (int i = 0; i < 4; i++) {
            float ex, ey;
            legacyPoint(pts[i][0], pts[i][1], 100, 50, 640, 480, port[0], port[1], port[2], port[3], &ex, &ey);
            expectPoint("view vs legacy D3D9 formula", &screen, pts[i][0], pts[i][1], ex, ey);
        }
    }
}

// The core camera and Matrix4f_viewProjection (used by the old GL path) must agree, including view_angle.
static void testRotatedCameraMatchesViewProjection(void) {
    const float angles[4] = { 0, 30, 90, -135 };
    for (int a = 0; a < 4; a++) {
        Matrix4f world, view, proj, cam, vp, s1, s2;
        Matrix4f_identity(&world);
        cameraMatrices(&view, &proj, 100, 50, 640, 480, angles[a]);
        wvp(&cam, &world, &view, &proj);
        Matrix4f_viewProjection(&vp, 100, 50, 640, 480, angles[a]);
        D3D9Transform_build(&s1, &cam, OFFX, OFFY, 960, 720, false);
        D3D9Transform_build(&s2, &vp, OFFX, OFFY, 960, 720, false);
        const float pts[3][2] = { { 420, 290 }, { 100, 50 }, { 600, 100 } };
        for (int i = 0; i < 3; i++) {
            float ex, ey;
            Matrix4f_transformPoint(&s2, pts[i][0], pts[i][1], &ex, &ey);
            expectPoint("camera matrices vs Matrix4f_viewProjection", &s1, pts[i][0], pts[i][1], ex, ey);
        }
        // The view centre stays at the port centre whatever the angle.
        expectPoint("rotated view centre", &s1, 420, 290, 640, 360);
    }
    // 90 degrees: a point 100 units right of centre lands 150 px (100 * 1.5) straight
    // above or below the centre. Direction is pinned by the viewProjection parity above.
    Matrix4f world, view, proj, cam, s;
    Matrix4f_identity(&world);
    cameraMatrices(&view, &proj, 100, 50, 640, 480, 90);
    wvp(&cam, &world, &view, &proj);
    D3D9Transform_build(&s, &cam, OFFX, OFFY, 960, 720, false);
    float sx, sy;
    Matrix4f_transformPoint(&s, 420 + 100, 290, &sx, &sy);
    if (fabsf(sx - 640) > 1e-2f || fabsf(fabsf(sy - 360) - 100 * 720.0f / 480.0f) > 1e-2f) {
        printf("FAIL rotate 90: (520,290) -> (%g,%g), expected x=640 and |y-360|=150\n", sx, sy);
        failures++;
    }
}

static void testLetterboxMatchesUpstream(void) {
    const int32_t cases[4][8] = {
        // content W,H  area W,H   expected x,y,w,h (upstream GLCommon_computeLetterbox)
        { 640, 480, 1280, 720, 160, 0, 960, 720 },
        { 1280, 720, 1280, 720, 0, 0, 1280, 720 },
        { 800, 600, 1280, 720, 160, 0, 960, 720 },
        { 1920, 1080, 1280, 1024, 0, 152, 1280, 720 },
    };
    for (int i = 0; i < 4; i++) {
        int32_t x, y, w, h;
        D3D9Transform_letterbox(cases[i][0], cases[i][1], cases[i][2], cases[i][3], &x, &y, &w, &h);
        if (x != cases[i][4] || y != cases[i][5] || w != cases[i][6] || h != cases[i][7]) {
            printf("FAIL letterbox case %d: got %d,%d %dx%d\n", i, x, y, w, h);
            failures++;
        }
    }
}

// GUI on the host framebuffer: upstream's GUI camera puts the top at clip -1.
static void testGuiOnHostIsUprightAndLetterboxed(void) {
    Matrix4f world, view, proj, m, s;
    Matrix4f_identity(&world);
    D3D9Transform_guiCamera(&view, &proj, 640, 480);
    wvp(&m, &world, &view, &proj);
    int32_t x, y, w, h;
    D3D9Transform_letterbox(640, 480, 1280, 720, &x, &y, &w, &h);
    D3D9Transform_build(&s, &m, (float)x, (float)y, (float)w, (float)h, true);
    expectPoint("gui top-left", &s, 0, 0, 160, 0);
    expectPoint("gui bottom-right", &s, 640, 480, 1120, 720);
    expectPoint("gui centre", &s, 320, 240, 640, 360);
    // Without the host flip the same matrices draw upside down: proves the flag matters.
    D3D9Transform_build(&s, &m, (float)x, (float)y, (float)w, (float)h, false);
    expectPoint("gui unflipped is upside down", &s, 0, 0, 160, 720);

    // Draw Post / Pre: GUI size == window size covers the whole back buffer.
    D3D9Transform_guiCamera(&view, &proj, 1280, 720);
    wvp(&m, &world, &view, &proj);
    D3D9Transform_build(&s, &m, 0, 0, 1280, 720, true);
    expectPoint("post top-left", &s, 0, 0, 0, 0);
    expectPoint("post bottom-right", &s, 1280, 720, 1280, 720);
}

// d3dApplyRectangleProjection (src/vm_builtins.c) builds opposite Y signs for
// world views and GUI passes; with the target-dependent flip both are upright.
static void testCoreD3DOrthoBothBranchesUpright(void) {
    for (int gui = 0; gui < 2; gui++) {
        Matrix4f world, view, proj, m, s;
        Matrix4f_identity(&world);
        Matrix4f_identity(&view);
        Matrix4f_LookAt(&view, 320, 240, -640, 320, 240, 0, sinf(0), cosf(0), 0);
        Matrix4f_Orthographic(&proj, 640, gui ? 480 : -480, 32000, 1);
        wvp(&m, &world, &view, &proj);
        D3D9Transform_build(&s, &m, OFFX, OFFY, 960, 720, gui != 0);
        expectPoint(gui ? "d3d ortho in GUI pass" : "d3d ortho in view", &s, 0, 0, 160, 0);
        expectPoint(gui ? "d3d ortho in GUI pass" : "d3d ortho in view", &s, 640, 480, 1120, 720);
    }
}

// matrix_set(matrix_world) / sequences: world applies before view and projection.
static void testWorldMatrixComposes(void) {
    Matrix4f world, view, proj, m, s;
    Matrix4f_identity(&world);
    Matrix4f_translate(&world, 10, 20, 0);
    Matrix4f_scale(&world, 2, 2, 1);
    cameraMatrices(&view, &proj, 0, 0, 640, 480, 0);
    wvp(&m, &world, &view, &proj);
    D3D9Transform_build(&s, &m, OFFX, OFFY, 960, 720, false);
    // local (5,5) -> world (20,30) -> screen (160+30, 45)
    expectPoint("world translate*scale", &s, 5, 5, 190, 45);
}

int main(void) {
    testViewMatchesLegacyRenderer();
    testRotatedCameraMatchesViewProjection();
    testLetterboxMatchesUpstream();
    testGuiOnHostIsUprightAndLetterboxed();
    testCoreD3DOrthoBothBranchesUpright();
    testWorldMatrixComposes();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("d3d9 transform: all checks passed\n");
    return 0;
}
