/*
 * Copyright 2008-2009 Henri Verbeet for CodeWeavers
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

#ifndef __WINE_D3D11_PRIVATE_H
#define __WINE_D3D11_PRIVATE_H

#include "wine/debug.h"
#include "wine/heap.h"

#include <assert.h>

#define COBJMACROS
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "objbase.h"

#include "dxgi1_6.h"
#include "d3d11_4.h"
#ifdef D3D11_INIT_GUID
#include "initguid.h"
#endif
#include "wine/wined3d.h"
#include "wine/winedxgi.h"
#include "wine/rbtree.h"

#define MAKE_TAG(ch0, ch1, ch2, ch3) \
    ((DWORD)(ch0) | ((DWORD)(ch1) << 8) | \
    ((DWORD)(ch2) << 16) | ((DWORD)(ch3) << 24 ))
#define TAG_AON9 MAKE_TAG('A', 'o', 'n', '9')
#define TAG_DXBC MAKE_TAG('D', 'X', 'B', 'C')
#define TAG_ISGN MAKE_TAG('I', 'S', 'G', 'N')
#define TAG_OSG5 MAKE_TAG('O', 'S', 'G', '5')
#define TAG_OSGN MAKE_TAG('O', 'S', 'G', 'N')
#define TAG_PCSG MAKE_TAG('P', 'C', 'S', 'G')
#define TAG_SHDR MAKE_TAG('S', 'H', 'D', 'R')
#define TAG_SHEX MAKE_TAG('S', 'H', 'E', 'X')

struct d3d_device;

/* TRACE helper functions */
const char *debug_d3d10_primitive_topology(D3D10_PRIMITIVE_TOPOLOGY topology) DECLSPEC_HIDDEN;
const char *debug_dxgi_format(DXGI_FORMAT format) DECLSPEC_HIDDEN;
const char *debug_float4(const float *values) DECLSPEC_HIDDEN;

DXGI_FORMAT dxgi_format_from_wined3dformat(enum wined3d_format_id format) DECLSPEC_HIDDEN;
enum wined3d_format_id wined3dformat_from_dxgi_format(DXGI_FORMAT format) DECLSPEC_HIDDEN;
void d3d11_primitive_topology_from_wined3d_primitive_type(enum wined3d_primitive_type primitive_type,
        unsigned int patch_vertex_count, D3D11_PRIMITIVE_TOPOLOGY *topology) DECLSPEC_HIDDEN;
void wined3d_primitive_type_from_d3d11_primitive_topology(D3D11_PRIMITIVE_TOPOLOGY topology,
        enum wined3d_primitive_type *type, unsigned int *patch_vertex_count) DECLSPEC_HIDDEN;
unsigned int wined3d_getdata_flags_from_d3d11_async_getdata_flags(unsigned int d3d11_flags) DECLSPEC_HIDDEN;
DWORD wined3d_usage_from_d3d11(enum D3D11_USAGE usage) DECLSPEC_HIDDEN;
struct wined3d_resource *wined3d_resource_from_d3d11_resource(ID3D11Resource *resource) DECLSPEC_HIDDEN;
struct wined3d_resource *wined3d_resource_from_d3d10_resource(ID3D10Resource *resource) DECLSPEC_HIDDEN;
DWORD wined3d_map_flags_from_d3d11_map_type(D3D11_MAP map_type) DECLSPEC_HIDDEN;
DWORD wined3d_map_flags_from_d3d10_map_type(D3D10_MAP map_type) DECLSPEC_HIDDEN;
DWORD wined3d_clear_flags_from_d3d11_clear_flags(UINT clear_flags) DECLSPEC_HIDDEN;
unsigned int wined3d_access_from_d3d11(D3D11_USAGE usage, UINT cpu_access) DECLSPEC_HIDDEN;
HRESULT d3d_device_create_dxgi_resource(IUnknown *device, struct wined3d_resource *wined3d_resource,
        IUnknown *outer, BOOL needs_surface, IUnknown **dxgi_resource) DECLSPEC_HIDDEN;

enum D3D11_USAGE d3d11_usage_from_d3d10_usage(enum D3D10_USAGE usage) DECLSPEC_HIDDEN;
enum D3D10_USAGE d3d10_usage_from_d3d11_usage(enum D3D11_USAGE usage) DECLSPEC_HIDDEN;
UINT d3d11_bind_flags_from_d3d10_bind_flags(UINT bind_flags) DECLSPEC_HIDDEN;
UINT d3d10_bind_flags_from_d3d11_bind_flags(UINT bind_flags) DECLSPEC_HIDDEN;
UINT d3d11_cpu_access_flags_from_d3d10_cpu_access_flags(UINT cpu_access_flags) DECLSPEC_HIDDEN;
UINT d3d10_cpu_access_flags_from_d3d11_cpu_access_flags(UINT cpu_access_flags) DECLSPEC_HIDDEN;
UINT d3d11_resource_misc_flags_from_d3d10_resource_misc_flags(UINT resource_misc_flags) DECLSPEC_HIDDEN;
UINT d3d10_resource_misc_flags_from_d3d11_resource_misc_flags(UINT resource_misc_flags) DECLSPEC_HIDDEN;

BOOL validate_d3d11_resource_access_flags(D3D11_RESOURCE_DIMENSION resource_dimension,
        D3D11_USAGE usage, UINT bind_flags, UINT cpu_access_flags,
        D3D_FEATURE_LEVEL feature_level) DECLSPEC_HIDDEN;

HRESULT d3d_get_private_data(struct wined3d_private_store *store,
        REFGUID guid, UINT *data_size, void *data) DECLSPEC_HIDDEN;
HRESULT d3d_set_private_data(struct wined3d_private_store *store,
        REFGUID guid, UINT data_size, const void *data) DECLSPEC_HIDDEN;
HRESULT d3d_set_private_data_interface(struct wined3d_private_store *store,
        REFGUID guid, const IUnknown *object) DECLSPEC_HIDDEN;

static inline unsigned int wined3d_bind_flags_from_d3d11(UINT bind_flags, UINT misc_flags)
{
    unsigned int wined3d_flags = bind_flags & (D3D11_BIND_VERTEX_BUFFER
            | D3D11_BIND_INDEX_BUFFER
            | D3D11_BIND_CONSTANT_BUFFER
            | D3D11_BIND_SHADER_RESOURCE
            | D3D11_BIND_STREAM_OUTPUT
            | D3D11_BIND_RENDER_TARGET
            | D3D11_BIND_DEPTH_STENCIL
            | D3D11_BIND_UNORDERED_ACCESS);

    if (misc_flags & D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS)
        wined3d_flags |= WINED3D_BIND_INDIRECT_BUFFER;

    return wined3d_flags;
}

static inline UINT d3d11_bind_flags_from_wined3d(unsigned int bind_flags)
{
    return bind_flags & (WINED3D_BIND_VERTEX_BUFFER
            | WINED3D_BIND_INDEX_BUFFER
            | WINED3D_BIND_CONSTANT_BUFFER
            | WINED3D_BIND_SHADER_RESOURCE
            | WINED3D_BIND_STREAM_OUTPUT
            | WINED3D_BIND_RENDER_TARGET
            | WINED3D_BIND_DEPTH_STENCIL
            | WINED3D_BIND_UNORDERED_ACCESS);
}

/* ID3D11Texture1D, ID3D10Texture1D */
struct d3d_texture1d
{
    ID3D11Texture1D ID3D11Texture1D_iface;
    ID3D10Texture1D ID3D10Texture1D_iface;
    LONG refcount;

    IUnknown *dxgi_resource;
    struct wined3d_texture *wined3d_texture;
    D3D11_TEXTURE1D_DESC desc;
    ID3D11Device2 *device;
};

HRESULT d3d_texture1d_create(struct d3d_device *device, const D3D11_TEXTURE1D_DESC *desc,
        const D3D11_SUBRESOURCE_DATA *data, struct d3d_texture1d **texture) DECLSPEC_HIDDEN;
struct d3d_texture1d *unsafe_impl_from_ID3D11Texture1D(ID3D11Texture1D *iface) DECLSPEC_HIDDEN;
struct d3d_texture1d *unsafe_impl_from_ID3D10Texture1D(ID3D10Texture1D *iface) DECLSPEC_HIDDEN;

/* ID3D11Texture2D, ID3D10Texture2D */
struct d3d_texture2d
{
    ID3D11Texture2D ID3D11Texture2D_iface;
    ID3D10Texture2D ID3D10Texture2D_iface;
    LONG refcount;

    IUnknown *dxgi_resource;
    struct wined3d_texture *wined3d_texture;
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Device2 *device;
};

static inline struct d3d_texture2d *impl_from_ID3D11Texture2D(ID3D11Texture2D *iface)
{
    return CONTAINING_RECORD(iface, struct d3d_texture2d, ID3D11Texture2D_iface);
}

HRESULT d3d_texture2d_create(struct d3d_device *device, const D3D11_TEXTURE2D_DESC *desc,
        const D3D11_SUBRESOURCE_DATA *data, struct d3d_texture2d **texture) DECLSPEC_HIDDEN;
struct d3d_texture2d *unsafe_impl_from_ID3D11Texture2D(ID3D11Texture2D *iface) DECLSPEC_HIDDEN;
struct d3d_texture2d *unsafe_impl_from_ID3D10Texture2D(ID3D10Texture2D *iface) DECLSPEC_HIDDEN;

/* ID3D11Texture3D, ID3D10Texture3D */
struct d3d_texture3d
{
    ID3D11Texture3D ID3D11Texture3D_iface;
    ID3D10Texture3D ID3D10Texture3D_iface;
    LONG refcount;

    IUnknown *dxgi_resource;
    struct wined3d_texture *wined3d_texture;
    D3D11_TEXTURE3D_DESC desc;
    ID3D11Device2 *device;
};

HRESULT d3d_texture3d_create(struct d3d_device *device, const D3D11_TEXTURE3D_DESC *desc,
        const D3D11_SUBRESOURCE_DATA *data, struct d3d_texture3d **texture) DECLSPEC_HIDDEN;
struct d3d_texture3d *unsafe_impl_from_ID3D11Texture3D(ID3D11Texture3D *iface) DECLSPEC_HIDDEN;
struct d3d_texture3d *unsafe_impl_from_ID3D10Texture3D(ID3D10Texture3D *iface) DECLSPEC_HIDDEN;

/* ID3D11Buffer, ID3D10Buffer */
struct d3d_buffer
{
    ID3D11Buffer ID3D11Buffer_iface;
    ID3D10Buffer ID3D10Buffer_iface;
    LONG refcount;

    IUnknown *dxgi_resource;
    struct wined3d_buffer *wined3d_buffer;
    D3D11_BUFFER_DESC desc;
    ID3D11Device2 *device;
};

HRESULT d3d_buffer_create(struct d3d_device *device, const D3D11_BUFFER_DESC *desc,
        const D3D11_SUBRESOURCE_DATA *data, struct d3d_buffer **buffer) DECLSPEC_HIDDEN;
struct d3d_buffer *unsafe_impl_from_ID3D11Buffer(ID3D11Buffer *iface) DECLSPEC_HIDDEN;
struct d3d_buffer *unsafe_impl_from_ID3D10Buffer(ID3D10Buffer *iface) DECLSPEC_HIDDEN;

/* ID3D11DepthStencilView, ID3D10DepthStencilView */
struct d3d_depthstencil_view
{
    ID3D11DepthStencilView ID3D11DepthStencilView_iface;
    ID3D10DepthStencilView ID3D10DepthStencilView_iface;
    LONG refcount;

    struct wined3d_private_store private_store;
    struct wined3d_rendertarget_view *wined3d_view;
    D3D11_DEPTH_STENCIL_VIEW_DESC desc;
    ID3D11Resource *resource;
    ID3D11Device2 *device;
};

HRESULT d3d_depthstencil_view_create(struct d3d_device *device, ID3D11Resource *resource,
        const D3D11_DEPTH_STENCIL_VIEW_DESC *desc, struct d3d_depthstencil_view **view) DECLSPEC_HIDDEN;
struct d3d_depthstencil_view *unsafe_impl_from_ID3D11DepthStencilView(ID3D11DepthStencilView *iface) DECLSPEC_HIDDEN;
struct d3d_depthstencil_view *unsafe_impl_from_ID3D10DepthStencilView(ID3D10DepthStencilView *iface) DECLSPEC_HIDDEN;

/* ID3D11RenderTargetView, ID3D10RenderTargetView */
struct d3d_rendertarget_view
{
    ID3D11RenderTargetView ID3D11RenderTargetView_iface;
    ID3D10RenderTargetView ID3D10RenderTargetView_iface;
    LONG refcount;

    struct wined3d_private_store private_store;
    struct wined3d_rendertarget_view *wined3d_view;
    D3D11_RENDER_TARGET_VIEW_DESC desc;
    ID3D11Resource *resource;
    ID3D11Device2 *device;
};

HRESULT d3d_rendertarget_view_create(struct d3d_device *device, ID3D11Resource *resource,
        const D3D11_RENDER_TARGET_VIEW_DESC *desc, struct d3d_rendertarget_view **view) DECLSPEC_HIDDEN;
struct d3d_rendertarget_view *unsafe_impl_from_ID3D11RenderTargetView(ID3D11RenderTargetView *iface) DECLSPEC_HIDDEN;
struct d3d_rendertarget_view *unsafe_impl_from_ID3D10RenderTargetView(ID3D10RenderTargetView *iface) DECLSPEC_HIDDEN;

/* ID3D11ShaderResourceView, ID3D10ShaderResourceView1 */
struct d3d_shader_resource_view
{
    ID3D11ShaderResourceView ID3D11ShaderResourceView_iface;
    ID3D10ShaderResourceView1 ID3D10ShaderResourceView1_iface;
    LONG refcount;

    struct wined3d_private_store private_store;
    struct wined3d_shader_resource_view *wined3d_view;
    D3D11_SHADER_RESOURCE_VIEW_DESC desc;
    ID3D11Resource *resource;
    ID3D11Device2 *device;
};

HRESULT d3d_shader_resource_view_create(struct d3d_device *device, ID3D11Resource *resource,
        const D3D11_SHADER_RESOURCE_VIEW_DESC *desc, struct d3d_shader_resource_view **view) DECLSPEC_HIDDEN;
struct d3d_shader_resource_view *unsafe_impl_from_ID3D11ShaderResourceView(
        ID3D11ShaderResourceView *iface) DECLSPEC_HIDDEN;
struct d3d_shader_resource_view *unsafe_impl_from_ID3D10ShaderResourceView(
        ID3D10ShaderResourceView *iface) DECLSPEC_HIDDEN;

/* ID3D11UnorderedAccessView */
struct d3d11_unordered_access_view
{
    ID3D11UnorderedAccessView ID3D11UnorderedAccessView_iface;
    LONG refcount;

    struct wined3d_private_store private_store;
    struct wined3d_unordered_access_view *wined3d_view;
    D3D11_UNORDERED_ACCESS_VIEW_DESC desc;
    ID3D11Resource *resource;
    ID3D11Device2 *device;
};

HRESULT d3d11_unordered_access_view_create(struct d3d_device *device, ID3D11Resource *resource,
        const D3D11_UNORDERED_ACCESS_VIEW_DESC *desc, struct d3d11_unordered_access_view **view) DECLSPEC_HIDDEN;
struct d3d11_unordered_access_view *unsafe_impl_from_ID3D11UnorderedAccessView(
        ID3D11UnorderedAccessView *iface) DECLSPEC_HIDDEN;

/* ID3D11InputLayout, ID3D10InputLayout */
struct d3d_input_layout
{
    ID3D11InputLayout ID3D11InputLayout_iface;
    ID3D10InputLayout ID3D10InputLayout_iface;
    LONG refcount;

    struct wined3d_private_store private_store;
    struct wined3d_vertex_declaration *wined3d_decl;
    ID3D11Device2 *device;
};

HRESULT d3d_input_layout_create(struct d3d_device *device,
        const D3D11_INPUT_ELEMENT_DESC *element_descs, UINT element_count,
        const void *shader_byte_code, SIZE_T shader_byte_code_length,
        struct d3d_input_layout **layout) DECLSPEC_HIDDEN;
struct d3d_input_layout *unsafe_impl_from_ID3D11InputLayout(ID3D11InputLayout *iface) DECLSPEC_HIDDEN;
struct d3d_input_layout *unsafe_impl_from_ID3D10InputLayout(ID3D10InputLayout *iface) DECLSPEC_HIDDEN;

/* ID3D11VertexShader, ID3D10VertexShader */
struct d3d_vertex_shader
{
    ID3D11VertexShader ID3D11VertexShader_iface;
    ID3D10VertexShader ID3D10VertexShader_iface;
    LONG refcount;

    struct wined3d_private_store private_store;
    struct wined3d_shader *wined3d_shader;
    ID3D11Device2 *device;
};

HRESULT d3d_vertex_shader_create(struct d3d_device *device, const void *byte_code, SIZE_T byte_code_length,
        struct d3d_vertex_shader **shader) DECLSPEC_HIDDEN;
struct d3d_vertex_shader *unsafe_impl_from_ID3D11VertexShader(ID3D11VertexShader *iface) DECLSPEC_HIDDEN;
struct d3d_vertex_shader *unsafe_impl_from_ID3D10VertexShader(ID3D10VertexShader *iface) DECLSPEC_HIDDEN;

/* ID3D11HullShader */
struct d3d11_hull_shader
{
    ID3D11HullShader ID3D11HullShader_iface;
    LONG refcount;

    struct wined3d_private_store private_store;
    struct wined3d_shader *wined3d_shader;
    ID3D11Device2 *device;
};

HRESULT d3d11_hull_shader_create(struct d3d_device *device, const void *byte_code, SIZE_T byte_code_length,
        struct d3d11_hull_shader **shader) DECLSPEC_HIDDEN;
struct d3d11_hull_shader *unsafe_impl_from_ID3D11HullShader(ID3D11HullShader *iface) DECLSPEC_HIDDEN;

/* ID3D11DomainShader */
struct d3d11_domain_shader
{
    ID3D11DomainShader ID3D11DomainShader_iface;
    LONG refcount;

    struct wined3d_private_store private_store;
    struct wined3d_shader *wined3d_shader;
    ID3D11Device2 *device;
};

HRESULT d3d11_domain_shader_create(struct d3d_device *device, const void *byte_code, SIZE_T byte_code_length,
        struct d3d11_domain_shader **shader) DECLSPEC_HIDDEN;
struct d3d11_domain_shader *unsafe_impl_from_ID3D11DomainShader(ID3D11DomainShader *iface) DECLSPEC_HIDDEN;

/* ID3D11GeometryShader, ID3D10GeometryShader */
struct d3d_geometry_shader
{
    ID3D11GeometryShader ID3D11GeometryShader_iface;
    ID3D10GeometryShader ID3D10GeometryShader_iface;
    LONG refcount;

    struct wined3d_private_store private_store;
    struct wined3d_shader *wined3d_shader;
    ID3D11Device2 *device;
};

HRESULT d3d_geometry_shader_create(struct d3d_device *device, const void *byte_code, SIZE_T byte_code_length,
        const D3D11_SO_DECLARATION_ENTRY *so_entries, unsigned int so_entry_count,
        const unsigned int *buffer_strides, unsigned int buffer_stride_count, unsigned int rasterizer_stream,
        struct d3d_geometry_shader **shader) DECLSPEC_HIDDEN;
struct d3d_geometry_shader *unsafe_impl_from_ID3D11GeometryShader(ID3D11GeometryShader *iface) DECLSPEC_HIDDEN;
struct d3d_geometry_shader *unsafe_impl_from_ID3D10GeometryShader(ID3D10GeometryShader *iface) DECLSPEC_HIDDEN;

/* ID3D11PixelShader, ID3D10PixelShader */
struct d3d_pixel_shader
{
    ID3D11PixelShader ID3D11PixelShader_iface;
    ID3D10PixelShader ID3D10PixelShader_iface;
    LONG refcount;

    struct wined3d_private_store private_store;
    struct wined3d_shader *wined3d_shader;
    ID3D11Device2 *device;
};

HRESULT d3d_pixel_shader_create(struct d3d_device *device, const void *byte_code, SIZE_T byte_code_length,
        struct d3d_pixel_shader **shader) DECLSPEC_HIDDEN;
struct d3d_pixel_shader *unsafe_impl_from_ID3D11PixelShader(ID3D11PixelShader *iface) DECLSPEC_HIDDEN;
struct d3d_pixel_shader *unsafe_impl_from_ID3D10PixelShader(ID3D10PixelShader *iface) DECLSPEC_HIDDEN;

/* ID3D11ComputeShader */
struct d3d11_compute_shader
{
    ID3D11ComputeShader ID3D11ComputeShader_iface;
    LONG refcount;

    struct wined3d_private_store private_store;
    struct wined3d_shader *wined3d_shader;
    ID3D11Device2 *device;
};

HRESULT d3d11_compute_shader_create(struct d3d_device *device, const void *byte_code, SIZE_T byte_code_length,
        struct d3d11_compute_shader **shader) DECLSPEC_HIDDEN;
struct d3d11_compute_shader *unsafe_impl_from_ID3D11ComputeShader(ID3D11ComputeShader *iface) DECLSPEC_HIDDEN;

/* ID3D11ClassLinkage */
struct d3d11_class_linkage
{
    ID3D11ClassLinkage ID3D11ClassLinkage_iface;
    LONG refcount;

    struct wined3d_private_store private_store;
    ID3D11Device2 *device;
};

HRESULT d3d11_class_linkage_create(struct d3d_device *device,
        struct d3d11_class_linkage **class_linkage) DECLSPEC_HIDDEN;

/* ID3D11BlendState1, ID3D10BlendState1 */
struct d3d_blend_state
{
    ID3D11BlendState1 ID3D11BlendState1_iface;
    ID3D10BlendState1 ID3D10BlendState1_iface;
    LONG refcount;

    struct wined3d_private_store private_store;
    struct wined3d_blend_state *wined3d_state;
    D3D11_BLEND_DESC1 desc;
    struct wine_rb_entry entry;
    ID3D11Device2 *device;
};

static inline struct d3d_blend_state *impl_from_ID3D11BlendState1(ID3D11BlendState1 *iface)
{
    return CONTAINING_RECORD(iface, struct d3d_blend_state, ID3D11BlendState1_iface);
}

HRESULT d3d_blend_state_create(struct d3d_device *device, const D3D11_BLEND_DESC1 *desc,
        struct d3d_blend_state **state) DECLSPEC_HIDDEN;
struct d3d_blend_state *unsafe_impl_from_ID3D11BlendState(ID3D11BlendState *iface) DECLSPEC_HIDDEN;
struct d3d_blend_state *unsafe_impl_from_ID3D10BlendState(ID3D10BlendState *iface) DECLSPEC_HIDDEN;

/* ID3D11DepthStencilState, ID3D10DepthStencilState */
struct d3d_depthstencil_state
{
    ID3D11DepthStencilState ID3D11DepthStencilState_iface;
    ID3D10DepthStencilState ID3D10DepthStencilState_iface;
    LONG refcount;

    struct wined3d_private_store private_store;
    struct wined3d_depth_stencil_state *wined3d_state;
    D3D11_DEPTH_STENCIL_DESC desc;
    struct wine_rb_entry entry;
    ID3D11Device2 *device;
};

static inline struct d3d_depthstencil_state *impl_from_ID3D11DepthStencilState(ID3D11DepthStencilState *iface)
{
    return CONTAINING_RECORD(iface, struct d3d_depthstencil_state, ID3D11DepthStencilState_iface);
}

HRESULT d3d_depthstencil_state_create(struct d3d_device *device, const D3D11_DEPTH_STENCIL_DESC *desc,
        struct d3d_depthstencil_state **state) DECLSPEC_HIDDEN;
struct d3d_depthstencil_state *unsafe_impl_from_ID3D11DepthStencilState(
        ID3D11DepthStencilState *iface) DECLSPEC_HIDDEN;
struct d3d_depthstencil_state *unsafe_impl_from_ID3D10DepthStencilState(
        ID3D10DepthStencilState *iface) DECLSPEC_HIDDEN;

/* ID3D11RasterizerState, ID3D10RasterizerState */
struct d3d_rasterizer_state
{
    ID3D11RasterizerState1 ID3D11RasterizerState1_iface;
    ID3D10RasterizerState ID3D10RasterizerState_iface;
    LONG refcount;

    struct wined3d_private_store private_store;
    struct wined3d_rasterizer_state *wined3d_state;
    D3D11_RASTERIZER_DESC1 desc;
    struct wine_rb_entry entry;
    ID3D11Device2 *device;
};

HRESULT d3d_rasterizer_state_create(struct d3d_device *device, const D3D11_RASTERIZER_DESC1 *desc,
        struct d3d_rasterizer_state **state) DECLSPEC_HIDDEN;
struct d3d_rasterizer_state *unsafe_impl_from_ID3D11RasterizerState(ID3D11RasterizerState *iface) DECLSPEC_HIDDEN;
struct d3d_rasterizer_state *unsafe_impl_from_ID3D10RasterizerState(ID3D10RasterizerState *iface) DECLSPEC_HIDDEN;

/* ID3D11SamplerState, ID3D10SamplerState */
struct d3d_sampler_state
{
    ID3D11SamplerState ID3D11SamplerState_iface;
    ID3D10SamplerState ID3D10SamplerState_iface;
    LONG refcount;

    struct wined3d_private_store private_store;
    struct wined3d_sampler *wined3d_sampler;
    D3D11_SAMPLER_DESC desc;
    struct wine_rb_entry entry;
    ID3D11Device2 *device;
};

HRESULT d3d_sampler_state_create(struct d3d_device *device, const D3D11_SAMPLER_DESC *desc,
        struct d3d_sampler_state **state) DECLSPEC_HIDDEN;
struct d3d_sampler_state *unsafe_impl_from_ID3D11SamplerState(ID3D11SamplerState *iface) DECLSPEC_HIDDEN;
struct d3d_sampler_state *unsafe_impl_from_ID3D10SamplerState(ID3D10SamplerState *iface) DECLSPEC_HIDDEN;

/* ID3D11Query, ID3D10Query */
struct d3d_query
{
    ID3D11Query ID3D11Query_iface;
    ID3D10Query ID3D10Query_iface;
    LONG refcount;

    struct wined3d_private_store private_store;
    struct wined3d_query *wined3d_query;
    BOOL predicate;
    D3D11_QUERY_DESC desc;
    ID3D11Device2 *device;
};

HRESULT d3d_query_create(struct d3d_device *device, const D3D11_QUERY_DESC *desc, BOOL predicate,
        struct d3d_query **query) DECLSPEC_HIDDEN;
struct d3d_query *unsafe_impl_from_ID3D11Query(ID3D11Query *iface) DECLSPEC_HIDDEN;
struct d3d_query *unsafe_impl_from_ID3D10Query(ID3D10Query *iface) DECLSPEC_HIDDEN;
struct d3d_query *unsafe_impl_from_ID3D11Asynchronous(ID3D11Asynchronous *iface) DECLSPEC_HIDDEN;

struct d3d_device_context_state_entry
{
    struct d3d_device *device;
    struct wined3d_state *wined3d_state;
};

/* ID3DDeviceContextState */
struct d3d_device_context_state
{
    ID3DDeviceContextState ID3DDeviceContextState_iface;
    LONG refcount, private_refcount;

    struct wined3d_private_store private_store;
    D3D_FEATURE_LEVEL feature_level;
    GUID emulated_interface;

    struct d3d_device_context_state_entry *entries;
    SIZE_T entries_size;
    SIZE_T entry_count;

    struct wined3d_device *wined3d_device;
    ID3D11Device2 *device;
};

/* ID3D11DeviceContext */
struct d3d11_device_context
{
    ID3D11DeviceContext1 ID3D11DeviceContext1_iface;
    ID3D11Multithread ID3D11Multithread_iface;
    ID3DUserDefinedAnnotation ID3DUserDefinedAnnotation_iface;
    IWineD3DDeviceContext IWineD3DDeviceContext_iface;
    LONG refcount;

    D3D11_DEVICE_CONTEXT_TYPE type;
    struct wined3d_device_context *wined3d_context;
    struct d3d_device *device;

    struct wined3d_private_store private_store;
};

/* ID3D11Device, ID3D10Device1 */
struct d3d_device
{
    IUnknown IUnknown_inner;
    ID3D11Device2 ID3D11Device2_iface;
    ID3D10Device1 ID3D10Device1_iface;
    ID3D10Multithread ID3D10Multithread_iface;
    IWineD3DDeviceContext IWineD3DDeviceContext_iface;
    IWineDXGIDeviceParent IWineDXGIDeviceParent_iface;
    IUnknown *outer_unk;
    LONG refcount;

    BOOL d3d11_only;

    struct d3d_device_context_state *state;
    struct d3d11_device_context immediate_context;

    struct wined3d_device_parent device_parent;
    struct wined3d_device *wined3d_device;

    struct wine_rb_tree blend_states;
    struct wine_rb_tree depthstencil_states;
    struct wine_rb_tree rasterizer_states;
    struct wine_rb_tree sampler_states;

    struct d3d_device_context_state **context_states;
    SIZE_T context_states_size;
    SIZE_T context_state_count;
};

struct d3d11_command_list
{
    ID3D11CommandList ID3D11CommandList_iface;
    LONG refcount;

    ID3D11Device2 *device;
    struct wined3d_command_list *wined3d_list;
    struct wined3d_private_store private_store;
};

static inline struct d3d_device *impl_from_ID3D11Device(ID3D11Device *iface)
{
    return CONTAINING_RECORD((ID3D11Device2 *)iface, struct d3d_device, ID3D11Device2_iface);
}

static inline struct d3d_device *impl_from_ID3D11Device2(ID3D11Device2 *iface)
{
    return CONTAINING_RECORD(iface, struct d3d_device, ID3D11Device2_iface);
}

static inline struct d3d_device *impl_from_ID3D10Device(ID3D10Device1 *iface)
{
    return CONTAINING_RECORD(iface, struct d3d_device, ID3D10Device1_iface);
}

void d3d_device_init(struct d3d_device *device, void *outer_unknown) DECLSPEC_HIDDEN;

/* Layered device */
enum dxgi_device_layer_id
{
    DXGI_DEVICE_LAYER_DEBUG1        = 0x8,
    DXGI_DEVICE_LAYER_THREAD_SAFE   = 0x10,
    DXGI_DEVICE_LAYER_DEBUG2        = 0x20,
    DXGI_DEVICE_LAYER_SWITCH_TO_REF = 0x30,
    DXGI_DEVICE_LAYER_D3D10_DEVICE  = 0xffffffff,
};

struct layer_get_size_args
{
    DWORD unknown0;
    DWORD unknown1;
    DWORD *unknown2;
    DWORD *unknown3;
    IDXGIAdapter *adapter;
    WORD interface_major;
    WORD interface_minor;
    WORD version_build;
    WORD version_revision;
};

struct dxgi_device_layer
{
    enum dxgi_device_layer_id id;
    HRESULT (WINAPI *init)(enum dxgi_device_layer_id id, DWORD *count, DWORD *values);
    UINT (WINAPI *get_size)(enum dxgi_device_layer_id id, struct layer_get_size_args *args, DWORD unknown0);
    HRESULT (WINAPI *create)(enum dxgi_device_layer_id id, void **layer_base, DWORD unknown0,
            void *device_object, REFIID riid, void **device_layer);
};

HRESULT WINAPI DXGID3D10CreateDevice(HMODULE d3d10core, IDXGIFactory *factory, IDXGIAdapter *adapter,
        unsigned int flags, const D3D_FEATURE_LEVEL *feature_levels, unsigned int level_count, void **device);
HRESULT WINAPI DXGID3D10RegisterLayers(const struct dxgi_device_layer *layers, UINT layer_count);

#endif /* __WINE_D3D11_PRIVATE_H */
