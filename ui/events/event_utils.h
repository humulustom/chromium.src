// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_EVENTS_EVENT_UTILS_H_
#define UI_EVENTS_EVENT_UTILS_H_

#include <stdint.h>

#include <memory>

#include "base/strings/string16.h"
#include "build/build_config.h"
#include "ui/display/display.h"
#include "ui/events/base_event_utils.h"
#include "ui/events/event.h"
#include "ui/events/event_constants.h"
#include "ui/events/events_export.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/events/platform_event.h"
#include "ui/gfx/native_widget_types.h"

#if defined(OS_WIN)
#include <windows.h>
#endif

namespace gfx {
class Point;
class Vector2d;
}

namespace base {
class TimeTicks;
}

// Common functions to be used for all platforms except Android.
namespace ui {

class Event;
class MouseEvent;
enum class DomCode;

// Key used to store keyboard 'group' values in Event::Properties
constexpr char kPropertyKeyboardGroup[] = "_keyevent_kbd_group_";

// Key used to store 'hardware key code' values in Event::Properties
constexpr char kPropertyKeyboardHwKeyCode[] = "_keyevent_kbd_hw_keycode_";

// IBus specific Event::Properties constants. ibus-gtk in async mode uses
// gtk-specific XKeyEvent::state bits 24 and 25 for its key events.
// https://mail.gnome.org/archives/gtk-devel-list/2013-June/msg00003.html
constexpr char kPropertyKeyboardIBusFlag[] = "_keyevent_kbd_ibus_ime_flags_";
constexpr unsigned int kPropertyKeyboardIBusFlagOffset = 24;
constexpr unsigned int kPropertyKeyboardIBusFlagMask = 0x03;

// Key used to store mouse event flag telling ET_MOUSE_EXITED must actually be
// interpreted as "crossing intermediate window" in blink context.
constexpr char kPropertyMouseCrossedIntermediateWindow[] =
    "_mouseevent_cros_window_";

// Returns a ui::Event wrapping a native event. Ownership of the returned value
// is transferred to the caller.
EVENTS_EXPORT std::unique_ptr<Event> EventFromNative(
    const PlatformEvent& native_event);

// Get the EventType from a native event.
EVENTS_EXPORT EventType EventTypeFromNative(const PlatformEvent& native_event);

// Get the EventFlags from a native event.
EVENTS_EXPORT int EventFlagsFromNative(const PlatformEvent& native_event);

// Get the timestamp from a native event.
// Note: This is not a pure function meaning that multiple applications on the
// same native event may return different values.
EVENTS_EXPORT base::TimeTicks EventTimeFromNative(
    const PlatformEvent& native_event);

// Ensures that the event timestamp values are coming from the same underlying
// monotonic clock as base::TimeTicks::Now() and if it is not then falls
// back to using the current ticks for event timestamp.
EVENTS_EXPORT void ValidateEventTimeClock(base::TimeTicks* timestamp);

// Get the location from a native event.  The coordinate system of the resultant
// |Point| has the origin at top-left of the "root window".  The nature of
// this "root window" and how it maps to platform-specific drawing surfaces is
// defined in ui/aura/root_window.* and ui/aura/window_tree_host*.
EVENTS_EXPORT gfx::PointF EventLocationFromNative(
    const PlatformEvent& native_event);

// Gets the location in native system coordinate space.
EVENTS_EXPORT gfx::Point EventSystemLocationFromNative(
    const PlatformEvent& native_event);

#if defined(USE_X11)
// Returns the 'real' button for an event. The button reported in slave events
// does not take into account any remapping (e.g. using xmodmap), while the
// button reported in master events do. This is a utility function to always
// return the mapped button.
EVENTS_EXPORT int EventButtonFromNative(const PlatformEvent& native_event);
#endif

// Returns the KeyboardCode from a native event.
EVENTS_EXPORT KeyboardCode
KeyboardCodeFromNative(const PlatformEvent& native_event);

// Returns the DOM KeyboardEvent code (physical location in the
// keyboard) from a native event.
EVENTS_EXPORT DomCode CodeFromNative(const PlatformEvent& native_event);

// Returns true if the keyboard event is a character event rather than
// a keystroke event.
EVENTS_EXPORT bool IsCharFromNative(const PlatformEvent& native_event);

// Returns the flags of the button that changed during a press/release.
EVENTS_EXPORT int GetChangedMouseButtonFlagsFromNative(
    const PlatformEvent& native_event);

// Returns the detailed pointer information for mouse events.
EVENTS_EXPORT PointerDetails
GetMousePointerDetailsFromNative(const PlatformEvent& native_event);

// Gets the mouse wheel offsets from a native event.
EVENTS_EXPORT gfx::Vector2d GetMouseWheelOffset(
    const PlatformEvent& native_event);

// Returns a copy of |native_event|. Depending on the platform, this copy may
// need to be deleted with ReleaseCopiedNativeEvent().
PlatformEvent CopyNativeEvent(const PlatformEvent& native_event);

// Delete a |native_event| previously created by CopyNativeEvent().
void ReleaseCopiedNativeEvent(const PlatformEvent& native_event);

// Returns the detailed pointer information for touch events.
EVENTS_EXPORT PointerDetails
GetTouchPointerDetailsFromNative(const PlatformEvent& native_event);

// Gets the fling velocity from a native event. is_cancel is set to true if
// this was a tap down, intended to stop an ongoing fling.
EVENTS_EXPORT bool GetFlingData(const PlatformEvent& native_event,
                                float* vx,
                                float* vy,
                                float* vx_ordinal,
                                float* vy_ordinal,
                                bool* is_cancel);

// Returns whether this is a scroll event and optionally gets the amount to be
// scrolled. |x_offset|, |y_offset| and |finger_count| can be NULL.
EVENTS_EXPORT bool GetScrollOffsets(const PlatformEvent& native_event,
                                    float* x_offset,
                                    float* y_offset,
                                    float* x_offset_ordinal,
                                    float* y_offset_ordinal,
                                    int* finger_count,
                                    EventMomentumPhase* momentum_phase);

// Returns whether natural scrolling should be used for touchpad.
EVENTS_EXPORT bool ShouldDefaultToNaturalScroll();

// Returns whether or not the internal display produces touch events.
EVENTS_EXPORT display::Display::TouchSupport GetInternalDisplayTouchSupport();

EVENTS_EXPORT void ComputeEventLatencyOS(const PlatformEvent& native_event);

#if defined(OS_WIN)
EVENTS_EXPORT int GetModifiersFromKeyState();

// Returns true if |message| identifies a mouse event that was generated as the
// result of a touch event.
EVENTS_EXPORT bool IsMouseEventFromTouch(UINT message);

// Converts scan code and lParam each other.  The scan code
// representing an extended key contains 0xE000 bits.
EVENTS_EXPORT uint16_t GetScanCodeFromLParam(LPARAM lParam);
EVENTS_EXPORT LPARAM GetLParamFromScanCode(uint16_t scan_code);

// Creates an MSG from the given KeyEvent if there is no native_event.
EVENTS_EXPORT MSG MSGFromKeyEvent(KeyEvent* key_event, HWND hwnd = nullptr);
EVENTS_EXPORT KeyEvent KeyEventFromMSG(const MSG& msg);
EVENTS_EXPORT MouseEvent MouseEventFromMSG(const MSG& msg);
EVENTS_EXPORT MouseWheelEvent MouseWheelEventFromMSG(const MSG& msg);

#endif  // defined(OS_WIN)

// Registers a custom event type.
EVENTS_EXPORT int RegisterCustomEventType();

// Updates the location of |located_event| from |current_window_origin| to be in
// |target_window_origin|'s coordinate system so that it can be dispatched to a
// window based on |target_window_origin|.
EVENTS_EXPORT void ConvertEventLocationToTargetWindowLocation(
    const gfx::Point& target_window_origin,
    const gfx::Point& current_window_origin,
    ui::LocatedEvent* located_event);

// Returns a string description of an event type. Useful for debugging.
EVENTS_EXPORT const char* EventTypeName(EventType type);

}  // namespace ui

#endif  // UI_EVENTS_EVENT_UTILS_H_
