#include "third_party/blink/renderer/bindings/core/v8/v8_file_list.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_file.h"
#include "third_party/blink/renderer/platform/bindings/v8_binding.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"

namespace blink {
void V8FileList::ConstructorCustom(const v8::FunctionCallbackInfo<v8::Value>& args)
{
  ExecutionContext* context = CurrentExecutionContext(args.GetIsolate());
  if (context && context->IsDocument()) {
    Document* document = static_cast<Document*>(context);
    if (document->GetFrame()->isNwDisabledChildFrame()) {
      V8ThrowException::ThrowTypeError(args.GetIsolate(), "FileList constructor cannot be called in nwdisabled frame.");
      return;
    }
  }

  FileList* impl = MakeGarbageCollected<FileList>();
  V8SetReturnValue(args, impl);
}

} // namespace blink
