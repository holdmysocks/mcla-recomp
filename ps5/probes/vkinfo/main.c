/* PS5 probe 5: what the Vulkan driver offers on this console.
 *
 * Creates an instance, picks the one physical device and writes its
 * properties, limits, features, extensions, memory layout and support for the
 * formats the Xenia-derived Vulkan backend uses. Creates no logical device and
 * submits no GPU work.
 *
 * Output: mcla-vkinfo.txt in the title's own folder (/app0), readable over FTP.
 * Linked like the driver's own titles: no Vulkan loader, every command comes
 * from RADV's vk_icdGetInstanceProcAddr.
 */

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);

#if defined(__PROSPERO__)
#define OUTPUT_PATH "/app0/mcla-vkinfo.txt"
#else
#define OUTPUT_PATH "mcla-vkinfo.txt"
#endif

static FILE *out;

static void line(const char *format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    fprintf(stderr, "[mcla-vkinfo] %s\n", text);
    if (out) {
        fprintf(out, "%s\n", text);
        fflush(out);
    }
}

#define LOAD(instance, name) PFN_vk##name name = (PFN_vk##name)vk_icdGetInstanceProcAddr(instance, "vk" #name)

static void samples(const char *name, VkSampleCountFlags flags) {
    line("%-44s %s%s%s%s%s", name, flags & 1 ? "1 " : "", flags & 2 ? "2 " : "", flags & 4 ? "4 " : "",
         flags & 8 ? "8 " : "", flags & 16 ? "16" : "");
}

int main(void) {
    out = fopen(OUTPUT_PATH, "w");
    line("mcla-vkinfo starts");

    LOAD(VK_NULL_HANDLE, CreateInstance);
    LOAD(VK_NULL_HANDLE, EnumerateInstanceExtensionProperties);
    if (!CreateInstance) {
        line("FAIL no vkCreateInstance");
        return 1;
    }

    static VkExtensionProperties instance_extensions[128];
    uint32_t instance_extension_count = 128;
    if (EnumerateInstanceExtensionProperties &&
        EnumerateInstanceExtensionProperties(NULL, &instance_extension_count, instance_extensions) >= VK_SUCCESS) {
        line("== instance extensions (%u)", instance_extension_count);
        for (uint32_t i = 0; i < instance_extension_count; i++) line("  %s", instance_extensions[i].extensionName);
    }

    const VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "mcla-vkinfo",
        .apiVersion = VK_API_VERSION_1_4,
    };
    const VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = CreateInstance(&instance_info, NULL, &instance);
    if (result != VK_SUCCESS) {
        line("FAIL vkCreateInstance returned %d", (int)result);
        return 1;
    }

    LOAD(instance, DestroyInstance);
    LOAD(instance, EnumeratePhysicalDevices);
    LOAD(instance, GetPhysicalDeviceProperties2);
    LOAD(instance, GetPhysicalDeviceFeatures2);
    LOAD(instance, GetPhysicalDeviceMemoryProperties);
    LOAD(instance, GetPhysicalDeviceQueueFamilyProperties);
    LOAD(instance, GetPhysicalDeviceFormatProperties);
    LOAD(instance, EnumerateDeviceExtensionProperties);
    if (!EnumeratePhysicalDevices || !GetPhysicalDeviceProperties2 || !GetPhysicalDeviceFeatures2 ||
        !GetPhysicalDeviceMemoryProperties || !GetPhysicalDeviceQueueFamilyProperties ||
        !GetPhysicalDeviceFormatProperties || !EnumerateDeviceExtensionProperties) {
        line("FAIL an instance command is missing");
        return 1;
    }

    VkPhysicalDevice device = VK_NULL_HANDLE;
    uint32_t device_count = 1;
    result = EnumeratePhysicalDevices(instance, &device_count, &device);
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || device_count == 0) {
        line("FAIL no physical device (result %d)", (int)result);
        return 1;
    }

    /* Properties and limits. */
    VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                              .pNext = &driver};
    GetPhysicalDeviceProperties2(device, &properties);
    const VkPhysicalDeviceProperties *p = &properties.properties;
    const VkPhysicalDeviceLimits *l = &p->limits;
    line("== device");
    line("name: %s", p->deviceName);
    line("api: %u.%u.%u  driver: %s %s  conformance: %u.%u.%u.%u", VK_API_VERSION_MAJOR(p->apiVersion),
         VK_API_VERSION_MINOR(p->apiVersion), VK_API_VERSION_PATCH(p->apiVersion), driver.driverName,
         driver.driverInfo, driver.conformanceVersion.major, driver.conformanceVersion.minor,
         driver.conformanceVersion.subminor, driver.conformanceVersion.patch);
    line("vendor 0x%04X device 0x%04X type %d", p->vendorID, p->deviceID, (int)p->deviceType);

    line("== limits");
#define LIMIT_U(name) line("%-44s %llu", #name, (unsigned long long)l->name)
    LIMIT_U(maxImageDimension2D);
    LIMIT_U(maxImageDimension3D);
    LIMIT_U(maxImageDimensionCube);
    LIMIT_U(maxImageArrayLayers);
    LIMIT_U(maxFramebufferWidth);
    LIMIT_U(maxFramebufferHeight);
    LIMIT_U(maxColorAttachments);
    LIMIT_U(maxBoundDescriptorSets);
    LIMIT_U(maxPerStageDescriptorSamplers);
    LIMIT_U(maxPerStageDescriptorSampledImages);
    LIMIT_U(maxPerStageDescriptorUniformBuffers);
    LIMIT_U(maxPerStageDescriptorStorageBuffers);
    LIMIT_U(maxPerStageResources);
    LIMIT_U(maxSamplerAllocationCount);
    LIMIT_U(maxMemoryAllocationCount);
    LIMIT_U(maxUniformBufferRange);
    LIMIT_U(maxStorageBufferRange);
    LIMIT_U(maxPushConstantsSize);
    LIMIT_U(maxVertexInputAttributes);
    LIMIT_U(maxVertexInputBindings);
    LIMIT_U(maxVertexOutputComponents);
    LIMIT_U(maxGeometryInputComponents);
    LIMIT_U(maxGeometryOutputComponents);
    LIMIT_U(maxGeometryOutputVertices);
    LIMIT_U(maxFragmentInputComponents);
    LIMIT_U(maxFragmentOutputAttachments);
    LIMIT_U(maxFragmentCombinedOutputResources);
    LIMIT_U(maxViewports);
    LIMIT_U(maxClipDistances);
    LIMIT_U(minUniformBufferOffsetAlignment);
    LIMIT_U(minStorageBufferOffsetAlignment);
    LIMIT_U(minTexelBufferOffsetAlignment);
    LIMIT_U(bufferImageGranularity);
    LIMIT_U(nonCoherentAtomSize);
    LIMIT_U(optimalBufferCopyOffsetAlignment);
    LIMIT_U(optimalBufferCopyRowPitchAlignment);
    LIMIT_U(standardSampleLocations);
    line("%-44s %g", "maxSamplerAnisotropy", (double)l->maxSamplerAnisotropy);
    line("%-44s %g", "maxSamplerLodBias", (double)l->maxSamplerLodBias);
    samples("framebufferColorSampleCounts", l->framebufferColorSampleCounts);
    samples("framebufferDepthSampleCounts", l->framebufferDepthSampleCounts);
    samples("framebufferStencilSampleCounts", l->framebufferStencilSampleCounts);
    samples("sampledImageColorSampleCounts", l->sampledImageColorSampleCounts);
    samples("sampledImageDepthSampleCounts", l->sampledImageDepthSampleCounts);
    samples("storageImageSampleCounts", l->storageImageSampleCounts);

    /* Features. Extension structures are queried whether or not the extension
     * is listed; a driver leaves a structure it does not know untouched. */
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlock = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT border = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, .pNext = &interlock};
    VkPhysicalDeviceRobustness2FeaturesEXT robustness2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, .pNext = &border};
    VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT non_seamless = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NON_SEAMLESS_CUBE_MAP_FEATURES_EXT, .pNext = &robustness2};
    VkPhysicalDeviceVulkan13Features features13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                                   .pNext = &non_seamless};
    VkPhysicalDeviceVulkan12Features features12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                                   .pNext = &features13};
    VkPhysicalDeviceVulkan11Features features11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                                   .pNext = &features12};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                          .pNext = &features11};
    GetPhysicalDeviceFeatures2(device, &features);
    const VkPhysicalDeviceFeatures *f = &features.features;

    line("== core features");
#define FEATURE(name) line("%-44s %s", #name, f->name ? "yes" : "NO")
    FEATURE(robustBufferAccess);
    FEATURE(fullDrawIndexUint32);
    FEATURE(imageCubeArray);
    FEATURE(independentBlend);
    FEATURE(geometryShader);
    FEATURE(tessellationShader);
    FEATURE(sampleRateShading);
    FEATURE(dualSrcBlend);
    FEATURE(logicOp);
    FEATURE(multiDrawIndirect);
    FEATURE(drawIndirectFirstInstance);
    FEATURE(depthClamp);
    FEATURE(depthBiasClamp);
    FEATURE(fillModeNonSolid);
    FEATURE(depthBounds);
    FEATURE(wideLines);
    FEATURE(largePoints);
    FEATURE(alphaToOne);
    FEATURE(multiViewport);
    FEATURE(samplerAnisotropy);
    FEATURE(textureCompressionETC2);
    FEATURE(textureCompressionASTC_LDR);
    FEATURE(textureCompressionBC);
    FEATURE(occlusionQueryPrecise);
    FEATURE(pipelineStatisticsQuery);
    FEATURE(vertexPipelineStoresAndAtomics);
    FEATURE(fragmentStoresAndAtomics);
    FEATURE(shaderTessellationAndGeometryPointSize);
    FEATURE(shaderImageGatherExtended);
    FEATURE(shaderStorageImageExtendedFormats);
    FEATURE(shaderStorageImageMultisample);
    FEATURE(shaderStorageImageReadWithoutFormat);
    FEATURE(shaderStorageImageWriteWithoutFormat);
    FEATURE(shaderUniformBufferArrayDynamicIndexing);
    FEATURE(shaderSampledImageArrayDynamicIndexing);
    FEATURE(shaderStorageBufferArrayDynamicIndexing);
    FEATURE(shaderStorageImageArrayDynamicIndexing);
    FEATURE(shaderClipDistance);
    FEATURE(shaderCullDistance);
    FEATURE(shaderFloat64);
    FEATURE(shaderInt64);
    FEATURE(shaderInt16);
    FEATURE(shaderResourceResidency);
    FEATURE(shaderResourceMinLod);
    FEATURE(sparseBinding);
    FEATURE(sparseResidencyBuffer);
    FEATURE(sparseResidencyImage2D);
    FEATURE(variableMultisampleRate);
    FEATURE(inheritedQueries);

    line("== other features");
#define OTHER(structure, name) line("%-44s %s", #name, structure.name ? "yes" : "NO")
    OTHER(features11, multiview);
    OTHER(features11, shaderDrawParameters);
    OTHER(features12, samplerMirrorClampToEdge);
    OTHER(features12, drawIndirectCount);
    OTHER(features12, descriptorIndexing);
    OTHER(features12, shaderOutputViewportIndex);
    OTHER(features12, shaderOutputLayer);
    OTHER(features12, bufferDeviceAddress);
    OTHER(features12, timelineSemaphore);
    OTHER(features12, separateDepthStencilLayouts);
    OTHER(features12, imagelessFramebuffer);
    OTHER(features13, dynamicRendering);
    OTHER(features13, synchronization2);
    OTHER(features13, shaderDemoteToHelperInvocation);
    OTHER(features13, maintenance4);
    OTHER(interlock, fragmentShaderSampleInterlock);
    OTHER(interlock, fragmentShaderPixelInterlock);
    OTHER(border, customBorderColors);
    OTHER(border, customBorderColorWithoutFormat);
    OTHER(robustness2, robustBufferAccess2);
    OTHER(robustness2, robustImageAccess2);
    OTHER(robustness2, nullDescriptor);
    OTHER(non_seamless, nonSeamlessCubeMap);

    /* Extensions. */
    static VkExtensionProperties extensions[512];
    uint32_t extension_count = 512;
    result = EnumerateDeviceExtensionProperties(device, NULL, &extension_count, extensions);
    line("== device extensions (%u%s)", extension_count, result == VK_INCOMPLETE ? ", list truncated" : "");
    for (uint32_t i = 0; i < extension_count; i++) line("  %s", extensions[i].extensionName);

    static const char *const wanted[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME,
        VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME,
        VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
        VK_EXT_NON_SEAMLESS_CUBE_MAP_EXTENSION_NAME,
        VK_EXT_ROBUSTNESS_2_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME,
    };
    line("== extensions the PC Vulkan run enabled, plus host-memory import");
    for (size_t w = 0; w < sizeof wanted / sizeof wanted[0]; w++) {
        int present = 0;
        for (uint32_t i = 0; i < extension_count; i++) present |= strcmp(extensions[i].extensionName, wanted[w]) == 0;
        line("%-44s %s", wanted[w], present ? "yes" : "NO");
    }

    /* Memory and queues. */
    VkPhysicalDeviceMemoryProperties memory;
    GetPhysicalDeviceMemoryProperties(device, &memory);
    line("== memory");
    for (uint32_t i = 0; i < memory.memoryHeapCount; i++)
        line("heap %u: %llu MiB flags 0x%X", i, (unsigned long long)(memory.memoryHeaps[i].size >> 20),
             memory.memoryHeaps[i].flags);
    for (uint32_t i = 0; i < memory.memoryTypeCount; i++)
        line("type %u: heap %u flags 0x%X%s%s%s%s", i, memory.memoryTypes[i].heapIndex,
             memory.memoryTypes[i].propertyFlags,
             memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? " device-local" : "",
             memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? " host-visible" : "",
             memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? " host-coherent" : "",
             memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? " host-cached" : "");

    static VkQueueFamilyProperties families[16];
    uint32_t family_count = 16;
    GetPhysicalDeviceQueueFamilyProperties(device, &family_count, families);
    line("== queue families");
    for (uint32_t i = 0; i < family_count; i++)
        line("family %u: %u queue(s) flags 0x%X%s%s%s%s", i, families[i].queueCount, families[i].queueFlags,
             families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT ? " graphics" : "",
             families[i].queueFlags & VK_QUEUE_COMPUTE_BIT ? " compute" : "",
             families[i].queueFlags & VK_QUEUE_TRANSFER_BIT ? " transfer" : "",
             families[i].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT ? " sparse" : "");

    /* Formats: S sampled, L linear filter, C colour attachment, B blend,
     * D depth-stencil attachment, T storage image. Optimal tiling. */
    static const struct {
        VkFormat format;
        const char *name;
    } formats[] = {
#define FORMAT(name) {VK_FORMAT_##name, #name}
        FORMAT(BC1_RGB_UNORM_BLOCK),      FORMAT(BC1_RGBA_UNORM_BLOCK),     FORMAT(BC2_UNORM_BLOCK),
        FORMAT(BC3_UNORM_BLOCK),          FORMAT(BC4_UNORM_BLOCK),          FORMAT(BC5_UNORM_BLOCK),
        FORMAT(BC5_SNORM_BLOCK),          FORMAT(BC6H_UFLOAT_BLOCK),        FORMAT(BC7_UNORM_BLOCK),
        FORMAT(R5G6B5_UNORM_PACK16),      FORMAT(B5G6R5_UNORM_PACK16),      FORMAT(A1R5G5B5_UNORM_PACK16),
        FORMAT(R5G5B5A1_UNORM_PACK16),    FORMAT(B5G5R5A1_UNORM_PACK16),    FORMAT(R4G4B4A4_UNORM_PACK16),
        FORMAT(B4G4R4A4_UNORM_PACK16),    FORMAT(A4R4G4B4_UNORM_PACK16),    FORMAT(R8_UNORM),
        FORMAT(R8_SNORM),                 FORMAT(R8G8_UNORM),               FORMAT(R8G8_SNORM),
        FORMAT(R8G8B8A8_UNORM),           FORMAT(R8G8B8A8_SNORM),           FORMAT(R8G8B8A8_SRGB),
        FORMAT(B8G8R8A8_UNORM),           FORMAT(A2B10G10R10_UNORM_PACK32), FORMAT(A2R10G10B10_UNORM_PACK32),
        FORMAT(R16_UNORM),                FORMAT(R16_SNORM),                FORMAT(R16G16_UNORM),
        FORMAT(R16G16_SNORM),             FORMAT(R16G16B16A16_UNORM),       FORMAT(R16G16B16A16_SNORM),
        FORMAT(R16_SFLOAT),               FORMAT(R16G16_SFLOAT),            FORMAT(R16G16B16A16_SFLOAT),
        FORMAT(R32_SFLOAT),               FORMAT(R32G32_SFLOAT),            FORMAT(R32G32B32A32_SFLOAT),
        FORMAT(R32_UINT),                 FORMAT(R32G32_UINT),              FORMAT(B10G11R11_UFLOAT_PACK32),
        FORMAT(E5B9G9R9_UFLOAT_PACK32),   FORMAT(D16_UNORM),                FORMAT(D24_UNORM_S8_UINT),
        FORMAT(X8_D24_UNORM_PACK32),      FORMAT(D32_SFLOAT),               FORMAT(D32_SFLOAT_S8_UINT),
        FORMAT(S8_UINT),
    };
    line("== formats (optimal tiling): S sampled, L linear filter, C colour target, B blend, D depth target, T storage");
    for (size_t i = 0; i < sizeof formats / sizeof formats[0]; i++) {
        VkFormatProperties fp;
        GetPhysicalDeviceFormatProperties(device, formats[i].format, &fp);
        const VkFormatFeatureFlags o = fp.optimalTilingFeatures;
        line("%-28s %s%s%s%s%s%s%s", formats[i].name, o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT ? "S" : "-",
             o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT ? "L" : "-",
             o & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT ? "C" : "-",
             o & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT ? "B" : "-",
             o & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT ? "D" : "-",
             o & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT ? "T" : "-", o ? "" : "  (unsupported)");
    }

    if (DestroyInstance) DestroyInstance(instance, NULL);
    line("mcla-vkinfo ends");
    if (out) fclose(out);
    return 0;
}
