#include <malloc.h>

#include <whb/proc.h>
#include <whb/log.h>
#include <whb/log_udp.h>
#include <whb/gfx.h>

#include <camera/camera.h>
#include <gx2/texture.h>
#include <gx2/utils.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/shaders.h>
#include <gx2/draw.h>
#include <coreinit/memdefaultheap.h>
#include <sndcore2/core.h>
#include <proc_ui/procui.h>

#include "display.h"

#define ATTRIB_SIZE (8 * 2 * sizeof(float))
#define ATTRIB_STRIDE (4 * sizeof(float))

uint32_t camCurrentDest __attribute__ ((aligned (4))) = 0;
static uint32_t camUpdate __attribute__ ((aligned (4))) = 1;

CAMHandle cHandle;
CAMSurface cSurface[2];
CAMSetupInfo cSetupInfo;

GX2Texture CameraYTexture;
GX2Texture CameraUVTexture;

GX2Sampler sampler;

static float *tvAttribs;
static float *drcAttribs;

static float tvScreenSize[2];
static float drcScreenSize[2];

void InitTexturePtrs(GX2Texture *texture, void *imagePtr, void *mipPtr) {
    texture->surface.image = imagePtr;
    if (texture->surface.mipLevels > 1) {
        if (mipPtr) {
            texture->surface.mipmaps = mipPtr;
        } else {
            texture->surface.mipmaps = (void *) ( (uint32_t)imagePtr + texture->surface.mipLevelOffset[0]);
        }
    } else {
        texture->surface.mipmaps = mipPtr;
    }
}

void *CameraMemAlloc(int size, int alignment) {
    void *pMem = NULL;
    CAMError err;
    
    if (size == 0)
        return 0;

    pMem = MEMAllocFromDefaultHeapEx(size, alignment);
    err = CAMCheckMemSegmentation(pMem, size);
    if (err != CAMERA_ERROR_OK) {
        WHBLogPrintf("Error: CAMCheckMemSegmentation returned %d", err);
    }

    return pMem;
}

void CameraInit() {
    CAMError err;
    int32_t total_size;
    
    // The camera stream needs to be rendered onto Y and UV planes
    CameraYTexture.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    CameraYTexture.surface.use = GX2_SURFACE_USE_TEXTURE;
    CameraYTexture.surface.format = GX2_SURFACE_FORMAT_UNORM_R8;
    CameraYTexture.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    CameraYTexture.surface.depth = 1;
    CameraYTexture.surface.width = CAMERA_WIDTH;
    CameraYTexture.surface.height = CAMERA_HEIGHT;
    CameraYTexture.surface.mipLevels = 1;
    CameraYTexture.viewNumSlices = 1;
    CameraYTexture.viewNumMips = 1;
    CameraYTexture.compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_0, GX2_SQ_SEL_0, GX2_SQ_SEL_1);

    GX2CalcSurfaceSizeAndAlignment(&CameraYTexture.surface);
    GX2InitTextureRegs(&CameraYTexture);

    CameraUVTexture.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    CameraUVTexture.surface.use = GX2_SURFACE_USE_TEXTURE;
    CameraUVTexture.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8;
    CameraUVTexture.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    CameraUVTexture.surface.depth = 1;
    CameraUVTexture.surface.width = CAMERA_WIDTH / 2;
    CameraUVTexture.surface.height = CAMERA_HEIGHT / 2;
    CameraUVTexture.surface.mipLevels = 1;
    CameraUVTexture.viewNumSlices = 1;
    CameraUVTexture.viewNumMips = 1;
    CameraUVTexture.compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_0, GX2_SQ_SEL_1);

    GX2CalcSurfaceSizeAndAlignment(&CameraUVTexture.surface);
    GX2InitTextureRegs(&CameraUVTexture);

    cSurface[0].surfaceBuffer = CameraMemAlloc(CAMERA_YUV_BUFFER_SIZE, CAMERA_YUV_BUFFER_ALIGNMENT);
    cSurface[0].surfaceSize = CAMERA_YUV_BUFFER_SIZE;
    cSurface[0].alignment = CAMERA_YUV_BUFFER_ALIGNMENT;

    cSurface[1].surfaceBuffer = CameraMemAlloc(CAMERA_YUV_BUFFER_SIZE, CAMERA_YUV_BUFFER_ALIGNMENT);
    cSurface[1].surfaceSize = CAMERA_YUV_BUFFER_SIZE;
    cSurface[1].alignment = CAMERA_YUV_BUFFER_ALIGNMENT;

    cSetupInfo.streamInfo.type = CAMERA_STREAM_TYPE_1;
    cSetupInfo.streamInfo.width = CAMERA_WIDTH;
    cSetupInfo.streamInfo.height = CAMERA_HEIGHT;
    cSetupInfo.mode.unk_0x00 = 0;
    cSetupInfo.mode.fps = CAMERA_FPS_30;

    total_size = CAMGetMemReq(&cSetupInfo.streamInfo);

    cSetupInfo.workMem.pMem = CameraMemAlloc(total_size, 256);
    cSetupInfo.workMem.size = total_size;
    cSetupInfo.threadAffinity = OS_THREAD_ATTRIB_AFFINITY_CPU1;
    
    cHandle = CAMInit(0, &cSetupInfo, &err);
    if (cHandle < 0) {
        WHBLogPrintf("Error: CAMInit returned: %d", err);
    }
}

void CameraShutdown() {
    CAMExit(cHandle);
    MEMFreeToDefaultHeap(cSurface[0].surfaceBuffer);
    MEMFreeToDefaultHeap(cSurface[1].surfaceBuffer);
    MEMFreeToDefaultHeap(cSetupInfo.workMem.pMem);
}

void CameraOpen() {
    CAMError err;
    err = CAMOpen(cHandle);
    if (err != CAMERA_ERROR_OK) {
        if (err != CAMERA_ERROR_DEVICE_IN_USE) {
            WHBLogPrintf("Error: CAMOpen returned %d", err);
        }
    }
}

void CameraClose() {
    CAMError err;

    err = CAMClose(cHandle);
    if (err != CAMERA_ERROR_OK) {
        if (err != CAMERA_ERROR_UNINITIALIZED) {
            WHBLogPrintf("Error: CAMClose returned %d", err);
        }
    }
}

// The camera lib calls CAMClose each time the foreground is released
static uint32_t procForegroudAcquired(void *context) {
    CameraOpen();
    return 0;
}

static void CamEventHandler(CAMEventData *eventData) {
    if (eventData->eventType == CAMERA_DECODE_DONE) {
        camCurrentDest = (camCurrentDest == 1) ? 0: camCurrentDest + 1;
        camUpdate = 1; 
    }
    else if (eventData->eventType == CAMERA_DRC_DETACH) {
    }

    return;
}

void RenderTV(WHBGfxShaderGroup *sGroup) {
    WHBGfxBeginRenderTV();

    WHBGfxClearColor(0, 0, 1.0f, 1.0f);

    void *imagePtr = (void *)cSurface[(camCurrentDest) ? 1 : 0].surfaceBuffer;
    GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, imagePtr, CAMERA_YUV_BUFFER_SIZE);

    InitTexturePtrs(&CameraYTexture, imagePtr, 0);
    InitTexturePtrs(&CameraUVTexture, (void *)(imagePtr + (uint32_t)CAMERA_Y_BUFFER_SIZE), 0);

    GX2SetDepthOnlyControl(GX2_ENABLE, GX2_ENABLE, GX2_COMPARE_FUNC_LESS);

    WHBGfxClearColor(0, 0, 1.0f, 1.0f);
    GX2SetFetchShader(&sGroup->fetchShader);
    GX2SetVertexShader(sGroup->vertexShader);
    GX2SetPixelShader(sGroup->pixelShader);

    GX2SetPixelTexture(&CameraYTexture, sGroup->pixelShader->samplerVars[0].location);
    GX2SetPixelTexture(&CameraUVTexture, sGroup->pixelShader->samplerVars[1].location);
    GX2SetPixelSampler(&sampler, sGroup->pixelShader->samplerVars[0].location);
    GX2SetPixelSampler(&sampler, sGroup->pixelShader->samplerVars[1].location);

    GX2SetVertexUniformReg(0, 2, tvScreenSize);

    GX2SetAttribBuffer(0, ATTRIB_SIZE, ATTRIB_STRIDE, tvAttribs);

    GX2DrawEx(GX2_PRIMITIVE_MODE_QUADS, 4, 0, 1);

    WHBGfxFinishRenderTV();
}

void RenderDRC(WHBGfxShaderGroup *sGroup) {
    WHBGfxBeginRenderDRC();

    WHBGfxClearColor(0, 0, 1.0f, 1.0f);

    void *imagePtr = (void *)cSurface[(camCurrentDest) ? 1 : 0].surfaceBuffer;
    GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, imagePtr, CAMERA_YUV_BUFFER_SIZE);

    InitTexturePtrs(&CameraYTexture, imagePtr, 0);
    InitTexturePtrs(&CameraUVTexture, (void *)(imagePtr + (uint32_t)CAMERA_Y_BUFFER_SIZE), 0);

    GX2SetDepthOnlyControl(GX2_ENABLE, GX2_ENABLE, GX2_COMPARE_FUNC_LESS);
    
    WHBGfxClearColor(0, 0, 1.0f, 1.0f);
    GX2SetFetchShader(&sGroup->fetchShader);
    GX2SetVertexShader(sGroup->vertexShader);
    GX2SetPixelShader(sGroup->pixelShader);

    GX2SetPixelTexture(&CameraYTexture, sGroup->pixelShader->samplerVars[0].location);
    GX2SetPixelTexture(&CameraUVTexture, sGroup->pixelShader->samplerVars[1].location);
    GX2SetPixelSampler(&sampler, sGroup->pixelShader->samplerVars[0].location);
    GX2SetPixelSampler(&sampler, sGroup->pixelShader->samplerVars[1].location);

    GX2SetVertexUniformReg(0, 2, drcScreenSize);

    GX2SetAttribBuffer(0, ATTRIB_SIZE, ATTRIB_STRIDE, drcAttribs);

    GX2DrawEx(GX2_PRIMITIVE_MODE_QUADS, 4, 0, 1);

    WHBGfxFinishRenderDRC();
}

int main(int argc, char **argv)
{
    WHBGfxShaderGroup sGroup = { 0 };
    
    WHBProcInit();
    WHBLogUdpInit();
    WHBGfxInit();
    AXInit();

    ProcUIRegisterCallback(PROCUI_CALLBACK_ACQUIRE, &procForegroudAcquired, NULL, 1);

    cSetupInfo.eventHandler = CamEventHandler;
    CameraInit();
    CameraOpen();

    // Since the stream is in YUV it must be converted to RGB via a shader
    // Shader code is from the moonlight-wiiu port
    // Gary is pretty epic :3
    // https://github.com/GaryOderNichts/moonlight-wiiu
    WHBGfxLoadGFDShaderGroup(&sGroup, 0, display_gsh);

    WHBGfxInitShaderAttribute(&sGroup, "in_pos", 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32);
    WHBGfxInitShaderAttribute(&sGroup, "in_texCoord", 0, 8, GX2_ATTRIB_FORMAT_FLOAT_32_32);

    WHBGfxInitFetchShader(&sGroup);

    GX2InitSampler(&sampler, GX2_TEX_CLAMP_MODE_WRAP, GX2_TEX_XY_FILTER_MODE_POINT);

    GX2ColorBuffer *cb = WHBGfxGetTVColourBuffer();
    tvScreenSize[0] = 1.0f / (float) cb->surface.width;
    tvScreenSize[1] = 1.0f / (float) cb->surface.height;

    tvAttribs = memalign(GX2_VERTEX_BUFFER_ALIGNMENT, ATTRIB_SIZE);
    int i = 0;

    tvAttribs[i++] = 0.0f;                      tvAttribs[i++] = 0.0f;
    tvAttribs[i++] = 0.0f;                      tvAttribs[i++] = 0.0f;

    tvAttribs[i++] = (float) cb->surface.width; tvAttribs[i++] = 0.0f;
    tvAttribs[i++] = 1.0f;                      tvAttribs[i++] = 0.0f;

    tvAttribs[i++] = (float) cb->surface.width; tvAttribs[i++] = (float) cb->surface.height;
    tvAttribs[i++] = 1.0f;                      tvAttribs[i++] = 1.0f;

    tvAttribs[i++] = 0.0f;                      tvAttribs[i++] = (float) cb->surface.height;
    tvAttribs[i++] = 0.0f;                      tvAttribs[i++] = 1.0f;
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, tvAttribs, ATTRIB_SIZE);

    cb = WHBGfxGetDRCColourBuffer();
    drcScreenSize[0] = 1.0f / (float) cb->surface.width;
    drcScreenSize[1] = 1.0f / (float) cb->surface.height;

    drcAttribs = memalign(GX2_VERTEX_BUFFER_ALIGNMENT, ATTRIB_SIZE);
    i = 0;

    drcAttribs[i++] = 0.0f;                      drcAttribs[i++] = 0.0f;
    drcAttribs[i++] = 0.0f;                      drcAttribs[i++] = 0.0f;

    drcAttribs[i++] = (float) cb->surface.width; drcAttribs[i++] = 0.0f;
    drcAttribs[i++] = 1.0f;                      drcAttribs[i++] = 0.0f;

    drcAttribs[i++] = (float) cb->surface.width; drcAttribs[i++] = (float) cb->surface.height;
    drcAttribs[i++] = 1.0f;                      drcAttribs[i++] = 1.0f;

    drcAttribs[i++] = 0.0f;                      drcAttribs[i++] = (float) cb->surface.height;
    drcAttribs[i++] = 0.0f;                      drcAttribs[i++] = 1.0f;
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, drcAttribs, ATTRIB_SIZE);

    while (WHBProcIsRunning()) {
        if (camUpdate == 1) {
            CAMError err = CAMSubmitTargetSurface(cHandle, &cSurface[camCurrentDest]);
            if (err != CAMERA_ERROR_OK) {
                WHBLogPrintf("CAMSubmitTargetSurface returned error %d", err);
            }
            else {
                camUpdate = 0;
            }
        }
        
        WHBGfxBeginRender();
        RenderTV(&sGroup);
        RenderDRC(&sGroup);
        WHBGfxFinishRender();
    }

    WHBGfxFreeShaderGroup(&sGroup);
    CameraClose();
    CameraShutdown();
    WHBLogUdpDeinit();
    WHBGfxShutdown();
    WHBProcShutdown();

    return 0;
}