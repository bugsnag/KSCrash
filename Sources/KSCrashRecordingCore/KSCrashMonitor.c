//
//  KSCrashMonitor.c
//
//  Created by Karl Stenerud on 2012-02-12.
//
//  Copyright (c) 2012 Karl Stenerud. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall remain in place
// in this source code.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "KSCrashMonitor.h"

#include <memory.h>
#include <os/lock.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "KSCrashMonitorContext.h"
#include "KSCrashMonitorHelper.h"
#include "KSDebug.h"
#include "KSID.h"
#include "KSString.h"
#include "KSSystemCapabilities.h"
#include "KSThread.h"

// #define KSLogger_LocalLevel TRACE
#include "KSLogger.h"

typedef struct {
    KSCrashMonitorAPI **apis;  // Array of MonitorAPIs
    size_t count;
    size_t capacity;
} MonitorList;

#define INITIAL_MONITOR_CAPACITY 15

#pragma mark - Helpers

__attribute__((unused))  // Suppress unused function warnings, especially in release builds.
static inline const char *
getMonitorNameForLogging(const KSCrashMonitorAPI *api)
{
    return api->monitorId() ?: "Unknown";
}

// ============================================================================
#pragma mark - Globals -
// ============================================================================

static MonitorList g_monitors = {};
static os_unfair_lock g_monitorsLock = OS_UNFAIR_LOCK_INIT;

static _Atomic(bool) g_areMonitorsInitialized = false;
static bool g_crashedDuringExceptionHandling = false;
static KSCrash_ExceptionHandlingPolicy g_currentPolicy;

static char g_eventIds[2][40];
static const size_t g_eventIdCount = sizeof(g_eventIds) / sizeof(*g_eventIds);
static size_t g_eventIdIdx = 0;

static void (*g_onExceptionEvent)(struct KSCrash_MonitorContext *monitorContext);

// ============================================================================
#pragma mark - Internal -
// ============================================================================

static void addMonitor(MonitorList *list, KSCrashMonitorAPI *api)
{
    if (list->count >= list->capacity) {
        list->capacity = list->capacity > 0 ? list->capacity * 2 : 2;
        list->apis = (KSCrashMonitorAPI **)realloc(list->apis, list->capacity * sizeof(KSCrashMonitorAPI *));
    }
    list->apis[list->count++] = api;
}

static void removeMonitor(MonitorList *list, const KSCrashMonitorAPI *api)
{
    if (list == NULL || api == NULL) {
        KSLOG_DEBUG("Either list or func is NULL. Removal operation aborted.");
        return;
    }

    bool found = false;

    for (size_t i = 0; i < list->count; i++) {
        if (list->apis[i] == api) {
            found = true;

            list->apis[i]->setEnabled(false);

            // Replace the current monitor with the last monitor in the list
            list->apis[i] = list->apis[list->count - 1];
            list->count--;
            list->apis[list->count] = NULL;

            KSLOG_DEBUG("Monitor %s removed from the list.", getMonitorNameForLogging(api));
            break;
        }
    }

    if (!found) {
        KSLOG_DEBUG("Monitor %s not found in the list. No removal performed.", getMonitorNameForLogging(api));
    }
}

static void freeMonitorFuncList(MonitorList *list)
{
    free(list->apis);
    list->apis = NULL;
    list->count = 0;
    list->capacity = 0;

    g_areMonitorsInitialized = false;
}

static void regenerateEventIds(void)
{
    for (size_t i = 0; i < g_eventIdCount; i++) {
        ksid_generate(g_eventIds[i]);
    }
    g_eventIdIdx = 0;
}

static void init(void)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong(&g_areMonitorsInitialized, &expected, true)) {
        return;
    }

    MonitorList *list = &g_monitors;
    list->count = 0;
    list->capacity = INITIAL_MONITOR_CAPACITY;
    list->apis = (KSCrashMonitorAPI **)malloc(list->capacity * sizeof(KSCrashMonitorAPI *));
}

__attribute__((unused)) // For tests. Declared as extern in TestCase
void kscm_resetState(void)
{
    os_unfair_lock_lock(&g_monitorsLock);
    freeMonitorFuncList(&g_monitors);
    os_unfair_lock_unlock(&g_monitorsLock);

    memset(&g_currentPolicy, 0, sizeof(g_currentPolicy));
    g_crashedDuringExceptionHandling = false;
    g_onExceptionEvent = NULL;
    regenerateEventIds();
}

// ============================================================================
#pragma mark - API -
// ============================================================================

void kscm_setEventCallback(void (*onEvent)(struct KSCrash_MonitorContext *monitorContext))
{
    init();
    g_onExceptionEvent = onEvent;
}

bool kscm_activateMonitors(void)
{
    init();
    // Check for debugger and async safety
    bool isDebuggerUnsafe = ksdebug_isBeingTraced();
    bool isAsyncSafeRequired = g_currentPolicy.asyncSafety;

    if (isDebuggerUnsafe) {
        static bool hasWarned = false;
        if (!hasWarned) {
            hasWarned = true;
            KSLOGBASIC_WARN("    ************************ Crash Handler Notice ************************");
            KSLOGBASIC_WARN("    *     App is running in a debugger. Masking out unsafe monitors.     *");
            KSLOGBASIC_WARN("    * This means that most crashes WILL NOT BE RECORDED while debugging! *");
            KSLOGBASIC_WARN("    **********************************************************************");
        }
    }

    if (isAsyncSafeRequired) {
        KSLOG_DEBUG("Async-safe environment detected. Masking out unsafe monitors.");
    }

    regenerateEventIds();

    os_unfair_lock_lock(&g_monitorsLock);

    regenerateEventIds();

    // Enable or disable monitors
    for (size_t i = 0; i < g_monitors.count; i++) {
        KSCrashMonitorAPI *api = g_monitors.apis[i];
        KSCrashMonitorFlag flags = api->monitorFlags();
        bool shouldEnable = true;

        if (isDebuggerUnsafe && (flags & KSCrashMonitorFlagDebuggerUnsafe)) {
            shouldEnable = false;
        }

        if (isAsyncSafeRequired && !(flags & KSCrashMonitorFlagAsyncSafe)) {
            shouldEnable = false;
        }

        api->setEnabled(shouldEnable);
    }

    bool anyMonitorActive = false;

    // Create a copy of enabled monitors to avoid holding the lock during notification
    size_t enabledCount = 0;
    KSCrashMonitorAPI **enabledMonitors = NULL;
    size_t monitorsCount = g_monitors.count;

    if (monitorsCount > 0) {
        enabledMonitors = (KSCrashMonitorAPI **)malloc(monitorsCount * sizeof(KSCrashMonitorAPI *));
        if (enabledMonitors == NULL) {
            KSLOG_ERROR("Failed to allocate memory for enabled monitors.");
        }
    }

    KSLOG_DEBUG("Active monitors are now:");
    for (size_t i = 0; i < g_monitors.count; i++) {
        KSCrashMonitorAPI *api = g_monitors.apis[i];
        if (api->isEnabled()) {
            KSLOG_DEBUG("Monitor %s is enabled.", getMonitorNameForLogging(api));
            if (enabledMonitors != NULL) {
                enabledMonitors[enabledCount++] = api;
            }
            anyMonitorActive = true;
        } else {
            KSLOG_DEBUG("Monitor %s is disabled.", getMonitorNameForLogging(api));
        }
    }

    // Release the lock before calling notifyPostSystemEnable
    os_unfair_lock_unlock(&g_monitorsLock);

    // Notify monitors about system enable without holding the lock
    for (size_t i = 0; i < enabledCount; i++) {
        enabledMonitors[i]->notifyPostSystemEnable();
    }

    if (enabledMonitors != NULL) {
        free(enabledMonitors);
    }

    return anyMonitorActive;
}

void kscm_disableAllMonitors(void)
{
    os_unfair_lock_lock(&g_monitorsLock);
    for (size_t i = 0; i < g_monitors.count; i++) {
        KSCrashMonitorAPI *api = g_monitors.apis[i];
        api->setEnabled(false);
    }
    os_unfair_lock_unlock(&g_monitorsLock);
    KSLOG_DEBUG("All monitors have been disabled.");
}

static bool notifyException(KSCrash_ExceptionHandlingPolicy recommendations)
{
    g_currentPolicy.asyncSafety |= recommendations.asyncSafety;  // Don't let it be unset.
    if (!recommendations.isFatal) {
        return false;
    }

    if (g_currentPolicy.isFatal) {
        g_crashedDuringExceptionHandling = true;
    }
    g_currentPolicy.isFatal = true;
    if (g_crashedDuringExceptionHandling) {
        KSLOG_INFO("Detected crash in the crash reporter. Uninstalling KSCrash.");
        kscm_disableAllMonitors();
    }
    return g_crashedDuringExceptionHandling;
}

static void handleException(struct KSCrash_MonitorContext *context)
{
    context->handlingCrash |= g_currentPolicy.isFatal;

    context->requiresAsyncSafety = g_currentPolicy.asyncSafety;
    if (g_crashedDuringExceptionHandling) {
        context->crashedDuringCrashHandling = true;
    }

    // If the crash happened during monitor registration, skip handling
    if (os_unfair_lock_trylock(&g_monitorsLock) == false) {
        KSLOG_ERROR("Unable to acquire lock for monitor list. Skipping exception handling.");
        return;
    }

    if (!g_currentPolicy.asyncSafety) {
        // If we don't need async-safety (NSException, user exception), then this is safe to call.
        ksid_generate(context->eventID);
    } else {
        // Otherwise use the pre-built primary or secondary event ID. We won't ever use
        // more than two (crash, recrash) because the app will terminate afterwards.
        if (g_eventIdIdx >= g_eventIdCount) {
            // Very unlikely, but if this happens, we're stuck in a handler loop.
            KSLOG_ERROR(
                "Requesting a pre-built event ID, but we've already used both up! Aborting exception handling.");
            return;
        }
        memcpy(context->eventID, g_eventIds[g_eventIdIdx++], sizeof(context->eventID));
    }

    // Add contextual info to the event for all enabled monitors
    for (size_t i = 0; i < g_monitors.count; i++) {
        KSCrashMonitorAPI *api = g_monitors.apis[i];
        if (api->isEnabled()) {
            api->addContextualInfoToEvent(context);
        }
    }

    os_unfair_lock_unlock(&g_monitorsLock);

    // Call the exception event handler if it exists
    if (g_onExceptionEvent) {
        g_onExceptionEvent(context);
    }

    // Restore original handlers if the exception is fatal and not already handled
    if (g_currentPolicy.isFatal && !g_crashedDuringExceptionHandling) {
        KSLOG_DEBUG("Exception is fatal. Restoring original handlers.");
        kscm_disableAllMonitors();
    }

    // Done handling the crash
    context->handlingCrash = false;
}

static KSCrash_ExceptionHandlerCallbacks g_exceptionCallbacks = {
    .notify = notifyException,
    .handle = handleException,
};

bool kscm_addMonitor(KSCrashMonitorAPI *api)
{
    init();
    if (api == NULL) {
        KSLOG_ERROR("Attempted to add a NULL monitor. Operation aborted.");
        return false;
    }

    const char *newMonitorId = api->monitorId();
    if (newMonitorId == NULL) {
        KSLOG_ERROR("Monitor has a NULL ID. Operation aborted.");
        return false;
    }

    os_unfair_lock_lock(&g_monitorsLock);

    // Check for duplicate monitors
    for (size_t i = 0; i < g_monitors.count; i++) {
        KSCrashMonitorAPI *existingApi = g_monitors.apis[i];
        const char *existingMonitorId = existingApi->monitorId();

        if (ksstring_safeStrcmp(existingMonitorId, newMonitorId) == 0) {
            KSLOG_DEBUG("Monitor %s already exists. Skipping addition.", getMonitorNameForLogging(api));
            os_unfair_lock_unlock(&g_monitorsLock);
            return false;
        }
    }

    api->init(&g_exceptionCallbacks);
    addMonitor(&g_monitors, api);
    KSLOG_DEBUG("Monitor %s injected.", getMonitorNameForLogging(api));

    os_unfair_lock_unlock(&g_monitorsLock);
    return true;
}

void kscm_removeMonitor(const KSCrashMonitorAPI *api)
{
    if (api == NULL) {
        KSLOG_DEBUG("Attempted to remove a NULL monitor. Operation aborted.");
        return;
    }

    os_unfair_lock_lock(&g_monitorsLock);

    removeMonitor(&g_monitors, api);

    os_unfair_lock_unlock(&g_monitorsLock);
}

// ============================================================================
#pragma mark - Private API -
// ============================================================================

void kscm_regenerateEventIDs(void)
{
    os_unfair_lock_lock(&g_monitorsLock);
    regenerateEventIds();
    os_unfair_lock_unlock(&g_monitorsLock);
}

void kscm_clearAsyncSafetyState(void) { g_currentPolicy.asyncSafety = false; }
