// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.share;

import android.content.Intent;
import android.net.Uri;
import android.support.test.filters.SmallTest;

import org.junit.Assert;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;

import org.chromium.base.test.util.CommandLineFlags;
import org.chromium.chrome.browser.ChromeSwitches;
import org.chromium.chrome.test.ChromeBrowserTestRule;
import org.chromium.chrome.test.ChromeJUnit4ClassRunner;
import org.chromium.chrome.test.util.browser.signin.SigninTestUtil;
import org.chromium.components.signin.ChromeSigninController;

/**
 * Tests of {@link LensUtils}.
 */
@RunWith(ChromeJUnit4ClassRunner.class)
@CommandLineFlags.Add(ChromeSwitches.DISABLE_FIRST_RUN_EXPERIENCE)
public class LensUtilsTest {
    @Rule
    public final ChromeBrowserTestRule mBrowserTestRule = new ChromeBrowserTestRule();

    /**
     * Test {@link LensUtils#getShareWithGoogleLensIntent()} method when user is signed in.
     */
    @Test
    @SmallTest
    public void getShareWithGoogleLensIntentSignedInTest() {
        SigninTestUtil.addAndSignInTestAccount();
        Assert.assertEquals("Chrome should be signed into the test account", "test@gmail.com",
                ChromeSigninController.get().getSignedInAccountName());

        Intent intentNoUri =
                LensUtils.getShareWithGoogleLensIntent(Uri.EMPTY, /* isIncognito= */ false);
        Assert.assertEquals("Intent without image has incorrect URI", "googleapp://lens",
                intentNoUri.getData().toString());
        Assert.assertEquals("Intent without image has incorrect action", Intent.ACTION_VIEW,
                intentNoUri.getAction());

        final String contentUrl = "content://image-url";
        Intent intentWithContentUri = LensUtils.getShareWithGoogleLensIntent(
                Uri.parse(contentUrl), /* isIncognito= */ false);
        Assert.assertEquals("Intent with image has incorrect URI",
                "googleapp://lens?LensBitmapUriKey=content%3A%2F%2Fimage-url&AccountNameUriKey="
                        + "test%40gmail.com&IncognitoUriKey=false",
                intentWithContentUri.getData().toString());
        Assert.assertEquals("Intent with image has incorrect action", Intent.ACTION_VIEW,
                intentWithContentUri.getAction());
    }

    /**
     * Test {@link LensUtils#getShareWithGoogleLensIntent()} method when user is incognito.
     */
    @Test
    @SmallTest
    public void getShareWithGoogleLensIntentIncognitoTest() {
        SigninTestUtil.addAndSignInTestAccount();
        Assert.assertEquals("Chrome should be signed into the test account", "test@gmail.com",
                ChromeSigninController.get().getSignedInAccountName());

        Intent intentNoUri =
                LensUtils.getShareWithGoogleLensIntent(Uri.EMPTY, /* isIncognito= */ true);
        Assert.assertEquals("Intent without image has incorrect URI", "googleapp://lens",
                intentNoUri.getData().toString());
        Assert.assertEquals("Intent without image has incorrect action", Intent.ACTION_VIEW,
                intentNoUri.getAction());

        final String contentUrl = "content://image-url";
        Intent intentWithContentUri = LensUtils.getShareWithGoogleLensIntent(
                Uri.parse(contentUrl), /* isIncognito= */ true);
        // The account name should not be included in the intent because the uesr is incognito.
        Assert.assertEquals("Intent with image has incorrect URI",
                "googleapp://lens?LensBitmapUriKey=content%3A%2F%2Fimage-url&AccountNameUriKey="
                        + "&IncognitoUriKey=true",
                intentWithContentUri.getData().toString());
        Assert.assertEquals("Intent with image has incorrect action", Intent.ACTION_VIEW,
                intentWithContentUri.getAction());
    }

    /**
     * Test {@link LensUtils#getShareWithGoogleLensIntent()} method when user is not signed in.
     */
    @Test
    @SmallTest
    public void getShareWithGoogleLensIntentNotSignedInTest() {
        Assert.assertNull("Chrome should not be signed in",
                ChromeSigninController.get().getSignedInAccountName());

        Intent intentNoUri =
                LensUtils.getShareWithGoogleLensIntent(Uri.EMPTY, /* isIncognito= */ false);
        Assert.assertEquals("Intent without image has incorrect URI", "googleapp://lens",
                intentNoUri.getData().toString());
        Assert.assertEquals("Intent without image has incorrect action", Intent.ACTION_VIEW,
                intentNoUri.getAction());

        final String contentUrl = "content://image-url";
        Intent intentWithContentUri = LensUtils.getShareWithGoogleLensIntent(
                Uri.parse(contentUrl), /* isIncognito= */ false);
        Assert.assertEquals("Intent with image has incorrect URI",
                "googleapp://lens?LensBitmapUriKey=content%3A%2F%2Fimage-url&AccountNameUriKey="
                        + "&IncognitoUriKey=false",
                intentWithContentUri.getData().toString());
        Assert.assertEquals("Intent with image has incorrect action", Intent.ACTION_VIEW,
                intentWithContentUri.getAction());
    }
}