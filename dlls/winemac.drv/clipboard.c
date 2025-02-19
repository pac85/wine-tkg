/*
 * Mac clipboard driver
 *
 * Copyright 1994 Martin Ayotte
 *           1996 Alex Korobka
 *           1999 Noel Borthwick
 *           2003 Ulrich Czekalla for CodeWeavers
 * Copyright 2011, 2012, 2013 Ken Thomases for CodeWeavers Inc.
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

#include "config.h"

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "macdrv.h"
#include "winuser.h"
#include "shellapi.h"
#include "shlobj.h"
#include "wine/list.h"
#include "wine/server.h"
#include "wine/unicode.h"


WINE_DEFAULT_DEBUG_CHANNEL(clipboard);


/**************************************************************************
 *              Types
 **************************************************************************/

typedef void *(*DRVIMPORTFUNC)(CFDataRef data, size_t *ret_size);
typedef CFDataRef (*DRVEXPORTFUNC)(void *data, size_t size);

typedef struct _WINE_CLIPFORMAT
{
    struct list             entry;
    UINT                    format_id;
    CFStringRef             type;
    DRVIMPORTFUNC           import_func;
    DRVEXPORTFUNC           export_func;
    BOOL                    synthesized;
    struct _WINE_CLIPFORMAT *natural_format;
} WINE_CLIPFORMAT;


/**************************************************************************
 *              Constants
 **************************************************************************/

#define CLIPBOARD_UPDATE_DELAY 2000   /* delay between checks of the Mac pasteboard */


/**************************************************************************
 *              Forward Function Declarations
 **************************************************************************/

static void *import_clipboard_data(CFDataRef data, size_t *ret_size);
static void *import_bmp_to_dib(CFDataRef data, size_t *ret_size);
static void *import_html(CFDataRef data, size_t *ret_size);
static void *import_nsfilenames_to_hdrop(CFDataRef data, size_t *ret_size);
static void *import_utf8_to_unicodetext(CFDataRef data, size_t *ret_size);
static void *import_utf16_to_unicodetext(CFDataRef data, size_t *ret_size);

static CFDataRef export_clipboard_data(void *data, size_t size);
static CFDataRef export_dib_to_bmp(void *data, size_t size);
static CFDataRef export_hdrop_to_filenames(void *data, size_t size);
static CFDataRef export_html(void *data, size_t size);
static CFDataRef export_unicodetext_to_utf8(void *data, size_t size);
static CFDataRef export_unicodetext_to_utf16(void *data, size_t size);


/**************************************************************************
 *              Static Variables
 **************************************************************************/

/* Clipboard formats */
static struct list format_list = LIST_INIT(format_list);

/*  There are two naming schemes involved and we want to have a mapping between
    them.  There are Win32 clipboard format names and there are Mac pasteboard
    types.

    The Win32 standard clipboard formats don't have names, but they are associated
    with Mac pasteboard types through the following tables, which are used to
    initialize the format_list.  Where possible, the standard clipboard formats
    are mapped to predefined pasteboard type UTIs.  Otherwise, we create Wine-
    specific types of the form "org.winehq.builtin.<format>", where <format> is
    the name of the symbolic constant for the format minus "CF_" and lowercased.
    E.g. CF_BITMAP -> org.winehq.builtin.bitmap.

    Win32 clipboard formats which originate in a Windows program may be registered
    with an arbitrary name.  We construct a Mac pasteboard type from these by
    prepending "org.winehq.registered." to the registered name.

    Likewise, Mac pasteboard types which originate in other apps may have
    arbitrary type strings.  We ignore these.

    Summary:
    Win32 clipboard format names:
        <none>                              standard clipboard format; maps via
                                            format_list to either a predefined Mac UTI
                                            or org.winehq.builtin.<format>.
        <other>                             name registered within Win32 land; maps to
                                            org.winehq.registered.<other>
    Mac pasteboard type names:
        org.winehq.builtin.<format ID>      representation of Win32 standard clipboard
                                            format for which there was no corresponding
                                            predefined Mac UTI; maps via format_list
        org.winehq.registered.<format name> representation of Win32 registered
                                            clipboard format name; maps to <format name>
        <other>                             Mac pasteboard type originating with system
                                            or other apps; either maps via format_list
                                            to a standard clipboard format or ignored
*/

static const struct
{
    UINT          id;
    CFStringRef   type;
    DRVIMPORTFUNC import;
    DRVEXPORTFUNC export;
    BOOL          synthesized;
} builtin_format_ids[] =
{
    { CF_DIBV5,             CFSTR("org.winehq.builtin.dibv5"),              import_clipboard_data,          export_clipboard_data,      FALSE },
    { CF_DIF,               CFSTR("org.winehq.builtin.dif"),                import_clipboard_data,          export_clipboard_data,      FALSE },
    { CF_ENHMETAFILE,       CFSTR("org.winehq.builtin.enhmetafile"),        import_clipboard_data,          export_clipboard_data,      FALSE },
    { CF_LOCALE,            CFSTR("org.winehq.builtin.locale"),             import_clipboard_data,          export_clipboard_data,      FALSE },
    { CF_OEMTEXT,           CFSTR("org.winehq.builtin.oemtext"),            import_clipboard_data,          export_clipboard_data,      FALSE },
    { CF_PALETTE,           CFSTR("org.winehq.builtin.palette"),            import_clipboard_data,          export_clipboard_data,      FALSE },
    { CF_PENDATA,           CFSTR("org.winehq.builtin.pendata"),            import_clipboard_data,          export_clipboard_data,      FALSE },
    { CF_RIFF,              CFSTR("org.winehq.builtin.riff"),               import_clipboard_data,          export_clipboard_data,      FALSE },
    { CF_SYLK,              CFSTR("org.winehq.builtin.sylk"),               import_clipboard_data,          export_clipboard_data,      FALSE },
    { CF_TEXT,              CFSTR("org.winehq.builtin.text"),               import_clipboard_data,          export_clipboard_data,      FALSE },
    { CF_TIFF,              CFSTR("public.tiff"),                           import_clipboard_data,          export_clipboard_data,      FALSE },
    { CF_WAVE,              CFSTR("com.microsoft.waveform-audio"),          import_clipboard_data,          export_clipboard_data,      FALSE },

    { CF_DIB,               CFSTR("org.winehq.builtin.dib"),                import_clipboard_data,          export_clipboard_data,      FALSE },
    { CF_DIB,               CFSTR("com.microsoft.bmp"),                     import_bmp_to_dib,              export_dib_to_bmp,          TRUE },

    { CF_HDROP,             CFSTR("org.winehq.builtin.hdrop"),              import_clipboard_data,          export_clipboard_data,      FALSE },
    { CF_HDROP,             CFSTR("NSFilenamesPboardType"),                 import_nsfilenames_to_hdrop,    export_hdrop_to_filenames,  TRUE },

    { CF_UNICODETEXT,       CFSTR("org.winehq.builtin.unicodetext"),        import_clipboard_data,          export_clipboard_data,      FALSE },
    { CF_UNICODETEXT,       CFSTR("public.utf16-plain-text"),               import_utf16_to_unicodetext,    export_unicodetext_to_utf16,TRUE },
    { CF_UNICODETEXT,       CFSTR("public.utf8-plain-text"),                import_utf8_to_unicodetext,     export_unicodetext_to_utf8, TRUE },
};

static const WCHAR wszRichTextFormat[] = {'R','i','c','h',' ','T','e','x','t',' ','F','o','r','m','a','t',0};
static const WCHAR wszGIF[] = {'G','I','F',0};
static const WCHAR wszJFIF[] = {'J','F','I','F',0};
static const WCHAR wszPNG[] = {'P','N','G',0};
static const WCHAR wszHTMLFormat[] = {'H','T','M','L',' ','F','o','r','m','a','t',0};
static const struct
{
    LPCWSTR       name;
    CFStringRef   type;
    DRVIMPORTFUNC import;
    DRVEXPORTFUNC export;
    BOOL          synthesized;
} builtin_format_names[] =
{
    { wszRichTextFormat,    CFSTR("public.rtf"),                            import_clipboard_data,          export_clipboard_data },
    { wszGIF,               CFSTR("com.compuserve.gif"),                    import_clipboard_data,          export_clipboard_data },
    { wszJFIF,              CFSTR("public.jpeg"),                           import_clipboard_data,          export_clipboard_data },
    { wszPNG,               CFSTR("public.png"),                            import_clipboard_data,          export_clipboard_data },
    { wszHTMLFormat,        NULL,                                           import_clipboard_data,          export_clipboard_data },
    { wszHTMLFormat,        CFSTR("public.html"),                           import_html,                    export_html,            TRUE },
    { CFSTR_INETURLW,       CFSTR("public.url"),                            import_utf8_to_unicodetext,     export_unicodetext_to_utf8 },
};

/* The prefix prepended to a Win32 clipboard format name to make a Mac pasteboard type. */
static const CFStringRef registered_name_type_prefix = CFSTR("org.winehq.registered.");

static DWORD clipboard_thread_id;
static HWND clipboard_hwnd;
static BOOL is_clipboard_owner;
static macdrv_window clipboard_cocoa_window;
static ULONG64 last_clipboard_update;
static DWORD last_get_seqno;
static WINE_CLIPFORMAT **current_mac_formats;
static unsigned int nb_current_mac_formats;
static WCHAR clipboard_pipe_name[256];


/**************************************************************************
 *              Internal Clipboard implementation methods
 **************************************************************************/

/*
 * format_list functions
 */

/**************************************************************************
 *              debugstr_format
 */
const char *debugstr_format(UINT id)
{
    WCHAR buffer[256];

    if (NtUserGetClipboardFormatName(id, buffer, 256))
        return wine_dbg_sprintf("0x%04x %s", id, debugstr_w(buffer));

    switch (id)
    {
#define BUILTIN(id) case id: return #id;
    BUILTIN(CF_TEXT)
    BUILTIN(CF_BITMAP)
    BUILTIN(CF_METAFILEPICT)
    BUILTIN(CF_SYLK)
    BUILTIN(CF_DIF)
    BUILTIN(CF_TIFF)
    BUILTIN(CF_OEMTEXT)
    BUILTIN(CF_DIB)
    BUILTIN(CF_PALETTE)
    BUILTIN(CF_PENDATA)
    BUILTIN(CF_RIFF)
    BUILTIN(CF_WAVE)
    BUILTIN(CF_UNICODETEXT)
    BUILTIN(CF_ENHMETAFILE)
    BUILTIN(CF_HDROP)
    BUILTIN(CF_LOCALE)
    BUILTIN(CF_DIBV5)
    BUILTIN(CF_OWNERDISPLAY)
    BUILTIN(CF_DSPTEXT)
    BUILTIN(CF_DSPBITMAP)
    BUILTIN(CF_DSPMETAFILEPICT)
    BUILTIN(CF_DSPENHMETAFILE)
#undef BUILTIN
    default: return wine_dbg_sprintf("0x%04x", id);
    }
}


/**************************************************************************
 *              insert_clipboard_format
 */
static WINE_CLIPFORMAT *insert_clipboard_format(UINT id, CFStringRef type)
{
    WINE_CLIPFORMAT *format;

    format = malloc(sizeof(*format));

    if (format == NULL)
    {
        WARN("No more memory for a new format!\n");
        return NULL;
    }
    format->format_id = id;
    format->import_func = import_clipboard_data;
    format->export_func = export_clipboard_data;
    format->synthesized = FALSE;
    format->natural_format = NULL;

    if (type)
        format->type = CFStringCreateCopy(NULL, type);
    else
    {
        WCHAR buffer[256];

        if (!NtUserGetClipboardFormatName(format->format_id, buffer, ARRAY_SIZE(buffer)))
        {
            WARN("failed to get name for format %s; error 0x%08x\n", debugstr_format(format->format_id), GetLastError());
            free(format);
            return NULL;
        }

        format->type = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@%S"),
                                                registered_name_type_prefix, (const WCHAR*)buffer);
    }

    list_add_tail(&format_list, &format->entry);

    TRACE("Registering format %s type %s\n", debugstr_format(format->format_id),
          debugstr_cf(format->type));

    return format;
}


/**************************************************************************
 *              register_format
 *
 * Register a custom Mac clipboard format.
 */
static WINE_CLIPFORMAT* register_format(UINT id, CFStringRef type)
{
    WINE_CLIPFORMAT *format;

    /* walk format chain to see if it's already registered */
    LIST_FOR_EACH_ENTRY(format, &format_list, WINE_CLIPFORMAT, entry)
        if (format->format_id == id) return format;

    return insert_clipboard_format(id, type);
}


/**************************************************************************
 *              natural_format_for_format
 *
 * Find the "natural" format for this format_id (the one which isn't
 * synthesized from another type).
 */
static WINE_CLIPFORMAT* natural_format_for_format(UINT format_id)
{
    WINE_CLIPFORMAT *format;

    LIST_FOR_EACH_ENTRY(format, &format_list, WINE_CLIPFORMAT, entry)
        if (format->format_id == format_id && !format->synthesized) break;

    if (&format->entry == &format_list)
        format = NULL;

    TRACE("%s -> %p/%s\n", debugstr_format(format_id), format, debugstr_cf(format ? format->type : NULL));
    return format;
}


static ATOM register_clipboard_format(const WCHAR *name)
{
    ATOM atom;
    if (NtAddAtom(name, lstrlenW(name) * sizeof(WCHAR), &atom)) return 0;
    return atom;
}


/**************************************************************************
 *              register_builtin_formats
 */
static void register_builtin_formats(void)
{
    UINT i;
    WINE_CLIPFORMAT *format;

    /* Register built-in formats */
    for (i = 0; i < ARRAY_SIZE(builtin_format_ids); i++)
    {
        if (!(format = malloc(sizeof(*format)))) break;
        format->format_id       = builtin_format_ids[i].id;
        format->type            = CFRetain(builtin_format_ids[i].type);
        format->import_func     = builtin_format_ids[i].import;
        format->export_func     = builtin_format_ids[i].export;
        format->synthesized     = builtin_format_ids[i].synthesized;
        format->natural_format  = NULL;
        list_add_tail(&format_list, &format->entry);
    }

    /* Register known mappings between Windows formats and Mac types */
    for (i = 0; i < ARRAY_SIZE(builtin_format_names); i++)
    {
        if (!(format = malloc(sizeof(*format)))) break;
        format->format_id       = register_clipboard_format(builtin_format_names[i].name);
        format->import_func     = builtin_format_names[i].import;
        format->export_func     = builtin_format_names[i].export;
        format->synthesized     = builtin_format_names[i].synthesized;
        format->natural_format  = NULL;

        if (builtin_format_names[i].type)
            format->type = CFRetain(builtin_format_names[i].type);
        else
        {
            format->type = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@%S"),
                                                    registered_name_type_prefix, builtin_format_names[i].name);
        }

        list_add_tail(&format_list, &format->entry);
    }

    LIST_FOR_EACH_ENTRY(format, &format_list, WINE_CLIPFORMAT, entry)
    {
        if (format->synthesized)
            format->natural_format = natural_format_for_format(format->format_id);
    }
}


/**************************************************************************
 *              format_for_type
 */
static WINE_CLIPFORMAT* format_for_type(CFStringRef type)
{
    WINE_CLIPFORMAT *format;

    TRACE("type %s\n", debugstr_cf(type));

    if (list_empty(&format_list)) register_builtin_formats();

    LIST_FOR_EACH_ENTRY(format, &format_list, WINE_CLIPFORMAT, entry)
    {
        if (CFEqual(format->type, type))
            goto done;
    }

    format = NULL;
    if (CFStringHasPrefix(type, CFSTR("org.winehq.builtin.")))
    {
        ERR("Shouldn't happen. Built-in type %s should have matched something in format list.\n",
            debugstr_cf(type));
    }
    else if (CFStringHasPrefix(type, registered_name_type_prefix))
    {
        LPWSTR name;
        int len = CFStringGetLength(type) - CFStringGetLength(registered_name_type_prefix);

        name = malloc((len + 1) * sizeof(WCHAR));
        CFStringGetCharacters(type, CFRangeMake(CFStringGetLength(registered_name_type_prefix), len),
                              (UniChar*)name);
        name[len] = 0;

        format = register_format(register_clipboard_format(name), type);
        if (!format)
            ERR("Failed to register format for type %s name %s\n", debugstr_cf(type), debugstr_w(name));

        free(name);
    }

done:
    TRACE(" -> %p/%s\n", format, debugstr_format(format ? format->format_id : 0));
    return format;
}


/***********************************************************************
 *              bitmap_info_size
 *
 * Return the size of the bitmap info structure including color table.
 */
static int bitmap_info_size(const BITMAPINFO *info, WORD coloruse)
{
    unsigned int colors, size, masks = 0;

    if (info->bmiHeader.biSize == sizeof(BITMAPCOREHEADER))
    {
        const BITMAPCOREHEADER *core = (const BITMAPCOREHEADER*)info;
        colors = (core->bcBitCount <= 8) ? 1 << core->bcBitCount : 0;
        return sizeof(BITMAPCOREHEADER) + colors *
             ((coloruse == DIB_RGB_COLORS) ? sizeof(RGBTRIPLE) : sizeof(WORD));
    }
    else  /* assume BITMAPINFOHEADER */
    {
        colors = min(info->bmiHeader.biClrUsed, 256);
        if (!colors && (info->bmiHeader.biBitCount <= 8))
            colors = 1 << info->bmiHeader.biBitCount;
        if (info->bmiHeader.biCompression == BI_BITFIELDS) masks = 3;
        size = max(info->bmiHeader.biSize, sizeof(BITMAPINFOHEADER) + masks * sizeof(DWORD));
        return size + colors * ((coloruse == DIB_RGB_COLORS) ? sizeof(RGBQUAD) : sizeof(WORD));
    }
}


/**************************************************************************
 *		get_html_description_field
 *
 *  Find the value of a field in an HTML Format description.
 */
static const char* get_html_description_field(const char* data, const char* keyword)
{
    const char* pos = data;

    while (pos && *pos && *pos != '<')
    {
        if (memcmp(pos, keyword, strlen(keyword)) == 0)
            return pos + strlen(keyword);

        pos = strchr(pos, '\n');
        if (pos) pos++;
    }

    return NULL;
}


/**************************************************************************
 *              import_clipboard_data
 *
 *  Generic import clipboard data routine.
 */
static void *import_clipboard_data(CFDataRef data, size_t *ret_size)
{
    void *ret = NULL;

    size_t len = CFDataGetLength(data);
    if (len && (ret = malloc(len)))
    {
        memcpy(ret, CFDataGetBytePtr(data), len);
        *ret_size = len;
    }

    return ret;
}


/**************************************************************************
 *              import_bmp_to_dib
 *
 *  Import BMP data, converting to CF_DIB or CF_DIBV5 format.  This just
 *  entails stripping the BMP file format header.
 */
static void *import_bmp_to_dib(CFDataRef data, size_t *ret_size)
{
    BITMAPFILEHEADER *bfh = (BITMAPFILEHEADER*)CFDataGetBytePtr(data);
    CFIndex len = CFDataGetLength(data);
    void *ret = NULL;

    if (len >= sizeof(*bfh) + sizeof(BITMAPCOREHEADER) &&
        bfh->bfType == 0x4d42 /* "BM" */)
    {
        BITMAPINFO *bmi = (BITMAPINFO*)(bfh + 1);

        len -= sizeof(*bfh);
        if ((ret = malloc(len)))
        {
            memcpy(ret, bmi, len);
            *ret_size = len;
        }
    }

    return ret;
}


/**************************************************************************
 *              import_html
 *
 *  Import HTML data.
 */
static void *import_html(CFDataRef data, size_t *ret_size)
{
    static const char header[] =
        "Version:0.9\n"
        "StartHTML:0000000100\n"
        "EndHTML:%010lu\n"
        "StartFragment:%010lu\n"
        "EndFragment:%010lu\n"
        "<!--StartFragment-->";
    static const char trailer[] = "\n<!--EndFragment-->";
    void *ret;
    SIZE_T len, total;
    size_t size = CFDataGetLength(data);

    len = strlen(header) + 12;  /* 3 * 4 extra chars for %010lu */
    total = len + size + sizeof(trailer);
    if ((ret = malloc(total)))
    {
        char *p = ret;
        p += sprintf(p, header, total - 1, len, len + size + 1 /* include the final \n in the data */);
        CFDataGetBytes(data, CFRangeMake(0, size), (UInt8*)p);
        strcpy(p + size, trailer);
        *ret_size = total;
        TRACE("returning %s\n", debugstr_a(ret));
    }
    return ret;
}


/* based on wine_get_dos_file_name */
static WCHAR *get_dos_file_name(const char *path)
{
    ULONG len = strlen(path) + 9; /* \??\unix prefix */
    WCHAR *ret;

    if (!(ret = malloc(len * sizeof(WCHAR)))) return NULL;
    if (wine_unix_to_nt_file_name(path, ret, &len))
    {
        free(ret);
        return NULL;
    }

    if (ret[5] == ':')
    {
        /* get rid of the \??\ prefix */
        memmove(ret, ret + 4, (len - 4) * sizeof(WCHAR));
    }
    else ret[1] = '\\';
    return ret;
}


/***********************************************************************
 *           get_nt_pathname
 *
 * Simplified version of RtlDosPathNameToNtPathName_U.
 */
static BOOL get_nt_pathname(const WCHAR *name, UNICODE_STRING *nt_name)
{
    static const WCHAR ntprefixW[] = {'\\','?','?','\\'};
    static const WCHAR uncprefixW[] = {'U','N','C','\\'};
    size_t len = lstrlenW(name);
    WCHAR *ptr;

    nt_name->MaximumLength = (len + 8) * sizeof(WCHAR);
    if (!(ptr = malloc(nt_name->MaximumLength))) return FALSE;
    nt_name->Buffer = ptr;

    memcpy(ptr, ntprefixW, sizeof(ntprefixW));
    ptr += ARRAYSIZE(ntprefixW);
    if (name[0] == '\\' && name[1] == '\\')
    {
        if ((name[2] == '.' || name[2] == '?') && name[3] == '\\')
        {
            name += 4;
            len -= 4;
        }
        else
        {
            memcpy(ptr, uncprefixW, sizeof(uncprefixW));
            ptr += ARRAYSIZE(uncprefixW);
            name += 2;
            len -= 2;
        }
    }
    memcpy(ptr, name, (len + 1) * sizeof(WCHAR));
    ptr += len;
    nt_name->Length = (ptr - nt_name->Buffer) * sizeof(WCHAR);
    return TRUE;
}


/* based on wine_get_unix_file_name */
static char *get_unix_file_name(const WCHAR *dosW)
{
    UNICODE_STRING nt_name;
    OBJECT_ATTRIBUTES attr;
    NTSTATUS status;
    ULONG size = 256;
    char *buffer;

    if (!get_nt_pathname(dosW, &nt_name)) return NULL;
    InitializeObjectAttributes(&attr, &nt_name, 0, 0, NULL);
    for (;;)
    {
        if (!(buffer = malloc(size)))
        {
            free(nt_name.Buffer);
            return NULL;
        }
        status = wine_nt_to_unix_file_name(&attr, buffer, &size, FILE_OPEN_IF);
        if (status != STATUS_BUFFER_TOO_SMALL) break;
        free(buffer);
    }
    free(nt_name.Buffer);
    if (status)
    {
        free(buffer);
        return NULL;
    }
    return buffer;
}


/**************************************************************************
 *              import_nsfilenames_to_hdrop
 *
 *  Import NSFilenamesPboardType data, converting the property-list-
 *  serialized array of path strings to CF_HDROP.
 */
static void *import_nsfilenames_to_hdrop(CFDataRef data, size_t *ret_size)
{
    CFArrayRef names;
    CFIndex count, i;
    size_t len;
    char *buffer = NULL;
    WCHAR **paths = NULL;
    DROPFILES *dropfiles = NULL;
    UniChar* p;

    TRACE("data %s\n", debugstr_cf(data));

    names = (CFArrayRef)CFPropertyListCreateWithData(NULL, data, kCFPropertyListImmutable,
                                                     NULL, NULL);
    if (!names || CFGetTypeID(names) != CFArrayGetTypeID())
    {
        WARN("failed to interpret data as a CFArray\n");
        goto done;
    }

    count = CFArrayGetCount(names);

    len = 0;
    for (i = 0; i < count; i++)
    {
        CFIndex this_len;
        CFStringRef name = (CFStringRef)CFArrayGetValueAtIndex(names, i);
        TRACE("    %s\n", debugstr_cf(name));
        if (CFGetTypeID(name) != CFStringGetTypeID())
        {
            WARN("non-string in array\n");
            goto done;
        }

        this_len = CFStringGetMaximumSizeOfFileSystemRepresentation(name);
        if (this_len > len)
            len = this_len;
    }

    buffer = malloc(len);
    if (!buffer)
    {
        WARN("failed to allocate buffer for file-system representations\n");
        goto done;
    }

    paths = calloc(count, sizeof(paths[0]));
    if (!paths)
    {
        WARN("failed to allocate array of DOS paths\n");
        goto done;
    }

    for (i = 0; i < count; i++)
    {
        CFStringRef name = (CFStringRef)CFArrayGetValueAtIndex(names, i);
        if (!CFStringGetFileSystemRepresentation(name, buffer, len))
        {
            WARN("failed to get file-system representation for %s\n", debugstr_cf(name));
            goto done;
        }
        paths[i] = get_dos_file_name(buffer);
        if (!paths[i])
        {
            WARN("failed to get DOS path for %s\n", debugstr_a(buffer));
            goto done;
        }
    }

    len = 1; /* for the terminating null */
    for (i = 0; i < count; i++)
        len += strlenW(paths[i]) + 1;

    *ret_size = sizeof(*dropfiles) + len * sizeof(WCHAR);
    if (!(dropfiles = malloc(*ret_size)))
    {
        WARN("failed to allocate HDROP\n");
        goto done;
    }

    dropfiles->pFiles   = sizeof(*dropfiles);
    dropfiles->pt.x     = 0;
    dropfiles->pt.y     = 0;
    dropfiles->fNC      = FALSE;
    dropfiles->fWide    = TRUE;

    p = (WCHAR*)(dropfiles + 1);
    for (i = 0; i < count; i++)
    {
        strcpyW(p, paths[i]);
        p += strlenW(p) + 1;
    }
    *p = 0;

done:
    if (paths)
    {
        for (i = 0; i < count; i++) free(paths[i]);
        free(paths);
    }
    free(buffer);
    if (names) CFRelease(names);
    return dropfiles;
}


/**************************************************************************
 *              import_utf8_to_unicodetext
 *
 *  Import a UTF-8 string, converting the string to CF_UNICODETEXT.
 */
static void *import_utf8_to_unicodetext(CFDataRef data, size_t *ret_size)
{
    const BYTE *src;
    unsigned long src_len;
    unsigned long new_lines = 0;
    LPSTR dst;
    unsigned long i, j;
    WCHAR *ret = NULL;

    src = CFDataGetBytePtr(data);
    src_len = CFDataGetLength(data);
    for (i = 0; i < src_len; i++)
    {
        if (src[i] == '\n')
            new_lines++;
    }

    if ((dst = malloc(src_len + new_lines + 1)))
    {
        for (i = 0, j = 0; i < src_len; i++)
        {
            if (src[i] == '\n')
                dst[j++] = '\r';

            dst[j++] = src[i];
        }
        dst[j++] = 0;

        if ((ret = malloc(j * sizeof(WCHAR))))
            *ret_size = MultiByteToWideChar(CP_UTF8, 0, dst, j, ret, j) * sizeof(WCHAR);

        free(dst);
    }

    return ret;
}


/**************************************************************************
 *              import_utf16_to_unicodetext
 *
 *  Import a UTF-8 string, converting the string to CF_UNICODETEXT.
 */
static void *import_utf16_to_unicodetext(CFDataRef data, size_t *ret_size)
{
    const WCHAR *src;
    unsigned long src_len;
    unsigned long new_lines = 0;
    LPWSTR dst;
    unsigned long i, j;

    src = (const WCHAR *)CFDataGetBytePtr(data);
    src_len = CFDataGetLength(data) / sizeof(WCHAR);
    for (i = 0; i < src_len; i++)
    {
        if (src[i] == '\n')
            new_lines++;
        else if (src[i] == '\r' && (i + 1 >= src_len || src[i + 1] != '\n'))
            new_lines++;
    }

    *ret_size = (src_len + new_lines + 1) * sizeof(WCHAR);
    if ((dst = malloc(*ret_size)))
    {
        for (i = 0, j = 0; i < src_len; i++)
        {
            if (src[i] == '\n')
                dst[j++] = '\r';

            dst[j++] = src[i];

            if (src[i] == '\r' && (i + 1 >= src_len || src[i + 1] != '\n'))
                dst[j++] = '\n';
        }
        dst[j] = 0;
    }

    return dst;
}


/**************************************************************************
 *              export_clipboard_data
 *
 *  Generic export clipboard data routine.
 */
static CFDataRef export_clipboard_data(void *data, size_t size)
{
    return CFDataCreate(NULL, data, size);
}


/**************************************************************************
 *              export_dib_to_bmp
 *
 *  Export CF_DIB or CF_DIBV5 to BMP file format.  This just entails
 *  prepending a BMP file format header to the data.
 */
static CFDataRef export_dib_to_bmp(void *data, size_t size)
{
    CFMutableDataRef ret = NULL;
    CFIndex len;
    BITMAPFILEHEADER bfh;

    len = sizeof(bfh) + size;
    ret = CFDataCreateMutable(NULL, len);
    if (ret)
    {
        bfh.bfType = 0x4d42; /* "BM" */
        bfh.bfSize = len;
        bfh.bfReserved1 = 0;
        bfh.bfReserved2 = 0;
        bfh.bfOffBits = sizeof(bfh) + bitmap_info_size(data, DIB_RGB_COLORS);
        CFDataAppendBytes(ret, (UInt8*)&bfh, sizeof(bfh));

        /* rest of bitmap is the same as the packed dib */
        CFDataAppendBytes(ret, data, size);
    }

    return ret;
}


/**************************************************************************
 *              export_hdrop_to_filenames
 *
 *  Export CF_HDROP to NSFilenamesPboardType data, which is a CFArray of
 *  CFStrings (holding Unix paths) which is serialized as a property list.
 */
static CFDataRef export_hdrop_to_filenames(void *data, size_t size)
{
    CFDataRef ret = NULL;
    DROPFILES *dropfiles = data;
    CFMutableArrayRef filenames = NULL;
    void *p;
    WCHAR *buffer = NULL;
    size_t buffer_len = 0;

    TRACE("data %p\n", data);

    filenames = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    if (!filenames)
    {
        WARN("failed to create filenames array\n");
        goto done;
    }

    p = (char*)dropfiles + dropfiles->pFiles;
    while (dropfiles->fWide ? *(WCHAR*)p : *(char*)p)
    {
        char *unixname;
        CFStringRef filename;

        TRACE("    %s\n", dropfiles->fWide ? debugstr_w(p) : debugstr_a(p));

        if (dropfiles->fWide)
            unixname = get_unix_file_name(p);
        else
        {
            int len = MultiByteToWideChar(CP_ACP, 0, p, -1, NULL, 0);
            if (len)
            {
                if (len > buffer_len)
                {
                    free(buffer);
                    buffer_len = len * 2;
                    buffer = malloc(buffer_len * sizeof(*buffer));
                }

                MultiByteToWideChar(CP_ACP, 0, p, -1, buffer, buffer_len);
                unixname = get_unix_file_name(buffer);
            }
            else
                unixname = NULL;
        }
        if (!unixname)
        {
            WARN("failed to convert DOS path to Unix: %s\n",
                 dropfiles->fWide ? debugstr_w(p) : debugstr_a(p));
            goto done;
        }

        if (dropfiles->fWide)
            p = (WCHAR*)p + strlenW(p) + 1;
        else
            p = (char*)p + strlen(p) + 1;

        filename = CFStringCreateWithFileSystemRepresentation(NULL, unixname);
        if (!filename)
        {
            WARN("failed to create CFString from Unix path %s\n", debugstr_a(unixname));
            free(unixname);
            goto done;
        }

        free(unixname);
        CFArrayAppendValue(filenames, filename);
        CFRelease(filename);
    }

    ret = CFPropertyListCreateData(NULL, filenames, kCFPropertyListXMLFormat_v1_0, 0, NULL);

done:
    free(buffer);
    if (filenames) CFRelease(filenames);
    TRACE(" -> %s\n", debugstr_cf(ret));
    return ret;
}


/**************************************************************************
 *              export_html
 *
 *  Export HTML Format to public.html data.
 *
 * FIXME: We should attempt to add an <a base> tag and convert windows paths.
 */
static CFDataRef export_html(void *data, size_t size)
{
    const char *field_value;
    int fragmentstart, fragmentend;

    /* read the important fields */
    field_value = get_html_description_field(data, "StartFragment:");
    if (!field_value)
    {
        ERR("Couldn't find StartFragment value\n");
        return NULL;
    }
    fragmentstart = atoi(field_value);

    field_value = get_html_description_field(data, "EndFragment:");
    if (!field_value)
    {
        ERR("Couldn't find EndFragment value\n");
        return NULL;
    }
    fragmentend = atoi(field_value);

    /* export only the fragment */
    return CFDataCreate(NULL, &((const UInt8*)data)[fragmentstart], fragmentend - fragmentstart);
}


/**************************************************************************
 *              export_unicodetext_to_utf8
 *
 *  Export CF_UNICODETEXT to UTF-8.
 */
static CFDataRef export_unicodetext_to_utf8(void *data, size_t size)
{
    CFMutableDataRef ret;
    INT dst_len;

    dst_len = WideCharToMultiByte(CP_UTF8, 0, data, -1, NULL, 0, NULL, NULL);
    if (dst_len) dst_len--; /* Leave off null terminator. */
    ret = CFDataCreateMutable(NULL, dst_len);
    if (ret)
    {
        LPSTR dst;
        int i, j;

        CFDataSetLength(ret, dst_len);
        dst = (LPSTR)CFDataGetMutableBytePtr(ret);
        WideCharToMultiByte(CP_UTF8, 0, data, -1, dst, dst_len, NULL, NULL);

        /* Remove carriage returns */
        for (i = 0, j = 0; i < dst_len; i++)
        {
            if (dst[i] == '\r' &&
                (i + 1 >= dst_len || dst[i + 1] == '\n' || dst[i + 1] == '\0'))
                continue;
            dst[j++] = dst[i];
        }
        CFDataSetLength(ret, j);
    }

    return ret;
}


/**************************************************************************
 *              export_unicodetext_to_utf16
 *
 *  Export CF_UNICODETEXT to UTF-16.
 */
static CFDataRef export_unicodetext_to_utf16(void *data, size_t size)
{
    CFMutableDataRef ret;
    const WCHAR *src = data;
    INT src_len;

    src_len = size / sizeof(WCHAR);
    if (src_len) src_len--; /* Leave off null terminator. */
    ret = CFDataCreateMutable(NULL, src_len * sizeof(WCHAR));
    if (ret)
    {
        LPWSTR dst;
        int i, j;

        CFDataSetLength(ret, src_len * sizeof(WCHAR));
        dst = (LPWSTR)CFDataGetMutableBytePtr(ret);

        /* Remove carriage returns */
        for (i = 0, j = 0; i < src_len; i++)
        {
            if (src[i] == '\r' &&
                (i + 1 >= src_len || src[i + 1] == '\n' || src[i + 1] == '\0'))
                continue;
            dst[j++] = src[i];
        }
        CFDataSetLength(ret, j * sizeof(WCHAR));
    }

    return ret;
}


/**************************************************************************
 *              macdrv_get_pasteboard_data
 */
HANDLE macdrv_get_pasteboard_data(CFTypeRef pasteboard, UINT desired_format)
{
    CFArrayRef types;
    CFIndex count;
    CFIndex i;
    CFStringRef type, best_type;
    WINE_CLIPFORMAT* best_format = NULL;
    HANDLE data = NULL;

    TRACE("pasteboard %p, desired_format %s\n", pasteboard, debugstr_format(desired_format));

    types = macdrv_copy_pasteboard_types(pasteboard);
    if (!types)
    {
        WARN("Failed to copy pasteboard types\n");
        return NULL;
    }

    count = CFArrayGetCount(types);
    TRACE("got %ld types\n", count);

    for (i = 0; (!best_format || best_format->synthesized) && i < count; i++)
    {
        WINE_CLIPFORMAT* format;

        type = CFArrayGetValueAtIndex(types, i);

        if ((format = format_for_type(type)))
        {
            TRACE("for type %s got format %p/%s\n", debugstr_cf(type), format, debugstr_format(format->format_id));

            if (format->format_id == desired_format)
            {
                /* The best format is the matching one which is not synthesized.  Failing that,
                   the best format is the first matching synthesized format. */
                if (!format->synthesized || !best_format)
                {
                    best_type = type;
                    best_format = format;
                }
            }
        }
    }

    if (best_format)
    {
        CFDataRef pasteboard_data = macdrv_copy_pasteboard_data(pasteboard, best_type);

        TRACE("got pasteboard data for type %s: %s\n", debugstr_cf(best_type), debugstr_cf(pasteboard_data));

        if (pasteboard_data)
        {
            size_t size;
            void *import = best_format->import_func(pasteboard_data, &size), *ptr;
            if (import)
            {
                data = GlobalAlloc(GMEM_FIXED, size);
                if (data && (ptr = GlobalLock(data)))
                {
                    memcpy(ptr, import, size);
                    GlobalUnlock(data);
                }
                free(import);
            }
            CFRelease(pasteboard_data);
        }
    }

    CFRelease(types);
    TRACE(" -> %p\n", data);
    return data;
}


/**************************************************************************
 *              macdrv_pasteboard_has_format
 */
BOOL macdrv_pasteboard_has_format(CFTypeRef pasteboard, UINT desired_format)
{
    CFArrayRef types;
    int count;
    UINT i;
    BOOL found = FALSE;

    TRACE("pasteboard %p, desired_format %s\n", pasteboard, debugstr_format(desired_format));

    types = macdrv_copy_pasteboard_types(pasteboard);
    if (!types)
    {
        WARN("Failed to copy pasteboard types\n");
        return FALSE;
    }

    count = CFArrayGetCount(types);
    TRACE("got %d types\n", count);

    for (i = 0; i < count; i++)
    {
        CFStringRef type = CFArrayGetValueAtIndex(types, i);
        WINE_CLIPFORMAT* format = format_for_type(type);

        if (format)
        {
            TRACE("for type %s got format %s\n", debugstr_cf(type), debugstr_format(format->format_id));

            if (format->format_id == desired_format)
            {
                found = TRUE;
                break;
            }
        }
    }

    CFRelease(types);
    TRACE(" -> %d\n", found);
    return found;
}


/**************************************************************************
 *              get_formats_for_pasteboard_types
 */
static WINE_CLIPFORMAT** get_formats_for_pasteboard_types(CFArrayRef types, UINT *num_formats)
{
    CFIndex count, i;
    CFMutableSetRef seen_formats;
    WINE_CLIPFORMAT** formats;
    UINT pos;

    count = CFArrayGetCount(types);
    TRACE("got %ld types\n", count);

    if (!count)
        return NULL;

    seen_formats = CFSetCreateMutable(NULL, count, NULL);
    if (!seen_formats)
    {
        WARN("Failed to allocate seen formats set\n");
        return NULL;
    }

    formats = malloc(count * sizeof(*formats));
    if (!formats)
    {
        WARN("Failed to allocate formats array\n");
        CFRelease(seen_formats);
        return NULL;
    }

    pos = 0;
    for (i = 0; i < count; i++)
    {
        CFStringRef type = CFArrayGetValueAtIndex(types, i);
        WINE_CLIPFORMAT* format = format_for_type(type);

        if (!format)
        {
            TRACE("ignoring type %s\n", debugstr_cf(type));
            continue;
        }

        if (!format->synthesized)
        {
            TRACE("for type %s got format %p/%s\n", debugstr_cf(type), format, debugstr_format(format->format_id));
            CFSetAddValue(seen_formats, ULongToPtr(format->format_id));
            formats[pos++] = format;
        }
        else if (format->natural_format &&
                 CFArrayContainsValue(types, CFRangeMake(0, count), format->natural_format->type))
        {
            TRACE("for type %s deferring synthesized formats because type %s is also present\n",
                  debugstr_cf(type), debugstr_cf(format->natural_format->type));
        }
        else if (CFSetContainsValue(seen_formats, ULongToPtr(format->format_id)))
        {
            TRACE("for type %s got duplicate synthesized format %p/%s; skipping\n", debugstr_cf(type), format,
                  debugstr_format(format->format_id));
        }
        else
        {
            TRACE("for type %s got synthesized format %p/%s\n", debugstr_cf(type), format, debugstr_format(format->format_id));
            CFSetAddValue(seen_formats, ULongToPtr(format->format_id));
            formats[pos++] = format;
        }
    }

    /* Now go back through the types adding the synthesized formats that we deferred before. */
    for (i = 0; i < count; i++)
    {
        CFStringRef type = CFArrayGetValueAtIndex(types, i);
        WINE_CLIPFORMAT* format = format_for_type(type);

        if (!format) continue;
        if (!format->synthesized) continue;

        /* Don't duplicate a real value with a synthesized value. */
        if (CFSetContainsValue(seen_formats, ULongToPtr(format->format_id))) continue;

        TRACE("for type %s got synthesized format %p/%s\n", debugstr_cf(type), format, debugstr_format(format->format_id));
        CFSetAddValue(seen_formats, ULongToPtr(format->format_id));
        formats[pos++] = format;
    }

    CFRelease(seen_formats);

    if (!pos)
    {
        free(formats);
        formats = NULL;
    }

    *num_formats = pos;
    return formats;
}


/**************************************************************************
 *              get_formats_for_pasteboard
 */
static WINE_CLIPFORMAT** get_formats_for_pasteboard(CFTypeRef pasteboard, UINT *num_formats)
{
    CFArrayRef types;
    WINE_CLIPFORMAT** formats;

    TRACE("pasteboard %s\n", debugstr_cf(pasteboard));

    types = macdrv_copy_pasteboard_types(pasteboard);
    if (!types)
    {
        WARN("Failed to copy pasteboard types\n");
        return NULL;
    }

    formats = get_formats_for_pasteboard_types(types, num_formats);
    CFRelease(types);
    return formats;
}


/**************************************************************************
 *              macdrv_get_pasteboard_formats
 */
UINT* macdrv_get_pasteboard_formats(CFTypeRef pasteboard, UINT* num_formats)
{
    WINE_CLIPFORMAT** formats;
    UINT count, i;
    UINT* format_ids;

    formats = get_formats_for_pasteboard(pasteboard, &count);
    if (!formats)
        return NULL;

    format_ids = malloc(count);
    if (!format_ids)
    {
        WARN("Failed to allocate formats IDs array\n");
        free(formats);
        return NULL;
    }

    for (i = 0; i < count; i++)
        format_ids[i] = formats[i]->format_id;

    free(formats);

    *num_formats = count;
    return format_ids;
}


/**************************************************************************
 *              register_win32_formats
 *
 * Register Win32 clipboard formats the first time we encounter them.
 */
static void register_win32_formats(const UINT *ids, UINT size)
{
    unsigned int i;

    if (list_empty(&format_list)) register_builtin_formats();

    for (i = 0; i < size; i++)
        register_format(ids[i], NULL);
}


/***********************************************************************
 *              get_clipboard_formats
 *
 * Return a list of all formats currently available on the Win32 clipboard.
 * Helper for set_mac_pasteboard_types_from_win32_clipboard.
 */
static UINT *get_clipboard_formats(UINT *size)
{
    UINT *ids;

    *size = 256;
    for (;;)
    {
        if (!(ids = malloc(*size * sizeof(*ids)))) return NULL;
        if (GetUpdatedClipboardFormats(ids, *size, size)) break;
        free(ids);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return NULL;
    }
    register_win32_formats(ids, *size);
    return ids;
}


/**************************************************************************
 *              set_mac_pasteboard_types_from_win32_clipboard
 */
static void set_mac_pasteboard_types_from_win32_clipboard(void)
{
    WINE_CLIPFORMAT *format;
    UINT count, i, *formats;

    if (!(formats = get_clipboard_formats(&count))) return;

    macdrv_clear_pasteboard(clipboard_cocoa_window);

    for (i = 0; i < count; i++)
    {
        LIST_FOR_EACH_ENTRY(format, &format_list, WINE_CLIPFORMAT, entry)
        {
            if (format->format_id != formats[i]) continue;
            TRACE("%s -> %s\n", debugstr_format(format->format_id), debugstr_cf(format->type));
            macdrv_set_pasteboard_data(format->type, NULL, clipboard_cocoa_window);
        }
    }

    free(formats);
    return;
}


/**************************************************************************
 *              set_win32_clipboard_formats_from_mac_pasteboard
 */
static void set_win32_clipboard_formats_from_mac_pasteboard(CFArrayRef types)
{
    WINE_CLIPFORMAT** formats;
    UINT count, i;

    formats = get_formats_for_pasteboard_types(types, &count);
    if (!formats)
        return;

    for (i = 0; i < count; i++)
    {
        TRACE("adding format %s\n", debugstr_format(formats[i]->format_id));
        SetClipboardData(formats[i]->format_id, 0);
    }

    free(current_mac_formats);
    current_mac_formats = formats;
    nb_current_mac_formats = count;
}


/**************************************************************************
 *              render_format
 */
static void render_format(UINT id)
{
    unsigned int i;

    for (i = 0; i < nb_current_mac_formats; i++)
    {
        CFDataRef pasteboard_data;

        if (current_mac_formats[i]->format_id != id) continue;

        pasteboard_data = macdrv_copy_pasteboard_data(NULL, current_mac_formats[i]->type);
        if (pasteboard_data)
        {
            struct set_clipboard_params params = { 0 };
            params.data = current_mac_formats[i]->import_func(pasteboard_data, &params.size);
            CFRelease(pasteboard_data);
            if (!params.data) continue;
            NtUserSetClipboardData(id, 0, &params);
            free(params.data);
            break;
        }
    }
}


/**************************************************************************
 *              grab_win32_clipboard
 *
 * Grab the Win32 clipboard when a Mac app has taken ownership of the
 * pasteboard, and fill it with the pasteboard data types.
 */
static void grab_win32_clipboard(void)
{
    static CFArrayRef last_types;
    CFArrayRef types;

    types = macdrv_copy_pasteboard_types(NULL);
    if (!types)
    {
        WARN("Failed to copy pasteboard types\n");
        return;
    }

    if (!macdrv_has_pasteboard_changed() && last_types && CFEqual(types, last_types))
    {
        CFRelease(types);
        return;
    }

    if (last_types) CFRelease(last_types);
    last_types = types; /* takes ownership */

    if (!NtUserOpenClipboard(clipboard_hwnd, 0)) return;
    NtUserEmptyClipboard();
    is_clipboard_owner = TRUE;
    last_clipboard_update = GetTickCount64();
    set_win32_clipboard_formats_from_mac_pasteboard(types);
    NtUserCloseClipboard();
    NtUserSetTimer(clipboard_hwnd, 1, CLIPBOARD_UPDATE_DELAY, NULL, TIMERV_DEFAULT_COALESCING);
}


/**************************************************************************
 *              update_clipboard
 *
 * Periodically update the clipboard while the clipboard is owned by a
 * Mac app.
 */
static void update_clipboard(void)
{
    static BOOL updating;

    TRACE("is_clipboard_owner %d last_clipboard_update %llu now %llu\n",
          is_clipboard_owner, (unsigned long long)last_clipboard_update, (unsigned long long)GetTickCount64());

    if (updating) return;
    updating = TRUE;

    if (is_clipboard_owner)
    {
        if (GetTickCount64() - last_clipboard_update > CLIPBOARD_UPDATE_DELAY)
            grab_win32_clipboard();
    }
    else if (!macdrv_is_pasteboard_owner(clipboard_cocoa_window))
        grab_win32_clipboard();

    updating = FALSE;
}


/**************************************************************************
 *              clipboard_wndproc
 *
 * Window procedure for the clipboard manager.
 */
static LRESULT CALLBACK clipboard_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
        case WM_NCCREATE:
            return TRUE;
        case WM_CLIPBOARDUPDATE:
            if (is_clipboard_owner) break;  /* ignore our own changes */
            if ((LONG)(GetClipboardSequenceNumber() - last_get_seqno) <= 0) break;
            set_mac_pasteboard_types_from_win32_clipboard();
            break;
        case WM_RENDERFORMAT:
            render_format(wp);
            break;
        case WM_TIMER:
            if (!is_clipboard_owner) break;
            grab_win32_clipboard();
            break;
        case WM_DESTROYCLIPBOARD:
            TRACE("WM_DESTROYCLIPBOARD: lost ownership\n");
            is_clipboard_owner = FALSE;
            KillTimer(hwnd, 1);
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}


/**************************************************************************
 *              wait_clipboard_mutex
 *
 * Make sure that there's only one clipboard thread per window station.
 */
static BOOL wait_clipboard_mutex(void)
{
    static const WCHAR prefix[] = {'_','_','w','i','n','e','_','c','l','i','p','b','o','a','r','d','_'};
    WCHAR buffer[MAX_PATH + ARRAY_SIZE(prefix)];
    HANDLE mutex;

    memcpy(buffer, prefix, sizeof(prefix));
    if (!GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME,
                                   buffer + ARRAY_SIZE(prefix),
                                   sizeof(buffer) - sizeof(prefix), NULL))
    {
        ERR("failed to get winstation name\n");
        return FALSE;
    }
    mutex = CreateMutexW(NULL, TRUE, buffer);
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        TRACE("waiting for mutex %s\n", debugstr_w(buffer));
        WaitForSingleObject(mutex, INFINITE);
    }
    return TRUE;
}


/**************************************************************************
 *              init_pipe_name
 *
 * Init-once helper for get_pipe_name.
 */
static BOOL CALLBACK init_pipe_name(INIT_ONCE* once, void* param, void** context)
{
    static const WCHAR prefix[] = {'\\','\\','.','\\','p','i','p','e','\\','_','_','w','i','n','e','_','c','l','i','p','b','o','a','r','d','_'};

    memcpy(clipboard_pipe_name, prefix, sizeof(prefix));
    if (!GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME,
                                   clipboard_pipe_name + ARRAY_SIZE(prefix),
                                   sizeof(clipboard_pipe_name) - sizeof(prefix), NULL))
    {
        ERR("failed to get winstation name\n");
        return FALSE;
    }

    return TRUE;
}


/**************************************************************************
 *              get_pipe_name
 *
 * Get the name of the pipe used to communicate with the per-window-station
 * clipboard manager thread.
 */
static const WCHAR* get_pipe_name(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, init_pipe_name, NULL, NULL);
    return clipboard_pipe_name[0] ? clipboard_pipe_name : NULL;
}


/**************************************************************************
 *              clipboard_thread
 *
 * Thread running inside the desktop process to manage the clipboard
 */
static DWORD WINAPI clipboard_thread(void *arg)
{
    static const WCHAR clipboard_classname[] = {'_','_','w','i','n','e','_','c','l','i','p','b','o','a','r','d','_','m','a','n','a','g','e','r',0};
    WNDCLASSW class;
    struct macdrv_window_features wf;
    const WCHAR* pipe_name;
    HANDLE pipe = NULL;
    HANDLE event = NULL;
    OVERLAPPED overlapped;
    BOOL need_connect = TRUE, pending = FALSE;
    MSG msg;

    if (!wait_clipboard_mutex()) return 0;

    memset(&class, 0, sizeof(class));
    class.lpfnWndProc   = clipboard_wndproc;
    class.lpszClassName = clipboard_classname;

    if (!RegisterClassW(&class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        ERR("could not register clipboard window class err %u\n", GetLastError());
        return 0;
    }
    if (!(clipboard_hwnd = CreateWindowW(clipboard_classname, NULL, 0, 0, 0, 0, 0,
                                         HWND_MESSAGE, 0, 0, NULL)))
    {
        ERR("failed to create clipboard window err %u\n", GetLastError());
        return 0;
    }

    memset(&wf, 0, sizeof(wf));
    clipboard_cocoa_window = macdrv_create_cocoa_window(&wf, CGRectMake(100, 100, 100, 100), clipboard_hwnd,
                                                        macdrv_init_thread_data()->queue);
    if (!clipboard_cocoa_window)
    {
        ERR("failed to create clipboard Cocoa window\n");
        goto done;
    }

    pipe_name = get_pipe_name();
    if (!pipe_name)
    {
        ERR("failed to get pipe name\n");
        goto done;
    }

    pipe = CreateNamedPipeW(pipe_name, PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
                            PIPE_TYPE_BYTE, PIPE_UNLIMITED_INSTANCES, 1, 1, 0, NULL);
    if (!pipe)
    {
        ERR("failed to create named pipe: %u\n", GetLastError());
        goto done;
    }

    event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!event)
    {
        ERR("failed to create event: %d\n", GetLastError());
        goto done;
    }

    clipboard_thread_id = GetCurrentThreadId();
    NtUserAddClipboardFormatListener(clipboard_hwnd);
    register_builtin_formats();
    grab_win32_clipboard();

    TRACE("clipboard thread %04x running\n", GetCurrentThreadId());
    while (1)
    {
        DWORD result;

        if (need_connect)
        {
            pending = FALSE;
            memset(&overlapped, 0, sizeof(overlapped));
            overlapped.hEvent = event;
            if (ConnectNamedPipe(pipe, &overlapped))
            {
                ERR("asynchronous ConnectNamedPipe unexpectedly returned true: %d\n", GetLastError());
                ResetEvent(event);
            }
            else
            {
                result = GetLastError();
                switch (result)
                {
                    case ERROR_PIPE_CONNECTED:
                    case ERROR_NO_DATA:
                        SetEvent(event);
                        need_connect = FALSE;
                        break;
                    case ERROR_IO_PENDING:
                        need_connect = FALSE;
                        pending = TRUE;
                        break;
                    default:
                        ERR("failed to initiate pipe connection: %d\n", result);
                        break;
                }
            }
        }

        result = MsgWaitForMultipleObjectsEx(1, &event, INFINITE, QS_ALLINPUT, MWMO_ALERTABLE | MWMO_INPUTAVAILABLE);
        switch (result)
        {
            case WAIT_OBJECT_0:
            {
                DWORD written;

                if (pending && !GetOverlappedResult(pipe, &overlapped, &written, FALSE))
                    ERR("failed to connect pipe: %d\n", GetLastError());

                update_clipboard();
                DisconnectNamedPipe(pipe);
                need_connect = TRUE;
                break;
            }
            case WAIT_OBJECT_0 + 1:
                while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
                {
                    if (msg.message == WM_QUIT)
                        goto done;
                    DispatchMessageW(&msg);
                }
                break;
            case WAIT_IO_COMPLETION:
                break;
            default:
                ERR("failed to wait for connection or input: %d\n", GetLastError());
                break;
        }
    }

done:
    if (event) CloseHandle(event);
    if (pipe) CloseHandle(pipe);
    macdrv_destroy_cocoa_window(clipboard_cocoa_window);
    DestroyWindow(clipboard_hwnd);
    return 0;
}


/**************************************************************************
 *              Mac User Driver Clipboard Exports
 **************************************************************************/


/**************************************************************************
 *              macdrv_UpdateClipboard
 */
void CDECL macdrv_UpdateClipboard(void)
{
    static ULONG last_update;
    ULONG now, end;
    const WCHAR* pipe_name;
    HANDLE pipe;
    BYTE dummy;
    DWORD count;
    OVERLAPPED overlapped = { 0 };
    BOOL canceled = FALSE;

    if (GetCurrentThreadId() == clipboard_thread_id) return;

    TRACE("\n");

    now = GetTickCount();
    if ((int)(now - last_update) <= CLIPBOARD_UPDATE_DELAY) return;
    last_update = now;

    if (!(pipe_name = get_pipe_name())) return;
    pipe = CreateFileW(pipe_name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if (pipe == INVALID_HANDLE_VALUE)
    {
        WARN("failed to open pipe to clipboard manager: %d\n", GetLastError());
        return;
    }

    overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!overlapped.hEvent)
    {
        ERR("failed to create event: %d\n", GetLastError());
        goto done;
    }

    /* We expect the read to fail because the server just closes our connection.  This
       is just waiting for that close to happen. */
    if (ReadFile(pipe, &dummy, sizeof(dummy), NULL, &overlapped))
    {
        WARN("asynchronous ReadFile unexpectedly returned true: %d\n", GetLastError());
        goto done;
    }
    else
    {
        DWORD error = GetLastError();
        if (error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_BROKEN_PIPE)
        {
            /* The server accepted, handled, and closed our connection before we
               attempted the read, which is fine. */
            goto done;
        }
        else if (error != ERROR_IO_PENDING)
        {
            ERR("failed to initiate read from pipe: %d\n", error);
            goto done;
        }
    }

    end = now + 500;
    while (1)
    {
        DWORD result, timeout;

        if (canceled)
            timeout = INFINITE;
        else
        {
            now = GetTickCount();
            timeout = end - now;
            if ((int)timeout < 0)
                timeout = 0;
        }

        result = MsgWaitForMultipleObjectsEx(1, &overlapped.hEvent, timeout, QS_SENDMESSAGE, MWMO_ALERTABLE);
        switch (result)
        {
            case WAIT_OBJECT_0:
            {
                if (GetOverlappedResult(pipe, &overlapped, &count, FALSE))
                    WARN("unexpectedly succeeded in reading from pipe\n");
                else
                {
                    result = GetLastError();
                    if (result != ERROR_BROKEN_PIPE && result != ERROR_OPERATION_ABORTED &&
                        result != ERROR_HANDLES_CLOSED)
                        WARN("failed to read from pipe: %d\n", result);
                }

                goto done;
            }
            case WAIT_OBJECT_0 + 1:
            {
                MSG msg;
                while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE | PM_QS_SENDMESSAGE))
                    DispatchMessageW(&msg);
                break;
            }
            case WAIT_IO_COMPLETION:
                break;
            case WAIT_TIMEOUT:
                WARN("timed out waiting for read\n");
                CancelIoEx(pipe, &overlapped);
                canceled = TRUE;
                break;
            default:
                if (canceled)
                {
                    ERR("failed to wait for cancel: %d\n", GetLastError());
                    goto done;
                }

                ERR("failed to wait for read: %d\n", GetLastError());
                CancelIoEx(pipe, &overlapped);
                canceled = TRUE;
                break;
        }
    }

done:
    if (overlapped.hEvent) CloseHandle(overlapped.hEvent);
    CloseHandle(pipe);
}


/**************************************************************************
 *              MACDRV Private Clipboard Exports
 **************************************************************************/


/**************************************************************************
 *              query_pasteboard_data
 */
BOOL query_pasteboard_data(HWND hwnd, CFStringRef type)
{
    struct get_clipboard_params params = { .data_only = TRUE, .size = 1024 };
    WINE_CLIPFORMAT *format;
    BOOL ret = FALSE;

    TRACE("win %p/%p type %s\n", hwnd, clipboard_cocoa_window, debugstr_cf(type));

    format = format_for_type(type);
    if (!format) return FALSE;

    if (!NtUserOpenClipboard(clipboard_hwnd, 0))
    {
        ERR("failed to open clipboard for %s\n", debugstr_cf(type));
        return FALSE;
    }

    for (;;)
    {
        if (!(params.data = malloc(params.size))) break;
        if (NtUserGetClipboardData(format->format_id, &params))
        {
            CFDataRef data;

            TRACE("exporting %s\n", debugstr_format(format->format_id));

            if ((data = format->export_func(params.data, params.size)))
            {
                ret = macdrv_set_pasteboard_data(format->type, data, clipboard_cocoa_window);
                CFRelease(data);
            }
            free(params.data);
            break;
        }
        free(params.data);
        if (!params.data_size) break;
        params.size = params.data_size;
        params.data_size = 0;
    }

    last_get_seqno = NtUserGetClipboardSequenceNumber();

    NtUserCloseClipboard();

    return ret;
}


/**************************************************************************
 *              macdrv_lost_pasteboard_ownership
 *
 * Handler for the LOST_PASTEBOARD_OWNERSHIP event.
 */
void macdrv_lost_pasteboard_ownership(HWND hwnd)
{
    TRACE("win %p\n", hwnd);
    if (!macdrv_is_pasteboard_owner(clipboard_cocoa_window))
        grab_win32_clipboard();
}


/**************************************************************************
 *              macdrv_init_clipboard
 */
void macdrv_init_clipboard(void)
{
    DWORD id;
    HANDLE handle = CreateThread(NULL, 0, clipboard_thread, NULL, 0, &id);

    if (handle) CloseHandle(handle);
    else ERR("failed to create clipboard thread\n");
}
