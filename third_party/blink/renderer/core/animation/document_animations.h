/*
 * Copyright (C) 2013 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_ANIMATION_DOCUMENT_ANIMATIONS_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_ANIMATION_DOCUMENT_ANIMATIONS_H_

#include "third_party/blink/renderer/core/animation/animation.h"
#include "third_party/blink/renderer/core/dom/document_lifecycle.h"
#include "third_party/blink/renderer/platform/heap/member.h"

namespace blink {

class AnimationTimeline;
class Document;
class PaintArtifactCompositor;

class CORE_EXPORT DocumentAnimations final
    : public GarbageCollected<DocumentAnimations> {
 public:
  DocumentAnimations(Document*);
  ~DocumentAnimations() = default;

  void AddTimeline(AnimationTimeline&);
  void UpdateAnimationTimingForAnimationFrame();
  bool NeedsAnimationTimingUpdate();
  void UpdateAnimationTimingIfNeeded();

  // Updates existing animations as part of generating a new (document
  // lifecycle) frame. Note that this considers and updates state for
  // both composited and non-composited animations.
  void UpdateAnimations(
      DocumentLifecycle::LifecycleState required_lifecycle_state,
      const PaintArtifactCompositor* paint_artifact_compositor);

  HeapVector<Member<Animation>> getAnimations();
  void Trace(blink::Visitor*);

 private:
  Member<Document> document_;
  HeapHashSet<WeakMember<AnimationTimeline>> timelines_;
};

}  // namespace blink

#endif
