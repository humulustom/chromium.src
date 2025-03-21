// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'chrome://new-tab-page/most_visited.js';
import 'chrome://resources/mojo/mojo/public/js/mojo_bindings_lite.js';
import 'chrome://resources/mojo/mojo/public/mojom/base/text_direction.mojom-lite.js';

import {BrowserProxy} from 'chrome://new-tab-page/browser_proxy.js';
import {getDeepActiveElement} from 'chrome://resources/js/util.m.js';
import {assertFocus, keydown, TestProxy} from 'chrome://test/new_tab_page/test_support.js';
import {eventToPromise, flushTasks} from 'chrome://test/test_util.m.js';

suite('NewTabPageMostVisitedFocusTest', () => {
  /** @type {!MostVisitedElement} */
  let mostVisited;

  /** @type {TestProxy} */
  let testProxy;

  /**
   * @param {string}
   * @return {!Array<!HTMLElement>}
   * @private
   */
  function queryAll(q) {
    return Array.from(mostVisited.shadowRoot.querySelectorAll(q));
  }

  /**
   * @return {!Array<!HTMLElement>}
   * @private
   */
  function queryTiles() {
    return queryAll('.tile');
  }

  /**
   * @param {number} n
   * @return {!Promise}
   * @private
   */
  async function addTiles(n) {
    const tiles = Array(n).fill(0).map((x, i) => {
      const char = String.fromCharCode(i + /* 'a' */ 97);
      return {
        title: char,
        titleDirection: mojoBase.mojom.TextDirection.LEFT_TO_RIGHT,
        url: {url: `https://${char}/`},
      };
    });
    const tilesRendered = eventToPromise('dom-change', mostVisited.$.tiles);
    testProxy.callbackRouterRemote.setMostVisitedInfo({
      customLinksEnabled: true,
      tiles: tiles,
      visible: true,
    });
    await testProxy.callbackRouterRemote.$.flushForTesting();
    await tilesRendered;
  }

  setup(() => {
    PolymerTest.clearBody();

    testProxy = new TestProxy();
    BrowserProxy.instance_ = testProxy;

    mostVisited = document.createElement('ntp-most-visited');
    document.body.appendChild(mostVisited);
  });

  test('right focuses on addShortcut', async () => {
    await addTiles(1);
    const [tile] = queryTiles();
    tile.focus();
    keydown(tile, 'ArrowRight');
    assertFocus(mostVisited.$.addShortcut);
  });

  test('right focuses on addShortcut when menu button focused', async () => {
    await addTiles(1);
    const [tile] = queryTiles();
    tile.querySelector('cr-icon-button').focus();
    keydown(tile, 'ArrowRight');
    assertFocus(mostVisited.$.addShortcut);
  });

  test('right focuses next tile', async () => {
    await addTiles(2);
    const [first, second] = queryTiles();
    first.focus();
    keydown(first, 'ArrowRight');
    assertFocus(second);
  });

  test('right focuses on next tile when menu button focused', async () => {
    await addTiles(2);
    const [first, second] = queryTiles();
    first.querySelector('cr-icon-button').focus();
    keydown(first, 'ArrowRight');
    assertFocus(second);
  });

  test('down focuses on addShortcut', async () => {
    await addTiles(1);
    const [tile] = queryTiles();
    tile.focus();
    keydown(tile, 'ArrowDown');
    assertFocus(mostVisited.$.addShortcut);
  });

  test('down focuses next tile', async () => {
    await addTiles(2);
    const [first, second] = queryTiles();
    first.focus();
    keydown(first, 'ArrowDown');
    assertFocus(second);
  });

  test('up focuses on previous tile from addShortcut', async () => {
    await addTiles(1);
    mostVisited.$.addShortcut.focus();
    keydown(mostVisited.$.addShortcut, 'ArrowUp');
    assertFocus(queryTiles()[0]);
  });

  test('up focuses on previous tile', async () => {
    await addTiles(2);
    const [first, second] = queryTiles();
    second.focus();
    keydown(second, 'ArrowUp');
    assertFocus(first);
  });

  test('up/left does not change focus when on first tile', async () => {
    await addTiles(1);
    const [tile] = queryTiles();
    tile.focus();
    keydown(tile, 'ArrowUp');
    assertFocus(tile);
    keydown(tile, 'ArrowLeft');
  });

  test('up/left/right/down addShortcut and no tiles', async () => {
    await addTiles(0);
    mostVisited.$.addShortcut.focus();
    ['ArrowUp', 'ArrowLeft', 'ArrowRight', 'ArrowDown'].forEach(key => {
      keydown(mostVisited.$.addShortcut, key);
      assertFocus(mostVisited.$.addShortcut);
    });
  });
});
