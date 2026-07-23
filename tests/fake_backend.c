#define COBJMACROS
#include <initguid.h>
#include <windows.h>
#include <d3d12.h>
#include <d3dcommon.h>
#include <stdlib.h>

struct fake_device { ID3D12Device10 iface; LONG refs; };
struct fake_resource
{
    ID3D12Resource2 iface;
    ID3DDestructionNotifier notifier;
    LONG refs;
    PFN_DESTRUCTION_CALLBACK callback;
    void *callback_data;
    UINT callback_id;
};

static LONG use_native_notifier;
static LONG native_registers;
static LONG resources_destroyed;
static LONG devices_destroyed;
static LONG resource_queries;

static BOOL device_iid(REFIID iid)
{
    return IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_ID3D12Object) ||
        IsEqualIID(iid, &IID_ID3D12Device) || IsEqualIID(iid, &IID_ID3D12Device1) ||
        IsEqualIID(iid, &IID_ID3D12Device2) || IsEqualIID(iid, &IID_ID3D12Device3) ||
        IsEqualIID(iid, &IID_ID3D12Device4) || IsEqualIID(iid, &IID_ID3D12Device5) ||
        IsEqualIID(iid, &IID_ID3D12Device6) || IsEqualIID(iid, &IID_ID3D12Device7) ||
        IsEqualIID(iid, &IID_ID3D12Device8) || IsEqualIID(iid, &IID_ID3D12Device9) ||
        IsEqualIID(iid, &IID_ID3D12Device10);
}

static BOOL resource_iid(REFIID iid)
{
    return IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_ID3D12Object) ||
        IsEqualIID(iid, &IID_ID3D12DeviceChild) || IsEqualIID(iid, &IID_ID3D12Pageable) ||
        IsEqualIID(iid, &IID_ID3D12Resource) || IsEqualIID(iid, &IID_ID3D12Resource1) ||
        IsEqualIID(iid, &IID_ID3D12Resource2);
}

static HRESULT STDMETHODCALLTYPE device_qi(ID3D12Device10 *iface, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (!device_iid(iid)) return E_NOINTERFACE;
    *out = iface;
    ID3D12Device10_AddRef(iface);
    return S_OK;
}

static ULONG STDMETHODCALLTYPE device_addref(ID3D12Device10 *iface)
{
    return InterlockedIncrement(&((struct fake_device *)iface)->refs);
}

static ULONG STDMETHODCALLTYPE device_release(ID3D12Device10 *iface)
{
    struct fake_device *device = (struct fake_device *)iface;
    ULONG refs = InterlockedDecrement(&device->refs);
    if (!refs)
    {
        InterlockedIncrement(&devices_destroyed);
        free(device);
    }
    return refs;
}

static HRESULT STDMETHODCALLTYPE resource_qi(ID3D12Resource2 *iface, REFIID iid, void **out)
{
    struct fake_resource *resource = (struct fake_resource *)iface;
    InterlockedIncrement(&resource_queries);
    if (!out) return E_POINTER;
    *out = NULL;
    if (resource_iid(iid)) *out = iface;
    else if (IsEqualIID(iid, &IID_ID3DDestructionNotifier) && use_native_notifier)
        *out = &resource->notifier;
    else return E_NOINTERFACE;
    ID3D12Resource2_AddRef(iface);
    return S_OK;
}

static ULONG STDMETHODCALLTYPE resource_addref(ID3D12Resource2 *iface)
{
    return InterlockedIncrement(&((struct fake_resource *)iface)->refs);
}

static ULONG STDMETHODCALLTYPE resource_release(ID3D12Resource2 *iface)
{
    struct fake_resource *resource = (struct fake_resource *)iface;
    ULONG refs = InterlockedDecrement(&resource->refs);
    if (!refs)
    {
        PFN_DESTRUCTION_CALLBACK callback = resource->callback;
        void *data = resource->callback_data;
        InterlockedIncrement(&resources_destroyed);
        if (callback) callback(data);
        free(resource);
    }
    return refs;
}

static struct fake_resource *resource_from_notifier(ID3DDestructionNotifier *iface)
{
    return CONTAINING_RECORD(iface, struct fake_resource, notifier);
}

static HRESULT STDMETHODCALLTYPE native_qi(ID3DDestructionNotifier *iface, REFIID iid, void **out)
{
    return resource_qi(&resource_from_notifier(iface)->iface, iid, out);
}
static ULONG STDMETHODCALLTYPE native_addref(ID3DDestructionNotifier *iface)
{
    return resource_addref(&resource_from_notifier(iface)->iface);
}
static ULONG STDMETHODCALLTYPE native_release(ID3DDestructionNotifier *iface)
{
    return resource_release(&resource_from_notifier(iface)->iface);
}
static HRESULT STDMETHODCALLTYPE native_register(ID3DDestructionNotifier *iface,
        PFN_DESTRUCTION_CALLBACK callback, void *data, UINT *id)
{
    struct fake_resource *resource = resource_from_notifier(iface);
    if (!callback || !id) return E_INVALIDARG;
    resource->callback = callback;
    resource->callback_data = data;
    resource->callback_id = 77;
    *id = 77;
    InterlockedIncrement(&native_registers);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE native_unregister(ID3DDestructionNotifier *iface, UINT id)
{
    struct fake_resource *resource = resource_from_notifier(iface);
    if (id != resource->callback_id) return E_INVALIDARG;
    resource->callback = NULL;
    resource->callback_data = NULL;
    resource->callback_id = 0;
    return S_OK;
}

static ID3DDestructionNotifierVtbl native_vtable =
{
    native_qi, native_addref, native_release, native_register, native_unregister,
};

static ID3D12Resource2Vtbl resource_vtable =
{
    .QueryInterface = resource_qi,
    .AddRef = resource_addref,
    .Release = resource_release,
};

static HRESULT make_resource(REFIID iid, void **out)
{
    struct fake_resource *resource;
    HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!(resource = calloc(1, sizeof(*resource)))) return E_OUTOFMEMORY;
    resource->iface.lpVtbl = &resource_vtable;
    resource->notifier.lpVtbl = &native_vtable;
    resource->refs = 1;
    hr = resource_qi(&resource->iface, iid, out);
    resource_release(&resource->iface);
    return hr;
}

static HRESULT STDMETHODCALLTYPE create_committed(ID3D12Device10 *iface,
        const D3D12_HEAP_PROPERTIES *p, D3D12_HEAP_FLAGS f, const D3D12_RESOURCE_DESC *d,
        D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE *c, REFIID iid, void **out)
{ (void)iface; (void)p; (void)f; (void)d; (void)s; (void)c; return make_resource(iid, out); }
static D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE get_resource_allocation_info(
        ID3D12Device10 *iface, D3D12_RESOURCE_ALLOCATION_INFO *info, UINT visible_mask,
        UINT desc_count, const D3D12_RESOURCE_DESC *descs)
{
    UINT64 bytes;
    (void)iface;
    (void)visible_mask;
    if (!info || desc_count != 1 || !descs) return info;
    bytes = descs->Width * descs->Height * 4;
    info->Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    info->SizeInBytes = (bytes + info->Alignment - 1) & ~(info->Alignment - 1);
    return info;
}
static HRESULT STDMETHODCALLTYPE create_placed(ID3D12Device10 *iface, ID3D12Heap *h, UINT64 o,
        const D3D12_RESOURCE_DESC *d, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE *c,
        REFIID iid, void **out)
{ (void)iface; (void)h; (void)o; (void)d; (void)s; (void)c; return make_resource(iid, out); }
static HRESULT STDMETHODCALLTYPE create_reserved(ID3D12Device10 *iface,
        const D3D12_RESOURCE_DESC *d, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE *c,
        REFIID iid, void **out)
{ (void)iface; (void)d; (void)s; (void)c; return make_resource(iid, out); }
static HRESULT STDMETHODCALLTYPE open_shared(ID3D12Device10 *iface, HANDLE handle,
        REFIID iid, void **out)
{ (void)iface; (void)handle; return make_resource(iid, out); }
static HRESULT STDMETHODCALLTYPE create_committed1(ID3D12Device10 *iface,
        const D3D12_HEAP_PROPERTIES *p, D3D12_HEAP_FLAGS f, const D3D12_RESOURCE_DESC *d,
        D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE *c, ID3D12ProtectedResourceSession *ps,
        REFIID iid, void **out)
{ (void)iface; (void)p; (void)f; (void)d; (void)s; (void)c; (void)ps; return make_resource(iid, out); }
static HRESULT STDMETHODCALLTYPE create_reserved1(ID3D12Device10 *iface,
        const D3D12_RESOURCE_DESC *d, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE *c,
        ID3D12ProtectedResourceSession *ps, REFIID iid, void **out)
{ (void)iface; (void)d; (void)s; (void)c; (void)ps; return make_resource(iid, out); }
static HRESULT STDMETHODCALLTYPE create_committed2(ID3D12Device10 *iface,
        const D3D12_HEAP_PROPERTIES *p, D3D12_HEAP_FLAGS f, const D3D12_RESOURCE_DESC1 *d,
        D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE *c, ID3D12ProtectedResourceSession *ps,
        REFIID iid, void **out)
{ (void)iface; (void)p; (void)f; (void)d; (void)s; (void)c; (void)ps; return make_resource(iid, out); }
static HRESULT STDMETHODCALLTYPE create_placed1(ID3D12Device10 *iface, ID3D12Heap *h, UINT64 o,
        const D3D12_RESOURCE_DESC1 *d, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE *c,
        REFIID iid, void **out)
{ (void)iface; (void)h; (void)o; (void)d; (void)s; (void)c; return make_resource(iid, out); }
static HRESULT STDMETHODCALLTYPE create_committed3(ID3D12Device10 *iface,
        const D3D12_HEAP_PROPERTIES *p, D3D12_HEAP_FLAGS f, const D3D12_RESOURCE_DESC1 *d,
        D3D12_BARRIER_LAYOUT l, const D3D12_CLEAR_VALUE *c, ID3D12ProtectedResourceSession *ps,
        UINT32 n, DXGI_FORMAT *formats, REFIID iid, void **out)
{ (void)iface; (void)p; (void)f; (void)d; (void)l; (void)c; (void)ps; (void)n; (void)formats; return make_resource(iid, out); }
static HRESULT STDMETHODCALLTYPE create_placed2(ID3D12Device10 *iface, ID3D12Heap *h, UINT64 o,
        const D3D12_RESOURCE_DESC1 *d, D3D12_BARRIER_LAYOUT l, const D3D12_CLEAR_VALUE *c,
        UINT32 n, DXGI_FORMAT *formats, REFIID iid, void **out)
{ (void)iface; (void)h; (void)o; (void)d; (void)l; (void)c; (void)n; (void)formats; return make_resource(iid, out); }
static HRESULT STDMETHODCALLTYPE create_reserved2(ID3D12Device10 *iface,
        const D3D12_RESOURCE_DESC *d, D3D12_BARRIER_LAYOUT l, const D3D12_CLEAR_VALUE *c,
        ID3D12ProtectedResourceSession *ps, UINT32 n, DXGI_FORMAT *formats, REFIID iid, void **out)
{ (void)iface; (void)d; (void)l; (void)c; (void)ps; (void)n; (void)formats; return make_resource(iid, out); }

static ID3D12Device10Vtbl device_vtable =
{
    .QueryInterface = device_qi,
    .AddRef = device_addref,
    .Release = device_release,
    .CreateCommittedResource = create_committed,
    .GetResourceAllocationInfo = get_resource_allocation_info,
    .CreatePlacedResource = create_placed,
    .CreateReservedResource = create_reserved,
    .OpenSharedHandle = open_shared,
    .CreateCommittedResource1 = create_committed1,
    .CreateReservedResource1 = create_reserved1,
    .CreateCommittedResource2 = create_committed2,
    .CreatePlacedResource1 = create_placed1,
    .CreateCommittedResource3 = create_committed3,
    .CreatePlacedResource2 = create_placed2,
    .CreateReservedResource2 = create_reserved2,
};

HRESULT WINAPI D3D12CreateDevice(IUnknown *adapter,
        D3D_FEATURE_LEVEL level, REFIID iid, void **out)
{
    struct fake_device *device;
    HRESULT hr;
    (void)adapter; (void)level;
    if (!out) return E_POINTER;
    if (!(device = calloc(1, sizeof(*device)))) return E_OUTOFMEMORY;
    device->iface.lpVtbl = &device_vtable;
    device->refs = 1;
    hr = device_qi(&device->iface, iid, out);
    device_release(&device->iface);
    return hr;
}

__declspec(dllexport) void WINAPI fake_set_native_notifier(BOOL enable) { use_native_notifier = enable; }
__declspec(dllexport) LONG WINAPI fake_get_native_registers(void) { return native_registers; }
__declspec(dllexport) LONG WINAPI fake_get_resource_destroyed(void) { return resources_destroyed; }
__declspec(dllexport) LONG WINAPI fake_get_device_destroyed(void) { return devices_destroyed; }
__declspec(dllexport) LONG WINAPI fake_get_resource_queries(void) { return resource_queries; }
