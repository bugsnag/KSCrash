//
//  KSMachineContext_Apple.h
//
//  Created by Karl Stenerud on 2016-12-02.
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

#ifndef HDR_KSMachineContext_Apple_h
#define HDR_KSMachineContext_Apple_h

#include <mach/mach_types.h>
#include <stdbool.h>
#include <sys/ucontext.h>

#include "KSCrashNamespace.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_CAPTURED_THREADS 1000

#ifdef __arm64__
#define STRUCT_MCONTEXT_L _STRUCT_MCONTEXT64
#else
#define STRUCT_MCONTEXT_L _STRUCT_MCONTEXT
#endif

typedef struct KSMachineContext {
    thread_t thisThread;
    thread_t allThreads[MAX_CAPTURED_THREADS];
    int threadCount;
    bool isCrashedContext;
    bool isCurrentThread;
    bool isStackOverflow;
    bool isSignalContext;
    STRUCT_MCONTEXT_L machineContext;
} KSMachineContext;

#ifdef __cplusplus
}
#endif

#endif  // HDR_KSMachineContext_Apple_h
