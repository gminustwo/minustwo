/* macOS: the Vulkan driver is KosmicKrisp (Mesa on Metal), shipped with the port: the loader is
 * pointed at its manifest beside the executable (a build in out/) or in the app bundle's resources
 * when minustwo starts, unless VK_DRIVER_FILES chooses a driver. The programs that create their own
 * instance (vulkan_smoke.c, tools/gpu_capabilities.c) also ask for portability drivers, as the
 * renderer does (vk_platform.cpp). Elsewhere, nothing. */
#ifndef MT_VULKAN_DRIVER_H
#define MT_VULKAN_DRIVER_H
#include <vulkan/vulkan.h>
#ifdef __APPLE__
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach-o/dyld.h>
static inline void mt_vulkan_driver(void) {
    if (getenv("VK_DRIVER_FILES") || getenv("VK_ICD_FILENAMES")) return;
    char executable[PATH_MAX], folder[PATH_MAX], manifest[PATH_MAX + 64];
    uint32_t length = sizeof(executable);
    if (_NSGetExecutablePath(executable, &length) || !realpath(executable, folder)) return;
    char *slash = strrchr(folder, '/');
    if (!slash) return;
    *slash = 0;
    static const char *const places[] = {"/kosmickrisp_mesa_icd.json",
                                         "/../Resources/vulkan/icd.d/kosmickrisp_mesa_icd.json"};
    for (size_t i = 0; i < sizeof(places) / sizeof(*places); ++i) {
        snprintf(manifest, sizeof(manifest), "%s%s", folder, places[i]);
        if (!access(manifest, R_OK)) { setenv("VK_DRIVER_FILES", manifest, 1); return; }
    }
}
/* Adds portability enumeration to an instance create info whose extension list is empty. */
static inline void mt_vulkan_portability(VkInstanceCreateInfo *create) {
    static const char *const extensions[] = {VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME};
    mt_vulkan_driver();
    create->flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    create->enabledExtensionCount = 1;
    create->ppEnabledExtensionNames = extensions;
}
#else
static inline void mt_vulkan_driver(void) {}
static inline void mt_vulkan_portability(VkInstanceCreateInfo *create) { (void)create; }
#endif
#endif
