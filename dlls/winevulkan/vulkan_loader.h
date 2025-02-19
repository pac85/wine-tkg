/* Wine Vulkan ICD private data structures
 *
 * Copyright 2017 Roderick Colenbrander
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef __WINE_VULKAN_LOADER_H
#define __WINE_VULKAN_LOADER_H

#include <stdarg.h>
#include <stdlib.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include <stdarg.h>
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wine/debug.h"
#include "wine/vulkan.h"
#include "wine/vulkan_driver.h"
#include "wine/unixlib.h"

#include "loader_thunks.h"

/* Magic value defined by Vulkan ICD / Loader spec */
#define VULKAN_ICD_MAGIC_VALUE 0x01CDC0DE

#define WINEVULKAN_QUIRK_GET_DEVICE_PROC_ADDR 0x00000001
#define WINEVULKAN_QUIRK_ADJUST_MAX_IMAGE_COUNT 0x00000002
#define WINEVULKAN_QUIRK_IGNORE_EXPLICIT_LAYERS 0x00000004

/* Base 'class' for our Vulkan dispatchable objects such as VkDevice and VkInstance.
 * This structure MUST be the first element of a dispatchable object as the ICD
 * loader depends on it. For now only contains loader_magic, but over time more common
 * functionality is expected.
 */
struct wine_vk_base
{
    /* Special section in each dispatchable object for use by the ICD loader for
     * storing dispatch tables. The start contains a magical value '0x01CDC0DE'.
     */
    UINT_PTR loader_magic;
};

struct wine_vk_device_base
{
    struct wine_vk_base base;
    unsigned int quirks;
};

struct vulkan_func
{
    const char *name;
    void *func;
};

void *wine_vk_get_device_proc_addr(const char *name) DECLSPEC_HIDDEN;
void *wine_vk_get_phys_dev_proc_addr(const char *name) DECLSPEC_HIDDEN;
void *wine_vk_get_instance_proc_addr(const char *name) DECLSPEC_HIDDEN;

/* debug callbacks params */

struct wine_vk_debug_utils_params
{
    PFN_vkDebugUtilsMessengerCallbackEXT user_callback;
    void *user_data;

    VkDebugUtilsMessageSeverityFlagBitsEXT severity;
    VkDebugUtilsMessageTypeFlagsEXT message_types;
    VkDebugUtilsMessengerCallbackDataEXT data;
};

struct wine_vk_debug_report_params
{
    PFN_vkDebugReportCallbackEXT user_callback;
    void *user_data;

    VkDebugReportFlagsEXT flags;
    VkDebugReportObjectTypeEXT object_type;
    uint64_t object_handle;
    size_t location;
    int32_t code;
    const char *layer_prefix;
    const char *message;
};

extern const struct unix_funcs *unix_funcs;
extern unixlib_handle_t unix_handle DECLSPEC_HIDDEN;

static inline NTSTATUS vk_unix_call(enum unix_call code, void *params)
{
    return __wine_unix_call(unix_handle, code, params);
}

struct unix_funcs
{
    NTSTATUS (WINAPI *p_vk_call)(enum unix_call, void *);
    BOOL (WINAPI *p_is_available_instance_function)(VkInstance, const char *);
    BOOL (WINAPI *p_is_available_device_function)(VkDevice, const char *);
};

#endif /* __WINE_VULKAN_LOADER_H */
