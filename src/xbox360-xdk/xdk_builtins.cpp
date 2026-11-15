// GameMaker functions the shared runner lacks, registered by the Xbox 360 build
// only (tools/xdk/missing_builtins.py lists what a game still misses).
// Desktop-only dialogs, recording and store/console APIs get the answer a
// player who dismisses them would give, so games carry on instead of stalling.
#include <xtl.h>
#include <cstdlib>
#include <cstring>
#include "vm.h"
#include "runner.h"
#include "d3d9_renderer.h"
#include "stb_ds.h"

// The shared runner may implement these later: never register a name twice.
static void reg(VMContext* vm, const char* name, BuiltinFunc fn) {
    if (shgeti(vm->builtinMap, (char*)name) < 0) VM_registerBuiltin(vm, name, fn);
}

#define ARG_OR(n, fallback) (argCount > (n) ? args[n] : (fallback))

static RValue bi_undefined(VMContext*, RValue*, int32_t) { return RValue_makeUndefined(); }
static RValue bi_true(VMContext*, RValue*, int32_t) { return RValue_makeBool(true); }
static RValue bi_false(VMContext*, RValue*, int32_t) { return RValue_makeBool(false); }
static RValue bi_zero(VMContext*, RValue*, int32_t) { return RValue_makeReal(0.0); }
static RValue bi_minus_one(VMContext*, RValue*, int32_t) { return RValue_makeReal(-1.0); }

// gpu_set_texrepeat(enable) / gpu_set_tex_repeat(enable)
static RValue bi_gpu_set_texrepeat(VMContext* ctx, RValue* args, int32_t argCount) {
    if (argCount >= 1 && ctx->runner && ctx->runner->renderer)
        D3D9Renderer_setTexRepeat(ctx->runner->renderer, RValue_toBool(args[0]));
    return RValue_makeUndefined();
}

static RValue bi_gpu_get_texrepeat(VMContext* ctx, RValue*, int32_t) {
    return RValue_makeBool(ctx->runner && ctx->runner->renderer && D3D9Renderer_getTexRepeat(ctx->runner->renderer));
}

// get_string(prompt, default) / get_integer(prompt, default): no keyboard dialog, keep the default.
static RValue bi_get_default(VMContext* ctx, RValue* args, int32_t argCount) {
    if (argCount < 2) return RValue_makeUndefined();
    if (args[1].type == RVALUE_STRING) return RValue_makeOwnedString(RValue_toString(args[1], ctx->dataWin));
    return RValue_makeReal(RValue_toReal(args[1]));
}

// sprite_duplicate(index): a new sprite sharing the original's texture pages.
static RValue bi_sprite_duplicate(VMContext* ctx, RValue* args, int32_t argCount) {
    DataWin* dw = ctx->dataWin;
    if (argCount < 1 || !ctx->runner || !ctx->runner->renderer) return RValue_makeReal(-1.0);
    int32_t src = RValue_toInt32(args[0]);
    if (src < 0 || (uint32_t)src >= dw->sprt.count || dw->sprt.sprites[src].textureCount == 0) return RValue_makeReal(-1.0);
    uint32_t first = ((D3D9Renderer*)ctx->runner->renderer)->originalSpriteCount;
    uint32_t slot = dw->sprt.count;
    for (uint32_t i = first; i < dw->sprt.count; i++)
        if (dw->sprt.sprites[i].textureCount == 0) { slot = i; break; }
    if (slot == dw->sprt.count) {
        Sprite* grown = (Sprite*)realloc(dw->sprt.sprites, (slot + 1) * sizeof(Sprite));
        if (!grown) return RValue_makeReal(-1.0);
        dw->sprt.sprites = grown;
        dw->sprt.count++;
    }
    Sprite* s = &dw->sprt.sprites[slot];
    *s = dw->sprt.sprites[src]; // after the realloc: src may have moved
    int32_t* tpags = (int32_t*)malloc(s->textureCount * sizeof(int32_t));
    if (!tpags) { s->textureCount = 0; return RValue_makeReal(-1.0); }
    memcpy(tpags, dw->sprt.sprites[src].tpagIndices, s->textureCount * sizeof(int32_t));
    s->tpagIndices = tpags; // own copy: sprite_delete frees it
    return RValue_makeReal((GMLReal)slot);
}

void XdkBuiltins_register(VMContext* vm) {
    reg(vm, "gpu_set_texrepeat", (BuiltinFunc)bi_gpu_set_texrepeat);
    reg(vm, "gpu_set_tex_repeat", (BuiltinFunc)bi_gpu_set_texrepeat);
    reg(vm, "gpu_get_texrepeat", (BuiltinFunc)bi_gpu_get_texrepeat);
    reg(vm, "gpu_get_tex_repeat", (BuiltinFunc)bi_gpu_get_texrepeat);
    reg(vm, "sprite_duplicate", (BuiltinFunc)bi_sprite_duplicate);
    reg(vm, "get_string", (BuiltinFunc)bi_get_default);
    reg(vm, "get_integer", (BuiltinFunc)bi_get_default);
    reg(vm, "show_question", (BuiltinFunc)bi_false);
    // Asset streaming hints: our pages stream on demand anyway.
    reg(vm, "texture_prefetch", (BuiltinFunc)bi_undefined);
    reg(vm, "sprite_prefetch", (BuiltinFunc)bi_zero);
    reg(vm, "texture_is_ready", (BuiltinFunc)bi_true);
    reg(vm, "buffer_async_group_option", (BuiltinFunc)bi_undefined);
    // Desktop/OS features with no 360 equivalent.
    static const char* const noops[] = {
        "show_message", "clipboard_set_text", "screen_save", "screen_save_part", "surface_save",
        "window_enable_borderless_fullscreen", "gamepad_test_mapping", "audio_falloff_set_model",
        "audio_listener_orientation", "gif_add_surface", "gif_save",
        // Depth state: the 2D renderer has no depth buffer yet.
        "gpu_set_ztestenable", "gpu_set_zwriteenable", "gpu_set_cullmode", "surface_depth_disable",
        "draw_set_lighting",
        // Deltarune Ch5's shader_replace_simple extension rewrites shader source at runtime on
        // some platforms; our shaders are precompiled, so the game's own shader stays in use.
        "shader_replace_simple_sync", "shader_replace_simple_init_raw",
    };
    for (size_t i = 0; i < sizeof(noops) / sizeof(noops[0]); i++) reg(vm, noops[i], (BuiltinFunc)bi_undefined);
    reg(vm, "audio_get_recorder_count", (BuiltinFunc)bi_zero);
    reg(vm, "audio_start_recording", (BuiltinFunc)bi_minus_one);
    reg(vm, "audio_stop_recording", (BuiltinFunc)bi_undefined);
    reg(vm, "gif_open", (BuiltinFunc)bi_minus_one);
}
