// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/webgpu/gpu_adapter.h"

#include "third_party/blink/renderer/bindings/core/v8/script_promise_resolver.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_object_builder.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_gpu_device_descriptor.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_gpu_extensions.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_gpu_request_adapter_options.h"
#include "third_party/blink/renderer/core/dom/dom_exception.h"
#include "third_party/blink/renderer/modules/webgpu/gpu_device.h"
#include "third_party/blink/renderer/platform/heap/heap.h"

namespace blink {

namespace {
WGPUDeviceProperties AsDawnType(const GPUDeviceDescriptor* descriptor) {
  DCHECK_NE(nullptr, descriptor);

  WGPUDeviceProperties requested_device_properties = {};
  requested_device_properties.textureCompressionBC =
      descriptor->extensions()->textureCompressionBC();

  return requested_device_properties;
}
}  // anonymous namespace

GPUAdapter::GPUAdapter(
    const String& name,
    uint32_t adapter_service_id,
    const WGPUDeviceProperties& properties,
    scoped_refptr<DawnControlClientHolder> dawn_control_client)
    : DawnObjectBase(dawn_control_client),
      name_(name),
      adapter_service_id_(adapter_service_id),
      adapter_properties_(properties) {}

const String& GPUAdapter::name() const {
  return name_;
}

ScriptValue GPUAdapter::extensions(ScriptState* script_state) const {
  V8ObjectBuilder object_builder(script_state);
  object_builder.AddBoolean("textureCompressionBC",
                            adapter_properties_.textureCompressionBC);
  return object_builder.GetScriptValue();
}

void GPUAdapter::OnRequestDeviceCallback(ScriptPromiseResolver* resolver,
                                         const GPUDeviceDescriptor* descriptor,
                                         bool is_request_device_success) {
  if (is_request_device_success) {
    ExecutionContext* execution_context = resolver->GetExecutionContext();
    auto* device = MakeGarbageCollected<GPUDevice>(
        execution_context, GetDawnControlClient(), this, descriptor);
    resolver->Resolve(device);
  } else {
    resolver->Reject(MakeGarbageCollected<DOMException>(
        DOMExceptionCode::kOperationError,
        "Fail to request GPUDevice with the given GPUDeviceDescriptor"));
  }
}

ScriptPromise GPUAdapter::requestDevice(ScriptState* script_state,
                                        const GPUDeviceDescriptor* descriptor) {
  auto* resolver = MakeGarbageCollected<ScriptPromiseResolver>(script_state);
  ScriptPromise promise = resolver->Promise();

  WGPUDeviceProperties requested_device_properties = AsDawnType(descriptor);

  if (!GetInterface()->RequestDeviceAsync(
          adapter_service_id_, &requested_device_properties,
          WTF::Bind(&GPUAdapter::OnRequestDeviceCallback, WrapPersistent(this),
                    WrapPersistent(resolver), WrapPersistent(descriptor)))) {
    resolver->Reject(MakeGarbageCollected<DOMException>(
        DOMExceptionCode::kOperationError, "Unknown error creating GPUDevice"));
  }

  return promise;
}

}  // namespace blink
