#ifndef WFDXCOMPAT_RESOURCE_NOTIFIER_H
#define WFDXCOMPAT_RESOURCE_NOTIFIER_H

#include <d3d12.h>

HRESULT wfdx_wrap_electra_placed_resource(void *resource, REFIID iid,
        const D3D12_HEAP_DESC *heap_desc, UINT64 heap_offset,
        const D3D12_RESOURCE_DESC *resource_desc, D3D12_RESOURCE_STATES state);

#endif
