// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'chrome://new-tab-page/customize_themes.js';

import {BrowserProxy} from 'chrome://new-tab-page/browser_proxy.js';
import {assertStyle, TestProxy} from 'chrome://test/new_tab_page/test_support.js';
import {flushTasks} from 'chrome://test/test_util.m.js';

suite('NewTabPageCustomizeThemesTest', () => {
  /** @type {TestProxy} */
  let testProxy;

  /** @return {!CustomizeThemesElement} */
  function createCustomizeThemes() {
    const customizeThemes = document.createElement('ntp-customize-themes');
    document.body.appendChild(customizeThemes);
    return customizeThemes;
  }

  /** @return {!CustomizeThemesElement} */
  function createCustomizeThemesWithThemes(themes) {
    testProxy.handler.setResultFor('getChromeThemes', Promise.resolve({
      chromeThemes: themes,
    }));
    return createCustomizeThemes();
  }

  setup(() => {
    PolymerTest.clearBody();

    testProxy = new TestProxy();
    BrowserProxy.instance_ = testProxy;
  });

  test('creating element shows theme tiles', async () => {
    // Act.
    const themes = [
      {
        id: 0,
        label: 'theme_0',
        colors: {
          frame: {value: 0xff000000},      // white.
          activeTab: {value: 0xff0000ff},  // blue.
        },
      },
      {
        id: 1,
        label: 'theme_1',
        colors: {
          frame: {value: 0xffff0000},      // red.
          activeTab: {value: 0xff00ff00},  // green.
        },
      },
    ];
    const getChromeThemesCalled =
        testProxy.handler.whenCalled('getChromeThemes');

    // Act.
    const customizeThemes = createCustomizeThemesWithThemes(themes);
    await getChromeThemesCalled;
    await flushTasks();

    // Assert.
    const tiles = customizeThemes.shadowRoot.querySelectorAll('ntp-theme-icon');
    assertEquals(tiles.length, 4);
    assertEquals(tiles[2].getAttribute('title'), 'theme_0');
    assertStyle(tiles[2], '--ntp-theme-icon-frame-color', 'rgb(0, 0, 0)');
    assertStyle(
        tiles[2], '--ntp-theme-icon-active-tab-color', 'rgb(0, 0, 255)');
    assertEquals(tiles[3].getAttribute('title'), 'theme_1');
    assertStyle(tiles[3], '--ntp-theme-icon-frame-color', 'rgb(255, 0, 0)');
    assertStyle(
        tiles[3], '--ntp-theme-icon-active-tab-color', 'rgb(0, 255, 0)');
  });

  test('clicking default theme calls applying default theme', async () => {
    // Arrange.
    const customizeThemes = createCustomizeThemes();
    const applyDefaultThemeCalled =
        testProxy.handler.whenCalled('applyDefaultTheme');

    // Act.
    customizeThemes.$.defaultTheme.click();

    // Assert.
    await applyDefaultThemeCalled;
  });

  test('selecting color calls applying autogenerated theme', async () => {
    // Arrange.
    const customizeThemes = createCustomizeThemes();
    const applyAutogeneratedThemeCalled =
        testProxy.handler.whenCalled('applyAutogeneratedTheme');

    // Act.
    customizeThemes.$.colorPicker.value = '#ff0000';
    customizeThemes.$.colorPicker.dispatchEvent(new Event('change'));

    // Assert.
    const {value} = await applyAutogeneratedThemeCalled;
    assertEquals(value, 0xffff0000);
  });

  test('setting autogenerated theme selects and updates icon', async () => {
    // Arrange.
    const customizeThemes = createCustomizeThemes();

    // Act.
    customizeThemes.theme = {
      type: newTabPage.mojom.ThemeType.AUTOGENERATED,
      info: {
        autogeneratedThemeColors: {
          frame: {value: 0xffff0000},
          activeTab: {value: 0xff0000ff},
        },
      },
      backgroundColor: {value: 0xffff0000},
      shortcutBackgroundColor: {value: 0xff00ff00},
      shortcutTextColor: {value: 0xff0000ff},
    };
    await flushTasks();

    // Assert.
    const selectedIcons =
        customizeThemes.shadowRoot.querySelectorAll('ntp-theme-icon[selected]');
    assertEquals(selectedIcons.length, 1);
    assertEquals(selectedIcons[0], customizeThemes.$.autogeneratedTheme);
    assertStyle(
        selectedIcons[0], '--ntp-theme-icon-frame-color', 'rgb(255, 0, 0)');
    assertStyle(
        selectedIcons[0], '--ntp-theme-icon-active-tab-color',
        'rgb(0, 0, 255)');
  });

  test('setting default theme selects and updates icon', async () => {
    // Arrange.
    const customizeThemes = createCustomizeThemes();

    // Act.
    customizeThemes.theme = {
      type: newTabPage.mojom.ThemeType.DEFAULT,
      info: {chromeThemeId: 0},
      backgroundColor: {value: 0xffff0000},
      shortcutBackgroundColor: {value: 0xff00ff00},
      shortcutTextColor: {value: 0xff0000ff},
    };
    await flushTasks();

    // Assert.
    const selectedIcons =
        customizeThemes.shadowRoot.querySelectorAll('ntp-theme-icon[selected]');
    assertEquals(selectedIcons.length, 1);
    assertEquals(selectedIcons[0], customizeThemes.$.defaultTheme);
  });

  test('setting Chrome theme selects and updates icon', async () => {
    // Arrange.
    const themes = [
      {
        id: 0,
        label: 'foo',
        colors: {
          frame: {value: 0xff000000},
          activeTab: {value: 0xff0000ff},
        },
      },
    ];
    const customizeThemes = createCustomizeThemesWithThemes(themes);

    // Act.
    customizeThemes.theme = {
      type: newTabPage.mojom.ThemeType.CHROME,
      info: {chromeThemeId: 0},
      backgroundColor: {value: 0xffff0000},
      shortcutBackgroundColor: {value: 0xff00ff00},
      shortcutTextColor: {value: 0xff0000ff},
    };
    await flushTasks();

    // Assert.
    const selectedIcons =
        customizeThemes.shadowRoot.querySelectorAll('ntp-theme-icon[selected]');
    assertEquals(selectedIcons.length, 1);
    assertEquals(selectedIcons[0].getAttribute('title'), 'foo');
  });

  test('setting third-party theme shows uninstall UI', async () => {
    // Arrange.
    const customizeThemes = createCustomizeThemes();

    // Act.
    customizeThemes.theme = {
      type: newTabPage.mojom.ThemeType.THIRD_PARTY,
      info: {
        thirdPartyThemeInfo: {
          id: 'foo',
          name: 'bar',
        },
      },
      backgroundColor: {value: 0xffff0000},
      shortcutBackgroundColor: {value: 0xff00ff00},
      shortcutTextColor: {value: 0xff0000ff},
    };
    await testProxy.callbackRouterRemote.$.flushForTesting();

    // Assert.
    assertStyle(customizeThemes.$.thirdPartyThemeContainer, 'display', 'block');
    assertEquals(
        customizeThemes.$.thirdPartyThemeName.textContent.trim(), 'bar');
    assertEquals(
        customizeThemes.$.thirdPartyLink.getAttribute('href'),
        'https://chrome.google.com/webstore/detail/foo');
  });

  test('setting non-third-party theme hides uninstall UI', async () => {
    // Arrange.
    const customizeThemes = createCustomizeThemes();

    // Act.
    customizeThemes.theme = {
      type: newTabPage.mojom.ThemeType.DEFAULT,
      info: {chromeThemeId: 0},
      backgroundColor: {value: 0xffff0000},
      shortcutBackgroundColor: {value: 0xff00ff00},
      shortcutTextColor: {value: 0xff0000ff},
    };
    await testProxy.callbackRouterRemote.$.flushForTesting();

    // Assert.
    assertStyle(customizeThemes.$.thirdPartyThemeContainer, 'display', 'none');
  });

  test('uninstalling third-party theme sets default theme', async () => {
    // Arrange.
    const customizeThemes = createCustomizeThemes();
    customizeThemes.theme = {
      type: newTabPage.mojom.ThemeType.THIRD_PARTY,
      info: {
        thirdPartyThemeInfo: {
          id: 'foo',
          name: 'bar',
        },
      },
      backgroundColor: {value: 0xffff0000},
      shortcutBackgroundColor: {value: 0xff00ff00},
      shortcutTextColor: {value: 0xff0000ff},
    };
    await testProxy.callbackRouterRemote.$.flushForTesting();
    const applyDefaultThemeCalled =
        testProxy.handler.whenCalled('applyDefaultTheme');
    const confirmThemeChangesCalled =
        testProxy.handler.whenCalled('confirmThemeChanges');

    // Act.
    customizeThemes.$.uninstallThirdPartyButton.click();

    // Assert.
    await applyDefaultThemeCalled;
    await confirmThemeChangesCalled;
  });
});
