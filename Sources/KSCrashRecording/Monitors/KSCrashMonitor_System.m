//
//  KSCrashMonitor_System.m
//
//  Created by Karl Stenerud on 2012-02-05.
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

#import "KSCrashMonitor_System.h"

#import "KSBinaryImageCache.h"
#import "KSCPU.h"
#import "KSCrashMonitorContext.h"
#include "KSCrashMonitorHelper.h"
#import "KSDate.h"
#import "KSDynamicLinker.h"
#import "KSSysCtl.h"
#import "KSSystemCapabilities.h"

// #define KSLogger_LocalLevel TRACE
#import "KSLogger.h"

#import <CommonCrypto/CommonDigest.h>
#import <Foundation/Foundation.h>
#if KSCRASH_HAS_UIKIT
#import <UIKit/UIKit.h>
#endif
#include <mach-o/dyld.h>
#include <mach/mach.h>

typedef struct {
    const char *systemName;
    const char *systemVersion;
    const char *machine;
    const char *model;
    const char *kernelVersion;
    const char *osVersion;
    bool isJailbroken;
    const char *appStartTime;
    const char *executablePath;
    const char *executableName;
    const char *bundleID;
    const char *bundleName;
    const char *bundleVersion;
    const char *bundleShortVersion;
    const char *appID;
    const char *cpuArchitecture;
    int cpuType;
    int cpuSubType;
    int binaryCPUType;
    int binaryCPUSubType;
    const char *timezone;
    const char *processName;
    int processID;
    int parentProcessID;
    const char *deviceAppHash;
    const char *buildType;
    uint64_t memorySize;
} SystemData;

static SystemData g_systemData;

static volatile bool g_isEnabled = false;

// ============================================================================
#pragma mark - Utility -
// ============================================================================

static const char *cString(NSString *str) { return str == NULL ? NULL : strdup(str.UTF8String); }

static NSString *nsstringSysctl(NSString *name)
{
    NSString *str = nil;
    int size = (int)kssysctl_stringForName(name.UTF8String, NULL, 0);

    if (size <= 0) {
        return @"";
    }

    NSMutableData *value = [NSMutableData dataWithLength:(unsigned)size];

    if (kssysctl_stringForName(name.UTF8String, value.mutableBytes, size) != 0) {
        str = [NSString stringWithCString:value.mutableBytes encoding:NSUTF8StringEncoding];
    }

    return str;
}

/** Get a sysctl value as a null terminated string.
 *
 * @param name The sysctl name.
 *
 * @return The result of the sysctl call.
 */
static const char *stringSysctl(const char *name)
{
    int size = (int)kssysctl_stringForName(name, NULL, 0);
    if (size <= 0) {
        return NULL;
    }

    char *value = malloc((size_t)size);
    if (kssysctl_stringForName(name, value, size) <= 0) {
        free(value);
        return NULL;
    }

    return value;
}

static const char *dateString(time_t date)
{
    char *buffer = malloc(21);
    ksdate_utcStringFromTimestamp(date, buffer);
    return buffer;
}

/** Get the current VM stats.
 *
 * @param vmStats Gets filled with the VM stats.
 *
 * @param pageSize gets filled with the page size.
 *
 * @return true if the operation was successful.
 */
static bool VMStats(vm_statistics_data_t *const vmStats, vm_size_t *const pageSize)
{
    kern_return_t kr;
    const mach_port_t hostPort = mach_host_self();

    if ((kr = host_page_size(hostPort, pageSize)) != KERN_SUCCESS) {
        KSLOG_ERROR(@"host_page_size: %s", mach_error_string(kr));
        return false;
    }

    mach_msg_type_number_t hostSize = sizeof(*vmStats) / sizeof(natural_t);
    kr = host_statistics(hostPort, HOST_VM_INFO, (host_info_t)vmStats, &hostSize);
    if (kr != KERN_SUCCESS) {
        KSLOG_ERROR(@"host_statistics: %s", mach_error_string(kr));
        return false;
    }

    return true;
}

static uint64_t freeMemory(void)
{
    vm_statistics_data_t vmStats;
    vm_size_t pageSize;
    if (VMStats(&vmStats, &pageSize)) {
        return ((uint64_t)pageSize) * vmStats.free_count;
    }
    return 0;
}

static uint64_t usableMemory(void)
{
    vm_statistics_data_t vmStats;
    vm_size_t pageSize;
    if (VMStats(&vmStats, &pageSize)) {
        return ((uint64_t)pageSize) *
               (vmStats.active_count + vmStats.inactive_count + vmStats.wire_count + vmStats.free_count);
    }
    return 0;
}

/** Convert raw UUID bytes to a human-readable string.
 *
 * @param uuidBytes The UUID bytes (must be 16 bytes long).
 *
 * @return The human readable form of the UUID.
 */
static const char *uuidBytesToString(const uint8_t *uuidBytes)
{
    CFUUIDRef uuidRef = CFUUIDCreateFromUUIDBytes(NULL, *((CFUUIDBytes *)uuidBytes));
    NSString *str = (__bridge_transfer NSString *)CFUUIDCreateString(NULL, uuidRef);
    CFRelease(uuidRef);

    return cString(str);
}

/** Get this application's executable path.
 *
 * @return Executable path.
 */
static NSString *getExecutablePath(void)
{
    NSBundle *mainBundle = [NSBundle mainBundle];
    NSDictionary *infoDict = [mainBundle infoDictionary];
    NSString *bundlePath = [mainBundle bundlePath];
    NSString *executableName = infoDict[@"CFBundleExecutable"];
    return [bundlePath stringByAppendingPathComponent:executableName];
}

/** Get this application's UUID.
 *
 * @return The UUID.
 */
static const char *getAppUUID(void)
{
    uint32_t count = 0;
    const ks_dyld_image_info *images = ksbic_getImages(&count);
    if (!images || count == 0) {
        return NULL;
    }
    const struct mach_header *header = images[0].imageLoadAddress;

    KSBinaryImage binary = { 0 };
    if (ksdl_binaryImageForHeader(header, NULL, &binary)) {
        if (binary.uuid) {
            return uuidBytesToString(binary.uuid);
        }
    }
    return NULL;
}

/** Get the current CPU's architecture.
 *
 * @return The current CPU archutecture.
 */
static const char *getCPUArchForCPUType(cpu_type_t cpuType, cpu_subtype_t subType)
{
    switch (cpuType) {
        case CPU_TYPE_ARM: {
            switch (subType) {
                case CPU_SUBTYPE_ARM_V6:
                    return "armv6";
                case CPU_SUBTYPE_ARM_V7:
                    return "armv7";
                case CPU_SUBTYPE_ARM_V7F:
                    return "armv7f";
                case CPU_SUBTYPE_ARM_V7K:
                    return "armv7k";
#ifdef CPU_SUBTYPE_ARM_V7S
                case CPU_SUBTYPE_ARM_V7S:
                    return "armv7s";
#endif
                default:
                    return "arm";
            }
        }
        case CPU_TYPE_ARM64: {
            switch (subType) {
                case CPU_SUBTYPE_ARM64E:
                    return "arm64e";
                default:
                    return "arm64";
            }
        }
        case CPU_TYPE_X86:
            return "x86";
        case CPU_TYPE_X86_64:
            return "x86_64";
        default:
            return NULL;
    }
}

static const char *getCurrentCPUArch(void)
{
    const char *result =
        getCPUArchForCPUType(kssysctl_int32ForName("hw.cputype"), kssysctl_int32ForName("hw.cpusubtype"));

    if (result == NULL) {
        result = kscpu_currentArch();
    }
    return result;
}

/** Check if the current device is jailbroken.
 *
 * @return YES if the device is jailbroken.
 */
static bool isJailbroken(void)
{
    static bool sJailbroken;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        const char *path = "/private/kscrash_jailbreak_test";
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            sJailbroken = false;
        } else {
            sJailbroken = true;
            unlink(path);
        }
    });
    return sJailbroken;
}

/** Check if the current build is a debug build.
 *
 * @return YES if the app was built in debug mode.
 */
static bool isDebugBuild(void)
{
#ifdef DEBUG
    return YES;
#else
    return NO;
#endif
}

/** Check if this code is built for the simulator.
 *
 * @return YES if this is a simulator build.
 */
static bool isSimulatorBuild(void)
{
#if TARGET_OS_SIMULATOR
    return YES;
#else
    return NO;
#endif
}

/** The file path for the bundle’s App Store receipt.
 *
 * @return App Store receipt for iOS 7+, nil otherwise.
 */
static NSString *getReceiptUrlPath(void)
{
    NSString *path = nil;
#if KSCRASH_HOST_IOS
    path = [NSBundle mainBundle].appStoreReceiptURL.path;
#endif
    return path;
}

/** Generate a 20 byte SHA1 hash that remains unique across a single device and
 * application. This is slightly different from the Apple crash report key,
 * which is unique to the device, regardless of the application.
 *
 * @return The stringified hex representation of the hash for this device + app.
 */
static const char *getDeviceAndAppHash(void)
{
    NSMutableData *data = nil;

#if KSCRASH_HAS_UIDEVICE
    if ([[UIDevice currentDevice] respondsToSelector:@selector(identifierForVendor)]) {
        data = [NSMutableData dataWithLength:16];
        [[UIDevice currentDevice].identifierForVendor getUUIDBytes:data.mutableBytes];
    } else
#endif
    {
        data = [NSMutableData dataWithLength:6];
        kssysctl_getMacAddress("en0", [data mutableBytes]);
    }

    // Append some device-specific data.
    [data appendData:(NSData *_Nonnull)[nsstringSysctl(@"hw.machine") dataUsingEncoding:NSUTF8StringEncoding]];
    [data appendData:(NSData *_Nonnull)[nsstringSysctl(@"hw.model") dataUsingEncoding:NSUTF8StringEncoding]];
    const char *cpuArch = getCurrentCPUArch();
    [data appendBytes:cpuArch length:strlen(cpuArch)];

    // Append the bundle ID.
    NSData *bundleID = [[[NSBundle mainBundle] bundleIdentifier] dataUsingEncoding:NSUTF8StringEncoding];
    if (bundleID != nil) {
        [data appendData:bundleID];
    }

    // SHA the whole thing.
    uint8_t sha[CC_SHA1_DIGEST_LENGTH];
    CC_SHA1([data bytes], (CC_LONG)[data length], sha);

    NSMutableString *hash = [NSMutableString string];
    for (unsigned i = 0; i < sizeof(sha); i++) {
        [hash appendFormat:@"%02x", sha[i]];
    }

    return cString(hash);
}

/** Check if the current build is a "testing" build.
 * This is useful for checking if the app was released through Testflight.
 *
 * @return YES if this is a testing build.
 */
static bool isTestBuild(void) { return [getReceiptUrlPath().lastPathComponent isEqualToString:@"sandboxReceipt"]; }

/** Check if the app has an app store receipt.
 * Only apps released through the app store will have a receipt.
 *
 * @return YES if there is an app store receipt.
 */
static bool hasAppStoreReceipt(void)
{
    NSString *receiptPath = getReceiptUrlPath();
    if (receiptPath == nil) {
        return NO;
    }
    bool isAppStoreReceipt = [receiptPath.lastPathComponent isEqualToString:@"receipt"];
    bool receiptExists = [[NSFileManager defaultManager] fileExistsAtPath:receiptPath];

    return isAppStoreReceipt && receiptExists;
}

static const char *getBuildType(void)
{
    if (isSimulatorBuild()) {
        return "simulator";
    }
    if (isDebugBuild()) {
        return "debug";
    }
    if (isTestBuild()) {
        return "test";
    }
    if (hasAppStoreReceipt()) {
        return "app store";
    }
    return "unknown";
}

// ============================================================================
#pragma mark - API -
// ============================================================================

static void initialize(void)
{
    static bool isInitialized = false;
    if (!isInitialized) {
        isInitialized = true;

        NSBundle *mainBundle = [NSBundle mainBundle];
        NSDictionary *infoDict = [mainBundle infoDictionary];
        const struct mach_header *header = _dyld_get_image_header(0);

#if KSCRASH_HAS_UIDEVICE
        g_systemData.systemName = cString([UIDevice currentDevice].systemName);
        g_systemData.systemVersion = cString([UIDevice currentDevice].systemVersion);
#else
#if KSCRASH_HOST_MAC
        g_systemData.systemName = "macOS";
#endif
#if KSCRASH_HOST_WATCH
        g_systemData.systemName = "watchOS";
#endif
        NSOperatingSystemVersion version = [NSProcessInfo processInfo].operatingSystemVersion;
        ;
        NSString *systemVersion;
        if (version.patchVersion == 0) {
            systemVersion = [NSString stringWithFormat:@"%d.%d", (int)version.majorVersion, (int)version.minorVersion];
        } else {
            systemVersion = [NSString stringWithFormat:@"%d.%d.%d", (int)version.majorVersion,
                                                       (int)version.minorVersion, (int)version.patchVersion];
        }
        g_systemData.systemVersion = cString(systemVersion);
#endif
        if (isSimulatorBuild()) {
            g_systemData.machine = cString([NSProcessInfo processInfo].environment[@"SIMULATOR_MODEL_IDENTIFIER"]);
            g_systemData.model = "simulator";
        } else {
#if KSCRASH_HOST_MAC
            // MacOS has the machine in the model field, and no model
            g_systemData.machine = stringSysctl("hw.model");
#else
            g_systemData.machine = stringSysctl("hw.machine");
            g_systemData.model = stringSysctl("hw.model");
#endif
        }

        g_systemData.kernelVersion = stringSysctl("kern.version");
        g_systemData.osVersion = stringSysctl("kern.osversion");
        g_systemData.isJailbroken = isJailbroken();
        g_systemData.appStartTime = dateString(time(NULL));
        g_systemData.executablePath = cString(getExecutablePath());
        g_systemData.executableName = cString(infoDict[@"CFBundleExecutable"]);
        g_systemData.bundleID = cString(infoDict[@"CFBundleIdentifier"]);
        g_systemData.bundleName = cString(infoDict[@"CFBundleName"]);
        g_systemData.bundleVersion = cString(infoDict[@"CFBundleVersion"]);
        g_systemData.bundleShortVersion = cString(infoDict[@"CFBundleShortVersionString"]);
        g_systemData.appID = getAppUUID();
        g_systemData.cpuArchitecture = getCurrentCPUArch();
        g_systemData.cpuType = kssysctl_int32ForName("hw.cputype");
        g_systemData.cpuSubType = kssysctl_int32ForName("hw.cpusubtype");
        g_systemData.binaryCPUType = header->cputype;
        g_systemData.binaryCPUSubType = header->cpusubtype;
        g_systemData.timezone = cString([NSTimeZone localTimeZone].abbreviation);
        g_systemData.processName = cString([NSProcessInfo processInfo].processName);
        g_systemData.processID = [NSProcessInfo processInfo].processIdentifier;
        g_systemData.parentProcessID = getppid();
        g_systemData.deviceAppHash = getDeviceAndAppHash();
        g_systemData.buildType = getBuildType();
        g_systemData.memorySize = kssysctl_uint64ForName("hw.memsize");
    }
}

static const char *monitorId(void) { return "System"; }

static void setEnabled(bool isEnabled)
{
    if (isEnabled != g_isEnabled) {
        g_isEnabled = isEnabled;
        if (isEnabled) {
            initialize();
        }
    }
}

static bool isEnabled(void) { return g_isEnabled; }

static void addContextualInfoToEvent(KSCrash_MonitorContext *eventContext)
{
    if (g_isEnabled) {
#define COPY_REFERENCE(NAME) eventContext->System.NAME = g_systemData.NAME
        COPY_REFERENCE(systemName);
        COPY_REFERENCE(systemVersion);
        COPY_REFERENCE(machine);
        COPY_REFERENCE(model);
        COPY_REFERENCE(kernelVersion);
        COPY_REFERENCE(osVersion);
        COPY_REFERENCE(isJailbroken);
        COPY_REFERENCE(appStartTime);
        COPY_REFERENCE(executablePath);
        COPY_REFERENCE(executableName);
        COPY_REFERENCE(bundleID);
        COPY_REFERENCE(bundleName);
        COPY_REFERENCE(bundleVersion);
        COPY_REFERENCE(bundleShortVersion);
        COPY_REFERENCE(appID);
        COPY_REFERENCE(cpuArchitecture);
        COPY_REFERENCE(cpuType);
        COPY_REFERENCE(cpuSubType);
        COPY_REFERENCE(binaryCPUType);
        COPY_REFERENCE(binaryCPUSubType);
        COPY_REFERENCE(timezone);
        COPY_REFERENCE(processName);
        COPY_REFERENCE(processID);
        COPY_REFERENCE(parentProcessID);
        COPY_REFERENCE(deviceAppHash);
        COPY_REFERENCE(buildType);
        COPY_REFERENCE(memorySize);
        eventContext->System.freeMemory = freeMemory();
        eventContext->System.usableMemory = usableMemory();
    }
}

KSCrashMonitorAPI *kscm_system_getAPI(void)
{
    static KSCrashMonitorAPI api = { 0 };
    if (kscm_initAPI(&api)) {
        api.monitorId = monitorId;
        api.setEnabled = setEnabled;
        api.isEnabled = isEnabled;
        api.addContextualInfoToEvent = addContextualInfoToEvent;
    }
    return &api;
}
