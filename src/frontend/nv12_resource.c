#define COBJMACROS
#include <initguid.h>
#include <windows.h>
#include <d3d12.h>
#include <stddef.h>
#include <stdlib.h>
#include "wfdxcompat/notifier_registry.h"
#include "wfdxcompat/nv12_resource.h"

struct nv12_resource;

struct nv12_notifier
{
    const ID3DDestructionNotifierVtbl *lpVtbl;
    struct nv12_resource *resource;
};

struct nv12_resource
{
    const ID3D12Resource2Vtbl *lpVtbl;
    LONG refcount;
    ID3D12Device10 *device;
    ID3D12Resource *planes[2];
    D3D12_RESOURCE_DESC desc;
    D3D12_HEAP_PROPERTIES heap_properties;
    D3D12_HEAP_FLAGS heap_flags;
    struct wfdx_notifier_registry *registry;
    struct nv12_notifier notifier;
};

static HRESULT STDMETHODCALLTYPE nv12_query_interface(ID3D12Resource2 *, REFIID, void **);
static ULONG STDMETHODCALLTYPE nv12_add_ref(ID3D12Resource2 *);
static ULONG STDMETHODCALLTYPE nv12_release(ID3D12Resource2 *);

static HRESULT STDMETHODCALLTYPE notifier_query_interface(ID3DDestructionNotifier *, REFIID, void **);
static ULONG STDMETHODCALLTYPE notifier_add_ref(ID3DDestructionNotifier *);
static ULONG STDMETHODCALLTYPE notifier_release(ID3DDestructionNotifier *);
static HRESULT STDMETHODCALLTYPE notifier_register(ID3DDestructionNotifier *, PFN_DESTRUCTION_CALLBACK,
        void *, UINT *);
static HRESULT STDMETHODCALLTYPE notifier_unregister(ID3DDestructionNotifier *, UINT);
static struct nv12_resource *resource_from_notifier(ID3DDestructionNotifier *);

static const ID3DDestructionNotifierVtbl notifier_vtable =
{
    notifier_query_interface,
    notifier_add_ref,
    notifier_release,
    notifier_register,
    notifier_unregister,
};

static BOOL resource_iid(REFIID iid)
{
    return IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_ID3D12Object) ||
            IsEqualGUID(iid, &IID_ID3D12DeviceChild) || IsEqualGUID(iid, &IID_ID3D12Pageable) ||
            IsEqualGUID(iid, &IID_ID3D12Resource) || IsEqualGUID(iid, &IID_ID3D12Resource1) ||
            IsEqualGUID(iid, &IID_ID3D12Resource2);
}

static struct nv12_resource *impl_from_resource(ID3D12Resource2 *iface)
{
    return CONTAINING_RECORD(iface, struct nv12_resource, lpVtbl);
}

static UINT plane_subresource(const struct nv12_resource *resource, UINT subresource, UINT *plane)
{
    UINT array_size = resource->desc.DepthOrArraySize;
    UINT mip_levels = resource->desc.MipLevels;
    UINT plane_stride;

    if (!mip_levels) mip_levels = 1;
    if (!array_size) array_size = 1;
    plane_stride = mip_levels * array_size;
    *plane = subresource / plane_stride;
    return subresource % plane_stride;
}

static HRESULT STDMETHODCALLTYPE nv12_query_interface(ID3D12Resource2 *iface, REFIID iid, void **out)
{
    struct nv12_resource *resource = impl_from_resource(iface);

    if (!out) return E_POINTER;
    *out = NULL;
    if (resource_iid(iid)) *out = iface;
    else if (IsEqualGUID(iid, &IID_ID3DDestructionNotifier)) *out = &resource->notifier;
    else return E_NOINTERFACE;
    nv12_add_ref(iface);
    return S_OK;
}

static ULONG STDMETHODCALLTYPE nv12_add_ref(ID3D12Resource2 *iface)
{
    return InterlockedIncrement(&impl_from_resource(iface)->refcount);
}

static ULONG STDMETHODCALLTYPE nv12_release(ID3D12Resource2 *iface)
{
    struct nv12_resource *resource = impl_from_resource(iface);
    ULONG refs = InterlockedDecrement(&resource->refcount);

    if (!refs)
    {
        wfdx_notifier_registry_close(resource->registry);
        ID3D12Resource_Release(resource->planes[1]);
        ID3D12Resource_Release(resource->planes[0]);
        ID3D12Device10_Release(resource->device);
        wfdx_notifier_registry_destroy(resource->registry);
        free(resource);
    }
    return refs;
}

static HRESULT STDMETHODCALLTYPE nv12_get_private_data(ID3D12Resource2 *iface, REFGUID guid,
        UINT *size, void *data)
{
    return ID3D12Resource_GetPrivateData(impl_from_resource(iface)->planes[0], guid, size, data);
}

static HRESULT STDMETHODCALLTYPE nv12_set_private_data(ID3D12Resource2 *iface, REFGUID guid,
        UINT size, const void *data)
{
    return ID3D12Resource_SetPrivateData(impl_from_resource(iface)->planes[0], guid, size, data);
}

static HRESULT STDMETHODCALLTYPE nv12_set_private_data_interface(ID3D12Resource2 *iface,
        REFGUID guid, const IUnknown *data)
{
    return ID3D12Resource_SetPrivateDataInterface(impl_from_resource(iface)->planes[0], guid, data);
}

static HRESULT STDMETHODCALLTYPE nv12_set_name(ID3D12Resource2 *iface, const WCHAR *name)
{
    struct nv12_resource *resource = impl_from_resource(iface);
    HRESULT hr = ID3D12Resource_SetName(resource->planes[0], name);

    if (SUCCEEDED(hr)) hr = ID3D12Resource_SetName(resource->planes[1], name);
    return hr;
}

static HRESULT STDMETHODCALLTYPE nv12_get_device(ID3D12Resource2 *iface, REFIID iid, void **device)
{
    return ID3D12Device10_QueryInterface(impl_from_resource(iface)->device, iid, device);
}

static HRESULT STDMETHODCALLTYPE nv12_map(ID3D12Resource2 *iface, UINT subresource,
        const D3D12_RANGE *range, void **data)
{
    struct nv12_resource *resource = impl_from_resource(iface);
    UINT plane, local = plane_subresource(resource, subresource, &plane);

    if (plane > 1) return E_INVALIDARG;
    return ID3D12Resource_Map(resource->planes[plane], local, range, data);
}

static void STDMETHODCALLTYPE nv12_unmap(ID3D12Resource2 *iface, UINT subresource,
        const D3D12_RANGE *range)
{
    struct nv12_resource *resource = impl_from_resource(iface);
    UINT plane, local = plane_subresource(resource, subresource, &plane);

    if (plane <= 1) ID3D12Resource_Unmap(resource->planes[plane], local, range);
}

static D3D12_RESOURCE_DESC *STDMETHODCALLTYPE nv12_get_desc(ID3D12Resource2 *iface,
        D3D12_RESOURCE_DESC *desc)
{
    *desc = impl_from_resource(iface)->desc;
    return desc;
}

static D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE nv12_get_gpu_address(ID3D12Resource2 *iface)
{
    return ID3D12Resource_GetGPUVirtualAddress(impl_from_resource(iface)->planes[0]);
}

static HRESULT STDMETHODCALLTYPE nv12_write_subresource(ID3D12Resource2 *iface, UINT subresource,
        const D3D12_BOX *box, const void *data, UINT row_pitch, UINT slice_pitch)
{
    struct nv12_resource *resource = impl_from_resource(iface);
    UINT plane, local = plane_subresource(resource, subresource, &plane);

    if (plane > 1) return E_INVALIDARG;
    return ID3D12Resource_WriteToSubresource(resource->planes[plane], local, box,
            data, row_pitch, slice_pitch);
}

static HRESULT STDMETHODCALLTYPE nv12_read_subresource(ID3D12Resource2 *iface, void *data,
        UINT row_pitch, UINT slice_pitch, UINT subresource, const D3D12_BOX *box)
{
    struct nv12_resource *resource = impl_from_resource(iface);
    UINT plane, local = plane_subresource(resource, subresource, &plane);

    if (plane > 1) return E_INVALIDARG;
    return ID3D12Resource_ReadFromSubresource(resource->planes[plane], data, row_pitch,
            slice_pitch, local, box);
}

static HRESULT STDMETHODCALLTYPE nv12_get_heap_properties(ID3D12Resource2 *iface,
        D3D12_HEAP_PROPERTIES *properties, D3D12_HEAP_FLAGS *flags)
{
    struct nv12_resource *resource = impl_from_resource(iface);

    if (!properties || !flags) return E_INVALIDARG;
    *properties = resource->heap_properties;
    *flags = resource->heap_flags;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE nv12_get_protected_session(ID3D12Resource2 *iface,
        REFIID iid, void **session)
{
    ID3D12Resource1 *resource1;
    HRESULT hr;

    if (FAILED(hr = ID3D12Resource_QueryInterface(impl_from_resource(iface)->planes[0],
            &IID_ID3D12Resource1, (void **)&resource1))) return hr;
    hr = ID3D12Resource1_GetProtectedResourceSession(resource1, iid, session);
    ID3D12Resource1_Release(resource1);
    return hr;
}

static D3D12_RESOURCE_DESC1 *STDMETHODCALLTYPE nv12_get_desc1(ID3D12Resource2 *iface,
        D3D12_RESOURCE_DESC1 *desc)
{
    const D3D12_RESOURCE_DESC *source = &impl_from_resource(iface)->desc;

    desc->Dimension = source->Dimension;
    desc->Alignment = source->Alignment;
    desc->Width = source->Width;
    desc->Height = source->Height;
    desc->DepthOrArraySize = source->DepthOrArraySize;
    desc->MipLevels = source->MipLevels;
    desc->Format = source->Format;
    desc->SampleDesc = source->SampleDesc;
    desc->Layout = source->Layout;
    desc->Flags = source->Flags;
    desc->SamplerFeedbackMipRegion.Width = 0;
    desc->SamplerFeedbackMipRegion.Height = 0;
    desc->SamplerFeedbackMipRegion.Depth = 0;
    return desc;
}

static const ID3D12Resource2Vtbl nv12_vtable =
{
    nv12_query_interface,
    nv12_add_ref,
    nv12_release,
    nv12_get_private_data,
    nv12_set_private_data,
    nv12_set_private_data_interface,
    nv12_set_name,
    nv12_get_device,
    nv12_map,
    nv12_unmap,
    nv12_get_desc,
    nv12_get_gpu_address,
    nv12_write_subresource,
    nv12_read_subresource,
    nv12_get_heap_properties,
    nv12_get_protected_session,
    nv12_get_desc1,
};

static HRESULT STDMETHODCALLTYPE notifier_query_interface(ID3DDestructionNotifier *iface,
        REFIID iid, void **out)
{
    return nv12_query_interface((ID3D12Resource2 *)resource_from_notifier(iface), iid, out);
}

static struct nv12_resource *resource_from_notifier(ID3DDestructionNotifier *iface)
{
    return CONTAINING_RECORD(iface, struct nv12_notifier, lpVtbl)->resource;
}

static ULONG STDMETHODCALLTYPE notifier_add_ref(ID3DDestructionNotifier *iface)
{
    return nv12_add_ref((ID3D12Resource2 *)resource_from_notifier(iface));
}

static ULONG STDMETHODCALLTYPE notifier_release(ID3DDestructionNotifier *iface)
{
    return nv12_release((ID3D12Resource2 *)resource_from_notifier(iface));
}

static HRESULT STDMETHODCALLTYPE notifier_register(ID3DDestructionNotifier *iface,
        PFN_DESTRUCTION_CALLBACK callback, void *data, UINT *id)
{
    return wfdx_notifier_registry_register(resource_from_notifier(iface)->registry,
            callback, data, id);
}

static HRESULT STDMETHODCALLTYPE notifier_unregister(ID3DDestructionNotifier *iface, UINT id)
{
    return wfdx_notifier_registry_unregister(resource_from_notifier(iface)->registry, id);
}

static HRESULT wfdx_nv12_create_resource(ID3D12Device10 *device,
        wfdx_create_committed_resource_fn create_resource,
        const D3D12_HEAP_PROPERTIES *properties, D3D12_HEAP_FLAGS create_flags,
        D3D12_HEAP_FLAGS reported_flags,
        const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state,
        REFIID iid, void **out)
{
    struct nv12_resource *resource;
    D3D12_RESOURCE_DESC plane_desc;
    HRESULT hr;

    if (!out) return E_POINTER;
    *out = NULL;
    if (!properties || !desc || desc->Format != DXGI_FORMAT_NV12 ||
        !resource_iid(iid)) return E_INVALIDARG;
    if (!(resource = calloc(1, sizeof(*resource)))) return E_OUTOFMEMORY;
    resource->lpVtbl = &nv12_vtable;
    resource->refcount = 1;
    resource->device = device;
    resource->desc = *desc;
    resource->heap_properties = *properties;
    resource->heap_flags = reported_flags;
    resource->notifier.lpVtbl = &notifier_vtable;
    resource->notifier.resource = resource;
    ID3D12Device10_AddRef(device);
    if (FAILED(hr = wfdx_notifier_registry_create(&resource->registry))) goto fail;

    plane_desc = *desc;
    plane_desc.Format = DXGI_FORMAT_R8_UNORM;
    if (FAILED(hr = create_resource(device, properties, create_flags, &plane_desc, state, NULL,
            &IID_ID3D12Resource, (void **)&resource->planes[0]))) goto fail;

    plane_desc.Width = (desc->Width + 1) / 2;
    plane_desc.Height = (desc->Height + 1) / 2;
    plane_desc.Format = DXGI_FORMAT_R8G8_UNORM;
    if (FAILED(hr = create_resource(device, properties, create_flags, &plane_desc, state, NULL,
            &IID_ID3D12Resource, (void **)&resource->planes[1]))) goto fail;

    *out = resource;
    return S_OK;

fail:
    if (resource->planes[1]) ID3D12Resource_Release(resource->planes[1]);
    if (resource->planes[0]) ID3D12Resource_Release(resource->planes[0]);
    if (resource->registry) wfdx_notifier_registry_destroy(resource->registry);
    ID3D12Device10_Release(device);
    free(resource);
    return hr;
}

HRESULT wfdx_nv12_create_committed_resource(ID3D12Device10 *device,
        wfdx_create_committed_resource_fn create_resource,
        const D3D12_HEAP_PROPERTIES *properties, D3D12_HEAP_FLAGS flags,
        const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state,
        REFIID iid, void **out)
{
    return wfdx_nv12_create_resource(device, create_resource, properties, flags, flags,
            desc, state, iid, out);
}

HRESULT wfdx_nv12_create_placed_resource(ID3D12Device10 *device,
        wfdx_create_committed_resource_fn create_resource,
        const D3D12_HEAP_DESC *heap_desc, const D3D12_RESOURCE_DESC *desc,
        D3D12_RESOURCE_STATES state, REFIID iid, void **out)
{
    if (!heap_desc || heap_desc->Properties.Type != D3D12_HEAP_TYPE_DEFAULT)
        return E_INVALIDARG;

    /* D3DMetal cannot create a native NV12 placed texture. Keep the public
     * placed-resource contract while backing the two facade planes with
     * ordinary default-heap resources. */
    return wfdx_nv12_create_resource(device, create_resource, &heap_desc->Properties,
            D3D12_HEAP_FLAG_NONE, heap_desc->Flags, desc, state, iid, out);
}

BOOL wfdx_nv12_is_resource(ID3D12Resource *resource)
{
    return resource && resource->lpVtbl == (const ID3D12ResourceVtbl *)&nv12_vtable;
}

ID3D12Resource *wfdx_nv12_get_plane(ID3D12Resource *iface, UINT plane)
{
    struct nv12_resource *resource;

    if (!wfdx_nv12_is_resource(iface) || plane > 1) return NULL;
    resource = impl_from_resource((ID3D12Resource2 *)iface);
    return resource->planes[plane];
}

BOOL wfdx_nv12_resolve_subresource(ID3D12Resource *iface, UINT subresource,
        ID3D12Resource **plane_resource, UINT *local_subresource, UINT *plane_index)
{
    struct nv12_resource *resource;
    UINT plane, local;

    if (!wfdx_nv12_is_resource(iface)) return FALSE;
    resource = impl_from_resource((ID3D12Resource2 *)iface);
    local = plane_subresource(resource, subresource, &plane);
    if (plane > 1) return FALSE;
    if (plane_resource) *plane_resource = resource->planes[plane];
    if (local_subresource) *local_subresource = local;
    if (plane_index) *plane_index = plane;
    return TRUE;
}
