/*
 * Raw Input
 *
 * Copyright 2012 Henri Verbeet
 * Copyright 2018 Zebediah Figura for CodeWeavers
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

#include <stdarg.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winioctl.h"
#include "winnls.h"
#include "winreg.h"
#include "winuser.h"
#include "ddk/hidclass.h"
#include "wine/debug.h"
#include "wine/server.h"
#include "wine/hid.h"

#include "user_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(rawinput);

struct device
{
    WCHAR *path;
    HANDLE file;
    HANDLE handle;
    RID_DEVICE_INFO info;
    struct hid_preparsed_data *data;
};

static struct device *rawinput_devices;
static unsigned int rawinput_devices_count, rawinput_devices_max;

static CRITICAL_SECTION rawinput_devices_cs;
static CRITICAL_SECTION_DEBUG rawinput_devices_cs_debug =
{
    0, 0, &rawinput_devices_cs,
    { &rawinput_devices_cs_debug.ProcessLocksList, &rawinput_devices_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": rawinput_devices_cs") }
};
static CRITICAL_SECTION rawinput_devices_cs = { &rawinput_devices_cs_debug, -1, 0, 0, 0, 0 };

static BOOL array_reserve(void **elements, unsigned int *capacity, unsigned int count, unsigned int size)
{
    unsigned int new_capacity, max_capacity;
    void *new_elements;

    if (count <= *capacity)
        return TRUE;

    max_capacity = ~(SIZE_T)0 / size;
    if (count > max_capacity)
        return FALSE;

    new_capacity = max(4, *capacity);
    while (new_capacity < count && new_capacity <= max_capacity / 2)
        new_capacity *= 2;
    if (new_capacity < count)
        new_capacity = max_capacity;

    if (!(new_elements = realloc(*elements, new_capacity * size)))
        return FALSE;

    *elements = new_elements;
    *capacity = new_capacity;

    return TRUE;
}

static ULONG query_reg_value( HKEY hkey, const WCHAR *name,
                              KEY_VALUE_PARTIAL_INFORMATION *info, ULONG size )
{
    unsigned int name_size = name ? lstrlenW( name ) * sizeof(WCHAR) : 0;
    UNICODE_STRING nameW = { name_size, name_size, (WCHAR *)name };

    if (NtQueryValueKey( hkey, &nameW, KeyValuePartialInformation,
                         info, size, &size ))
        return 0;

    return size - FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data);
}

static struct device *add_device( HKEY key, DWORD type )
{
    static const WCHAR symbolic_linkW[] = {'S','y','m','b','o','l','i','c','L','i','n','k',0};
    char value_buffer[4096];
    KEY_VALUE_PARTIAL_INFORMATION *value = (KEY_VALUE_PARTIAL_INFORMATION *)value_buffer;
    static const RID_DEVICE_INFO_KEYBOARD keyboard_info = {0, 0, 1, 12, 3, 101};
    static const RID_DEVICE_INFO_MOUSE mouse_info = {1, 5, 0, FALSE};
    struct hid_preparsed_data *preparsed = NULL;
    HID_COLLECTION_INFORMATION hid_info;
    struct device *device = NULL;
    RID_DEVICE_INFO info;
    IO_STATUS_BLOCK io;
    WCHAR *path, *pos;
    NTSTATUS status;
    unsigned int i;
    UINT32 handle;
    HANDLE file;

    if (!query_reg_value( key, symbolic_linkW, value, sizeof(value_buffer) ))
    {
        ERR( "failed to get symbolic link value\n" );
        return NULL;
    }

    if (!(path = malloc( value->DataLength + sizeof(WCHAR) )))
        return NULL;
    memcpy( path, value->Data, value->DataLength );
    path[value->DataLength / sizeof(WCHAR)] = 0;

    /* upper case everything but the GUID */
    for (pos = path; *pos && *pos != '{'; pos++) *pos = towupper(*pos);

    file = CreateFileW( path, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, 0 );
    if (file == INVALID_HANDLE_VALUE)
    {
        ERR( "Failed to open device file %s, error %lu.\n", debugstr_w(path), GetLastError() );
        free( path );
        return NULL;
    }

    status = NtDeviceIoControlFile( file, NULL, NULL, NULL, &io,
                                    IOCTL_HID_GET_WINE_RAWINPUT_HANDLE,
                                    NULL, 0, &handle, sizeof(handle) );
    if (status)
    {
        ERR( "Failed to get raw input handle, status %#lx.\n", status );
        goto fail;
    }

    memset( &info, 0, sizeof(info) );
    info.cbSize = sizeof(info);
    info.dwType = type;

    switch (type)
    {
    case RIM_TYPEHID:
        status = NtDeviceIoControlFile( file, NULL, NULL, NULL, &io,
                                        IOCTL_HID_GET_COLLECTION_INFORMATION,
                                        NULL, 0, &hid_info, sizeof(hid_info) );
        if (status)
        {
            ERR( "Failed to get collection information, status %#lx.\n", status );
            goto fail;
        }

        info.hid.dwVendorId = hid_info.VendorID;
        info.hid.dwProductId = hid_info.ProductID;
        info.hid.dwVersionNumber = hid_info.VersionNumber;

        if (!(preparsed = malloc( hid_info.DescriptorSize )))
        {
            ERR( "Failed to allocate memory.\n" );
            goto fail;
        }

        status = NtDeviceIoControlFile( file, NULL, NULL, NULL, &io,
                                        IOCTL_HID_GET_COLLECTION_DESCRIPTOR,
                                        NULL, 0, preparsed, hid_info.DescriptorSize );
        if (status)
        {
            ERR( "Failed to get collection descriptor, status %#lx.\n", status );
            goto fail;
        }

        info.hid.usUsagePage = preparsed->usage_page;
        info.hid.usUsage = preparsed->usage;
        break;

    case RIM_TYPEMOUSE:
        info.mouse = mouse_info;
        break;

    case RIM_TYPEKEYBOARD:
        info.keyboard = keyboard_info;
        break;
    }

    for (i = 0; i < rawinput_devices_count && !device; ++i)
        if (rawinput_devices[i].handle == UlongToHandle(handle))
            device = rawinput_devices + i;

    if (device)
    {
        TRACE( "Updating device %#x / %s.\n", handle, debugstr_w(path) );
        free(device->data);
        CloseHandle(device->file);
        free( device->path );
    }
    else if (array_reserve((void **)&rawinput_devices, &rawinput_devices_max,
                           rawinput_devices_count + 1, sizeof(*rawinput_devices)))
    {
        device = &rawinput_devices[rawinput_devices_count++];
        TRACE( "Adding device %#x / %s.\n", handle, debugstr_w(path) );
    }
    else
    {
        ERR("Failed to allocate memory.\n");
        goto fail;
    }

    device->path = path;
    device->file = file;
    device->handle = ULongToHandle(handle);
    device->info = info;
    device->data = preparsed;

    return device;

fail:
    free( preparsed );
    CloseHandle( file );
    free( path );
    return NULL;
}

static HKEY reg_open_key( HKEY root, const WCHAR *name, ULONG name_len )
{
    UNICODE_STRING nameW = { name_len, name_len, (WCHAR *)name };
    OBJECT_ATTRIBUTES attr;
    HANDLE ret;

    attr.Length = sizeof(attr);
    attr.RootDirectory = root;
    attr.ObjectName = &nameW;
    attr.Attributes = 0;
    attr.SecurityDescriptor = NULL;
    attr.SecurityQualityOfService = NULL;

    if (NtOpenKeyEx( &ret, MAXIMUM_ALLOWED, &attr, 0 )) return 0;
    return ret;
}

static const WCHAR device_classesW[] = L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\DeviceClasses\\";
static const WCHAR guid_devinterface_hidW[] = L"{4d1e55b2-f16f-11cf-88cb-001111000030}";
static const WCHAR guid_devinterface_keyboardW[] = L"{884b96c3-56ef-11d1-bc8c-00a0c91405dd}";
static const WCHAR guid_devinterface_mouseW[] = L"{378de44c-56ef-11d1-bc8c-00a0c91405dd}";

static void enumerate_devices( DWORD type, const WCHAR *class )
{
    WCHAR buffer[1024];
    KEY_NODE_INFORMATION *subkey_info = (void *)buffer;
    HKEY class_key, device_key, iface_key;
    unsigned int i, j;
    DWORD size;

    wcscpy( buffer, device_classesW );
    wcscat( buffer, class );
    if (!(class_key = reg_open_key( NULL, buffer, wcslen( buffer ) * sizeof(WCHAR) )))
        return;

    for (i = 0; !NtEnumerateKey( class_key, i, KeyNodeInformation, buffer, sizeof(buffer), &size ); ++i)
    {
        if (!(device_key = reg_open_key( class_key, subkey_info->Name, subkey_info->NameLength )))
        {
            ERR( "failed to open %s\n", debugstr_wn(subkey_info->Name, subkey_info->NameLength / sizeof(WCHAR)) );
            continue;
        }

        for (j = 0; !NtEnumerateKey( device_key, j, KeyNodeInformation, buffer, sizeof(buffer), &size ); ++j)
        {
            if (!(iface_key = reg_open_key( device_key, subkey_info->Name, subkey_info->NameLength )))
            {
                ERR( "failed to open %s\n", debugstr_wn(subkey_info->Name, subkey_info->NameLength / sizeof(WCHAR)) );
                continue;
            }

            add_device( iface_key, type );
            NtClose( iface_key );
        }

        NtClose( device_key );
    }

    NtClose( class_key );
}

void rawinput_update_device_list(void)
{
    DWORD idx;

    TRACE("\n");

    EnterCriticalSection(&rawinput_devices_cs);

    /* destroy previous list */
    for (idx = 0; idx < rawinput_devices_count; ++idx)
    {
        free(rawinput_devices[idx].data);
        CloseHandle(rawinput_devices[idx].file);
        free( rawinput_devices[idx].path );
    }
    rawinput_devices_count = 0;

    enumerate_devices( RIM_TYPEHID, guid_devinterface_hidW );
    enumerate_devices( RIM_TYPEMOUSE, guid_devinterface_mouseW );
    enumerate_devices( RIM_TYPEKEYBOARD, guid_devinterface_keyboardW );

    LeaveCriticalSection(&rawinput_devices_cs);
}


static struct device *find_device_from_handle(HANDLE handle)
{
    UINT i;
    for (i = 0; i < rawinput_devices_count; ++i)
        if (rawinput_devices[i].handle == handle)
            return rawinput_devices + i;
    rawinput_update_device_list();
    for (i = 0; i < rawinput_devices_count; ++i)
        if (rawinput_devices[i].handle == handle)
            return rawinput_devices + i;
    return NULL;
}


BOOL rawinput_device_get_usages(HANDLE handle, USAGE *usage_page, USAGE *usage)
{
    struct device *device;

    *usage_page = *usage = 0;

    if (!(device = find_device_from_handle(handle))) return FALSE;
    if (device->info.dwType != RIM_TYPEHID) return FALSE;

    *usage_page = device->info.hid.usUsagePage;
    *usage = device->info.hid.usUsage;
    return TRUE;
}


struct rawinput_thread_data *rawinput_thread_data(void)
{
    struct user_thread_info *thread_info = get_user_thread_info();
    struct rawinput_thread_data *data = thread_info->rawinput;
    if (data) return data;
    data = thread_info->rawinput = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                                              RAWINPUT_BUFFER_SIZE + sizeof(struct user_thread_info) );
    return data;
}


BOOL rawinput_from_hardware_message(RAWINPUT *rawinput, const struct hardware_msg_data *msg_data)
{
    SIZE_T size;

    rawinput->header.dwType = msg_data->rawinput.type;
    if (msg_data->rawinput.type == RIM_TYPEMOUSE)
    {
        static const unsigned int button_flags[] =
        {
            0,                              /* MOUSEEVENTF_MOVE */
            RI_MOUSE_LEFT_BUTTON_DOWN,      /* MOUSEEVENTF_LEFTDOWN */
            RI_MOUSE_LEFT_BUTTON_UP,        /* MOUSEEVENTF_LEFTUP */
            RI_MOUSE_RIGHT_BUTTON_DOWN,     /* MOUSEEVENTF_RIGHTDOWN */
            RI_MOUSE_RIGHT_BUTTON_UP,       /* MOUSEEVENTF_RIGHTUP */
            RI_MOUSE_MIDDLE_BUTTON_DOWN,    /* MOUSEEVENTF_MIDDLEDOWN */
            RI_MOUSE_MIDDLE_BUTTON_UP,      /* MOUSEEVENTF_MIDDLEUP */
        };
        unsigned int i;

        rawinput->header.dwSize  = FIELD_OFFSET(RAWINPUT, data) + sizeof(RAWMOUSE);
        rawinput->header.hDevice = WINE_MOUSE_HANDLE;
        rawinput->header.wParam  = 0;

        rawinput->data.mouse.usFlags = msg_data->flags & MOUSEEVENTF_ABSOLUTE ? MOUSE_MOVE_ABSOLUTE : MOUSE_MOVE_RELATIVE;
        if (msg_data->flags & MOUSEEVENTF_VIRTUALDESK) rawinput->data.mouse.usFlags |= MOUSE_VIRTUAL_DESKTOP;

        rawinput->data.mouse.usButtonFlags = 0;
        rawinput->data.mouse.usButtonData  = 0;
        for (i = 1; i < ARRAY_SIZE(button_flags); ++i)
        {
            if (msg_data->flags & (1 << i))
                rawinput->data.mouse.usButtonFlags |= button_flags[i];
        }
        if (msg_data->flags & MOUSEEVENTF_WHEEL)
        {
            rawinput->data.mouse.usButtonFlags |= RI_MOUSE_WHEEL;
            rawinput->data.mouse.usButtonData   = msg_data->rawinput.mouse.data;
        }
        if (msg_data->flags & MOUSEEVENTF_HWHEEL)
        {
            rawinput->data.mouse.usButtonFlags |= RI_MOUSE_HORIZONTAL_WHEEL;
            rawinput->data.mouse.usButtonData   = msg_data->rawinput.mouse.data;
        }
        if (msg_data->flags & MOUSEEVENTF_XDOWN)
        {
            if (msg_data->rawinput.mouse.data == XBUTTON1)
                rawinput->data.mouse.usButtonFlags |= RI_MOUSE_BUTTON_4_DOWN;
            else if (msg_data->rawinput.mouse.data == XBUTTON2)
                rawinput->data.mouse.usButtonFlags |= RI_MOUSE_BUTTON_5_DOWN;
        }
        if (msg_data->flags & MOUSEEVENTF_XUP)
        {
            if (msg_data->rawinput.mouse.data == XBUTTON1)
                rawinput->data.mouse.usButtonFlags |= RI_MOUSE_BUTTON_4_UP;
            else if (msg_data->rawinput.mouse.data == XBUTTON2)
                rawinput->data.mouse.usButtonFlags |= RI_MOUSE_BUTTON_5_UP;
        }

        rawinput->data.mouse.ulRawButtons       = 0;
        rawinput->data.mouse.lLastX             = msg_data->rawinput.mouse.x;
        rawinput->data.mouse.lLastY             = msg_data->rawinput.mouse.y;
        rawinput->data.mouse.ulExtraInformation = msg_data->info;
    }
    else if (msg_data->rawinput.type == RIM_TYPEKEYBOARD)
    {
        rawinput->header.dwSize  = FIELD_OFFSET(RAWINPUT, data) + sizeof(RAWKEYBOARD);
        rawinput->header.hDevice = WINE_KEYBOARD_HANDLE;
        rawinput->header.wParam  = 0;

        rawinput->data.keyboard.MakeCode = msg_data->rawinput.kbd.scan;
        rawinput->data.keyboard.Flags    = msg_data->flags & KEYEVENTF_KEYUP ? RI_KEY_BREAK : RI_KEY_MAKE;
        if (msg_data->flags & KEYEVENTF_EXTENDEDKEY) rawinput->data.keyboard.Flags |= RI_KEY_E0;
        rawinput->data.keyboard.Reserved = 0;

        switch (msg_data->rawinput.kbd.vkey)
        {
        case VK_LSHIFT:
        case VK_RSHIFT:
            rawinput->data.keyboard.VKey   = VK_SHIFT;
            rawinput->data.keyboard.Flags &= ~RI_KEY_E0;
            break;
        case VK_LCONTROL:
        case VK_RCONTROL:
            rawinput->data.keyboard.VKey = VK_CONTROL;
            break;
        case VK_LMENU:
        case VK_RMENU:
            rawinput->data.keyboard.VKey = VK_MENU;
            break;
        default:
            rawinput->data.keyboard.VKey = msg_data->rawinput.kbd.vkey;
            break;
        }

        rawinput->data.keyboard.Message          = msg_data->rawinput.kbd.message;
        rawinput->data.keyboard.ExtraInformation = msg_data->info;
    }
    else if (msg_data->rawinput.type == RIM_TYPEHID)
    {
        size = msg_data->size - sizeof(*msg_data);
        if (size > rawinput->header.dwSize - sizeof(*rawinput)) return FALSE;

        rawinput->header.dwSize  = FIELD_OFFSET( RAWINPUT, data.hid.bRawData ) + size;
        rawinput->header.hDevice = ULongToHandle( msg_data->rawinput.hid.device );
        rawinput->header.wParam  = 0;

        rawinput->data.hid.dwCount = msg_data->rawinput.hid.count;
        rawinput->data.hid.dwSizeHid = msg_data->rawinput.hid.length;
        memcpy( rawinput->data.hid.bRawData, msg_data + 1, size );
    }
    else
    {
        FIXME("Unhandled rawinput type %#x.\n", msg_data->rawinput.type);
        return FALSE;
    }

    return TRUE;
}


/***********************************************************************
 *              GetRawInputDeviceList   (USER32.@)
 */
UINT WINAPI GetRawInputDeviceList(RAWINPUTDEVICELIST *devices, UINT *device_count, UINT size)
{
    static UINT last_check;
    UINT i, ticks = GetTickCount();

    TRACE("devices %p, device_count %p, size %u.\n", devices, device_count, size);

    if (size != sizeof(*devices))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return ~0U;
    }

    if (!device_count)
    {
        SetLastError(ERROR_NOACCESS);
        return ~0U;
    }

    if (ticks - last_check > 2000)
    {
        last_check = ticks;
        rawinput_update_device_list();
    }

    if (!devices)
    {
        *device_count = rawinput_devices_count;
        return 0;
    }

    if (*device_count < rawinput_devices_count)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        *device_count = rawinput_devices_count;
        return ~0U;
    }

    for (i = 0; i < rawinput_devices_count; ++i)
    {
        devices[i].hDevice = rawinput_devices[i].handle;
        devices[i].dwType = rawinput_devices[i].info.dwType;
    }

    return rawinput_devices_count;
}

/***********************************************************************
 *              RegisterRawInputDevices   (USER32.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH RegisterRawInputDevices(const RAWINPUTDEVICE *devices, UINT device_count, UINT size)
{
    struct rawinput_device *d;
    BOOL ret;
    UINT i;

    TRACE("devices %p, device_count %u, size %u.\n", devices, device_count, size);

    if (size != sizeof(*devices))
    {
        WARN("Invalid structure size %u.\n", size);
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    for (i = 0; i < device_count; ++i)
    {
        if ((devices[i].dwFlags & RIDEV_INPUTSINK) &&
            (devices[i].hwndTarget == NULL))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }

        if ((devices[i].dwFlags & RIDEV_REMOVE) &&
            (devices[i].hwndTarget != NULL))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
    }

    if (!(d = HeapAlloc( GetProcessHeap(), 0, device_count * sizeof(*d) ))) return FALSE;

    for (i = 0; i < device_count; ++i)
    {
        TRACE("device %u: page %#x, usage %#x, flags %#lx, target %p.\n",
                i, devices[i].usUsagePage, devices[i].usUsage,
                devices[i].dwFlags, devices[i].hwndTarget);
        if (devices[i].dwFlags & ~(RIDEV_REMOVE|RIDEV_NOLEGACY|RIDEV_INPUTSINK|RIDEV_DEVNOTIFY))
            FIXME("Unhandled flags %#lx for device %u.\n", devices[i].dwFlags, i);

        d[i].usage_page = devices[i].usUsagePage;
        d[i].usage = devices[i].usUsage;
        d[i].flags = devices[i].dwFlags;
        d[i].target = wine_server_user_handle( devices[i].hwndTarget );
    }

    SERVER_START_REQ( update_rawinput_devices )
    {
        wine_server_add_data( req, d, device_count * sizeof(*d) );
        ret = !wine_server_call( req );
    }
    SERVER_END_REQ;

    HeapFree( GetProcessHeap(), 0, d );

    return ret;
}

/***********************************************************************
 *              GetRawInputData   (USER32.@)
 */
UINT WINAPI GetRawInputData(HRAWINPUT rawinput, UINT command, void *data, UINT *data_size, UINT header_size)
{
    struct rawinput_thread_data *thread_data = rawinput_thread_data();
    UINT size;

    TRACE("rawinput %p, command %#x, data %p, data_size %p, header_size %u.\n",
            rawinput, command, data, data_size, header_size);

    if (!rawinput || thread_data->hw_id != (UINT_PTR)rawinput)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return ~0U;
    }

    if (header_size != sizeof(RAWINPUTHEADER))
    {
        WARN("Invalid structure size %u.\n", header_size);
        SetLastError(ERROR_INVALID_PARAMETER);
        return ~0U;
    }

    switch (command)
    {
    case RID_INPUT:
        size = thread_data->buffer->header.dwSize;
        break;
    case RID_HEADER:
        size = sizeof(RAWINPUTHEADER);
        break;
    default:
        SetLastError(ERROR_INVALID_PARAMETER);
        return ~0U;
    }

    if (!data)
    {
        *data_size = size;
        return 0;
    }

    if (*data_size < size)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return ~0U;
    }
    memcpy(data, thread_data->buffer, size);
    return size;
}

#ifdef _WIN64
typedef RAWINPUT RAWINPUT64;
#else
typedef struct
{
    RAWINPUTHEADER header;
    char pad[8];
    union {
        RAWMOUSE    mouse;
        RAWKEYBOARD keyboard;
        RAWHID      hid;
    } data;
} RAWINPUT64;
#endif

/***********************************************************************
 *              GetRawInputBuffer   (USER32.@)
 */
UINT WINAPI DECLSPEC_HOTPATCH GetRawInputBuffer(RAWINPUT *data, UINT *data_size, UINT header_size)
{
    struct hardware_msg_data *msg_data;
    struct rawinput_thread_data *thread_data;
    RAWINPUT *rawinput;
    UINT count = 0, remaining, rawinput_size, next_size, overhead;
    BOOL is_wow64;
    int i;

    if (IsWow64Process( GetCurrentProcess(), &is_wow64 ) && is_wow64)
        rawinput_size = sizeof(RAWINPUT64);
    else
        rawinput_size = sizeof(RAWINPUT);
    overhead = rawinput_size - sizeof(RAWINPUT);

    if (header_size != sizeof(RAWINPUTHEADER))
    {
        WARN("Invalid structure size %u.\n", header_size);
        SetLastError(ERROR_INVALID_PARAMETER);
        return ~0U;
    }

    if (!data_size)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return ~0U;
    }

    if (!data)
    {
        TRACE("data %p, data_size %p (%u), header_size %u\n", data, data_size, *data_size, header_size);
        SERVER_START_REQ( get_rawinput_buffer )
        {
            req->rawinput_size = rawinput_size;
            req->buffer_size = 0;
            if (wine_server_call( req )) return ~0U;
            *data_size = reply->next_size;
        }
        SERVER_END_REQ;
        return 0;
    }

    if (!(thread_data = rawinput_thread_data())) return ~0U;
    rawinput = thread_data->buffer;

    /* first RAWINPUT block in the buffer is used for WM_INPUT message data */
    msg_data = (struct hardware_msg_data *)NEXTRAWINPUTBLOCK(rawinput);
    SERVER_START_REQ( get_rawinput_buffer )
    {
        req->rawinput_size = rawinput_size;
        req->buffer_size = *data_size;
        wine_server_set_reply( req, msg_data, RAWINPUT_BUFFER_SIZE - rawinput->header.dwSize );
        if (wine_server_call( req )) return ~0U;
        next_size = reply->next_size;
        count = reply->count;
    }
    SERVER_END_REQ;

    remaining = *data_size;
    for (i = 0; i < count; ++i)
    {
        data->header.dwSize = remaining;
        if (!rawinput_from_hardware_message(data, msg_data)) break;
        if (overhead) memmove((char *)&data->data + overhead, &data->data,
                              data->header.dwSize - sizeof(RAWINPUTHEADER));
        data->header.dwSize += overhead;
        remaining -= data->header.dwSize;
        data = NEXTRAWINPUTBLOCK(data);
        msg_data = (struct hardware_msg_data *)((char *)msg_data + msg_data->size);
    }

    if (count == 0 && next_size == 0) *data_size = 0;
    else if (next_size == 0) next_size = rawinput_size;

    if (next_size && *data_size <= next_size)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        *data_size = next_size;
        count = ~0U;
    }

    if (count) TRACE("data %p, data_size %p (%u), header_size %u, count %u\n", data, data_size, *data_size, header_size, count);
    return count;
}

/***********************************************************************
 *              GetRawInputDeviceInfoA   (USER32.@)
 */
UINT WINAPI GetRawInputDeviceInfoA(HANDLE device, UINT command, void *data, UINT *data_size)
{
    TRACE("device %p, command %#x, data %p, data_size %p.\n",
            device, command, data, data_size);

    /* RIDI_DEVICENAME data_size is in chars, not bytes */
    if (command == RIDI_DEVICENAME)
    {
        WCHAR *nameW;
        UINT ret, nameW_sz;

        if (!data_size) return ~0U;

        nameW_sz = *data_size;

        if (data && nameW_sz > 0)
            nameW = HeapAlloc(GetProcessHeap(), 0, sizeof(WCHAR) * nameW_sz);
        else
            nameW = NULL;

        ret = GetRawInputDeviceInfoW(device, command, nameW, &nameW_sz);

        if (ret && ret != ~0U)
            WideCharToMultiByte(CP_ACP, 0, nameW, -1, data, *data_size, NULL, NULL);

        *data_size = nameW_sz;

        HeapFree(GetProcessHeap(), 0, nameW);

        return ret;
    }

    return GetRawInputDeviceInfoW(device, command, data, data_size);
}

/***********************************************************************
 *              GetRawInputDeviceInfoW   (USER32.@)
 */
UINT WINAPI GetRawInputDeviceInfoW(HANDLE handle, UINT command, void *data, UINT *data_size)
{
    struct hid_preparsed_data *preparsed;
    RID_DEVICE_INFO info;
    struct device *device;
    DWORD len, data_len;

    TRACE("handle %p, command %#x, data %p, data_size %p.\n",
            handle, command, data, data_size);

    if (!data_size)
    {
        SetLastError(ERROR_NOACCESS);
        return ~0U;
    }
    if (!(device = find_device_from_handle(handle)))
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return ~0U;
    }

    data_len = *data_size;
    switch (command)
    {
    case RIDI_DEVICENAME:
        if ((len = wcslen( device->path ) + 1) <= data_len && data)
            memcpy( data, device->path, len * sizeof(WCHAR) );
        *data_size = len;
        break;

    case RIDI_DEVICEINFO:
        if ((len = sizeof(info)) <= data_len && data)
            memcpy(data, &device->info, len);
        *data_size = len;
        break;

    case RIDI_PREPARSEDDATA:
        if (!(preparsed = device->data)) len = 0;
        else len = preparsed->caps_size + FIELD_OFFSET(struct hid_preparsed_data, value_caps[0]) +
                   preparsed->number_link_collection_nodes * sizeof(struct hid_collection_node);

        if (preparsed && len <= data_len && data)
            memcpy(data, preparsed, len);
        *data_size = len;
        break;

    default:
        FIXME("command %#x not supported\n", command);
        SetLastError(ERROR_INVALID_PARAMETER);
        return ~0U;
    }

    if (!data)
        return 0;

    if (data_len < len)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return ~0U;
    }

    return *data_size;
}

static int __cdecl compare_raw_input_devices(const void *ap, const void *bp)
{
    const RAWINPUTDEVICE a = *(const RAWINPUTDEVICE *)ap;
    const RAWINPUTDEVICE b = *(const RAWINPUTDEVICE *)bp;

    if (a.usUsagePage != b.usUsagePage) return a.usUsagePage - b.usUsagePage;
    if (a.usUsage != b.usUsage) return a.usUsage - b.usUsage;
    return 0;
}

/***********************************************************************
 *              GetRegisteredRawInputDevices   (USER32.@)
 */
UINT WINAPI DECLSPEC_HOTPATCH GetRegisteredRawInputDevices(RAWINPUTDEVICE *devices, UINT *device_count, UINT size)
{
    struct rawinput_device *buffer = NULL;
    unsigned int i, status, count = ~0U, buffer_size;

    TRACE("devices %p, device_count %p, size %u\n", devices, device_count, size);

    if (size != sizeof(RAWINPUTDEVICE) || !device_count || (devices && !*device_count))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return ~0U;
    }

    buffer_size = *device_count * sizeof(*buffer);
    if (devices && !(buffer = HeapAlloc(GetProcessHeap(), 0, buffer_size)))
        return ~0U;

    SERVER_START_REQ(get_rawinput_devices)
    {
        if (buffer) wine_server_set_reply(req, buffer, buffer_size);
        status = wine_server_call_err(req);
        *device_count = reply->device_count;
    }
    SERVER_END_REQ;

    if (buffer && !status)
    {
        for (i = 0, count = *device_count; i < count; ++i)
        {
            devices[i].usUsagePage = buffer[i].usage_page;
            devices[i].usUsage = buffer[i].usage;
            devices[i].dwFlags = buffer[i].flags;
            devices[i].hwndTarget = wine_server_ptr_handle(buffer[i].target);
        }

        qsort(devices, count, sizeof(*devices), compare_raw_input_devices);
    }

    if (buffer) HeapFree(GetProcessHeap(), 0, buffer);
    else count = 0;
    return count;
}


/***********************************************************************
 *              DefRawInputProc   (USER32.@)
 */
LRESULT WINAPI DefRawInputProc(RAWINPUT **data, INT data_count, UINT header_size)
{
    FIXME("data %p, data_count %d, header_size %u stub!\n", data, data_count, header_size);

    return 0;
}
