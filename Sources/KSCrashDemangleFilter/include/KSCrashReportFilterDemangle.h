//
//  KSCrashReportFilterDemangle.h
//
//  Created by Nikolay Volosatov on 2024-08-16.
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

#include "KSCrashNamespace.h"
#import "KSCrashReportFilter.h"
#import "KSJSONCodecObjC.h"

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/** Demangle symbols in raw crash reports.
 *
 * Input: NSDictionary
 * Output: NSDictionary
 */
NS_SWIFT_NAME(CrashReportFilterDemangle)
@interface KSCrashReportFilterDemangle : NSObject <KSCrashReportFilter>

/** Demangles a C++ symbol.
 *
 * @param symbol The mangled symbol.
 *
 * @return A demangled symbol, or `nil` if demangling failed.
 */
+ (nullable NSString *)demangledCppSymbol:(NSString *)symbol;

/** Demangles a Swift symbol.
 *
 * @param symbol The mangled symbol.
 *
 * @return A demangled symbol, or `nil` if demangling failed.
 */
+ (nullable NSString *)demangledSwiftSymbol:(NSString *)symbol;

@end

NS_ASSUME_NONNULL_END
