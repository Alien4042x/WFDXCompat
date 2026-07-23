#ifndef WFDXCOMPAT_NOTIFIER_REGISTRY_H
#define WFDXCOMPAT_NOTIFIER_REGISTRY_H

#include <d3dcommon.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wfdx_notifier_registry;

#define WFDX_E_CLOSED HRESULT_FROM_WIN32(ERROR_INVALID_STATE)

HRESULT wfdx_notifier_registry_create(struct wfdx_notifier_registry **registry);
HRESULT wfdx_notifier_registry_register(struct wfdx_notifier_registry *registry,
        PFN_DESTRUCTION_CALLBACK callback, void *data, UINT *callback_id);
HRESULT wfdx_notifier_registry_unregister(struct wfdx_notifier_registry *registry,
        UINT callback_id);

/*
 * Atomically closes the registry and invokes every callback which won the
 * race with unregister exactly once. Calls made after close return WFDX_E_CLOSED.
 * The caller must keep the registry allocated until close returns.
 */
void wfdx_notifier_registry_close(struct wfdx_notifier_registry *registry);
void wfdx_notifier_registry_destroy(struct wfdx_notifier_registry *registry);

#ifdef __cplusplus
}
#endif

#endif
