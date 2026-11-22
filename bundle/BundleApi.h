// Prism — the WebKit injected-bundle C API used by PrismBundle.dll.
//
// PrismBundle.dll runs inside WebKit's web content processes (one per group
// of tabs). Like Prism.exe it doesn't link against WebKit: WebKit2.dll is
// already loaded in that process, so the bundle looks the functions up in it
// when WebKit calls WKBundleInitialize.
//
// The declarations mirror WebKit's public headers
// (Source/WebKit/WebProcess/InjectedBundle/API/c and Source/WebKit/Shared/API/c).
// Client structs are versioned by WebKit. Do not reorder fields.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {

typedef uint32_t WKTypeID;
typedef const void* WKTypeRef;
typedef const struct OpaqueWKString* WKStringRef;
typedef const struct OpaqueWKURL* WKURLRef;
typedef const struct OpaqueWKURLRequest* WKURLRequestRef;
typedef const struct OpaqueWKURLResponse* WKURLResponseRef;
typedef const struct OpaqueWKError* WKErrorRef;
typedef const struct OpaqueWKBundle* WKBundleRef;
typedef const struct OpaqueWKBundlePage* WKBundlePageRef;
typedef const struct OpaqueWKBundleFrame* WKBundleFrameRef;
typedef const struct OpaqueWKBundlePageGroup* WKBundlePageGroupRef;

// WKBundle.h
typedef void (*WKBundleDidCreatePageCallback)(WKBundleRef, WKBundlePageRef, const void* clientInfo);
typedef void (*WKBundleWillDestroyPageCallback)(WKBundleRef, WKBundlePageRef, const void* clientInfo);
typedef void (*WKBundleDidInitializePageGroupCallback)(WKBundleRef, WKBundlePageGroupRef, const void* clientInfo);
typedef void (*WKBundleDidReceiveMessageCallback)(WKBundleRef, WKStringRef name, WKTypeRef messageBody, const void* clientInfo);
typedef void (*WKBundleDidReceiveMessageToPageCallback)(WKBundleRef, WKBundlePageRef, WKStringRef name, WKTypeRef messageBody, const void* clientInfo);

typedef struct WKBundleClientBase {
    int version;
    const void* clientInfo;
} WKBundleClientBase;

typedef struct WKBundleClientV1 {
    WKBundleClientBase base;
    // Version 0.
    WKBundleDidCreatePageCallback didCreatePage;
    WKBundleWillDestroyPageCallback willDestroyPage;
    WKBundleDidInitializePageGroupCallback didInitializePageGroup;
    WKBundleDidReceiveMessageCallback didReceiveMessage;
    // Version 1.
    WKBundleDidReceiveMessageToPageCallback didReceiveMessageToPage;
} WKBundleClientV1;

// WKBundlePageResourceLoadClient.h
typedef void (*WKBundlePageDidInitiateLoadForResourceCallback)(WKBundlePageRef, WKBundleFrameRef, uint64_t resourceIdentifier, WKURLRequestRef, bool pageIsProvisionallyLoading, const void* clientInfo);
typedef WKURLRequestRef (*WKBundlePageWillSendRequestForFrameCallback)(WKBundlePageRef, WKBundleFrameRef, uint64_t resourceIdentifier, WKURLRequestRef, WKURLResponseRef redirectResponse, const void* clientInfo);
typedef void (*WKBundlePageDidReceiveResponseForResourceCallback)(WKBundlePageRef, WKBundleFrameRef, uint64_t resourceIdentifier, WKURLResponseRef, const void* clientInfo);
typedef void (*WKBundlePageDidReceiveContentLengthForResourceCallback)(WKBundlePageRef, WKBundleFrameRef, uint64_t resourceIdentifier, uint64_t contentLength, const void* clientInfo);
typedef void (*WKBundlePageDidFinishLoadForResourceCallback)(WKBundlePageRef, WKBundleFrameRef, uint64_t resourceIdentifier, const void* clientInfo);
typedef void (*WKBundlePageDidFailLoadForResourceCallback)(WKBundlePageRef, WKBundleFrameRef, uint64_t resourceIdentifier, WKErrorRef, const void* clientInfo);

typedef struct WKBundlePageResourceLoadClientBase {
    int version;
    const void* clientInfo;
} WKBundlePageResourceLoadClientBase;

typedef struct WKBundlePageResourceLoadClientV0 {
    WKBundlePageResourceLoadClientBase base;
    WKBundlePageDidInitiateLoadForResourceCallback didInitiateLoadForResource;
    // Must return a retained request (or null to cancel the load).
    WKBundlePageWillSendRequestForFrameCallback willSendRequestForFrame;
    WKBundlePageDidReceiveResponseForResourceCallback didReceiveResponseForResource;
    WKBundlePageDidReceiveContentLengthForResourceCallback didReceiveContentLengthForResource;
    WKBundlePageDidFinishLoadForResourceCallback didFinishLoadForResource;
    WKBundlePageDidFailLoadForResourceCallback didFailLoadForResource;
} WKBundlePageResourceLoadClientV0;

} // extern "C"

// Required functions: the bundle does nothing without them.
#define PRISM_BUNDLE_REQUIRED(X) \
    X(WKRetain, WKTypeRef, (WKTypeRef)) \
    X(WKRelease, void, (WKTypeRef)) \
    X(WKGetTypeID, WKTypeID, (WKTypeRef)) \
    X(WKStringGetTypeID, WKTypeID, ()) \
    X(WKStringCreateWithUTF8CString, WKStringRef, (const char*)) \
    X(WKStringGetMaximumUTF8CStringSize, size_t, (WKStringRef)) \
    X(WKStringGetUTF8CString, size_t, (WKStringRef, char*, size_t)) \
    X(WKURLCopyString, WKStringRef, (WKURLRef)) \
    X(WKURLRequestCopyURL, WKURLRef, (WKURLRequestRef)) \
    X(WKURLRequestCopyFirstPartyForCookies, WKURLRef, (WKURLRequestRef)) \
    X(WKURLResponseCopyURL, WKURLRef, (WKURLResponseRef)) \
    X(WKBundleSetClient, void, (WKBundleRef, WKBundleClientBase*)) \
    X(WKBundlePostSynchronousMessage, void, (WKBundleRef, WKStringRef, WKTypeRef, WKTypeRef*)) \
    X(WKBundlePagePostMessage, void, (WKBundlePageRef, WKStringRef, WKTypeRef)) \
    X(WKBundlePageSetResourceLoadClient, void, (WKBundlePageRef, WKBundlePageResourceLoadClientBase*)) \
    X(WKBundleFrameIsMainFrame, bool, (WKBundleFrameRef)) \
    X(WKBundleFrameCopyProvisionalURL, WKURLRef, (WKBundleFrameRef)) \
    X(WKBundleFrameCopyURL, WKURLRef, (WKBundleFrameRef)) \
    X(WKBundlePostMessage, void, (WKBundleRef, WKStringRef, WKTypeRef)) \
    X(WKURLRequestCopyHTTPMethod, WKStringRef, (WKURLRequestRef)) \
    X(WKURLRequestCreateWithWKURL, WKURLRequestRef, (WKURLRef)) \
    X(WKURLCreateWithUTF8CString, WKURLRef, (const char*))

// Optional functions: used when present.
// ---- Extension content scripts in their own JavaScript worlds ----------------
// (ContentScripts.cpp). Optional: without them, Prism runs content scripts
// in the page's world as before (it checks the same functions itself).

typedef const struct OpaqueWKBundleScriptWorld* WKBundleScriptWorldRef;

typedef void (*WKBundlePageDidClearWindowObjectForFrameCallback)(WKBundlePageRef, WKBundleFrameRef, WKBundleScriptWorldRef, const void* clientInfo);

typedef struct WKBundlePageLoaderClientBase {
    int version;
    const void* clientInfo;
} WKBundlePageLoaderClientBase;

// Version 0: the first 14 callbacks come before didClearWindowObjectForFrame.
typedef struct WKBundlePageLoaderClientV0 {
    WKBundlePageLoaderClientBase base;
    void* beforeClearWindowObject[14];
    WKBundlePageDidClearWindowObjectForFrameCallback didClearWindowObjectForFrame;
    void* didCancelClientRedirectForFrame;
    void* willPerformClientRedirectForFrame;
    void* didHandleOnloadEventsForFrame;
} WKBundlePageLoaderClientV0;

// JavaScriptCore's C API (JavaScriptCore.dll).
typedef const struct OpaqueJSContext* JSContextRef;
typedef struct OpaqueJSContext* JSGlobalContextRef;
typedef struct OpaqueJSString* JSStringRef;
typedef const struct OpaqueJSValue* JSValueRef;
typedef struct OpaqueJSValue* JSObjectRef;
typedef unsigned JSPropertyAttributes;
typedef JSValueRef (*JSObjectCallAsFunctionCallback)(JSContextRef, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception);
constexpr JSPropertyAttributes kPrismJSReadOnlyDontEnumDontDelete = (1 << 1) | (1 << 2) | (1 << 3);

#define PRISM_BUNDLE_CONTENT(X) \
    X(WKBundlePageSetPageLoaderClient, void, (WKBundlePageRef, WKBundlePageLoaderClientBase*)) \
    X(WKBundleScriptWorldCreateWorld, WKBundleScriptWorldRef, ()) \
    X(WKBundleScriptWorldNormalWorld, WKBundleScriptWorldRef, ()) \
    X(WKBundleFrameGetJavaScriptContextForWorld, JSGlobalContextRef, (WKBundleFrameRef, WKBundleScriptWorldRef)) \
    X(WKBundlePageGetMainFrame, WKBundleFrameRef, (WKBundlePageRef))

#define PRISM_JSC(X) \
    X(JSStringCreateWithUTF8CString, JSStringRef, (const char*)) \
    X(JSStringRelease, void, (JSStringRef)) \
    X(JSStringGetMaximumUTF8CStringSize, size_t, (JSStringRef)) \
    X(JSStringGetUTF8CString, size_t, (JSStringRef, char*, size_t)) \
    X(JSEvaluateScript, JSValueRef, (JSContextRef, JSStringRef, JSObjectRef, JSStringRef, int, JSValueRef*)) \
    X(JSContextGetGlobalObject, JSObjectRef, (JSContextRef)) \
    X(JSContextGetGlobalContext, JSGlobalContextRef, (JSContextRef)) \
    X(JSGlobalContextRetain, JSGlobalContextRef, (JSGlobalContextRef)) \
    X(JSGlobalContextRelease, void, (JSGlobalContextRef)) \
    X(JSObjectMakeFunctionWithCallback, JSObjectRef, (JSContextRef, JSStringRef, JSObjectCallAsFunctionCallback)) \
    X(JSObjectSetProperty, void, (JSContextRef, JSObjectRef, JSStringRef, JSValueRef, JSPropertyAttributes, JSValueRef*)) \
    X(JSObjectGetProperty, JSValueRef, (JSContextRef, JSObjectRef, JSStringRef, JSValueRef*)) \
    X(JSValueIsObject, bool, (JSContextRef, JSValueRef)) \
    X(JSValueToObject, JSObjectRef, (JSContextRef, JSValueRef, JSValueRef*)) \
    X(JSObjectIsFunction, bool, (JSContextRef, JSObjectRef)) \
    X(JSObjectCallAsFunction, JSValueRef, (JSContextRef, JSObjectRef, JSObjectRef, size_t, const JSValueRef*, JSValueRef*)) \
    X(JSValueMakeString, JSValueRef, (JSContextRef, JSStringRef)) \
    X(JSValueMakeNumber, JSValueRef, (JSContextRef, double)) \
    X(JSValueMakeUndefined, JSValueRef, (JSContextRef)) \
    X(JSValueToStringCopy, JSStringRef, (JSContextRef, JSValueRef, JSValueRef*)) \
    X(JSValueToNumber, double, (JSContextRef, JSValueRef, JSValueRef*))

#define PRISM_BUNDLE_OPTIONAL(X) \
    X(WKBundlePageGetWebPageProxyIdentifierForTesting, uint64_t, (WKBundlePageRef)) \
    X(WKBundleFrameGetParentFrame, WKBundleFrameRef, (WKBundleFrameRef)) \
    X(WKErrorCopyLocalizedDescription, WKStringRef, (WKErrorRef))
