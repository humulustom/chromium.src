// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.settings;

import android.Manifest;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.nfc.NfcAdapter;
import android.os.Process;
import android.provider.Settings;

import androidx.annotation.VisibleForTesting;

import org.chromium.base.ContextUtils;
import org.chromium.base.annotations.CalledByNative;
import org.chromium.base.annotations.NativeMethods;
import org.chromium.base.task.PostTask;
import org.chromium.chrome.browser.nfc.NfcSystemLevelPrompt;
import org.chromium.content_public.browser.UiThreadTaskTraits;
import org.chromium.content_public.browser.WebContents;
import org.chromium.ui.base.WindowAndroid;

/**
 * Provides methods for querying NFC sytem-level setting on Android.
 *
 * This class should be used only on the UI thread.
 */
public class NfcSystemLevelSetting {
    private static Boolean sSystemNfcSettingForTesting;

    @CalledByNative
    private static boolean isNfcAccessPossible() {
        Context context = ContextUtils.getApplicationContext();
        int permission =
                context.checkPermission(Manifest.permission.NFC, Process.myPid(), Process.myUid());
        if (permission != PackageManager.PERMISSION_GRANTED) {
            return false;
        }

        NfcAdapter nfcAdapter = NfcAdapter.getDefaultAdapter(context);
        return nfcAdapter != null;
    }

    @CalledByNative
    public static boolean isNfcSystemLevelSettingEnabled() {
        if (sSystemNfcSettingForTesting != null) {
            return sSystemNfcSettingForTesting;
        }

        if (!isNfcAccessPossible()) return false;

        NfcAdapter nfcAdapter = NfcAdapter.getDefaultAdapter(ContextUtils.getApplicationContext());
        return nfcAdapter.isEnabled();
    }

    @CalledByNative
    private static void promptToEnableNfcSystemLevelSetting(
            WebContents webContents, final long nativeCallback) {
        WindowAndroid window = webContents.getTopLevelNativeWindow();
        if (window == null) {
            // Consuming code may not expect a sync callback to happen.
            PostTask.postTask(UiThreadTaskTraits.DEFAULT,
                    ()
                            -> NfcSystemLevelSettingJni.get().onNfcSystemLevelPromptCompleted(
                                    nativeCallback));
            return;
        }

        NfcSystemLevelPrompt prompt = new NfcSystemLevelPrompt();
        prompt.show(window,
                ()
                        -> NfcSystemLevelSettingJni.get().onNfcSystemLevelPromptCompleted(
                                nativeCallback));
    }

    public static Intent getNfcSystemLevelSettingIntent() {
        Intent intent = new Intent(Settings.ACTION_NFC_SETTINGS);
        Context context = ContextUtils.getApplicationContext();
        if (intent.resolveActivity(context.getPackageManager()) == null) {
            return null;
        }
        return intent;
    }

    /** Disable/enable Android NFC setting for testing use only. */
    @VisibleForTesting
    public static void setNfcSettingForTesting(Boolean enabled) {
        sSystemNfcSettingForTesting = enabled;
    }

    @NativeMethods
    interface Natives {
        void onNfcSystemLevelPromptCompleted(long callback);
    }
}
