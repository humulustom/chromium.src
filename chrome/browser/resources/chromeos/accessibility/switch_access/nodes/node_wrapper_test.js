// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

GEN_INCLUDE(['../switch_access_e2e_test_base.js']);

/**
 * @constructor
 * @extends {SwitchAccessE2ETest}
 */
function SwitchAccessNodeWrapperTest() {
  SwitchAccessE2ETest.call(this);
}

SwitchAccessNodeWrapperTest.prototype = {
  __proto__: SwitchAccessE2ETest.prototype,
};

TEST_F('SwitchAccessNodeWrapperTest', 'BuildDesktopTree', function() {
  this.runWithLoadedTree('', (desktop) => {
    const desktopRootNode = RootNodeWrapper.buildDesktopTree(desktop);

    const children = desktopRootNode.children;
    for (let i = 0; i < children.length; i++) {
      const child = children[i];
      // The desktop tree should not include a back button.
      assertFalse(child instanceof BackButtonNode);

      // Check that the children form a loop.
      const next = children[(i + 1) % children.length];
      assertEquals(
          next, child.next, 'next not properly initialized on child ' + i);
      // We add children.length to ensure the value is greater than zero.
      const previous = children[(i - 1 + children.length) % children.length];
      assertEquals(
          previous, child.previous,
          'previous not properly initialized on child ' + i);
    }
  });
});

TEST_F('SwitchAccessNodeWrapperTest', 'AsRootNode', function() {
  const website = `<div aria-label="outer">
                     <div aria-label="inner">
                       <input type="range">
                       <button></button>
                     </div>
                     <button></button>
                   </div>`;
  this.runWithLoadedTree(website, (desktop) => {
    const slider = desktop.find({role: chrome.automation.RoleType.SLIDER});
    const inner = slider.parent;
    assertNotEquals(undefined, inner, 'Could not find inner group');
    const outer = inner.parent;
    assertNotEquals(undefined, outer, 'Could not find outer group');

    const outerRootNode = RootNodeWrapper.buildTree(outer, null);
    const innerNode = outerRootNode.firstChild;
    assertTrue(innerNode.isGroup(), 'Inner group node is not a group');

    const innerRootNode = innerNode.asRootNode();
    assertEquals(3, innerRootNode.children.length, 'Expected 3 children');
    const sliderNode = innerRootNode.firstChild;
    assertEquals(
        chrome.automation.RoleType.SLIDER, sliderNode.role,
        'First child should be a slider');
    assertEquals(
        chrome.automation.RoleType.BUTTON, sliderNode.next.role,
        'Second child should be a button');
    assertTrue(
        innerRootNode.lastChild instanceof BackButtonNode,
        'Final child should be the back button');
  });
});

TEST_F('SwitchAccessNodeWrapperTest', 'Equals', function() {
  this.runWithLoadedTree('', (desktop) => {
    const desktopRootNode = RootNodeWrapper.buildDesktopTree(desktop);

    let childGroup = desktopRootNode.firstChild;
    let i = 0;
    while (!childGroup.isGroup() && i < desktopRootNode.children.length) {
      childGroup = childGroup.next;
      i++;
    }
    childGroup = childGroup.asRootNode();

    assertFalse(desktopRootNode.equals(), 'Root node equals nothing');
    assertFalse(
        desktopRootNode.equals(new SARootNode()),
        'Different type root nodes are equal');
    assertFalse(
        new SARootNode().equals(desktopRootNode),
        'Equals is not symmetric? Different types of root are equal');
    assertFalse(
        desktopRootNode.equals(childGroup),
        'Groups with different children are equal');
    assertFalse(
        childGroup.equals(desktopRootNode),
        'Equals is not symmetric? Groups with different children are equal');

    assertTrue(
        desktopRootNode.equals(desktopRootNode),
        'Equals is not reflexive? (root node)');
    const desktopCopy = RootNodeWrapper.buildDesktopTree(desktop);
    assertTrue(
        desktopRootNode.equals(desktopCopy), 'Two desktop roots are not equal');
    assertTrue(
        desktopCopy.equals(desktopRootNode),
        'Equals is not symmetric? Two desktop roots aren\'t equal');

    const wrappedNode = desktopRootNode.firstChild;
    assertTrue(
        wrappedNode instanceof NodeWrapper,
        'Child node is not of type NodeWrapper');
    assertGT(
        desktopRootNode.children.length, 1, 'Desktop root has only 1 child');

    assertFalse(wrappedNode.equals(), 'Child NodeWrapper equals nothing');
    assertFalse(
        wrappedNode.equals(new BackButtonNode()),
        'Child NodeWrapper equals a BackButtonNode');
    assertFalse(
        new BackButtonNode().equals(wrappedNode),
        'Equals is not symmetric? NodeWrapper equals a BackButtonNode');
    assertFalse(
        wrappedNode.equals(desktopRootNode.lastChild),
        'Children with different base nodes are equal');
    assertFalse(
        desktopRootNode.lastChild.equals(wrappedNode),
        'Equals is not symmetric? Nodes with different base nodes are equal');

    const equivalentWrappedNode =
        new NodeWrapper(wrappedNode.baseNode_, desktopRootNode);
    assertTrue(
        wrappedNode.equals(wrappedNode),
        'Equals is not reflexive? (child node)');
    assertTrue(
        wrappedNode.equals(equivalentWrappedNode),
        'Two nodes with the same base node are not equal');
    assertTrue(
        equivalentWrappedNode.equals(wrappedNode),
        'Equals is not symmetric? Nodes with the same base node aren\'t equal');
  });
});

TEST_F('SwitchAccessNodeWrapperTest', 'Actions', function() {
  const website = `<input type="text">
                   <button></button>
                   <input type="range" min=1 max=5 value=3>`;
  this.runWithLoadedTree(website, (desktop) => {
    const textField = new NodeWrapper(
        desktop.find({role: chrome.automation.RoleType.TEXT_FIELD}),
        new SARootNode());

    assertEquals(
        chrome.automation.RoleType.TEXT_FIELD, textField.role,
        'Text field node is not a text field');
    assertTrue(
        textField.hasAction(SAConstants.MenuAction.OPEN_KEYBOARD),
        'Text field does not have action OPEN_KEYBOARD');
    assertTrue(
        textField.hasAction(SAConstants.MenuAction.DICTATION),
        'Text field does not have action DICTATION');
    assertFalse(
        textField.hasAction(SAConstants.MenuAction.SELECT),
        'Text field has action SELECT');

    const button = new NodeWrapper(
        desktop.find({role: chrome.automation.RoleType.BUTTON}),
        new SARootNode());

    assertEquals(
        chrome.automation.RoleType.BUTTON, button.role,
        'Button node is not a button');
    assertTrue(
        button.hasAction(SAConstants.MenuAction.SELECT),
        'Button does not have action SELECT');
    assertFalse(
        button.hasAction(SAConstants.MenuAction.OPEN_KEYBOARD),
        'Button has action OPEN_KEYBOARD');
    assertFalse(
        button.hasAction(SAConstants.MenuAction.DICTATION),
        'Button has action DICTATION');

    const slider = new NodeWrapper(
        desktop.find({role: chrome.automation.RoleType.SLIDER}),
        new SARootNode());

    assertEquals(
        chrome.automation.RoleType.SLIDER, slider.role,
        'Slider node is not a slider');
    assertTrue(
        slider.hasAction(SAConstants.MenuAction.INCREMENT),
        'Slider does not have action INCREMENT');
    assertTrue(
        slider.hasAction(SAConstants.MenuAction.DECREMENT),
        'Slider does not have action DECREMENT');
  });
});
