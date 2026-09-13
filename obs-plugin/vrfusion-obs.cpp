#include <windows.h>

#include <obs-module.h>
#include <graphics/graphics.h>

#include <cstdint>

#include "shared_protocol.hpp"

OBS_DECLARE_MODULE()

MODULE_EXPORT const char *obs_module_description(void)
{
    return "VRFusion zero-copy D3D11 spectator source";
}

namespace {

struct VRFusionSource {
    obs_source_t *source = nullptr;
    HANDLE mapping = nullptr;
    const vrfusion::SharedFrameInfo *info = nullptr;
    gs_texture_t *sharedTexture = nullptr;
    gs_texture_t *localTexture = nullptr;
    uint64_t openedHandle = 0;
    uint32_t width = 1920;
    uint32_t height = 1080;
};

void CloseMapping(VRFusionSource *ctx)
{
    if (ctx->info) {
        UnmapViewOfFile(ctx->info);
        ctx->info = nullptr;
    }
    if (ctx->mapping) {
        CloseHandle(ctx->mapping);
        ctx->mapping = nullptr;
    }
}

bool EnsureMapping(VRFusionSource *ctx)
{
    if (ctx->info &&
        ctx->info->magic == vrfusion::kSharedMagic &&
        ctx->info->version == vrfusion::kSharedVersion) {
        ctx->width = ctx->info->width ? ctx->info->width : 1920;
        ctx->height = ctx->info->height ? ctx->info->height : 1080;
        return true;
    }

    CloseMapping(ctx);
    ctx->mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, vrfusion::kSharedMapName);
    if (!ctx->mapping) return false;

    ctx->info = reinterpret_cast<const vrfusion::SharedFrameInfo *>(
        MapViewOfFile(ctx->mapping, FILE_MAP_READ, 0, 0, sizeof(vrfusion::SharedFrameInfo)));
    if (!ctx->info) {
        CloseMapping(ctx);
        return false;
    }

    if (ctx->info->magic != vrfusion::kSharedMagic ||
        ctx->info->version != vrfusion::kSharedVersion) {
        CloseMapping(ctx);
        return false;
    }

    ctx->width = ctx->info->width ? ctx->info->width : 1920;
    ctx->height = ctx->info->height ? ctx->info->height : 1080;
    return true;
}

void DestroyTextures(VRFusionSource *ctx)
{
    if (ctx->localTexture) {
        gs_texture_destroy(ctx->localTexture);
        ctx->localTexture = nullptr;
    }
    if (ctx->sharedTexture) {
        gs_texture_destroy(ctx->sharedTexture);
        ctx->sharedTexture = nullptr;
    }
    ctx->openedHandle = 0;
}

bool EnsureTextures(VRFusionSource *ctx)
{
    if (!ctx->info || !ctx->info->sharedHandle) return false;
    const uint64_t handle = ctx->info->sharedHandle;

    if (ctx->sharedTexture && ctx->openedHandle == handle) return true;

    DestroyTextures(ctx);

    // VRFusion publishes a legacy D3D11 shared handle intentionally because
    // libobs exposes gs_texture_open_shared for that handle type on Windows.
    ctx->sharedTexture = gs_texture_open_shared(static_cast<uint32_t>(handle));
    if (!ctx->sharedTexture) return false;

    ctx->width = gs_texture_get_width(ctx->sharedTexture);
    ctx->height = gs_texture_get_height(ctx->sharedTexture);
    ctx->localTexture = gs_texture_create(ctx->width, ctx->height, GS_RGBA, 1, nullptr, 0);
    if (!ctx->localTexture) {
        DestroyTextures(ctx);
        return false;
    }

    ctx->openedHandle = handle;
    blog(LOG_INFO, "[VRFusion] Opened GPU texture %ux%u", ctx->width, ctx->height);
    return true;
}

const char *SourceName(void *)
{
    return "VRFusion GPU Capture";
}

void *SourceCreate(obs_data_t *, obs_source_t *source)
{
    auto *ctx = static_cast<VRFusionSource *>(bzalloc(sizeof(VRFusionSource)));
    ctx->source = source;
    ctx->width = 1920;
    ctx->height = 1080;
    return ctx;
}

void SourceDestroy(void *data)
{
    auto *ctx = static_cast<VRFusionSource *>(data);
    if (!ctx) return;

    obs_enter_graphics();
    DestroyTextures(ctx);
    obs_leave_graphics();

    CloseMapping(ctx);
    bfree(ctx);
}

uint32_t SourceWidth(void *data)
{
    auto *ctx = static_cast<VRFusionSource *>(data);
    if (!ctx) return 1920;
    return ctx->width;
}

uint32_t SourceHeight(void *data)
{
    auto *ctx = static_cast<VRFusionSource *>(data);
    if (!ctx) return 1080;
    return ctx->height;
}

void SourceRender(void *data, gs_effect_t *)
{
    auto *ctx = static_cast<VRFusionSource *>(data);
    if (!ctx || !EnsureMapping(ctx) || !EnsureTextures(ctx)) return;

    // Never block OBS. If the producer has not published a new key-1 frame,
    // keep rendering our previous local GPU copy.
    if (gs_texture_acquire_sync(ctx->sharedTexture, 1, 0) == 0) {
        gs_copy_texture(ctx->localTexture, ctx->sharedTexture);
        gs_texture_release_sync(ctx->sharedTexture, 0);
    }

    if (!ctx->localTexture) return;

    gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_OPAQUE);
    gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
    const bool previousSrgb = gs_framebuffer_srgb_enabled();
    const bool linearSrgb = gs_get_linear_srgb();
    gs_enable_framebuffer_srgb(linearSrgb);

    if (linearSrgb)
        gs_effect_set_texture_srgb(image, ctx->localTexture);
    else
        gs_effect_set_texture(image, ctx->localTexture);

    while (gs_effect_loop(effect, "Draw"))
        gs_draw_sprite(ctx->localTexture, 0, ctx->width, ctx->height);

    gs_enable_framebuffer_srgb(previousSrgb);
}

obs_source_info MakeSourceInfo()
{
    obs_source_info info{};
    info.id = "vrfusion_gpu_capture";
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
    info.get_name = SourceName;
    info.create = SourceCreate;
    info.destroy = SourceDestroy;
    info.get_width = SourceWidth;
    info.get_height = SourceHeight;
    info.video_render = SourceRender;
    info.icon_type = OBS_ICON_TYPE_GAME_CAPTURE;
    return info;
}

obs_source_info g_sourceInfo = MakeSourceInfo();

} // namespace

bool obs_module_load(void)
{
    obs_register_source(&g_sourceInfo);
    blog(LOG_INFO, "[VRFusion] OBS source loaded");
    return true;
}
