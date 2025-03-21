// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.customtabs;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ResolveInfo;
import android.os.Build;
import android.text.TextUtils;

import androidx.annotation.Nullable;
import androidx.annotation.VisibleForTesting;

import org.chromium.base.ContextUtils;
import org.chromium.base.Log;
import org.chromium.base.PackageManagerUtils;
import org.chromium.base.supplier.Supplier;
import org.chromium.chrome.browser.ChromeActivity;
import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.chrome.browser.ShortcutHelper;
import org.chromium.chrome.browser.browserservices.BrowserServicesIntentDataProvider;
import org.chromium.chrome.browser.contextmenu.ChromeContextMenuPopulator;
import org.chromium.chrome.browser.contextmenu.ContextMenuPopulator;
import org.chromium.chrome.browser.customtabs.features.toolbar.CustomTabBrowserControlsVisibilityDelegate;
import org.chromium.chrome.browser.dependency_injection.ActivityScope;
import org.chromium.chrome.browser.externalauth.ExternalAuthUtils;
import org.chromium.chrome.browser.externalnav.ExternalNavigationDelegateImpl;
import org.chromium.chrome.browser.externalnav.ExternalNavigationHandler;
import org.chromium.chrome.browser.flags.ActivityType;
import org.chromium.chrome.browser.fullscreen.ComposedBrowserControlsVisibilityDelegate;
import org.chromium.chrome.browser.multiwindow.MultiWindowUtils;
import org.chromium.chrome.browser.share.ShareDelegate;
import org.chromium.chrome.browser.tab.BrowserControlsVisibilityDelegate;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.tab.TabAssociatedApp;
import org.chromium.chrome.browser.tab.TabContextMenuItemDelegate;
import org.chromium.chrome.browser.tab.TabDelegateFactory;
import org.chromium.chrome.browser.tab.TabLaunchType;
import org.chromium.chrome.browser.tab.TabStateBrowserControlsVisibilityDelegate;
import org.chromium.chrome.browser.tab.TabWebContentsDelegateAndroid;
import org.chromium.chrome.browser.tab_activity_glue.ActivityTabWebContentsDelegateAndroid;
import org.chromium.chrome.browser.tabmodel.TabModel;
import org.chromium.chrome.browser.tabmodel.document.AsyncTabCreationParams;
import org.chromium.chrome.browser.tabmodel.document.TabDelegate;
import org.chromium.chrome.browser.util.IntentUtils;
import org.chromium.chrome.browser.util.UrlUtilities;
import org.chromium.chrome.browser.webapps.WebDisplayMode;
import org.chromium.chrome.browser.webapps.WebappActivity;
import org.chromium.chrome.browser.webapps.WebappExtras;
import org.chromium.chrome.browser.webapps.WebappInfo;
import org.chromium.chrome.browser.webapps.WebappLauncherActivity;
import org.chromium.chrome.browser.webapps.WebappScopePolicy;
import org.chromium.components.embedder_support.delegate.WebContentsDelegateAndroid;
import org.chromium.content_public.browser.LoadUrlParams;
import org.chromium.content_public.common.BrowserControlsState;
import org.chromium.content_public.common.ResourceRequestBody;
import org.chromium.ui.mojom.WindowOpenDisposition;
import org.chromium.webapk.lib.client.WebApkNavigationClient;

import javax.inject.Inject;

/**
 * A {@link TabDelegateFactory} class to be used in all {@link Tab} owned
 * by a {@link CustomTabActivity}.
 */
@ActivityScope
public class CustomTabDelegateFactory implements TabDelegateFactory {
    /** Action for do-nothing activity for activating WebAPK. */
    private static final String ACTION_ACTIVATE_WEBAPK =
            "org.chromium.chrome.browser.webapps.ActivateWebApkActivity.ACTIVATE";

    /**
     * A custom external navigation delegate that forbids the intent picker from showing up.
     */
    static class CustomTabNavigationDelegate extends ExternalNavigationDelegateImpl {
        private static final String TAG = "customtabs";
        private final TabAssociatedApp mTabAssociatedApp;
        private final ExternalAuthUtils mExternalAuthUtils;
        private final ExternalIntentsPolicyProvider mExternalIntentsPolicyProvider;
        private final @ActivityType int mActivityType;

        private boolean mHasActivityStarted;

        /**
         * Constructs a new instance of {@link CustomTabNavigationDelegate}.
         */
        CustomTabNavigationDelegate(Tab tab, ExternalAuthUtils authUtils,
                ExternalIntentsPolicyProvider externalIntentsPolicyProvider,
                @ActivityType int activityType) {
            super(tab);
            mTabAssociatedApp = TabAssociatedApp.from(tab);
            mExternalAuthUtils = authUtils;
            mExternalIntentsPolicyProvider = externalIntentsPolicyProvider;
            mActivityType = activityType;
        }

        @Override
        public void startActivity(Intent intent, boolean proxy) {
            super.startActivity(intent, proxy);
            mHasActivityStarted = true;
        }

        @Override
        public boolean startActivityIfNeeded(Intent intent, boolean proxy) {
            // Note: This method will not be called if applyWebappScopePolicyForUrl returns
            // IGNORE_EXTERNAL_INTENT_REQUESTS.

            boolean isExternalProtocol = !UrlUtilities.isAcceptedScheme(intent.toUri(0));
            boolean hasDefaultHandler = hasDefaultHandler(intent);
            try {
                // For a URL chrome can handle and there is no default set, handle it ourselves.
                if (!hasDefaultHandler) {
                    String clientPackageName = mTabAssociatedApp.getAppId();
                    if (!TextUtils.isEmpty(clientPackageName)
                            && isPackageSpecializedHandler(clientPackageName, intent)) {
                        intent.setPackage(clientPackageName);
                    } else if (!isExternalProtocol) {
                        return false;
                    }
                }

                if (proxy) {
                    dispatchAuthenticatedIntent(intent);
                    mHasActivityStarted = true;
                    return true;
                } else {
                    // If android fails to find a handler, handle it ourselves.
                    Context context = getAvailableContext();
                    if (context instanceof Activity
                            && ((Activity) context).startActivityIfNeeded(intent, -1)) {
                        mHasActivityStarted = true;
                        return true;
                    }
                }
                return false;
            } catch (SecurityException e) {
                // https://crbug.com/808494: Handle the URL in Chrome if dispatching to another
                // application fails with a SecurityException. This happens due to malformed
                // manifests in another app.
                return false;
            } catch (RuntimeException e) {
                IntentUtils.logTransactionTooLargeOrRethrow(e, intent);
                return false;
            }
        }

        /**
         * Resolve the default external handler of an intent.
         * @return Whether the default external handler is found: if chrome turns out to be the
         *         default handler, this method will return false.
         */
        private boolean hasDefaultHandler(Intent intent) {
            ResolveInfo info = PackageManagerUtils.resolveActivity(intent, 0);
            if (info == null) return false;

            final String chromePackage = mApplicationContext.getPackageName();
            // If a default handler is found and it is not chrome itself, fire the intent.
            return info.match != 0 && !chromePackage.equals(info.activityInfo.packageName);
        }

        @Override
        public boolean isIntentForTrustedCallingApp(Intent intent) {
            String clientPackageName = mTabAssociatedApp.getAppId();
            if (TextUtils.isEmpty(clientPackageName)) return false;
            if (!mExternalAuthUtils.isGoogleSigned(clientPackageName)) return false;

            return isPackageSpecializedHandler(clientPackageName, intent);
        }

        /**
         * @return Whether an external activity has started to handle a url. For testing only.
         */
        @VisibleForTesting
        public boolean hasExternalActivityStarted() {
            return mHasActivityStarted;
        }

        @Override
        public int applyWebappScopePolicyForUrl(String url) {
            int scopePolicy = super.applyWebappScopePolicyForUrl(url);
            if (scopePolicy != WebappScopePolicy.NavigationDirective.NORMAL_BEHAVIOR) {
                return scopePolicy;
            }
            return mExternalIntentsPolicyProvider.shouldIgnoreExternalIntentHandlers(url)
                    ? WebappScopePolicy.NavigationDirective.IGNORE_EXTERNAL_INTENT_REQUESTS
                    : WebappScopePolicy.NavigationDirective.NORMAL_BEHAVIOR;
        }
    }

    private static class CustomTabWebContentsDelegate
            extends ActivityTabWebContentsDelegateAndroid {
        private static final String TAG = "CustomTabWebContentsDelegate";
        private final ChromeActivity mActivity;
        private final @ActivityType int mActivityType;
        private final @Nullable String mWebappScopeUrl;
        private final @WebDisplayMode int mDisplayMode;
        private final MultiWindowUtils mMultiWindowUtils;
        private final boolean mShouldEnableEmbeddedMediaExperience;
        private final @Nullable PendingIntent mFocusIntent;

        /**
         * See {@link TabWebContentsDelegateAndroid}.
         */
        public CustomTabWebContentsDelegate(Tab tab, ChromeActivity activity,
                @ActivityType int activityType, @Nullable String webappScopeUrl,
                @WebDisplayMode int displayMode, MultiWindowUtils multiWindowUtils,
                boolean shouldEnableEmbeddedMediaExperience, @Nullable PendingIntent focusIntent) {
            super(tab, activity);
            mActivity = activity;
            mActivityType = activityType;
            mWebappScopeUrl = webappScopeUrl;
            mDisplayMode = displayMode;
            mMultiWindowUtils = multiWindowUtils;
            mShouldEnableEmbeddedMediaExperience = shouldEnableEmbeddedMediaExperience;
            mFocusIntent = focusIntent;
        }

        @Override
        public boolean shouldResumeRequestsForCreatedWindow() {
            return true;
        }

        @Override
        protected void bringActivityToForeground() {
            // TODO: Unify WebAPK and CCT behavior.
            if (isWebappOrWebApk(mActivityType)) {
                bringActivityToForegroundWebapp();
            } else {
                bringActivityToForegroundCustomTab();
            }
        }

        private void bringActivityToForegroundCustomTab() {
            // super.bringActivityToForeground creates an Intent to ChromeLauncherActivity for the
            // current Tab. This will then bring to the foreground the Chrome Task that contains
            // the Chrome Activity displaying the tab (this will usually be a ChromeTabbedActivity).

            // Since Custom Tabs will be launched as part of the client's Task, we can't launch a
            // Chrome Intent specifically to the Chrome Activity in that Task. Therefore we must
            // use an Intent the client has provided to bring its Task to the foreground.

            // If we've not been provided with an Intent to focus the client's Task, we can't do
            // anything.
            if (mFocusIntent == null) return;

            try {
                mFocusIntent.send();
            } catch (PendingIntent.CanceledException e) {
                Log.e(TAG, "CanceledException when sending pending intent.");
            }
        }

        private void bringActivityToForegroundWebapp() {
            WebappActivity webappActivity = (WebappActivity) mActivity;

            // Create an Intent that will be fired toward the WebappLauncherActivity, which in turn
            // will fire an Intent to launch the correct WebappActivity or WebApkActivity. On L+
            // this could probably be changed to call AppTask.moveToFront(), but for backwards
            // compatibility we relaunch it the hard way.
            String startUrl = webappActivity.getWebappInfo().url();

            WebappInfo webappInfo = webappActivity.getWebappInfo();
            if (webappInfo.isForWebApk()) {
                Intent activateIntent = null;
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
                    activateIntent = new Intent(ACTION_ACTIVATE_WEBAPK);
                    activateIntent.setPackage(
                            ContextUtils.getApplicationContext().getPackageName());
                } else {
                    // For WebAPKs with new-style splash screen we cannot activate the WebAPK by
                    // sending an intent because that would relaunch the WebAPK.
                    assert !webappInfo.isSplashProvidedByWebApk();

                    activateIntent = WebApkNavigationClient.createLaunchWebApkIntent(
                            webappInfo.webApkPackageName(), startUrl, false /* forceNavigation */);
                }
                IntentUtils.safeStartActivity(mActivity, activateIntent);
                return;
            }

            Intent intent = new Intent();
            intent.setAction(WebappLauncherActivity.ACTION_START_WEBAPP);
            intent.setPackage(mActivity.getPackageName());
            webappInfo.setWebappIntentExtras(intent);

            intent.putExtra(ShortcutHelper.EXTRA_MAC, ShortcutHelper.getEncodedMac(startUrl));
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            IntentUtils.safeStartActivity(ContextUtils.getApplicationContext(), intent);
        }

        @Override
        protected boolean shouldEnableEmbeddedMediaExperience() {
            return mShouldEnableEmbeddedMediaExperience;
        }

        @Override
        public void openNewTab(String url, String extraHeaders, ResourceRequestBody postData,
                int disposition, boolean isRendererInitiated) {
            // If attempting to open an incognito tab, always send the user to tabbed mode.
            if (disposition == WindowOpenDisposition.OFF_THE_RECORD) {
                if (isRendererInitiated) {
                    throw new IllegalStateException(
                            "Invalid attempt to open an incognito tab from the renderer");
                }
                LoadUrlParams loadUrlParams = new LoadUrlParams(url);
                loadUrlParams.setVerbatimHeaders(extraHeaders);
                loadUrlParams.setPostData(postData);
                loadUrlParams.setIsRendererInitiated(isRendererInitiated);

                Class<? extends ChromeTabbedActivity> tabbedClass =
                        mMultiWindowUtils.getTabbedActivityForIntent(
                                null, ContextUtils.getApplicationContext());
                AsyncTabCreationParams tabParams = new AsyncTabCreationParams(loadUrlParams,
                        new ComponentName(ContextUtils.getApplicationContext(), tabbedClass));
                new TabDelegate(true).createNewTab(tabParams,
                        TabLaunchType.FROM_LONGPRESS_FOREGROUND, TabModel.INVALID_TAB_INDEX);
                return;
            }

            super.openNewTab(url, extraHeaders, postData, disposition, isRendererInitiated);
        }

        @Override
        protected @WebDisplayMode int getDisplayMode() {
            return mDisplayMode;
        }

        @Override
        protected String getManifestScope() {
            return mWebappScopeUrl;
        }

        @Override
        public boolean canShowAppBanners() {
            return mActivityType == ActivityType.CUSTOM_TAB;
        }
    }

    private final ChromeActivity<?> mActivity;
    private final boolean mShouldHideBrowserControls;
    private final boolean mIsOpenedByChrome;
    private final @ActivityType int mActivityType;
    @Nullable
    private final String mWebappScopeUrl;
    private final @WebDisplayMode int mDisplayMode;
    private final boolean mShouldEnableEmbeddedMediaExperience;
    private final BrowserControlsVisibilityDelegate mBrowserStateVisibilityDelegate;
    private final ExternalAuthUtils mExternalAuthUtils;
    private final MultiWindowUtils mMultiWindowUtils;
    private final PendingIntent mFocusIntent;
    private ExternalIntentsPolicyProvider mExternalIntentsPolicyProvider;

    private TabWebContentsDelegateAndroid mWebContentsDelegateAndroid;
    private ExternalNavigationDelegateImpl mNavigationDelegate;

    /**
     * @param activity {@link ChromeActivity} instance.
     * @param shouldHideBrowserControls Whether or not the browser controls may auto-hide.
     * @param isOpenedByChrome Whether the CustomTab was originally opened by Chrome.
     * @param webappScopeUrl The URL of the webapp manifest scope. Null if the delegate is not for a
     *                       webapp.
     * @param displayMode  The activity's display mode.
     * @param shouldEnableEmbeddedMediaExperience Whether embedded media experience is enabled.
     * @param visibilityDelegate The delegate that handles browser control visibility associated
     *                           with browser actions (as opposed to tab state).
     * @param authUtils To determine whether apps are Google signed.
     * @param multiWindowUtils To use to determine which ChromeTabbedActivity to open new tabs in.
     * @param focusIntent A PendingIntent to launch to focus the client.
     */
    private CustomTabDelegateFactory(ChromeActivity<?> activity, boolean shouldHideBrowserControls,
            boolean isOpenedByChrome, @Nullable String webappScopeUrl,
            @WebDisplayMode int displayMode, boolean shouldEnableEmbeddedMediaExperience,
            BrowserControlsVisibilityDelegate visibilityDelegate, ExternalAuthUtils authUtils,
            MultiWindowUtils multiWindowUtils, @Nullable PendingIntent focusIntent,
            ExternalIntentsPolicyProvider externalIntentsPolicyProvider) {
        mActivity = activity;
        mShouldHideBrowserControls = shouldHideBrowserControls;
        mIsOpenedByChrome = isOpenedByChrome;
        mActivityType = (activity == null) ? ActivityType.CUSTOM_TAB : activity.getActivityType();
        mWebappScopeUrl = webappScopeUrl;
        mDisplayMode = displayMode;
        mShouldEnableEmbeddedMediaExperience = shouldEnableEmbeddedMediaExperience;
        mBrowserStateVisibilityDelegate = visibilityDelegate;
        mExternalAuthUtils = authUtils;
        mMultiWindowUtils = multiWindowUtils;
        mFocusIntent = focusIntent;
        mExternalIntentsPolicyProvider = externalIntentsPolicyProvider;
    }

    @Inject
    public CustomTabDelegateFactory(ChromeActivity<?> activity,
            BrowserServicesIntentDataProvider intentDataProvider,
            CustomTabBrowserControlsVisibilityDelegate visibilityDelegate,
            ExternalAuthUtils authUtils, MultiWindowUtils multiWindowUtils,
            ExternalIntentsPolicyProvider externalIntentsPolicyProvider) {
        this(activity, intentDataProvider.shouldEnableUrlBarHiding(),
                intentDataProvider.isOpenedByChrome(), getWebappScopeUrl(intentDataProvider),
                getDisplayMode(intentDataProvider),
                intentDataProvider.shouldEnableEmbeddedMediaExperience(), visibilityDelegate,
                authUtils, multiWindowUtils, intentDataProvider.getFocusIntent(),
                externalIntentsPolicyProvider);
    }

    /**
     * Creates a basic/empty CustomTabDelegateFactory for use when creating a hidden tab. It will
     * be replaced when the hidden Tab becomes shown.
     */
    static CustomTabDelegateFactory createDummy() {
        return new CustomTabDelegateFactory(null, false, false, null, WebDisplayMode.BROWSER, false,
                null, null, null, null, null);
    }

    @Override
    public BrowserControlsVisibilityDelegate createBrowserControlsVisibilityDelegate(Tab tab) {
        TabStateBrowserControlsVisibilityDelegate tabDelegate =
                new TabStateBrowserControlsVisibilityDelegate(tab) {
                    @Override
                    protected int calculateVisibilityConstraints() {
                        @BrowserControlsState
                        int constraints = super.calculateVisibilityConstraints();
                        if (constraints == BrowserControlsState.BOTH
                                && !mShouldHideBrowserControls) {
                            return BrowserControlsState.SHOWN;
                        }
                        return constraints;
                    }
                };

        // mBrowserStateVisibilityDelegate == null for background tabs for which
        // fullscreen state info from BrowserStateVisibilityDelegate is not available.
        return mBrowserStateVisibilityDelegate == null
                ? tabDelegate
                : new ComposedBrowserControlsVisibilityDelegate(
                        tabDelegate, mBrowserStateVisibilityDelegate);
    }

    @Override
    public TabWebContentsDelegateAndroid createWebContentsDelegate(Tab tab) {
        mWebContentsDelegateAndroid = new CustomTabWebContentsDelegate(tab, mActivity,
                mActivityType, mWebappScopeUrl, mDisplayMode, mMultiWindowUtils,
                mShouldEnableEmbeddedMediaExperience, mFocusIntent);
        return mWebContentsDelegateAndroid;
    }

    @Override
    public ExternalNavigationHandler createExternalNavigationHandler(Tab tab) {
        if (mIsOpenedByChrome) {
            mNavigationDelegate = new ExternalNavigationDelegateImpl(tab);
        } else {
            mNavigationDelegate = new CustomTabNavigationDelegate(
                    tab, mExternalAuthUtils, mExternalIntentsPolicyProvider, mActivityType);
        }
        return new ExternalNavigationHandler(mNavigationDelegate);
    }

    @Override
    public ContextMenuPopulator createContextMenuPopulator(Tab tab) {
        @ChromeContextMenuPopulator.ContextMenuMode
        int contextMenuMode = getContextMenuMode(mActivityType);
        Supplier<ShareDelegate> shareDelegateSupplier =
                mActivity == null ? null : mActivity.getShareDelegateSupplier();
        return new ChromeContextMenuPopulator(
                new TabContextMenuItemDelegate(tab), shareDelegateSupplier, contextMenuMode);
    }

    /**
     * @return The {@link CustomTabNavigationDelegate} in this tab. For test purpose only.
     */
    @VisibleForTesting
    ExternalNavigationDelegateImpl getExternalNavigationDelegate() {
        return mNavigationDelegate;
    }

    public WebContentsDelegateAndroid getWebContentsDelegate() {
        return mWebContentsDelegateAndroid;
    }

    /**
     * Gets the scope URL if the {@link BrowserServicesIntentDataProvider} is for a webapp. Returns
     * null otherwise.
     */
    private static @Nullable String getWebappScopeUrl(
            BrowserServicesIntentDataProvider intentDataProvider) {
        WebappExtras webappExtras = intentDataProvider.getWebappExtras();
        return (webappExtras != null) ? webappExtras.scopeUrl : null;
    }

    /**
     * Returns the WebDisplayMode for the passed-in {@link BrowserServicesIntentDataProvider}.
     */
    public static @WebDisplayMode int getDisplayMode(
            BrowserServicesIntentDataProvider intentDataProvider) {
        WebappExtras webappExtras = intentDataProvider.getWebappExtras();
        if (webappExtras != null) {
            return webappExtras.displayMode;
        }
        return intentDataProvider.isTrustedWebActivity() ? WebDisplayMode.STANDALONE
                                                         : WebDisplayMode.BROWSER;
    }

    private static boolean isWebappOrWebApk(@ActivityType int activityType) {
        return activityType == ActivityType.WEBAPP || activityType == ActivityType.WEB_APK;
    }

    private static @ChromeContextMenuPopulator.ContextMenuMode int getContextMenuMode(
            @ActivityType int activityType) {
        return isWebappOrWebApk(activityType)
                ? ChromeContextMenuPopulator.ContextMenuMode.WEB_APP
                : ChromeContextMenuPopulator.ContextMenuMode.CUSTOM_TAB;
    }
}
