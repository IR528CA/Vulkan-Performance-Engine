#include "pch.h"
#include <jni.h>
#include <iostream>
#include <vector>
#include <chrono>
#include <thread>
#include <intrin.h>
#include <cwchar>
#include <windows.h>
#include <mmsystem.h>
#include <vulkan/vulkan.h>

#pragma comment(lib, "winmm.lib")

struct PushConstantsAFMF {
    float camFlowX;
    float camFlowY;
    int enableFG;
    float rcasStrength;
    float alpha;
    int stepIndex;
};

VkPhysicalDevice g_vkPhysicalDevice = VK_NULL_HANDLE;
VkDevice g_vkDevice = VK_NULL_HANDLE;
bool g_fsrInitialized = false;

int g_renderWidth = 0;
int g_renderHeight = 0;
float g_camFlowX = 0.0f;
float g_camFlowY = 0.0f;
VkImage g_currentColorImg = VK_NULL_HANDLE;

VkImage g_lowResImage = VK_NULL_HANDLE;
VkDeviceMemory g_lowResMemory = VK_NULL_HANDLE;

VkImage g_tempImage = VK_NULL_HANDLE;
VkDeviceMemory g_tempMemory = VK_NULL_HANDLE;
VkImageView g_tempView = VK_NULL_HANDLE;

VkImage g_prevImage = VK_NULL_HANDLE;
VkDeviceMemory g_prevMemory = VK_NULL_HANDLE;
VkImageView g_prevView = VK_NULL_HANDLE;

VkImage g_interpImage = VK_NULL_HANDLE;
VkDeviceMemory g_interpMemory = VK_NULL_HANDLE;
VkImageView g_interpView = VK_NULL_HANDLE;

VkImage g_flowImage = VK_NULL_HANDLE;
VkDeviceMemory g_flowMemory = VK_NULL_HANDLE;
VkImageView g_flowView = VK_NULL_HANDLE;

VkSampler g_linearSampler = VK_NULL_HANDLE;

uint32_t g_tempWidth = 0;
uint32_t g_tempHeight = 0;
bool g_hasPrevFrame = false;
int g_resizeCooldownFrames = 0;

VkDescriptorSetLayout g_csSetLayout = VK_NULL_HANDLE;
VkPipelineLayout g_csPipelineLayout = VK_NULL_HANDLE;
VkPipeline g_csPipeline = VK_NULL_HANDLE;
VkDescriptorPool g_csDescPool = VK_NULL_HANDLE;
VkDescriptorSet g_csDescSet = VK_NULL_HANDLE;
bool g_csReady = false;
bool g_descriptorsValid = false;

VkCommandPool g_cmdPool = VK_NULL_HANDLE;
VkCommandBuffer g_cmdBuffers[16] = { VK_NULL_HANDLE };
VkSemaphore g_upscaleSemaphores[4] = { VK_NULL_HANDLE };
VkSemaphore g_fgAcquireSemaphores[16] = { VK_NULL_HANDLE };
VkSemaphore g_fgRenderSemaphores[16] = { VK_NULL_HANDLE };
VkFence g_upscaleFences[4] = { VK_NULL_HANDLE };
VkFence g_fgFences[16] = { VK_NULL_HANDLE };
uint32_t g_syncIndex = 0;
uint32_t g_fgRingIndex = 0;

VkSwapchainKHR g_cachedSwapchain = VK_NULL_HANDLE;
uint32_t g_lastSeenWidth = 0;
uint32_t g_lastSeenHeight = 0;
std::vector<VkImage> g_swapchainImages;

auto g_lastPresentEndTime = std::chrono::high_resolution_clock::now();
auto g_lastPresentStartTime = std::chrono::high_resolution_clock::now();
double g_smoothedCycleUs = 16666.0;

typedef void* (*pfn_shaderc_compiler_initialize)();
typedef void (*pfn_shaderc_compiler_release)(void*);
typedef void* (*pfn_shaderc_compile_into_spv)(void*, const char*, size_t, int, const char*, const char*, void*);
typedef int (*pfn_shaderc_result_get_compilation_status)(void*);
typedef size_t(*pfn_shaderc_result_get_length)(void*);
typedef const char* (*pfn_shaderc_result_get_bytes)(void*);
typedef const char* (*pfn_shaderc_result_get_error_message)(void*);
typedef void (*pfn_shaderc_result_release)(void*);

static const char* g_afmfComputeShaderGLSL = R"(
#version 450
layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D uPrevTex;
layout(set = 0, binding = 1) uniform sampler2D uCurrTex;
layout(set = 0, binding = 2, rgba8) uniform writeonly image2D uOutInterpImg;
layout(set = 0, binding = 3, rg16f) uniform image2D uFlowImg;

layout(push_constant) uniform PushConsts {
    vec2 camFlow;
    int enableFG;
    float rcasStrength;
    float alpha;
    int stepIndex;
} pc;

shared vec2 s_flow[16][16];
shared float s_luma[16][16];

float luma(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

vec3 applyRCAS(sampler2D tex, vec2 uv, vec2 invSize, vec3 baseColor, float strength) {
    vec3 n = texture(tex, uv + vec2(0.0, -invSize.y)).rgb;
    vec3 s = texture(tex, uv + vec2(0.0,  invSize.y)).rgb;
    vec3 w = texture(tex, uv + vec2(-invSize.x, 0.0)).rgb;
    vec3 e = texture(tex, uv + vec2( invSize.x, 0.0)).rgb;
    vec3 min4 = min(min(n, s), min(w, e));
    vec3 max4 = max(max(n, s), max(w, e));
    vec3 avg4 = (n + s + w + e) * 0.25;
    vec3 contrast = max4 - min4;
    float edgeWeight = clamp(1.0 - dot(contrast, vec3(0.333)), 0.25, 1.0);
    vec3 sharp = baseColor + (baseColor - avg4) * (strength * edgeWeight);
    return clamp(sharp, vec3(0.0), vec3(1.0));
}

vec3 sampleSharp(sampler2D tex, vec2 uv, vec2 invSize) {
    vec3 c = texture(tex, uv).rgb;
    vec3 n = texture(tex, uv + vec2(0.0, -invSize.y * 0.75)).rgb;
    vec3 s = texture(tex, uv + vec2(0.0,  invSize.y * 0.75)).rgb;
    vec3 w = texture(tex, uv + vec2(-invSize.x * 0.75, 0.0)).rgb;
    vec3 e = texture(tex, uv + vec2( invSize.x * 0.75, 0.0)).rgb;
    vec3 minCross = min(c, min(min(n, s), min(w, e)));
    vec3 maxCross = max(c, max(max(n, s), max(w, e)));
    vec3 sharpened = c + (c - (n + s + w + e) * 0.25) * 0.28;
    return clamp(sharpened, minCross, maxCross);
}

float evalFast(vec2 uv, vec2 flowPx, vec2 invSize) {
    vec2 halfUV = (flowPx * 0.5) * invSize;
    vec3 p = texture(uPrevTex, clamp(uv - halfUV, vec2(0.001), vec2(0.999))).rgb;
    vec3 c = texture(uCurrTex, clamp(uv + halfUV, vec2(0.001), vec2(0.999))).rgb;
    return dot(abs(p - c), vec3(0.333));
}

void main() {
    ivec2 gid = ivec2(gl_GlobalInvocationID.xy);
    ivec2 lid = ivec2(gl_LocalInvocationID.xy);
    ivec2 size = imageSize(uOutInterpImg);
    bool inBounds = (gid.x < size.x && gid.y < size.y);

    vec2 invSize = 1.0 / vec2(max(size, ivec2(1)));
    vec2 uv = (vec2(min(gid, size - ivec2(1))) + 0.5) * invSize;
    vec3 currCenter = texture(uCurrTex, uv).rgb;

    if (pc.enableFG == 0) {
        if (inBounds) {
            vec3 sharpOnly = applyRCAS(uCurrTex, uv, invSize, currCenter, pc.rcasStrength);
            imageStore(uOutInterpImg, gid, vec4(sharpOnly, 1.0));
        }
        return;
    }

    float phase = clamp(pc.alpha, 0.05, 0.95);
    vec3 prevCenter = texture(uPrevTex, uv).rgb;

    vec2 centerDist = abs(uv - vec2(0.5, 0.5)) * vec2(size);
    bool isCrosshair = (centerDist.x < 11.0 && centerDist.y < 11.0);
    bool isHotbar = (uv.y > 0.885 && uv.x > 0.24 && uv.x < 0.76);
    float staticDiff = dot(abs(currCenter - prevCenter), vec3(0.333));
    bool isStaticPixel = (!inBounds || isCrosshair || isHotbar || staticDiff < 0.002);

    vec2 bestFlow = vec2(0.0);

    if (pc.stepIndex > 1) {
        if (!inBounds) return;
        if (isStaticPixel) {
            vec3 sharpHud = applyRCAS(uCurrTex, uv, invSize, currCenter, pc.rcasStrength);
            imageStore(uOutInterpImg, gid, vec4(sharpHud, 1.0));
            return;
        }
        bestFlow = imageLoad(uFlowImg, gid).xy;
    } else {
        float currLuma = luma(currCenter);
        if (!isStaticPixel) {
            vec2 ndc = (uv - vec2(0.5)) * 2.0;
            vec2 perspScale = vec2(1.0 + ndc.x * ndc.x * 0.18, 1.0 + ndc.y * ndc.y * 0.18);
            vec2 camVec = clamp(pc.camFlow * perspScale, vec2(-128.0), vec2(128.0));

            bestFlow = camVec;
            float bestErr = evalFast(uv, bestFlow, invSize);

            float zeroErr = staticDiff;
            bool isHandZone = (uv.x > 0.56 && uv.y > 0.50);
            float zeroBias = isHandZone ? 0.90 : 1.18;
            if (zeroErr * zeroBias < bestErr) {
                bestErr = zeroErr;
                bestFlow = vec2(0.0);
            }

            vec2 radialFlow = ndc * 7.0;
            float radErr = evalFast(uv, camVec + radialFlow, invSize);
            if (radErr < bestErr) {
                bestErr = radErr;
                bestFlow = camVec + radialFlow;
            }

            const vec2 coarseDirs[8] = vec2[8](
                vec2(-6.0, 0.0), vec2(6.0, 0.0), vec2(0.0, -6.0), vec2(0.0, 6.0),
                vec2(-3.5, -3.5), vec2(3.5, -3.5), vec2(-3.5, 3.5), vec2(3.5, 3.5)
            );
            vec2 baseFlow = bestFlow;
            if (bestErr > 0.011) {
                for (int i = 0; i < 8; i++) {
                    vec2 cand = baseFlow + coarseDirs[i];
                    float err = evalFast(uv, cand, invSize);
                    if (err < bestErr) {
                        bestErr = err;
                        bestFlow = cand;
                    }
                }
            }

            const vec2 fineDirs[4] = vec2[4](
                vec2(-1.5, 0.0), vec2(1.5, 0.0), vec2(0.0, -1.5), vec2(0.0, 1.5)
            );
            baseFlow = bestFlow;
            if (bestErr > 0.005) {
                for (int j = 0; j < 4; j++) {
                    vec2 cand = baseFlow + fineDirs[j];
                    float err = evalFast(uv, cand, invSize);
                    if (err < bestErr) {
                        bestErr = err;
                        bestFlow = cand;
                    }
                }
            }
        }

        s_flow[lid.y][lid.x] = bestFlow;
        s_luma[lid.y][lid.x] = currLuma;
        memoryBarrierShared();
        barrier();

        if (!inBounds) return;

        if (isStaticPixel) {
            imageStore(uFlowImg, gid, vec4(0.0));
            vec3 sharpHud = applyRCAS(uCurrTex, uv, invSize, currCenter, pc.rcasStrength);
            imageStore(uOutInterpImg, gid, vec4(sharpHud, 1.0));
            return;
        }

        vec2 smoothFlow = bestFlow * 2.0;
        float totalWeight = 2.0;
        int minX = max(lid.x - 1, 0);
        int maxX = min(lid.x + 1, 15);
        int minY = max(lid.y - 1, 0);
        int maxY = min(lid.y + 1, 15);

        for (int ny = minY; ny <= maxY; ny++) {
            for (int nx = minX; nx <= maxX; nx++) {
                float lDiff = abs(s_luma[ny][nx] - currLuma);
                float w = exp(-lDiff * 14.0);
                smoothFlow += s_flow[ny][nx] * w;
                totalWeight += w;
            }
        }
        bestFlow = smoothFlow / totalWeight;
        imageStore(uFlowImg, gid, vec4(bestFlow, 0.0, 0.0));
    }

    vec2 uvPrev = clamp(uv - (bestFlow * phase) * invSize, vec2(0.001), vec2(0.999));
    vec2 uvCurr = clamp(uv + (bestFlow * (1.0 - phase)) * invSize, vec2(0.001), vec2(0.999));

    vec3 pWarp = sampleSharp(uPrevTex, uvPrev, invSize);
    vec3 cWarp = sampleSharp(uCurrTex, uvCurr, invSize);

    float warpDiff = dot(abs(cWarp - pWarp), vec3(0.333));
    float errForward = dot(abs(pWarp - currCenter), vec3(0.333));
    float errBackward = dot(abs(cWarp - prevCenter), vec3(0.333));
    float blendWeight = clamp(phase + (errForward - errBackward) * 1.4, 0.08, 0.92);

    vec3 warpedInterp = mix(pWarp, cWarp, blendWeight);
    vec3 temporalInterp = mix(prevCenter, currCenter, phase);
    float confidence = clamp(1.0 - warpDiff * 1.45, 0.28, 1.0);
    vec3 interpColor = mix(temporalInterp, warpedInterp, confidence);

    vec3 finalSharp = applyRCAS(uCurrTex, uvCurr, invSize, interpColor, pc.rcasStrength * 0.85);
    imageStore(uOutInterpImg, gid, vec4(finalSharp, 1.0));
}
)";

uint32_t FindMemoryType(VkPhysicalDevice physDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physDevice, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return 0;
}

uint32_t FindGraphicsQueueFamily(VkPhysicalDevice physDevice) {
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &queueFamilyCount, queueFamilies.data());
    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if (queueFamilies[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            return i;
        }
    }
    return 0;
}

bool InitComputeInterpolationPipeline() {
    if (g_csReady) return true;

    if (g_linearSampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo samplerInfo = {};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = 1.0f;
        vkCreateSampler(g_vkDevice, &samplerInfo, nullptr, &g_linearSampler);
    }

    HMODULE hShaderc = GetModuleHandleA("shaderc_shared.dll");
    if (!hShaderc) hShaderc = GetModuleHandleA("lwjgl_shaderc.dll");
    if (!hShaderc) hShaderc = LoadLibraryA("shaderc_shared.dll");
    if (!hShaderc) return false;

    auto initCompiler = (pfn_shaderc_compiler_initialize)GetProcAddress(hShaderc, "shaderc_compiler_initialize");
    auto releaseCompiler = (pfn_shaderc_compiler_release)GetProcAddress(hShaderc, "shaderc_compiler_release");
    auto compileIntoSpv = (pfn_shaderc_compile_into_spv)GetProcAddress(hShaderc, "shaderc_compile_into_spv");
    auto getStatus = (pfn_shaderc_result_get_compilation_status)GetProcAddress(hShaderc, "shaderc_result_get_compilation_status");
    auto getLength = (pfn_shaderc_result_get_length)GetProcAddress(hShaderc, "shaderc_result_get_length");
    auto getBytes = (pfn_shaderc_result_get_bytes)GetProcAddress(hShaderc, "shaderc_result_get_bytes");
    auto getErrorMsg = (pfn_shaderc_result_get_error_message)GetProcAddress(hShaderc, "shaderc_result_get_error_message");
    auto releaseResult = (pfn_shaderc_result_release)GetProcAddress(hShaderc, "shaderc_result_release");

    if (!initCompiler || !compileIntoSpv || !getStatus || !getLength || !getBytes) return false;

    void* compiler = initCompiler();
    void* result = compileIntoSpv(compiler, g_afmfComputeShaderGLSL, strlen(g_afmfComputeShaderGLSL), 2, "afmf_dlss_mfg.comp", "main", nullptr);
    if (!result || getStatus(result) != 0) {
        if (result && getErrorMsg) {
            std::cout << "[FSR3-CPP] Error compilando Compute Shader: " << getErrorMsg(result) << std::endl << std::flush;
        }
        if (result) releaseResult(result);
        releaseCompiler(compiler);
        return false;
    }

    VkShaderModuleCreateInfo smInfo = {};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = getLength(result);
    smInfo.pCode = (const uint32_t*)getBytes(result);

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    VkResult smRes = vkCreateShaderModule(g_vkDevice, &smInfo, nullptr, &shaderModule);
    releaseResult(result);
    releaseCompiler(compiler);

    if (smRes != VK_SUCCESS || shaderModule == VK_NULL_HANDLE) return false;

    VkDescriptorSetLayoutBinding bindings[4] = {};
    bindings[0] = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    bindings[1] = { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    bindings[2] = { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    bindings[3] = { 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };

    VkDescriptorSetLayoutCreateInfo dslInfo = {};
    dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 4;
    dslInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(g_vkDevice, &dslInfo, nullptr, &g_csSetLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(g_vkDevice, shaderModule, nullptr);
        return false;
    }

    VkPushConstantRange pushRange = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstantsAFMF) };
    VkPipelineLayoutCreateInfo plInfo = {};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &g_csSetLayout;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges = &pushRange;
    vkCreatePipelineLayout(g_vkDevice, &plInfo, nullptr, &g_csPipelineLayout);

    VkComputePipelineCreateInfo cpInfo = {};
    cpInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpInfo.stage.module = shaderModule;
    cpInfo.stage.pName = "main";
    cpInfo.layout = g_csPipelineLayout;

    if (vkCreateComputePipelines(g_vkDevice, VK_NULL_HANDLE, 1, &cpInfo, nullptr, &g_csPipeline) != VK_SUCCESS) {
        vkDestroyShaderModule(g_vkDevice, shaderModule, nullptr);
        return false;
    }
    vkDestroyShaderModule(g_vkDevice, shaderModule, nullptr);

    VkDescriptorPoolSize poolSizes[2] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 6 }
    };
    VkDescriptorPoolCreateInfo dpInfo = {};
    dpInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpInfo.maxSets = 2;
    dpInfo.poolSizeCount = 2;
    dpInfo.pPoolSizes = poolSizes;
    vkCreateDescriptorPool(g_vkDevice, &dpInfo, nullptr, &g_csDescPool);

    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = g_csDescPool;
    dsAlloc.descriptorSetCount = 1;
    dsAlloc.pSetLayouts = &g_csSetLayout;
    vkAllocateDescriptorSets(g_vkDevice, &dsAlloc, &g_csDescSet);

    g_csReady = true;
    std::cout << "[FSR3-CPP] Compute Shader AFMF v6.1 (True 180Hz Cadence + Flow Cache + Catmull-Rom) activo!" << std::endl << std::flush;
    return true;
}

bool InitUpscaleResources() {
    if (g_cmdPool != VK_NULL_HANDLE) return true;
    timeBeginPeriod(1);

    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = FindGraphicsQueueFamily(g_vkPhysicalDevice);

    if (vkCreateCommandPool(g_vkDevice, &poolInfo, nullptr, &g_cmdPool) != VK_SUCCESS) return false;

    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = g_cmdPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 16;
    if (vkAllocateCommandBuffers(g_vkDevice, &allocInfo, g_cmdBuffers) != VK_SUCCESS) return false;

    VkSemaphoreCreateInfo semInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, nullptr, 0 };
    VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT };

    for (int i = 0; i < 4; i++) {
        vkCreateSemaphore(g_vkDevice, &semInfo, nullptr, &g_upscaleSemaphores[i]);
        vkCreateFence(g_vkDevice, &fenceInfo, nullptr, &g_upscaleFences[i]);
    }
    for (int i = 0; i < 16; i++) {
        vkCreateSemaphore(g_vkDevice, &semInfo, nullptr, &g_fgAcquireSemaphores[i]);
        vkCreateSemaphore(g_vkDevice, &semInfo, nullptr, &g_fgRenderSemaphores[i]);
        vkCreateFence(g_vkDevice, &fenceInfo, nullptr, &g_fgFences[i]);
    }

    InitComputeInterpolationPipeline();
    return true;
}

bool CreateVRAMImageCustom(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage, VkImage& outImg, VkDeviceMemory& outMem, VkImageView* outView = nullptr) {
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = { width, height, 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(g_vkDevice, &imageInfo, nullptr, &outImg) != VK_SUCCESS) return false;

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(g_vkDevice, outImg, &memReq);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = FindMemoryType(g_vkPhysicalDevice, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(g_vkDevice, &allocInfo, nullptr, &outMem) != VK_SUCCESS) {
        vkDestroyImage(g_vkDevice, outImg, nullptr);
        outImg = VK_NULL_HANDLE;
        return false;
    }
    vkBindImageMemory(g_vkDevice, outImg, outMem, 0);

    if (outView != nullptr) {
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = outImg;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCreateImageView(g_vkDevice, &viewInfo, nullptr, outView);
    }
    return true;
}

void DestroyVRAMImage(VkImage& img, VkDeviceMemory& mem, VkImageView* view = nullptr) {
    if (view != nullptr && *view != VK_NULL_HANDLE) {
        vkDestroyImageView(g_vkDevice, *view, nullptr);
        *view = VK_NULL_HANDLE;
    }
    if (img != VK_NULL_HANDLE) {
        vkDestroyImage(g_vkDevice, img, nullptr);
        img = VK_NULL_HANDLE;
    }
    if (mem != VK_NULL_HANDLE) {
        vkFreeMemory(g_vkDevice, mem, nullptr);
        mem = VK_NULL_HANDLE;
    }
}

void EnsureTempImages(uint32_t width, uint32_t height) {
    if (g_vkDevice == VK_NULL_HANDLE || g_vkPhysicalDevice == VK_NULL_HANDLE || width < 64 || height < 64) return;
    if (g_lowResImage != VK_NULL_HANDLE && g_tempImage != VK_NULL_HANDLE &&
        g_prevImage != VK_NULL_HANDLE && g_interpImage != VK_NULL_HANDLE &&
        g_flowImage != VK_NULL_HANDLE && g_tempWidth == width && g_tempHeight == height) {
        return;
    }

    vkDeviceWaitIdle(g_vkDevice);
    g_descriptorsValid = false;

    DestroyVRAMImage(g_lowResImage, g_lowResMemory, nullptr);
    DestroyVRAMImage(g_tempImage, g_tempMemory, &g_tempView);
    DestroyVRAMImage(g_prevImage, g_prevMemory, &g_prevView);
    DestroyVRAMImage(g_interpImage, g_interpMemory, &g_interpView);
    DestroyVRAMImage(g_flowImage, g_flowMemory, &g_flowView);

    VkImageUsageFlags colorUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;

    if (CreateVRAMImageCustom(width, height, VK_FORMAT_R8G8B8A8_UNORM, colorUsage, g_lowResImage, g_lowResMemory, nullptr) &&
        CreateVRAMImageCustom(width, height, VK_FORMAT_R8G8B8A8_UNORM, colorUsage, g_tempImage, g_tempMemory, &g_tempView) &&
        CreateVRAMImageCustom(width, height, VK_FORMAT_R8G8B8A8_UNORM, colorUsage, g_prevImage, g_prevMemory, &g_prevView) &&
        CreateVRAMImageCustom(width, height, VK_FORMAT_R8G8B8A8_UNORM, colorUsage, g_interpImage, g_interpMemory, &g_interpView) &&
        CreateVRAMImageCustom(width, height, VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, g_flowImage, g_flowMemory, &g_flowView)) {

        g_tempWidth = width;
        g_tempHeight = height;
        g_hasPrevFrame = false;

        if (g_csReady && g_csDescSet != VK_NULL_HANDLE && g_linearSampler != VK_NULL_HANDLE &&
            g_prevView != VK_NULL_HANDLE && g_tempView != VK_NULL_HANDLE &&
            g_interpView != VK_NULL_HANDLE && g_flowView != VK_NULL_HANDLE) {

            VkDescriptorImageInfo imgInfos[4] = {
                { g_linearSampler, g_prevView,   VK_IMAGE_LAYOUT_GENERAL },
                { g_linearSampler, g_tempView,   VK_IMAGE_LAYOUT_GENERAL },
                { VK_NULL_HANDLE,  g_interpView, VK_IMAGE_LAYOUT_GENERAL },
                { VK_NULL_HANDLE,  g_flowView,   VK_IMAGE_LAYOUT_GENERAL }
            };
            VkWriteDescriptorSet writes[4] = {
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, g_csDescSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &imgInfos[0], nullptr, nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, g_csDescSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &imgInfos[1], nullptr, nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, g_csDescSet, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          &imgInfos[2], nullptr, nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, g_csDescSet, 3, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          &imgInfos[3], nullptr, nullptr }
            };
            vkUpdateDescriptorSets(g_vkDevice, 4, writes, 0, nullptr);
            g_descriptorsValid = true;
        }
        std::cout << "[FSR3-CPP] Buffers AFMF v6.1 listos a: " << width << "x" << height << std::endl << std::flush;
    }
}

void BlitRegionImage(VkCommandBuffer cmd, VkImage src, VkImageLayout srcLayout, int srcW, int srcH, VkImage dst, VkImageLayout dstLayout, int dstW, int dstH, VkFilter filter) {
    VkImageBlit blit = {};
    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.srcOffsets[0] = { 0, 0, 0 };
    blit.srcOffsets[1] = { srcW, srcH, 1 };
    blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.dstOffsets[0] = { 0, 0, 0 };
    blit.dstOffsets[1] = { dstW, dstH, 1 };
    vkCmdBlitImage(cmd, src, srcLayout, dst, dstLayout, 1, &blit, filter);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_vpe_core_FSR3NativeBridge_initFSR3(JNIEnv* env, jclass clazz, jlong instance, jlong physDevice, jlong device, jlong swapchain, jint displayWidth, jint displayHeight) {
    g_vkPhysicalDevice = (VkPhysicalDevice)physDevice;
    g_vkDevice = (VkDevice)device;
    if (!InitUpscaleResources()) return JNI_FALSE;
    g_fsrInitialized = true;
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_vpe_core_FSR3NativeBridge_dispatchFSR3(JNIEnv* env, jclass clazz,
    jlong cmdBufferPtr, jlong colorImgPtr,
    jlong depthImgPtr, jlong motionImgPtr,
    jlong outputImgPtr, jint renderWidth, jint renderHeight, jint displayWidth, jint displayHeight,
    jfloat camFlowX, jfloat camFlowY) {

    g_renderWidth = renderWidth;
    g_renderHeight = renderHeight;
    g_currentColorImg = (VkImage)colorImgPtr;
    g_camFlowX = camFlowX;
    g_camFlowY = camFlowY;
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_vpe_core_FSR3NativeBridge_presentFSR3(JNIEnv* env, jclass clazz,
    jlong queuePtr, jlong swapchainPtr, jint imageIndex, jlong semaphorePtr,
    jint displayWidth, jint displayHeight, jboolean enableFrameGen, jint fgMultiplier) {

    auto frameStart = std::chrono::high_resolution_clock::now();
    long long externalJavaUs = std::chrono::duration_cast<std::chrono::microseconds>(frameStart - g_lastPresentEndTime).count();
    long long fullCycleUs = std::chrono::duration_cast<std::chrono::microseconds>(frameStart - g_lastPresentStartTime).count();
    g_lastPresentStartTime = frameStart;

    if (fullCycleUs > 2000 && fullCycleUs < 60000) {
        g_smoothedCycleUs = g_smoothedCycleUs * 0.85 + (double)fullCycleUs * 0.15;
    }

    int mult = (int)fgMultiplier;
    if (mult < 2) mult = 2;
    if (mult > 6) mult = 6;

    VkQueue vkQueue = (VkQueue)queuePtr;
    VkSwapchainKHR swapchain = (VkSwapchainKHR)swapchainPtr;
    uint32_t imgIndex = (uint32_t)imageIndex;
    VkSemaphore waitSemaphore = (VkSemaphore)semaphorePtr;

    if (displayWidth < 64 || displayHeight < 64 || swapchain == VK_NULL_HANDLE) {
        VkPresentInfoKHR presentInfo = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1, &waitSemaphore, 1, &swapchain, &imgIndex, nullptr };
        vkQueuePresentKHR(vkQueue, &presentInfo);
        g_lastPresentEndTime = std::chrono::high_resolution_clock::now();
        return;
    }

    if (swapchain != g_cachedSwapchain || (uint32_t)displayWidth != g_lastSeenWidth || (uint32_t)displayHeight != g_lastSeenHeight) {
        g_cachedSwapchain = swapchain;
        g_lastSeenWidth = (uint32_t)displayWidth;
        g_lastSeenHeight = (uint32_t)displayHeight;
        g_resizeCooldownFrames = 12;
        g_hasPrevFrame = false;
        g_swapchainImages.clear();
    }

    if (g_resizeCooldownFrames > 0) {
        g_resizeCooldownFrames--;
        if (g_resizeCooldownFrames == 0 && g_vkDevice != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(g_vkDevice);
            uint32_t imgCount = 0;
            if (vkGetSwapchainImagesKHR(g_vkDevice, swapchain, &imgCount, nullptr) == VK_SUCCESS && imgCount > 0) {
                g_swapchainImages.resize(imgCount);
                vkGetSwapchainImagesKHR(g_vkDevice, swapchain, &imgCount, g_swapchainImages.data());
            }
            EnsureTempImages(displayWidth, displayHeight);
        }
        VkPresentInfoKHR presentInfo = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1, &waitSemaphore, 1, &swapchain, &imgIndex, nullptr };
        vkQueuePresentKHR(vkQueue, &presentInfo);
        g_lastPresentEndTime = std::chrono::high_resolution_clock::now();
        return;
    }

    if (g_vkDevice != VK_NULL_HANDLE && g_cmdPool != VK_NULL_HANDLE) {
        if (g_swapchainImages.empty()) {
            uint32_t imgCount = 0;
            if (vkGetSwapchainImagesKHR(g_vkDevice, swapchain, &imgCount, nullptr) == VK_SUCCESS && imgCount > 0) {
                g_swapchainImages.resize(imgCount);
                vkGetSwapchainImagesKHR(g_vkDevice, swapchain, &imgCount, g_swapchainImages.data());
            }
        }
        EnsureTempImages(displayWidth, displayHeight);
    }

    VkSemaphore finalSemaphoreToWait = waitSemaphore;
    uint32_t slot = g_syncIndex % 4;
    g_syncIndex++;

    VkImage vkColor = g_currentColorImg;
    if (vkColor == VK_NULL_HANDLE && imgIndex < g_swapchainImages.size()) {
        vkColor = g_swapchainImages[imgIndex];
    }

    int scaledW = (g_renderWidth > 0 && g_renderWidth <= displayWidth) ? g_renderWidth : displayWidth;
    int scaledH = (g_renderHeight > 0 && g_renderHeight <= displayHeight) ? g_renderHeight : displayHeight;
    bool needsUpscale = (scaledW != displayWidth || scaledH != displayHeight);
    bool doMultiFG = (enableFrameGen && g_hasPrevFrame && g_csReady && g_descriptorsValid && g_csPipeline != VK_NULL_HANDLE);

    if (vkColor != VK_NULL_HANDLE && g_lowResImage != VK_NULL_HANDLE && g_tempImage != VK_NULL_HANDLE &&
        g_prevImage != VK_NULL_HANDLE && g_interpImage != VK_NULL_HANDLE && g_flowImage != VK_NULL_HANDLE &&
        (needsUpscale || enableFrameGen)) {

        vkWaitForFences(g_vkDevice, 1, &g_upscaleFences[slot], VK_TRUE, UINT64_MAX);
        vkResetFences(g_vkDevice, 1, &g_upscaleFences[slot]);

        VkCommandBuffer cmd = g_cmdBuffers[slot];
        vkResetCommandBuffer(cmd, 0);

        VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr };
        vkBeginCommandBuffer(cmd, &beginInfo);

        VkImageMemoryBarrier initBarriers[4] = {
            { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, vkColor, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } },
            { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, g_tempImage, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } },
            { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, g_interpImage, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } },
            { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, g_flowImage, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } }
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 4, initBarriers);

        if (needsUpscale) {
            VkImageMemoryBarrier lowInit = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, g_lowResImage, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &lowInit);

            BlitRegionImage(cmd, vkColor, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, displayWidth, displayHeight,
                g_lowResImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, scaledW, scaledH, VK_FILTER_LINEAR);

            VkImageMemoryBarrier lowToSrc = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, g_lowResImage, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &lowToSrc);

            BlitRegionImage(cmd, g_lowResImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scaledW, scaledH,
                g_tempImage, VK_IMAGE_LAYOUT_GENERAL, displayWidth, displayHeight, VK_FILTER_LINEAR);
        }
        else {
            BlitRegionImage(cmd, vkColor, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, displayWidth, displayHeight,
                g_tempImage, VK_IMAGE_LAYOUT_GENERAL, displayWidth, displayHeight, VK_FILTER_NEAREST);
        }

        VkImageMemoryBarrier csBarriers[2] = {
            { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, g_tempImage, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } },
            { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, vkColor, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } }
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, csBarriers);

        if (g_csReady && g_descriptorsValid && g_csPipeline != VK_NULL_HANDLE && (needsUpscale || doMultiFG)) {
            float firstAlpha = 1.0f / (float)mult;
            PushConstantsAFMF pc = { g_camFlowX, g_camFlowY, doMultiFG ? 1 : 0, needsUpscale ? 0.72f : 0.35f, firstAlpha, 1 };
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_csPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_csPipelineLayout, 0, 1, &g_csDescSet, 0, nullptr);
            vkCmdPushConstants(cmd, g_csPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstantsAFMF), &pc);
            vkCmdDispatch(cmd, (displayWidth + 15) / 16, (displayHeight + 15) / 16, 1);

            VkImageMemoryBarrier afterCs[2] = {
                { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, g_interpImage, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } },
                { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, g_flowImage, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } }
            };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, afterCs);
            BlitRegionImage(cmd, g_interpImage, VK_IMAGE_LAYOUT_GENERAL, displayWidth, displayHeight, vkColor, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, displayWidth, displayHeight, VK_FILTER_NEAREST);
        }
        else {
            BlitRegionImage(cmd, g_tempImage, VK_IMAGE_LAYOUT_GENERAL, displayWidth, displayHeight, vkColor, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, displayWidth, displayHeight, VK_FILTER_LINEAR);
        }

        if (!doMultiFG || mult == 2) {
            VkImageMemoryBarrier prevToGen = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, g_prevImage, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &prevToGen);
            BlitRegionImage(cmd, g_tempImage, VK_IMAGE_LAYOUT_GENERAL, displayWidth, displayHeight, g_prevImage, VK_IMAGE_LAYOUT_GENERAL, displayWidth, displayHeight, VK_FILTER_NEAREST);

            VkImageMemoryBarrier endBarriers[2] = {
                { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, vkColor, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } },
                { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, g_prevImage, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } }
            };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 2, endBarriers);
        }
        else {
            VkImageMemoryBarrier endBarrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, vkColor, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &endBarrier);
        }

        vkEndCommandBuffer(cmd);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 1, &waitSemaphore, &waitStage, 1, &cmd, 1, &g_upscaleSemaphores[slot] };
        if (vkQueueSubmit(vkQueue, 1, &submitInfo, g_upscaleFences[slot]) == VK_SUCCESS) {
            finalSemaphoreToWait = g_upscaleSemaphores[slot];
            g_hasPrevFrame = true;
        }
    }

    VkPresentInfoKHR presentInfo = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1, &finalSemaphoreToWait, 1, &swapchain, &imgIndex, nullptr };
    VkResult presRes1 = vkQueuePresentKHR(vkQueue, &presentInfo);
    if (presRes1 == VK_ERROR_OUT_OF_DATE_KHR || presRes1 == VK_SUBOPTIMAL_KHR) {
        g_resizeCooldownFrames = 12;
        g_lastPresentEndTime = std::chrono::high_resolution_clock::now();
        return;
    }

    // 2. BUCLE DE SUB-CUADROS CON RELOJ DE CICLO REAL (g_smoothedCycleUs)
    if (enableFrameGen && !g_swapchainImages.empty() && g_tempImage != VK_NULL_HANDLE) {
        bool shouldPace = (externalJavaUs > 3800 && g_smoothedCycleUs > 6200.0 && g_smoothedCycleUs < 55000.0);
        uint64_t acquireTimeoutNs = shouldPace ? 2000000ULL : 120000ULL;

        for (int step = 2; step <= mult; step++) {
            if (shouldPace) {
                double equidistantFraction = ((double)(step - 1) / (double)mult) * 0.96;
                long long targetStepUs = (long long)(g_smoothedCycleUs * equidistantFraction);
                while (std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::high_resolution_clock::now() - frameStart).count() < targetStepUs) {
                    _mm_pause();
                }
            }

            uint32_t fgIdx = g_fgRingIndex % 12;
            g_fgRingIndex++;
            uint32_t cmdSlot = 4 + fgIdx;
            uint32_t genImgIndex = 0;

            vkWaitForFences(g_vkDevice, 1, &g_fgFences[fgIdx], VK_TRUE, UINT64_MAX);

            VkResult acqRes = vkAcquireNextImageKHR(g_vkDevice, swapchain, acquireTimeoutNs, g_fgAcquireSemaphores[fgIdx], VK_NULL_HANDLE, &genImgIndex);
            if (acqRes == VK_ERROR_OUT_OF_DATE_KHR) {
                g_resizeCooldownFrames = 12;
                break;
            }
            if ((acqRes != VK_SUCCESS && acqRes != VK_SUBOPTIMAL_KHR) || genImgIndex >= g_swapchainImages.size()) {
                break;
            }

            vkResetFences(g_vkDevice, 1, &g_fgFences[fgIdx]);

            VkImage genImage = g_swapchainImages[genImgIndex];
            VkCommandBuffer fgCmd = g_cmdBuffers[cmdSlot];
            vkResetCommandBuffer(fgCmd, 0);

            VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr };
            vkBeginCommandBuffer(fgCmd, &beginInfo);

            VkImageMemoryBarrier fgBarrier1 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, genImage, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
            vkCmdPipelineBarrier(fgCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &fgBarrier1);

            if (step < mult && doMultiFG) {
                float stepAlpha = (float)step / (float)mult;
                PushConstantsAFMF pc = { g_camFlowX, g_camFlowY, 1, needsUpscale ? 0.72f : 0.35f, stepAlpha, step };
                vkCmdBindPipeline(fgCmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_csPipeline);
                vkCmdBindDescriptorSets(fgCmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_csPipelineLayout, 0, 1, &g_csDescSet, 0, nullptr);
                vkCmdPushConstants(fgCmd, g_csPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstantsAFMF), &pc);
                vkCmdDispatch(fgCmd, (displayWidth + 15) / 16, (displayHeight + 15) / 16, 1);

                VkImageMemoryBarrier afterCs = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, g_interpImage, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
                vkCmdPipelineBarrier(fgCmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &afterCs);

                BlitRegionImage(fgCmd, g_interpImage, VK_IMAGE_LAYOUT_GENERAL, displayWidth, displayHeight, genImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, displayWidth, displayHeight, VK_FILTER_NEAREST);
            }
            else {
                BlitRegionImage(fgCmd, g_tempImage, VK_IMAGE_LAYOUT_GENERAL, displayWidth, displayHeight, genImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, displayWidth, displayHeight, VK_FILTER_NEAREST);

                if (doMultiFG && mult > 2) {
                    VkImageMemoryBarrier prevToGen = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, g_prevImage, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
                    vkCmdPipelineBarrier(fgCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &prevToGen);
                    BlitRegionImage(fgCmd, g_tempImage, VK_IMAGE_LAYOUT_GENERAL, displayWidth, displayHeight, g_prevImage, VK_IMAGE_LAYOUT_GENERAL, displayWidth, displayHeight, VK_FILTER_NEAREST);

                    VkImageMemoryBarrier prevReady = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, g_prevImage, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
                    vkCmdPipelineBarrier(fgCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &prevReady);
                }
            }

            VkImageMemoryBarrier fgBarrier2 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, genImage, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
            vkCmdPipelineBarrier(fgCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &fgBarrier2);
            vkEndCommandBuffer(fgCmd);

            VkPipelineStageFlags fgWaitStage = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            VkSubmitInfo fgSubmit = { VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 1, &g_fgAcquireSemaphores[fgIdx], &fgWaitStage, 1, &fgCmd, 1, &g_fgRenderSemaphores[fgIdx] };
            if (vkQueueSubmit(vkQueue, 1, &fgSubmit, g_fgFences[fgIdx]) == VK_SUCCESS) {
                VkPresentInfoKHR fgPresent = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1, &g_fgRenderSemaphores[fgIdx], 1, &swapchain, &genImgIndex, nullptr };
                vkQueuePresentKHR(vkQueue, &fgPresent);
            }
        }
    }

    g_lastPresentEndTime = std::chrono::high_resolution_clock::now();
}
// FIN DEL ARCHIVO