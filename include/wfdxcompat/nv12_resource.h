#ifndef WFDXCOMPAT_NV12_RESOURCE_H
#define WFDXCOMPAT_NV12_RESOURCE_H

#include <d3d12.h>

typedef HRESULT (STDMETHODCALLTYPE *wfdx_create_committed_resource_fn)(
        ID3D12Device10 *device, const D3D12_HEAP_PROPERTIES *properties,
        D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC *desc,
        D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear,
        REFIID iid, void **resource);

HRESULT wfdx_nv12_create_committed_resource(ID3D12Device10 *device,
        wfdx_create_committed_resource_fn create_resource,
        const D3D12_HEAP_PROPERTIES *properties, D3D12_HEAP_FLAGS flags,
        const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state,
        REFIID iid, void **resource);
HRESULT wfdx_nv12_create_placed_resource(ID3D12Device10 *device,
        wfdx_create_committed_resource_fn create_resource,
        const D3D12_HEAP_DESC *heap_desc, const D3D12_RESOURCE_DESC *desc,
        D3D12_RESOURCE_STATES state, REFIID iid, void **resource);
BOOL wfdx_nv12_is_resource(ID3D12Resource *resource);
ID3D12Resource *wfdx_nv12_get_plane(ID3D12Resource *resource, UINT plane);
BOOL wfdx_nv12_resolve_subresource(ID3D12Resource *resource, UINT subresource,
        ID3D12Resource **plane_resource, UINT *plane_subresource, UINT *plane_index);

#endif
