#include <windows.h>
#include <stdio.h>
#include "wfdxcompat/notifier_registry.h"

struct state { struct wfdx_notifier_registry *registry; LONG calls; HRESULT reenter_hr; };

static void STDMETHODCALLTYPE count_callback(void *data)
{
    struct state *state = data;
    InterlockedIncrement(&state->calls);
}

static void STDMETHODCALLTYPE reenter_callback(void *data)
{
    struct state *state = data;
    InterlockedIncrement(&state->calls);
    state->reenter_hr = wfdx_notifier_registry_unregister(state->registry, 1);
}

#define check(x) do { if (!(x)) { fprintf(stderr, "FAIL line %u: %s\n", __LINE__, #x); return 1; } } while (0)

int main(void)
{
    struct wfdx_notifier_registry *registry;
    struct state a = {0}, b = {0};
    UINT a_id, b_id;

    check(wfdx_notifier_registry_create(&registry) == S_OK);
    a.registry = b.registry = registry;
    check(wfdx_notifier_registry_register(registry, count_callback, &a, &a_id) == S_OK);
    check(wfdx_notifier_registry_register(registry, reenter_callback, &b, &b_id) == S_OK);
    check(a_id != b_id && a_id && b_id);
    check(wfdx_notifier_registry_unregister(registry, a_id) == S_OK);
    check(wfdx_notifier_registry_unregister(registry, a_id) == E_INVALIDARG);
    wfdx_notifier_registry_close(registry);
    check(a.calls == 0 && b.calls == 1);
    check(b.reenter_hr == WFDX_E_CLOSED);
    wfdx_notifier_registry_close(registry);
    check(b.calls == 1);
    check(wfdx_notifier_registry_register(registry, count_callback, &a, &a_id) == WFDX_E_CLOSED);
    wfdx_notifier_registry_destroy(registry);
    puts("notifier_registry: PASS");
    return 0;
}
