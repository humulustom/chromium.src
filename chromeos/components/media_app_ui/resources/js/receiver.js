// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/** @implements mediaApp.AbstractFileList */
class SingleArrayBufferFileList {
  /** @param {!mediaApp.AbstractFile} file */
  constructor(file) {
    this.file = file;
    this.length = 1;
  }
  /** @override */
  item(index) {
    return index === 0 ? this.file : null;
  }
}

/**
 * Loads files associated with a message received from the host.
 * @param {!mediaApp.MessageEventData} data
 */
async function load(data) {
  const fileList = new SingleArrayBufferFileList({
    blob: new Blob([data.buffer], {type: data.type}),
    size: data.buffer.byteLength,
    mimeType: data.type,
    name: 'buffer',
  });

  const app = /** @type {?mediaApp.ClientApi} */ (
      document.querySelector('backlight-app'));
  if (app) {
    app.loadFiles(fileList);
  } else {
    window.customLaunchData = {files: fileList};
  }
}

function receiveMessage(/** Event */ e) {
  const event = /** @type{MessageEvent<Object>} */ (e);
  if (event.origin !== 'chrome://media-app') {
    return;
  }

  // Tests messages won't have a buffer (and are not handled by this listener).
  if ('buffer' in event.data) {
    const message =
        /** @type{MessageEvent<mediaApp.MessageEventData>}*/ (event);
    load(message.data);
  }
}

// Attempting to execute chooseFileSystemEntries is guaranteed to result in a
// SecurityError due to the fact that we are running in a unprivileged iframe.
// Note, we can not do window.chooseFileSystemEntries due to the fact that
// closure does not yet know that 'chooseFileSystemEntries' is on the window.
// TODO(crbug/1040328): Remove this when we have a polyfill that allows us to
// talk to the privileged frame.
window['chooseFileSystemEntries'] = null;

window.addEventListener('message', receiveMessage, false);
