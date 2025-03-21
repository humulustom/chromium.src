// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <list>
#include <map>

#include "base/bind.h"
#include "base/feature_list.h"
#include "base/metrics/field_trial.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/scoped_feature_list.h"
#include "build/build_config.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/chrome_notification_types.h"
#include "chrome/browser/extensions/extension_browsertest.h"
#include "chrome/browser/net/proxy_test_utils.h"
#include "chrome/browser/prerender/prerender_manager.h"
#include "chrome/browser/prerender/prerender_origin.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/login/login_handler.h"
#include "chrome/browser/ui/login/login_handler_test_utils.h"
#include "chrome/browser/ui/login/login_interstitial_delegate.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/chrome_features.h"
#include "chrome/common/pref_names.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/prefs/pref_service.h"
#include "components/security_interstitials/content/security_interstitial_tab_helper.h"
#include "components/security_interstitials/content/ssl_blocking_page.h"
#include "content/public/browser/interstitial_page.h"
#include "content/public/browser/notification_details.h"
#include "content/public/browser/notification_source.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_features.h"
#include "content/public/common/network_service_util.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/slow_http_response.h"
#include "content/public/test/test_navigation_observer.h"
#include "net/base/auth.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_response.h"
#include "services/network/public/cpp/features.h"
#include "url/gurl.h"

#if BUILDFLAG(ENABLE_EXTENSIONS)
#include "extensions/common/extension_features.h"
#endif

using content::NavigationController;
using content::OpenURLParams;
using content::Referrer;

namespace {

// A slow HTTP response that serves a WWW-Authenticate header and 401 status
// code.
class SlowAuthResponse : public content::SlowHttpResponse {
 public:
  explicit SlowAuthResponse(const std::string& relative_url)
      : content::SlowHttpResponse(relative_url) {}
  ~SlowAuthResponse() override = default;

  SlowAuthResponse(const SlowAuthResponse& other) = delete;
  SlowAuthResponse operator=(const SlowAuthResponse& other) = delete;

  // content::SlowHttpResponse:
  void AddResponseHeaders(std::string* response) override {
    response->append("WWW-Authenticate: Basic realm=\"test\"\r\n");
    response->append("Cache-Control: max-age=0\r\n");
    // Content-length and Content-type are both necessary to trigger the bug
    // that this class is used to test. Specifically, there must be a delay
    // between the OnAuthRequired notification from the net stack and when the
    // response body is ready, and the OnAuthRequired notification requires
    // headers to be complete (which requires a known content type and length).
    response->append("Content-type: text/html");
    response->append(
        base::StringPrintf("Content-Length: %d\r\n",
                           kFirstResponsePartSize + kSecondResponsePartSize));
  }

  void SetStatusLine(std::string* response) override {
    response->append("HTTP/1.1 401 Unauthorized\r\n");
  }
};

// This request handler returns a WWW-Authenticate header along with a slow
// response body. It is used to exercise a race in how auth requests are
// dispatched to extensions (https://crbug.com/1034468).
std::unique_ptr<net::test_server::HttpResponse> HandleBasicAuthSlowResponse(
    const net::test_server::HttpRequest& request) {
  std::unique_ptr<SlowAuthResponse> response =
      std::make_unique<SlowAuthResponse>(request.relative_url);
  if (!response->IsHandledUrl()) {
    return nullptr;
  }
  return response;
}

// This helper function sets |notification_fired| to true if called. It's used
// as an observer callback for notifications that are not expected to fire.
bool FailIfNotificationFires(bool* notification_fired) {
  *notification_fired = true;
  return true;
}

void TestProxyAuth(Browser* browser, const GURL& test_page) {
  bool https = test_page.SchemeIs(url::kHttpsScheme);

  content::WebContents* contents =
      browser->tab_strip_model()->GetActiveWebContents();
  NavigationController* controller = &contents->GetController();
  LoginPromptBrowserTestObserver observer;
  observer.Register(content::Source<NavigationController>(controller));

  {
    WindowedAuthNeededObserver auth_needed_waiter(controller);
    ui_test_utils::NavigateToURL(browser, test_page);
    auth_needed_waiter.Wait();
  }

  // On HTTPS pages, no error page content should be rendered to avoid origin
  // confusion issues.
  if (https) {
    EXPECT_FALSE(contents->IsLoading());
    EXPECT_EQ("<head></head><body></body>",
              content::EvalJs(contents, "document.documentElement.innerHTML"));
  }

  // The URL should be hidden to avoid origin confusion issues.
  EXPECT_TRUE(browser->location_bar_model()->GetFormattedFullURL().empty());

  // Cancel the prompt, which triggers a reload to read the error page content
  // from the server. On HTTPS pages, the error page content still shouldn't be
  // shown.
  {
    WindowedAuthCancelledObserver auth_cancelled_waiter(controller);
    LoginHandler* handler = observer.handlers().front();
    content::TestNavigationObserver reload_observer(contents);
    handler->CancelAuth();
    auth_cancelled_waiter.Wait();
    reload_observer.Wait();
    if (https) {
      EXPECT_EQ(true, content::EvalJs(contents, "document.body === null"));
    }
    EXPECT_FALSE(browser->location_bar_model()->GetFormattedFullURL().empty());
  }

  // Reload; this time, supply credentials and check that the page loads.
  {
    WindowedAuthNeededObserver auth_needed_waiter(controller);
    browser->OpenURL(OpenURLParams(test_page, Referrer(),
                                   WindowOpenDisposition::CURRENT_TAB,
                                   ui::PAGE_TRANSITION_TYPED, false));
    auth_needed_waiter.Wait();
    EXPECT_TRUE(browser->location_bar_model()->GetFormattedFullURL().empty());
  }

  WindowedAuthSuppliedObserver auth_supplied_waiter(controller);
  LoginHandler* handler = observer.handlers().front();
  handler->SetAuth(base::UTF8ToUTF16("foo"), base::UTF8ToUTF16("bar"));
  auth_supplied_waiter.Wait();

  base::string16 expected_title = base::ASCIIToUTF16("OK");
  content::TitleWatcher title_watcher(contents, expected_title);
  EXPECT_EQ(expected_title, title_watcher.WaitAndGetTitle());
  EXPECT_FALSE(browser->location_bar_model()->GetFormattedFullURL().empty());
}

content::InterstitialPageDelegate* GetInterstitialDelegate(
    content::WebContents* tab) {
  security_interstitials::SecurityInterstitialTabHelper* helper =
      security_interstitials::SecurityInterstitialTabHelper::FromWebContents(
          tab);
  if (!helper)
    return nullptr;
  return helper->GetBlockingPageForCurrentlyCommittedNavigationForTesting();
}

// Tests that a cross origin navigation triggering a login prompt should cause:
// - A login interstitial being displayed.
// - The destination URL being shown in the omnibox.
// Navigates to |visit_url| which triggers an HTTP auth dialog, and checks if
// the URL displayed in the omnibox is equal to |expected_url| after all
// navigations including page redirects are completed.
// If |cancel_prompt| is true, the auth dialog is cancelled at the end.
void TestCrossOriginPrompt(Browser* browser,
                           const GURL& visit_url,
                           const std::string& expected_hostname,
                           bool cancel_prompt) {
  content::WebContents* contents =
      browser->tab_strip_model()->GetActiveWebContents();
  NavigationController* controller = &contents->GetController();
  LoginPromptBrowserTestObserver observer;

  observer.Register(content::Source<NavigationController>(controller));

  // Load a page which will trigger a login prompt.
  WindowedAuthNeededObserver auth_needed_waiter(controller);
  ui_test_utils::NavigateToURL(browser, visit_url);
  auth_needed_waiter.Wait();
  ASSERT_EQ(1u, observer.handlers().size());

  // The omnibox should show the correct origin for the new page when the
  // login prompt is shown.
  EXPECT_EQ(expected_hostname, contents->GetVisibleURL().host());

  if (cancel_prompt) {
    // Cancel, which triggers a reload to get the error page content from the
    // server.
    LoginHandler* handler = *observer.handlers().begin();
    content::TestNavigationObserver reload_observer(contents);
    handler->CancelAuth();
    reload_observer.Wait();
    EXPECT_EQ(expected_hostname, contents->GetVisibleURL().host());
  }
}

enum class SplitAuthCacheByNetworkIsolationKey {
  kFalse,
  kTrue,
};

class LoginPromptBrowserTest
    : public InProcessBrowserTest,
      public testing::WithParamInterface<SplitAuthCacheByNetworkIsolationKey> {
 public:
  LoginPromptBrowserTest()
      : bad_password_("incorrect"),
        bad_username_("nouser"),
        password_("secret"),
        username_basic_("basicuser"),
        username_digest_("digestuser") {
    auth_map_["foo"] = AuthInfo("testuser", "foopassword");
    auth_map_["bar"] = AuthInfo("testuser", "barpassword");
    auth_map_["testrealm"] = AuthInfo(username_basic_, password_);

    // TODO(https://crbug.com/333943): Remove kFtpProtocol feature and FTP
    // credential tests when FTP support is removed.
    if (GetParam() == SplitAuthCacheByNetworkIsolationKey::kFalse) {
      scoped_feature_list_.InitWithFeatures(
          // enabled_features
          {features::kHTTPAuthCommittedInterstitials, features::kFtpProtocol},
          // disabled_features
          {network::features::kSplitAuthCacheByNetworkIsolationKey});
    } else {
      scoped_feature_list_.InitWithFeatures(
          // enabled_features
          {features::kHTTPAuthCommittedInterstitials,
           network::features::kSplitAuthCacheByNetworkIsolationKey,
           features::kFtpProtocol},
          // disabled_features
          {});
    }
  }

  void SetUpOnMainThread() override {
    host_resolver()->AddRule("*", "127.0.0.1");
  }

 protected:
  struct AuthInfo {
    std::string username_;
    std::string password_;

    AuthInfo() {}

    AuthInfo(const std::string& username,
             const std::string& password)
        : username_(username), password_(password) {}
  };

  typedef std::map<std::string, AuthInfo> AuthMap;

  void SetAuthFor(LoginHandler* handler);

  AuthMap auth_map_;
  std::string bad_password_;
  std::string bad_username_;
  std::string password_;
  std::string username_basic_;
  std::string username_digest_;

 private:
  base::test::ScopedFeatureList scoped_feature_list_;
};

INSTANTIATE_TEST_SUITE_P(
    All,
    LoginPromptBrowserTest,
    ::testing::Values(SplitAuthCacheByNetworkIsolationKey::kFalse,
                      SplitAuthCacheByNetworkIsolationKey::kTrue));

void LoginPromptBrowserTest::SetAuthFor(LoginHandler* handler) {
  const net::AuthChallengeInfo& challenge = handler->auth_info();

  auto i = auth_map_.find(challenge.realm);
  EXPECT_TRUE(auth_map_.end() != i);
  if (i != auth_map_.end()) {
    const AuthInfo& info = i->second;
    handler->SetAuth(base::UTF8ToUTF16(info.username_),
                     base::UTF8ToUTF16(info.password_));
  }
}

const char kPrefetchAuthPage[] = "/login/prefetch.html";

const char kMultiRealmTestPage[] = "/login/multi_realm.html";
const int  kMultiRealmTestRealmCount = 2;
const int kMultiRealmTestAuthRequestsCount = 4;

const char kSingleRealmTestPage[] = "/login/single_realm.html";

const char kAuthBasicPage[] = "/auth-basic";
const char kAuthDigestPage[] = "/auth-digest";

// It does not matter what pages are selected as no-auth, as long as they exist.
// Navigating to non-existing pages caused flakes in the past
// (https://crbug.com/636875).
const char kNoAuthPage1[] = "/simple.html";

base::string16 ExpectedTitleFromAuth(const base::string16& username,
                                     const base::string16& password) {
  // The TestServer sets the title to username/password on successful login.
  return username + base::UTF8ToUTF16("/") + password;
}

// Confirm that <link rel="prefetch"> targetting an auth required
// resource does not provide a login dialog.  These types of requests
// should instead just cancel the auth.

// Unfortunately, this test doesn't assert on anything for its
// correctness.  Instead, it relies on the auth dialog blocking the
// browser, and triggering a timeout to cause failure when the
// prefetch resource requires authorization.
IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest, PrefetchAuthCancels) {
  ASSERT_TRUE(embedded_test_server()->Start());
  GURL test_page = embedded_test_server()->GetURL(kPrefetchAuthPage);

  class SetPrefetchForTest {
   public:
    explicit SetPrefetchForTest(bool prefetch)
        : old_prerender_mode_(prerender::PrerenderManager::GetMode()) {
      std::string exp_group = prefetch ? "ExperimentYes" : "ExperimentNo";
      base::FieldTrialList::CreateFieldTrial("Prefetch", exp_group);
      // Disable prerender so this is just a prefetch of the top-level page.
      prerender::PrerenderManager::SetMode(
          prerender::PrerenderManager::PRERENDER_MODE_SIMPLE_LOAD_EXPERIMENT);
    }

    ~SetPrefetchForTest() {
      prerender::PrerenderManager::SetMode(old_prerender_mode_);
    }

   private:
    prerender::PrerenderManager::PrerenderManagerMode old_prerender_mode_;
  } set_prefetch_for_test(true);

  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  NavigationController* controller = &contents->GetController();
  LoginPromptBrowserTestObserver observer;

  observer.Register(content::Source<NavigationController>(controller));

  WindowedLoadStopObserver load_stop_waiter(controller, 1);
  browser()->OpenURL(OpenURLParams(test_page, Referrer(),
                                   WindowOpenDisposition::CURRENT_TAB,
                                   ui::PAGE_TRANSITION_TYPED, false));

  load_stop_waiter.Wait();
  EXPECT_TRUE(observer.handlers().empty());
}

// Test that "Basic" HTTP authentication works.
IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest, TestBasicAuth) {
  ASSERT_TRUE(embedded_test_server()->Start());
  GURL test_page = embedded_test_server()->GetURL(kAuthBasicPage);

  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  NavigationController* controller = &contents->GetController();

  // If the network service crashes, basic auth should still be enabled.
  for (bool crash_network_service : {false, true}) {
    if (crash_network_service) {
      // Can't crash the network service if it isn't running out of process.
      if (!content::IsOutOfProcessNetworkService())
        return;

      SimulateNetworkServiceCrash();
      // Flush the network interface to make sure it notices the crash.
      content::BrowserContext::GetDefaultStoragePartition(browser()->profile())
          ->FlushNetworkInterfaceForTesting();
    }

    LoginPromptBrowserTestObserver observer;

    observer.Register(content::Source<NavigationController>(controller));

    {
      WindowedAuthNeededObserver auth_needed_waiter(controller);
      browser()->OpenURL(OpenURLParams(test_page, Referrer(),
                                       WindowOpenDisposition::CURRENT_TAB,
                                       ui::PAGE_TRANSITION_TYPED, false));
      auth_needed_waiter.Wait();
    }

    ASSERT_FALSE(observer.handlers().empty());
    {
      WindowedAuthNeededObserver auth_needed_waiter(controller);
      WindowedAuthSuppliedObserver auth_supplied_waiter(controller);
      LoginHandler* handler = *observer.handlers().begin();

      ASSERT_TRUE(handler);
      handler->SetAuth(base::UTF8ToUTF16(bad_username_),
                       base::UTF8ToUTF16(bad_password_));
      auth_supplied_waiter.Wait();

      // The request should be retried after the incorrect password is
      // supplied.  This should result in a new AUTH_NEEDED notification
      // for the same realm.
      auth_needed_waiter.Wait();
    }

    ASSERT_EQ(1u, observer.handlers().size());
    WindowedAuthSuppliedObserver auth_supplied_waiter(controller);
    LoginHandler* handler = *observer.handlers().begin();
    SetAuthFor(handler);
    auth_supplied_waiter.Wait();

    base::string16 expected_title = ExpectedTitleFromAuth(
        base::ASCIIToUTF16("basicuser"), base::ASCIIToUTF16("secret"));
    content::TitleWatcher title_watcher(contents, expected_title);
    EXPECT_EQ(expected_title, title_watcher.WaitAndGetTitle());
  }
}

// Test that "Digest" HTTP authentication works.
IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest, TestDigestAuth) {
  ASSERT_TRUE(embedded_test_server()->Start());
  GURL test_page = embedded_test_server()->GetURL(kAuthDigestPage);

  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  NavigationController* controller = &contents->GetController();
  LoginPromptBrowserTestObserver observer;

  observer.Register(content::Source<NavigationController>(controller));

  {
    WindowedAuthNeededObserver auth_needed_waiter(controller);
    browser()->OpenURL(OpenURLParams(test_page, Referrer(),
                                     WindowOpenDisposition::CURRENT_TAB,
                                     ui::PAGE_TRANSITION_TYPED, false));
    auth_needed_waiter.Wait();
  }

  ASSERT_FALSE(observer.handlers().empty());
  {
    WindowedAuthNeededObserver auth_needed_waiter(controller);
    WindowedAuthSuppliedObserver auth_supplied_waiter(controller);
    LoginHandler* handler = *observer.handlers().begin();

    ASSERT_TRUE(handler);
    handler->SetAuth(base::UTF8ToUTF16(bad_username_),
                     base::UTF8ToUTF16(bad_password_));
    auth_supplied_waiter.Wait();

    // The request should be retried after the incorrect password is
    // supplied.  This should result in a new AUTH_NEEDED notification
    // for the same realm.
    auth_needed_waiter.Wait();
  }

  ASSERT_EQ(1u, observer.handlers().size());
  WindowedAuthSuppliedObserver auth_supplied_waiter(controller);
  LoginHandler* handler = *observer.handlers().begin();

  base::string16 username(base::UTF8ToUTF16(username_digest_));
  base::string16 password(base::UTF8ToUTF16(password_));
  handler->SetAuth(username, password);
  auth_supplied_waiter.Wait();

  base::string16 expected_title = ExpectedTitleFromAuth(username, password);
  content::TitleWatcher title_watcher(contents, expected_title);
  EXPECT_EQ(expected_title, title_watcher.WaitAndGetTitle());
}

IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest, TestTwoAuths) {
  ASSERT_TRUE(embedded_test_server()->Start());

  content::WebContents* contents1 =
      browser()->tab_strip_model()->GetActiveWebContents();
  NavigationController* controller1 = &contents1->GetController();
  LoginPromptBrowserTestObserver observer;

  observer.Register(content::Source<NavigationController>(controller1));

  // Open a new tab.
  ui_test_utils::NavigateToURLWithDisposition(
      browser(), GURL("about:blank"), WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_TAB);

  content::WebContents* contents2 =
      browser()->tab_strip_model()->GetActiveWebContents();
  ASSERT_NE(contents1, contents2);
  NavigationController* controller2 = &contents2->GetController();
  observer.Register(content::Source<NavigationController>(controller2));

  {
    WindowedAuthNeededObserver auth_needed_waiter(controller1);
    contents1->OpenURL(OpenURLParams(
        embedded_test_server()->GetURL(kAuthBasicPage), Referrer(),
        WindowOpenDisposition::CURRENT_TAB, ui::PAGE_TRANSITION_TYPED, false));
    auth_needed_waiter.Wait();
  }

  {
    WindowedAuthNeededObserver auth_needed_waiter(controller2);
    contents2->OpenURL(OpenURLParams(
        embedded_test_server()->GetURL(kAuthDigestPage), Referrer(),
        WindowOpenDisposition::CURRENT_TAB, ui::PAGE_TRANSITION_TYPED, false));
    auth_needed_waiter.Wait();
  }

  ASSERT_EQ(2u, observer.handlers().size());

  LoginHandler* handler1 = *observer.handlers().begin();
  LoginHandler* handler2 = *(++(observer.handlers().begin()));

  base::string16 expected_title1 = ExpectedTitleFromAuth(
      base::UTF8ToUTF16(username_basic_), base::UTF8ToUTF16(password_));
  base::string16 expected_title2 = ExpectedTitleFromAuth(
      base::UTF8ToUTF16(username_digest_), base::UTF8ToUTF16(password_));
  content::TitleWatcher title_watcher1(contents1, expected_title1);
  content::TitleWatcher title_watcher2(contents2, expected_title2);

  handler1->SetAuth(base::UTF8ToUTF16(username_basic_),
                    base::UTF8ToUTF16(password_));
  handler2->SetAuth(base::UTF8ToUTF16(username_digest_),
                    base::UTF8ToUTF16(password_));

  EXPECT_EQ(expected_title1, title_watcher1.WaitAndGetTitle());
  EXPECT_EQ(expected_title2, title_watcher2.WaitAndGetTitle());
}

// Test manual login prompt cancellation.
IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest, TestCancelAuth_Manual) {
  ASSERT_TRUE(embedded_test_server()->Start());
  const GURL kAuthURL = embedded_test_server()->GetURL(kAuthBasicPage);

  NavigationController* controller =
      &browser()->tab_strip_model()->GetActiveWebContents()->GetController();

  LoginPromptBrowserTestObserver observer;
  observer.Register(content::Source<NavigationController>(controller));

  WindowedAuthNeededObserver auth_needed_waiter(controller);
  browser()->OpenURL(OpenURLParams(kAuthURL, Referrer(),
                                   WindowOpenDisposition::CURRENT_TAB,
                                   ui::PAGE_TRANSITION_TYPED, false));
  auth_needed_waiter.Wait();
  WindowedAuthCancelledObserver auth_cancelled_waiter(controller);
  LoginHandler* handler = *observer.handlers().begin();
  ASSERT_TRUE(handler);
  content::TestNavigationObserver reload_observer(
      browser()->tab_strip_model()->GetActiveWebContents());
  handler->CancelAuth();
  auth_cancelled_waiter.Wait();
  reload_observer.Wait();
  EXPECT_TRUE(observer.handlers().empty());
}

// Test login prompt cancellation on navigation to a new page.
IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest, TestCancelAuth_OnNavigation) {
  ASSERT_TRUE(embedded_test_server()->Start());
  const GURL kAuthURL = embedded_test_server()->GetURL(kAuthBasicPage);
  const GURL kNoAuthURL = embedded_test_server()->GetURL(kNoAuthPage1);

  NavigationController* controller =
      &browser()->tab_strip_model()->GetActiveWebContents()->GetController();

  LoginPromptBrowserTestObserver observer;
  observer.Register(content::Source<NavigationController>(controller));

  // One LOAD_STOP event for kAuthURL and second for kNoAuthURL.
  WindowedLoadStopObserver load_stop_waiter(controller, 2);
  WindowedAuthNeededObserver auth_needed_waiter(controller);
  browser()->OpenURL(OpenURLParams(kAuthURL, Referrer(),
                                   WindowOpenDisposition::CURRENT_TAB,
                                   ui::PAGE_TRANSITION_TYPED, false));
  WindowedAuthCancelledObserver auth_cancelled_waiter(controller);
  // Navigating while auth is requested is the same as cancelling.
  ui_test_utils::NavigateToURL(browser(), kNoAuthURL);
  auth_cancelled_waiter.Wait();
  load_stop_waiter.Wait();
  EXPECT_TRUE(observer.handlers().empty());
}

// Test login prompt cancellation on navigation to back.
IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest, TestCancelAuth_OnBack) {
  ASSERT_TRUE(embedded_test_server()->Start());
  const GURL kAuthURL = embedded_test_server()->GetURL(kAuthBasicPage);
  const GURL kNoAuthURL = embedded_test_server()->GetURL(kNoAuthPage1);

  NavigationController* controller =
      &browser()->tab_strip_model()->GetActiveWebContents()->GetController();

  LoginPromptBrowserTestObserver observer;
  observer.Register(content::Source<NavigationController>(controller));

  // First navigate to an unauthenticated page so we have something to
  // go back to.
  ui_test_utils::NavigateToURL(browser(), kNoAuthURL);

  WindowedLoadStopObserver load_stop_waiter(controller, 1);
  WindowedAuthNeededObserver auth_needed_waiter(controller);
  browser()->OpenURL(OpenURLParams(kAuthURL, Referrer(),
                                   WindowOpenDisposition::CURRENT_TAB,
                                   ui::PAGE_TRANSITION_TYPED, false));
  auth_needed_waiter.Wait();
  WindowedAuthCancelledObserver auth_cancelled_waiter(controller);
  // Navigating back while auth is requested is the same as cancelling.
  ASSERT_TRUE(controller->CanGoBack());
  controller->GoBack();
  auth_cancelled_waiter.Wait();
  load_stop_waiter.Wait();
  EXPECT_TRUE(observer.handlers().empty());
}

// Test login prompt cancellation on navigation to forward.
IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest, TestCancelAuth_OnForward) {
  ASSERT_TRUE(embedded_test_server()->Start());
  const GURL kAuthURL = embedded_test_server()->GetURL(kAuthBasicPage);
  const GURL kNoAuthURL1 = embedded_test_server()->GetURL(kNoAuthPage1);

  NavigationController* controller =
      &browser()->tab_strip_model()->GetActiveWebContents()->GetController();

  LoginPromptBrowserTestObserver observer;
  observer.Register(content::Source<NavigationController>(controller));

  ui_test_utils::NavigateToURL(browser(), kAuthURL);
  ui_test_utils::NavigateToURL(browser(), kNoAuthURL1);
  ASSERT_TRUE(controller->CanGoBack());
  WindowedAuthNeededObserver auth_needed_waiter(controller);
  controller->GoBack();
  auth_needed_waiter.Wait();

  // Go forward and test that the login prompt is cancelled.
  WindowedAuthCancelledObserver auth_cancelled_waiter(controller);
  ASSERT_TRUE(controller->CanGoForward());
  controller->GoForward();
  auth_cancelled_waiter.Wait();
  EXPECT_TRUE(observer.handlers().empty());
}

// Test handling of resources that require authentication even though
// the page they are included on doesn't.  In this case we should only
// present the minimal number of prompts necessary for successfully
// displaying the page.
class MultiRealmLoginPromptBrowserTest : public LoginPromptBrowserTest {
 public:
  void TearDownOnMainThread() override {
    login_prompt_observer_.UnregisterAll();
    LoginPromptBrowserTest::TearDownOnMainThread();
  }

  // Load the multi-realm test page, waits for LoginHandlers to be created, then
  // calls |for_each_realm_func| once for each authentication realm, passing a
  // LoginHandler for the realm as an argument. The page should stop loading
  // after that.
  template <class F>
  void RunTest(const F& for_each_realm_func);

  NavigationController* GetNavigationController() {
    return &browser()
                ->tab_strip_model()
                ->GetActiveWebContents()
                ->GetController();
  }

  LoginPromptBrowserTestObserver* login_prompt_observer() {
    return &login_prompt_observer_;
  }

 private:
  LoginPromptBrowserTestObserver login_prompt_observer_;
};

INSTANTIATE_TEST_SUITE_P(
    All,
    MultiRealmLoginPromptBrowserTest,
    ::testing::Values(SplitAuthCacheByNetworkIsolationKey::kFalse,
                      SplitAuthCacheByNetworkIsolationKey::kTrue));

template <class F>
void MultiRealmLoginPromptBrowserTest::RunTest(const F& for_each_realm_func) {
  ASSERT_TRUE(embedded_test_server()->Start());
  GURL test_page = embedded_test_server()->GetURL(kMultiRealmTestPage);

  NavigationController* controller = GetNavigationController();

  login_prompt_observer_.Register(
      content::Source<NavigationController>(controller));

  WindowedLoadStopObserver load_stop_waiter(controller, 1);

  browser()->OpenURL(OpenURLParams(test_page, Referrer(),
                                   WindowOpenDisposition::CURRENT_TAB,
                                   ui::PAGE_TRANSITION_TYPED, false));

  // Need to have LoginHandlers created for all requests that need
  // authentication.
  while (login_prompt_observer_.handlers().size() <
         kMultiRealmTestAuthRequestsCount)
    WindowedAuthNeededObserver(controller).Wait();

  // Now confirm or cancel auth once per realm.
  std::set<std::string> seen_realms;
  for (int i = 0; i < kMultiRealmTestRealmCount; ++i) {
    auto it = std::find_if(
        login_prompt_observer_.handlers().begin(),
        login_prompt_observer_.handlers().end(),
        [&seen_realms](LoginHandler* handler) {
          return seen_realms.count(handler->auth_info().realm) == 0;
        });
    ASSERT_TRUE(it != login_prompt_observer_.handlers().end());
    seen_realms.insert((*it)->auth_info().realm);

    for_each_realm_func(*it);
  }

  load_stop_waiter.Wait();
}

// Checks that cancelling works as expected.
IN_PROC_BROWSER_TEST_P(MultiRealmLoginPromptBrowserTest,
                       MultipleRealmCancellation) {
  RunTest([this](LoginHandler* handler) {
    WindowedAuthCancelledObserver waiter(GetNavigationController());
    handler->CancelAuth();
    waiter.Wait();
  });

  EXPECT_EQ(0, login_prompt_observer()->auth_supplied_count());
  EXPECT_LT(0, login_prompt_observer()->auth_needed_count());
  EXPECT_LT(0, login_prompt_observer()->auth_cancelled_count());
}

// Checks that supplying credentials works as expected.
IN_PROC_BROWSER_TEST_P(MultiRealmLoginPromptBrowserTest,
                       MultipleRealmConfirmation) {
  RunTest([this](LoginHandler* handler) {
    WindowedAuthSuppliedObserver waiter(GetNavigationController());
    SetAuthFor(handler);
    waiter.Wait();
  });

  EXPECT_LT(0, login_prompt_observer()->auth_needed_count());
  EXPECT_LT(0, login_prompt_observer()->auth_supplied_count());
  EXPECT_EQ(0, login_prompt_observer()->auth_cancelled_count());
}

// Testing for recovery from an incorrect password for the case where
// there are multiple authenticated resources.
IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest, IncorrectConfirmation) {
  ASSERT_TRUE(embedded_test_server()->Start());
  GURL test_page = embedded_test_server()->GetURL(kSingleRealmTestPage);

  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  NavigationController* controller = &contents->GetController();
  LoginPromptBrowserTestObserver observer;

  observer.Register(content::Source<NavigationController>(controller));

  {
    WindowedAuthNeededObserver auth_needed_waiter(controller);
    browser()->OpenURL(OpenURLParams(test_page, Referrer(),
                                     WindowOpenDisposition::CURRENT_TAB,
                                     ui::PAGE_TRANSITION_TYPED, false));
    auth_needed_waiter.Wait();
  }

  EXPECT_FALSE(observer.handlers().empty());

  if (!observer.handlers().empty()) {
    WindowedAuthNeededObserver auth_needed_waiter(controller);
    WindowedAuthSuppliedObserver auth_supplied_waiter(controller);
    LoginHandler* handler = *observer.handlers().begin();

    ASSERT_TRUE(handler);
    handler->SetAuth(base::UTF8ToUTF16(bad_username_),
                     base::UTF8ToUTF16(bad_password_));
    auth_supplied_waiter.Wait();

    // The request should be retried after the incorrect password is
    // supplied.  This should result in a new AUTH_NEEDED notification
    // for the same realm.
    auth_needed_waiter.Wait();
  }

  int n_handlers = 0;

  while (n_handlers < 1) {
    WindowedAuthNeededObserver auth_needed_waiter(controller);

    while (!observer.handlers().empty()) {
      WindowedAuthSuppliedObserver auth_supplied_waiter(controller);
      LoginHandler* handler = *observer.handlers().begin();

      ASSERT_TRUE(handler);
      n_handlers++;
      SetAuthFor(handler);
      auth_supplied_waiter.Wait();
    }

    if (n_handlers < 1)
      auth_needed_waiter.Wait();
  }

  // The single realm test has only one realm, and thus only one login
  // prompt.
  EXPECT_EQ(1, n_handlers);
  EXPECT_LT(0, observer.auth_needed_count());
  EXPECT_EQ(0, observer.auth_cancelled_count());
  EXPECT_EQ(observer.auth_needed_count(), observer.auth_supplied_count());
}

// If the favicon is an authenticated resource, we shouldn't prompt
// for credentials.  The same URL, if requested elsewhere should
// prompt for credentials.
IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest, NoLoginPromptForFavicon) {
  const char kFaviconTestPage[] = "/login/has_favicon.html";
  const char kFaviconResource[] = "/auth-basic/favicon.gif";

  ASSERT_TRUE(embedded_test_server()->Start());

  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  NavigationController* controller = &contents->GetController();
  LoginPromptBrowserTestObserver observer;

  observer.Register(content::Source<NavigationController>(controller));

  // First load a page that has a favicon that requires
  // authentication.  There should be no login prompt.
  {
    GURL test_page = embedded_test_server()->GetURL(kFaviconTestPage);
    WindowedLoadStopObserver load_stop_waiter(controller, 1);
    browser()->OpenURL(OpenURLParams(test_page, Referrer(),
                                     WindowOpenDisposition::CURRENT_TAB,
                                     ui::PAGE_TRANSITION_TYPED, false));
    load_stop_waiter.Wait();
  }

  // Now request the same favicon, but directly as the document.
  // There should be one login prompt.
  {
    GURL test_page = embedded_test_server()->GetURL(kFaviconResource);
    WindowedLoadStopObserver load_stop_waiter(controller, 1);
    WindowedAuthNeededObserver auth_needed_waiter(controller);
    browser()->OpenURL(OpenURLParams(test_page, Referrer(),
                                     WindowOpenDisposition::CURRENT_TAB,
                                     ui::PAGE_TRANSITION_TYPED, false));
    auth_needed_waiter.Wait();
    ASSERT_EQ(1u, observer.handlers().size());

    while (!observer.handlers().empty()) {
      WindowedAuthCancelledObserver auth_cancelled_waiter(controller);
      LoginHandler* handler = *observer.handlers().begin();

      ASSERT_TRUE(handler);
      handler->CancelAuth();
      auth_cancelled_waiter.Wait();
    }

    load_stop_waiter.Wait();
  }

  EXPECT_EQ(0, observer.auth_supplied_count());
  EXPECT_EQ(1, observer.auth_needed_count());
  EXPECT_EQ(1, observer.auth_cancelled_count());
}

// Block crossdomain image login prompting as a phishing defense.
IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest,
                       BlockCrossdomainPromptForSubresources) {
  const char kTestPage[] = "/login/load_img_from_b.html";

  ASSERT_TRUE(embedded_test_server()->Start());

  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  NavigationController* controller = &contents->GetController();
  LoginPromptBrowserTestObserver observer;
  observer.Register(content::Source<NavigationController>(controller));

  // Load a page that has a cross-domain sub-resource authentication.
  // There should be no login prompt.
  {
    GURL test_page = embedded_test_server()->GetURL(kTestPage);
    ASSERT_EQ("127.0.0.1", test_page.host());

    // Change the host from 127.0.0.1 to www.a.com so that when the
    // page tries to load from b, it will be cross-origin.
    GURL::Replacements replacements;
    replacements.SetHostStr("www.a.com");
    test_page = test_page.ReplaceComponents(replacements);

    WindowedLoadStopObserver load_stop_waiter(controller, 1);
    browser()->OpenURL(OpenURLParams(test_page, Referrer(),
                                     WindowOpenDisposition::CURRENT_TAB,
                                     ui::PAGE_TRANSITION_TYPED, false));
    load_stop_waiter.Wait();
  }

  EXPECT_EQ(0, observer.auth_needed_count());

  // Now request the same page, but from the same origin.
  // There should be one login prompt.
  {
    GURL test_page = embedded_test_server()->GetURL(kTestPage);
    ASSERT_EQ("127.0.0.1", test_page.host());

    // Change the host from 127.0.0.1 to www.b.com so that when the
    // page tries to load from b, it will be same-origin.
    GURL::Replacements replacements;
    replacements.SetHostStr("www.b.com");
    test_page = test_page.ReplaceComponents(replacements);

    WindowedAuthNeededObserver auth_needed_waiter(controller);
    browser()->OpenURL(OpenURLParams(test_page, Referrer(),
                                     WindowOpenDisposition::CURRENT_TAB,
                                     ui::PAGE_TRANSITION_TYPED, false));
    auth_needed_waiter.Wait();
    ASSERT_EQ(1u, observer.handlers().size());

    while (!observer.handlers().empty()) {
      WindowedAuthCancelledObserver auth_cancelled_waiter(controller);
      LoginHandler* handler = *observer.handlers().begin();

      ASSERT_TRUE(handler);
      handler->CancelAuth();
      auth_cancelled_waiter.Wait();
    }
  }

  EXPECT_EQ(1, observer.auth_needed_count());
}

// Deep cross-domain image login prompting should be blocked, too.
IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest,
                       BlockDeepCrossdomainPromptForSubresources) {
  const char kTestPage[] = "/iframe_login_load_img_from_b.html";

  ASSERT_TRUE(embedded_test_server()->Start());

  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  NavigationController* controller = &contents->GetController();
  LoginPromptBrowserTestObserver observer;
  observer.Register(content::Source<NavigationController>(controller));

  // b.com is iframe'd under 127.0.0.1 and includes an image. This is still
  // cross-domain.
  {
    GURL test_page = embedded_test_server()->GetURL(kTestPage);
    ASSERT_EQ("127.0.0.1", test_page.host());

    WindowedLoadStopObserver load_stop_waiter(controller, 1);
    browser()->OpenURL(OpenURLParams(test_page, Referrer(),
                                     WindowOpenDisposition::CURRENT_TAB,
                                     ui::PAGE_TRANSITION_TYPED, false));
    load_stop_waiter.Wait();
  }
  EXPECT_EQ(0, observer.auth_needed_count());

  // b.com iframe'd under b.com and includes an image.
  {
    GURL test_page = embedded_test_server()->GetURL(kTestPage);
    ASSERT_EQ("127.0.0.1", test_page.host());

    // Change the host from 127.0.0.1 to www.b.com so that when the
    // page tries to load from b, it will be same-origin.
    GURL::Replacements replacements;
    replacements.SetHostStr("www.b.com");
    test_page = test_page.ReplaceComponents(replacements);

    WindowedAuthNeededObserver auth_needed_waiter(controller);
    browser()->OpenURL(OpenURLParams(test_page, Referrer(),
                                     WindowOpenDisposition::CURRENT_TAB,
                                     ui::PAGE_TRANSITION_TYPED, false));
    auth_needed_waiter.Wait();
    ASSERT_EQ(1u, observer.handlers().size());

    while (!observer.handlers().empty()) {
      WindowedAuthCancelledObserver auth_cancelled_waiter(controller);
      LoginHandler* handler = *observer.handlers().begin();

      ASSERT_TRUE(handler);
      handler->CancelAuth();
      auth_cancelled_waiter.Wait();
    }
  }

  EXPECT_EQ(1, observer.auth_needed_count());
}

// Block same domain image resource if the top level frame is HTTPS and the
// image resource is HTTP.
// E.g. Top level: https://example.com, Image resource: http://example.com/image
IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest,
                       BlockCrossdomainPromptForSubresourcesMixedContent) {
  ASSERT_TRUE(embedded_test_server()->Start());
  net::EmbeddedTestServer https_server(net::EmbeddedTestServer::TYPE_HTTPS);
  https_server.SetSSLConfig(net::EmbeddedTestServer::CERT_OK);
  https_server.ServeFilesFromSourceDirectory(GetChromeTestDataDir());
  ASSERT_TRUE(https_server.Start());

  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  NavigationController* controller = &contents->GetController();
  LoginPromptBrowserTestObserver observer;
  observer.Register(content::Source<NavigationController>(controller));

  GURL image_url = embedded_test_server()->GetURL("/auth-basic/index.html");
  GURL test_page = https_server.GetURL(
      std::string("/login/load_img_from_same_domain_mixed_content.html?") +
      image_url.spec());
  GURL::Replacements replacements;
  replacements.SetHostStr("a.com");
  test_page = test_page.ReplaceComponents(replacements);
  image_url = image_url.ReplaceComponents(replacements);

  WindowedLoadStopObserver load_stop_waiter(controller, 1);
  browser()->OpenURL(OpenURLParams(test_page, Referrer(),
                                   WindowOpenDisposition::CURRENT_TAB,
                                   ui::PAGE_TRANSITION_TYPED, false));
  load_stop_waiter.Wait();
  EXPECT_EQ(0, observer.auth_needed_count());
}

// Allow crossdomain iframe login prompting despite the above.
IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest,
                       AllowCrossdomainPromptForSubframes) {
  const char kTestPage[] = "/login/load_iframe_from_b.html";

  ASSERT_TRUE(embedded_test_server()->Start());

  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  NavigationController* controller = &contents->GetController();
  LoginPromptBrowserTestObserver observer;
  observer.Register(content::Source<NavigationController>(controller));

  // Load a page that has a cross-domain iframe authentication.
  {
    GURL test_page = embedded_test_server()->GetURL(kTestPage);
    ASSERT_EQ("127.0.0.1", test_page.host());

    // Change the host from 127.0.0.1 to www.a.com so that when the
    // page tries to load from b, it will be cross-origin.
    static const char kNewHost[] = "www.a.com";
    GURL::Replacements replacements;
    replacements.SetHostStr(kNewHost);
    test_page = test_page.ReplaceComponents(replacements);

    WindowedAuthNeededObserver auth_needed_waiter(controller);
    browser()->OpenURL(OpenURLParams(test_page, Referrer(),
                                     WindowOpenDisposition::CURRENT_TAB,
                                     ui::PAGE_TRANSITION_TYPED, false));
    auth_needed_waiter.Wait();
    ASSERT_EQ(1u, observer.handlers().size());

    while (!observer.handlers().empty()) {
      WindowedAuthCancelledObserver auth_cancelled_waiter(controller);
      LoginHandler* handler = *observer.handlers().begin();

      ASSERT_TRUE(handler);
      // When a cross origin iframe displays a login prompt, the blank
      // interstitial shouldn't be displayed and the omnibox should show the
      // main frame's url, not the iframe's.
      EXPECT_EQ(kNewHost, contents->GetVisibleURL().host());

      handler->CancelAuth();
      auth_cancelled_waiter.Wait();
    }
  }

  // Should stay on the main frame's url once the prompt the iframe is closed.
  EXPECT_EQ("www.a.com", contents->GetVisibleURL().host());

  EXPECT_EQ(1, observer.auth_needed_count());
}

IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest, SupplyRedundantAuths) {
  ASSERT_TRUE(embedded_test_server()->Start());

  // Get NavigationController for tab 1.
  content::WebContents* contents_1 =
      browser()->tab_strip_model()->GetActiveWebContents();
  NavigationController* controller_1 = &contents_1->GetController();

  // Open a new tab.
  ui_test_utils::NavigateToURLWithDisposition(
      browser(), GURL("about:blank"), WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_TAB);

  // Get NavigationController for tab 2.
  content::WebContents* contents_2 =
      browser()->tab_strip_model()->GetActiveWebContents();
  ASSERT_NE(contents_1, contents_2);
  NavigationController* controller_2 = &contents_2->GetController();

  LoginPromptBrowserTestObserver observer;
  observer.Register(content::Source<NavigationController>(controller_1));
  observer.Register(content::Source<NavigationController>(controller_2));

  {
    // Open different auth urls in each tab.
    WindowedAuthNeededObserver auth_needed_waiter_1(controller_1);
    contents_1->OpenURL(OpenURLParams(
        embedded_test_server()->GetURL("/auth-basic/1"), content::Referrer(),
        WindowOpenDisposition::CURRENT_TAB, ui::PAGE_TRANSITION_TYPED, false));
    auth_needed_waiter_1.Wait();

    WindowedAuthNeededObserver auth_needed_waiter_2(controller_2);
    contents_2->OpenURL(OpenURLParams(
        embedded_test_server()->GetURL("/auth-basic/2"), content::Referrer(),
        WindowOpenDisposition::CURRENT_TAB, ui::PAGE_TRANSITION_TYPED, false));
    auth_needed_waiter_2.Wait();

    ASSERT_EQ(2U, observer.handlers().size());

    // Supply auth in one of the tabs.
    WindowedAuthSuppliedObserver auth_supplied_waiter_1(controller_1);
    WindowedAuthSuppliedObserver auth_supplied_waiter_2(controller_2);
    LoginHandler* handler_1 = *observer.handlers().begin();
    ASSERT_TRUE(handler_1);
    SetAuthFor(handler_1);

    // Both tabs should be authenticated.
    auth_supplied_waiter_1.Wait();
    auth_supplied_waiter_2.Wait();
  }

  EXPECT_EQ(2, observer.auth_needed_count());
  EXPECT_EQ(2, observer.auth_supplied_count());
  EXPECT_EQ(0, observer.auth_cancelled_count());
}

IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest, CancelRedundantAuths) {
  ASSERT_TRUE(embedded_test_server()->Start());

  // Get NavigationController for tab 1.
  content::WebContents* contents_1 =
      browser()->tab_strip_model()->GetActiveWebContents();
  NavigationController* controller_1 = &contents_1->GetController();

  // Open a new tab.
  ui_test_utils::NavigateToURLWithDisposition(
      browser(), GURL("about:blank"), WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_TAB);

  // Get NavigationController for tab 2.
  content::WebContents* contents_2 =
      browser()->tab_strip_model()->GetActiveWebContents();
  ASSERT_NE(contents_1, contents_2);
  NavigationController* controller_2 = &contents_2->GetController();

  LoginPromptBrowserTestObserver observer;
  observer.Register(content::Source<NavigationController>(controller_1));
  observer.Register(content::Source<NavigationController>(controller_2));

  {
    // Open different auth urls in each tab.
    WindowedAuthNeededObserver auth_needed_waiter_1(controller_1);
    contents_1->OpenURL(OpenURLParams(
        embedded_test_server()->GetURL("/auth-basic/1"), content::Referrer(),
        WindowOpenDisposition::CURRENT_TAB, ui::PAGE_TRANSITION_TYPED, false));
    auth_needed_waiter_1.Wait();

    WindowedAuthNeededObserver auth_needed_waiter_2(controller_2);
    contents_2->OpenURL(OpenURLParams(
        embedded_test_server()->GetURL("/auth-basic/2"), content::Referrer(),
        WindowOpenDisposition::CURRENT_TAB, ui::PAGE_TRANSITION_TYPED, false));
    auth_needed_waiter_2.Wait();

    ASSERT_EQ(2U, observer.handlers().size());

    // Cancel auth in one of the tabs.
    WindowedAuthCancelledObserver auth_cancelled_waiter_1(controller_1);
    WindowedAuthCancelledObserver auth_cancelled_waiter_2(controller_2);
    LoginHandler* handler_1 = *observer.handlers().begin();
    ASSERT_TRUE(handler_1);
    handler_1->CancelAuth();

    // Both tabs should cancel auth.
    auth_cancelled_waiter_1.Wait();
    auth_cancelled_waiter_2.Wait();
  }

  EXPECT_EQ(2, observer.auth_needed_count());
  EXPECT_EQ(0, observer.auth_supplied_count());
  EXPECT_EQ(2, observer.auth_cancelled_count());
}

IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest,
                       SupplyRedundantAuthsMultiProfile) {
  ASSERT_TRUE(embedded_test_server()->Start());

  // Get NavigationController for regular tab.
  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  NavigationController* controller = &contents->GetController();

  // Open an incognito window.
  Browser* browser_incognito = CreateIncognitoBrowser();

  // Get NavigationController for incognito tab.
  content::WebContents* contents_incognito =
      browser_incognito->tab_strip_model()->GetActiveWebContents();
  ASSERT_NE(contents, contents_incognito);
  NavigationController* controller_incognito =
      &contents_incognito->GetController();

  LoginPromptBrowserTestObserver observer;
  observer.Register(content::Source<NavigationController>(controller));
  LoginPromptBrowserTestObserver observer_incognito;
  observer_incognito.Register(
      content::Source<NavigationController>(controller_incognito));

  {
    // Open an auth url in each window.
    WindowedAuthNeededObserver auth_needed_waiter(controller);
    WindowedAuthNeededObserver auth_needed_waiter_incognito(
        controller_incognito);
    contents->OpenURL(OpenURLParams(
        embedded_test_server()->GetURL("/auth-basic/1"), content::Referrer(),
        WindowOpenDisposition::CURRENT_TAB, ui::PAGE_TRANSITION_TYPED, false));
    contents_incognito->OpenURL(OpenURLParams(
        embedded_test_server()->GetURL("/auth-basic/2"), content::Referrer(),
        WindowOpenDisposition::CURRENT_TAB, ui::PAGE_TRANSITION_TYPED, false));
    auth_needed_waiter.Wait();
    auth_needed_waiter_incognito.Wait();

    ASSERT_EQ(1U, observer.handlers().size());
    ASSERT_EQ(1U, observer_incognito.handlers().size());

    // Supply auth in regular tab.
    WindowedAuthSuppliedObserver auth_supplied_waiter(controller);
    LoginHandler* handler = *observer.handlers().begin();
    ASSERT_TRUE(handler);
    SetAuthFor(handler);

    // Regular tab should be authenticated.
    auth_supplied_waiter.Wait();

    // There's not really a way to wait for the incognito window to "do
    // nothing".  Run anything pending in the message loop just to be sure.
    // (This shouldn't be necessary since notifications are synchronous, but
    // maybe it will help avoid flake someday in the future..)
    content::RunAllPendingInMessageLoop();
  }

  EXPECT_EQ(1, observer.auth_needed_count());
  EXPECT_EQ(1, observer.auth_supplied_count());
  EXPECT_EQ(0, observer.auth_cancelled_count());
  EXPECT_EQ(1, observer_incognito.auth_needed_count());
  EXPECT_EQ(0, observer_incognito.auth_supplied_count());
  EXPECT_EQ(0, observer_incognito.auth_cancelled_count());
}

// If an XMLHttpRequest is made with incorrect credentials, there should be no
// login prompt; instead the 401 status should be returned to the script.
IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest,
                       NoLoginPromptForXHRWithBadCredentials) {
  const char kXHRTestPage[] = "/login/xhr_with_credentials.html#incorrect";

  ASSERT_TRUE(embedded_test_server()->Start());

  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  NavigationController* controller = &contents->GetController();
  LoginPromptBrowserTestObserver observer;

  observer.Register(content::Source<NavigationController>(controller));

  // Load a page which makes a synchronous XMLHttpRequest for an authenticated
  // resource with the wrong credentials.  There should be no login prompt.
  {
    GURL test_page = embedded_test_server()->GetURL(kXHRTestPage);
    WindowedLoadStopObserver load_stop_waiter(controller, 1);
    browser()->OpenURL(OpenURLParams(test_page, Referrer(),
                                     WindowOpenDisposition::CURRENT_TAB,
                                     ui::PAGE_TRANSITION_TYPED, false));
    load_stop_waiter.Wait();
  }

  base::string16 expected_title(base::UTF8ToUTF16("status=401"));

  EXPECT_EQ(expected_title, contents->GetTitle());
  EXPECT_EQ(0, observer.auth_supplied_count());
  EXPECT_EQ(0, observer.auth_needed_count());
  EXPECT_EQ(0, observer.auth_cancelled_count());
}

// If an XMLHttpRequest is made with correct credentials, there should be no
// login prompt either.
IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest,
                       NoLoginPromptForXHRWithGoodCredentials) {
  const char kXHRTestPage[] = "/login/xhr_with_credentials.html#secret";

  ASSERT_TRUE(embedded_test_server()->Start());

  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  NavigationController* controller = &contents->GetController();
  LoginPromptBrowserTestObserver observer;

  observer.Register(content::Source<NavigationController>(controller));

  // Load a page which makes a synchronous XMLHttpRequest for an authenticated
  // resource with the wrong credentials.  There should be no login prompt.
  {
    GURL test_page = embedded_test_server()->GetURL(kXHRTestPage);
    WindowedLoadStopObserver load_stop_waiter(controller, 1);
    browser()->OpenURL(OpenURLParams(test_page, Referrer(),
                                     WindowOpenDisposition::CURRENT_TAB,
                                     ui::PAGE_TRANSITION_TYPED, false));
    load_stop_waiter.Wait();
  }

  base::string16 expected_title(base::UTF8ToUTF16("status=200"));

  EXPECT_EQ(expected_title, contents->GetTitle());
  EXPECT_EQ(0, observer.auth_supplied_count());
  EXPECT_EQ(0, observer.auth_needed_count());
  EXPECT_EQ(0, observer.auth_cancelled_count());
}

// If an XMLHttpRequest is made without credentials, there should be a login
// prompt.
IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest,
                       LoginPromptForXHRWithoutCredentials) {
  const char kXHRTestPage[] = "/login/xhr_without_credentials.html";

  ASSERT_TRUE(embedded_test_server()->Start());

  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  NavigationController* controller = &contents->GetController();
  LoginPromptBrowserTestObserver observer;

  observer.Register(content::Source<NavigationController>(controller));

  // Load a page which makes a synchronous XMLHttpRequest for an authenticated
  // resource with the wrong credentials.  There should be no login prompt.
  {
    GURL test_page = embedded_test_server()->GetURL(kXHRTestPage);
    WindowedAuthNeededObserver auth_needed_waiter(controller);
    browser()->OpenURL(OpenURLParams(test_page, Referrer(),
                                     WindowOpenDisposition::CURRENT_TAB,
                                     ui::PAGE_TRANSITION_TYPED, false));
    auth_needed_waiter.Wait();
  }

  ASSERT_FALSE(observer.handlers().empty());
  {
    WindowedAuthNeededObserver auth_needed_waiter(controller);
    WindowedAuthSuppliedObserver auth_supplied_waiter(controller);
    LoginHandler* handler = *observer.handlers().begin();

    ASSERT_TRUE(handler);
    handler->SetAuth(base::UTF8ToUTF16(bad_username_),
                     base::UTF8ToUTF16(bad_password_));
    auth_supplied_waiter.Wait();

    // The request should be retried after the incorrect password is
    // supplied.  This should result in a new AUTH_NEEDED notification
    // for the same realm.
    auth_needed_waiter.Wait();
  }

  ASSERT_EQ(1u, observer.handlers().size());
  WindowedAuthSuppliedObserver auth_supplied_waiter(controller);
  LoginHandler* handler = *observer.handlers().begin();

  base::string16 username(base::UTF8ToUTF16(username_digest_));
  base::string16 password(base::UTF8ToUTF16(password_));
  handler->SetAuth(username, password);
  auth_supplied_waiter.Wait();

  WindowedLoadStopObserver load_stop_waiter(controller, 1);
  load_stop_waiter.Wait();

  base::string16 expected_title(base::UTF8ToUTF16("status=200"));

  EXPECT_EQ(expected_title, contents->GetTitle());
  EXPECT_EQ(2, observer.auth_supplied_count());
  EXPECT_EQ(2, observer.auth_needed_count());
  EXPECT_EQ(0, observer.auth_cancelled_count());
}

// If an XMLHttpRequest is made without credentials, there should be a login
// prompt.  If it's cancelled, the script should get a 401 status.
IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest,
                       LoginPromptForXHRWithoutCredentialsCancelled) {
  const char kXHRTestPage[] = "/login/xhr_without_credentials.html";

  ASSERT_TRUE(embedded_test_server()->Start());

  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  NavigationController* controller = &contents->GetController();
  LoginPromptBrowserTestObserver observer;

  observer.Register(content::Source<NavigationController>(controller));

  // Load a page which makes a synchronous XMLHttpRequest for an authenticated
  // resource with the wrong credentials.  There should be no login prompt.
  {
    GURL test_page = embedded_test_server()->GetURL(kXHRTestPage);
    WindowedAuthNeededObserver auth_needed_waiter(controller);
    browser()->OpenURL(OpenURLParams(test_page, Referrer(),
                                     WindowOpenDisposition::CURRENT_TAB,
                                     ui::PAGE_TRANSITION_TYPED, false));
    auth_needed_waiter.Wait();
  }

  ASSERT_EQ(1u, observer.handlers().size());
  WindowedAuthCancelledObserver auth_cancelled_waiter(controller);
  LoginHandler* handler = *observer.handlers().begin();

  handler->CancelAuth();
  auth_cancelled_waiter.Wait();

  WindowedLoadStopObserver load_stop_waiter(controller, 1);
  load_stop_waiter.Wait();

  base::string16 expected_title(base::UTF8ToUTF16("status=401"));

  EXPECT_EQ(expected_title, contents->GetTitle());
  EXPECT_EQ(0, observer.auth_supplied_count());
  EXPECT_EQ(1, observer.auth_needed_count());
  EXPECT_EQ(1, observer.auth_cancelled_count());
}

// Test that the auth cache respects NetworkIsolationKeys when splitting the
// cache based on the key is enabled.
IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest,
                       AuthCacheAcrossNetworkIsolationKeys) {
  ASSERT_TRUE(embedded_test_server()->Start());
  GURL test_page = embedded_test_server()->GetURL(kAuthBasicPage);

  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  NavigationController* controller = &contents->GetController();

  LoginPromptBrowserTestObserver observer;
  observer.Register(content::Source<NavigationController>(controller));
  WindowedAuthNeededObserver auth_needed_waiter(controller);
  browser()->OpenURL(OpenURLParams(test_page, Referrer(),
                                   WindowOpenDisposition::CURRENT_TAB,
                                   ui::PAGE_TRANSITION_TYPED, false));
  auth_needed_waiter.Wait();

  ASSERT_EQ(1u, observer.handlers().size());
  WindowedAuthSuppliedObserver auth_supplied_waiter(controller);
  LoginHandler* handler = *observer.handlers().begin();
  SetAuthFor(handler);
  auth_supplied_waiter.Wait();

  base::string16 expected_title = ExpectedTitleFromAuth(
      base::ASCIIToUTF16("basicuser"), base::ASCIIToUTF16("secret"));
  content::TitleWatcher title_watcher(contents, expected_title);
  EXPECT_EQ(expected_title, title_watcher.WaitAndGetTitle());
  EXPECT_EQ(1, observer.auth_needed_count());

  base::RunLoop run_loop;
  content::BrowserContext::GetDefaultStoragePartition(browser()->profile())
      ->GetNetworkContext()
      ->ClearHttpCache(base::Time(), base::Time(), nullptr,
                       run_loop.QuitClosure());
  run_loop.Run();

  // Navigate to a URL on a different origin that iframes the URL with the
  // challenge.
  GURL cross_origin_page = embedded_test_server()->GetURL(
      "localhost", "/iframe?" + test_page.spec());
  if (GetParam() == SplitAuthCacheByNetworkIsolationKey::kFalse) {
    // When allowing credentials to be used across NetworkIsolationKeys, the
    // auth credentials should be reused and there should be no new auth dialog.
    ui_test_utils::NavigateToURL(browser(), cross_origin_page);
    EXPECT_EQ(0u, observer.handlers().size());
    EXPECT_EQ(1, observer.auth_needed_count());
  } else {
    // When not allowing credentials to be used across NetworkIsolationKeys,
    // there should be another auth challenge.
    content::TestNavigationObserver navigation_observer(contents);
    WindowedAuthNeededObserver auth_needed_waiter2(controller);
    browser()->OpenURL(OpenURLParams(cross_origin_page, Referrer(),
                                     WindowOpenDisposition::CURRENT_TAB,
                                     ui::PAGE_TRANSITION_TYPED, false));
    auth_needed_waiter2.Wait();
    ASSERT_EQ(1u, observer.handlers().size());
    WindowedAuthSuppliedObserver auth_supplied_waiter(controller);
    LoginHandler* handler = *observer.handlers().begin();
    SetAuthFor(handler);
    auth_supplied_waiter.Wait();
    navigation_observer.Wait();
    EXPECT_EQ(2, observer.auth_needed_count());
  }

  std::vector<content::RenderFrameHost*> frames = contents->GetAllFrames();
  ASSERT_EQ(2u, frames.size());
  ASSERT_TRUE(frames[1]->IsDescendantOf(frames[0]));
  ASSERT_EQ(test_page, frames[1]->GetLastCommittedURL());

  // Make sure the iframe is displaying the base64-encoded credentials that
  // should have been set, which the EmbeddedTestServer echos back in response
  // bodies when /basic-auth is requested.
  EXPECT_EQ(true, content::EvalJs(frames[1],
                                  "document.documentElement.innerText.search("
                                  "'YmFzaWN1c2VyOnNlY3JldA==') >= 0"));
}

IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest,
                       GloballyScopeHTTPAuthCacheEnabled) {
  ASSERT_TRUE(embedded_test_server()->Start());

  browser()->profile()->GetPrefs()->SetBoolean(
      prefs::kGloballyScopeHTTPAuthCacheEnabled, true);
  // This is not technically necessary, since the SetAuthFor() call below uses
  // the same pipe that the pref change uses, making sure the change is applied
  // before the network process receives credentials, but seems safest to flush
  // the NetworkContext pipe explicitly.
  content::BrowserContext::GetDefaultStoragePartition(browser()->profile())
      ->FlushNetworkInterfaceForTesting();

  GURL test_page = embedded_test_server()->GetURL(kAuthBasicPage);

  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  NavigationController* controller = &contents->GetController();

  LoginPromptBrowserTestObserver observer;
  observer.Register(content::Source<NavigationController>(controller));
  WindowedAuthNeededObserver auth_needed_waiter(controller);
  browser()->OpenURL(OpenURLParams(test_page, Referrer(),
                                   WindowOpenDisposition::CURRENT_TAB,
                                   ui::PAGE_TRANSITION_TYPED, false));
  auth_needed_waiter.Wait();

  ASSERT_EQ(1u, observer.handlers().size());
  WindowedAuthSuppliedObserver auth_supplied_waiter(controller);
  LoginHandler* handler = *observer.handlers().begin();
  SetAuthFor(handler);
  auth_supplied_waiter.Wait();

  base::string16 expected_title = ExpectedTitleFromAuth(
      base::ASCIIToUTF16("basicuser"), base::ASCIIToUTF16("secret"));
  content::TitleWatcher title_watcher(contents, expected_title);
  EXPECT_EQ(expected_title, title_watcher.WaitAndGetTitle());
  EXPECT_EQ(1, observer.auth_needed_count());

  base::RunLoop run_loop;
  content::BrowserContext::GetDefaultStoragePartition(browser()->profile())
      ->GetNetworkContext()
      ->ClearHttpCache(base::Time(), base::Time(), nullptr,
                       run_loop.QuitClosure());
  run_loop.Run();

  // Navigate to a URL on a different origin that iframes the URL with the
  // challenge.
  GURL cross_origin_page = embedded_test_server()->GetURL(
      "localhost", "/iframe?" + test_page.spec());

  // When allowing credentials to be used across NetworkIsolationKeys, the
  // auth credentials should be reused and there should be no new auth dialog.
  ui_test_utils::NavigateToURL(browser(), cross_origin_page);
  EXPECT_EQ(0u, observer.handlers().size());
  EXPECT_EQ(1, observer.auth_needed_count());

  std::vector<content::RenderFrameHost*> frames = contents->GetAllFrames();
  ASSERT_EQ(2u, frames.size());
  ASSERT_TRUE(frames[1]->IsDescendantOf(frames[0]));
  ASSERT_EQ(test_page, frames[1]->GetLastCommittedURL());

  // Make sure the iframe is displaying the base64-encoded credentials that
  // should have been set, which the EmbeddedTestServer echos back in response
  // bodies when /basic-auth is requested.
  EXPECT_EQ(true, content::EvalJs(frames[1],
                                  "document.documentElement.innerText.search("
                                  "'YmFzaWN1c2VyOnNlY3JldA==') >= 0"));
}

// If a cross origin direct navigation triggers a login prompt, the login
// interstitial should be shown.
IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest,
                       ShowCorrectUrlForCrossOriginMainFrameRequests) {
  ASSERT_TRUE(embedded_test_server()->Start());

  GURL test_page = embedded_test_server()->GetURL(kAuthBasicPage);
  ASSERT_EQ("127.0.0.1", test_page.host());
  std::string auth_host("127.0.0.1");
  TestCrossOriginPrompt(browser(), test_page, auth_host, true);
}

// Same as ShowCorrectUrlForCrossOriginMainFrameRequests, but happening in a
// popup window.
IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest,
                       ShowCorrectUrlForCrossOriginMainFrameRequests_Popup) {
  ASSERT_TRUE(embedded_test_server()->Start());

  Browser* popup = CreateBrowserForPopup(browser()->profile());
  const GURL test_page = embedded_test_server()->GetURL(kAuthBasicPage);
  ASSERT_EQ("127.0.0.1", test_page.host());
  const std::string auth_host("127.0.0.1");
  TestCrossOriginPrompt(popup, test_page, auth_host, true);
}

// If a cross origin redirect triggers a login prompt, the destination URL
// should be shown in the omnibox when the auth dialog is displayed.
IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest,
                       ShowCorrectUrlForCrossOriginMainFrameRedirects) {
  ASSERT_TRUE(embedded_test_server()->Start());

  const char kTestPage[] = "/login/cross_origin.html";
  GURL test_page = embedded_test_server()->GetURL(kTestPage);
  ASSERT_EQ("127.0.0.1", test_page.host());
  std::string auth_host("www.a.com");
  TestCrossOriginPrompt(browser(), test_page, auth_host, true);
}

// Same as above, but instead of cancelling the prompt for www.a.com at the end,
// the page redirects to another page (www.b.com) that triggers an auth dialog.
// This should cancel the login interstitial for the first page (www.a.com),
// create a blank interstitial for second page (www.b.com) and show its URL in
// the omnibox.

// Fails occasionally on Mac. http://crbug.com/852703
#if defined(OS_MACOSX)
#define MAYBE_CancelLoginInterstitialOnRedirect \
  DISABLED_CancelLoginInterstitialOnRedirect
#else
#define MAYBE_CancelLoginInterstitialOnRedirect \
  CancelLoginInterstitialOnRedirect
#endif
IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest,
                       MAYBE_CancelLoginInterstitialOnRedirect) {
  ASSERT_TRUE(embedded_test_server()->Start());

  // The test page redirects to www.a.com which triggers an auth dialog.
  const char kTestPage[] = "/login/cross_origin.html";
  GURL test_page = embedded_test_server()->GetURL(kTestPage);
  ASSERT_EQ("127.0.0.1", test_page.host());

  // The page at b.com simply displays an auth dialog.
  GURL::Replacements replace_host2;
  replace_host2.SetHostStr("www.b.com");
  GURL page2 = embedded_test_server()
                   ->GetURL(kAuthBasicPage)
                   .ReplaceComponents(replace_host2);

  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  NavigationController* controller = &contents->GetController();
  LoginPromptBrowserTestObserver observer;
  observer.Register(content::Source<NavigationController>(controller));

  // Load the test page. It should end up on www.a.com with the auth dialog
  // open.
  TestCrossOriginPrompt(browser(), test_page, "www.a.com", false);
  ASSERT_EQ(1u, observer.handlers().size());

  // While the auth dialog is open for www.a.com, redirect to www.b.com which
  // also triggers an auth dialog. This should cancel the auth dialog for
  // www.a.com and end up displaying an auth interstitial and the URL for
  // www.b.com.
  WindowedAuthCancelledObserver auth_cancelled_waiter(controller);
  {
    WindowedLoadStopObserver load_stop_observer(controller, 1);
    EXPECT_TRUE(content::ExecuteScript(
        contents, std::string("document.location='") + page2.spec() + "';"));
    auth_cancelled_waiter.Wait();
    // Wait for the auth dialog and the interstitial for www.b.com.
    WindowedAuthNeededObserver auth_needed_waiter(controller);
    auth_needed_waiter.Wait();
    ASSERT_EQ(1u, observer.handlers().size());
    load_stop_observer.Wait();
  }

  EXPECT_EQ("www.b.com", contents->GetVisibleURL().host());

  // Cancel auth dialog for www.b.com.
  LoginHandler* handler = *observer.handlers().begin();
  handler->CancelAuth();
  EXPECT_EQ("www.b.com", contents->GetVisibleURL().host());
}

// Test the scenario where proceeding through a different type of interstitial
// that ends up with an auth URL works fine. This can happen if a URL that
// triggers the auth dialog can also trigger an SSL interstitial (or any other
// type of interstitial).
IN_PROC_BROWSER_TEST_P(
    LoginPromptBrowserTest,
    DISABLED_LoginInterstitialShouldReplaceExistingInterstitial) {
  net::EmbeddedTestServer https_server(net::EmbeddedTestServer::TYPE_HTTPS);
  https_server.SetSSLConfig(net::EmbeddedTestServer::CERT_EXPIRED);
  ASSERT_TRUE(https_server.Start());

  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  NavigationController* controller = &contents->GetController();
  LoginPromptBrowserTestObserver observer;

  observer.Register(content::Source<NavigationController>(controller));

  // Load a page which triggers an SSL interstitial. Proceeding through it
  // should show the login page with the blank interstitial.
  {
    GURL test_page = https_server.GetURL(kAuthBasicPage);
    ASSERT_EQ("127.0.0.1", test_page.host());

    WindowedAuthNeededObserver auth_needed_waiter(controller);
    browser()->OpenURL(OpenURLParams(test_page, Referrer(),
                                     WindowOpenDisposition::CURRENT_TAB,
                                     ui::PAGE_TRANSITION_TYPED, false));
    ASSERT_EQ("127.0.0.1", contents->GetURL().host());
    content::WaitForInterstitialAttach(contents);

    EXPECT_EQ(SSLBlockingPage::kTypeForTesting, contents->GetInterstitialPage()
                                                    ->GetDelegateForTesting()
                                                    ->GetTypeForTesting());
    // An overrideable SSL interstitial is now being displayed. Proceed through
    // the interstitial to see the login prompt.
    contents->GetInterstitialPage()->Proceed();
    auth_needed_waiter.Wait();
    ASSERT_EQ(1u, observer.handlers().size());
    content::WaitForInterstitialAttach(contents);

    // The omnibox should show the correct origin while the login prompt is
    // being displayed.
    EXPECT_EQ("127.0.0.1", contents->GetVisibleURL().host());

    // Cancelling the login prompt should detach the interstitial while keeping
    // the correct origin.
    LoginHandler* handler = *observer.handlers().begin();
    content::RunTaskAndWaitForInterstitialDetach(
        contents,
        base::BindOnce(&LoginHandler::CancelAuth, base::Unretained(handler)));

    EXPECT_EQ("127.0.0.1", contents->GetVisibleURL().host());
    EXPECT_FALSE(contents->ShowingInterstitialPage());
  }
}

// Test the scenario where an auth interstitial should replace a different type
// of interstitial (e.g. SSL) even though the navigation isn't cross origin.
// This is different than the above scenario in that the last
// committed url is the same as the auth url. This can happen when:
//
// 1. Tab is navigated to the auth URL and the auth prompt is cancelled.
// 2. Tab is then navigated to an SSL interstitial.
// 3. Tab is again navigated to the same auth URL in (1).
//
// In this case, the last committed url is the same as the auth URL since the
// navigation at (1) is committed (user clicked cancel and the page loaded), but
// the navigation at (2) isn't (navigations ending up in interstitials don't
// immediately commit). So just checking for cross origin navigation before
// prompting the auth interstitial is not sufficient, must also check if there
// is any other interstitial being displayed. With committed SSL interstitials,
// the navigation is actually cross domain since the interstitial is actually
// a committed navigation, but we still expect the same behavior.
IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest,
                       ShouldReplaceExistingInterstitialWhenNavigated) {
  ASSERT_TRUE(embedded_test_server()->Start());
  net::EmbeddedTestServer https_server(net::EmbeddedTestServer::TYPE_HTTPS);
  https_server.SetSSLConfig(net::EmbeddedTestServer::CERT_EXPIRED);
  ASSERT_TRUE(https_server.Start());

  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  NavigationController* controller = &contents->GetController();
  LoginPromptBrowserTestObserver observer;

  observer.Register(content::Source<NavigationController>(controller));

  GURL auth_url = embedded_test_server()->GetURL(kAuthBasicPage);
  GURL broken_ssl_page = https_server.GetURL("/");

  // Navigate to an auth url and wait for the login prompt.
  {
    WindowedAuthNeededObserver auth_needed_waiter(controller);
    ui_test_utils::NavigateToURL(browser(), auth_url);
    ASSERT_EQ("127.0.0.1", contents->GetURL().host());
    ASSERT_TRUE(contents->GetURL().SchemeIs("http"));
    auth_needed_waiter.Wait();
    ASSERT_EQ(1u, observer.handlers().size());
    // Cancel the auth prompt, which triggers a reload.
    LoginHandler* handler = *observer.handlers().begin();
    content::TestNavigationObserver reload_observer(contents);
    handler->CancelAuth();
    reload_observer.Wait();
    EXPECT_EQ("127.0.0.1", contents->GetVisibleURL().host());
    EXPECT_EQ(auth_url, contents->GetLastCommittedURL());
  }

  // Navigate to a broken SSL page. This is a cross origin navigation since
  // schemes don't match (http vs https).
  {
    ASSERT_EQ("127.0.0.1", broken_ssl_page.host());
    ui_test_utils::NavigateToURL(browser(), broken_ssl_page);
    ASSERT_EQ("127.0.0.1", contents->GetURL().host());
    ASSERT_TRUE(contents->GetURL().SchemeIs("https"));
    ASSERT_TRUE(WaitForRenderFrameReady(contents->GetMainFrame()));
  }

  // An overrideable SSL interstitial is now being displayed. Navigate to the
  // auth URL again. This is again a cross origin navigation, but last committed
  // URL is the same as the auth URL (since SSL navigation never committed).
  // Should still replace SSL interstitial with an auth interstitial even though
  // last committed URL and the new URL is the same. With committed SSL
  // interstitials enabled we still check for the behavior, but the
  // SSL interstitial will be a committed navigation so it will be handled as a
  // cross origin navigation.
  {
    WindowedAuthNeededObserver auth_needed_waiter(controller);
    ui_test_utils::NavigateToURL(browser(), auth_url);
    ASSERT_EQ("127.0.0.1", contents->GetURL().host());
    ASSERT_TRUE(contents->GetURL().SchemeIs("http"));

    auth_needed_waiter.Wait();
    ASSERT_EQ(1u, observer.handlers().size());
    // The login prompt is displayed above an empty page.
    EXPECT_EQ("<head></head><body></body>",
              content::EvalJs(contents, "document.documentElement.innerHTML"));
  }
}

// Test that the login interstitial isn't proceeding itself or any other
// interstitial. If this test becomes flaky, it's likely that the logic that
// prevents the tested scenario from happening got broken, rather than the test
// itself.
IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest,
                       ShouldNotProceedExistingInterstitial) {
  net::EmbeddedTestServer https_server(net::EmbeddedTestServer::TYPE_HTTPS);
  https_server.SetSSLConfig(net::EmbeddedTestServer::CERT_EXPIRED);
  ASSERT_TRUE(https_server.Start());

  const char* kTestPage = "/login/load_iframe_from_b.html";

  ASSERT_TRUE(embedded_test_server()->Start());

  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  NavigationController* controller = &contents->GetController();
  LoginPromptBrowserTestObserver observer;
  observer.Register(content::Source<NavigationController>(controller));

  // Load a page that has a cross-domain iframe authentication. This should
  // trigger a login prompt but no login interstitial.
  GURL test_page = embedded_test_server()->GetURL(kTestPage);
  GURL broken_ssl_page = https_server.GetURL("/");
  ASSERT_EQ("127.0.0.1", test_page.host());
  WindowedAuthNeededObserver auth_needed_waiter(controller);
  browser()->OpenURL(OpenURLParams(test_page, Referrer(),
                                   WindowOpenDisposition::CURRENT_TAB,
                                   ui::PAGE_TRANSITION_TYPED, false));
  auth_needed_waiter.Wait();
  ASSERT_EQ(1u, observer.handlers().size());
  EXPECT_FALSE(contents->ShowingInterstitialPage());

  // Redirect to a broken SSL page. This redirect should not accidentally
  // proceed through the SSL interstitial.
  content::TestNavigationObserver ssl_observer(contents);
  EXPECT_TRUE(content::ExecuteScript(
      browser()->tab_strip_model()->GetActiveWebContents(),
      std::string("window.location = '") + broken_ssl_page.spec() + "'"));
  ssl_observer.Wait();

  content::InterstitialPageDelegate* delegate =
      GetInterstitialDelegate(contents);

  EXPECT_TRUE(delegate);
  EXPECT_EQ(SSLBlockingPage::kTypeForTesting, delegate->GetTypeForTesting());
}

// Test where Basic HTTP authentication is disabled.
IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest, PRE_TestBasicAuthDisabled) {
  // Disable all auth schemes. The modified list isn't respected until the
  // browser is restarted, however.
  g_browser_process->local_state()->SetString(prefs::kAuthSchemes, "");
}

IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest, TestBasicAuthDisabled) {
  ASSERT_TRUE(embedded_test_server()->Start());
  GURL test_page = embedded_test_server()->GetURL(kAuthBasicPage);

  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  NavigationController* controller = &contents->GetController();

  // If the network service crashes, basic auth should still be disabled.
  for (bool crash_network_service : {false, true}) {
    // Crash the network service if it is enabled.
    if (crash_network_service && content::IsOutOfProcessNetworkService()) {
      SimulateNetworkServiceCrash();
      // Flush the network interface to make sure it notices the crash.
      content::BrowserContext::GetDefaultStoragePartition(browser()->profile())
          ->FlushNetworkInterfaceForTesting();
    }

    LoginPromptBrowserTestObserver observer;

    observer.Register(content::Source<NavigationController>(controller));
    browser()->OpenURL(OpenURLParams(test_page, Referrer(),
                                     WindowOpenDisposition::CURRENT_TAB,
                                     ui::PAGE_TRANSITION_TYPED, false));
    EXPECT_EQ(0, observer.auth_supplied_count());

    const base::string16 kExpectedTitle =
        base::ASCIIToUTF16("Denied: Missing Authorization Header");
    content::TitleWatcher title_watcher(contents, kExpectedTitle);
    EXPECT_EQ(kExpectedTitle, title_watcher.WaitAndGetTitle());
  }
}

// Tests that when HTTP Auth committed interstitials are enabled, a cross-origin
// main-frame auth challenge cancels the auth request.
IN_PROC_BROWSER_TEST_P(
    LoginPromptBrowserTest,
    TestAuthChallengeCancelsNavigationWithCommittedInterstitials) {
  ASSERT_TRUE(embedded_test_server()->Start());

  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  NavigationController* controller = &contents->GetController();
  LoginPromptBrowserTestObserver observer;
  observer.Register(content::Source<NavigationController>(controller));

  GURL test_page = embedded_test_server()->GetURL(kAuthBasicPage);
  ui_test_utils::NavigateToURL(browser(), test_page);

  // The login prompt should display above an empty page.
  EXPECT_EQ("<head></head><body></body>",
            content::EvalJs(contents, "document.documentElement.innerHTML"));
  EXPECT_EQ(0, observer.auth_cancelled_count());
}

// Tests that when HTTP Auth committed interstitials are enabled, the login
// prompt is shown on top of a committed error page when there is a cross-origin
// main-frame auth challenge.
IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest,
                       PromptWithCommittedInterstitials) {
  ASSERT_TRUE(embedded_test_server()->Start());

  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  NavigationController* controller = &contents->GetController();
  LoginPromptBrowserTestObserver observer;
  observer.Register(content::Source<NavigationController>(controller));
  WindowedAuthNeededObserver auth_needed_waiter(controller);

  GURL test_page = embedded_test_server()->GetURL(kAuthBasicPage);
  ui_test_utils::NavigateToURL(browser(), test_page);

  // Test that the login prompt displays above an empty page.
  EXPECT_EQ("<head></head><body></body>",
            content::EvalJs(contents, "document.documentElement.innerHTML"));

  auth_needed_waiter.Wait();
  ASSERT_EQ(1u, observer.handlers().size());

  // Test that credentials are handled correctly.
  WindowedAuthSuppliedObserver auth_supplied_waiter(controller);
  LoginHandler* handler = *observer.handlers().begin();
  SetAuthFor(handler);
  auth_supplied_waiter.Wait();

  base::string16 expected_title = ExpectedTitleFromAuth(
      base::ASCIIToUTF16("basicuser"), base::ASCIIToUTF16("secret"));
  content::TitleWatcher auth_supplied_title_watcher(contents, expected_title);
  EXPECT_EQ(expected_title, auth_supplied_title_watcher.WaitAndGetTitle());
}

// Tests that when HTTP Auth committed interstitials are enabled, showing a
// login prompt in a new window opened from window.open() does not
// crash. Regression test for https://crbug.com/1005096.
IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest, PromptWithNoVisibleEntry) {
  ASSERT_TRUE(embedded_test_server()->Start());

  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();

  ui_test_utils::NavigateToURL(browser(), GURL("about:blank"));

  // Open a new window via JavaScript and navigate it to a page that delivers an
  // auth prompt.
  GURL test_page = embedded_test_server()->GetURL(kAuthBasicPage);
  ASSERT_NE(false, content::EvalJs(contents, "w = window.open();"));
  content::WebContents* opened_contents =
      browser()->tab_strip_model()->GetWebContentsAt(1);
  NavigationController* opened_controller = &opened_contents->GetController();
  ASSERT_FALSE(opened_controller->GetVisibleEntry());
  LoginPromptBrowserTestObserver observer;
  observer.Register(content::Source<NavigationController>(opened_controller));
  WindowedAuthNeededObserver auth_needed_waiter(opened_controller);
  ASSERT_NE(false, content::EvalJs(contents, "w.location.href = '" +
                                                 test_page.spec() + "';"));

  // Test that the login prompt displays above an empty page.
  EXPECT_EQ(
      "<head></head><body></body>",
      content::EvalJs(opened_contents, "document.documentElement.innerHTML"));

  auth_needed_waiter.Wait();
  ASSERT_EQ(1u, observer.handlers().size());

  // Test that credentials are handled correctly.
  WindowedAuthSuppliedObserver auth_supplied_waiter(opened_controller);
  LoginHandler* handler = *observer.handlers().begin();
  SetAuthFor(handler);
  auth_supplied_waiter.Wait();

  base::string16 expected_title = ExpectedTitleFromAuth(
      base::ASCIIToUTF16("basicuser"), base::ASCIIToUTF16("secret"));
  content::TitleWatcher auth_supplied_title_watcher(opened_contents,
                                                    expected_title);
  EXPECT_EQ(expected_title, auth_supplied_title_watcher.WaitAndGetTitle());
}

// Tests that when HTTP Auth committed interstitials are enabled, a prompt
// triggered by a subframe can be cancelled.
IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest, PromptFromSubframe) {
  ASSERT_TRUE(embedded_test_server()->Start());

  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  content::NavigationController* controller = &contents->GetController();

  ui_test_utils::NavigateToURL(browser(), GURL("about:blank"));

  // Via JavaScript, create an iframe that delivers an auth prompt.
  GURL test_page = embedded_test_server()->GetURL(kAuthBasicPage);
  content::TestNavigationObserver subframe_observer(contents);
  LoginPromptBrowserTestObserver observer;
  observer.Register(content::Source<NavigationController>(controller));
  WindowedAuthNeededObserver auth_needed_waiter(controller);
  ASSERT_NE(
      false,
      content::EvalJs(
          contents, "var i = document.createElement('iframe'); i.src = '" +
                        test_page.spec() + "'; document.body.appendChild(i);"));
  auth_needed_waiter.Wait();
  ASSERT_EQ(1u, observer.handlers().size());

  // Cancel the prompt and check that another prompt is not shown.
  bool notification_fired = false;
  content::WindowedNotificationObserver no_auth_needed_observer(
      chrome::NOTIFICATION_AUTH_NEEDED,
      base::BindRepeating(&FailIfNotificationFires, &notification_fired));
  WindowedAuthCancelledObserver auth_cancelled_waiter(controller);
  LoginHandler* handler = observer.handlers().front();
  handler->CancelAuth();
  auth_cancelled_waiter.Wait();
  subframe_observer.Wait();
  EXPECT_FALSE(notification_fired);
}

// Tests that FTP auth challenges appear over a blank committed interstitial.
IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest, FtpAuth) {
  net::SpawnedTestServer ftp_server(
      net::SpawnedTestServer::TYPE_FTP,
      base::FilePath(FILE_PATH_LITERAL("chrome/test/data/ftp")));
  ftp_server.set_no_anonymous_ftp_user(true);
  ASSERT_TRUE(ftp_server.Start());

  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  NavigationController* controller = &contents->GetController();
  LoginPromptBrowserTestObserver observer;
  observer.Register(content::Source<NavigationController>(controller));

  // Navigate to an FTP server and wait for the auth prompt to appear.
  WindowedAuthNeededObserver auth_needed_waiter(controller);
  ui_test_utils::NavigateToURL(browser(), ftp_server.GetURL(""));
  auth_needed_waiter.Wait();
  ASSERT_EQ(1u, observer.handlers().size());
  EXPECT_EQ("<head></head><body></body>",
            content::EvalJs(contents, "document.documentElement.innerHTML"));

  // Supply credentials and wait for the page to successfully load.
  LoginHandler* handler = *observer.handlers().begin();
  WindowedAuthSuppliedObserver auth_supplied_waiter(controller);
  handler->SetAuth(base::ASCIIToUTF16("chrome"), base::ASCIIToUTF16("chrome"));
  auth_supplied_waiter.Wait();
  const base::string16 kExpectedTitle = base::ASCIIToUTF16("Index of /");
  content::TitleWatcher title_watcher(contents, kExpectedTitle);
  EXPECT_EQ(kExpectedTitle, title_watcher.WaitAndGetTitle());
}

// Tests that FTP auth prompts do not appear when credentials have been
// previously entered and cached.
IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest, FtpAuthWithCache) {
  net::SpawnedTestServer ftp_server(
      net::SpawnedTestServer::TYPE_FTP,
      base::FilePath(FILE_PATH_LITERAL("chrome/test/data/ftp")));
  ftp_server.set_no_anonymous_ftp_user(true);
  ASSERT_TRUE(ftp_server.Start());

  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  NavigationController* controller = &contents->GetController();
  LoginPromptBrowserTestObserver observer;
  observer.Register(content::Source<NavigationController>(controller));

  // Navigate to an FTP server and wait for the auth prompt to appear.
  WindowedAuthNeededObserver auth_needed_waiter(controller);
  ui_test_utils::NavigateToURL(browser(), ftp_server.GetURL(""));
  auth_needed_waiter.Wait();
  ASSERT_EQ(1u, observer.handlers().size());

  // Supply credentials and wait for the page to successfully load.
  LoginHandler* handler = *observer.handlers().begin();
  WindowedAuthSuppliedObserver auth_supplied_waiter(controller);
  handler->SetAuth(base::ASCIIToUTF16("chrome"), base::ASCIIToUTF16("chrome"));
  auth_supplied_waiter.Wait();
  const base::string16 kExpectedTitle = base::ASCIIToUTF16("Index of /");
  content::TitleWatcher title_watcher(contents, kExpectedTitle);
  EXPECT_EQ(kExpectedTitle, title_watcher.WaitAndGetTitle());

  // Navigate away and then back to the FTP server. There should be no auth
  // prompt because the credentials are cached.
  ui_test_utils::NavigateToURL(browser(), GURL("about:blank"));
  content::TitleWatcher revisit_title_watcher(contents, kExpectedTitle);
  ui_test_utils::NavigateToURL(browser(), ftp_server.GetURL(""));
  EXPECT_EQ(kExpectedTitle, revisit_title_watcher.WaitAndGetTitle());
  EXPECT_EQ(0u, observer.handlers().size());
}

namespace {

// A request handler that returns a 401 Unauthorized response on the
// /unauthorized path.
std::unique_ptr<net::test_server::HttpResponse> HandleUnauthorized(
    const net::test_server::HttpRequest& request) {
  if (request.relative_url != "/unauthorized") {
    return nullptr;
  }

  std::unique_ptr<net::test_server::BasicHttpResponse> response =
      std::make_unique<net::test_server::BasicHttpResponse>();
  response->set_code(net::HTTP_UNAUTHORIZED);
  response->set_content("<html><body>Unauthorized</body></html>");
  return response;
}

}  // namespace

// Tests that 401 responses are not cancelled and replaced with a blank page
// when incorrect credentials were supplied in the request. See
// https://crbug.com/1047742.
IN_PROC_BROWSER_TEST_P(LoginPromptBrowserTest,
                       ResponseNotCancelledWithIncorrectCredentials) {
  // Register a custom handler that returns a 401 Unauthorized response
  // regardless of what credentials were supplied in the request.
  embedded_test_server()->RegisterRequestHandler(
      base::BindRepeating(&HandleUnauthorized));
  ASSERT_TRUE(embedded_test_server()->Start());
  GURL test_page = embedded_test_server()->GetURL(kAuthBasicPage);
  content::WebContents* web_contents =
      browser()->tab_strip_model()->GetActiveWebContents();

  // Navigate to a page that prompts basic auth and fill in correct
  // credentials. A subsequent navigation handled by HandleUnauthorized() will
  // send the credentials cached from the navigation to |test_page|, but return
  // a 401 Unauthorized response.
  NavigationController* controller = &web_contents->GetController();
  LoginPromptBrowserTestObserver observer;
  observer.Register(content::Source<NavigationController>(controller));
  WindowedAuthNeededObserver auth_needed_waiter(controller);
  ui_test_utils::NavigateToURL(browser(), test_page);
  auth_needed_waiter.Wait();
  WindowedAuthSuppliedObserver auth_supplied_waiter(controller);
  LoginHandler* handler = *observer.handlers().begin();
  SetAuthFor(handler);
  auth_supplied_waiter.Wait();
  base::string16 expected_title = ExpectedTitleFromAuth(
      base::ASCIIToUTF16("basicuser"), base::ASCIIToUTF16("secret"));
  content::TitleWatcher title_watcher(web_contents, expected_title);
  EXPECT_EQ(expected_title, title_watcher.WaitAndGetTitle());

  // Now navigate to a page handled by HandleUnauthorized(), for which the
  // cached credentials are incorrect.
  ui_test_utils::NavigateToURL(browser(),
                               embedded_test_server()->GetURL("/unauthorized"));
  // Test that the 401 response body is rendered, instead of the navigation
  // being cancelled and a blank error page committing.
  EXPECT_EQ(false,
            content::EvalJs(
                web_contents,
                "document.body.innerHTML.indexOf('Unauthorized') === -1"));
}

class ProxyBrowserTestWithHttpAuthCommittedInterstitials
    : public ProxyBrowserTest {
 public:
  ProxyBrowserTestWithHttpAuthCommittedInterstitials() {
    feature_list_.InitAndEnableFeature(
        features::kHTTPAuthCommittedInterstitials);
  }

 private:
  base::test::ScopedFeatureList feature_list_;
};

// Tests that basic proxy auth works as expected, for HTTPS pages.
#if defined(OS_MACOSX)
// TODO(https://crbug.com/1000446): Re-enable this test.
#define MAYBE_ProxyAuthHTTPS DISABLED_ProxyAuthHTTPS
#else
#define MAYBE_ProxyAuthHTTPS ProxyAuthHTTPS
#endif
IN_PROC_BROWSER_TEST_F(ProxyBrowserTestWithHttpAuthCommittedInterstitials,
                       MAYBE_ProxyAuthHTTPS) {
  net::EmbeddedTestServer https_server(net::EmbeddedTestServer::TYPE_HTTPS);
  https_server.AddDefaultHandlers(GetChromeTestDataDir());
  ASSERT_TRUE(https_server.Start());
  ASSERT_NO_FATAL_FAILURE(
      TestProxyAuth(browser(), https_server.GetURL("/simple.html")));
}

// Tests that basic proxy auth works as expected, for HTTP pages.
IN_PROC_BROWSER_TEST_F(ProxyBrowserTestWithHttpAuthCommittedInterstitials,
                       ProxyAuthHTTP) {
  ASSERT_TRUE(embedded_test_server()->Start());
  ASSERT_NO_FATAL_FAILURE(
      TestProxyAuth(browser(), embedded_test_server()->GetURL("/simple.html")));
}

class LoginPromptExtensionBrowserTest
    : public extensions::ExtensionBrowserTest,
      public testing::WithParamInterface<SplitAuthCacheByNetworkIsolationKey> {
 public:
  LoginPromptExtensionBrowserTest() {
    if (GetParam() == SplitAuthCacheByNetworkIsolationKey::kFalse) {
      scoped_feature_list_.InitWithFeatures(
          // enabled_features
          {features::kHTTPAuthCommittedInterstitials},
          // disabled_features
          {network::features::kSplitAuthCacheByNetworkIsolationKey});
    } else {
      scoped_feature_list_.InitWithFeatures(
          // enabled_features
          {features::kHTTPAuthCommittedInterstitials,
           network::features::kSplitAuthCacheByNetworkIsolationKey},
          // disabled_features
          {});
    }
  }

  ~LoginPromptExtensionBrowserTest() override = default;

 private:
  base::test::ScopedFeatureList scoped_feature_list_;
};

INSTANTIATE_TEST_SUITE_P(
    All,
    LoginPromptExtensionBrowserTest,
    ::testing::Values(SplitAuthCacheByNetworkIsolationKey::kFalse,
                      SplitAuthCacheByNetworkIsolationKey::kTrue));

// Tests that with committed interstitials, extensions are notified once per
// request when auth is required. Regression test for https://crbug.com/1034468.
IN_PROC_BROWSER_TEST_P(LoginPromptExtensionBrowserTest,
                       OnAuthRequiredNotifiedOnce) {
  embedded_test_server()->RegisterRequestHandler(
      base::BindRepeating(&HandleBasicAuthSlowResponse));
  ASSERT_TRUE(embedded_test_server()->Start());

  // Load an extension that logs to the console each time onAuthRequired is
  // called. We attach a console observer so that we can verify that the
  // extension only logs once per request.
  const extensions::Extension* extension =
      LoadExtension(test_data_dir_.AppendASCII("log_auth_required"));
  ASSERT_TRUE(extension);
  content::WebContentsConsoleObserver console_observer(
      extensions::ProcessManager::Get(profile())
          ->GetBackgroundHostForExtension(extension->id())
          ->host_contents());

  // Navigate to a page that prompts for basic auth and then hangs.
  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  NavigationController* controller = &contents->GetController();
  WindowedAuthNeededObserver auth_needed_waiter(controller);
  GURL test_page =
      embedded_test_server()->GetURL(SlowAuthResponse::kSlowResponseUrl);
  ui_test_utils::NavigateToURL(browser(), test_page);

  console_observer.Wait();
  ASSERT_EQ(1u, console_observer.messages().size());
  EXPECT_EQ(base::ASCIIToUTF16("onAuthRequired " + test_page.spec()),
            console_observer.messages()[0].message);

  // Trigger a background request to end the response that prompted for basic
  // auth.
  ui_test_utils::NavigateToURLWithDisposition(
      browser(),
      embedded_test_server()->GetURL(SlowAuthResponse::kSlowResponseHostName,
                                     SlowAuthResponse::kFinishSlowResponseUrl),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP);

  // If https://crbug.com/1034468 regresses, the test may hang here. In that
  // bug, extensions were getting notified of each auth request twice, and the
  // extension must handle the auth request both times before LoginHandler
  // proceeds to show the login prompt. Usually, the request is fully destroyed
  // before the second extension dispatch, so the second extension dispatch is a
  // no-op. But when there is a delay between the OnAuthRequired notification
  // and the response body being read (as provided by SlowAuthResponse), the
  // WebRequestAPI is notified that the request is destroyed between the second
  // dispatch to an extension and when the extension replies. When this happens,
  // the LoginHandler is never notified that it can continue to show the login
  // prompt, so the auth needed notification that we are waiting for <will never
  // come. The fix to this bug is to ensure that extensions are notified of each
  // auth request only once; this test verifies that condition by checking that
  // the auth needed notification comes as expected and that the test extension
  // only logs once for onAuthRequired.
  auth_needed_waiter.Wait();
  // No second console message should have been logged, because extensions
  // should only be notified of the auth request once.
  EXPECT_EQ(1u, console_observer.messages().size());

  // It's possible that a second message was in fact logged, but the observer
  // hasn't heard about it yet. Navigate to a different URL and wait for the
  // corresponding console message, to "flush" any possible second message from
  // the current page load.
  WindowedAuthNeededObserver second_auth_needed_waiter(controller);
  GURL second_test_page = embedded_test_server()->GetURL("/auth-basic");
  ui_test_utils::NavigateToURL(browser(), second_test_page);
  second_auth_needed_waiter.Wait();
  ASSERT_EQ(2u, console_observer.messages().size());
  EXPECT_EQ(base::ASCIIToUTF16("onAuthRequired " + second_test_page.spec()),
            console_observer.messages()[1].message);
}

}  // namespace
