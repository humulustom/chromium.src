// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
'use strict';

(() => {
  /**
   * Waits for Quick View dialog to be closed.
   *
   * @param {string} appId Files app windowId.
   */
  async function waitQuickViewClose(appId) {
    const caller = getCaller();

    function checkQuickViewElementsDisplayNone(elements) {
      chrome.test.assertTrue(Array.isArray(elements));
      if (elements.length == 0 || elements[0].styles.display !== 'none') {
        return pending(caller, 'Waiting for Quick View to close.');
      }
    }

    // Check: the Quick View dialog should not be shown.
    await repeatUntil(async () => {
      const elements = ['#quick-view', '#dialog:not([open])'];
      return checkQuickViewElementsDisplayNone(
          await remoteCall.callRemoteTestUtil(
              'deepQueryAllElements', appId, [elements, ['display']]));
    });
  }

  /**
   * Opens the Quick View dialog on a given file |name|. The file must be
   * present in the Files app file list.
   *
   * @param {string} appId Files app windowId.
   * @param {string} name File name.
   */
  async function openQuickView(appId, name) {
    const caller = getCaller();

    function checkQuickViewElementsDisplayBlock(elements) {
      const haveElements = Array.isArray(elements) && elements.length !== 0;
      if (!haveElements || elements[0].styles.display !== 'block') {
        return pending(caller, 'Waiting for Quick View to open.');
      }
      return;
    }

    // Select file |name| in the file list.
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil('selectFile', appId, [name]),
        'selectFile failed');

    // Press the space key.
    const space = ['#file-list', ' ', false, false, false];
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, space),
        'fakeKeyDown failed');

    // Check: the Quick View dialog should be shown.
    await repeatUntil(async () => {
      const elements = ['#quick-view', '#dialog[open]'];
      return checkQuickViewElementsDisplayBlock(
          await remoteCall.callRemoteTestUtil(
              'deepQueryAllElements', appId, [elements, ['display']]));
    });
  }

  /**
   * Opens the Quick View dialog with given file |names|. The files must be
   * present and check-selected in the Files app file list.
   *
   * @param {string} appId Files app windowId.
   * @param {Array<string>} names File names.
   */
  async function openQuickViewMultipleSelection(appId, names) {
    const caller = getCaller();

    function checkQuickViewElementsDisplayBlock(elements) {
      const haveElements = Array.isArray(elements) && elements.length !== 0;
      if (!haveElements || elements[0].styles.display !== 'block') {
        return pending(caller, 'Waiting for Quick View to open.');
      }
      return;
    }

    // Get the file-list rows that are check-selected (multi-selected).
    const selectedRows = await remoteCall.callRemoteTestUtil(
        'deepQueryAllElements', appId, ['#file-list li[selected]']);

    // Check: the selection should contain the given file names.
    chrome.test.assertEq(names.length, selectedRows.length);
    for (let i = 0; i < names.length; i++) {
      chrome.test.assertTrue(
          selectedRows[i].attributes['file-name'].includes(names[i]));
    }

    // Open Quick View via its keyboard shortcut.
    const space = ['#file-list', ' ', false, false, false];
    await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, space);

    // Check: the Quick View dialog should be shown.
    await repeatUntil(async () => {
      const elements = ['#quick-view', '#dialog[open]'];
      return checkQuickViewElementsDisplayBlock(
          await remoteCall.callRemoteTestUtil(
              'deepQueryAllElements', appId, [elements, ['display']]));
    });
  }

  /**
   * Assuming that Quick View is currently open per openQuickView above, closes
   * the Quick View dialog.
   *
   * @param {string} appId Files app windowId.
   */
  async function closeQuickView(appId) {
    // Click on Quick View to close it.
    const panelElements = ['#quick-view', '#contentPanel'];
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil(
            'fakeMouseClick', appId, [panelElements]),
        'fakeMouseClick failed');

    // Check: the Quick View dialog should not be shown.
    await waitQuickViewClose(appId);
  }

  /**
   * Assuming that Quick View is currently open per openQuickView above, return
   * the text shown in the QuickView Metadata Box field |name|.
   *
   * @param {string} appId Files app windowId.
   * @param {string} name QuickView Metadata Box field name.
   *
   * @return {string} text Text value in the field name.
   */
  async function getQuickViewMetadataBoxField(appId, name) {
    let filesMetadataBox = 'files-metadata-box';

    /**
     * <files-metadata-box> field rendering is async. The field name has been
     * rendered when its 'metadata' attribute indicates that.
     */
    switch (name) {
      case 'Size':
        filesMetadataBox += '[metadata~="size"]';
        break;
      case 'Date modified':
      case 'Type':
        filesMetadataBox += '[metadata~="mime"]';
        break;
      default:
        filesMetadataBox += '[metadata~="meta"]';
        break;
    }

    /**
     * The <files-metadata-box> element resides in the #quick-view shadow DOM
     * as a child of the #dialog element.
     */
    const quickViewQuery = ['#quick-view', '#dialog[open] ' + filesMetadataBox];

    /**
     * The <files-metadata-entry key="name"> element resides in the shadow DOM
     * of the <files-metadata-box>.
     */
    quickViewQuery.push(`files-metadata-entry[key="${name}"]`);

    /**
     * It has a #value div child in its shadow DOM containing the field value.
     */
    quickViewQuery.push('#value > div:not([hidden])');

    const element = await remoteCall.waitForElement(appId, quickViewQuery);
    return element.text;
  }

  /**
   * Tests opening Quick View on a local downloads file.
   */
  testcase.openQuickView = async () => {
    // Open Files app on Downloads containing ENTRIES.hello.
    const appId =
        await setupAndWaitUntilReady(RootPath.DOWNLOADS, [ENTRIES.hello], []);

    // Open the file in Quick View.
    await openQuickView(appId, ENTRIES.hello.nameText);

    // Check the open button is shown.
    await remoteCall.waitForElement(
        appId, ['#quick-view', '#open-button:not([hidden])']);
  };

  /**
   * Tests opening Quick View on a local downloads file in an open file dialog.
   */
  testcase.openQuickViewDialog = async () => {
    // Open Files app on Downloads containing ENTRIES.hello.
    const appId = await setupAndWaitUntilReady(
        RootPath.DOWNLOADS, [ENTRIES.hello], [], {type: 'open-file'});

    // Open the file in Quick View.
    await openQuickView(appId, ENTRIES.hello.nameText);

    // Check the open button is not shown as we're in an open file dialog.
    await remoteCall.waitForElement(
        appId, ['#quick-view', '#open-button[hidden]']);
  };

  /**
   * Tests that Quick View opens via the context menu with a single selection.
   */
  testcase.openQuickViewViaContextMenuSingleSelection = async () => {
    // Open Files app on Downloads containing BASIC_LOCAL_ENTRY_SET.
    const appId = await setupAndWaitUntilReady(
        RootPath.DOWNLOADS, BASIC_LOCAL_ENTRY_SET, []);

    // Select hello.txt in the file list.
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil(
            'selectFile', appId, [ENTRIES.hello.nameText]),
        'selectFile failed');

    // Right-click the file in the file-list.
    const query = '#file-list [file-name="hello.txt"]';
    chrome.test.assertTrue(!!await remoteCall.callRemoteTestUtil(
        'fakeMouseRightClick', appId, [query]));

    // Wait because WebUI Menu ignores the following click if it happens in
    // <200ms from the previous click.
    await wait(300);

    // Click the file-list context menu "Get info" command.
    const getInfoMenuItem = '#file-context-menu:not([hidden]) ' +
        ' [command="#get-info"]:not([hidden])';
    chrome.test.assertTrue(!!await remoteCall.callRemoteTestUtil(
        'fakeMouseClick', appId, [getInfoMenuItem]));

    // Check: the Quick View dialog should be shown.
    const caller = getCaller();
    await repeatUntil(async () => {
      const query = ['#quick-view', '#dialog[open]'];
      const elements = await remoteCall.callRemoteTestUtil(
          'deepQueryAllElements', appId, [query, ['display']]);
      const haveElements = Array.isArray(elements) && elements.length !== 0;
      if (!haveElements || elements[0].styles.display !== 'block') {
        return pending(caller, 'Waiting for Quick View to open.');
      }
      return true;
    });
  };

  /**
   * Tests that Quick View opens via the context menu when multiple files
   * are selected (file-list check-select mode).
   */
  testcase.openQuickViewViaContextMenuCheckSelections = async () => {
    // Open Files app on Downloads containing BASIC_LOCAL_ENTRY_SET.
    const appId = await setupAndWaitUntilReady(
        RootPath.DOWNLOADS, BASIC_LOCAL_ENTRY_SET, []);

    // Ctrl+A to select all files in the file-list.
    const ctrlA = ['#file-list', 'a', true, false, false];
    await remoteCall.fakeKeyDown(appId, ...ctrlA);

    // Right-click the file in the file-list.
    const query = '#file-list [file-name="hello.txt"]';
    chrome.test.assertTrue(!!await remoteCall.callRemoteTestUtil(
        'fakeMouseRightClick', appId, [query]));

    // Wait because WebUI Menu ignores the following click if it happens in
    // <200ms from the previous click.
    await wait(300);

    // Click the file-list context menu "Get info" command.
    const getInfoMenuItem = '#file-context-menu:not([hidden]) ' +
        ' [command="#get-info"]:not([hidden])';
    chrome.test.assertTrue(!!await remoteCall.callRemoteTestUtil(
        'fakeMouseClick', appId, [getInfoMenuItem]));

    // Check: the Quick View dialog should be shown.
    const caller = getCaller();
    await repeatUntil(async () => {
      const query = ['#quick-view', '#dialog[open]'];
      const elements = await remoteCall.callRemoteTestUtil(
          'deepQueryAllElements', appId, [query, ['display']]);
      const haveElements = Array.isArray(elements) && elements.length !== 0;
      if (!haveElements || elements[0].styles.display !== 'block') {
        return pending(caller, 'Waiting for Quick View to open.');
      }
      return true;
    });
  };

  /**
   * Tests opening then closing Quick View on a local downloads file.
   */
  testcase.closeQuickView = async () => {
    // Open Files app on Downloads containing ENTRIES.hello.
    const appId =
        await setupAndWaitUntilReady(RootPath.DOWNLOADS, [ENTRIES.hello], []);

    // Open the file in Quick View.
    await openQuickView(appId, ENTRIES.hello.nameText);

    // Close Quick View.
    await closeQuickView(appId);
  };

  /**
   * Tests opening Quick View on a Drive file.
   */
  testcase.openQuickViewDrive = async () => {
    // Open Files app on Drive containing ENTRIES.hello.
    const appId =
        await setupAndWaitUntilReady(RootPath.DRIVE, [], [ENTRIES.hello]);

    // Open the file in Quick View.
    await openQuickView(appId, ENTRIES.hello.nameText);

    // Check: the correct mimeType should be displayed.
    const mimeType = await getQuickViewMetadataBoxField(appId, 'Type');
    chrome.test.assertEq('text/plain', mimeType);
  };

  /**
   * Tests opening Quick View on a USB file.
   */
  testcase.openQuickViewUsb = async () => {
    const USB_VOLUME_QUERY = '#directory-tree [volume-type-icon="removable"]';

    // Open Files app on Downloads containing ENTRIES.photos.
    const appId =
        await setupAndWaitUntilReady(RootPath.DOWNLOADS, [ENTRIES.photos], []);

    // Mount a USB volume.
    await sendTestMessage({name: 'mountFakeUsb'});

    // Wait for the USB volume to mount.
    await remoteCall.waitForElement(appId, USB_VOLUME_QUERY);

    // Click to open the USB volume.
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil(
            'fakeMouseClick', appId, [USB_VOLUME_QUERY]),
        'fakeMouseClick failed');

    // Check: the USB files should appear in the file list.
    const files = TestEntryInfo.getExpectedRows(BASIC_FAKE_ENTRY_SET);
    await remoteCall.waitForFiles(appId, files, {ignoreLastModifiedTime: true});

    // Open a USB file in Quick View.
    await openQuickView(appId, ENTRIES.hello.nameText);
  };

  /**
   * Tests opening Quick View on a removable partition.
   */
  testcase.openQuickViewRemovablePartitions = async () => {
    const PARTITION_QUERY =
        '#directory-tree .tree-children [volume-type-icon="removable"]';
    const caller = getCaller();

    // Open Files app on Downloads containing ENTRIES.photos.
    const appId =
        await setupAndWaitUntilReady(RootPath.DOWNLOADS, [ENTRIES.photos], []);

    // Mount USB device containing partitions.
    await sendTestMessage({name: 'mountUsbWithPartitions'});

    // Wait for 2 removable partitions to appear in the directory tree.
    await repeatUntil(async () => {
      const partitions = await remoteCall.callRemoteTestUtil(
          'queryAllElements', appId, [PARTITION_QUERY]);

      if (partitions.length == 2) {
        return true;
      }
      return pending(
          caller, 'Found %d partitions, waiting for 2.', partitions.length);
    });

    // Click to open the first partition.
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil(
            'fakeMouseClick', appId, [PARTITION_QUERY]),
        'fakeMouseClick failed');

    // Check: the USB files should appear in the file list.
    const files = TestEntryInfo.getExpectedRows(BASIC_FAKE_ENTRY_SET);
    await remoteCall.waitForFiles(appId, files, {ignoreLastModifiedTime: true});

    // Open the file in Quick View.
    await openQuickView(appId, ENTRIES.hello.nameText);
  };

  /**
   * Tests opening Quick View on an MTP file.
   */
  testcase.openQuickViewMtp = async () => {
    const MTP_VOLUME_QUERY = '#directory-tree [volume-type-icon="mtp"]';

    // Open Files app on Downloads containing ENTRIES.photos.
    const appId =
        await setupAndWaitUntilReady(RootPath.DOWNLOADS, [ENTRIES.photos], []);

    // Mount a non-empty MTP volume.
    await sendTestMessage({name: 'mountFakeMtp'});

    // Wait for the MTP volume to mount.
    await remoteCall.waitForElement(appId, MTP_VOLUME_QUERY);

    // Click to open the MTP volume.
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil(
            'fakeMouseClick', appId, [MTP_VOLUME_QUERY]),
        'fakeMouseClick failed');

    // Check: the MTP files should appear in the file list.
    const files = TestEntryInfo.getExpectedRows(BASIC_FAKE_ENTRY_SET);
    await remoteCall.waitForFiles(appId, files, {ignoreLastModifiedTime: true});

    // Open an MTP file in Quick View.
    await openQuickView(appId, ENTRIES.hello.nameText);
  };

  /**
   * Tests opening Quick View on a Crostini file.
   */
  testcase.openQuickViewCrostini = async () => {
    // Open Files app on Downloads containing ENTRIES.photos.
    const appId =
        await setupAndWaitUntilReady(RootPath.DOWNLOADS, [ENTRIES.photos], []);

    // Open a Crostini file in Quick View.
    await mountCrostini(appId);
    await openQuickView(appId, ENTRIES.hello.nameText);
  };

  /**
   * Tests opening Quick View on an Android file.
   */
  testcase.openQuickViewAndroid = async () => {
    // Open Files app on Android files.
    const appId = await openNewWindow(RootPath.ANDROID_FILES);

    // Add files to the Android files volume.
    const entrySet = BASIC_ANDROID_ENTRY_SET.concat([ENTRIES.documentsText]);
    await addEntries(['android_files'], entrySet);

    // Wait for the file list to appear.
    await remoteCall.waitForElement(appId, '#file-list');

    // Check: the basic Android file set should appear in the file list.
    let files = TestEntryInfo.getExpectedRows(BASIC_ANDROID_ENTRY_SET);
    await remoteCall.waitForFiles(appId, files, {ignoreLastModifiedTime: true});

    // Navigate to the Android files '/Documents' directory.
    await remoteCall.navigateWithDirectoryTree(
        appId, '/Documents', 'My files/Play files', 'android_files');

    // Check: the 'android.txt' file should appear in the file list.
    files = [ENTRIES.documentsText.getExpectedRow()];
    await remoteCall.waitForFiles(appId, files, {ignoreLastModifiedTime: true});

    // Open the Android file in Quick View.
    const documentsFileName = ENTRIES.documentsText.nameText;
    await openQuickView(appId, documentsFileName);
  };

  /**
   * Tests opening Quick View on a DocumentsProvider root.
   */
  testcase.openQuickViewDocumentsProvider = async () => {
    const DOCUMENTS_PROVIDER_VOLUME_QUERY =
        '[has-children="true"] [volume-type-icon="documents_provider"]';

    // Open Files app.
    const appId = await openNewWindow(RootPath.DOWNLOADS);

    // Add files to the DocumentsProvider volume.
    await addEntries(['documents_provider'], BASIC_LOCAL_ENTRY_SET);

    // Wait for the DocumentsProvider volume to mount.
    await remoteCall.waitForElement(appId, DOCUMENTS_PROVIDER_VOLUME_QUERY);

    // Click to open the DocumentsProvider volume.
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil(
            'fakeMouseClick', appId, [DOCUMENTS_PROVIDER_VOLUME_QUERY]),
        'fakeMouseClick failed');

    // Check: the DocumentsProvider files should appear in the file list.
    const files = TestEntryInfo.getExpectedRows(BASIC_LOCAL_ENTRY_SET);
    await remoteCall.waitForFiles(appId, files, {ignoreLastModifiedTime: true});

    // Open a DocumentsProvider file in Quick View.
    await openQuickView(appId, ENTRIES.hello.nameText);
  };

  /**
   * Tests opening Quick View with a document identified as text from file
   * sniffing because it has no filename extension.
   */
  testcase.openQuickViewSniffedText = async () => {
    const caller = getCaller();

    /**
     * The text <webview> resides in the #quick-view shadow DOM, as a child of
     * the #dialog element.
     */
    const webView = ['#quick-view', '#dialog[open] webview.text-content'];

    // Open Files app on Downloads containing ENTRIES.plainText.
    const appId = await setupAndWaitUntilReady(
        RootPath.DOWNLOADS, [ENTRIES.plainText], []);

    // Open the file in Quick View.
    await openQuickView(appId, ENTRIES.plainText.nameText);

    // Wait for the Quick View <webview> to load and display its content.
    function checkWebViewTextLoaded(elements) {
      let haveElements = Array.isArray(elements) && elements.length === 1;
      if (haveElements) {
        haveElements = elements[0].styles.display.includes('block');
      }
      if (!haveElements || !elements[0].attributes.src) {
        return pending(caller, 'Waiting for <webview> to load.');
      }
      return;
    }
    await repeatUntil(async () => {
      return checkWebViewTextLoaded(await remoteCall.callRemoteTestUtil(
          'deepQueryAllElements', appId, [webView, ['display']]));
    });

    // Check: the correct mimeType should be displayed.
    const mimeType = await getQuickViewMetadataBoxField(appId, 'Type');
    chrome.test.assertEq('text/plain', mimeType);
  };

  /**
   * Tests opening Quick View and scrolling its <webview> which contains a tall
   * text document.
   */
  testcase.openQuickViewScrollText = async () => {
    const caller = getCaller();

    /**
     * The text <webview> resides in the #quick-view shadow DOM, as a child of
     * the #dialog element.
     */
    const webView = ['#quick-view', '#dialog[open] webview.text-content'];

    function scrollQuickViewTextBy(y) {
      const doScrollBy = `window.scrollBy(0,${y})`;
      return remoteCall.callRemoteTestUtil(
          'deepExecuteScriptInWebView', appId, [webView, doScrollBy]);
    }

    async function checkQuickViewTextScrollY(scrollY) {
      if (!scrollY || Number(scrollY.toString()) <= 200) {
        console.log('checkQuickViewTextScrollY: scrollY '.concat(scrollY));
        await scrollQuickViewTextBy(100);
        return pending(caller, 'Waiting for Quick View to scroll.');
      }
    }

    // Open Files app on Downloads containing ENTRIES.tallText.
    const appId = await setupAndWaitUntilReady(
        RootPath.DOWNLOADS, [ENTRIES.tallText], []);

    // Open the file in Quick View.
    await openQuickView(appId, ENTRIES.tallText.nameText);

    // Wait for the Quick View <webview> to load and display its content.
    function checkWebViewTextLoaded(elements) {
      let haveElements = Array.isArray(elements) && elements.length === 1;
      if (haveElements) {
        haveElements = elements[0].styles.display.includes('block');
      }
      if (!haveElements || !elements[0].attributes.src) {
        return pending(caller, 'Waiting for <webview> to load.');
      }
      return;
    }
    await repeatUntil(async () => {
      return checkWebViewTextLoaded(await remoteCall.callRemoteTestUtil(
          'deepQueryAllElements', appId, [webView, ['display']]));
    });

    // Get the Quick View <webview> scrollY.
    const getScrollY = 'window.scrollY';
    await remoteCall.callRemoteTestUtil(
        'deepExecuteScriptInWebView', appId, [webView, getScrollY]);

    // Check: the initial <webview> scrollY should be 0.
    chrome.test.assertEq('0', scrollY.toString());

    // Scroll the <webview> and verify that it scrolled.
    await repeatUntil(async () => {
      const getScrollY = 'window.scrollY';
      return checkQuickViewTextScrollY(await remoteCall.callRemoteTestUtil(
          'deepExecuteScriptInWebView', appId, [webView, getScrollY]));
    });
  };

  /**
   * Tests opening Quick View containing a PDF document.
   */
  testcase.openQuickViewPdf = async () => {
    const caller = getCaller();

    /**
     * The PDF <webview> resides in the #quick-view shadow DOM, as a child of
     * the #dialog element.
     */
    const webView = ['#quick-view', '#dialog[open] webview.content'];

    // Open Files app on Downloads containing ENTRIES.tallPdf.
    const appId =
        await setupAndWaitUntilReady(RootPath.DOWNLOADS, [ENTRIES.tallPdf], []);

    // Open the file in Quick View.
    await openQuickView(appId, ENTRIES.tallPdf.nameText);

    // Wait for the Quick View <webview> to load and display its content.
    function checkWebViewPdfLoaded(elements) {
      let haveElements = Array.isArray(elements) && elements.length === 1;
      if (haveElements) {
        haveElements = elements[0].styles.display.includes('block');
      }
      if (!haveElements || !elements[0].attributes.src) {
        return pending(caller, 'Waiting for <webview> to load.');
      }
      return;
    }
    await repeatUntil(async () => {
      return checkWebViewPdfLoaded(await remoteCall.callRemoteTestUtil(
          'deepQueryAllElements', appId, [webView, ['display']]));
    });

    // Get the <webview> embed type attribute.
    function checkPdfEmbedType(type) {
      const haveElements = Array.isArray(type) && type.length === 1;
      if (!haveElements || !type[0].toString().includes('pdf')) {
        return pending(caller, 'Waiting for plugin <embed> type.');
      }
      return type[0];
    }
    const type = await repeatUntil(async () => {
      const getType = 'window.document.querySelector("embed").type';
      return checkPdfEmbedType(await remoteCall.callRemoteTestUtil(
          'deepExecuteScriptInWebView', appId, [webView, getType]));
    });

    // Check: the <webview> embed type should be PDF mime type.
    chrome.test.assertEq('application/pdf', type);
  };

  /**
   * Tests that Quick View does not display a PDF file preview when that is
   * disabled by system settings (preferences).
   */
  testcase.openQuickViewPdfPreviewsDisabled = async () => {
    const caller = getCaller();

    /**
     * The #innerContentPanel resides in the #quick-view shadow DOM as a child
     * of the #dialog element, and contains the file preview result.
     */
    const contentPanel = ['#quick-view', '#dialog[open] #innerContentPanel'];

    // Disable PDF previews.
    await sendTestMessage({name: 'setPdfPreviewEnabled', enabled: false});

    // Open Files app on Downloads containing ENTRIES.tallPdf.
    const appId =
        await setupAndWaitUntilReady(RootPath.DOWNLOADS, [ENTRIES.tallPdf], []);

    // Open the file in Quick View.
    await openQuickView(appId, ENTRIES.tallPdf.nameText);

    // Wait for the innerContentPanel to load and display its content.
    function checkInnerContentPanel(elements) {
      const haveElements = Array.isArray(elements) && elements.length === 1;
      if (!haveElements || elements[0].styles.display !== 'flex') {
        return pending(caller, 'Waiting for inner content panel to load.');
      }
      // Check: the PDF preview should not be shown.
      chrome.test.assertEq('No preview available', elements[0].text);
      return;
    }
    await repeatUntil(async () => {
      return checkInnerContentPanel(await remoteCall.callRemoteTestUtil(
          'deepQueryAllElements', appId, [contentPanel, ['display']]));
    });

    // Check: the correct file mimeType should be displayed.
    const mimeType = await getQuickViewMetadataBoxField(appId, 'Type');
    chrome.test.assertEq('application/pdf', mimeType);
  };

  /**
   * Tests opening Quick View and scrolling its <webview> which contains a tall
   * html document.
   */
  testcase.openQuickViewScrollHtml = async () => {
    const caller = getCaller();

    /**
     * The <webview> resides in the <files-safe-media type="html"> shadow DOM,
     * which is a child of the #quick-view shadow DOM.
     */
    const webView = ['#quick-view', 'files-safe-media[type="html"]', 'webview'];

    function scrollQuickViewHtmlBy(y) {
      const doScrollBy = `window.scrollBy(0,${y})`;
      return remoteCall.callRemoteTestUtil(
          'deepExecuteScriptInWebView', appId, [webView, doScrollBy]);
    }

    async function checkQuickViewHtmlScrollY(scrollY) {
      if (!scrollY || Number(scrollY.toString()) <= 200) {
        console.log('checkQuickViewHtmlScrollY: scrollY '.concat(scrollY));
        await scrollQuickViewHtmlBy(100);
        return pending(caller, 'Waiting for Quick View to scroll.');
      }
    }

    // Open Files app on Downloads containing ENTRIES.tallHtml.
    const appId = await setupAndWaitUntilReady(
        RootPath.DOWNLOADS, [ENTRIES.tallHtml], []);

    // Open the file in Quick View.
    await openQuickView(appId, ENTRIES.tallHtml.nameText);

    // Wait for the Quick View <webview> to load and display its content.
    function checkWebViewHtmlLoaded(elements) {
      let haveElements = Array.isArray(elements) && elements.length === 1;
      if (haveElements) {
        haveElements = elements[0].styles.display.includes('block');
      }
      if (!haveElements || elements[0].attributes.loaded !== '') {
        return pending(caller, 'Waiting for <webview> to load.');
      }
      return;
    }
    await repeatUntil(async () => {
      return checkWebViewHtmlLoaded(await remoteCall.callRemoteTestUtil(
          'deepQueryAllElements', appId, [webView, ['display']]));
    });

    // Get the Quick View <webview> scrollY.
    const getScrollY = 'window.scrollY';
    await remoteCall.callRemoteTestUtil(
        'deepExecuteScriptInWebView', appId, [webView, getScrollY]);

    // Check: the initial <webview> scrollY should be 0.
    chrome.test.assertEq('0', scrollY.toString());

    // Scroll the <webview> and verify that it scrolled.
    await repeatUntil(async () => {
      const getScrollY = 'window.scrollY';
      return checkQuickViewHtmlScrollY(await remoteCall.callRemoteTestUtil(
          'deepExecuteScriptInWebView', appId, [webView, getScrollY]));
    });
  };

  /**
   * Tests opening Quick View on an html document to verify that the background
   * color of the <files-safe-media type="html"> that contains the <webview> is
   * solid white.
   */
  testcase.openQuickViewBackgroundColorHtml = async () => {
    const caller = getCaller();

    /**
     * The <webview> resides in the <files-safe-media type="html"> shadow DOM,
     * which is a child of the #quick-view shadow DOM. This test only needs to
     * examine the <files-safe-media> element.
     */
    const fileSafeMedia = ['#quick-view', 'files-safe-media[type="html"]'];

    // Open Files app on Downloads containing ENTRIES.tallHtml.
    const appId = await setupAndWaitUntilReady(
        RootPath.DOWNLOADS, [ENTRIES.tallHtml], []);

    // Open the file in Quick View.
    await openQuickView(appId, ENTRIES.tallHtml.nameText);

    // Get the <files-safe-media type='html'> backgroundColor style.
    function getFileSafeMediaBackgroundColor(elements) {
      let haveElements = Array.isArray(elements) && elements.length === 1;
      if (haveElements) {
        haveElements = elements[0].styles.display.includes('block');
      }
      if (!haveElements || !elements[0].styles.backgroundColor) {
        return pending(caller, 'Waiting for <file-safe-media> element.');
      }
      return elements[0].styles.backgroundColor;
    }
    const backgroundColor = await repeatUntil(async () => {
      const styles = ['display', 'backgroundColor'];
      return getFileSafeMediaBackgroundColor(
          await remoteCall.callRemoteTestUtil(
              'deepQueryAllElements', appId, [fileSafeMedia, styles]));
    });

    // Check: the <files-safe-media> backgroundColor should be solid white.
    chrome.test.assertEq('rgb(255, 255, 255)', backgroundColor);
  };

  /**
   * Tests opening Quick View containing an audio file.
   */
  testcase.openQuickViewAudio = async () => {
    const caller = getCaller();

    /**
     * The <webview> resides in the <files-safe-media type="audio"> shadow DOM,
     * which is a child of the #quick-view shadow DOM.
     */
    const webView =
        ['#quick-view', 'files-safe-media[type="audio"]', 'webview'];

    // Open Files app on Downloads containing ENTRIES.beautiful song.
    const appId = await setupAndWaitUntilReady(
        RootPath.DOWNLOADS, [ENTRIES.beautiful], []);

    // Open the file in Quick View.
    await openQuickView(appId, ENTRIES.beautiful.nameText);

    // Wait for the Quick View <webview> to load and display its content.
    function checkWebViewAudioLoaded(elements) {
      let haveElements = Array.isArray(elements) && elements.length === 1;
      if (haveElements) {
        haveElements = elements[0].styles.display.includes('block');
      }
      if (!haveElements || elements[0].attributes.loaded !== '') {
        return pending(caller, 'Waiting for <webview> to load.');
      }
      return;
    }
    await repeatUntil(async () => {
      return checkWebViewAudioLoaded(await remoteCall.callRemoteTestUtil(
          'deepQueryAllElements', appId, [webView, ['display']]));
    });

    // Get the <webview> document.body backgroundColor style.
    const getBackgroundStyle =
        'window.getComputedStyle(document.body).backgroundColor';
    const backgroundColor = await remoteCall.callRemoteTestUtil(
        'deepExecuteScriptInWebView', appId, [webView, getBackgroundStyle]);

    // Check: the <webview> body backgroundColor should be transparent black.
    chrome.test.assertEq('rgba(0, 0, 0, 0)', backgroundColor[0]);
  };

  /**
   * Tests opening Quick View containing an audio file that has an album art
   * image in its metadata.
   */
  testcase.openQuickViewAudioWithImageMetadata = async () => {
    const caller = getCaller();

    // Define a test file containing audio file with metadata.
    const id3Audio = new TestEntryInfo({
      type: EntryType.FILE,
      sourceFileName: 'id3Audio.mp3',
      targetPath: 'id3Audio.mp3',
      mimeType: 'audio/mpeg',
      lastModifiedTime: 'December 25 2015, 11:16 PM',
      nameText: 'id3Audio.mp3',
      sizeText: '5KB',
      typeText: 'id3 encoded MP3 audio',
    });

    /**
     * The <webview> resides in the <files-safe-media> shadow DOM, which
     * is a child of the #quick-view shadow DOM.
     */
    const albumArtWebView = ['#quick-view', '#audio-artwork', 'webview'];

    // Open Files app on Downloads containing the audio test file.
    const appId =
        await setupAndWaitUntilReady(RootPath.DOWNLOADS, [id3Audio], []);

    // Open the file in Quick View.
    await openQuickView(appId, id3Audio.nameText);

    // Wait for the Quick View <webview> to load and display its content.
    function checkWebViewImageLoaded(elements) {
      let haveElements = Array.isArray(elements) && elements.length === 1;
      if (haveElements) {
        haveElements = elements[0].styles.display.includes('block');
      }
      if (!haveElements || elements[0].attributes.loaded !== '') {
        return pending(caller, 'Waiting for <webview> to load.');
      }
      return;
    }

    // Wait until the <webview> has loaded the album image of the audio file.
    await repeatUntil(async () => {
      return checkWebViewImageLoaded(await remoteCall.callRemoteTestUtil(
          'deepQueryAllElements', appId, [albumArtWebView, ['display']]));
    });

    // Check: the audio album metadata should also be displayed.
    const album = await getQuickViewMetadataBoxField(appId, 'Album');
    chrome.test.assertEq(album, 'OK Computer');
  };

  /**
   * Tests opening Quick View containing an image.
   */
  testcase.openQuickViewImage = async () => {
    const caller = getCaller();

    /**
     * The <webview> resides in the <files-safe-media type="image"> shadow DOM,
     * which is a child of the #quick-view shadow DOM.
     */
    const webView =
        ['#quick-view', 'files-safe-media[type="image"]', 'webview'];

    // Open Files app on Downloads containing ENTRIES.smallJpeg.
    const appId = await setupAndWaitUntilReady(
        RootPath.DOWNLOADS, [ENTRIES.smallJpeg], []);

    // Open the file in Quick View.
    await openQuickView(appId, ENTRIES.smallJpeg.nameText);

    // Wait for the Quick View <webview> to load and display its content.
    function checkWebViewImageLoaded(elements) {
      let haveElements = Array.isArray(elements) && elements.length === 1;
      if (haveElements) {
        haveElements = elements[0].styles.display.includes('block');
      }
      if (!haveElements || elements[0].attributes.loaded !== '') {
        return pending(caller, 'Waiting for <webview> to load.');
      }
      return;
    }
    await repeatUntil(async () => {
      return checkWebViewImageLoaded(await remoteCall.callRemoteTestUtil(
          'deepQueryAllElements', appId, [webView, ['display']]));
    });

    // Get the <webview> document.body backgroundColor style.
    const getBackgroundStyle =
        'window.getComputedStyle(document.body).backgroundColor';
    const backgroundColor = await remoteCall.callRemoteTestUtil(
        'deepExecuteScriptInWebView', appId, [webView, getBackgroundStyle]);

    // Check: the <webview> body backgroundColor should be transparent black.
    chrome.test.assertEq('rgba(0, 0, 0, 0)', backgroundColor[0]);
  };

  /**
   * Tests opening Quick View on an JPEG image that has EXIF displays the EXIF
   * information in the QuickView Metadata Box.
   */
  testcase.openQuickViewImageExif = async () => {
    const caller = getCaller();

    /**
     * The <webview> resides in the <files-safe-media type="image"> shadow DOM,
     * which is a child of the #quick-view shadow DOM.
     */
    const webView =
        ['#quick-view', 'files-safe-media[type="image"]', 'webview'];

    // Open Files app on Downloads containing ENTRIES.exifImage.
    const appId = await setupAndWaitUntilReady(
        RootPath.DOWNLOADS, [ENTRIES.exifImage], []);

    // Open the file in Quick View.
    await openQuickView(appId, ENTRIES.exifImage.nameText);

    // Wait for the Quick View <webview> to load and display its content.
    function checkWebViewImageLoaded(elements) {
      let haveElements = Array.isArray(elements) && elements.length === 1;
      if (haveElements) {
        haveElements = elements[0].styles.display.includes('block');
      }
      if (!haveElements || elements[0].attributes.loaded !== '') {
        return pending(caller, 'Waiting for <webview> to load.');
      }
      return;
    }
    await repeatUntil(async () => {
      return checkWebViewImageLoaded(await remoteCall.callRemoteTestUtil(
          'deepQueryAllElements', appId, [webView, ['display']]));
    });

    // Check: the correct mimeType should be displayed.
    const mimeType = await getQuickViewMetadataBoxField(appId, 'Type');
    chrome.test.assertEq('image/jpeg', mimeType);

    // Check: the correct file modified time should be displayed.
    const time = await getQuickViewMetadataBoxField(appId, 'Date modified');
    chrome.test.assertEq('Jan 18, 2038, 1:02 AM', time);

    // Check: the correct image EXIF metadata should be displayed.
    const size = await getQuickViewMetadataBoxField(appId, 'Dimensions');
    chrome.test.assertEq('378 x 272', size);
    const model = await getQuickViewMetadataBoxField(appId, 'Device model');
    chrome.test.assertEq(model, 'FinePix S5000');
    const film = await getQuickViewMetadataBoxField(appId, 'Device settings');
    chrome.test.assertEq('f/2.8 0.004 5.7mm ISO200', film);
  };

  /**
   * Tests opening Quick View on an RAW image. The RAW image has EXIF and that
   * information should be displayed in the QuickView metadata box.
   */
  testcase.openQuickViewImageRaw = async () => {
    const caller = getCaller();

    /**
     * The <webview> resides in the <files-safe-media type="image"> shadow DOM,
     * which is a child of the #quick-view shadow DOM.
     */
    const webView =
        ['#quick-view', 'files-safe-media[type="image"]', 'webview'];

    // Open Files app on Downloads containing ENTRIES.rawImage.
    const appId = await setupAndWaitUntilReady(
        RootPath.DOWNLOADS, [ENTRIES.rawImage], []);

    // Open the file in Quick View.
    await openQuickView(appId, ENTRIES.rawImage.nameText);

    // Wait for the Quick View <webview> to load and display its content.
    function checkWebViewImageLoaded(elements) {
      let haveElements = Array.isArray(elements) && elements.length === 1;
      if (haveElements) {
        haveElements = elements[0].styles.display.includes('block');
      }
      if (!haveElements || elements[0].attributes.loaded !== '') {
        return pending(caller, 'Waiting for <webview> to load.');
      }
      return;
    }
    await repeatUntil(async () => {
      return checkWebViewImageLoaded(await remoteCall.callRemoteTestUtil(
          'deepQueryAllElements', appId, [webView, ['display']]));
    });

    // Check: the correct mimeType should be displayed.
    const mimeType = await getQuickViewMetadataBoxField(appId, 'Type');
    chrome.test.assertEq('image/x-olympus-orf', mimeType);

    // Check: the RAW image EXIF metadata should be displayed.
    const size = await getQuickViewMetadataBoxField(appId, 'Dimensions');
    chrome.test.assertEq('4608 x 3456', size);
    const model = await getQuickViewMetadataBoxField(appId, 'Device model');
    chrome.test.assertEq(model, 'E-M1');
    const film = await getQuickViewMetadataBoxField(appId, 'Device settings');
    chrome.test.assertEq('f/8 0.002 12mm ISO200', film);
  };

  /**
   * Tests that opening a broken image in Quick View displays the "no-preview
   * available" generic icon and has a [load-error] attribute.
   */
  testcase.openQuickViewBrokenImage = async () => {
    const caller = getCaller();

    /**
     * The [generic-thumbnail] element resides in the #quick-view shadow DOM
     * as a sibling of the files-safe-media[type="image"] element.
     */
    const genericThumbnail = [
      '#quick-view',
      'files-safe-media[type="image"][hidden] + [generic-thumbnail="image"]',
    ];

    // Open Files app on Downloads containing ENTRIES.brokenJpeg.
    const appId = await setupAndWaitUntilReady(
        RootPath.DOWNLOADS, [ENTRIES.brokenJpeg], []);

    // Open the file in Quick View.
    await openQuickView(appId, ENTRIES.brokenJpeg.nameText);

    // Check: the quick view element should have a 'load-error' attribute.
    await remoteCall.waitForElement(appId, '#quick-view[load-error]');

    // Wait for the generic thumbnail to load and display its content.
    function checkForGenericThumbnail(elements) {
      const haveElements = Array.isArray(elements) && elements.length === 1;
      if (!haveElements || elements[0].styles.display !== 'block') {
        return pending(caller, 'Waiting for generic thumbnail to load.');
      }
      return;
    }

    // Check: the generic thumbnail icon should be displayed.
    await repeatUntil(async () => {
      return checkForGenericThumbnail(await remoteCall.callRemoteTestUtil(
          'deepQueryAllElements', appId, [genericThumbnail, ['display']]));
    });
  };

  /**
   * Tests opening Quick View containing a video.
   */
  testcase.openQuickViewVideo = async () => {
    const caller = getCaller();

    /**
     * The <webview> resides in the <files-safe-media type="video"> shadow DOM,
     * which is a child of the #quick-view shadow DOM.
     */
    const webView =
        ['#quick-view', 'files-safe-media[type="video"]', 'webview'];

    // Open Files app on Downloads containing ENTRIES.webm video.
    const appId =
        await setupAndWaitUntilReady(RootPath.DOWNLOADS, [ENTRIES.webm], []);

    // Open the file in Quick View.
    await openQuickView(appId, ENTRIES.webm.nameText);

    // Wait for the Quick View <webview> to load and display its content.
    function checkWebViewVideoLoaded(elements) {
      let haveElements = Array.isArray(elements) && elements.length === 1;
      if (haveElements) {
        haveElements = elements[0].styles.display.includes('block');
      }
      if (!haveElements || elements[0].attributes.loaded !== '') {
        return pending(caller, 'Waiting for <webview> to load.');
      }
      return;
    }
    await repeatUntil(async () => {
      return checkWebViewVideoLoaded(await remoteCall.callRemoteTestUtil(
          'deepQueryAllElements', appId, [webView, ['display']]));
    });

    // Get the <webview> document.body backgroundColor style.
    const getBackgroundStyle =
        'window.getComputedStyle(document.body).backgroundColor';
    const backgroundColor = await remoteCall.callRemoteTestUtil(
        'deepExecuteScriptInWebView', appId, [webView, getBackgroundStyle]);

    // Check: the <webview> body backgroundColor should be transparent black.
    chrome.test.assertEq('rgba(0, 0, 0, 0)', backgroundColor[0]);

    // Close Quick View.
    await closeQuickView(appId);

    // Check quickview video <files-safe-media> has no "src", so it stops
    // playing the video. crbug.com/970192
    const noSrcFilesSafeMedia = ['#quick-view', '#videoSafeMedia[src=""]'];
    await remoteCall.waitForElement(appId, noSrcFilesSafeMedia);
  };

  /**
   * Tests opening Quick View with multiple files and using the up/down arrow
   * keys to select and view their content.
   */
  testcase.openQuickViewKeyboardUpDownChangesView = async () => {
    const caller = getCaller();

    /**
     * The text <webview> resides in the #quick-view shadow DOM, as a child of
     * the #dialog element.
     */
    const webView = ['#quick-view', '#dialog[open] webview.text-content'];

    // Open Files app on Downloads containing two text files.
    const files = [ENTRIES.hello, ENTRIES.tallText];
    const appId = await setupAndWaitUntilReady(RootPath.DOWNLOADS, files, []);

    // Open the last file in Quick View.
    await openQuickView(appId, ENTRIES.tallText.nameText);

    // Wait for the Quick View <webview> to load and display its content.
    function checkWebViewTextLoaded(elements) {
      let haveElements = Array.isArray(elements) && elements.length === 1;
      if (haveElements) {
        haveElements = elements[0].styles.display.includes('block');
      }
      if (!haveElements || !elements[0].attributes.src) {
        return pending(caller, 'Waiting for <webview> to load.');
      }
      return;
    }
    await repeatUntil(async () => {
      return checkWebViewTextLoaded(await remoteCall.callRemoteTestUtil(
          'deepQueryAllElements', appId, [webView, ['display']]));
    });

    // Press the down arrow key to select the next file.
    const downArrow = ['#quick-view', 'ArrowDown', false, false, false];
    chrome.test.assertTrue(
        await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, downArrow));

    // Wait until the <webview> displays that file's content.
    await repeatUntil(async () => {
      const getTextContent = 'window.document.body.textContent';
      const text = await remoteCall.callRemoteTestUtil(
          'deepExecuteScriptInWebView', appId, [webView, getTextContent]);
      if (!text || !text[0].includes('This is a sample file')) {
        return pending(caller, 'Waiting for <webview> content.');
      }
    });

    // Press the up arrow key to select the previous file.
    const upArrow = ['#quick-view', 'ArrowUp', false, false, false];
    chrome.test.assertTrue(
        await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, upArrow));

    // Wait until the <webview> displays that file's content.
    await repeatUntil(async () => {
      const getTextContent = 'window.document.body.textContent';
      const text = await remoteCall.callRemoteTestUtil(
          'deepExecuteScriptInWebView', appId, [webView, getTextContent]);
      if (!text || !text[0].includes('42 tall text')) {
        return pending(caller, 'Waiting for <webview> content.');
      }
    });
  };

  /**
   * Tests opening Quick View with multiple files and using the left/right arrow
   * keys to select and view their content.
   */
  testcase.openQuickViewKeyboardLeftRightChangesView = async () => {
    const caller = getCaller();

    /**
     * The text <webview> resides in the #quick-view shadow DOM, as a child of
     * the #dialog element.
     */
    const webView = ['#quick-view', '#dialog[open] webview.text-content'];

    // Open Files app on Downloads containing two text files.
    const files = [ENTRIES.hello, ENTRIES.tallText];
    const appId = await setupAndWaitUntilReady(RootPath.DOWNLOADS, files, []);

    // Open the last file in Quick View.
    await openQuickView(appId, ENTRIES.tallText.nameText);

    // Wait for the Quick View <webview> to load and display its content.
    function checkWebViewTextLoaded(elements) {
      let haveElements = Array.isArray(elements) && elements.length === 1;
      if (haveElements) {
        haveElements = elements[0].styles.display.includes('block');
      }
      if (!haveElements || !elements[0].attributes.src) {
        return pending(caller, 'Waiting for <webview> to load.');
      }
      return;
    }
    await repeatUntil(async () => {
      return checkWebViewTextLoaded(await remoteCall.callRemoteTestUtil(
          'deepQueryAllElements', appId, [webView, ['display']]));
    });

    // Press the right arrow key to select the next file item.
    const rightArrow = ['#quick-view', 'ArrowRight', false, false, false];
    chrome.test.assertTrue(
        await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, rightArrow));

    // Wait until the <webview> displays that file's content.
    await repeatUntil(async () => {
      const getTextContent = 'window.document.body.textContent';
      const text = await remoteCall.callRemoteTestUtil(
          'deepExecuteScriptInWebView', appId, [webView, getTextContent]);
      if (!text || !text[0].includes('This is a sample file')) {
        return pending(caller, 'Waiting for <webview> content.');
      }
    });

    // Press the left arrow key to select the previous file item.
    const leftArrow = ['#quick-view', 'ArrowLeft', false, false, false];
    chrome.test.assertTrue(
        await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, leftArrow));

    // Wait until the <webview> displays that file's content.
    await repeatUntil(async () => {
      const getTextContent = 'window.document.body.textContent';
      const text = await remoteCall.callRemoteTestUtil(
          'deepExecuteScriptInWebView', appId, [webView, getTextContent]);
      if (!text || !text[0].includes('42 tall text')) {
        return pending(caller, 'Waiting for <webview> content.');
      }
    });
  };

  /**
   * Tests close/open metadata info via Enter key.
   */
  testcase.pressEnterOnInfoBoxToOpenClose = async () => {
    const infoButton = ['#quick-view', '#metadata-button'];
    const key = [infoButton, 'Enter', false, false, false];
    const infoShown = ['#quick-view', '#contentPanel[metadata-box-active]'];
    const infoHidden =
        ['#quick-view', '#contentPanel:not([metadata-box-active])'];

    // Open Files app on Downloads containing ENTRIES.hello.
    const appId =
        await setupAndWaitUntilReady(RootPath.DOWNLOADS, [ENTRIES.hello], []);

    // Open the file in Quick View.
    await openQuickView(appId, ENTRIES.hello.nameText);

    // Press Enter on info button to close metadata box.
    await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, key);

    // Info should be hidden.
    await remoteCall.waitForElement(appId, infoHidden);

    // Press Enter on info button to open metadata box.
    await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, key);

    // Info should be shown.
    await remoteCall.waitForElement(appId, infoShown);

    // Close Quick View.
    await closeQuickView(appId);
  };

  /**
   * Tests that Quick View opens with multiple files selected.
   */
  testcase.openQuickViewWithMultipleFiles = async () => {
    const caller = getCaller();

    /**
     * The <webview> resides in the <files-safe-media type="image"> shadow DOM,
     * which is a child of the #quick-view shadow DOM.
     */
    const webView =
        ['#quick-view', 'files-safe-media[type="image"]', 'webview'];

    // Open Files app on Downloads containing BASIC_LOCAL_ENTRY_SET.
    const appId = await setupAndWaitUntilReady(
        RootPath.DOWNLOADS, BASIC_LOCAL_ENTRY_SET, []);

    // Add item 3 to the check-selection, ENTRIES.desktop.
    const downKey = ['#file-list', 'ArrowDown', false, false, false];
    for (let i = 0; i < 3; i++) {
      chrome.test.assertTrue(
          !!await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, downKey),
          'ArrowDown failed');
    }
    const ctrlSpace = ['#file-list', ' ', true, false, false];
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, ctrlSpace),
        'Ctrl+Space failed');

    // Add item 5 to the check-selection, ENTRIES.hello.
    const ctrlDown = ['#file-list', 'ArrowDown', true, false, false];
    for (let i = 0; i < 2; i++) {
      chrome.test.assertTrue(
          !!await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, ctrlDown),
          'Ctrl+ArrowDown failed');
    }
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, ctrlSpace),
        'Ctrl+Space failed');

    // Open Quick View with the check-selected files.
    await openQuickViewMultipleSelection(appId, ['Desktop', 'hello']);

    // Wait for the Quick View <webview> to load and display its content.
    function checkWebViewImageLoaded(elements) {
      let haveElements = Array.isArray(elements) && elements.length === 1;
      if (haveElements) {
        haveElements = elements[0].styles.display.includes('block');
      }
      if (!haveElements || elements[0].attributes.loaded !== '') {
        return pending(caller, 'Waiting for <webview> to load.');
      }
      return;
    }

    // Check: ENTRIES.desktop should be displayed in the webview.
    await repeatUntil(async () => {
      return checkWebViewImageLoaded(await remoteCall.callRemoteTestUtil(
          'deepQueryAllElements', appId, [webView, ['display']]));
    });

    // Check: the correct file mimeType should be displayed.
    const mimeType = await getQuickViewMetadataBoxField(appId, 'Type');
    chrome.test.assertEq('image/png', mimeType);
  };

  /**
   * Tests that Quick View displays text files when multiple files are
   * selected.
   */
  testcase.openQuickViewWithMultipleFilesText = async () => {
    const caller = getCaller();

    /**
     * The <webview> resides in the <files-safe-media type="image"> shadow DOM,
     * which is a child of the #quick-view shadow DOM.
     */
    const webView =
        ['#quick-view', 'files-safe-media[type="image"]', 'webview'];

    const files = [ENTRIES.tallText, ENTRIES.hello, ENTRIES.smallJpeg];
    const appId = await setupAndWaitUntilReady(RootPath.DOWNLOADS, files, []);

    // Add item 1 to the check-selection, ENTRIES.smallJpeg.
    const downKey = ['#file-list', 'ArrowDown', false, false, false];
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, downKey),
        'ArrowDown failed');
    const ctrlSpace = ['#file-list', ' ', true, false, false];
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, ctrlSpace),
        'Ctrl+Space failed');

    // Add item 3 to the check-selection, ENTRIES.hello.
    const ctrlDown = ['#file-list', 'ArrowDown', true, false, false];
    for (let i = 0; i < 2; i++) {
      chrome.test.assertTrue(
          !!await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, ctrlDown),
          'Ctrl+ArrowDown failed');
    }
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, ctrlSpace),
        'Ctrl+Space failed');

    // Open Quick View with the check-selected files.
    await openQuickViewMultipleSelection(appId, ['small', 'hello']);

    // Wait for the Quick View <webview> to load and display its content.
    function checkWebViewImageLoaded(elements) {
      let haveElements = Array.isArray(elements) && elements.length === 1;
      if (haveElements) {
        haveElements = elements[0].styles.display.includes('block');
      }
      if (!haveElements || elements[0].attributes.loaded !== '') {
        return pending(caller, 'Waiting for <webview> to load.');
      }
      return;
    }

    // Check: the image file should be displayed in the content panel.
    await repeatUntil(async () => {
      return checkWebViewImageLoaded(await remoteCall.callRemoteTestUtil(
          'deepQueryAllElements', appId, [webView, ['display']]));
    });

    /**
     * The text <webview> resides in the #quick-view shadow DOM, as a child of
     * the #dialog element.
     */
    const textView = ['#quick-view', '#dialog[open] webview.text-content'];

    // Press the down arrow key to select the next file.
    const downArrow = ['#quick-view', 'ArrowDown', false, false, false];
    chrome.test.assertTrue(
        await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, downArrow));

    // Wait for the Quick View <webview> to load and display its content.
    function checkWebViewTextLoaded(elements) {
      let haveElements = Array.isArray(elements) && elements.length === 1;
      if (haveElements) {
        haveElements = elements[0].styles.display.includes('block');
      }
      if (!haveElements || !elements[0].attributes.src) {
        return pending(caller, 'Waiting for <webview> to load.');
      }
      return;
    }

    // Check: the text file should be displayed in the content panel.
    await repeatUntil(async () => {
      return checkWebViewTextLoaded(await remoteCall.callRemoteTestUtil(
          'deepQueryAllElements', appId, [textView, ['display']]));
    });

    // Check: the open button should be displayed.
    await remoteCall.waitForElement(
        appId, ['#quick-view', '#open-button:not([hidden])']);
  };

  /**
   * Tests that Quick View displays pdf files when multiple files are
   * selected.
   */
  testcase.openQuickViewWithMultipleFilesPdf = async () => {
    const caller = getCaller();

    /**
     * The <webview> resides in the <files-safe-media type="image"> shadow DOM,
     * which is a child of the #quick-view shadow DOM.
     */
    const webView =
        ['#quick-view', 'files-safe-media[type="image"]', 'webview'];

    const files = [ENTRIES.tallPdf, ENTRIES.desktop, ENTRIES.smallJpeg];
    const appId = await setupAndWaitUntilReady(RootPath.DOWNLOADS, files, []);

    // Add item 1 to the check-selection, ENTRIES.smallJpeg.
    const downKey = ['#file-list', 'ArrowDown', false, false, false];
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, downKey),
        'ArrowDown failed');
    const ctrlSpace = ['#file-list', ' ', true, false, false];
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, ctrlSpace),
        'Ctrl+Space failed');

    // Add item 3 to the check-selection, ENTRIES.tallPdf.
    const ctrlDown = ['#file-list', 'ArrowDown', true, false, false];
    for (let i = 0; i < 2; i++) {
      chrome.test.assertTrue(
          !!await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, ctrlDown),
          'Ctrl+ArrowDown failed');
    }
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, ctrlSpace),
        'Ctrl+Space failed');

    // Open Quick View with the check-selected files.
    await openQuickViewMultipleSelection(appId, ['small', 'tall']);

    // Wait for the Quick View <webview> to load and display its content.
    function checkWebViewImageLoaded(elements) {
      let haveElements = Array.isArray(elements) && elements.length === 1;
      if (haveElements) {
        haveElements = elements[0].styles.display.includes('block');
      }
      if (!haveElements || elements[0].attributes.loaded !== '') {
        return pending(caller, 'Waiting for <webview> to load.');
      }
      return;
    }

    // Check: the image file should be displayed in the content panel.
    await repeatUntil(async () => {
      return checkWebViewImageLoaded(await remoteCall.callRemoteTestUtil(
          'deepQueryAllElements', appId, [webView, ['display']]));
    });

    /**
     * The PDF <webview> resides in the #quick-view shadow DOM, as a child of
     * the #dialog element.
     */
    const pdfView = ['#quick-view', '#dialog[open] webview.content'];

    // Press the down arrow key to select the next file.
    const downArrow = ['#quick-view', 'ArrowDown', false, false, false];
    chrome.test.assertTrue(
        await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, downArrow));

    // Wait for the Quick View <webview> to load and display its content.
    function checkWebViewPdfLoaded(elements) {
      let haveElements = Array.isArray(elements) && elements.length === 1;
      if (haveElements) {
        haveElements = elements[0].styles.display.includes('block');
      }
      if (!haveElements || !elements[0].attributes.src) {
        return pending(caller, 'Waiting for <webview> to load.');
      }
      return;
    }

    // Check: the pdf file should be displayed in the content panel.
    await repeatUntil(async () => {
      return checkWebViewPdfLoaded(await remoteCall.callRemoteTestUtil(
          'deepQueryAllElements', appId, [pdfView, ['display']]));
    });

    // Check: the open button should be displayed.
    await remoteCall.waitForElement(
        appId, ['#quick-view', '#open-button:not([hidden])']);
  };

  /**
   * Tests that the content panel changes when using the up/down arrow keys
   * when multiple files are selected.
   */
  testcase.openQuickViewWithMultipleFilesKeyboardUpDown = async () => {
    const caller = getCaller();

    /**
     * The text <webview> resides in the #quick-view shadow DOM, as a child of
     * the #dialog element.
     */
    const webView = ['#quick-view', '#dialog[open] webview.text-content'];

    // Open Files app on Downloads containing three text files.
    const files = [ENTRIES.hello, ENTRIES.tallText, ENTRIES.plainText];
    const appId = await setupAndWaitUntilReady(RootPath.DOWNLOADS, files, []);

    // Add item 1 to the check-selection, ENTRIES.tallText.
    const downKey = ['#file-list', 'ArrowDown', false, false, false];
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, downKey),
        'ArrowDown failed');
    const ctrlSpace = ['#file-list', ' ', true, false, false];
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, ctrlSpace),
        'Ctrl+Space failed');

    // Add item 3 to the check-selection, ENTRIES.hello.
    const ctrlDown = ['#file-list', 'ArrowDown', true, false, false];
    for (let i = 0; i < 2; i++) {
      chrome.test.assertTrue(
          !!await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, ctrlDown),
          'Ctrl+ArrowDown failed');
    }
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, ctrlSpace),
        'Ctrl+Space failed');

    // Open Quick View with the check-selected files.
    await openQuickViewMultipleSelection(appId, ['tall', 'hello']);

    // Wait for the Quick View <webview> to load and display its content.
    function checkWebViewTextLoaded(elements) {
      let haveElements = Array.isArray(elements) && elements.length === 1;
      if (haveElements) {
        haveElements = elements[0].styles.display.includes('block');
      }
      if (!haveElements || !elements[0].attributes.src) {
        return pending(caller, 'Waiting for <webview> to load.');
      }
      return;
    }
    await repeatUntil(async () => {
      return checkWebViewTextLoaded(await remoteCall.callRemoteTestUtil(
          'deepQueryAllElements', appId, [webView, ['display']]));
    });

    // Press the down arrow key to select the next file.
    const downArrow = ['#quick-view', 'ArrowDown', false, false, false];
    chrome.test.assertTrue(
        await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, downArrow));

    // Wait until the <webview> displays that file's content.
    await repeatUntil(async () => {
      const getTextContent = 'window.document.body.textContent';
      const text = await remoteCall.callRemoteTestUtil(
          'deepExecuteScriptInWebView', appId, [webView, getTextContent]);
      // Check: the content of ENTRIES.hello should be shown.
      if (!text || !text[0].includes('This is a sample file')) {
        return pending(caller, 'Waiting for <webview> content.');
      }
    });

    // Press the up arrow key to select the previous file.
    const upArrow = ['#quick-view', 'ArrowUp', false, false, false];
    chrome.test.assertTrue(
        await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, upArrow));

    // Wait until the <webview> displays that file's content.
    await repeatUntil(async () => {
      const getTextContent = 'window.document.body.textContent';
      const text = await remoteCall.callRemoteTestUtil(
          'deepExecuteScriptInWebView', appId, [webView, getTextContent]);
      // Check: the content of ENTRIES.tallText should be shown.
      if (!text || !text[0].includes('42 tall text')) {
        return pending(caller, 'Waiting for <webview> content.');
      }
    });
  };

  /**
   * Tests that the content panel changes when using the left/right arrow keys
   * when multiple files are selected.
   */
  testcase.openQuickViewWithMultipleFilesKeyboardLeftRight = async () => {
    const caller = getCaller();

    /**
     * The text <webview> resides in the #quick-view shadow DOM, as a child of
     * the #dialog element.
     */
    const webView = ['#quick-view', '#dialog[open] webview.text-content'];

    // Open Files app on Downloads containing three text files.
    const files = [ENTRIES.hello, ENTRIES.tallText, ENTRIES.plainText];
    const appId = await setupAndWaitUntilReady(RootPath.DOWNLOADS, files, []);

    // Add item 1 to the check-selection, ENTRIES.tallText.
    const downKey = ['#file-list', 'ArrowDown', false, false, false];
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, downKey),
        'ArrowDown failed');
    const ctrlSpace = ['#file-list', ' ', true, false, false];
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, ctrlSpace),
        'Ctrl+Space failed');

    // Add item 3 to the check-selection, ENTRIES.hello.
    const ctrlDown = ['#file-list', 'ArrowDown', true, false, false];
    for (let i = 0; i < 2; i++) {
      chrome.test.assertTrue(
          !!await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, ctrlDown),
          'Ctrl+ArrowDown failed');
    }
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, ctrlSpace),
        'Ctrl+Space failed');

    // Open Quick View with the check-selected files.
    await openQuickViewMultipleSelection(appId, ['tall', 'hello']);

    // Wait for the Quick View <webview> to load and display its content.
    function checkWebViewTextLoaded(elements) {
      let haveElements = Array.isArray(elements) && elements.length === 1;
      if (haveElements) {
        haveElements = elements[0].styles.display.includes('block');
      }
      if (!haveElements || !elements[0].attributes.src) {
        return pending(caller, 'Waiting for <webview> to load.');
      }
      return;
    }
    await repeatUntil(async () => {
      return checkWebViewTextLoaded(await remoteCall.callRemoteTestUtil(
          'deepQueryAllElements', appId, [webView, ['display']]));
    });

    // Press the right arrow key to select the next file item.
    const rightArrow = ['#quick-view', 'ArrowRight', false, false, false];
    chrome.test.assertTrue(
        await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, rightArrow));

    // Wait until the <webview> displays that file's content.
    await repeatUntil(async () => {
      const getTextContent = 'window.document.body.textContent';
      const text = await remoteCall.callRemoteTestUtil(
          'deepExecuteScriptInWebView', appId, [webView, getTextContent]);
      // Check: the content of ENTRIES.hello should be shown.
      if (!text || !text[0].includes('This is a sample file')) {
        return pending(caller, 'Waiting for <webview> content.');
      }
    });

    // Press the left arrow key to select the previous file item.
    const leftArrow = ['#quick-view', 'ArrowLeft', false, false, false];
    chrome.test.assertTrue(
        await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, leftArrow));

    // Wait until the <webview> displays that file's content.
    await repeatUntil(async () => {
      const getTextContent = 'window.document.body.textContent';
      const text = await remoteCall.callRemoteTestUtil(
          'deepExecuteScriptInWebView', appId, [webView, getTextContent]);
      // Check: the content of ENTRIES.tallText should be shown.
      if (!text || !text[0].includes('42 tall text')) {
        return pending(caller, 'Waiting for <webview> content.');
      }
    });
  };

  /**
   * Tests opening Quick View and closing with Escape key returns focus to file
   * list.
   */
  testcase.openQuickViewAndEscape = async () => {
    // Open Files app on Downloads containing ENTRIES.hello.
    const appId =
        await setupAndWaitUntilReady(RootPath.DOWNLOADS, [ENTRIES.hello], []);

    // Open the file in Quick View.
    await openQuickView(appId, ENTRIES.hello.nameText);

    // Hit Escape key to close Quick View.
    const panelElements = ['#quick-view', '#contentPanel'];
    const key = [panelElements, 'Escape', false, false, false];
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, key),
        'fakeKeyDown Escape failed');

    // Check: the Quick View element should not be shown.
    await waitQuickViewClose(appId);

    // Check: the file list should gain the focus.
    const element = await remoteCall.waitForElement(appId, '#file-list:focus');
    chrome.test.assertEq(
        'file-list', element.attributes['id'], '#file-list should be focused');
  };

  /**
   * Test opening Quick View when Directory Tree is focused it should display if
   * there is only 1 file/folder selected in the file list.
   */
  testcase.openQuickViewFromDirectoryTree = async () => {
    // Open Files app on Downloads containing ENTRIES.hello.
    const appId =
        await setupAndWaitUntilReady(RootPath.DOWNLOADS, [ENTRIES.hello], []);

    // Focus Directory Tree.
    await remoteCall.focus(appId, ['#directory-tree']);

    // Ctrl+A to select the only file.
    const ctrlA = ['#directory-tree', 'a', true, false, false];
    await remoteCall.fakeKeyDown(appId, ...ctrlA);

    // Use selection menu button to open Quick View.
    await remoteCall.simulateUiClick(
        appId, '#selection-menu-button:not([hidden])');

    // Wait because WebUI Menu ignores the following click if it happens in
    // <200ms from the previous click.
    await wait(300);

    // Click the Menu item to show the Quick View.
    const getInfoMenuItem = '#file-context-menu:not([hidden]) ' +
        ' [command="#get-info"]:not([hidden])';
    await remoteCall.simulateUiClick(appId, getInfoMenuItem);

    // Check: the Quick View dialog should be shown.
    const caller = getCaller();
    await repeatUntil(async () => {
      const query = ['#quick-view', '#dialog[open]'];
      const elements = await remoteCall.callRemoteTestUtil(
          'deepQueryAllElements', appId, [query, ['display']]);
      const haveElements = Array.isArray(elements) && elements.length !== 0;
      if (!haveElements || elements[0].styles.display !== 'block') {
        return pending(caller, 'Waiting for Quick View to open.');
      }
      return true;
    });
  };

  /**
   * Tests the tab-index focus order when sending tab keys when an image file is
   * shown in Quick View.
   */
  testcase.openQuickViewTabIndexImage = async () => {
    // Prepare a list of tab-index focus queries.
    const tabQueries = [
      {'query': ['#quick-view', '[aria-label="Back"]:focus']},
      {'query': ['#quick-view', '[aria-label="Open"]:focus']},
      {'query': ['#quick-view', '[aria-label="File info"]:focus']},
      {'query': ['#quick-view', '[aria-label="Back"]:focus']},
    ];

    // Open Files app on Downloads containing ENTRIES.smallJpeg.
    const appId = await setupAndWaitUntilReady(
        RootPath.DOWNLOADS, [ENTRIES.smallJpeg], []);

    // Open the file in Quick View.
    await openQuickView(appId, ENTRIES.smallJpeg.nameText);

    for (const query of tabQueries) {
      // Make the browser dispatch a tab key event to FilesApp.
      const result = await sendTestMessage(
          {name: 'dispatchTabKey', shift: query.shift || false});
      chrome.test.assertEq(
          result, 'tabKeyDispatched', 'Tab key dispatch failure');

      // Note: Allow 500ms between key events to filter out the focus
      // traversal problems noted in crbug.com/907380#c10.
      await wait(500);

      // Check: the queried element should gain the focus.
      await remoteCall.waitForElement(appId, query.query);
    }
  };

  /**
   * Tests the tab-index focus order when sending tab keys when a text file is
   * shown in Quick View.
   */
  testcase.openQuickViewTabIndexText = async () => {
    // Prepare a list of tab-index focus queries.
    const tabQueries = [
      {'query': ['#quick-view', '[aria-label="Back"]:focus']},
      {'query': ['#quick-view', '[aria-label="Open"]:focus']},
      {'query': ['#quick-view', '[aria-label="File info"]:focus']},
      {'query': ['#quick-view']},  // Tab past the content panel.
      {'query': ['#quick-view', '[aria-label="Back"]:focus']},
    ];

    // Open Files app on Downloads containing ENTRIES.tallText.
    const appId = await setupAndWaitUntilReady(
        RootPath.DOWNLOADS, [ENTRIES.tallText], []);

    // Open the file in Quick View.
    await openQuickView(appId, ENTRIES.tallText.nameText);

    for (const query of tabQueries) {
      // Make the browser dispatch a tab key event to FilesApp.
      const result = await sendTestMessage(
          {name: 'dispatchTabKey', shift: query.shift || false});
      chrome.test.assertEq(
          result, 'tabKeyDispatched', 'Tab key dispatch failure');

      // Note: Allow 500ms between key events to filter out the focus
      // traversal problems noted in crbug.com/907380#c10.
      await wait(500);

      // Check: the queried element should gain the focus.
      await remoteCall.waitForElement(appId, query.query);
    }
  };

  /**
   * Tests the tab-index focus order when sending tab keys when an audio file is
   * shown in Quick View.
   */
  testcase.openQuickViewTabIndexAudio = async () => {
    // Open Files app on Downloads containing ENTRIES.beautiful song.
    const appId = await setupAndWaitUntilReady(
        RootPath.DOWNLOADS, [ENTRIES.beautiful], []);

    // Open the file in Quick View.
    await openQuickView(appId, ENTRIES.beautiful.nameText);

    // Prepare a list of tab-index focus queries.
    const tabQueries = [
      {'query': ['#quick-view', '[aria-label="Back"]:focus']},
      {'query': ['#quick-view', '[aria-label="Open"]:focus']},
      {'query': ['#quick-view', '[aria-label="File info"]:focus']},
    ];

    for (const query of tabQueries) {
      // Make the browser dispatch a tab key event to FilesApp.
      const result = await sendTestMessage(
          {name: 'dispatchTabKey', shift: query.shift || false});
      chrome.test.assertEq(
          result, 'tabKeyDispatched', 'Tab key dispatch failure');

      // Note: Allow 500ms between key events to filter out the focus
      // traversal problems noted in crbug.com/907380#c10.
      await wait(500);

      // Check: the queried element should gain the focus.
      await remoteCall.waitForElement(appId, query.query);
    }

    // Send tab keys until Back gains the focus again.
    while (true) {
      // Make the browser dispatch a tab key event to FilesApp.
      const result =
          await sendTestMessage({name: 'dispatchTabKey', shift: false});
      chrome.test.assertEq(
          result, 'tabKeyDispatched', 'Tab key dispatch failure');

      // Note: Allow 500ms between key events to filter out the focus
      // traversal problems noted in crbug.com/907380#c10.
      await wait(500);

      // Check: back should eventually get the focus again.
      const activeElement = await remoteCall.callRemoteTestUtil(
          'deepGetActiveElement', appId, []);
      if (activeElement.attributes['aria-label'] === 'Back') {
        break;
      }
    }
  };

  /**
   * Tests the tab-index focus order when sending tab keys when a video file is
   * shown in Quick View.
   */
  testcase.openQuickViewTabIndexVideo = async () => {
    // Open Files app on Downloads containing ENTRIES.webm video.
    const appId =
        await setupAndWaitUntilReady(RootPath.DOWNLOADS, [ENTRIES.webm], []);

    // Open the file in Quick View.
    await openQuickView(appId, ENTRIES.webm.nameText);

    // Prepare a list of tab-index focus queries.
    const tabQueries = [
      {'query': ['#quick-view', '[aria-label="Back"]:focus']},
      {'query': ['#quick-view', '[aria-label="Open"]:focus']},
      {'query': ['#quick-view', '[aria-label="File info"]:focus']},
    ];

    for (const query of tabQueries) {
      // Make the browser dispatch a tab key event to FilesApp.
      const result = await sendTestMessage(
          {name: 'dispatchTabKey', shift: query.shift || false});
      chrome.test.assertEq(
          result, 'tabKeyDispatched', 'Tab key dispatch failure');

      // Note: Allow 500ms between key events to filter out the focus
      // traversal problems noted in crbug.com/907380#c10.
      await wait(500);

      // Check: the queried element should gain the focus.
      await remoteCall.waitForElement(appId, query.query);
    }

    // Send tab keys until Back gains the focus again.
    while (true) {
      // Make the browser dispatch a tab key event to FilesApp.
      const result =
          await sendTestMessage({name: 'dispatchTabKey', shift: false});
      chrome.test.assertEq(
          result, 'tabKeyDispatched', 'Tab key dispatch failure');

      // Note: Allow 500ms between key events to filter out the focus
      // traversal problems noted in crbug.com/907380#c10.
      await wait(500);

      // Check: back should eventually get the focus again.
      const activeElement = await remoteCall.callRemoteTestUtil(
          'deepGetActiveElement', appId, []);
      if (activeElement.attributes['aria-label'] === 'Back') {
        break;
      }
    }
  };
})();
