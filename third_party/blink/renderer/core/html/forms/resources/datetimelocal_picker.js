'use strict';
// Copyright (C) 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * @fileoverview Picker used by <input type='datetime-local' />
 */

function initializeDateTimeLocalPicker(config) {
  const dateTimeLocalPicker = new DateTimeLocalPicker(config);
  global.picker = dateTimeLocalPicker;
  main.append(dateTimeLocalPicker);
  main.style.border = '1px solid transparent';
  main.style.height = dateTimeLocalPicker.height + 'px';
  main.style.width = dateTimeLocalPicker.width + 'px';
  resizeWindow(dateTimeLocalPicker.width + 2, dateTimeLocalPicker.height + 2);
}

/**
 * DateTimeLocalPicker: Custom element providing a datetime-local picker implementation.
 *             DateTimeLocalPicker contains 2 parts:
 *                 - date picker
 *                 - time picker
 */
class DateTimeLocalPicker extends HTMLElement {
  constructor(config) {
    super();

    this.className = DateTimeLocalPicker.ClassName;

    this.datePicker_ = new CalendarPicker(config.mode, config);
    this.timePicker_ = new TimePicker(config);
    this.append(this.datePicker_.element, this.timePicker_);

    this.hadValidValueWhenOpened_ =
        (config.currentValue !== '') && (this.datePicker_.selection() != null);
    this.initialSelectedValue_ = this.selectedValue;

    this.addEventListener('keydown', this.onKeyDown_);
    this.addEventListener('click', this.onClick_);

    window.addEventListener('resize', this.onWindowResize_, {once: true});
  };

  onKeyDown_ = (event) => {
    switch (event.key) {
      case 'Enter':
        // Submit the popup for an Enter keypress except when the user is
        // hitting Enter to activate the month switcher button, Today button,
        // or previous/next month arrows.
        if (!event.target.matches(
                '.calendar-navigation-button, .month-popup-button, .year-list-view')) {
          window.pagePopupController.setValueAndClosePopup(
              0, this.selectedValue);
        } else if (event.target.matches(
                       '.calendar-navigation-button, .year-list-view')) {
          // Navigating with the previous/next arrows may change selection,
          // so push this change to the in-page control but don't
          // close the popup.
          window.pagePopupController.setValue(this.selectedValue);
        }
        break;
      case 'Escape':
        if (this.selectedValue === this.initialSelectedValue_) {
          window.pagePopupController.closePopup();
        } else {
          this.datePicker_.resetToInitialValue();
          this.timePicker_.resetToInitialValue();
          window.pagePopupController.setValue(
              this.hadValidValueWhenOpened_ ? this.initialSelectedValue_ : '');
        }
        break;
      case 'ArrowUp':
      case 'ArrowDown':
      case 'ArrowLeft':
      case 'ArrowRight':
      case 'PageUp':
      case 'PageDown':
      case 't':
      case 'm':
      case 'M':
      case 'y':
      case 'Y':
      case 'd':
      case 'D':
        if (event.target.matches('.calendar-table-view, .time-column') &&
            this.hasSelectedDate) {
          window.pagePopupController.setValue(this.selectedValue);
        }
        break;
    }
  };

  onClick_ = (event) => {
    if (event.target.matches(
            '.day-cell, .time-cell, .today-button-refresh, .calendar-navigation-button, .year-list-view, .calendar-navigation-button, .today-button-icon-refresh, .month-button') &&
        this.hasSelectedDate) {
      window.pagePopupController.setValue(this.selectedValue);
    }
  };

  onWindowResize_ = (event) => {
    this.datePicker_.calendarTableView.element.focus();
  };

  // This will be false if neither the initial value of the
  // control nor today's date are within a valid date range defined
  // by the 'step', 'min', and 'max' attributes of the control.
  get hasSelectedDate() {
    return (this.datePicker_.selection() != null);
  }

  get selectedValue() {
    return this.hasSelectedDate ? (this.datePicker_.getSelectedValue() + 'T' +
                                   this.timePicker_.selectedValue) :
                                  '';
  }

  get height() {
    return DateTimeLocalPicker.Height;
  }

  get width() {
    return this.datePicker_.width() + this.timePicker_.width;
  }

  get datePicker() {
    return this.datePicker_;
  }

  get timePicker() {
    return this.timePicker_;
  }
}
DateTimeLocalPicker.ClassName = 'datetimelocal-picker';
DateTimeLocalPicker.Height = 280;
window.customElements.define('datetimelocal-picker', DateTimeLocalPicker);
