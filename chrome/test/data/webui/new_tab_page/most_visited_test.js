// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'chrome://new-tab-page/most_visited.js';
import 'chrome://resources/mojo/mojo/public/js/mojo_bindings_lite.js';
import 'chrome://resources/mojo/mojo/public/mojom/base/text_direction.mojom-lite.js';

import {BrowserProxy} from 'chrome://new-tab-page/browser_proxy.js';
import {isMac} from 'chrome://resources/js/cr.m.js';
import {assertStyle, keydown, TestProxy} from 'chrome://test/new_tab_page/test_support.js';
import {eventToPromise, flushTasks} from 'chrome://test/test_util.m.js';

suite('NewTabPageMostVisitedTest', () => {
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
   * @param {boolean=} customLinksEnabled
   * @param {boolean=} visible
   * @return {!Promise}
   * @private
   */
  async function addTiles(n, customLinksEnabled = true, visible = true) {
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
      customLinksEnabled: customLinksEnabled,
      tiles: tiles,
      visible: visible,
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

  test('empty shows add shortcut only', async () => {
    assertTrue(mostVisited.$.addShortcut.hidden);
    await addTiles(0);
    assertEquals(0, queryTiles().length);
    assertFalse(mostVisited.$.addShortcut.hidden);
  });

  test('clicking on add shortcut opens dialog', () => {
    assertFalse(mostVisited.$.dialog.open);
    mostVisited.$.addShortcut.click();
    assertTrue(mostVisited.$.dialog.open);
  });

  test('pressing enter when add shortcut has focus opens dialog', () => {
    mostVisited.$.addShortcut.focus();
    assertFalse(mostVisited.$.dialog.open);
    keydown(mostVisited.$.addShortcut, 'Enter');
    assertTrue(mostVisited.$.dialog.open);
  });

  test('pressing space when add shortcut has focus opens dialog', () => {
    mostVisited.$.addShortcut.focus();
    assertFalse(mostVisited.$.dialog.open);
    keydown(mostVisited.$.addShortcut, ' ');
    assertTrue(mostVisited.$.dialog.open);
  });

  test('four tiles fit on one line with addShortcut', async () => {
    await addTiles(4);
    assertEquals(4, queryTiles().length);
    assertFalse(mostVisited.$.addShortcut.hidden);
    const tops = queryAll('a').map(({offsetTop}) => offsetTop);
    assertEquals(5, tops.length);
    tops.forEach(top => {
      assertEquals(tops[0], top);
    });
  });

  test('five tiles are displayed on two rows with addShortcut', async () => {
    await addTiles(5);
    assertEquals(5, queryTiles().length);
    assertFalse(mostVisited.$.addShortcut.hidden);
    const tops = queryAll('a').map(({offsetTop}) => offsetTop);
    assertEquals(6, tops.length);
    const firstRowTop = tops[0];
    const secondRowTop = tops[3];
    assertNotEquals(firstRowTop, secondRowTop);
    tops.slice(0, 3).forEach(top => {
      assertEquals(firstRowTop, top);
    });
    tops.slice(3).forEach(top => {
      assertEquals(secondRowTop, top);
    });
  });

  test('nine tiles are displayed on two rows with addShortcut', async () => {
    await addTiles(9);
    assertEquals(9, queryTiles().length);
    assertFalse(mostVisited.$.addShortcut.hidden);
    const tops = queryAll('a').map(({offsetTop}) => offsetTop);
    assertEquals(10, tops.length);
    const firstRowTop = tops[0];
    const secondRowTop = tops[5];
    assertNotEquals(firstRowTop, secondRowTop);
    tops.slice(0, 5).forEach(top => {
      assertEquals(firstRowTop, top);
    });
    tops.slice(5).forEach(top => {
      assertEquals(secondRowTop, top);
    });
  });

  test('ten tiles are displayed on two rows without addShortcut', async () => {
    await addTiles(10);
    assertEquals(10, queryTiles().length);
    assertTrue(mostVisited.$.addShortcut.hidden);
    const tops = queryAll('a:not([hidden])').map(a => a.offsetTop);
    assertEquals(10, tops.length);
    const firstRowTop = tops[0];
    const secondRowTop = tops[5];
    assertNotEquals(firstRowTop, secondRowTop);
    tops.slice(0, 5).forEach(top => {
      assertEquals(firstRowTop, top);
    });
    tops.slice(5).forEach(top => {
      assertEquals(secondRowTop, top);
    });
  });

  test('ten tiles is the max tiles displayed', async () => {
    await addTiles(11);
    assertEquals(10, queryTiles().length);
    assertTrue(mostVisited.$.addShortcut.hidden);
  });

  test('eight tiles is the max (customLinksEnabled=false)', async () => {
    await addTiles(11, /* customLinksEnabled */ true);
    assertEquals(10, queryTiles().length);
    assertEquals(0, queryAll('.tile[hidden]').length);
    assertTrue(mostVisited.$.addShortcut.hidden);
    await addTiles(11, /* customLinksEnabled */ false);
    assertEquals(10, queryTiles().length);
    assertEquals(2, queryAll('.tile[hidden]').length);
    assertTrue(mostVisited.$.addShortcut.hidden);
    await addTiles(11, /* customLinksEnabled */ true);
    assertEquals(10, queryTiles().length);
    assertEquals(0, queryAll('.tile[hidden]').length);
  });

  test('7 tiles and no add shortcut (customLinksEnabled=false)', async () => {
    await addTiles(7, /* customLinksEnabled */ true);
    assertFalse(mostVisited.$.addShortcut.hidden);
    await addTiles(7, /* customLinksEnabled */ false);
    assertTrue(mostVisited.$.addShortcut.hidden);
    await addTiles(7, /* customLinksEnabled */ true);
    assertFalse(mostVisited.$.addShortcut.hidden);
  });

  test('no tiles shown when (visible=false)', async () => {
    await addTiles(1);
    assertEquals(1, queryTiles().length);
    assertEquals(0, queryAll('.tile[hidden]').length);
    await addTiles(1, /* customLinksEnabled */ true, /* visible */ false);
    assertEquals(1, queryTiles().length);
    assertEquals(1, queryAll('.tile[hidden]').length);
    await addTiles(1, /* customLinksEnabled */ true, /* visible */ true);
    assertEquals(1, queryTiles().length);
    assertEquals(0, queryAll('.tile[hidden]').length);
  });

  test('dialog opens when add shortcut clicked', () => {
    const {dialog} = mostVisited.$;
    assertFalse(dialog.open);
    mostVisited.$.addShortcut.click();
    assertTrue(dialog.open);
  });

  suite('add dialog', () => {
    /** @private {CrDialogElement} */
    let dialog;
    /** @private {CrInputElement} */
    let inputName;
    /** @private {CrInputElement} */
    let inputUrl;
    /** @private {CrButtonElement} */
    let saveButton;
    /** @private {CrButtonElement} */
    let cancelButton;

    setup(() => {
      dialog = mostVisited.$.dialog;
      inputName = mostVisited.$.dialogInputName;
      inputUrl = mostVisited.$.dialogInputUrl;
      saveButton = dialog.querySelector('.action-button');
      cancelButton = dialog.querySelector('.cancel-button');
      mostVisited.$.addShortcut.click();
    });

    test('inputs are initially empty', () => {
      assertEquals('', inputName.value);
      assertEquals('', inputUrl.value);
    });

    test('saveButton is enabled with URL is not empty', () => {
      assertTrue(saveButton.disabled);

      inputName.value = 'name';
      assertTrue(saveButton.disabled);

      inputUrl.value = 'url';
      assertFalse(saveButton.disabled);

      inputUrl.value = '';
      assertTrue(saveButton.disabled);
    });

    test('cancel closes dialog', () => {
      assertTrue(dialog.open);
      cancelButton.click();
      assertFalse(dialog.open);
    });

    test('inputs are clear after dialog reuse', () => {
      inputName.value = 'name';
      inputUrl.value = 'url';
      cancelButton.click();
      mostVisited.$.addShortcut.click();
      assertEquals('', inputName.value);
      assertEquals('', inputUrl.value);
    });

    test('use URL input for title when title empty', async () => {
      inputUrl.value = 'url';
      const addCalled = testProxy.handler.whenCalled('addMostVisitedTile');
      saveButton.click();
      const [url, title] = await addCalled;
      assertEquals('url', title);
    });

    test('toast shown on save', async () => {
      inputUrl.value = 'url';
      assertFalse(mostVisited.$.toast.open);
      const addCalled = testProxy.handler.whenCalled('addMostVisitedTile');
      saveButton.click();
      await addCalled;
      assertTrue(mostVisited.$.toast.open);
    });

    test('save name and URL', async () => {
      inputName.value = 'name';
      inputUrl.value = 'https://url/';
      const addCalled = testProxy.handler.whenCalled('addMostVisitedTile');
      saveButton.click();
      const [{url}, title] = await addCalled;
      assertEquals('name', title);
      assertEquals('https://url/', url);
    });

    test('dialog closes on save', () => {
      inputUrl.value = 'url';
      assertTrue(dialog.open);
      saveButton.click();
      assertFalse(dialog.open);
    });

    test('https:// is added if no scheme is used', async () => {
      inputUrl.value = 'url';
      const addCalled = testProxy.handler.whenCalled('addMostVisitedTile');
      saveButton.click();
      const [{url}, title] = await addCalled;
      assertEquals('https://url/', url);
    });

    test('http is a valid scheme', async () => {
      inputUrl.value = 'http://url';
      const addCalled = testProxy.handler.whenCalled('addMostVisitedTile');
      saveButton.click();
      await addCalled;
    });

    test('https is a valid scheme', async () => {
      inputUrl.value = 'https://url';
      const addCalled = testProxy.handler.whenCalled('addMostVisitedTile');
      saveButton.click();
      await addCalled;
    });

    test('chrome is not a valid scheme', () => {
      inputUrl.value = 'chrome://url';
      assertFalse(inputUrl.invalid);
      saveButton.click();
      assertTrue(inputUrl.invalid);
    });
  });

  test('open edit dialog', async () => {
    await addTiles(2);
    const {actionMenu, dialog} = mostVisited.$;
    assertFalse(actionMenu.open);
    queryTiles()[0].querySelector('cr-icon-button').click();
    assertTrue(actionMenu.open);
    assertFalse(dialog.open);
    mostVisited.$.actionMenuEdit.click();
    assertFalse(actionMenu.open);
    assertTrue(dialog.open);
  });

  suite('edit dialog', () => {
    /** @private {CrActionMenuElement} */
    let actionMenu;
    /** @private {CrIconButtonElement} */
    let actionMenuButton;
    /** @private {CrDialogElement} */
    let dialog;
    /** @private {CrInputElement} */
    let inputName;
    /** @private {CrInputElement} */
    let inputUrl;
    /** @private {CrButtonElement} */
    let saveButton;
    /** @private {CrButtonElement} */
    let cancelButton;
    /** @private {HTMLElement} */
    let tile;

    setup(async () => {
      actionMenu = mostVisited.$.actionMenu;
      dialog = mostVisited.$.dialog;
      inputName = mostVisited.$.dialogInputName;
      inputUrl = mostVisited.$.dialogInputUrl;
      saveButton = dialog.querySelector('.action-button');
      cancelButton = dialog.querySelector('.cancel-button');
      await addTiles(2);
      tile = queryTiles()[1];
      actionMenuButton = tile.querySelector('cr-icon-button');
      actionMenuButton.click();
      mostVisited.$.actionMenuEdit.click();
    });

    test('edit a tile URL', async () => {
      assertEquals('https://b/', inputUrl.value);
      const updateCalled =
          testProxy.handler.whenCalled('updateMostVisitedTile');
      inputUrl.value = 'updated-url';
      saveButton.click();
      const [url, newUrl, newTitle] = await updateCalled;
      assertEquals('https://updated-url/', newUrl.url);
    });

    test('toast shown when tile editted', async () => {
      inputUrl.value = 'updated-url';
      assertFalse(mostVisited.$.toast.open);
      saveButton.click();
      await flushTasks();
      assertTrue(mostVisited.$.toast.open);
    });

    test('no toast when not editted', async () => {
      assertFalse(mostVisited.$.toast.open);
      saveButton.click();
      await flushTasks();
      assertFalse(mostVisited.$.toast.open);
    });

    test('edit a tile title', async () => {
      assertEquals('b', inputName.value);
      const updateCalled =
          testProxy.handler.whenCalled('updateMostVisitedTile');
      inputName.value = 'updated name';
      saveButton.click();
      const [url, newUrl, newTitle] = await updateCalled;
      assertEquals('updated name', newTitle);
    });

    test('update not called when name and URL not changed', async () => {
      // |updateMostVisitedTile| will be called only after either the title or
      // url has changed.
      const updateCalled =
          testProxy.handler.whenCalled('updateMostVisitedTile');
      saveButton.click();

      // Reopen dialog and edit URL.
      actionMenuButton.click();
      mostVisited.$.actionMenuEdit.click();
      inputUrl.value = 'updated-url';
      saveButton.click();

      const [url, newUrl, newTitle] = await updateCalled;
      assertEquals('https://updated-url/', newUrl.url);
    });
  });

  test('remove with action menu', async () => {
    const {actionMenu, actionMenuRemove: removeButton} = mostVisited.$;
    await addTiles(2);
    const secondTile = queryTiles()[1];
    const actionMenuButton = secondTile.querySelector('cr-icon-button');

    assertFalse(actionMenu.open);
    actionMenuButton.click();
    assertTrue(actionMenu.open);
    const deleteCalled = testProxy.handler.whenCalled('deleteMostVisitedTile');
    assertFalse(mostVisited.$.toast.open);
    removeButton.click();
    assertFalse(actionMenu.open);
    assertEquals('https://b/', (await deleteCalled).url);
    assertTrue(mostVisited.$.toast.open);
  });

  test('remove with icon button (customLinksEnabled=false)', async () => {
    await addTiles(1, /* customLinksEnabled */ false);
    const removeButton = queryTiles()[0].querySelector('cr-icon-button');
    const deleteCalled = testProxy.handler.whenCalled('deleteMostVisitedTile');
    removeButton.click();
    assertEquals('https://a/', (await deleteCalled).url);
  });

  test('tile url is set to href of <a>', async () => {
    await addTiles(1);
    const [tile] = queryTiles();
    assertEquals('https://a/', tile.href);
  });

  test('delete first tile', async () => {
    await addTiles(1);
    const [tile] = queryTiles();
    const deleteCalled = testProxy.handler.whenCalled('deleteMostVisitedTile');
    assertFalse(mostVisited.$.toast.open);
    keydown(tile, 'Delete');
    assertEquals('https://a/', (await deleteCalled).url);
    assertTrue(mostVisited.$.toast.open);
  });

  test('ctrl+z undo', async () => {
    const undoCalled =
        testProxy.handler.whenCalled('undoMostVisitedTileAction');
    mostVisited.dispatchEvent(new KeyboardEvent('keydown', {
      bubbles: true,
      ctrlKey: !isMac,
      key: 'z',
      metaKey: isMac,
    }));
    await undoCalled;
  });

  test('toast restore defaults button', async () => {
    const wait = testProxy.handler.whenCalled('restoreMostVisitedDefaults');
    const {toast} = mostVisited.$;
    toast.querySelector('dom-if').if = true;
    await flushTasks();
    assertFalse(toast.open);
    toast.show('');
    assertTrue(toast.open);
    toast.querySelector('#restore').click();
    await wait;
    assertFalse(toast.open);
  });

  test('toast undo button', async () => {
    const wait = testProxy.handler.whenCalled('undoMostVisitedTileAction');
    const {toast} = mostVisited.$;
    toast.querySelector('dom-if').if = true;
    await flushTasks();
    assertFalse(toast.open);
    toast.show('');
    assertTrue(toast.open);
    toast.querySelector('#undo').click();
    await wait;
    assertFalse(toast.open);
  });

  test('drag first tile to second position', async () => {
    await addTiles(2);
    const [first, second] = queryTiles();
    const firstRect = first.getBoundingClientRect();
    const secondRect = second.getBoundingClientRect();
    first.dispatchEvent(new DragEvent('dragstart', {
      clientX: firstRect.x + firstRect.width / 2,
      clientY: firstRect.y + firstRect.height / 2,
    }));
    await flushTasks();
    const reorderCalled =
        testProxy.handler.whenCalled('reorderMostVisitedTile');
    document.dispatchEvent(new DragEvent('dragend', {
      clientX: secondRect.x + 1,
      clientY: secondRect.y + 1,
    }));
    const [url, newPos] = await reorderCalled;
    assertEquals('https://a/', url.url);
    assertEquals(1, newPos);
    const [newFirst, newSecond] = queryTiles();
    assertEquals('https://b/', newFirst.href);
    assertEquals('https://a/', newSecond.href);
  });

  test('drag second tile to first position', async () => {
    await addTiles(2);
    const [first, second] = queryTiles();
    const firstRect = first.getBoundingClientRect();
    const secondRect = second.getBoundingClientRect();
    second.dispatchEvent(new DragEvent('dragstart', {
      clientX: secondRect.x + secondRect.width / 2,
      clientY: secondRect.y + secondRect.height / 2,
    }));
    await flushTasks();
    const reorderCalled =
        testProxy.handler.whenCalled('reorderMostVisitedTile');
    document.dispatchEvent(new DragEvent('dragend', {
      clientX: firstRect.x + 1,
      clientY: firstRect.y + 1,
    }));
    const [url, newPos] = await reorderCalled;
    assertEquals('https://b/', url.url);
    assertEquals(0, newPos);
    const [newFirst, newSecond] = queryTiles();
    assertEquals('https://b/', newFirst.href);
    assertEquals('https://a/', newSecond.href);
  });

  test('RIGHT_TO_LEFT tile title text direction', async () => {
    const tilesRendered = eventToPromise('dom-change', mostVisited.$.tiles);
    testProxy.callbackRouterRemote.setMostVisitedInfo({
      customLinksEnabled: true,
      tiles: [{
        title: 'title',
        titleDirection: mojoBase.mojom.TextDirection.RIGHT_TO_LEFT,
        url: {url: 'https://url/'},
      }],
      visible: true,
    });
    await testProxy.callbackRouterRemote.$.flushForTesting();
    await tilesRendered;
    const [tile] = queryTiles();
    const titleElement = tile.querySelector('.tile-title');
    assertEquals('rtl', window.getComputedStyle(titleElement).direction);

    tile.querySelector('cr-icon-button').click();
    mostVisited.$.actionMenuEdit.click();
    assertEquals(
        'rtl',
        window.getComputedStyle(mostVisited.$.dialogInputName).direction);
  });

  test('LEFT_TO_RIGHT tile title text direction', async () => {
    const tilesRendered = eventToPromise('dom-change', mostVisited.$.tiles);
    testProxy.callbackRouterRemote.setMostVisitedInfo({
      customLinksEnabled: true,
      tiles: [{
        title: 'title',
        titleDirection: mojoBase.mojom.TextDirection.LEFT_TO_RIGHT,
        url: {url: 'https://url/'},
      }],
      visible: true,
    });
    await testProxy.callbackRouterRemote.$.flushForTesting();
    await tilesRendered;
    const [tile] = queryTiles();
    const titleElement = tile.querySelector('.tile-title');
    assertEquals('ltr', window.getComputedStyle(titleElement).direction);

    tile.querySelector('cr-icon-button').click();
    mostVisited.$.actionMenuEdit.click();
    assertEquals(
        'ltr',
        window.getComputedStyle(mostVisited.$.dialogInputName).direction);
  });

  test('setting color styles tile color', () => {
    // Act.
    mostVisited.style.setProperty('--tile-title-color', 'blue');
    mostVisited.style.setProperty('--icon-background-color', 'red');

    // Assert.
    queryAll('.tile-title').forEach(tile => {
      assertStyle(tile, 'color', 'rgb(0, 0, 255)');
    });
    queryAll('.tile-icon').forEach(tile => {
      assertStyle(tile, 'background-color', 'rgb(255, 0, 0)');
    });
    assertStyle(
        mostVisited.$.addShortCutIcon, 'background-color', 'rgb(0, 0, 255)');
  });
});
