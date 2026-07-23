#define COBJMACROS
#include <initguid.h>
#include <windows.h>
#include <d3d12.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wfdxcompat/notifier_registry.h"
#include "wfdxcompat/resource_notifier.h"

#define VTABLE_COUNT(type) (sizeof(type) / sizeof(void *))

typedef HRESULT (STDMETHODCALLTYPE *query_interface_fn)(void *, REFIID, void **);
typedef ULONG (STDMETHODCALLTYPE *add_ref_fn)(void *);
typedef ULONG (STDMETHODCALLTYPE *release_fn)(void *);

struct notifier_resource_state;

struct resource_notifier
{
    const ID3DDestructionNotifierVtbl *lpVtbl;
    struct notifier_resource_state *resource;
};

struct notifier_resource_state
{
    struct notifier_resource_state *next;
    void *object;
    query_interface_fn query_interface;
    add_ref_fn add_ref;
    release_fn release;
    struct wfdx_notifier_registry *registry;
    struct resource_notifier notifier;
};

struct resource_vtable_overlay
{
    struct resource_vtable_overlay *next;
    void **original;
    void **patched;
};

static SRWLOCK resource_lock = SRWLOCK_INIT;
static struct notifier_resource_state *notifier_resources;
static struct resource_vtable_overlay *resource_vtables;

static BOOL notifier_trace_enabled(void)
{
    char value[2];

    return GetEnvironmentVariableA("WFDXCOMPAT_TRACE_VIDEO", value, sizeof(value)) != 0;
}

static HRESULT STDMETHODCALLTYPE resource_query_interface(ID3D12Resource2 *, REFIID, void **);
static ULONG STDMETHODCALLTYPE resource_add_ref(ID3D12Resource2 *);
static ULONG STDMETHODCALLTYPE resource_release(ID3D12Resource2 *);
static HRESULT STDMETHODCALLTYPE notifier_query_interface(ID3DDestructionNotifier *, REFIID, void **);
static ULONG STDMETHODCALLTYPE notifier_add_ref(ID3DDestructionNotifier *);
static ULONG STDMETHODCALLTYPE notifier_release(ID3DDestructionNotifier *);
static HRESULT STDMETHODCALLTYPE notifier_register(ID3DDestructionNotifier *,
        PFN_DESTRUCTION_CALLBACK, void *, UINT *);
static HRESULT STDMETHODCALLTYPE notifier_unregister(ID3DDestructionNotifier *, UINT);

static const ID3DDestructionNotifierVtbl notifier_vtable =
{
    notifier_query_interface,
    notifier_add_ref,
    notifier_release,
    notifier_register,
    notifier_unregister,
};

static BOOL public_resource_iid(REFIID iid)
{
    return IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_ID3D12Object) ||
            IsEqualGUID(iid, &IID_ID3D12DeviceChild) ||
            IsEqualGUID(iid, &IID_ID3D12Pageable) ||
            IsEqualGUID(iid, &IID_ID3D12Resource) ||
            IsEqualGUID(iid, &IID_ID3D12Resource1) ||
            IsEqualGUID(iid, &IID_ID3D12Resource2);
}

static BOOL electra_data_buffer_desc_matches(const D3D12_HEAP_DESC *heap_desc,
        UINT64 heap_offset, const D3D12_RESOURCE_DESC *desc,
        D3D12_RESOURCE_STATES state)
{
    const D3D12_HEAP_FLAGS required_flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    const D3D12_HEAP_FLAGS allowed_flags = required_flags |
            D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;
    UINT64 slot_count;

    if (!heap_desc || !desc ||
        heap_desc->Properties.Type != D3D12_HEAP_TYPE_UPLOAD ||
        (heap_desc->Flags & required_flags) != required_flags ||
        (heap_desc->Flags & ~allowed_flags) ||
        desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        desc->Alignment != 0 || !desc->Width ||
        desc->Width % D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT ||
        desc->Height != 1 || desc->DepthOrArraySize != 1 || desc->MipLevels != 1 ||
        desc->Format != DXGI_FORMAT_UNKNOWN || desc->SampleDesc.Count != 1 ||
        desc->SampleDesc.Quality != 0 ||
        desc->Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR ||
        desc->Flags != D3D12_RESOURCE_FLAG_NONE ||
        state != D3D12_RESOURCE_STATE_GENERIC_READ ||
        heap_desc->SizeInBytes > 0xffff0000ULL ||
        heap_desc->SizeInBytes % desc->Width || heap_offset % desc->Width)
        return FALSE;

    slot_count = heap_desc->SizeInBytes / desc->Width;
    return slot_count && slot_count <= 32 && heap_offset / desc->Width < slot_count;
}

static struct notifier_resource_state *find_resource_locked(void *object)
{
    struct notifier_resource_state *resource;

    for (resource = notifier_resources; resource; resource = resource->next)
        if (resource->object == object) return resource;
    return NULL;
}

static struct notifier_resource_state *find_resource(void *object)
{
    struct notifier_resource_state *resource;

    AcquireSRWLockShared(&resource_lock);
    resource = find_resource_locked(object);
    ReleaseSRWLockShared(&resource_lock);
    return resource;
}

static ULONG release_resource(struct notifier_resource_state *resource, void *object)
{
    struct notifier_resource_state **cursor;
    ULONG refs = resource->release(object);

    if (refs) return refs;
    AcquireSRWLockExclusive(&resource_lock);
    for (cursor = &notifier_resources; *cursor; cursor = &(*cursor)->next)
        if (*cursor == resource)
        {
            *cursor = resource->next;
            break;
        }
    ReleaseSRWLockExclusive(&resource_lock);
    wfdx_notifier_registry_close(resource->registry);
    wfdx_notifier_registry_destroy(resource->registry);
    free(resource);
    return 0;
}

static HRESULT STDMETHODCALLTYPE resource_query_interface(ID3D12Resource2 *iface,
        REFIID iid, void **out)
{
    struct notifier_resource_state *resource = find_resource(iface);
    HRESULT hr;

    if (IsEqualGUID(iid, &IID_ID3DDestructionNotifier) && notifier_trace_enabled())
    {
        fprintf(stderr, "wfdxcompat-video: notifier-QI iface=%p tracked=%u\n",
                iface, resource != NULL);
        fflush(stderr);
    }
    if (!resource) return E_NOINTERFACE;
    if (!out) return E_POINTER;
    *out = NULL;
    hr = resource->query_interface(iface, iid, out);
    if (SUCCEEDED(hr) || !IsEqualGUID(iid, &IID_ID3DDestructionNotifier)) return hr;
    if (hr != E_NOINTERFACE) return hr;
    *out = &resource->notifier;
    resource->add_ref(resource->object);
    if (notifier_trace_enabled())
    {
        fprintf(stderr, "wfdxcompat-video: notifier-QI fallback iface=%p notifier=%p\n",
                iface, *out);
        fflush(stderr);
    }
    return S_OK;
}

static ULONG STDMETHODCALLTYPE resource_add_ref(ID3D12Resource2 *iface)
{
    struct notifier_resource_state *resource = find_resource(iface);

    return resource ? resource->add_ref(iface) : 0;
}

static ULONG STDMETHODCALLTYPE resource_release(ID3D12Resource2 *iface)
{
    struct notifier_resource_state *resource = find_resource(iface);

    return resource ? release_resource(resource, iface) : 0;
}

static struct notifier_resource_state *resource_from_notifier(ID3DDestructionNotifier *iface)
{
    return CONTAINING_RECORD(iface, struct resource_notifier, lpVtbl)->resource;
}

static HRESULT STDMETHODCALLTYPE notifier_query_interface(ID3DDestructionNotifier *iface,
        REFIID iid, void **out)
{
    struct notifier_resource_state *resource = resource_from_notifier(iface);

    if (IsEqualGUID(iid, &IID_ID3DDestructionNotifier))
    {
        if (!out) return E_POINTER;
        *out = iface;
        resource->add_ref(resource->object);
        return S_OK;
    }
    return resource->query_interface(resource->object, iid, out);
}

static ULONG STDMETHODCALLTYPE notifier_add_ref(ID3DDestructionNotifier *iface)
{
    struct notifier_resource_state *resource = resource_from_notifier(iface);

    return resource->add_ref(resource->object);
}

static ULONG STDMETHODCALLTYPE notifier_release(ID3DDestructionNotifier *iface)
{
    struct notifier_resource_state *resource = resource_from_notifier(iface);

    return release_resource(resource, resource->object);
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

static void **get_or_create_vtable(void **original)
{
    struct resource_vtable_overlay *entry;
    void **patched;

    for (entry = resource_vtables; entry; entry = entry->next)
        if (entry->original == original) return entry->patched;
    if (!(entry = calloc(1, sizeof(*entry)))) return NULL;
    if (!(patched = malloc(sizeof(ID3D12Resource2Vtbl))))
    {
        free(entry);
        return NULL;
    }
    memcpy(patched, original, sizeof(ID3D12Resource2Vtbl));
    patched[0] = resource_query_interface;
    patched[1] = resource_add_ref;
    patched[2] = resource_release;
    entry->original = original;
    entry->patched = patched;
    entry->next = resource_vtables;
    resource_vtables = entry;
    return patched;
}

HRESULT wfdx_wrap_electra_placed_resource(void *object, REFIID iid,
        const D3D12_HEAP_DESC *heap_desc, UINT64 heap_offset,
        const D3D12_RESOURCE_DESC *resource_desc, D3D12_RESOURCE_STATES state)
{
    struct notifier_resource_state *resource;
    ID3D12Resource2 *resource2 = NULL;
    ID3DDestructionNotifier *native_notifier = NULL;
    void **original, **patched;
    query_interface_fn query_interface;
    HRESULT hr;

    if (!object || !public_resource_iid(iid) ||
        !electra_data_buffer_desc_matches(heap_desc, heap_offset, resource_desc, state))
        return S_FALSE;

    original = *(void ***)object;
    query_interface = (query_interface_fn)original[0];
    hr = query_interface(object, &IID_ID3DDestructionNotifier, (void **)&native_notifier);
    if (SUCCEEDED(hr))
    {
        ID3DDestructionNotifier_Release(native_notifier);
        return S_FALSE;
    }
    if (hr != E_NOINTERFACE) return hr;

    hr = query_interface(object, &IID_ID3D12Resource2, (void **)&resource2);
    if (FAILED(hr)) return S_FALSE;
    if (resource2 != object)
    {
        ID3D12Resource2_Release(resource2);
        return S_FALSE;
    }
    ((release_fn)original[2])(resource2);

    if (!(resource = calloc(1, sizeof(*resource)))) return E_OUTOFMEMORY;
    if (FAILED(hr = wfdx_notifier_registry_create(&resource->registry)))
    {
        free(resource);
        return hr;
    }
    resource->object = object;
    resource->query_interface = query_interface;
    resource->add_ref = (add_ref_fn)original[1];
    resource->release = (release_fn)original[2];
    resource->notifier.lpVtbl = &notifier_vtable;
    resource->notifier.resource = resource;

    AcquireSRWLockExclusive(&resource_lock);
    if (find_resource_locked(object))
    {
        ReleaseSRWLockExclusive(&resource_lock);
        wfdx_notifier_registry_destroy(resource->registry);
        free(resource);
        return S_OK;
    }
    if (!(patched = get_or_create_vtable(original)))
    {
        ReleaseSRWLockExclusive(&resource_lock);
        wfdx_notifier_registry_destroy(resource->registry);
        free(resource);
        return E_OUTOFMEMORY;
    }
    resource->next = notifier_resources;
    notifier_resources = resource;
    InterlockedExchangePointer((void *volatile *)object, patched);
    ReleaseSRWLockExclusive(&resource_lock);
    return S_OK;
}
