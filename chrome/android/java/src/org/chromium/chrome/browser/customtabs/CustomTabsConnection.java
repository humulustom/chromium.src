// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.customtabs;

import android.app.ActivityManager;
import android.app.PendingIntent;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.net.ConnectivityManager;
import android.net.Uri;
import android.os.Binder;
import android.os.Build;
import android.os.Bundle;
import android.os.Process;
import android.os.SystemClock;
import android.text.TextUtils;
import android.widget.RemoteViews;

import androidx.annotation.IntDef;
import androidx.annotation.Nullable;
import androidx.annotation.VisibleForTesting;
import androidx.browser.customtabs.CustomTabsCallback;
import androidx.browser.customtabs.CustomTabsIntent;
import androidx.browser.customtabs.CustomTabsService;
import androidx.browser.customtabs.CustomTabsSessionToken;
import androidx.browser.customtabs.PostMessageServiceConnection;

import org.json.JSONException;
import org.json.JSONObject;

import org.chromium.base.Callback;
import org.chromium.base.CommandLine;
import org.chromium.base.ContextUtils;
import org.chromium.base.Log;
import org.chromium.base.StrictModeContext;
import org.chromium.base.SysUtils;
import org.chromium.base.ThreadUtils;
import org.chromium.base.TimeUtilsJni;
import org.chromium.base.TraceEvent;
import org.chromium.base.annotations.CalledByNative;
import org.chromium.base.annotations.JNINamespace;
import org.chromium.base.annotations.NativeMethods;
import org.chromium.base.library_loader.LibraryProcessType;
import org.chromium.base.metrics.CachedMetrics.EnumeratedHistogramSample;
import org.chromium.base.metrics.RecordHistogram;
import org.chromium.base.task.PostTask;
import org.chromium.chrome.R;
import org.chromium.chrome.browser.AppHooks;
import org.chromium.chrome.browser.ChromeApplication;
import org.chromium.chrome.browser.IntentHandler;
import org.chromium.chrome.browser.WarmupManager;
import org.chromium.chrome.browser.browserservices.Origin;
import org.chromium.chrome.browser.browserservices.PostMessageHandler;
import org.chromium.chrome.browser.browserservices.SessionDataHolder;
import org.chromium.chrome.browser.browserservices.SessionHandler;
import org.chromium.chrome.browser.customtabs.dynamicmodule.ModuleLoader;
import org.chromium.chrome.browser.customtabs.dynamicmodule.ModuleMetrics;
import org.chromium.chrome.browser.device.DeviceClassManager;
import org.chromium.chrome.browser.flags.ChromeFeatureList;
import org.chromium.chrome.browser.init.ChainedTasks;
import org.chromium.chrome.browser.init.ChromeBrowserInitializer;
import org.chromium.chrome.browser.metrics.PageLoadMetrics;
import org.chromium.chrome.browser.net.spdyproxy.DataReductionProxySettings;
import org.chromium.chrome.browser.preferences.Pref;
import org.chromium.chrome.browser.preferences.PrefServiceBridge;
import org.chromium.chrome.browser.profiles.Profile;
import org.chromium.chrome.browser.settings.privacy.PrivacyPreferencesManager;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.util.IntentUtils;
import org.chromium.chrome.browser.util.UrlConstants;
import org.chromium.content_public.browser.BrowserStartupController;
import org.chromium.content_public.browser.ChildProcessLauncherHelper;
import org.chromium.content_public.browser.UiThreadTaskTraits;
import org.chromium.content_public.browser.WebContents;
import org.chromium.content_public.common.Referrer;
import org.chromium.network.mojom.ReferrerPolicy;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Implementation of the ICustomTabsService interface.
 *
 * Note: This class is meant to be package private, and is public to be
 * accessible from {@link ChromeApplication}.
 */
@JNINamespace("customtabs")
public class CustomTabsConnection {
    private static final String TAG = "ChromeConnection";
    private static final String LOG_SERVICE_REQUESTS = "custom-tabs-log-service-requests";

    // Callback names for |extraCallback()|.
    @VisibleForTesting
    static final String PAGE_LOAD_METRICS_CALLBACK = "NavigationMetrics";
    static final String BOTTOM_BAR_SCROLL_STATE_CALLBACK = "onBottomBarScrollStateChanged";
    @VisibleForTesting
    static final String OPEN_IN_BROWSER_CALLBACK = "onOpenInBrowser";
    @VisibleForTesting
    static final String ON_WARMUP_COMPLETED = "onWarmupCompleted";
    @VisibleForTesting
    static final String ON_DETACHED_REQUEST_REQUESTED = "onDetachedRequestRequested";
    @VisibleForTesting
    static final String ON_DETACHED_REQUEST_COMPLETED = "onDetachedRequestCompleted";

    // For CustomTabs.SpeculationStatusOnStart, see tools/metrics/enums.xml. Append only.
    private static final int SPECULATION_STATUS_ON_START_ALLOWED = 0;
    // What kind of speculation was started, counted in addition to
    // SPECULATION_STATUS_ALLOWED.
    private static final int SPECULATION_STATUS_ON_START_PREFETCH = 1;
    private static final int SPECULATION_STATUS_ON_START_PRERENDER = 2;
    private static final int SPECULATION_STATUS_ON_START_BACKGROUND_TAB = 3;
    private static final int SPECULATION_STATUS_ON_START_PRERENDER_NOT_STARTED = 4;
    // The following describe reasons why a speculation was not allowed, and are
    // counted instead of SPECULATION_STATUS_ALLOWED.
    private static final int SPECULATION_STATUS_ON_START_NOT_ALLOWED_DEVICE_CLASS = 5;
    private static final int SPECULATION_STATUS_ON_START_NOT_ALLOWED_BLOCK_3RD_PARTY_COOKIES = 6;
    private static final int SPECULATION_STATUS_ON_START_NOT_ALLOWED_NETWORK_PREDICTION_DISABLED =
            7;
    private static final int SPECULATION_STATUS_ON_START_NOT_ALLOWED_DATA_REDUCTION_ENABLED = 8;
    private static final int SPECULATION_STATUS_ON_START_NOT_ALLOWED_NETWORK_METERED = 9;
    private static final int SPECULATION_STATUS_ON_START_MAX = 10;

    // For CustomTabs.SpeculationStatusOnSwap, see tools/metrics/enums.xml. Append only.
    private static final int SPECULATION_STATUS_ON_SWAP_BACKGROUND_TAB_TAKEN = 0;
    private static final int SPECULATION_STATUS_ON_SWAP_BACKGROUND_TAB_NOT_MATCHED = 1;
    private static final int SPECULATION_STATUS_ON_SWAP_PRERENDER_TAKEN = 2;
    private static final int SPECULATION_STATUS_ON_SWAP_PRERENDER_NOT_MATCHED = 3;
    private static final int SPECULATION_STATUS_ON_SWAP_MAX = 4;

    // Constants for sending connection characteristics.
    public static final String DATA_REDUCTION_ENABLED = "dataReductionEnabled";

    // "/bg_non_interactive" is from L MR1, "/apps/bg_non_interactive" before,
    // and "background" from O.
    @VisibleForTesting
    static final Set<String> BACKGROUND_GROUPS = new HashSet<>(
            Arrays.asList("/bg_non_interactive", "/apps/bg_non_interactive", "/background"));

    // TODO(lizeb): Move to the support library.
    @VisibleForTesting
    static final String REDIRECT_ENDPOINT_KEY = "android.support.customtabs.REDIRECT_ENDPOINT";
    @VisibleForTesting
    static final String PARALLEL_REQUEST_REFERRER_KEY =
            "android.support.customtabs.PARALLEL_REQUEST_REFERRER";
    static final String PARALLEL_REQUEST_REFERRER_POLICY_KEY =
            "android.support.customtabs.PARALLEL_REQUEST_REFERRER_POLICY";
    @VisibleForTesting
    static final String PARALLEL_REQUEST_URL_KEY =
            "android.support.customtabs.PARALLEL_REQUEST_URL";
    static final String RESOURCE_PREFETCH_URL_LIST_KEY =
            "android.support.customtabs.RESOURCE_PREFETCH_URL_LIST";

    @IntDef({ParallelRequestStatus.NO_REQUEST, ParallelRequestStatus.SUCCESS,
            ParallelRequestStatus.FAILURE_NOT_INITIALIZED,
            ParallelRequestStatus.FAILURE_NOT_AUTHORIZED, ParallelRequestStatus.FAILURE_INVALID_URL,
            ParallelRequestStatus.FAILURE_INVALID_REFERRER,
            ParallelRequestStatus.FAILURE_INVALID_REFERRER_FOR_SESSION})
    @Retention(RetentionPolicy.SOURCE)
    @interface ParallelRequestStatus {
        // Values should start from 0 and can't have gaps (they're used for indexing
        // PARALLEL_REQUEST_MESSAGES).
        @VisibleForTesting
        int NO_REQUEST = 0;
        @VisibleForTesting
        int SUCCESS = 1;
        @VisibleForTesting
        int FAILURE_NOT_INITIALIZED = 2;
        @VisibleForTesting
        int FAILURE_NOT_AUTHORIZED = 3;
        @VisibleForTesting
        int FAILURE_INVALID_URL = 4;
        @VisibleForTesting
        int FAILURE_INVALID_REFERRER = 5;
        @VisibleForTesting
        int FAILURE_INVALID_REFERRER_FOR_SESSION = 6;
        int NUM_ENTRIES = 7;
    }

    private static final String[] PARALLEL_REQUEST_MESSAGES = {"No request", "Success",
            "Chrome not initialized", "Not authorized", "Invalid URL", "Invalid referrer",
            "Invalid referrer for session"};

    private static final EnumeratedHistogramSample sParallelRequestStatusOnStart =
            new EnumeratedHistogramSample(
                    "CustomTabs.ParallelRequestStatusOnStart", ParallelRequestStatus.NUM_ENTRIES);

    private static CustomTabsConnection sInstance;
    private @Nullable String mTrustedPublisherUrlPackage;

    private final HiddenTabHolder mHiddenTabHolder = new HiddenTabHolder();
    protected final SessionDataHolder mSessionDataHolder;
    @VisibleForTesting
    final ClientManager mClientManager;
    protected final boolean mLogRequests;
    private final AtomicBoolean mWarmupHasBeenCalled = new AtomicBoolean();
    private final AtomicBoolean mWarmupHasBeenFinished = new AtomicBoolean();

    @Nullable
    private Callback<CustomTabsSessionToken> mDisconnectCallback;

    // Conversion between native TimeTicks and SystemClock.uptimeMillis().
    private long mNativeTickOffsetUs;
    private boolean mNativeTickOffsetUsComputed;

    private volatile ChainedTasks mWarmupTasks;

    private @Nullable ModuleLoader mModuleLoader;
    /**
     * <strong>DO NOT CALL</strong>
     * Public to be instanciable from {@link ChromeApplication}. This is however
     * intended to be private.
     */
    public CustomTabsConnection() {
        super();
        mClientManager = new ClientManager();
        mLogRequests = CommandLine.getInstance().hasSwitch(LOG_SERVICE_REQUESTS);
        mSessionDataHolder =
                ChromeApplication.getComponent().resolveSessionDataHolder();
    }

    /**
     * @return The unique instance of ChromeCustomTabsConnection.
     */
    public static CustomTabsConnection getInstance() {
        if (sInstance == null) {
            sInstance = AppHooks.get().createCustomTabsConnection();
        }

        return sInstance;
    }

    private static boolean hasInstance() {
        return sInstance != null;
    }

    /**
     * If service requests logging is enabled, logs that a call was made.
     *
     * No rate-limiting, can be spammy if the app is misbehaved.
     *
     * @param name Call name to log.
     * @param result The return value for the logged call.
     */
    void logCall(String name, Object result) {
        if (!mLogRequests) return;
        Log.w(TAG, "%s = %b, Calling UID = %d", name, result, Binder.getCallingUid());
    }

    /**
     * If service requests logging is enabled, logs a callback.
     *
     * No rate-limiting, can be spammy if the app is misbehaved.
     *
     * @param name Callback name to log.
     * @param args arguments of the callback.
     */
    void logCallback(String name, Object args) {
        if (!mLogRequests) return;
        Log.w(TAG, "%s args = %s", name, args);
    }

    /**
     * Converts a Bundle to JSON.
     *
     * The conversion is limited to Bundles not containing any array, and some elements are
     * converted into strings.
     *
     * @param bundle a Bundle to convert.
     * @return A JSON object, empty object if the parameter is null.
     */
    protected static JSONObject bundleToJson(Bundle bundle) {
        JSONObject json = new JSONObject();
        if (bundle == null) return json;
        for (String key : bundle.keySet()) {
            Object o = bundle.get(key);
            try {
                if (o instanceof Bundle) {
                    json.put(key, bundleToJson((Bundle) o));
                } else if (o instanceof Integer || o instanceof Long || o instanceof Boolean) {
                    json.put(key, o);
                } else if (o == null) {
                    json.put(key, JSONObject.NULL);
                } else {
                    json.put(key, o.toString());
                }
            } catch (JSONException e) {
                // Ok, only used for logging.
            }
        }
        return json;
    }

    /*
     * Logging for page load metrics callback, if service has enabled logging.
     *
     * No rate-limiting, can be spammy if the app is misbehaved.
     *
     * @param args arguments of the callback.
     */
    void logPageLoadMetricsCallback(Bundle args) {
        if (!mLogRequests) return; // Don't build args if not necessary.
        logCallback(
                "extraCallback(" + PAGE_LOAD_METRICS_CALLBACK + ")", bundleToJson(args).toString());
    }

    /** Sets a callback to be triggered when a service connection is terminated. */
    public void setDisconnectCallback(@Nullable Callback<CustomTabsSessionToken> callback) {
        mDisconnectCallback = callback;
    }

    public boolean newSession(CustomTabsSessionToken session) {
        boolean success = newSessionInternal(session);
        logCall("newSession()", success);
        return success;
    }

    private boolean newSessionInternal(CustomTabsSessionToken session) {
        if (session == null) return false;
        ClientManager.DisconnectCallback onDisconnect = new ClientManager.DisconnectCallback() {
            @Override
            public void run(CustomTabsSessionToken session) {
                cancelSpeculation(session);
                if (mDisconnectCallback != null) {
                    mDisconnectCallback.onResult(session);
                }

                // TODO(pshmakov): invert this dependency by moving event dispatching to a separate
                // class.
                ChromeApplication.getComponent().resolveCustomTabsFileProcessor()
                        .onSessionDisconnected(session);
            }
        };

        // TODO(peconn): Make this not an anonymous class once PostMessageServiceConnection is made
        // non-abstract in AndroidX.
        PostMessageServiceConnection serviceConnection =
                new PostMessageServiceConnection(session) {};
        PostMessageHandler handler = new PostMessageHandler(serviceConnection);
        return mClientManager.newSession(
                session, Binder.getCallingUid(), onDisconnect, handler, serviceConnection);
    }

    /**
     * Overrides the given session's packageName if it is generated by Chrome. To be used for
     * testing only. To be called before the session given is associated with a tab.
     * @param session The session for which the package name should be overridden.
     * @param packageName The new package name to set.
     */
    public void overridePackageNameForSessionForTesting(
            CustomTabsSessionToken session, String packageName) {
        String originalPackage = getClientPackageNameForSession(session);
        String selfPackage = ContextUtils.getApplicationContext().getPackageName();
        if (TextUtils.isEmpty(originalPackage) || !selfPackage.equals(originalPackage)) return;
        mClientManager.overridePackageNameForSession(session, packageName);
    }

    /** Warmup activities that should only happen once. */
    private static void initializeBrowser(final Context context) {
        ThreadUtils.assertOnUiThread();
        ChromeBrowserInitializer.getInstance().handleSynchronousStartupWithGpuWarmUp();
        ChildProcessLauncherHelper.warmUp(context, true);
    }

    public boolean warmup(long flags) {
        try (TraceEvent e = TraceEvent.scoped("CustomTabsConnection.warmup")) {
            boolean success = warmupInternal(true);
            logCall("warmup()", success);
            return success;
        }
    }

    /**
     * @return Whether {@link CustomTabsConnection#warmup(long)} has been called.
     */
    public boolean hasWarmUpBeenFinished() {
        return mWarmupHasBeenFinished.get();
    }

    /**
     * Starts as much as possible in anticipation of a future navigation.
     *
     * @param mayCreateSpareWebContents true if warmup() can create a spare renderer.
     * @return true for success.
     */
    private boolean warmupInternal(final boolean mayCreateSpareWebContents) {
        // Here and in mayLaunchUrl(), don't do expensive work for background applications.
        if (!isCallerForegroundOrSelf()) return false;
        int uid = Binder.getCallingUid();
        mClientManager.recordUidHasCalledWarmup(uid);
        final boolean initialized = !mWarmupHasBeenCalled.compareAndSet(false, true);

        // The call is non-blocking and this must execute on the UI thread, post chained tasks.
        ChainedTasks tasks = new ChainedTasks();

        // Ordering of actions here:
        // 1. Initializing the browser needs to be done once, and first.
        // 2. Creating a spare renderer takes time, in other threads and processes, so start it
        //    sooner rather than later. Can be done several times.
        // 3. UI inflation has to be done for any new activity.
        // 4. Initializing the LoadingPredictor is done once, and triggers work on other threads,
        //    start it early.
        // 5. RequestThrottler first access has to be done only once.

        // (1)
        if (!initialized) {
            tasks.add(UiThreadTaskTraits.BOOTSTRAP, () -> {
                try (TraceEvent e = TraceEvent.scoped("CustomTabsConnection.initializeBrowser()")) {
                    initializeBrowser(ContextUtils.getApplicationContext());
                    ChromeBrowserInitializer.getInstance().initNetworkChangeNotifier();
                    mWarmupHasBeenFinished.set(true);
                }
            });
        }

        // (2)
        if (mayCreateSpareWebContents && !mHiddenTabHolder.hasHiddenTab()) {
            tasks.add(UiThreadTaskTraits.BOOTSTRAP, () -> {
                // Temporary fix for https://crbug.com/797832.
                // TODO(lizeb): Properly fix instead of papering over the bug, this code should
                // not be scheduled unless startup is done. See https://crbug.com/797832.
                if (!BrowserStartupController.get(LibraryProcessType.PROCESS_BROWSER)
                                .isFullBrowserStarted()) {
                    return;
                }
                try (TraceEvent e = TraceEvent.scoped("CreateSpareWebContents")) {
                    createSpareWebContents();
                }
            });
        }

        // (3)
        tasks.add(UiThreadTaskTraits.BOOTSTRAP, () -> {
            try (TraceEvent e = TraceEvent.scoped("InitializeViewHierarchy")) {
                WarmupManager.getInstance().initializeViewHierarchy(
                        ContextUtils.getApplicationContext(),
                        R.layout.custom_tabs_control_container, R.layout.custom_tabs_toolbar);
            }
        });

        if (!initialized) {
            tasks.add(UiThreadTaskTraits.BOOTSTRAP, () -> {
                try (TraceEvent e = TraceEvent.scoped("WarmupInternalFinishInitialization")) {
                    // (4)
                    Profile profile = Profile.getLastUsedProfile();
                    WarmupManager.startPreconnectPredictorInitialization(profile);

                    // (5)
                    // The throttling database uses shared preferences, that can cause a
                    // StrictMode violation on the first access. Make sure that this access is
                    // not in mayLauchUrl.
                    RequestThrottler.loadInBackground();
                }
            });
        }

        tasks.add(UiThreadTaskTraits.BOOTSTRAP, () -> notifyWarmupIsDone(uid));
        tasks.start(false);
        mWarmupTasks = tasks;
        return true;
    }

    /** @return the URL or null if it's invalid. */
    private boolean isValid(Uri uri) {
        if (uri == null) return false;
        // Don't do anything for unknown schemes. Not having a scheme is allowed, as we allow
        // "www.example.com".
        String scheme = uri.normalizeScheme().getScheme();
        boolean allowedScheme = scheme == null || scheme.equals(UrlConstants.HTTP_SCHEME)
                || scheme.equals(UrlConstants.HTTPS_SCHEME);
        return allowedScheme;
    }

    /**
     * High confidence mayLaunchUrl() call, that is:
     * - Tries to speculate if possible.
     * - An empty URL cancels the current prerender if any.
     * - Start a spare renderer if necessary.
     */
    private void highConfidenceMayLaunchUrl(CustomTabsSessionToken session,
            int uid, String url, Bundle extras, List<Bundle> otherLikelyBundles) {
        ThreadUtils.assertOnUiThread();
        if (TextUtils.isEmpty(url)) {
            cancelSpeculation(session);
            return;
        }

        url = DataReductionProxySettings.getInstance().maybeRewriteWebliteUrl(url);
        if (maySpeculate(session)) {
            boolean canUseHiddenTab = mClientManager.getCanUseHiddenTab(session);
            startSpeculation(session, url, canUseHiddenTab, extras, uid);
        }
        preconnectUrls(otherLikelyBundles);
    }

    /**
     * Low confidence mayLaunchUrl() call, that is:
     * - Preconnects to the ordered list of URLs.
     * - Makes sure that there is a spare renderer.
     */
    @VisibleForTesting
    boolean lowConfidenceMayLaunchUrl(List<Bundle> likelyBundles) {
        ThreadUtils.assertOnUiThread();
        if (!preconnectUrls(likelyBundles)) return false;
        createSpareWebContents();
        return true;
    }

    private boolean preconnectUrls(List<Bundle> likelyBundles) {
        boolean atLeastOneUrl = false;
        if (likelyBundles == null) return false;
        WarmupManager warmupManager = WarmupManager.getInstance();
        Profile profile = Profile.getLastUsedProfile().getOriginalProfile();
        for (Bundle bundle : likelyBundles) {
            Uri uri;
            try {
                uri = IntentUtils.safeGetParcelable(bundle, CustomTabsService.KEY_URL);
            } catch (ClassCastException e) {
                continue;
            }
            if (isValid(uri)) {
                warmupManager.maybePreconnectUrlAndSubResources(profile, uri.toString());
                atLeastOneUrl = true;
            }
        }
        return atLeastOneUrl;
    }

    public boolean mayLaunchUrl(CustomTabsSessionToken session, Uri url, Bundle extras,
            List<Bundle> otherLikelyBundles) {
        try (TraceEvent e = TraceEvent.scoped("CustomTabsConnection.mayLaunchUrl")) {
            boolean success = mayLaunchUrlInternal(session, url, extras, otherLikelyBundles);
            logCall("mayLaunchUrl(" + url + ")", success);
            return success;
        }
    }

    private boolean mayLaunchUrlInternal(final CustomTabsSessionToken session, final Uri url,
            final Bundle extras, final List<Bundle> otherLikelyBundles) {
        final boolean lowConfidence =
                (url == null || TextUtils.isEmpty(url.toString())) && otherLikelyBundles != null;
        final String urlString = isValid(url) ? url.toString() : null;
        if (url != null && urlString == null && !lowConfidence) return false;

        // Things below need the browser process to be initialized.

        // Forbids warmup() from creating a spare renderer, as prerendering wouldn't reuse
        // it. Checking whether prerendering is enabled requires the native library to be loaded,
        // which is not necessarily the case yet.
        if (!warmupInternal(false)) return false; // Also does the foreground check.

        final int uid = Binder.getCallingUid();
        if (!mClientManager.updateStatsAndReturnWhetherAllowed(
                    session, uid, urlString, otherLikelyBundles != null)) {
            return false;
        }

        PostTask.postTask(UiThreadTaskTraits.DEFAULT, () -> {
            doMayLaunchUrlOnUiThread(
                    lowConfidence, session, uid, urlString, extras, otherLikelyBundles, true);
        });
        return true;
    }

    private void doMayLaunchUrlOnUiThread(final boolean lowConfidence,
            final CustomTabsSessionToken session, final int uid, final String urlString,
            final Bundle extras, final List<Bundle> otherLikelyBundles, boolean retryIfNotLoaded) {
        ThreadUtils.assertOnUiThread();
        try (TraceEvent e = TraceEvent.scoped("CustomTabsConnection.mayLaunchUrlOnUiThread")) {
            // doMayLaunchUrlInternal() is always called once the native level initialization is
            // done, at least the initial profile load. However, at that stage the startup callback
            // may not have run, which causes Profile.getLastUsedProfile() to throw an
            // exception. But the tasks have been posted by then, so reschedule ourselves, only
            // once.
            if (!BrowserStartupController.get(LibraryProcessType.PROCESS_BROWSER)
                            .isFullBrowserStarted()) {
                if (retryIfNotLoaded) {
                    PostTask.postTask(UiThreadTaskTraits.DEFAULT, () -> {
                        doMayLaunchUrlOnUiThread(lowConfidence, session, uid, urlString, extras,
                                otherLikelyBundles, false);
                    });
                }
                return;
            }

            if (lowConfidence) {
                lowConfidenceMayLaunchUrl(otherLikelyBundles);
            } else {
                highConfidenceMayLaunchUrl(session, uid, urlString, extras, otherLikelyBundles);
            }
        }
    }

    public Bundle extraCommand(String commandName, Bundle args) {
        return null;
    }

    public boolean updateVisuals(final CustomTabsSessionToken session, Bundle bundle) {
        if (mLogRequests) Log.w(TAG, "updateVisuals: %s", bundleToJson(bundle));
        SessionHandler handler = mSessionDataHolder.getActiveHandler(session);
        if (handler == null) return false;

        final Bundle actionButtonBundle = IntentUtils.safeGetBundle(bundle,
                CustomTabsIntent.EXTRA_ACTION_BUTTON_BUNDLE);
        boolean result = true;
        List<Integer> ids = new ArrayList<>();
        List<String> descriptions = new ArrayList<>();
        List<Bitmap> icons = new ArrayList<>();
        if (actionButtonBundle != null) {
            int id = IntentUtils.safeGetInt(actionButtonBundle, CustomTabsIntent.KEY_ID,
                    CustomTabsIntent.TOOLBAR_ACTION_BUTTON_ID);
            Bitmap bitmap = CustomButtonParams.parseBitmapFromBundle(actionButtonBundle);
            String description = CustomButtonParams.parseDescriptionFromBundle(actionButtonBundle);
            if (bitmap != null && description != null) {
                ids.add(id);
                descriptions.add(description);
                icons.add(bitmap);
            }
        }

        List<Bundle> bundleList = IntentUtils.safeGetParcelableArrayList(
                bundle, CustomTabsIntent.EXTRA_TOOLBAR_ITEMS);
        if (bundleList != null) {
            for (Bundle toolbarItemBundle : bundleList) {
                int id = IntentUtils.safeGetInt(toolbarItemBundle, CustomTabsIntent.KEY_ID,
                        CustomTabsIntent.TOOLBAR_ACTION_BUTTON_ID);
                if (ids.contains(id)) continue;

                Bitmap bitmap = CustomButtonParams.parseBitmapFromBundle(toolbarItemBundle);
                if (bitmap == null) continue;

                String description =
                        CustomButtonParams.parseDescriptionFromBundle(toolbarItemBundle);
                if (description == null) continue;

                ids.add(id);
                descriptions.add(description);
                icons.add(bitmap);
            }
        }

        if (!ids.isEmpty()) {
            result &= PostTask.runSynchronously(UiThreadTaskTraits.DEFAULT, () -> {
                boolean res = true;
                for (int i = 0; i < ids.size(); i++) {
                    res &= handler.updateCustomButton(ids.get(i), icons.get(i),
                            descriptions.get(i));
                }
                return res;
            });
        }

        if (bundle.containsKey(CustomTabsIntent.EXTRA_REMOTEVIEWS)) {
            final RemoteViews remoteViews = IntentUtils.safeGetParcelable(bundle,
                    CustomTabsIntent.EXTRA_REMOTEVIEWS);
            final int[] clickableIDs = IntentUtils.safeGetIntArray(bundle,
                    CustomTabsIntent.EXTRA_REMOTEVIEWS_VIEW_IDS);
            final PendingIntent pendingIntent = IntentUtils.safeGetParcelable(bundle,
                    CustomTabsIntent.EXTRA_REMOTEVIEWS_PENDINGINTENT);
            result &= PostTask.runSynchronously(UiThreadTaskTraits.DEFAULT, () ->
                handler.updateRemoteViews(remoteViews, clickableIDs, pendingIntent)
            );
        }
        logCall("updateVisuals()", result);
        return result;
    }

    public boolean requestPostMessageChannel(CustomTabsSessionToken session,
            Origin postMessageOrigin) {
        boolean success = requestPostMessageChannelInternal(session, postMessageOrigin);
        logCall("requestPostMessageChannel() with origin "
                + (postMessageOrigin != null ? postMessageOrigin.toString() : ""), success);
        return success;
    }

    private boolean requestPostMessageChannelInternal(final CustomTabsSessionToken session,
            final Origin postMessageOrigin) {
        if (!mWarmupHasBeenCalled.get()) return false;
        if (!isCallerForegroundOrSelf() && !mSessionDataHolder.isActiveSession(session)) {
            return false;
        }
        if (!mClientManager.bindToPostMessageServiceForSession(session)) return false;

        final int uid = Binder.getCallingUid();
        PostTask.postTask(UiThreadTaskTraits.DEFAULT, () -> {
            // If the API is not enabled, we don't set the post message origin, which will avoid
            // PostMessageHandler initialization and disallow postMessage calls.
            if (!ChromeFeatureList.isEnabled(ChromeFeatureList.CCT_POST_MESSAGE_API)) return;

            // Attempt to verify origin synchronously. If successful directly initialize postMessage
            // channel for session.
            Uri verifiedOrigin = verifyOriginForSession(session, uid, postMessageOrigin);
            if (verifiedOrigin == null) {
                mClientManager.verifyAndInitializeWithPostMessageOriginForSession(
                        session, postMessageOrigin, CustomTabsService.RELATION_USE_AS_ORIGIN);
            } else {
                mClientManager.initializeWithPostMessageOriginForSession(session, verifiedOrigin);
            }
        });
        return true;
    }

    /**
     * Acquire the origin for the client that owns the given session.
     * @param session The session to use for getting client information.
     * @param clientUid The UID for the client controlling the session.
     * @param origin The origin that is suggested by the client. The validated origin may be this or
     *               a derivative of this.
     * @return The validated origin {@link Uri} for the given session's client.
     */
    protected Uri verifyOriginForSession(
            CustomTabsSessionToken session, int clientUid, Origin origin) {
        if (clientUid == Process.myUid()) return Uri.EMPTY;
        return null;
    }

    public int postMessage(CustomTabsSessionToken session, String message, Bundle extras) {
        int result;
        if (!mWarmupHasBeenCalled.get()) result = CustomTabsService.RESULT_FAILURE_DISALLOWED;
        if (!isCallerForegroundOrSelf() && !mSessionDataHolder.isActiveSession(session)) {
            result = CustomTabsService.RESULT_FAILURE_DISALLOWED;
        }
        // If called before a validatePostMessageOrigin, the post message origin will be invalid and
        // will return a failure result here.
        result = mClientManager.postMessage(session, message);
        logCall("postMessage", result);
        return result;
    }

    public boolean validateRelationship(
            CustomTabsSessionToken sessionToken, int relation, Origin origin, Bundle extras) {
        // Essential parts of the verification will depend on native code and will be run sync on UI
        // thread. Make sure the client has called warmup() beforehand.
        if (!mWarmupHasBeenCalled.get()) {
            Log.d(TAG, "Verification failed due to warmup not having been previously called.");
            mClientManager.getCallbackForSession(sessionToken).onRelationshipValidationResult(
                    relation, Uri.parse(origin.toString()), false, null);
            return false;
        }
        return mClientManager.validateRelationship(sessionToken, relation, origin, extras);
    }

    /**
     * See
     * {@link ClientManager#resetPostMessageHandlerForSession(CustomTabsSessionToken, WebContents)}.
     */
    public void resetPostMessageHandlerForSession(
            CustomTabsSessionToken session, WebContents webContents) {
        mClientManager.resetPostMessageHandlerForSession(session, webContents);
    }

    /**
     * Registers a launch of a |url| for a given |session|.
     *
     * This is used for accounting.
     */
    void registerLaunch(CustomTabsSessionToken session, String url) {
        mClientManager.registerLaunch(session, url);
    }

    @Nullable public String getSpeculatedUrl(CustomTabsSessionToken session) {
        return mHiddenTabHolder.getSpeculatedUrl(session);
    }

    /**
     * Returns the preloaded {@link Tab} if it matches the given |url| and |referrer|. Null if no
     * such {@link Tab}. If a {@link Tab} is preloaded but it does not match, it is discarded.
     *
     * @param session The Binder object identifying a session.
     * @param url The URL the tab is for.
     * @param referrer The referrer to use for |url|.
     * @return The hidden tab, or null.
     */
    @Nullable public Tab takeHiddenTab(@Nullable CustomTabsSessionToken session, String url,
            @Nullable String referrer) {
        return mHiddenTabHolder.takeHiddenTab(session,
                mClientManager.getIgnoreFragmentsForSession(session), url, referrer);
    }

    /**
     * Called when an intent is handled by either an existing or a new CustomTabActivity.
     *
     * @param session Session extracted from the intent.
     * @param intent incoming intent.
     */
    public void onHandledIntent(CustomTabsSessionToken session, Intent intent) {
        String url = IntentHandler.getUrlFromIntent(intent);
        if (TextUtils.isEmpty(url)) {
            return;
        }
        if (mLogRequests) {
            Log.w(TAG, "onHandledIntent, URL: %s, extras: %s", url,
                    bundleToJson(intent.getExtras()));
        }

        // If we still have pending warmup tasks, don't continue as they would only delay intent
        // processing from now on.
        if (mWarmupTasks != null) mWarmupTasks.cancel();

        maybePreconnectToRedirectEndpoint(session, url, intent);
        ChromeBrowserInitializer.getInstance().runNowOrAfterFullBrowserStarted(
                () -> handleParallelRequest(session, intent));
        maybePrefetchResources(session, intent);
    }

    /**
     * Called each time a CCT tab is created to check if a client data header was set and if so
     * forward it along to the native side.
     * @param session Session identifier.
     * @param webContents the WebContents of the new tab.
     */
    public void setClientDataHeaderForNewTab(
            CustomTabsSessionToken session, WebContents webContents) {}

    protected void setClientDataHeader(WebContents webContents, String header) {
        if (TextUtils.isEmpty(header)) return;

        CustomTabsConnectionJni.get().setClientDataHeader(webContents, header);
    }

    private void maybePreconnectToRedirectEndpoint(
            CustomTabsSessionToken session, String url, Intent intent) {
        // For the preconnection to not be a no-op, we need more than just the native library.
        if (!ChromeBrowserInitializer.getInstance().isFullBrowserInitialized()) {
            return;
        }
        if (!ChromeFeatureList.isEnabled(ChromeFeatureList.CCT_REDIRECT_PRECONNECT)) return;

        // Conditions:
        // - There is a valid redirect endpoint.
        // - The URL's origin is first party with respect to the app.
        Uri redirectEndpoint = intent.getParcelableExtra(REDIRECT_ENDPOINT_KEY);
        if (redirectEndpoint == null || !isValid(redirectEndpoint)) return;

        Origin origin = Origin.create(url);
        if (origin == null) return;
        if (!mClientManager.isFirstPartyOriginForSession(session, origin)) return;

        WarmupManager.getInstance().maybePreconnectUrlAndSubResources(
                Profile.getLastUsedProfile(), redirectEndpoint.toString());
    }

    @VisibleForTesting
    @ParallelRequestStatus
    int handleParallelRequest(CustomTabsSessionToken session, Intent intent) {
        int status = maybeStartParallelRequest(session, intent);
        sParallelRequestStatusOnStart.record(status);

        if (mLogRequests) {
            Log.w(TAG, "handleParallelRequest() = " + PARALLEL_REQUEST_MESSAGES[status]);
        }

        if ((status != ParallelRequestStatus.NO_REQUEST)
                && (status != ParallelRequestStatus.FAILURE_NOT_INITIALIZED)
                && (status != ParallelRequestStatus.FAILURE_NOT_AUTHORIZED)
                && ChromeFeatureList.isEnabled(
                           ChromeFeatureList.CCT_REPORT_PARALLEL_REQUEST_STATUS)) {
            Bundle args = new Bundle();
            Uri url = intent.getParcelableExtra(PARALLEL_REQUEST_URL_KEY);
            args.putParcelable("url", url);
            args.putInt("status", status);
            safeExtraCallback(session, ON_DETACHED_REQUEST_REQUESTED, args);
            if (mLogRequests) {
                logCallback(ON_DETACHED_REQUEST_REQUESTED, bundleToJson(args).toString());
            }
        }

        return status;
    }

    /**
     * Maybe starts a parallel request.
     *
     * @param session Calling context session.
     * @param intent Incoming intent with the extras.
     * @return Whether the request was started, with reason in case of failure.
     */
    @ParallelRequestStatus
    private int maybeStartParallelRequest(CustomTabsSessionToken session, Intent intent) {
        ThreadUtils.assertOnUiThread();

        if (!intent.hasExtra(PARALLEL_REQUEST_URL_KEY)) return ParallelRequestStatus.NO_REQUEST;
        if (!ChromeBrowserInitializer.getInstance().isFullBrowserInitialized()) {
            return ParallelRequestStatus.FAILURE_NOT_INITIALIZED;
        }
        if (!mClientManager.getAllowParallelRequestForSession(session)) {
            return ParallelRequestStatus.FAILURE_NOT_AUTHORIZED;
        }
        Uri referrer = intent.getParcelableExtra(PARALLEL_REQUEST_REFERRER_KEY);
        Uri url = intent.getParcelableExtra(PARALLEL_REQUEST_URL_KEY);
        int policy =
                intent.getIntExtra(PARALLEL_REQUEST_REFERRER_POLICY_KEY, ReferrerPolicy.DEFAULT);
        if (url == null) return ParallelRequestStatus.FAILURE_INVALID_URL;
        if (referrer == null) return ParallelRequestStatus.FAILURE_INVALID_REFERRER;
        if (policy < 0 || policy > ReferrerPolicy.LAST) policy = ReferrerPolicy.DEFAULT;

        if (url.toString().equals("") || !isValid(url)) {
            return ParallelRequestStatus.FAILURE_INVALID_URL;
        }
        if (!canDoParallelRequest(session, referrer)) {
            return ParallelRequestStatus.FAILURE_INVALID_REFERRER_FOR_SESSION;
        }

        String urlString = url.toString();
        String referrerString = referrer.toString();
        CustomTabsConnectionJni.get().createAndStartDetachedResourceRequest(
                Profile.getLastUsedProfile(), session, urlString, referrerString, policy,
                DetachedResourceRequestMotivation.PARALLEL_REQUEST);
        if (mLogRequests) {
            Log.w(TAG, "startParallelRequest(%s, %s, %d)", urlString, referrerString, policy);
        }

        return ParallelRequestStatus.SUCCESS;
    }

    /**
     * Maybe starts a resource prefetch.
     *
     * @param session Calling context session.
     * @param intent Incoming intent with the extras.
     * @return Number of prefetch requests that have been sent.
     */
    @VisibleForTesting
    int maybePrefetchResources(CustomTabsSessionToken session, Intent intent) {
        ThreadUtils.assertOnUiThread();

        if (!mClientManager.getAllowResourcePrefetchForSession(session)) return 0;

        List<Uri> resourceList =
                intent.getParcelableArrayListExtra(RESOURCE_PREFETCH_URL_LIST_KEY);
        Uri referrer = intent.getParcelableExtra(PARALLEL_REQUEST_REFERRER_KEY);
        int policy =
                intent.getIntExtra(PARALLEL_REQUEST_REFERRER_POLICY_KEY, ReferrerPolicy.DEFAULT);

        if (resourceList == null || referrer == null) return 0;
        if (policy < 0 || policy > ReferrerPolicy.LAST) policy = ReferrerPolicy.DEFAULT;
        Origin origin = Origin.create(referrer);
        if (origin == null) return 0;
        if (!mClientManager.isFirstPartyOriginForSession(session, origin)) return 0;

        String referrerString = referrer.toString();
        int requestsSent = 0;
        for (Uri url : resourceList) {
            String urlString = url.toString();
            if (urlString.isEmpty() || !isValid(url)) continue;

            // Session is null because we don't need completion notifications.
            CustomTabsConnectionJni.get().createAndStartDetachedResourceRequest(
                    Profile.getLastUsedProfile(), null, urlString, referrerString, policy,
                    DetachedResourceRequestMotivation.RESOURCE_PREFETCH);
            ++requestsSent;

            if (mLogRequests) {
                Log.w(TAG, "startResourcePrefetch(%s, %s, %d)", urlString, referrerString, policy);
            }
        }

        return requestsSent;
    }

    /** @return Whether {@code session} can create a parallel request for a given
     * {@code referrer}.
     */
    @VisibleForTesting
    boolean canDoParallelRequest(CustomTabsSessionToken session, Uri referrer) {
        ThreadUtils.assertOnUiThread();
        Origin origin = Origin.create(referrer);
        if (origin == null) return false;
        return mClientManager.isFirstPartyOriginForSession(session, origin);
    }

    /** See {@link ClientManager#getReferrerForSession(CustomTabsSessionToken)} */
    public Referrer getReferrerForSession(CustomTabsSessionToken session) {
        return mClientManager.getReferrerForSession(session);
    }

    /** @see ClientManager#shouldHideDomainForSession(CustomTabsSessionToken) */
    public boolean shouldHideDomainForSession(CustomTabsSessionToken session) {
        return mClientManager.shouldHideDomainForSession(session);
    }

    /** @see ClientManager#shouldHideTopBarOnModuleManagedUrlsForSession(CustomTabsSessionToken) */
    public boolean shouldHideTopBarOnModuleManagedUrlsForSession(CustomTabsSessionToken session) {
        return mClientManager.shouldHideTopBarOnModuleManagedUrlsForSession(session);
    }

    /** @see ClientManager#shouldSpeculateLoadOnCellularForSession(CustomTabsSessionToken) */
    public boolean shouldSpeculateLoadOnCellularForSession(CustomTabsSessionToken session) {
        return mClientManager.shouldSpeculateLoadOnCellularForSession(session);
    }

    /** @see ClientManager#shouldSendNavigationInfoForSession(CustomTabsSessionToken) */
    public boolean shouldSendNavigationInfoForSession(CustomTabsSessionToken session) {
        return mClientManager.shouldSendNavigationInfoForSession(session);
    }

    /** @see ClientManager#shouldSendBottomBarScrollStateForSession(CustomTabsSessionToken) */
    public boolean shouldSendBottomBarScrollStateForSession(CustomTabsSessionToken session) {
        return mClientManager.shouldSendBottomBarScrollStateForSession(session);
    }

    /** See {@link ClientManager#getClientPackageNameForSession(CustomTabsSessionToken)} */
    public String getClientPackageNameForSession(CustomTabsSessionToken session) {
        return mClientManager.getClientPackageNameForSession(session);
    }

    @VisibleForTesting
    void setIgnoreUrlFragmentsForSession(CustomTabsSessionToken session, boolean value) {
        mClientManager.setIgnoreFragmentsForSession(session, value);
    }

    @VisibleForTesting
    boolean getIgnoreUrlFragmentsForSession(CustomTabsSessionToken session) {
        return mClientManager.getIgnoreFragmentsForSession(session);
    }

    @VisibleForTesting
    void setShouldSpeculateLoadOnCellularForSession(CustomTabsSessionToken session, boolean value) {
        mClientManager.setSpeculateLoadOnCellularForSession(session, value);
    }

    @VisibleForTesting
    void setCanUseHiddenTabForSession(CustomTabsSessionToken session, boolean value) {
        mClientManager.setCanUseHiddenTab(session, value);
    }

    @VisibleForTesting
    void setHideCCTTopBarOnModuleManagedUrls(CustomTabsSessionToken session, boolean value) {
        mClientManager.setHideCCTTopBarOnModuleManagedUrls(session, value);
    }

    /**
     * See {@link ClientManager#setSendNavigationInfoForSession(CustomTabsSessionToken, boolean)}.
     */
    void setSendNavigationInfoForSession(CustomTabsSessionToken session, boolean send) {
        mClientManager.setSendNavigationInfoForSession(session, send);
    }

    /**
     * Extracts the creator package name from the intent.
     * @param intent The intent to get the package name from.
     * @return the package name which can be null.
     */
    String extractCreatorPackage(Intent intent) {
        return null;
    }

    /**
     * Shows a toast about any possible sign in issues encountered during custom tab startup.
     * @param session The session that corresponding custom tab is assigned.
     * @param intent The intent that launched the custom tab.
     */
    void showSignInToastIfNecessary(CustomTabsSessionToken session, Intent intent) { }

    /**
     * Sends a callback using {@link CustomTabsCallback} with the first run result if necessary.
     * @param intentExtras The extras for the initial VIEW intent that initiated first run.
     * @param resultOK Whether first run was successful.
     */
    public void sendFirstRunCallbackIfNecessary(Bundle intentExtras, boolean resultOK) {}

    /**
     * Sends the navigation info that was captured to the client callback.
     * @param session The session to use for getting client callback.
     * @param url The current url for the tab.
     * @param title The current title for the tab.
     * @param snapshotPath Uri location for screenshot of the tab contents which is publicly
     *         available for sharing.
     */
    public void sendNavigationInfo(
            CustomTabsSessionToken session, String url, String title, Uri snapshotPath) {}

    // TODO(yfriedman): Remove when internal code is deleted.
    public void sendNavigationInfo(
            CustomTabsSessionToken session, String url, String title, Bitmap snapshotPath) {}

    /**
     * Called when the bottom bar for the custom tab has been hidden or shown completely by user
     * scroll.
     *
     * @param session The session that is linked with the custom tab.
     * @param hidden Whether the bottom bar is hidden or shown.
     */
    public void onBottomBarScrollStateChanged(CustomTabsSessionToken session, boolean hidden) {
        Bundle args = new Bundle();
        args.putBoolean("hidden", hidden);

        if (safeExtraCallback(session, BOTTOM_BAR_SCROLL_STATE_CALLBACK, args) && mLogRequests) {
            logCallback("extraCallback(" + BOTTOM_BAR_SCROLL_STATE_CALLBACK + ")", hidden);
        }
    }

    /**
     * Notifies the application of a navigation event.
     *
     * Delivers the {@link CustomTabsCallback#onNavigationEvent} callback to the application.
     *
     * @param session The Binder object identifying the session.
     * @param navigationEvent The navigation event code, defined in {@link CustomTabsCallback}
     * @return true for success.
     */
    public boolean notifyNavigationEvent(CustomTabsSessionToken session, int navigationEvent) {
        CustomTabsCallback callback = mClientManager.getCallbackForSession(session);
        if (callback == null) return false;
        try {
            callback.onNavigationEvent(
                    navigationEvent, getExtrasBundleForNavigationEventForSession(session));
        } catch (Exception e) {
            // Catching all exceptions is really bad, but we need it here,
            // because Android exposes us to client bugs by throwing a variety
            // of exceptions. See crbug.com/517023.
            return false;
        }
        logCallback("onNavigationEvent()", navigationEvent);
        return true;
    }

    /**
     * @return The {@link Bundle} to use as extra to
     *         {@link CustomTabsCallback#onNavigationEvent(int, Bundle)}
     */
    protected Bundle getExtrasBundleForNavigationEventForSession(CustomTabsSessionToken session) {
        // SystemClock.uptimeMillis() is used here as it (as of June 2017) uses the same system call
        // as all the native side of Chrome, and this is the same clock used for page load metrics.
        Bundle extras = new Bundle();
        extras.putLong("timestampUptimeMillis", SystemClock.uptimeMillis());
        return extras;
    }

    private void notifyWarmupIsDone(int uid) {
        ThreadUtils.assertOnUiThread();
        // Notifies all the sessions, as warmup() is tied to a UID, not a session.
        for (CustomTabsSessionToken session : mClientManager.uidToSessions(uid)) {
            safeExtraCallback(session, ON_WARMUP_COMPLETED, null);
        }
    }

    /**
     * Creates a Bundle with a value for navigation start and the specified page load metric.
     *
     * @param metricName Name of the page load metric.
     * @param navigationStartTick Absolute navigation start time, as TimeTicks taken from native.
     * @param offsetMs Offset in ms from navigationStart for the page load metric.
     *
     * @return A Bundle containing navigation start and the page load metric.
     */
    Bundle createBundleWithNavigationStartAndPageLoadMetric(
            String metricName, long navigationStartTick, long offsetMs) {
        if (!mNativeTickOffsetUsComputed) {
            // Compute offset from time ticks to uptimeMillis.
            mNativeTickOffsetUsComputed = true;
            long nativeNowUs = TimeUtilsJni.get().getTimeTicksNowUs();
            long javaNowUs = SystemClock.uptimeMillis() * 1000;
            mNativeTickOffsetUs = nativeNowUs - javaNowUs;
        }
        Bundle args = new Bundle();
        args.putLong(metricName, offsetMs);
        // SystemClock.uptimeMillis() is used here as it (as of June 2017) uses the same system call
        // as all the native side of Chrome, that is clock_gettime(CLOCK_MONOTONIC). Meaning that
        // the offset relative to navigationStart is to be compared with a
        // SystemClock.uptimeMillis() value.
        args.putLong(PageLoadMetrics.NAVIGATION_START,
                (navigationStartTick - mNativeTickOffsetUs) / 1000);
        return args;
    }

    /**
     * Notifies the application of a page load metric for a single metric.
     *
     * @param session Session identifier.
     * @param metricName Name of the page load metric.
     * @param navigationStartTick Absolute navigation start time, as TimeTicks taken from native.
     * @param offsetMs Offset in ms from navigationStart for the page load metric.
     *
     * @return Whether the metric has been dispatched to the client.
     */
    boolean notifySinglePageLoadMetric(CustomTabsSessionToken session, String metricName,
            long navigationStartTick, long offsetMs) {
        return notifyPageLoadMetrics(session,
                createBundleWithNavigationStartAndPageLoadMetric(
                        metricName, navigationStartTick, offsetMs));
    }

    /**
     * Notifies the application of a general page load metrics.
     *
     * TODD(lizeb): Move this to a proper method in {@link CustomTabsCallback} once one is
     * available.
     *
     * @param session Session identifier.
     * @param args Bundle containing metric information to update. Each item in the bundle
     *     should be a key specifying the metric name and the metric value as the value.
     */
    boolean notifyPageLoadMetrics(CustomTabsSessionToken session, Bundle args) {
        if (!mClientManager.shouldGetPageLoadMetrics(session)) return false;
        if (safeExtraCallback(session, PAGE_LOAD_METRICS_CALLBACK, args)) {
            logPageLoadMetricsCallback(args);
            return true;
        }
        return false;
    }

    /**
     * Notifies the application that the user has selected to open the page in their browser.
     * @param session Session identifier.
     * @param webContents the WebContents of the tab being taken out of CCT.
     * @return true if success. To protect Chrome exceptions in the client application are swallowed
     *     and false is returned.
     */
    boolean notifyOpenInBrowser(CustomTabsSessionToken session, WebContents webContents) {
        // Reset the client data header for the WebContents since it's not a CCT tab anymore.
        if (webContents != null) CustomTabsConnectionJni.get().setClientDataHeader(webContents, "");
        return safeExtraCallback(session, OPEN_IN_BROWSER_CALLBACK,
                getExtrasBundleForNavigationEventForSession(session));
    }

    /**
     * Wraps calling extraCallback in a try/catch so exceptions thrown by the host app don't crash
     * Chrome. See https://crbug.com/517023.
     */
    protected boolean safeExtraCallback(
            CustomTabsSessionToken session, String callbackName, @Nullable Bundle args) {
        CustomTabsCallback callback = mClientManager.getCallbackForSession(session);
        if (callback == null) return false;

        try {
            callback.extraCallback(callbackName, args);
        } catch (Exception e) {
            return false;
        }
        return true;
    }

    /**
     * Calls {@link CustomTabsCallback#extraCallbackWithResult)}.
     * Wraps calling sendExtraCallbackWithResult in a try/catch so that exceptions thrown by the
     * host app don't crash Chrome.
     */
    @Nullable
    public Bundle sendExtraCallbackWithResult(CustomTabsSessionToken session, String callbackName,
            @Nullable Bundle args) {
        CustomTabsCallback callback = mClientManager.getCallbackForSession(session);
        if (callback == null) return null;

        try {
            return callback.extraCallbackWithResult(callbackName, args);
        } catch (Exception e) {
            return null;
        }
    }

    /**
     * Keeps the application linked with a given session alive.
     *
     * The application is kept alive (that is, raised to at least the current process priority
     * level) until {@link #dontKeepAliveForSession} is called.
     *
     * @param session The Binder object identifying the session.
     * @param intent Intent describing the service to bind to.
     * @return true for success.
     */
    boolean keepAliveForSession(CustomTabsSessionToken session, Intent intent) {
        return mClientManager.keepAliveForSession(session, intent);
    }

    /**
     * Lets the lifetime of the process linked to a given sessionId be managed normally.
     *
     * Without a matching call to {@link #keepAliveForSession}, this is a no-op.
     *
     * @param session The Binder object identifying the session.
     */
    void dontKeepAliveForSession(CustomTabsSessionToken session) {
        mClientManager.dontKeepAliveForSession(session);
    }

    /**
     * Returns whether /proc/PID/ is accessible.
     *
     * On devices where /proc is mounted with the "hidepid=2" option, cannot get access to the
     * scheduler group, as this is under this directory, which is hidden unless PID == self (or
     * its numeric value).
     */
    @VisibleForTesting
    static boolean canGetSchedulerGroup(int pid) {
        String cgroupFilename = "/proc/" + pid;
        File f = new File(cgroupFilename);
        return f.exists() && f.isDirectory() && f.canExecute();
    }

    /**
     * @return the CPU cgroup of a given process, identified by its PID, or null.
     */
    @VisibleForTesting
    static String getSchedulerGroup(int pid) {
        // Android uses several cgroups for processes, depending on their priority. The list of
        // cgroups a process is part of can be queried by reading /proc/<pid>/cgroup, which is
        // world-readable.
        String cgroupFilename = "/proc/" + pid + "/cgroup";
        String controllerName = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O ? "cpuset" : "cpu";
        // Reading from /proc does not cause disk IO, but strict mode doesn't like it.
        // crbug.com/567143
        try (StrictModeContext ignored = StrictModeContext.allowDiskReads();
                BufferedReader reader = new BufferedReader(new FileReader(cgroupFilename))) {
            String line = null;
            while ((line = reader.readLine()) != null) {
                // line format: 2:cpu:/bg_non_interactive
                String[] fields = line.trim().split(":");
                if (fields.length == 3 && fields[1].equals(controllerName)) return fields[2];
            }
        } catch (IOException e) {
            return null;
        }
        return null;
    }

    private static boolean isBackgroundProcess(int pid) {
        return BACKGROUND_GROUPS.contains(getSchedulerGroup(pid));
    }

    /**
     * @return true when inside a Binder transaction and the caller is in the
     * foreground or self. Don't use outside a Binder transaction.
     */
    private boolean isCallerForegroundOrSelf() {
        int uid = Binder.getCallingUid();
        if (uid == Process.myUid()) return true;

        // Starting with L MR1, AM.getRunningAppProcesses doesn't return all the
        // processes. We use a workaround in this case.
        boolean useWorkaround = true;
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.LOLLIPOP_MR1) {
            do {
                Context context = ContextUtils.getApplicationContext();
                ActivityManager am =
                        (ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
                // Extra paranoia here and below, some L 5.0.x devices seem to throw NPE somewhere
                // in this code.
                // See https://crbug.com/654705.
                if (am == null) break;
                List<ActivityManager.RunningAppProcessInfo> running = am.getRunningAppProcesses();
                if (running == null) break;
                for (ActivityManager.RunningAppProcessInfo rpi : running) {
                    if (rpi == null) continue;
                    boolean matchingUid = rpi.uid == uid;
                    boolean isForeground = rpi.importance
                            == ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND;
                    useWorkaround &= !matchingUid;
                    if (matchingUid && isForeground) return true;
                }
            } while (false);
        }
        if (useWorkaround) {
            int pid = Binder.getCallingPid();
            boolean workaroundAvailable = canGetSchedulerGroup(pid);
            // If we have no way to find out whether the calling process is in the foreground,
            // optimistically assume it is. Otherwise we would effectively disable CCT warmup
            // on these devices.
            if (!workaroundAvailable) return true;
            return isBackgroundProcess(pid);
        }
        return false;
    }

    @VisibleForTesting
    void cleanupAll() {
        ThreadUtils.assertOnUiThread();
        mClientManager.cleanupAll();
    }

    /**
     * Handle any clean up left after a session is destroyed.
     * @param session The session that has been destroyed.
     */
    @VisibleForTesting
    void cleanUpSession(final CustomTabsSessionToken session) {
        PostTask.runOrPostTask(
                UiThreadTaskTraits.DEFAULT, () -> mClientManager.cleanupSession(session));
    }

    /**
     * Discards substantial objects that are not currently in use.
     * @param level The type of signal as defined in {@link android.content.ComponentCallbacks2}.
     */
    public static void onTrimMemory(int level) {
        if (!hasInstance()) return;

        if (ChromeApplication.isSevereMemorySignal(level)) {
            getInstance().mClientManager.cleanupUnusedSessions();
        }
        if (getInstance().mModuleLoader != null) getInstance().mModuleLoader.onTrimMemory(level);
    }

    @VisibleForTesting
    int maySpeculateWithResult(CustomTabsSessionToken session) {
        if (!DeviceClassManager.enablePrerendering()) {
            return SPECULATION_STATUS_ON_START_NOT_ALLOWED_DEVICE_CLASS;
        }
        if (PrefServiceBridge.getInstance().getBoolean(Pref.BLOCK_THIRD_PARTY_COOKIES)) {
            return SPECULATION_STATUS_ON_START_NOT_ALLOWED_BLOCK_3RD_PARTY_COOKIES;
        }
        // TODO(yusufo): The check for prerender in PrivacyPreferencesManager now checks for the
        // network connection type as well, we should either change that or add another check for
        // custom tabs. Then that method should be used to make the below check.
        if (!PrivacyPreferencesManager.getInstance().getNetworkPredictionEnabled()) {
            return SPECULATION_STATUS_ON_START_NOT_ALLOWED_NETWORK_PREDICTION_DISABLED;
        }
        if (DataReductionProxySettings.getInstance().isDataReductionProxyEnabled()
                && !ChromeFeatureList.isEnabled(
                        ChromeFeatureList.PREDICTIVE_PREFETCHING_ALLOWED_ON_ALL_CONNECTION_TYPES)) {
            return SPECULATION_STATUS_ON_START_NOT_ALLOWED_DATA_REDUCTION_ENABLED;
        }
        ConnectivityManager cm =
                (ConnectivityManager) ContextUtils.getApplicationContext().getSystemService(
                        Context.CONNECTIVITY_SERVICE);
        if (cm.isActiveNetworkMetered() && !shouldSpeculateLoadOnCellularForSession(session)
                && !ChromeFeatureList.isEnabled(
                        ChromeFeatureList.PREDICTIVE_PREFETCHING_ALLOWED_ON_ALL_CONNECTION_TYPES)) {
            return SPECULATION_STATUS_ON_START_NOT_ALLOWED_NETWORK_METERED;
        }
        return SPECULATION_STATUS_ON_START_ALLOWED;
    }

    boolean maySpeculate(CustomTabsSessionToken session) {
        int speculationResult = maySpeculateWithResult(session);
        recordSpeculationStatusOnStart(speculationResult);
        return speculationResult == SPECULATION_STATUS_ON_START_ALLOWED;
    }

    /** Cancels the speculation for a given session, or any session if null. */
    public void cancelSpeculation(@Nullable CustomTabsSessionToken session) {
        ThreadUtils.assertOnUiThread();
        mHiddenTabHolder.destroyHiddenTab(session);
    }

    /*
     * This function will do as much as it can to have a subsequent navigation
     * to the specified url sped up, including speculatively loading a url, preconnecting,
     * and starting a spare renderer.
     */
    private void startSpeculation(CustomTabsSessionToken session, String url, boolean useHiddenTab,
            Bundle extras, int uid) {
        WarmupManager warmupManager = WarmupManager.getInstance();
        Profile profile = Profile.getLastUsedProfile();

        // At most one on-going speculation, clears the previous one.
        cancelSpeculation(null);

        if (useHiddenTab) {
            recordSpeculationStatusOnStart(SPECULATION_STATUS_ON_START_BACKGROUND_TAB);
            launchUrlInHiddenTab(session, url, extras);
        } else {
            createSpareWebContents();
        }
        warmupManager.maybePreconnectUrlAndSubResources(profile, url);
    }

    /**
     * Creates a hidden tab and initiates a navigation.
     */
    private void launchUrlInHiddenTab(CustomTabsSessionToken session, String url,
            @Nullable Bundle extras) {
        ThreadUtils.assertOnUiThread();
        mHiddenTabHolder.launchUrlInHiddenTab(session, mClientManager, url, extras);
    }

    @VisibleForTesting
    void resetThrottling(int uid) {
        mClientManager.resetThrottling(uid);
    }

    @VisibleForTesting
    void ban(int uid) {
        mClientManager.ban(uid);
    }

    /**
     * Get any referrer that has been explicitly set.
     *
     * Inspects the two possible sources for the referrer:
     * - A session for which the referrer might have been set.
     * - An intent for a navigation that contains a referer in the headers.
     *
     * @param session session to inspect for referrer settings.
     * @param intent intent to inspect for referrer header.
     * @return referrer URL as a string if any was found, empty string otherwise.
     */
    public String getReferrer(CustomTabsSessionToken session, Intent intent) {
        String referrer = IntentHandler.getReferrerUrlIncludingExtraHeaders(intent);
        if (referrer == null && getReferrerForSession(session) != null) {
            referrer = getReferrerForSession(session).getUrl();
        }
        if (referrer == null) referrer = "";
        return referrer;
    }

    /**
     * @return The package name of a client for which the publisher URL from a trusted CDN can be
     *         shown, or null to disallow showing the publisher URL.
     */
    public @Nullable String getTrustedCdnPublisherUrlPackage() {
        return mTrustedPublisherUrlPackage;
    }

    void setTrustedPublisherUrlPackageForTest(@Nullable String packageName) {
        mTrustedPublisherUrlPackage = packageName;
    }

    private static void recordSpeculationStatusOnStart(int status) {
        RecordHistogram.recordEnumeratedHistogram(
                "CustomTabs.SpeculationStatusOnStart", status, SPECULATION_STATUS_ON_START_MAX);
    }

    private static void recordSpeculationStatusOnSwap(int status) {
        RecordHistogram.recordEnumeratedHistogram(
                "CustomTabs.SpeculationStatusOnSwap", status, SPECULATION_STATUS_ON_SWAP_MAX);
    }

    static void recordSpeculationStatusSwapTabTaken() {
        recordSpeculationStatusOnSwap(SPECULATION_STATUS_ON_SWAP_BACKGROUND_TAB_TAKEN);
    }

    static void recordSpeculationStatusSwapTabNotMatched() {
        recordSpeculationStatusOnSwap(SPECULATION_STATUS_ON_SWAP_BACKGROUND_TAB_NOT_MATCHED);
    }

    public ModuleLoader getModuleLoader(ComponentName componentName, @Nullable String assetName) {
        if (!ChromeFeatureList.isEnabled(ChromeFeatureList.CCT_MODULE_DEX_LOADING)) {
            assetName = null;
        }

        if (mModuleLoader != null) {
            boolean isComponentNameChanged =
                    !componentName.equals(mModuleLoader.getComponentName());
            boolean isAssetNameChanged =
                    !TextUtils.equals(assetName, mModuleLoader.getDexAssetName());
            ModuleLoader.ModuleApkVersion newModuleApkVersion =
                    ModuleLoader.ModuleApkVersion.getModuleVersion(componentName.getPackageName());
            boolean isModuleVersionChanged =
                    !mModuleLoader.getModuleApkVersion().equals(newModuleApkVersion);

            if (isComponentNameChanged || isAssetNameChanged || isModuleVersionChanged) {
                mModuleLoader.destroyModule(ModuleMetrics.DestructionReason.MODULE_LOADER_CHANGED);
                mModuleLoader = null;
            }
        }

        if (mModuleLoader == null) mModuleLoader = new ModuleLoader(componentName, assetName);

        return mModuleLoader;
    }

    @CalledByNative
    public static void notifyClientOfDetachedRequestCompletion(
            CustomTabsSessionToken session, String url, int status) {
        if (!ChromeFeatureList.isEnabled(ChromeFeatureList.CCT_REPORT_PARALLEL_REQUEST_STATUS)) {
            return;
        }
        Bundle args = new Bundle();
        args.putParcelable("url", Uri.parse(url));
        args.putInt("net_error", status);
        CustomTabsConnection connection = getInstance();
        connection.safeExtraCallback(session, ON_DETACHED_REQUEST_COMPLETED, args);
        if (connection.mLogRequests) {
            connection.logCallback(ON_DETACHED_REQUEST_COMPLETED, bundleToJson(args).toString());
        }
    }

    @VisibleForTesting
    @Nullable HiddenTabHolder.SpeculationParams getSpeculationParamsForTesting() {
        return mHiddenTabHolder.getSpeculationParamsForTesting();
    }

    public static void createSpareWebContents() {
        if (SysUtils.isLowEndDevice()) return;
        WarmupManager.getInstance().createSpareWebContents(WarmupManager.FOR_CCT);
    }

    public boolean receiveFile(CustomTabsSessionToken sessionToken, Uri uri, int purpose,
            Bundle extras) {
        return ChromeApplication.getComponent().resolveCustomTabsFileProcessor()
                .processFile(sessionToken, uri, purpose, extras);
    }

    @NativeMethods
    interface Natives {
        void createAndStartDetachedResourceRequest(Profile profile, CustomTabsSessionToken session,
                String url, String origin, int referrerPolicy,
                @DetachedResourceRequestMotivation int motivation);
        void setClientDataHeader(WebContents webContents, String header);
    }
}
