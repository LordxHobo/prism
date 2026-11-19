// Prism — WebKit C API bindings, loaded at runtime from WebKit2.dll.
//
// Prism does not link against WebKit at build time. Instead it loads the
// WebKit runtime (downloaded by scripts/fetch-webkit.ps1) when it starts, and
// looks up the C API functions it needs. This keeps Prism's own build tiny
// and lets us update WebKit without rebuilding Prism.
//
// The type and struct declarations below mirror WebKit's public C API headers
// (Source/WebKit/UIProcess/API/C and Source/WebKit/Shared/API/c). The client
// structs are versioned by WebKit, so older versions keep working on newer
// WebKit builds. Do not reorder fields.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <utility>

extern "C" {

// ---- Opaque reference types (WKBase.h / WKBaseWin.h) ----------------------
typedef uint32_t WKTypeID;
typedef const void* WKTypeRef;
typedef const struct OpaqueWKString* WKStringRef;
typedef const struct OpaqueWKURL* WKURLRef;
typedef const struct OpaqueWKURLRequest* WKURLRequestRef;
typedef const struct OpaqueWKURLResponse* WKURLResponseRef;
typedef const struct OpaqueWKError* WKErrorRef;
typedef const struct OpaqueWKData* WKDataRef;
typedef const struct OpaqueWKSecurityOrigin* WKSecurityOriginRef;
typedef const struct OpaqueWKContext* WKContextRef;
typedef const struct OpaqueWKContextConfiguration* WKContextConfigurationRef;
typedef const struct OpaqueWKPage* WKPageRef;
typedef const struct OpaqueWKPageConfiguration* WKPageConfigurationRef;
typedef const struct OpaqueWKPreferences* WKPreferencesRef;
typedef const struct OpaqueWKWebsiteDataStore* WKWebsiteDataStoreRef;
typedef const struct OpaqueWKWebsiteDataStoreConfiguration* WKWebsiteDataStoreConfigurationRef;
typedef const struct OpaqueWKHTTPCookieStore* WKHTTPCookieStoreRef;
typedef const struct OpaqueWKFrame* WKFrameRef;
typedef const struct OpaqueWKFrameInfo* WKFrameInfoRef;
typedef const struct OpaqueWKFramePolicyListener* WKFramePolicyListenerRef;
typedef const struct OpaqueWKNavigation* WKNavigationRef;
typedef const struct OpaqueWKNavigationAction* WKNavigationActionRef;
typedef const struct OpaqueWKNavigationResponse* WKNavigationResponseRef;
typedef const struct OpaqueWKWindowFeatures* WKWindowFeaturesRef;
typedef const struct OpaqueWKDownload* WKDownloadRef;
typedef const struct OpaqueWKInspector* WKInspectorRef;
typedef const struct OpaqueWKPageRunJavaScriptAlertResultListener* WKPageRunJavaScriptAlertResultListenerRef;
typedef const struct OpaqueWKPageRunJavaScriptConfirmResultListener* WKPageRunJavaScriptConfirmResultListenerRef;
typedef const struct OpaqueWKPageRunJavaScriptPromptResultListener* WKPageRunJavaScriptPromptResultListenerRef;
typedef const struct OpaqueWKView* WKViewRef;
typedef const struct OpaqueWKUserContentController* WKUserContentControllerRef;
typedef const struct OpaqueWKUserScript* WKUserScriptRef;
typedef const struct OpaqueWKScriptMessage* WKScriptMessageRef;
typedef const struct OpaqueWKCompletionListener* WKCompletionListenerRef;
typedef const struct OpaqueWKArray* WKArrayRef;
typedef const struct OpaqueWKDictionary* WKDictionaryRef;
typedef struct OpaqueWKDictionary* WKMutableDictionaryRef;
typedef const struct OpaqueWKBoolean* WKBooleanRef;
typedef const struct OpaqueWKGeolocationManager* WKGeolocationManagerRef;
typedef const struct OpaqueWKGeolocationPermissionRequest* WKGeolocationPermissionRequestRef;
typedef const struct OpaqueWKGeolocationPosition* WKGeolocationPositionRef;
typedef const struct OpaqueWKNotification* WKNotificationRef;
typedef const struct OpaqueWKNotificationManager* WKNotificationManagerRef;
typedef const struct OpaqueWKNotificationPermissionRequest* WKNotificationPermissionRequestRef;

// Find in page (WKFindOptions.h).
typedef uint32_t WKFindOptions;
enum {
    kWKFindOptionsCaseInsensitive = 1 << 0,
    kWKFindOptionsAtWordStarts = 1 << 1,
    kWKFindOptionsTreatMedialCapitalAsWordStart = 1 << 2,
    kWKFindOptionsBackwards = 1 << 3,
    kWKFindOptionsWrapAround = 1 << 4,
    kWKFindOptionsShowOverlay = 1 << 5,
    kWKFindOptionsShowFindIndicator = 1 << 6,
    kWKFindOptionsShowHighlight = 1 << 7
};

// WKPageLoadTypes.h
typedef uint32_t WKSameDocumentNavigationType;
enum {
    kWKSameDocumentNavigationAnchorNavigation,
    kWKSameDocumentNavigationSessionStatePush,
    kWKSameDocumentNavigationSessionStateReplace,
    kWKSameDocumentNavigationSessionStatePop
};

// When a user script runs (WKUserScriptInjectionTime.h).
enum _WKUserScriptInjectionTime {
    kWKInjectAtDocumentStart,
    kWKInjectAtDocumentEnd
};
typedef enum _WKUserScriptInjectionTime _WKUserScriptInjectionTime;

typedef uint32_t WKHTTPCookieAcceptPolicy;
enum {
    kWKHTTPCookieAcceptPolicyAlways = 0,
    kWKHTTPCookieAcceptPolicyNever = 1,
    kWKHTTPCookieAcceptPolicyOnlyFromMainDocumentDomain = 2,
    kWKHTTPCookieAcceptPolicyExclusivelyFromMainDocumentDomain = 3,
};

// ---- Callback types -------------------------------------------------------
typedef void (*WKPageEvaluateJavaScriptFunction)(WKTypeRef, WKErrorRef, void*);
typedef void (*WKHTTPCookieStoreSetHTTPCookieAcceptPolicyFunction)(void*);
// Script message handler (WKUserContentControllerRef.h).
typedef void (*WKScriptMessageHandlerCallback)(WKScriptMessageRef message, WKCompletionListenerRef reply, const void* context);

// Completion callback used by the data-removal functions.
typedef void (*WKCompletionFunction)(void*);

// UI client (WKPageUIClient.h)
typedef void (*WKPageUIClientCallback)(WKPageRef page, const void* clientInfo);
typedef WKPageRef (*WKPageCreateNewPageCallback)(WKPageRef page, WKPageConfigurationRef configuration, WKNavigationActionRef navigationAction, WKWindowFeaturesRef windowFeatures, const void* clientInfo);
typedef void (*WKPageRunJavaScriptAlertCallback)(WKPageRef page, WKStringRef alertText, WKFrameRef frame, WKSecurityOriginRef securityOrigin, WKPageRunJavaScriptAlertResultListenerRef listener, const void* clientInfo);
typedef void (*WKPageRunJavaScriptConfirmCallback)(WKPageRef page, WKStringRef message, WKFrameRef frame, WKSecurityOriginRef securityOrigin, WKPageRunJavaScriptConfirmResultListenerRef listener, const void* clientInfo);
typedef void (*WKPageRunJavaScriptPromptCallback)(WKPageRef page, WKStringRef message, WKStringRef defaultValue, WKFrameRef frame, WKSecurityOriginRef securityOrigin, WKPageRunJavaScriptPromptResultListenerRef listener, const void* clientInfo);

typedef void (*WKPageDecidePolicyForGeolocationPermissionRequestCallback)(WKPageRef page, WKFrameRef frame, WKSecurityOriginRef origin, WKGeolocationPermissionRequestRef permissionRequest, const void* clientInfo);
typedef void (*WKPageDecidePolicyForNotificationPermissionRequestCallback)(WKPageRef page, WKSecurityOriginRef origin, WKNotificationPermissionRequestRef permissionRequest, const void* clientInfo);
typedef void (*WKPagePrintFrameCallback)(WKPageRef page, WKFrameRef frame, const void* clientInfo);

// Navigation client (WKPageNavigationClient.h)
typedef void (*WKPageNavigationDecidePolicyForNavigationActionCallback)(WKPageRef page, WKNavigationActionRef navigationAction, WKFramePolicyListenerRef listener, WKTypeRef userData, const void* clientInfo);
typedef void (*WKPageNavigationDecidePolicyForNavigationResponseCallback)(WKPageRef page, WKNavigationResponseRef navigationResponse, WKFramePolicyListenerRef listener, WKTypeRef userData, const void* clientInfo);
typedef void (*WKPageNavigationDidFailProvisionalNavigationCallback)(WKPageRef page, WKNavigationRef navigation, WKErrorRef error, WKTypeRef userData, const void* clientInfo);
typedef void (*WKPageNavigationWebProcessDidTerminateCallback)(WKPageRef page, uint32_t reason, const void* clientInfo);
typedef void (*WKPageNavigationActionDidBecomeDownloadCallback)(WKPageRef page, WKNavigationActionRef navigationAction, WKDownloadRef download, const void* clientInfo);
typedef void (*WKPageNavigationResponseDidBecomeDownloadCallback)(WKPageRef page, WKNavigationResponseRef navigationResponse, WKDownloadRef download, const void* clientInfo);
typedef void (*WKPageNavigationContextMenuDidCreateDownloadCallback)(WKPageRef page, WKDownloadRef download, const void* clientInfo);
typedef void (*WKPageNavigationDidCommitNavigationCallback)(WKPageRef page, WKNavigationRef navigation, WKTypeRef userData, const void* clientInfo);
typedef void (*WKPageNavigationDidSameDocumentNavigationCallback)(WKPageRef page, WKNavigationRef navigation, WKSameDocumentNavigationType navigationType, WKTypeRef userData, const void* clientInfo);

// Find client (WKPageFindClient.h)
typedef void (*WKPageDidFindStringCallback)(WKPageRef page, WKStringRef string, unsigned matchCount, const void* clientInfo);
typedef void (*WKPageDidFailToFindStringCallback)(WKPageRef page, WKStringRef string, const void* clientInfo);
typedef void (*WKPageDidCountStringMatchesCallback)(WKPageRef page, WKStringRef string, unsigned matchCount, const void* clientInfo);

// Geolocation provider (WKGeolocationManager.h)
typedef void (*WKGeolocationProviderStartUpdatingCallback)(WKGeolocationManagerRef geolocationManager, const void* clientInfo);
typedef void (*WKGeolocationProviderStopUpdatingCallback)(WKGeolocationManagerRef geolocationManager, const void* clientInfo);

// Notification provider (WKNotificationProvider.h)
typedef void (*WKNotificationProviderShowCallback)(WKPageRef page, WKNotificationRef notification, const void* clientInfo);
typedef void (*WKNotificationProviderCancelCallback)(WKNotificationRef notification, const void* clientInfo);
typedef void (*WKNotificationProviderDidDestroyNotificationCallback)(WKNotificationRef notification, const void* clientInfo);
typedef void (*WKNotificationProviderAddNotificationManagerCallback)(WKNotificationManagerRef manager, const void* clientInfo);
typedef void (*WKNotificationProviderRemoveNotificationManagerCallback)(WKNotificationManagerRef manager, const void* clientInfo);
typedef WKDictionaryRef (*WKNotificationProviderNotificationPermissionsCallback)(const void* clientInfo);
typedef void (*WKNotificationProviderClearNotificationsCallback)(WKArrayRef notificationIDs, const void* clientInfo);

// State client (WKPageStateClient.h)
typedef void (*WKPageStateClientWillChangeCallback)(const void* clientInfo);
typedef void (*WKPageStateClientDidChangeCallback)(const void* clientInfo);

// Download client (WKDownloadClient.h)
typedef WKStringRef (*WKDownloadDecideDestinationWithResponse)(WKDownloadRef download, WKURLResponseRef response, WKStringRef suggestedFilename, const void* clientInfo);
typedef void (*WKDownloadDidWriteData)(WKDownloadRef download, long long bytesWritten, long long totalBytesWritten, long long totalBytesExpectedToWrite, const void* clientInfo);
typedef void (*WKDownloadDidFinish)(WKDownloadRef download, const void* clientInfo);
typedef void (*WKDownloadDidFailWithError)(WKDownloadRef download, WKErrorRef error, WKDataRef resumeData, const void* clientInfo);

// ---- Client structs -------------------------------------------------------
// Unused callbacks are declared as void* (same size as a function pointer).

typedef struct WKPageUIClientBase {
    int version;
    const void* clientInfo;
} WKPageUIClientBase;

typedef struct WKPageUIClientV6 {
    WKPageUIClientBase base;

    // Version 0.
    void* createNewPage_deprecatedForUseWithV0;
    WKPageUIClientCallback showPage;
    WKPageUIClientCallback close;
    void* takeFocus;
    void* focus;
    void* unfocus;
    void* runJavaScriptAlert_deprecatedForUseWithV0;
    void* runJavaScriptConfirm_deprecatedForUseWithV0;
    void* runJavaScriptPrompt_deprecatedForUseWithV0;
    void* setStatusText;
    void* mouseDidMoveOverElement_deprecatedForUseWithV0;
    void* missingPluginButtonClicked_deprecatedForUseWithV0;
    void* didNotHandleKeyEvent;
    void* didNotHandleWheelEvent;
    void* toolbarsAreVisible;
    void* setToolbarsAreVisible;
    void* menuBarIsVisible;
    void* setMenuBarIsVisible;
    void* statusBarIsVisible;
    void* setStatusBarIsVisible;
    void* isResizable;
    void* setIsResizable;
    void* getWindowFrame;
    void* setWindowFrame;
    void* runBeforeUnloadConfirmPanel;
    void* didDraw;
    void* pageDidScroll;
    void* exceededDatabaseQuota;
    void* runOpenPanel;
    WKPageDecidePolicyForGeolocationPermissionRequestCallback decidePolicyForGeolocationPermissionRequest;
    void* headerHeight;
    void* footerHeight;
    void* drawHeader;
    void* drawFooter;
    WKPagePrintFrameCallback printFrame;
    void* runModal;
    void* unused1;
    void* saveDataToFileInDownloadsFolder;
    void* shouldInterruptJavaScript_unavailable;

    // Version 1.
    void* createNewPage_deprecatedForUseWithV1;
    void* mouseDidMoveOverElement;
    WKPageDecidePolicyForNotificationPermissionRequestCallback decidePolicyForNotificationPermissionRequest;
    void* unavailablePluginButtonClicked_deprecatedForUseWithV1;

    // Version 2.
    void* showColorPicker;
    void* hideColorPicker;
    void* unavailablePluginButtonClicked;

    // Version 3.
    void* pinnedStateDidChange;

    // Version 4.
    void* unused2;
    void* unused3;
    void* unused4;
    void* isPlayingAudioDidChange;

    // Version 5.
    void* decidePolicyForUserMediaPermissionRequest;
    void* didClickAutoFillButton;
    void* runJavaScriptAlert_deprecatedForUseWithV5;
    void* runJavaScriptConfirm_deprecatedForUseWithV5;
    void* runJavaScriptPrompt_deprecatedForUseWithV5;
    void* unused5;

    // Version 6.
    WKPageCreateNewPageCallback createNewPage;
    WKPageRunJavaScriptAlertCallback runJavaScriptAlert;
    WKPageRunJavaScriptConfirmCallback runJavaScriptConfirm;
    WKPageRunJavaScriptPromptCallback runJavaScriptPrompt;
    void* unused6;
} WKPageUIClientV6;

typedef struct WKPageNavigationClientBase {
    int version;
    const void* clientInfo;
} WKPageNavigationClientBase;

typedef struct WKPageNavigationClientV3 {
    WKPageNavigationClientBase base;

    // Version 0.
    WKPageNavigationDecidePolicyForNavigationActionCallback decidePolicyForNavigationAction;
    WKPageNavigationDecidePolicyForNavigationResponseCallback decidePolicyForNavigationResponse;
    void* decidePolicyForPluginLoad;
    void* didStartProvisionalNavigation;
    void* didReceiveServerRedirectForProvisionalNavigation;
    WKPageNavigationDidFailProvisionalNavigationCallback didFailProvisionalNavigation;
    WKPageNavigationDidCommitNavigationCallback didCommitNavigation;
    void* didFinishNavigation;
    void* didFailNavigation;
    void* didFailProvisionalLoadInSubframe;
    void* didFinishDocumentLoad;
    WKPageNavigationDidSameDocumentNavigationCallback didSameDocumentNavigation;
    void* renderingProgressDidChange;
    void* canAuthenticateAgainstProtectionSpace;
    void* didReceiveAuthenticationChallenge;
    void* webProcessDidCrash;
    void* copyWebCryptoMasterKey;
    void* didBeginNavigationGesture;
    void* willEndNavigationGesture;
    void* didEndNavigationGesture;
    void* didRemoveNavigationGestureSnapshot;

    // Version 1.
    WKPageNavigationWebProcessDidTerminateCallback webProcessDidTerminate;

    // Version 2.
    void* contentRuleListNotification;

    // Version 3.
    void* copySignedPublicKeyAndChallengeString;
    WKPageNavigationActionDidBecomeDownloadCallback navigationActionDidBecomeDownload;
    WKPageNavigationResponseDidBecomeDownloadCallback navigationResponseDidBecomeDownload;
    WKPageNavigationContextMenuDidCreateDownloadCallback contextMenuDidCreateDownload;
} WKPageNavigationClientV3;

typedef struct WKPageStateClientBase {
    int version;
    const void* clientInfo;
} WKPageStateClientBase;

typedef struct WKPageStateClientV0 {
    WKPageStateClientBase base;

    // Version 0.
    WKPageStateClientWillChangeCallback willChangeIsLoading;
    WKPageStateClientDidChangeCallback didChangeIsLoading;
    WKPageStateClientWillChangeCallback willChangeTitle;
    WKPageStateClientDidChangeCallback didChangeTitle;
    WKPageStateClientWillChangeCallback willChangeActiveURL;
    WKPageStateClientDidChangeCallback didChangeActiveURL;
    WKPageStateClientWillChangeCallback willChangeHasOnlySecureContent;
    WKPageStateClientDidChangeCallback didChangeHasOnlySecureContent;
    WKPageStateClientWillChangeCallback willChangeEstimatedProgress;
    WKPageStateClientDidChangeCallback didChangeEstimatedProgress;
    WKPageStateClientWillChangeCallback willChangeCanGoBack;
    WKPageStateClientDidChangeCallback didChangeCanGoBack;
    WKPageStateClientWillChangeCallback willChangeCanGoForward;
    WKPageStateClientDidChangeCallback didChangeCanGoForward;
    WKPageStateClientWillChangeCallback willChangeNetworkRequestsInProgress;
    WKPageStateClientDidChangeCallback didChangeNetworkRequestsInProgress;
    WKPageStateClientWillChangeCallback willChangeCertificateInfo;
    WKPageStateClientDidChangeCallback didChangeCertificateInfo;
    WKPageStateClientWillChangeCallback willChangeWebProcessIsResponsive;
    WKPageStateClientDidChangeCallback didChangeWebProcessIsResponsive;
    WKPageStateClientDidChangeCallback didSwapWebProcesses;
} WKPageStateClientV0;

typedef struct WKDownloadClientBase {
    int version;
    const void* clientInfo;
} WKDownloadClientBase;

typedef struct WKDownloadClientV0 {
    WKDownloadClientBase base;

    // Version 0.
    void* willPerformHTTPRedirection;
    void* didReceiveAuthenticationChallenge;
    WKDownloadDecideDestinationWithResponse decideDestinationWithResponse;
    WKDownloadDidWriteData didWriteData;
    WKDownloadDidFinish didFinish;
    WKDownloadDidFailWithError didFailWithError;
} WKDownloadClientV0;

typedef struct WKPageFindClientBase {
    int version;
    const void* clientInfo;
} WKPageFindClientBase;

typedef struct WKPageFindClientV0 {
    WKPageFindClientBase base;

    // Version 0.
    WKPageDidFindStringCallback didFindString;
    WKPageDidFailToFindStringCallback didFailToFindString;
    WKPageDidCountStringMatchesCallback didCountStringMatches;
} WKPageFindClientV0;

typedef struct WKGeolocationProviderBase {
    int version;
    const void* clientInfo;
} WKGeolocationProviderBase;

typedef struct WKGeolocationProviderV0 {
    WKGeolocationProviderBase base;

    // Version 0.
    WKGeolocationProviderStartUpdatingCallback startUpdating;
    WKGeolocationProviderStopUpdatingCallback stopUpdating;
} WKGeolocationProviderV0;

typedef struct WKNotificationProviderBase {
    int version;
    const void* clientInfo;
} WKNotificationProviderBase;

typedef struct WKNotificationProviderV0 {
    WKNotificationProviderBase base;

    // Version 0.
    WKNotificationProviderShowCallback show;
    WKNotificationProviderCancelCallback cancel;
    WKNotificationProviderDidDestroyNotificationCallback didDestroyNotification;
    WKNotificationProviderAddNotificationManagerCallback addNotificationManager;
    WKNotificationProviderRemoveNotificationManagerCallback removeNotificationManager;
    WKNotificationProviderNotificationPermissionsCallback notificationPermissions;
    WKNotificationProviderClearNotificationsCallback clearNotifications;
} WKNotificationProviderV0;

} // extern "C"

// ---- Function table -------------------------------------------------------
// X(name, return type, parameter list). REQUIRED functions must exist in the
// WebKit runtime or Prism refuses to start with a clear error. OPTIONAL ones
// may be missing in some WebKit builds; check the pointer before calling.

#define PRISM_WK_REQUIRED(X) \
    X(WKRetain, WKTypeRef, (WKTypeRef)) \
    X(WKRelease, void, (WKTypeRef)) \
    X(WKStringCreateWithUTF8CString, WKStringRef, (const char*)) \
    X(WKStringGetMaximumUTF8CStringSize, size_t, (WKStringRef)) \
    X(WKStringGetUTF8CString, size_t, (WKStringRef, char*, size_t)) \
    X(WKURLCreateWithUTF8CString, WKURLRef, (const char*)) \
    X(WKURLCopyString, WKStringRef, (WKURLRef)) \
    X(WKContextCreateWithConfiguration, WKContextRef, (WKContextConfigurationRef)) \
    X(WKPreferencesCreate, WKPreferencesRef, (void)) \
    X(WKWebsiteDataStoreConfigurationCreate, WKWebsiteDataStoreConfigurationRef, (void)) \
    X(WKWebsiteDataStoreCreateWithConfiguration, WKWebsiteDataStoreRef, (WKWebsiteDataStoreConfigurationRef)) \
    X(WKPageConfigurationCreate, WKPageConfigurationRef, (void)) \
    X(WKPageConfigurationSetContext, void, (WKPageConfigurationRef, WKContextRef)) \
    X(WKPageConfigurationSetPreferences, void, (WKPageConfigurationRef, WKPreferencesRef)) \
    X(WKPageConfigurationSetWebsiteDataStore, void, (WKPageConfigurationRef, WKWebsiteDataStoreRef)) \
    X(WKViewCreate, WKViewRef, (RECT, WKPageConfigurationRef, HWND)) \
    X(WKViewGetWindow, HWND, (WKViewRef)) \
    X(WKViewGetPage, WKPageRef, (WKViewRef)) \
    X(WKViewSetIsInWindow, void, (WKViewRef, bool)) \
    X(WKPageLoadURL, void, (WKPageRef, WKURLRef)) \
    X(WKPageLoadFile, void, (WKPageRef, WKURLRef, WKURLRef)) \
    X(WKPageStopLoading, void, (WKPageRef)) \
    X(WKPageReload, void, (WKPageRef)) \
    X(WKPageReloadFromOrigin, void, (WKPageRef)) \
    X(WKPageClose, void, (WKPageRef)) \
    X(WKPageGoBack, void, (WKPageRef)) \
    X(WKPageGoForward, void, (WKPageRef)) \
    X(WKPageCanGoBack, bool, (WKPageRef)) \
    X(WKPageCanGoForward, bool, (WKPageRef)) \
    X(WKPageCopyTitle, WKStringRef, (WKPageRef)) \
    X(WKPageCopyActiveURL, WKURLRef, (WKPageRef)) \
    X(WKPageGetEstimatedProgress, double, (WKPageRef)) \
    X(WKPageGetPageZoomFactor, double, (WKPageRef)) \
    X(WKPageSetPageZoomFactor, void, (WKPageRef, double)) \
    X(WKPageSetPageUIClient, void, (WKPageRef, const WKPageUIClientBase*)) \
    X(WKPageSetPageNavigationClient, void, (WKPageRef, const WKPageNavigationClientBase*)) \
    X(WKPageSetPageStateClient, void, (WKPageRef, WKPageStateClientBase*)) \
    X(WKPageEvaluateJavaScriptInMainFrame, void, (WKPageRef, WKStringRef, void*, WKPageEvaluateJavaScriptFunction)) \
    X(WKPageRunJavaScriptAlertResultListenerCall, void, (WKPageRunJavaScriptAlertResultListenerRef)) \
    X(WKPageRunJavaScriptConfirmResultListenerCall, void, (WKPageRunJavaScriptConfirmResultListenerRef, bool)) \
    X(WKPageRunJavaScriptPromptResultListenerCall, void, (WKPageRunJavaScriptPromptResultListenerRef, WKStringRef)) \
    X(WKFramePolicyListenerUse, void, (WKFramePolicyListenerRef)) \
    X(WKFramePolicyListenerIgnore, void, (WKFramePolicyListenerRef)) \
    X(WKFramePolicyListenerDownload, void, (WKFramePolicyListenerRef)) \
    X(WKFrameInfoGetIsMainFrame, bool, (WKFrameInfoRef)) \
    X(WKFrameInfoGetPage, WKPageRef, (WKFrameInfoRef)) \
    X(WKFrameIsMainFrame, bool, (WKFrameRef)) \
    X(WKFrameCopyURL, WKURLRef, (WKFrameRef)) \
    X(WKNavigationActionCopyRequest, WKURLRequestRef, (WKNavigationActionRef)) \
    X(WKNavigationActionCopyTargetFrameInfo, WKFrameInfoRef, (WKNavigationActionRef)) \
    X(WKNavigationResponseCanShowMIMEType, bool, (WKNavigationResponseRef)) \
    X(WKNavigationResponseCopyFrameInfo, WKFrameInfoRef, (WKNavigationResponseRef)) \
    X(WKNavigationResponseCopyResponse, WKURLResponseRef, (WKNavigationResponseRef)) \
    X(WKURLRequestCopyURL, WKURLRef, (WKURLRequestRef)) \
    X(WKURLResponseCopyURL, WKURLRef, (WKURLResponseRef)) \
    X(WKErrorGetErrorCode, int, (WKErrorRef)) \
    X(WKErrorCopyLocalizedDescription, WKStringRef, (WKErrorRef)) \
    X(WKErrorCopyFailingURL, WKURLRef, (WKErrorRef)) \
    X(WKDownloadSetClient, void, (WKDownloadRef, WKDownloadClientBase*)) \
    X(WKGetTypeID, WKTypeID, (WKTypeRef)) \
    X(WKStringGetTypeID, WKTypeID, (void)) \
    X(WKUserContentControllerCreate, WKUserContentControllerRef, (void)) \
    X(WKUserContentControllerAddUserScript, void, (WKUserContentControllerRef, WKUserScriptRef)) \
    X(WKUserContentControllerRemoveAllUserScripts, void, (WKUserContentControllerRef)) \
    X(WKUserContentControllerAddScriptMessageHandler, void, (WKUserContentControllerRef, WKStringRef, WKScriptMessageHandlerCallback, const void*)) \
    X(WKUserScriptCreateWithSource, WKUserScriptRef, (WKStringRef, _WKUserScriptInjectionTime, bool)) \
    X(WKPageConfigurationSetUserContentController, void, (WKPageConfigurationRef, WKUserContentControllerRef)) \
    X(WKScriptMessageGetBody, WKTypeRef, (WKScriptMessageRef)) \
    X(WKScriptMessageGetFrameInfo, WKFrameInfoRef, (WKScriptMessageRef)) \
    X(WKCompletionListenerComplete, void, (WKCompletionListenerRef, WKTypeRef))

#define PRISM_WK_OPTIONAL(X) \
    X(WKWebsiteDataStoreConfigurationSetNetworkCacheDirectory, void, (WKWebsiteDataStoreConfigurationRef, WKStringRef)) \
    X(WKWebsiteDataStoreConfigurationSetIndexedDBDatabaseDirectory, void, (WKWebsiteDataStoreConfigurationRef, WKStringRef)) \
    X(WKWebsiteDataStoreConfigurationSetLocalStorageDirectory, void, (WKWebsiteDataStoreConfigurationRef, WKStringRef)) \
    X(WKWebsiteDataStoreConfigurationSetCacheStorageDirectory, void, (WKWebsiteDataStoreConfigurationRef, WKStringRef)) \
    X(WKWebsiteDataStoreConfigurationSetGeneralStorageDirectory, void, (WKWebsiteDataStoreConfigurationRef, WKStringRef)) \
    X(WKWebsiteDataStoreConfigurationSetMediaKeysStorageDirectory, void, (WKWebsiteDataStoreConfigurationRef, WKStringRef)) \
    X(WKWebsiteDataStoreConfigurationSetResourceLoadStatisticsDirectory, void, (WKWebsiteDataStoreConfigurationRef, WKStringRef)) \
    X(WKWebsiteDataStoreConfigurationSetServiceWorkerRegistrationDirectory, void, (WKWebsiteDataStoreConfigurationRef, WKStringRef)) \
    X(WKWebsiteDataStoreConfigurationSetCookieStorageFile, void, (WKWebsiteDataStoreConfigurationRef, WKStringRef)) \
    X(WKWebsiteDataStoreSetResourceLoadStatisticsEnabled, void, (WKWebsiteDataStoreRef, bool)) \
    X(WKWebsiteDataStoreGetHTTPCookieStore, WKHTTPCookieStoreRef, (WKWebsiteDataStoreRef)) \
    X(WKHTTPCookieStoreSetHTTPCookieAcceptPolicy, void, (WKHTTPCookieStoreRef, WKHTTPCookieAcceptPolicy, void*, WKHTTPCookieStoreSetHTTPCookieAcceptPolicyFunction)) \
    X(WKHTTPCookieStoreDeleteAllCookies, void, (WKHTTPCookieStoreRef, void*, WKCompletionFunction)) \
    X(WKWebsiteDataStoreClearStorage, void, (WKWebsiteDataStoreRef, void*, WKCompletionFunction)) \
    X(WKWebsiteDataStoreRemoveNetworkCache, void, (WKWebsiteDataStoreRef, void*, WKCompletionFunction)) \
    X(WKWebsiteDataStoreRemoveMemoryCaches, void, (WKWebsiteDataStoreRef, void*, WKCompletionFunction)) \
    X(WKWebsiteDataStoreRemoveAllFetchCaches, void, (WKWebsiteDataStoreRef, void*, WKCompletionFunction)) \
    X(WKWebsiteDataStoreStatisticsClearThroughWebsiteDataRemoval, void, (WKWebsiteDataStoreRef, void*, WKCompletionFunction)) \
    X(WKWebsiteDataStoreEnableCustomNetworkProxySettings, void, (WKWebsiteDataStoreRef, WKURLRef, WKStringRef)) \
    X(WKWebsiteDataStoreDisableNetworkProxySettings, void, (WKWebsiteDataStoreRef)) \
    X(WKPreferencesSetDNSPrefetchingEnabled, void, (WKPreferencesRef, bool)) \
    X(WKPreferencesSetHyperlinkAuditingEnabled, void, (WKPreferencesRef, bool)) \
    X(WKPreferencesSetDeveloperExtrasEnabled, void, (WKPreferencesRef, bool)) \
    X(WKPreferencesSetHiddenPageDOMTimerThrottlingEnabled, void, (WKPreferencesRef, bool)) \
    X(WKPreferencesSetPageVisibilityBasedProcessSuppressionEnabled, void, (WKPreferencesRef, bool)) \
    X(WKPreferencesSetJavaScriptCanOpenWindowsAutomatically, void, (WKPreferencesRef, bool)) \
    X(WKPageSetApplicationNameForUserAgent, void, (WKPageRef, WKStringRef)) \
    X(WKPageSetCustomBackingScaleFactor, void, (WKPageRef, double)) \
    X(WKPageGetInspector, WKInspectorRef, (WKPageRef)) \
    X(WKInspectorShow, void, (WKInspectorRef)) \
    X(WKPageFindString, void, (WKPageRef, WKStringRef, WKFindOptions, unsigned)) \
    X(WKPageHideFindUI, void, (WKPageRef)) \
    X(WKPageSetPageFindClient, void, (WKPageRef, const WKPageFindClientBase*)) \
    X(WKSecurityOriginCopyToString, WKStringRef, (WKSecurityOriginRef)) \
    X(WKSecurityOriginCreateFromString, WKSecurityOriginRef, (WKStringRef)) \
    X(WKMutableDictionaryCreate, WKMutableDictionaryRef, (void)) \
    X(WKDictionarySetItem, bool, (WKMutableDictionaryRef, WKStringRef, WKTypeRef)) \
    X(WKBooleanCreate, WKBooleanRef, (bool)) \
    X(WKContextGetGeolocationManager, WKGeolocationManagerRef, (WKContextRef)) \
    X(WKGeolocationManagerSetProvider, void, (WKGeolocationManagerRef, const WKGeolocationProviderBase*)) \
    X(WKGeolocationManagerProviderDidChangePosition, void, (WKGeolocationManagerRef, WKGeolocationPositionRef)) \
    X(WKGeolocationManagerProviderDidFailToDeterminePositionWithErrorMessage, void, (WKGeolocationManagerRef, WKStringRef)) \
    X(WKGeolocationPositionCreate, WKGeolocationPositionRef, (double, double, double, double)) \
    X(WKGeolocationPermissionRequestAllow, void, (WKGeolocationPermissionRequestRef)) \
    X(WKGeolocationPermissionRequestDeny, void, (WKGeolocationPermissionRequestRef)) \
    X(WKContextGetNotificationManager, WKNotificationManagerRef, (WKContextRef)) \
    X(WKNotificationManagerSetProvider, void, (WKNotificationManagerRef, const WKNotificationProviderBase*)) \
    X(WKNotificationManagerProviderDidShowNotification, void, (WKNotificationManagerRef, uint64_t)) \
    X(WKNotificationManagerProviderDidClickNotification, void, (WKNotificationManagerRef, uint64_t)) \
    X(WKNotificationManagerProviderDidUpdateNotificationPolicy, void, (WKNotificationManagerRef, WKSecurityOriginRef, bool)) \
    X(WKNotificationPermissionRequestAllow, void, (WKNotificationPermissionRequestRef)) \
    X(WKNotificationPermissionRequestDeny, void, (WKNotificationPermissionRequestRef)) \
    X(WKNotificationCopyTitle, WKStringRef, (WKNotificationRef)) \
    X(WKNotificationCopyBody, WKStringRef, (WKNotificationRef)) \
    X(WKNotificationGetSecurityOrigin, WKSecurityOriginRef, (WKNotificationRef)) \
    X(WKNotificationGetID, uint64_t, (WKNotificationRef)) \
    X(WKWebsiteDataStoreTerminateNetworkProcess, void, (WKWebsiteDataStoreRef))

namespace wk {

#define PRISM_WK_DECLARE(name, ret, params) extern ret (*name) params;
PRISM_WK_REQUIRED(PRISM_WK_DECLARE)
PRISM_WK_OPTIONAL(PRISM_WK_DECLARE)
#undef PRISM_WK_DECLARE

// Loads WebKit2.dll from the folder Prism.exe lives in. On failure returns
// false and fills `error` with a message that can be shown to the user.
bool load(std::wstring& error);

// Small RAII holder for WebKit references (like WebKit's WKRetainPtr).
template<typename T>
class Ref {
public:
    Ref() = default;
    // Takes ownership of an already-retained reference (from a Create/Copy call).
    static Ref adopt(T value) { Ref r; r.m_value = value; return r; }
    // Retains a borrowed reference (from a Get call).
    static Ref retain(T value) { Ref r; r.m_value = value; if (value) WKRetain(value); return r; }
    ~Ref() { reset(); }
    Ref(const Ref& other) : m_value(other.m_value) { if (m_value) WKRetain(m_value); }
    Ref(Ref&& other) noexcept : m_value(other.m_value) { other.m_value = nullptr; }
    Ref& operator=(Ref other) noexcept { std::swap(m_value, other.m_value); return *this; }
    T get() const { return m_value; }
    explicit operator bool() const { return !!m_value; }
    void reset() { if (m_value) WKRelease(m_value); m_value = nullptr; }
private:
    T m_value { nullptr };
};

// String helpers (UTF-8 inside WebKit, UTF-16 in Win32).
Ref<WKStringRef> string(const std::wstring&);
Ref<WKStringRef> stringUTF8(const std::string&);
Ref<WKURLRef> url(const std::wstring&);
std::wstring toWide(WKStringRef);
std::wstring toWide(WKURLRef);

} // namespace wk
