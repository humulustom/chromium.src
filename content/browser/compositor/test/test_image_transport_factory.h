// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_COMPOSITOR_TEST_TEST_IMAGE_TRANSPORT_FACTORY_H_
#define CONTENT_BROWSER_COMPOSITOR_TEST_TEST_IMAGE_TRANSPORT_FACTORY_H_

#include <memory>

#include "base/macros.h"
#include "build/build_config.h"
#include "cc/test/fake_layer_tree_frame_sink.h"
#include "cc/test/test_task_graph_runner.h"
#include "components/viz/common/display/renderer_settings.h"
#include "components/viz/common/surfaces/frame_sink_id_allocator.h"
#include "components/viz/host/host_frame_sink_manager.h"
#include "components/viz/test/test_frame_sink_manager.h"
#include "components/viz/test/test_gpu_memory_buffer_manager.h"
#include "components/viz/test/test_image_factory.h"
#include "content/browser/compositor/image_transport_factory.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "services/viz/privileged/mojom/compositing/vsync_parameter_observer.mojom.h"
#include "ui/compositor/compositor.h"

namespace content {

// Test implementation of ImageTransportFactory, ContextFactory and
// ContextFactoryPrivate. This class tries to do very little, mostly setting up
// HostFrameSinkManager and returning fake implementations where possible.
class TestImageTransportFactory : public ui::ContextFactory,
                                  public ui::ContextFactoryPrivate,
                                  public ImageTransportFactory {
 public:
  TestImageTransportFactory();
  ~TestImageTransportFactory() override;

  // ui::ContextFactory implementation.
  void CreateLayerTreeFrameSink(
      base::WeakPtr<ui::Compositor> compositor) override;
  scoped_refptr<viz::ContextProvider> SharedMainThreadContextProvider()
      override;
  scoped_refptr<viz::RasterContextProvider>
  SharedMainThreadRasterContextProvider() override;
  void RemoveCompositor(ui::Compositor* compositor) override {}
  gpu::GpuMemoryBufferManager* GetGpuMemoryBufferManager() override;
  cc::TaskGraphRunner* GetTaskGraphRunner() override;

  // ui::ContextFactoryPrivate implementation.
  viz::FrameSinkId AllocateFrameSinkId() override;
  viz::HostFrameSinkManager* GetHostFrameSinkManager() override;
  void SetDisplayVisible(ui::Compositor* compositor, bool visible) override {}
  void ResizeDisplay(ui::Compositor* compositor,
                     const gfx::Size& size) override {}
  void DisableSwapUntilResize(ui::Compositor* compositor) override {}
  void SetDisplayColorMatrix(ui::Compositor* compositor,
                             const SkMatrix44& matrix) override {}
  void SetDisplayColorSpaces(
      ui::Compositor* compositor,
      const gfx::DisplayColorSpaces& display_color_spaces) override {}
  void SetDisplayVSyncParameters(ui::Compositor* compositor,
                                 base::TimeTicks timebase,
                                 base::TimeDelta interval) override {}
  void IssueExternalBeginFrame(
      ui::Compositor* compositor,
      const viz::BeginFrameArgs& args,
      bool force,
      base::OnceCallback<void(const viz::BeginFrameAck&)> callback) override {}
  void SetOutputIsSecure(ui::Compositor* compositor, bool secure) override {}
  void AddVSyncParameterObserver(
      ui::Compositor* compositor,
      mojo::PendingRemote<viz::mojom::VSyncParameterObserver> observer)
      override {}

  // ImageTransportFactory implementation.
  void DisableGpuCompositing() override;
  ui::ContextFactory* GetContextFactory() override;
  ui::ContextFactoryPrivate* GetContextFactoryPrivate() override;

 private:
  cc::TestTaskGraphRunner task_graph_runner_;
  viz::TestImageFactory image_factory_;
  viz::TestGpuMemoryBufferManager gpu_memory_buffer_manager_;
  viz::RendererSettings renderer_settings_;
  viz::FrameSinkIdAllocator frame_sink_id_allocator_;
  scoped_refptr<viz::ContextProvider> shared_main_context_provider_;
  viz::HostFrameSinkManager host_frame_sink_manager_;
  viz::TestFrameSinkManagerImpl test_frame_sink_manager_impl_;

  DISALLOW_COPY_AND_ASSIGN(TestImageTransportFactory);
};

}  // namespace content

#endif  // CONTENT_BROWSER_COMPOSITOR_TEST_TEST_IMAGE_TRANSPORT_FACTORY_H_
