/*
 * Copyright (C) 2006, 2007, 2009, 2010 Apple Inc. All rights reserved.
 * Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_FRAME_LOCAL_DOM_WINDOW_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_FRAME_LOCAL_DOM_WINDOW_H_

#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/events/event_target.h"
#include "third_party/blink/renderer/core/events/page_transition_event.h"
#include "third_party/blink/renderer/core/frame/dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/scroll/scrollable_area.h"
#include "third_party/blink/renderer/platform/heap/handle.h"
#include "third_party/blink/renderer/platform/supplementable.h"
#include "third_party/blink/renderer/platform/wtf/assertions.h"
#include "third_party/blink/renderer/platform/wtf/casting.h"
#include "third_party/blink/renderer/platform/wtf/forward.h"

#include <memory>

namespace blink {

class ApplicationCache;
class BarProp;
class CSSStyleDeclaration;
class CustomElementRegistry;
class Document;
class DocumentInit;
class DOMSelection;
class DOMVisualViewport;
class Element;
class ExceptionState;
class External;
class FrameConsole;
class History;
class IdleRequestOptions;
class MediaQueryList;
class MessageEvent;
class Modulator;
class Navigator;
class Screen;
class ScriptPromise;
class ScriptState;
class ScrollToOptions;
class SecurityOrigin;
class SerializedScriptValue;
class SourceLocation;
class StyleMedia;
class TrustedTypePolicyFactory;
class V8FrameRequestCallback;
class V8IdleRequestCallback;
class V8VoidFunction;

enum PageTransitionEventPersistence {
  kPageTransitionEventNotPersisted = 0,
  kPageTransitionEventPersisted = 1
};

// Note: if you're thinking of returning something DOM-related by reference,
// please ping dcheng@chromium.org first. You probably don't want to do that.
class CORE_EXPORT LocalDOMWindow final : public DOMWindow,
                                         public Supplementable<LocalDOMWindow> {
  USING_GARBAGE_COLLECTED_MIXIN(LocalDOMWindow);
  USING_PRE_FINALIZER(LocalDOMWindow, Dispose);

 public:
  class CORE_EXPORT EventListenerObserver : public GarbageCollectedMixin {
   public:
    virtual void DidAddEventListener(LocalDOMWindow*, const AtomicString&) = 0;
    virtual void DidRemoveEventListener(LocalDOMWindow*,
                                        const AtomicString&) = 0;
    virtual void DidRemoveAllEventListeners(LocalDOMWindow*) = 0;
  };

  static Document* CreateDocument(const DocumentInit&, bool force_xhtml);

  static LocalDOMWindow* From(const ScriptState*);

  explicit LocalDOMWindow(LocalFrame&);
  ~LocalDOMWindow() override;

  LocalFrame* GetFrame() const { return To<LocalFrame>(DOMWindow::GetFrame()); }

  void Trace(blink::Visitor*) override;

  Document* InstallNewDocument(const DocumentInit&, bool force_xhtml);

  // EventTarget overrides:
  ExecutionContext* GetExecutionContext() const override;
  const LocalDOMWindow* ToLocalDOMWindow() const override;
  LocalDOMWindow* ToLocalDOMWindow() override;

  // Same-origin DOM Level 0
  Screen* screen() const;
  History* history() const;
  BarProp* locationbar() const;
  BarProp* menubar() const;
  BarProp* personalbar() const;
  BarProp* scrollbars() const;
  BarProp* statusbar() const;
  BarProp* toolbar() const;
  Navigator* navigator() const;
  Navigator* clientInformation() const { return navigator(); }

  bool offscreenBuffering() const;

  int outerHeight() const;
  int outerWidth() const;
  int innerHeight() const;
  int innerWidth() const;
  int screenX() const;
  int screenY() const;
  int screenLeft() const { return screenX(); }
  int screenTop() const { return screenY(); }
  double scrollX() const;
  double scrollY() const;
  double pageXOffset() const { return scrollX(); }
  double pageYOffset() const { return scrollY(); }

  DOMVisualViewport* visualViewport();

  const AtomicString& name() const;
  void setName(const AtomicString&);

  String status() const;
  void setStatus(const String&);
  String defaultStatus() const;
  void setDefaultStatus(const String&);
  String origin() const;

  // DOM Level 2 AbstractView Interface
  Document* document() const;

  // CSSOM View Module
  StyleMedia* styleMedia() const;

  // WebKit extensions
  double devicePixelRatio() const;

  ApplicationCache* applicationCache() const;

  // This is the interface orientation in degrees. Some examples are:
  //  0 is straight up; -90 is when the device is rotated 90 clockwise;
  //  90 is when rotated counter clockwise.
  int orientation() const;

  DOMSelection* getSelection();

  void blur() override;
  void print(ScriptState*);
  void stop();

  void alert(ScriptState*, const String& message = String());
  bool confirm(ScriptState*, const String& message);
  String prompt(ScriptState*,
                const String& message,
                const String& default_value);

  bool find(const String&,
            bool case_sensitive,
            bool backwards,
            bool wrap,
            bool whole_word,
            bool search_in_frames,
            bool show_dialog) const;

  // FIXME: ScrollBehaviorSmooth is currently unsupported in VisualViewport.
  // crbug.com/434497
  void scrollBy(double x, double y) const;
  void scrollBy(const ScrollToOptions*) const;
  void scrollTo(double x, double y) const;
  void scrollTo(const ScrollToOptions*) const;
  void scroll(double x, double y) const { scrollTo(x, y); }
  void scroll(const ScrollToOptions* scroll_to_options) const {
    scrollTo(scroll_to_options);
  }
  void moveBy(int x, int y) const;
  void moveTo(int x, int y) const;

  void resizeBy(int x, int y) const;
  void resizeTo(int width, int height) const;

  MediaQueryList* matchMedia(const String&);

  // DOM Level 2 Style Interface
  CSSStyleDeclaration* getComputedStyle(
      Element*,
      const String& pseudo_elt = String()) const;

  // Acessibility Object Model
  ScriptPromise getComputedAccessibleNode(ScriptState*, Element*);

  // WebKit animation extensions
  int requestAnimationFrame(V8FrameRequestCallback*);
  int webkitRequestAnimationFrame(V8FrameRequestCallback*);
  void cancelAnimationFrame(int id);
  int requestPostAnimationFrame(V8FrameRequestCallback*);
  void cancelPostAnimationFrame(int id);

  // https://html.spec.whatwg.org/C/#windoworworkerglobalscope-mixin
  void queueMicrotask(V8VoidFunction*);

  // Idle callback extensions
  int requestIdleCallback(V8IdleRequestCallback*, const IdleRequestOptions*);
  void cancelIdleCallback(int id);

  // Custom elements
  CustomElementRegistry* customElements(ScriptState*) const;
  CustomElementRegistry* customElements() const;
  CustomElementRegistry* MaybeCustomElements() const;

  void SetModulator(Modulator*);

  // Obsolete APIs
  void captureEvents() {}
  void releaseEvents() {}
  External* external();

  bool isSecureContext() const;

  DEFINE_ATTRIBUTE_EVENT_LISTENER(search, kSearch)

  DEFINE_ATTRIBUTE_EVENT_LISTENER(orientationchange, kOrientationchange)

  void RegisterEventListenerObserver(EventListenerObserver*);

  void FrameDestroyed();
  void Reset();

  Element* frameElement() const;

  DOMWindow* open(v8::Isolate*,
                  const String& url_string,
                  const AtomicString& target,
                  const String& features,
                  ExceptionState&);

  FrameConsole* GetFrameConsole() const;

  void PrintErrorMessage(const String&) const;

  void DispatchPostMessage(
      MessageEvent* event,
      scoped_refptr<const SecurityOrigin> intended_target_origin,
      std::unique_ptr<SourceLocation> location,
      const base::UnguessableToken& source_agent_cluster_id);

  void DispatchMessageEventWithOriginCheck(
      const SecurityOrigin* intended_target_origin,
      MessageEvent*,
      std::unique_ptr<SourceLocation>,
      const base::UnguessableToken& source_agent_cluster_id);

  // Events
  // EventTarget API
  void RemoveAllEventListeners() override;

  using EventTarget::DispatchEvent;
  DispatchEventResult DispatchEvent(Event&, EventTarget*);

  void FinishedLoading(FrameLoader::NavigationFinishState);

  // Dispatch the (deprecated) orientationchange event to this DOMWindow and
  // recurse on its child frames.
  void SendOrientationChangeEvent();

  void EnqueueWindowEvent(Event&, TaskType);
  void EnqueueDocumentEvent(Event&, TaskType);
  void EnqueueNonPersistedPageshowEvent();
  void EnqueueHashchangeEvent(const String& old_url, const String& new_url);
  void EnqueuePopstateEvent(scoped_refptr<SerializedScriptValue>);
  void DispatchWindowLoadEvent();
  void DocumentWasClosed();
  void StatePopped(scoped_refptr<SerializedScriptValue>);

  void AcceptLanguagesChanged();

  TrustedTypePolicyFactory* trustedTypes() const;

  void DispatchPersistedPageshowEvent(base::TimeTicks navigation_start);

  void DispatchPagehideEvent(PageTransitionEventPersistence persistence) {
    DispatchEvent(
        *PageTransitionEvent::Create(event_type_names::kPagehide, persistence),
        document_.Get());
  }

 protected:
  // EventTarget overrides.
  void AddedEventListener(const AtomicString& event_type,
                          RegisteredEventListener&) override;
  void RemovedEventListener(const AtomicString& event_type,
                            const RegisteredEventListener&) override;

  // Protected DOMWindow overrides.
  void SchedulePostMessage(MessageEvent*,
                           scoped_refptr<const SecurityOrigin> target,
                           Document* source) override;

 private:
  // Intentionally private to prevent redundant checks when the type is
  // already LocalDOMWindow.
  bool IsLocalDOMWindow() const override { return true; }
  bool IsRemoteDOMWindow() const override { return false; }
  void WarnUnusedPreloads(TimerBase*);

  void Dispose();

  void DispatchLoadEvent();
  void ClearDocument();

  // Return the viewport size including scrollbars.
  IntSize GetViewportSize() const;

  Member<Document> document_;
  Member<DOMVisualViewport> visualViewport_;
  TaskRunnerTimer<LocalDOMWindow> unused_preloads_timer_;

  bool should_print_when_finished_loading_;
  bool has_load_event_fired_ = false;

  mutable Member<Screen> screen_;
  mutable Member<History> history_;
  mutable Member<BarProp> locationbar_;
  mutable Member<BarProp> menubar_;
  mutable Member<BarProp> personalbar_;
  mutable Member<BarProp> scrollbars_;
  mutable Member<BarProp> statusbar_;
  mutable Member<BarProp> toolbar_;
  mutable Member<Navigator> navigator_;
  mutable Member<StyleMedia> media_;
  mutable Member<CustomElementRegistry> custom_elements_;
  // We store reference to Modulator here to have it TraceWrapper-ed.
  // This is wrong, as Modulator is per-context, where as LocalDOMWindow is
  // shared among context. However, this *works* as Modulator is currently only
  // enabled in the main world,
  Member<Modulator> modulator_;
  Member<External> external_;

  String status_;
  String default_status_;

  mutable Member<ApplicationCache> application_cache_;

  scoped_refptr<SerializedScriptValue> pending_state_object_;

  HeapHashSet<WeakMember<EventListenerObserver>> event_listener_observers_;

  mutable Member<TrustedTypePolicyFactory> trusted_types_;
};

template <>
struct DowncastTraits<LocalDOMWindow> {
  static bool AllowFrom(const DOMWindow& window) {
    return window.IsLocalDOMWindow();
  }
};

inline String LocalDOMWindow::status() const {
  return status_;
}

inline String LocalDOMWindow::defaultStatus() const {
  return default_status_;
}

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_FRAME_LOCAL_DOM_WINDOW_H_
