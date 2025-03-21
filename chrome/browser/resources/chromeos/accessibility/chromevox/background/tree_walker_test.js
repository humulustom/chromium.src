// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Include test fixture.
GEN_INCLUDE([
  '//chrome/browser/resources/chromeos/accessibility/chromevox/testing/chromevox_next_e2e_test_base.js'
]);

GEN_INCLUDE([
  '//chrome/browser/resources/chromeos/accessibility/chromevox/testing/snippets.js'
]);

/**
 * Test fixture for tree_walker.js.
 */
ChromeVoxAutomationTreeWalkerTest = class extends ChromeVoxNextE2ETest {
  /** @override */
  testGenCppIncludes() {
    ChromeVoxE2ETest.prototype.testGenCppIncludes.call(this);

    // See https://crbug.com/981953 for details.
    GEN(`
  #if !defined(NDEBUG)
  #define MAYBE_Forward DISABLED_Forward
  #define MAYBE_Backward DISABLED_Backward
  #else
  #define MAYBE_Forward Forward
  #define MAYBE_Backward Backward
  #endif
      `);
  }

  flattenTree(node, outResult) {
    outResult.push(node);
    node = node.firstChild;
    while (node) {
      // Ensure proper parent/child links.
      assertTrue(node.parent.children.some(function(c) {
        return node == c;
      }));
      this.flattenTree(node, outResult);
      node = node.nextSibling;
    }
  }

  isAncestor(ancestor, node) {
    while (node = node.parent) {
      if (node === ancestor) {
        return true;
      }
    }
    return false;
  }

  isDescendant(descendant, node) {
    return this.isAncestor(node, descendant);
  }
};


TEST_F('ChromeVoxAutomationTreeWalkerTest', 'MAYBE_Forward', function() {
  chrome.automation.getDesktop(this.newCallback(function(d) {
    const resultList = [];
    this.flattenTree(d, resultList);
    let it = new AutomationTreeWalker(d, 'forward');
    for (let i = 1; i < resultList.length; i++) {
      assertEquals(resultList[i], it.next().node);
    }
    assertEquals(null, it.next().node);

    for (let j = 0; j < resultList.length; j++) {
      it = new AutomationTreeWalker(resultList[j], 'forward');
      const start = it.node;
      let cur = it.next().node;
      while (cur) {
        const isDescendant = this.isDescendant(cur, start);
        if (it.phase == 'descendant') {
          assertTrue(isDescendant);
        } else if (it.phase == 'other') {
          assertFalse(isDescendant);
        } else {
          assertNotReached();
        }
        cur = it.next().node;
      }
    }
  }.bind(this)));
});

TEST_F('ChromeVoxAutomationTreeWalkerTest', 'MAYBE_Backward', function() {
  chrome.automation.getDesktop(this.newCallback(function(d) {
    const resultList = [];
    this.flattenTree(d, resultList);
    let it =
        new AutomationTreeWalker(resultList[resultList.length - 1], 'backward');
    for (let i = resultList.length - 2; i >= 0; i--) {
      assertEquals(resultList[i], it.next().node);
    }

    for (let j = resultList.length - 1; j >= 0; j--) {
      it = new AutomationTreeWalker(resultList[j], 'backward');
      const start = it.node;
      let cur = it.next().node;
      while (cur) {
        const isAncestor = this.isAncestor(cur, start);
        if (it.phase == 'ancestor') {
          assertTrue(isAncestor);
        } else if (it.phase == 'other') {
          assertFalse(isAncestor);
        } else {
          assertNotReached();
        }
        cur = it.next().node;
      }
    }
  }.bind(this)));
});

TEST_F('ChromeVoxAutomationTreeWalkerTest', 'RootLeafRestriction', function() {
  this.runWithLoadedTree(
      `
      <div role="group" aria-label="1">
        <div role="group" aria-label="2">
          <div role="group" aria-label="3">
            <div role="group" aria-label="4"></div>
          </div>
          <div role="group" aria-label="5"></div>
        </div>
        <div role="group" aria-label="6"></div>
      </div>
    `,
      function(r) {
        const node2 = r.firstChild.firstChild;
        assertEquals('2', node2.name);

        // Restrict to 2's subtree and consider 3 and 5 leaves.
        const leafP = function(n) {
          return n.name == '3' || n.name == '5';
        };
        const rootP = function(n) {
          return n.name == '2';
        };

        // Track the nodes we've visited.
        let visited = '';
        const visit = function(n) {
          visited += n.name;
        };
        const restrictions = {leaf: leafP, root: rootP, visit};
        let walker = new AutomationTreeWalker(node2, 'forward', restrictions);
        while (walker.next().node) {
        }
        assertEquals('35', visited);
        assertEquals(AutomationTreeWalkerPhase.OTHER, walker.phase);

        // And the reverse.
        // Note that walking into a root is allowed.
        visited = '';
        const node6 = r.lastChild.lastChild;
        assertEquals('6', node6.name);
        walker = new AutomationTreeWalker(node6, 'backward', restrictions);
        while (walker.next().node) {
        }
        assertEquals('532', visited);

        // Test not visiting ancestors of initial node.
        const node5 = r.firstChild.firstChild.lastChild;
        assertEquals('5', node5.name);
        restrictions.root = function(n) {
          return n.name == '1';
        };
        restrictions.leaf = function(n) {
          return !n.firstChild;
        };

        visited = '';
        restrictions.skipInitialAncestry = false;
        walker = new AutomationTreeWalker(node5, 'backward', restrictions);
        while (walker.next().node) {
        }
        assertEquals('4321', visited);

        // 2 and 1 are ancestors; check they get skipped.
        visited = '';
        restrictions.skipInitialAncestry = true;
        walker = new AutomationTreeWalker(node5, 'backward', restrictions);
        while (walker.next().node) {
        }
        assertEquals('43', visited);

        // We should skip node 2's subtree.
        walker = new AutomationTreeWalker(
            node2, 'forward', {skipInitialSubtree: true});
        assertEquals(node6, walker.next().node);
      });
});

TEST_F(
    'ChromeVoxAutomationTreeWalkerTest', 'LeafPredicateSymmetry', function() {
      this.runWithLoadedTree(toolbarDoc, function(r) {
        const d = r.root.parent.root;
        const forwardWalker = new AutomationTreeWalker(d, 'forward');
        const forwardNodes = [];

        // Get all nodes according to the walker in the forward direction.
        do {
          forwardNodes.push(forwardWalker.node);
        } while (forwardWalker.next().node);

        // Now, verify the walker moving backwards matches the forwards list.
        const backwardWalker = new AutomationTreeWalker(
            forwardNodes[forwardNodes.length - 1], 'backward');

        do {
          const next = forwardNodes.pop();
          assertEquals(next, backwardWalker.node);
        } while (backwardWalker.next().node);
      });
    });

TEST_F('ChromeVoxAutomationTreeWalkerTest', 'RootPredicateEnding', function() {
  this.runWithLoadedTree(toolbarDoc, function(r) {
    const backwardWalker = new AutomationTreeWalker(r.firstChild, 'backward', {
      root(node) {
        return node === r;
      }
    });
    assertEquals(r, backwardWalker.next().node);
    assertEquals(null, backwardWalker.next().node);

    const forwardWalker =
        new AutomationTreeWalker(r.firstChild.lastChild, 'forward', {
          root(node) {
            return node === r;
          }
        });
    // Advance to the static text box of button contains text "Forward".
    assertEquals('Forward', forwardWalker.next().node.name);
    // Advance to the inline text box of button contains text "Forward".
    assertEquals('Forward', forwardWalker.next().node.name);
    assertEquals(null, forwardWalker.next().node);
  });
});
