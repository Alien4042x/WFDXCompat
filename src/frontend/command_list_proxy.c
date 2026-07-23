#define COBJMACROS
#include <initguid.h>
#include <windows.h>
#include <d3d12.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "wfdxcompat/command_list_proxy.h"
#include "wfdxcompat/nv12_resource.h"

#define VTABLE_COUNT(type) (sizeof(type) / sizeof(void *))
#define SLOT(type, member) (offsetof(type, member) / sizeof(void *))
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

struct command_list_vtable
{
    struct command_list_vtable *next;
    void **original;
    void **patched;
    size_t count;
};

static SRWLOCK command_list_lock = SRWLOCK_INIT;
static struct command_list_vtable *command_list_vtables;

typedef HRESULT (STDMETHODCALLTYPE *query_interface_fn)(void *, REFIID, void **);
typedef void (STDMETHODCALLTYPE *copy_texture_region_fn)(ID3D12GraphicsCommandList7 *,
        const D3D12_TEXTURE_COPY_LOCATION *, UINT, UINT, UINT,
        const D3D12_TEXTURE_COPY_LOCATION *, const D3D12_BOX *);
typedef void (STDMETHODCALLTYPE *resource_barrier_fn)(ID3D12GraphicsCommandList7 *,
        UINT, const D3D12_RESOURCE_BARRIER *);
typedef void (STDMETHODCALLTYPE *enhanced_barrier_fn)(ID3D12GraphicsCommandList7 *,
        UINT32, const D3D12_BARRIER_GROUP *);

static BOOL iid_equal(REFIID a, REFIID b)
{
    return IsEqualGUID(a, b);
}

static size_t command_list_vtable_count(REFIID iid)
{
    if (iid_equal(iid, &IID_ID3D12GraphicsCommandList7)) return VTABLE_COUNT(ID3D12GraphicsCommandList7Vtbl);
    if (iid_equal(iid, &IID_ID3D12GraphicsCommandList6)) return VTABLE_COUNT(ID3D12GraphicsCommandList6Vtbl);
    if (iid_equal(iid, &IID_ID3D12GraphicsCommandList5)) return VTABLE_COUNT(ID3D12GraphicsCommandList5Vtbl);
    if (iid_equal(iid, &IID_ID3D12GraphicsCommandList4)) return VTABLE_COUNT(ID3D12GraphicsCommandList4Vtbl);
    if (iid_equal(iid, &IID_ID3D12GraphicsCommandList3)) return VTABLE_COUNT(ID3D12GraphicsCommandList3Vtbl);
    if (iid_equal(iid, &IID_ID3D12GraphicsCommandList2)) return VTABLE_COUNT(ID3D12GraphicsCommandList2Vtbl);
    if (iid_equal(iid, &IID_ID3D12GraphicsCommandList1)) return VTABLE_COUNT(ID3D12GraphicsCommandList1Vtbl);
    if (iid_equal(iid, &IID_ID3D12GraphicsCommandList)) return VTABLE_COUNT(ID3D12GraphicsCommandListVtbl);
    if (iid_equal(iid, &IID_ID3D12CommandList)) return VTABLE_COUNT(ID3D12CommandListVtbl);
    if (iid_equal(iid, &IID_ID3D12DeviceChild)) return VTABLE_COUNT(ID3D12DeviceChildVtbl);
    if (iid_equal(iid, &IID_ID3D12Object)) return VTABLE_COUNT(ID3D12ObjectVtbl);
    if (iid_equal(iid, &IID_IUnknown)) return VTABLE_COUNT(IUnknownVtbl);
    return 0;
}

static struct command_list_vtable *find_vtable_locked(void **patched)
{
    struct command_list_vtable *entry;

    for (entry = command_list_vtables; entry; entry = entry->next)
        if (entry->patched == patched) return entry;
    return NULL;
}

static void **find_original_vtable(void *object)
{
    struct command_list_vtable *entry;
    void **original = NULL;

    AcquireSRWLockShared(&command_list_lock);
    entry = find_vtable_locked(*(void ***)object);
    if (entry) original = entry->original;
    ReleaseSRWLockShared(&command_list_lock);
    return original;
}

static DXGI_FORMAT plane_format(UINT plane)
{
    return plane ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM;
}

static BOOL translate_copy_location(const D3D12_TEXTURE_COPY_LOCATION *source,
        D3D12_TEXTURE_COPY_LOCATION *translated, UINT *plane)
{
    ID3D12Resource *resource;
    UINT subresource;

    *translated = *source;
    if (source->Type != D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) return FALSE;
    if (!wfdx_nv12_resolve_subresource(source->pResource, source->SubresourceIndex,
            &resource, &subresource, plane)) return FALSE;
    translated->pResource = resource;
    translated->SubresourceIndex = subresource;
    return TRUE;
}

static HRESULT STDMETHODCALLTYPE proxy_query_interface(ID3D12GraphicsCommandList7 *iface,
        REFIID iid, void **out)
{
    void **original = find_original_vtable(iface);
    query_interface_fn query_interface;
    HRESULT hr;

    if (!original) return E_FAIL;
    query_interface = (query_interface_fn)original[0];
    hr = query_interface(iface, iid, out);
    if (SUCCEEDED(hr) && out && *out && command_list_vtable_count(iid))
    {
        HRESULT wrap_hr = wfdx_wrap_command_list(*out, iid);
        if (FAILED(wrap_hr))
        {
            IUnknown_Release((IUnknown *)*out);
            *out = NULL;
            return wrap_hr;
        }
    }
    return hr;
}

static void STDMETHODCALLTYPE proxy_copy_texture_region(ID3D12GraphicsCommandList7 *iface,
        const D3D12_TEXTURE_COPY_LOCATION *dst, UINT dst_x, UINT dst_y, UINT dst_z,
        const D3D12_TEXTURE_COPY_LOCATION *src, const D3D12_BOX *src_box)
{
    void **original = find_original_vtable(iface);
    copy_texture_region_fn copy_texture_region;
    D3D12_TEXTURE_COPY_LOCATION translated_dst, translated_src;
    BOOL dst_nv12, src_nv12;
    UINT dst_plane = 0, src_plane = 0;

    if (!original || !dst || !src) return;
    copy_texture_region = (copy_texture_region_fn)original[
            SLOT(ID3D12GraphicsCommandList7Vtbl, CopyTextureRegion)];
    dst_nv12 = translate_copy_location(dst, &translated_dst, &dst_plane);
    src_nv12 = translate_copy_location(src, &translated_src, &src_plane);

    if (dst_nv12 && src->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT)
        translated_src.PlacedFootprint.Footprint.Format = plane_format(dst_plane);
    if (src_nv12 && dst->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT)
        translated_dst.PlacedFootprint.Footprint.Format = plane_format(src_plane);

    copy_texture_region(iface, dst_nv12 ? &translated_dst : dst, dst_x, dst_y, dst_z,
            src_nv12 ? &translated_src : src, src_box);
}

static UINT barrier_expansion_count(const D3D12_RESOURCE_BARRIER *barrier)
{
    switch (barrier->Type)
    {
    case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
        return wfdx_nv12_is_resource(barrier->Transition.pResource) &&
                barrier->Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ? 2 : 1;
    case D3D12_RESOURCE_BARRIER_TYPE_UAV:
        return wfdx_nv12_is_resource(barrier->UAV.pResource) ? 2 : 1;
    case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
        return wfdx_nv12_is_resource(barrier->Aliasing.pResourceBefore) ||
                wfdx_nv12_is_resource(barrier->Aliasing.pResourceAfter) ? 2 : 1;
    default:
        return 1;
    }
}

static void translate_transition_barrier(const D3D12_RESOURCE_BARRIER *source,
        D3D12_RESOURCE_BARRIER *translated, UINT plane)
{
    ID3D12Resource *resource;
    UINT subresource;

    *translated = *source;
    if (source->Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
    {
        translated->Transition.pResource = wfdx_nv12_get_plane(source->Transition.pResource, plane);
        return;
    }
    if (wfdx_nv12_resolve_subresource(source->Transition.pResource,
            source->Transition.Subresource, &resource, &subresource, NULL))
    {
        translated->Transition.pResource = resource;
        translated->Transition.Subresource = subresource;
    }
}

static void translate_barrier(const D3D12_RESOURCE_BARRIER *source,
        D3D12_RESOURCE_BARRIER *translated, UINT plane)
{
    *translated = *source;
    switch (source->Type)
    {
    case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
        translate_transition_barrier(source, translated, plane);
        break;
    case D3D12_RESOURCE_BARRIER_TYPE_UAV:
        if (wfdx_nv12_is_resource(source->UAV.pResource))
            translated->UAV.pResource = wfdx_nv12_get_plane(source->UAV.pResource, plane);
        break;
    case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
        if (wfdx_nv12_is_resource(source->Aliasing.pResourceBefore))
            translated->Aliasing.pResourceBefore =
                    wfdx_nv12_get_plane(source->Aliasing.pResourceBefore, plane);
        if (wfdx_nv12_is_resource(source->Aliasing.pResourceAfter))
            translated->Aliasing.pResourceAfter =
                    wfdx_nv12_get_plane(source->Aliasing.pResourceAfter, plane);
        break;
    default:
        break;
    }
}

static void STDMETHODCALLTYPE proxy_resource_barrier(ID3D12GraphicsCommandList7 *iface,
        UINT barrier_count, const D3D12_RESOURCE_BARRIER *barriers)
{
    D3D12_RESOURCE_BARRIER local[32], *translated = local;
    void **original = find_original_vtable(iface);
    resource_barrier_fn resource_barrier;
    UINT i, j = 0, translated_count = 0;

    if (!original || (!barriers && barrier_count)) return;
    resource_barrier = (resource_barrier_fn)original[
            SLOT(ID3D12GraphicsCommandList7Vtbl, ResourceBarrier)];
    for (i = 0; i < barrier_count; ++i)
        translated_count += barrier_expansion_count(&barriers[i]);
    if (translated_count == barrier_count)
    {
        BOOL needs_translation = FALSE;
        for (i = 0; i < barrier_count; ++i)
        {
            const D3D12_RESOURCE_BARRIER *barrier = &barriers[i];
            needs_translation |= barrier->Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION &&
                    wfdx_nv12_is_resource(barrier->Transition.pResource);
        }
        if (!needs_translation)
        {
            resource_barrier(iface, barrier_count, barriers);
            return;
        }
    }
    if (translated_count > ARRAY_SIZE(local) &&
        !(translated = malloc(translated_count * sizeof(*translated)))) return;
    for (i = 0; i < barrier_count; ++i)
    {
        UINT count = barrier_expansion_count(&barriers[i]);
        UINT plane;
        for (plane = 0; plane < count; ++plane)
            translate_barrier(&barriers[i], &translated[j++], plane);
    }
    resource_barrier(iface, translated_count, translated);
    if (translated != local) free(translated);
}

static UINT texture_barrier_expansion_count(const D3D12_TEXTURE_BARRIER *barrier)
{
    UINT first, count;

    if (!wfdx_nv12_is_resource(barrier->pResource)) return 1;
    if (barrier->Subresources.IndexOrFirstMipLevel == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
        return 2;
    first = barrier->Subresources.FirstPlane;
    count = barrier->Subresources.NumPlanes;
    if (first >= 2) return 0;
    if (!count || count > 2 - first) count = 2 - first;
    return count;
}

static void translate_texture_barrier(const D3D12_TEXTURE_BARRIER *source,
        D3D12_TEXTURE_BARRIER *translated, UINT plane)
{
    *translated = *source;
    translated->pResource = wfdx_nv12_get_plane(source->pResource, plane);
    if (source->Subresources.IndexOrFirstMipLevel != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
    {
        translated->Subresources.FirstPlane = 0;
        translated->Subresources.NumPlanes = 1;
    }
}

static void STDMETHODCALLTYPE proxy_enhanced_barrier(ID3D12GraphicsCommandList7 *iface,
        UINT32 group_count, const D3D12_BARRIER_GROUP *groups)
{
    void **original = find_original_vtable(iface);
    enhanced_barrier_fn barrier;
    D3D12_BARRIER_GROUP *translated_groups = NULL;
    D3D12_TEXTURE_BARRIER **owned = NULL;
    BOOL needs_translation = FALSE;
    UINT32 i;

    if (!original || (!groups && group_count)) return;
    barrier = (enhanced_barrier_fn)original[
            SLOT(ID3D12GraphicsCommandList7Vtbl, Barrier)];
    for (i = 0; i < group_count && !needs_translation; ++i)
    {
        UINT32 j;
        if (groups[i].Type != D3D12_BARRIER_TYPE_TEXTURE) continue;
        for (j = 0; j < groups[i].NumBarriers; ++j)
            if (wfdx_nv12_is_resource(groups[i].pTextureBarriers[j].pResource))
            {
                needs_translation = TRUE;
                break;
            }
    }
    if (!needs_translation)
    {
        barrier(iface, group_count, groups);
        return;
    }
    if (!(translated_groups = calloc(group_count, sizeof(*translated_groups))) ||
        !(owned = calloc(group_count, sizeof(*owned)))) goto done;
    for (i = 0; i < group_count; ++i)
    {
        const D3D12_BARRIER_GROUP *source_group = &groups[i];
        D3D12_BARRIER_GROUP *translated_group = &translated_groups[i];
        UINT32 translated_count = 0, j, k;

        *translated_group = *source_group;
        if (source_group->Type != D3D12_BARRIER_TYPE_TEXTURE) continue;
        for (j = 0; j < source_group->NumBarriers; ++j)
            translated_count += texture_barrier_expansion_count(&source_group->pTextureBarriers[j]);
        if (!(owned[i] = calloc(translated_count, sizeof(*owned[i])))) goto done;
        translated_group->pTextureBarriers = owned[i];
        translated_group->NumBarriers = translated_count;
        translated_count = 0;
        for (j = 0; j < source_group->NumBarriers; ++j)
        {
            const D3D12_TEXTURE_BARRIER *source = &source_group->pTextureBarriers[j];
            UINT first = source->Subresources.IndexOrFirstMipLevel ==
                    D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ? 0 : source->Subresources.FirstPlane;
            UINT count = texture_barrier_expansion_count(source);

            if (!wfdx_nv12_is_resource(source->pResource))
            {
                owned[i][translated_count++] = *source;
                continue;
            }
            for (k = 0; k < count; ++k)
                translate_texture_barrier(source, &owned[i][translated_count++], first + k);
        }
    }
    barrier(iface, group_count, translated_groups);

done:
    if (owned)
    {
        for (i = 0; i < group_count; ++i) free(owned[i]);
        free(owned);
    }
    free(translated_groups);
}

HRESULT wfdx_wrap_command_list(void *object, REFIID iid)
{
    struct command_list_vtable *entry, *current_entry;
    void **current, **original, **patched;
    size_t count = command_list_vtable_count(iid);

    if (!object || !count) return E_INVALIDARG;
    current = *(void ***)object;
    AcquireSRWLockExclusive(&command_list_lock);
    if ((current_entry = find_vtable_locked(current)))
    {
        if (current_entry->count >= count)
        {
            ReleaseSRWLockExclusive(&command_list_lock);
            return S_OK;
        }
        original = current_entry->original;
    }
    else original = current;

    for (entry = command_list_vtables; entry; entry = entry->next)
        if (entry->original == original && entry->count >= count)
        {
            InterlockedExchangePointer((void *volatile *)object, entry->patched);
            ReleaseSRWLockExclusive(&command_list_lock);
            return S_OK;
        }
    if (!(entry = calloc(1, sizeof(*entry))) ||
        !(patched = malloc(count * sizeof(*patched))))
    {
        free(entry);
        ReleaseSRWLockExclusive(&command_list_lock);
        return E_OUTOFMEMORY;
    }
    memcpy(patched, original, count * sizeof(*patched));
    patched[SLOT(ID3D12GraphicsCommandList7Vtbl, QueryInterface)] = proxy_query_interface;
    if (count > SLOT(ID3D12GraphicsCommandList7Vtbl, CopyTextureRegion))
        patched[SLOT(ID3D12GraphicsCommandList7Vtbl, CopyTextureRegion)] = proxy_copy_texture_region;
    if (count > SLOT(ID3D12GraphicsCommandList7Vtbl, ResourceBarrier))
        patched[SLOT(ID3D12GraphicsCommandList7Vtbl, ResourceBarrier)] = proxy_resource_barrier;
    if (count > SLOT(ID3D12GraphicsCommandList7Vtbl, Barrier))
        patched[SLOT(ID3D12GraphicsCommandList7Vtbl, Barrier)] = proxy_enhanced_barrier;
    entry->original = original;
    entry->patched = patched;
    entry->count = count;
    entry->next = command_list_vtables;
    command_list_vtables = entry;
    InterlockedExchangePointer((void *volatile *)object, patched);
    ReleaseSRWLockExclusive(&command_list_lock);
    return S_OK;
}
