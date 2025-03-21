// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.weblayer.shell;

import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.support.v4.app.Fragment;
import android.support.v4.app.FragmentActivity;
import android.support.v4.app.FragmentManager;
import android.support.v4.app.FragmentTransaction;
import android.text.TextUtils;
import android.view.KeyEvent;
import android.view.LayoutInflater;
import android.view.MenuItem;
import android.view.View;
import android.view.ViewGroup.LayoutParams;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.webkit.ValueCallback;
import android.widget.EditText;
import android.widget.ImageButton;
import android.widget.LinearLayout;
import android.widget.PopupMenu;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.TextView.OnEditorActionListener;

import org.chromium.weblayer.Browser;
import org.chromium.weblayer.DownloadCallback;
import org.chromium.weblayer.ErrorPageCallback;
import org.chromium.weblayer.FindInPageCallback;
import org.chromium.weblayer.FullscreenCallback;
import org.chromium.weblayer.Navigation;
import org.chromium.weblayer.NavigationCallback;
import org.chromium.weblayer.NavigationController;
import org.chromium.weblayer.NewTabCallback;
import org.chromium.weblayer.NewTabType;
import org.chromium.weblayer.Profile;
import org.chromium.weblayer.Tab;
import org.chromium.weblayer.TabCallback;
import org.chromium.weblayer.TabListCallback;
import org.chromium.weblayer.UnsupportedVersionException;
import org.chromium.weblayer.WebLayer;

import java.util.ArrayList;
import java.util.List;

/**
 * Activity for managing the Demo Shell.
 */
public class WebLayerShellActivity extends FragmentActivity {
    private static final String TAG = "WebLayerShell";
    private static final String KEY_MAIN_VIEW_ID = "mainViewId";

    private Profile mProfile;
    private Browser mBrowser;
    private EditText mUrlView;
    private ImageButton mMenuButton;
    private ProgressBar mLoadProgressBar;
    private View mMainView;
    private int mMainViewId;
    private View mTopContentsContainer;
    private TabListCallback mTabListCallback;
    private List<Tab> mPreviousTabList = new ArrayList<>();
    private Runnable mExitFullscreenRunnable;

    @Override
    protected void onCreate(final Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        LinearLayout mainView = new LinearLayout(this);
        mainView.setOrientation(LinearLayout.VERTICAL);
        TextView versionText = new TextView(this);
        versionText.setPadding(10, 0, 0, 0);
        versionText.setText(getString(
                R.string.version, WebLayer.getVersion(), WebLayer.getSupportedFullVersion(this)));
        mainView.addView(versionText,
                new LinearLayout.LayoutParams(
                        LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT));
        if (savedInstanceState == null) {
            mMainViewId = View.generateViewId();
        } else {
            mMainViewId = savedInstanceState.getInt(KEY_MAIN_VIEW_ID);
        }
        mainView.setId(mMainViewId);
        mMainView = mainView;
        setContentView(mainView);

        mTopContentsContainer =
                LayoutInflater.from(this).inflate(R.layout.shell_browser_controls, null);

        mUrlView = mTopContentsContainer.findViewById(R.id.url_view);
        // Make the view transparent. Note that setBackgroundColor() applies a color filter to the
        // background drawable rather than replacing it, and thus does not affect layout.
        mUrlView.setBackgroundColor(0);
        mUrlView.setOnEditorActionListener(new OnEditorActionListener() {
            @Override
            public boolean onEditorAction(TextView v, int actionId, KeyEvent event) {
                if ((actionId != EditorInfo.IME_ACTION_GO)
                        && (event == null || event.getKeyCode() != KeyEvent.KEYCODE_ENTER
                                || event.getAction() != KeyEvent.ACTION_DOWN)) {
                    return false;
                }
                loadUrl(mUrlView.getText().toString());
                mUrlView.clearFocus();
                InputMethodManager imm =
                        (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
                imm.hideSoftInputFromWindow(mUrlView.getWindowToken(), 0);
                return true;
            }
        });

        mMenuButton = mTopContentsContainer.findViewById(R.id.menu_button);
        mMenuButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                PopupMenu popup = new PopupMenu(WebLayerShellActivity.this, v);
                popup.getMenuInflater().inflate(R.menu.app_menu, popup.getMenu());
                popup.setOnMenuItemClickListener(new PopupMenu.OnMenuItemClickListener() {
                    @Override
                    public boolean onMenuItemClick(MenuItem item) {
                        if (item.getItemId() == R.id.find_begin_menu_id) {
                            // TODO(estade): add a UI for FIP. For now, just search for "cat", or go
                            // to the next result if a search has already been initiated.
                            mBrowser.getActiveTab().getFindInPageController().setFindInPageCallback(
                                    new FindInPageCallback() {});
                            mBrowser.getActiveTab().getFindInPageController().find("cat", true);
                            return true;
                        }

                        if (item.getItemId() == R.id.find_end_menu_id) {
                            mBrowser.getActiveTab().getFindInPageController().setFindInPageCallback(
                                    null);
                            return true;
                        }

                        return false;
                    }
                });
                popup.show();
            }
        });

        mLoadProgressBar = mTopContentsContainer.findViewById(R.id.progress_bar);

        try {
            // This ensures asynchronous initialization of WebLayer on first start of activity.
            // If activity is re-created during process restart, FragmentManager attaches
            // BrowserFragment immediately, resulting in synchronous init. By the time this line
            // executes, the synchronous init has already happened.
            WebLayer.loadAsync(
                    getApplication(), webLayer -> onWebLayerReady(webLayer, savedInstanceState));
        } catch (UnsupportedVersionException e) {
            throw new RuntimeException("Failed to initialize WebLayer", e);
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (mTabListCallback != null) {
            mBrowser.unregisterTabListCallback(mTabListCallback);
            mTabListCallback = null;
        }
    }

    private void onWebLayerReady(WebLayer webLayer, Bundle savedInstanceState) {
        if (isFinishing() || isDestroyed()) return;

        webLayer.setRemoteDebuggingEnabled(true);

        Fragment fragment = getOrCreateBrowserFragment(savedInstanceState);

        // Have WebLayer Shell retain the fragment instance to simulate the behavior of
        // external embedders (note that if this is changed, then WebLayer Shell should handle
        // rotations and resizes itself via its manifest, as otherwise the user loses all state
        // when the shell is rotated in the foreground).
        fragment.setRetainInstance(true);
        mBrowser = Browser.fromFragment(fragment);
        mTabListCallback = new TabListCallback() {
            @Override
            public void onActiveTabChanged(Tab activeTab) {
                String currentDisplayUrl = getCurrentDisplayUrl();
                if (currentDisplayUrl != null) {
                    mUrlView.setText(currentDisplayUrl);
                }
            }
        };
        mBrowser.registerTabListCallback(mTabListCallback);
        setTabCallbacks(mBrowser.getActiveTab(), fragment);
        mProfile = mBrowser.getProfile();

        mBrowser.setTopView(mTopContentsContainer);

        // If there is already a url loaded in the current tab just display it in the top bar;
        // otherwise load the startup url.
        String currentDisplayUrl = getCurrentDisplayUrl();

        if (currentDisplayUrl != null) {
            mUrlView.setText(currentDisplayUrl);

        } else {
            String startupUrl = getUrlFromIntent(getIntent());
            if (TextUtils.isEmpty(startupUrl)) {
                startupUrl = "https://google.com";
            }
            loadUrl(startupUrl);
        }
    }

    /* Returns the Url for the current tab as a String, or null if there is no
     * current tab. */
    private String getCurrentDisplayUrl() {
        NavigationController navigationController =
                mBrowser.getActiveTab().getNavigationController();

        if (navigationController.getNavigationListSize() == 0) {
            return null;
        }

        return navigationController
                .getNavigationEntryDisplayUri(navigationController.getNavigationListCurrentIndex())
                .toString();
    }

    private void setTabCallbacks(Tab tab, Fragment fragment) {
        tab.setNewTabCallback(new NewTabCallback() {
            @Override
            public void onNewTab(Tab newTab, @NewTabType int type) {
                setTabCallbacks(newTab, fragment);
                mBrowser.getActiveTab().getFindInPageController().setFindInPageCallback(null);
                mPreviousTabList.add(mBrowser.getActiveTab());
                mBrowser.setActiveTab(newTab);
            }

            @Override
            public void onCloseTab() {
                closeTab(tab);
            }
        });
        tab.setFullscreenCallback(new FullscreenCallback() {
            private int mSystemVisibilityToRestore;

            @Override
            public void onEnterFullscreen(Runnable exitFullscreenRunnable) {
                mExitFullscreenRunnable = exitFullscreenRunnable;
                // This comes from Chrome code to avoid an extra resize.
                final WindowManager.LayoutParams attrs = getWindow().getAttributes();
                attrs.flags |= WindowManager.LayoutParams.FLAG_TRANSLUCENT_STATUS;
                getWindow().setAttributes(attrs);

                View decorView = getWindow().getDecorView();
                // Caching the system ui visibility is ok for shell, but likely not ok for
                // real code.
                mSystemVisibilityToRestore = decorView.getSystemUiVisibility();
                decorView.setSystemUiVisibility(View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION // hide nav bar
                        | View.SYSTEM_UI_FLAG_FULLSCREEN // hide status bar
                        | View.SYSTEM_UI_FLAG_LOW_PROFILE | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
            }

            @Override
            public void onExitFullscreen() {
                mExitFullscreenRunnable = null;
                View decorView = getWindow().getDecorView();
                decorView.setSystemUiVisibility(mSystemVisibilityToRestore);

                final WindowManager.LayoutParams attrs = getWindow().getAttributes();
                if ((attrs.flags & WindowManager.LayoutParams.FLAG_TRANSLUCENT_STATUS) != 0) {
                    attrs.flags &= ~WindowManager.LayoutParams.FLAG_TRANSLUCENT_STATUS;
                    getWindow().setAttributes(attrs);
                }
            }
        });
        tab.registerTabCallback(new TabCallback() {
            @Override
            public void onVisibleUriChanged(Uri uri) {
                mUrlView.setText(uri.toString());
            }
        });
        tab.getNavigationController().registerNavigationCallback(new NavigationCallback() {
            @Override
            public void onNavigationStarted(Navigation navigation) {
                mBrowser.getActiveTab().getFindInPageController().setFindInPageCallback(null);
            }

            @Override
            public void onLoadStateChanged(boolean isLoading, boolean toDifferentDocument) {
                mLoadProgressBar.setVisibility(
                        isLoading && toDifferentDocument ? View.VISIBLE : View.INVISIBLE);
            }

            @Override
            public void onLoadProgressChanged(double progress) {
                mLoadProgressBar.setProgress((int) Math.round(100 * progress));
            }
        });
        tab.setDownloadCallback(new DownloadCallback() {
            @Override
            public boolean onInterceptDownload(Uri uri, String userAgent, String contentDisposition,
                    String mimetype, long contentLength) {
                return false;
            }

            @Override
            public void allowDownload(Uri uri, String requestMethod, Uri requestInitiator,
                    ValueCallback<Boolean> callback) {
                callback.onReceiveValue(true);
            }
        });
        tab.setErrorPageCallback(new ErrorPageCallback() {
            @Override
            public boolean onBackToSafety() {
                fragment.getActivity().onBackPressed();
                return true;
            }
        });
    }

    private void closeTab(Tab tab) {
        mPreviousTabList.remove(tab);
        if (mBrowser.getActiveTab() == tab && !mPreviousTabList.isEmpty()) {
            mBrowser.setActiveTab(mPreviousTabList.remove(mPreviousTabList.size() - 1));
        }
        mBrowser.destroyTab(tab);
    }

    private Fragment getOrCreateBrowserFragment(Bundle savedInstanceState) {
        FragmentManager fragmentManager = getSupportFragmentManager();
        if (savedInstanceState != null) {
            // FragmentManager could have re-created the fragment.
            List<Fragment> fragments = fragmentManager.getFragments();
            if (fragments.size() > 1) {
                throw new IllegalStateException("More than one fragment added, shouldn't happen");
            }
            if (fragments.size() == 1) {
                return fragments.get(0);
            }
        }

        String profileName = "DefaultProfile";
        Fragment fragment = WebLayer.createBrowserFragment(profileName);
        FragmentTransaction transaction = fragmentManager.beginTransaction();
        transaction.add(mMainViewId, fragment);

        // Note the commitNow() instead of commit(). We want the fragment to get attached to
        // activity synchronously, so we can use all the functionality immediately. Otherwise we'd
        // have to wait until the commit is executed.
        transaction.commitNow();
        return fragment;
    }

    public void loadUrl(String url) {
        mBrowser.getActiveTab().getNavigationController().navigate(Uri.parse(sanitizeUrl(url)));
        mUrlView.clearFocus();
    }

    private static String getUrlFromIntent(Intent intent) {
        return intent != null ? intent.getDataString() : null;
    }

    /**
     * Given an URL, this performs minimal sanitizing to ensure it will be valid.
     * @param url The url to be sanitized.
     * @return The sanitized URL.
     */
    public static String sanitizeUrl(String url) {
        if (url == null) return null;
        if (url.startsWith("www.") || url.indexOf(":") == -1) url = "http://" + url;
        return url;
    }

    @Override
    public void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        // When restoring Fragments, FragmentManager tries to put them in the containers with same
        // ids as before.
        outState.putInt(KEY_MAIN_VIEW_ID, mMainViewId);
    }

    @Override
    public void onBackPressed() {
        if (mExitFullscreenRunnable != null) {
            mExitFullscreenRunnable.run();
            return;
        }
        if (mBrowser != null) {
            NavigationController controller = mBrowser.getActiveTab().getNavigationController();
            if (controller.canGoBack()) {
                controller.goBack();
                return;
            } else if (!mPreviousTabList.isEmpty()) {
                closeTab(mBrowser.getActiveTab());
                return;
            }
        }
        super.onBackPressed();
    }
}
