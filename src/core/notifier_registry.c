#include <windows.h>
#include <stdlib.h>
#include "wfdxcompat/notifier_registry.h"

struct callback_entry
{
    struct callback_entry *next;
    PFN_DESTRUCTION_CALLBACK callback;
    void *data;
    UINT id;
};

struct wfdx_notifier_registry
{
    SRWLOCK lock;
    struct callback_entry *callbacks;
    UINT next_id;
    BOOL closed;
};

HRESULT wfdx_notifier_registry_create(struct wfdx_notifier_registry **out)
{
    struct wfdx_notifier_registry *registry;

    if (!out) return E_POINTER;
    *out = NULL;
    if (!(registry = calloc(1, sizeof(*registry)))) return E_OUTOFMEMORY;
    InitializeSRWLock(&registry->lock);
    registry->next_id = 1;
    *out = registry;
    return S_OK;
}

HRESULT wfdx_notifier_registry_register(struct wfdx_notifier_registry *registry,
        PFN_DESTRUCTION_CALLBACK callback, void *data, UINT *callback_id)
{
    struct callback_entry *entry;

    if (!registry || !callback || !callback_id) return E_INVALIDARG;
    *callback_id = 0;
    if (!(entry = malloc(sizeof(*entry)))) return E_OUTOFMEMORY;
    entry->callback = callback;
    entry->data = data;

    AcquireSRWLockExclusive(&registry->lock);
    if (registry->closed)
    {
        ReleaseSRWLockExclusive(&registry->lock);
        free(entry);
        return WFDX_E_CLOSED;
    }
    do
    {
        entry->id = registry->next_id++;
        if (!registry->next_id) registry->next_id = 1;
    } while (!entry->id);
    entry->next = registry->callbacks;
    registry->callbacks = entry;
    *callback_id = entry->id;
    ReleaseSRWLockExclusive(&registry->lock);
    return S_OK;
}

HRESULT wfdx_notifier_registry_unregister(struct wfdx_notifier_registry *registry,
        UINT callback_id)
{
    struct callback_entry **cursor, *entry;

    if (!registry || !callback_id) return E_INVALIDARG;
    AcquireSRWLockExclusive(&registry->lock);
    if (registry->closed)
    {
        ReleaseSRWLockExclusive(&registry->lock);
        return WFDX_E_CLOSED;
    }
    for (cursor = &registry->callbacks; (entry = *cursor); cursor = &entry->next)
    {
        if (entry->id == callback_id)
        {
            *cursor = entry->next;
            ReleaseSRWLockExclusive(&registry->lock);
            free(entry);
            return S_OK;
        }
    }
    ReleaseSRWLockExclusive(&registry->lock);
    return E_INVALIDARG;
}

void wfdx_notifier_registry_close(struct wfdx_notifier_registry *registry)
{
    struct callback_entry *callbacks, *entry;

    if (!registry) return;
    AcquireSRWLockExclusive(&registry->lock);
    if (registry->closed)
    {
        ReleaseSRWLockExclusive(&registry->lock);
        return;
    }
    registry->closed = TRUE;
    callbacks = registry->callbacks;
    registry->callbacks = NULL;
    ReleaseSRWLockExclusive(&registry->lock);

    /* No lock is held while user code runs: callbacks may safely re-enter. */
    while ((entry = callbacks))
    {
        callbacks = entry->next;
        entry->callback(entry->data);
        free(entry);
    }
}

void wfdx_notifier_registry_destroy(struct wfdx_notifier_registry *registry)
{
    if (!registry) return;
    wfdx_notifier_registry_close(registry);
    free(registry);
}
