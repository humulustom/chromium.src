/*
 * Copyright (C) 2011 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

#include "third_party/blink/renderer/platform/graphics/canvas_2d_layer_bridge.h"

#include <utility>

#include "base/location.h"
#include "base/memory/scoped_refptr.h"
#include "base/run_loop.h"
#include "base/single_thread_task_runner.h"
#include "base/test/metrics/histogram_tester.h"
#include "build/build_config.h"
#include "cc/layers/texture_layer.h"
#include "cc/test/skia_common.h"
#include "cc/test/stub_decode_cache.h"
#include "components/viz/common/resources/single_release_callback.h"
#include "components/viz/common/resources/transferable_resource.h"
#include "components/viz/test/test_context_provider.h"
#include "components/viz/test/test_gles2_interface.h"
#include "components/viz/test/test_gpu_memory_buffer_manager.h"
#include "gpu/GLES2/gl2extchromium.h"
#include "gpu/command_buffer/client/gles2_interface.h"
#include "gpu/command_buffer/common/capabilities.h"
#include "gpu/command_buffer/common/gpu_memory_buffer_support.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/renderer/platform/graphics/canvas_resource_host.h"
#include "third_party/blink/renderer/platform/graphics/canvas_resource_provider.h"
#include "third_party/blink/renderer/platform/graphics/gpu/shared_gpu_context.h"
#include "third_party/blink/renderer/platform/graphics/paint/paint_flags.h"
#include "third_party/blink/renderer/platform/graphics/static_bitmap_image.h"
#include "third_party/blink/renderer/platform/graphics/test/fake_canvas_resource_host.h"
#include "third_party/blink/renderer/platform/graphics/test/gpu_memory_buffer_test_platform.h"
#include "third_party/blink/renderer/platform/graphics/test/gpu_test_utils.h"
#include "third_party/blink/renderer/platform/graphics/web_graphics_context_3d_provider_wrapper.h"
#include "third_party/blink/renderer/platform/scheduler/public/thread.h"
#include "third_party/blink/renderer/platform/scheduler/public/thread_scheduler.h"
#include "third_party/blink/renderer/platform/testing/runtime_enabled_features_test_helpers.h"
#include "third_party/blink/renderer/platform/wtf/cross_thread_functional.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "third_party/skia/include/gpu/gl/GrGLTypes.h"

#include <memory>

using testing::_;
using testing::AnyNumber;
using testing::AtLeast;
using testing::InSequence;
using testing::Pointee;
using testing::Return;
using testing::SetArgPointee;
using testing::Test;

namespace blink {

namespace {

class ImageTrackingDecodeCache : public cc::StubDecodeCache {
 public:
  ImageTrackingDecodeCache() = default;
  ~ImageTrackingDecodeCache() override { EXPECT_EQ(num_locked_images_, 0); }

  cc::DecodedDrawImage GetDecodedImageForDraw(
      const cc::DrawImage& image) override {
    EXPECT_FALSE(disallow_cache_use_);

    num_locked_images_++;
    decoded_images_.push_back(image);
    SkBitmap bitmap;
    bitmap.allocPixelsFlags(SkImageInfo::MakeN32Premul(10, 10),
                            SkBitmap::kZeroPixels_AllocFlag);
    sk_sp<SkImage> sk_image = SkImage::MakeFromBitmap(bitmap);
    return cc::DecodedDrawImage(sk_image, SkSize::Make(0, 0),
                                SkSize::Make(1, 1), kLow_SkFilterQuality,
                                !budget_exceeded_);
  }

  void set_budget_exceeded(bool exceeded) { budget_exceeded_ = exceeded; }
  void set_disallow_cache_use(bool disallow) { disallow_cache_use_ = disallow; }

  void DrawWithImageFinished(
      const cc::DrawImage& image,
      const cc::DecodedDrawImage& decoded_image) override {
    EXPECT_FALSE(disallow_cache_use_);
    num_locked_images_--;
  }

  const Vector<cc::DrawImage>& decoded_images() const {
    return decoded_images_;
  }
  int num_locked_images() const { return num_locked_images_; }

 private:
  Vector<cc::DrawImage> decoded_images_;
  int num_locked_images_ = 0;
  bool budget_exceeded_ = false;
  bool disallow_cache_use_ = false;
};

}  // anonymous namespace

class Canvas2DLayerBridgeTest : public Test {
 public:
  std::unique_ptr<Canvas2DLayerBridge> MakeBridge(
      const IntSize& size,
      Canvas2DLayerBridge::AccelerationMode acceleration_mode,
      const CanvasColorParams& color_params,
      std::unique_ptr<FakeCanvasResourceHost> custom_host = nullptr) {
    std::unique_ptr<Canvas2DLayerBridge> bridge =
        std::make_unique<Canvas2DLayerBridge>(size, acceleration_mode,
                                              color_params);
    bridge->DontUseIdleSchedulingForTesting();
    if (custom_host)
      host_ = std::move(custom_host);
    if (!host_)
      host_ = std::make_unique<FakeCanvasResourceHost>(size);
    bridge->SetCanvasResourceHost(host_.get());
    return bridge;
  }

  void SetUp() override {
    test_context_provider_ = viz::TestContextProvider::Create();
    InitializeSharedGpuContext(test_context_provider_.get(),
                               &image_decode_cache_);
  }

  virtual bool NeedsMockGL() { return false; }

  void TearDown() override {
    SharedGpuContext::ResetForTesting();
    test_context_provider_.reset();
  }

  FakeCanvasResourceHost* Host() {
    DCHECK(host_);
    return host_.get();
  }

 protected:
  scoped_refptr<viz::TestContextProvider> test_context_provider_;
  ImageTrackingDecodeCache image_decode_cache_;
  std::unique_ptr<FakeCanvasResourceHost> host_;
};

TEST_F(Canvas2DLayerBridgeTest, DisableAcceleration) {
  std::unique_ptr<Canvas2DLayerBridge> bridge =
      MakeBridge(IntSize(300, 150), Canvas2DLayerBridge::kDisableAcceleration,
                 CanvasColorParams());

  GrBackendTexture backend_texture =
      bridge->NewImageSnapshot(kPreferAcceleration)
          ->PaintImageForCurrentFrame()
          .GetSkImage()
          ->getBackendTexture(true);

  EXPECT_FALSE(backend_texture.isValid());
}

TEST_F(Canvas2DLayerBridgeTest, NoDrawOnContextLost) {
  std::unique_ptr<Canvas2DLayerBridge> bridge = MakeBridge(
      IntSize(300, 150), Canvas2DLayerBridge::kForceAccelerationForTesting,
      CanvasColorParams());
  EXPECT_TRUE(bridge->IsValid());
  PaintFlags flags;
  uint32_t gen_id = bridge->GetOrCreateResourceProvider()->ContentUniqueID();
  bridge->DrawingCanvas()->drawRect(SkRect::MakeXYWH(0, 0, 1, 1), flags);
  EXPECT_EQ(gen_id, bridge->GetOrCreateResourceProvider()->ContentUniqueID());
  test_context_provider_->TestContextGL()->set_context_lost(true);
  EXPECT_EQ(nullptr, bridge->GetOrCreateResourceProvider());
  // The following passes by not crashing
  bridge->NewImageSnapshot(kPreferAcceleration);
}

TEST_F(Canvas2DLayerBridgeTest, PrepareMailboxWhenContextIsLost) {
  std::unique_ptr<Canvas2DLayerBridge> bridge = MakeBridge(
      IntSize(300, 150), Canvas2DLayerBridge::kForceAccelerationForTesting,
      CanvasColorParams());

  EXPECT_TRUE(bridge->IsAccelerated());
  bridge->FinalizeFrame();  // Trigger the creation of a backing store
  // When the context is lost we are not sure if we should still be producing
  // GL frames for the compositor or not, so fail to generate frames.
  test_context_provider_->TestContextGL()->set_context_lost(true);

  viz::TransferableResource resource;
  std::unique_ptr<viz::SingleReleaseCallback> release_callback;
  EXPECT_FALSE(bridge->PrepareTransferableResource(nullptr, &resource,
                                                   &release_callback));
}

TEST_F(Canvas2DLayerBridgeTest,
       PrepareMailboxWhenContextIsLostWithFailedRestore) {
  std::unique_ptr<Canvas2DLayerBridge> bridge = MakeBridge(
      IntSize(300, 150), Canvas2DLayerBridge::kForceAccelerationForTesting,
      CanvasColorParams());

  bridge->GetOrCreateResourceProvider();
  EXPECT_TRUE(bridge->IsValid());
  // When the context is lost we are not sure if we should still be producing
  // GL frames for the compositor or not, so fail to generate frames.
  test_context_provider_->TestContextGL()->set_context_lost(true);
  EXPECT_FALSE(bridge->IsValid());

  // Restoration will fail because
  // Platform::createSharedOffscreenGraphicsContext3DProvider() is stubbed
  // in unit tests.  This simulates what would happen when attempting to
  // restore while the GPU process is down.
  bridge->Restore();

  viz::TransferableResource resource;
  std::unique_ptr<viz::SingleReleaseCallback> release_callback;
  EXPECT_FALSE(bridge->PrepareTransferableResource(nullptr, &resource,
                                                   &release_callback));
}

TEST_F(Canvas2DLayerBridgeTest, PrepareMailboxAndLoseResource) {
  // Prepare a mailbox, then report the resource as lost.
  // This test passes by not crashing and not triggering assertions.
  {
    std::unique_ptr<Canvas2DLayerBridge> bridge = MakeBridge(
        IntSize(300, 150), Canvas2DLayerBridge::kForceAccelerationForTesting,
        CanvasColorParams());
    bridge->FinalizeFrame();
    viz::TransferableResource resource;
    std::unique_ptr<viz::SingleReleaseCallback> release_callback;
    EXPECT_TRUE(bridge->PrepareTransferableResource(nullptr, &resource,
                                                    &release_callback));

    bool lost_resource = true;
    release_callback->Run(gpu::SyncToken(), lost_resource);
  }

  // Retry with mailbox released while bridge destruction is in progress.
  {
    viz::TransferableResource resource;
    std::unique_ptr<viz::SingleReleaseCallback> release_callback;

    {
      std::unique_ptr<Canvas2DLayerBridge> bridge = MakeBridge(
          IntSize(300, 150), Canvas2DLayerBridge::kForceAccelerationForTesting,
          CanvasColorParams());
      bridge->FinalizeFrame();
      bridge->PrepareTransferableResource(nullptr, &resource,
                                          &release_callback);
      // |bridge| goes out of scope and would normally be destroyed, but
      // object is kept alive by self references.
    }

    // This should cause the bridge to be destroyed.
    bool lost_resource = true;
    // Before fixing crbug.com/411864, the following line would cause a memory
    // use after free that sometimes caused a crash in normal builds and
    // crashed consistently with ASAN.
    release_callback->Run(gpu::SyncToken(), lost_resource);
  }
}

TEST_F(Canvas2DLayerBridgeTest, ReleaseCallbackWithNullContextProviderWrapper) {
  viz::TransferableResource resource;
  std::unique_ptr<viz::SingleReleaseCallback> release_callback;

  {
    std::unique_ptr<Canvas2DLayerBridge> bridge = MakeBridge(
        IntSize(300, 150), Canvas2DLayerBridge::kForceAccelerationForTesting,
        CanvasColorParams());
    bridge->FinalizeFrame();
    EXPECT_TRUE(bridge->PrepareTransferableResource(nullptr, &resource,
                                                    &release_callback));
  }

  bool lost_resource = true;
  test_context_provider_->TestContextGL()->set_context_lost(true);
  // Get a new context provider so that the WeakPtr to the old one is null.
  // This is the test to make sure that ReleaseMailboxImageResource() handles
  // null context_provider_wrapper properly.
  SharedGpuContext::ContextProviderWrapper();
  release_callback->Run(gpu::SyncToken(), lost_resource);
}

TEST_F(Canvas2DLayerBridgeTest, AccelerationHint) {
  {
    std::unique_ptr<Canvas2DLayerBridge> bridge =
        MakeBridge(IntSize(300, 300), Canvas2DLayerBridge::kEnableAcceleration,
                   CanvasColorParams());
    PaintFlags flags;
    bridge->DrawingCanvas()->drawRect(SkRect::MakeXYWH(0, 0, 1, 1), flags);
    scoped_refptr<StaticBitmapImage> image =
        bridge->NewImageSnapshot(kPreferAcceleration);
    EXPECT_TRUE(bridge->IsValid());
    EXPECT_TRUE(bridge->IsAccelerated());
  }

  {
    std::unique_ptr<Canvas2DLayerBridge> bridge =
        MakeBridge(IntSize(300, 300), Canvas2DLayerBridge::kEnableAcceleration,
                   CanvasColorParams());
    PaintFlags flags;
    bridge->DrawingCanvas()->drawRect(SkRect::MakeXYWH(0, 0, 1, 1), flags);
    scoped_refptr<StaticBitmapImage> image =
        bridge->NewImageSnapshot(kPreferNoAcceleration);
    EXPECT_TRUE(bridge->IsValid());
    EXPECT_TRUE(bridge->IsAccelerated());
  }

  {
    std::unique_ptr<Canvas2DLayerBridge> bridge =
        MakeBridge(IntSize(300, 300), Canvas2DLayerBridge::kDisableAcceleration,
                   CanvasColorParams());
    PaintFlags flags;
    bridge->DrawingCanvas()->drawRect(SkRect::MakeXYWH(0, 0, 1, 1), flags);
    scoped_refptr<StaticBitmapImage> image =
        bridge->NewImageSnapshot(kPreferAcceleration);
    EXPECT_TRUE(bridge->IsValid());
    EXPECT_FALSE(bridge->IsAccelerated());
  }

  {
    std::unique_ptr<Canvas2DLayerBridge> bridge =
        MakeBridge(IntSize(300, 300), Canvas2DLayerBridge::kDisableAcceleration,
                   CanvasColorParams());
    PaintFlags flags;
    bridge->DrawingCanvas()->drawRect(SkRect::MakeXYWH(0, 0, 1, 1), flags);
    scoped_refptr<StaticBitmapImage> image =
        bridge->NewImageSnapshot(kPreferNoAcceleration);
    EXPECT_TRUE(bridge->IsValid());
    EXPECT_FALSE(bridge->IsAccelerated());
  }
}

TEST_F(Canvas2DLayerBridgeTest, FallbackToSoftwareIfContextLost) {
  test_context_provider_->TestContextGL()->set_context_lost(true);
  std::unique_ptr<Canvas2DLayerBridge> bridge =
      MakeBridge(IntSize(300, 150), Canvas2DLayerBridge::kEnableAcceleration,
                 CanvasColorParams());
  EXPECT_TRUE(bridge->IsValid());
  EXPECT_FALSE(bridge->IsAccelerated());
}

TEST_F(Canvas2DLayerBridgeTest, FallbackToSoftwareOnFailedTextureAlloc) {
  {
    // No fallback case.
    std::unique_ptr<Canvas2DLayerBridge> bridge =
        MakeBridge(IntSize(300, 150), Canvas2DLayerBridge::kEnableAcceleration,
                   CanvasColorParams());
    EXPECT_TRUE(bridge->IsValid());
    EXPECT_TRUE(bridge->IsAccelerated());
    scoped_refptr<StaticBitmapImage> snapshot =
        bridge->NewImageSnapshot(kPreferAcceleration);
    EXPECT_TRUE(bridge->IsAccelerated());
    EXPECT_TRUE(snapshot->IsTextureBacked());
  }

  {
    // Fallback case.
    GrContext* gr = SharedGpuContext::ContextProviderWrapper()
                        ->ContextProvider()
                        ->GetGrContext();
    std::unique_ptr<Canvas2DLayerBridge> bridge =
        MakeBridge(IntSize(300, 150), Canvas2DLayerBridge::kEnableAcceleration,
                   CanvasColorParams());
    EXPECT_TRUE(bridge->IsValid());
    EXPECT_TRUE(bridge->IsAccelerated());  // We don't yet know that
                                           // allocation will fail.
    // This will cause SkSurface_Gpu creation to fail without
    // Canvas2DLayerBridge otherwise detecting that anything was disabled.
    gr->abandonContext();
    scoped_refptr<StaticBitmapImage> snapshot =
        bridge->NewImageSnapshot(kPreferAcceleration);
    EXPECT_FALSE(bridge->IsAccelerated());
    EXPECT_FALSE(snapshot->IsTextureBacked());
  }
}

class MockLogger : public Canvas2DLayerBridge::Logger {
 public:
  MOCK_METHOD1(ReportHibernationEvent,
               void(Canvas2DLayerBridge::HibernationEvent));
  MOCK_METHOD0(DidStartHibernating, void());
  ~MockLogger() override = default;
};

void DrawSomething(Canvas2DLayerBridge* bridge) {
  bridge->DidDraw(FloatRect(0, 0, 1, 1));
  bridge->FinalizeFrame();
  // Grabbing an image forces a flush
  bridge->NewImageSnapshot(kPreferAcceleration);
}

#if CANVAS2D_HIBERNATION_ENABLED
TEST_F(Canvas2DLayerBridgeTest, HibernationLifeCycle)
#else
TEST_F(Canvas2DLayerBridgeTest, DISABLED_HibernationLifeCycle)
#endif
{
  ScopedTestingPlatformSupport<GpuMemoryBufferTestPlatform> platform;
  std::unique_ptr<Canvas2DLayerBridge> bridge =
      MakeBridge(IntSize(300, 300), Canvas2DLayerBridge::kEnableAcceleration,
                 CanvasColorParams());
  bridge->DontUseIdleSchedulingForTesting();
  DrawSomething(bridge.get());
  EXPECT_TRUE(bridge->IsAccelerated());

  // Register an alternate Logger for tracking hibernation events
  std::unique_ptr<MockLogger> mock_logger = std::make_unique<MockLogger>();
  MockLogger* mock_logger_ptr = mock_logger.get();
  bridge->SetLoggerForTesting(std::move(mock_logger));

  // Test entering hibernation
  EXPECT_CALL(
      *mock_logger_ptr,
      ReportHibernationEvent(Canvas2DLayerBridge::kHibernationScheduled));
  EXPECT_CALL(*mock_logger_ptr, DidStartHibernating()).Times(1);

  bridge->SetIsInHiddenPage(true);
  platform->RunUntilIdle();

  testing::Mock::VerifyAndClearExpectations(mock_logger_ptr);
  EXPECT_FALSE(bridge->IsAccelerated());
  EXPECT_TRUE(bridge->IsHibernating());
  EXPECT_TRUE(bridge->IsValid());

  // Test exiting hibernation
  EXPECT_CALL(
      *mock_logger_ptr,
      ReportHibernationEvent(Canvas2DLayerBridge::kHibernationEndedNormally));

  bridge->SetIsInHiddenPage(false);

  testing::Mock::VerifyAndClearExpectations(mock_logger_ptr);
  EXPECT_TRUE(bridge->IsAccelerated());
  EXPECT_FALSE(bridge->IsHibernating());
  EXPECT_TRUE(bridge->IsValid());
}

#if CANVAS2D_HIBERNATION_ENABLED
TEST_F(Canvas2DLayerBridgeTest, HibernationReEntry)
#else
TEST_F(Canvas2DLayerBridgeTest, DISABLED_HibernationReEntry)
#endif
{
  ScopedTestingPlatformSupport<GpuMemoryBufferTestPlatform> platform;
  std::unique_ptr<Canvas2DLayerBridge> bridge =
      MakeBridge(IntSize(300, 300), Canvas2DLayerBridge::kEnableAcceleration,
                 CanvasColorParams());
  bridge->DontUseIdleSchedulingForTesting();
  DrawSomething(bridge.get());

  // Register an alternate Logger for tracking hibernation events
  std::unique_ptr<MockLogger> mock_logger = std::make_unique<MockLogger>();
  MockLogger* mock_logger_ptr = mock_logger.get();
  bridge->SetLoggerForTesting(std::move(mock_logger));

  // Test entering hibernation
  EXPECT_CALL(
      *mock_logger_ptr,
      ReportHibernationEvent(Canvas2DLayerBridge::kHibernationScheduled));
  EXPECT_CALL(*mock_logger_ptr, DidStartHibernating()).Times(1);
  bridge->SetIsInHiddenPage(true);
  // Toggle visibility before the task that enters hibernation gets a
  // chance to run.
  bridge->SetIsInHiddenPage(false);
  bridge->SetIsInHiddenPage(true);
  platform->RunUntilIdle();

  testing::Mock::VerifyAndClearExpectations(mock_logger_ptr);
  EXPECT_FALSE(bridge->IsAccelerated());
  EXPECT_TRUE(bridge->IsHibernating());
  EXPECT_TRUE(bridge->IsValid());

  // Test exiting hibernation
  EXPECT_CALL(
      *mock_logger_ptr,
      ReportHibernationEvent(Canvas2DLayerBridge::kHibernationEndedNormally));

  bridge->SetIsInHiddenPage(false);

  testing::Mock::VerifyAndClearExpectations(mock_logger_ptr);
  EXPECT_TRUE(bridge->IsAccelerated());
  EXPECT_FALSE(bridge->IsHibernating());
  EXPECT_TRUE(bridge->IsValid());
}

#if CANVAS2D_HIBERNATION_ENABLED && CANVAS2D_BACKGROUND_RENDER_SWITCH_TO_CPU
TEST_F(Canvas2DLayerBridgeTest, BackgroundRenderingWhileHibernating)
#else
TEST_F(Canvas2DLayerBridgeTest, DISABLED_BackgroundRenderingWhileHibernating)
#endif
{
  ScopedTestingPlatformSupport<GpuMemoryBufferTestPlatform> platform;
  std::unique_ptr<Canvas2DLayerBridge> bridge =
      MakeBridge(IntSize(300, 300), Canvas2DLayerBridge::kEnableAcceleration,
                 CanvasColorParams());
  bridge->DontUseIdleSchedulingForTesting();
  DrawSomething(bridge.get());

  // Register an alternate Logger for tracking hibernation events
  std::unique_ptr<MockLogger> mock_logger = std::make_unique<MockLogger>();
  MockLogger* mock_logger_ptr = mock_logger.get();
  bridge->SetLoggerForTesting(std::move(mock_logger));

  // Test entering hibernation
  EXPECT_CALL(
      *mock_logger_ptr,
      ReportHibernationEvent(Canvas2DLayerBridge::kHibernationScheduled));
  EXPECT_CALL(*mock_logger_ptr, DidStartHibernating()).Times(1);
  bridge->SetIsInHiddenPage(true);
  platform->RunUntilIdle();
  testing::Mock::VerifyAndClearExpectations(mock_logger_ptr);
  EXPECT_FALSE(bridge->IsAccelerated());
  EXPECT_TRUE(bridge->IsHibernating());
  EXPECT_TRUE(bridge->IsValid());

  // Rendering in the background -> temp switch to SW
  EXPECT_CALL(*mock_logger_ptr,
              ReportHibernationEvent(
                  Canvas2DLayerBridge::
                      kHibernationEndedWithSwitchToBackgroundRendering));
  DrawSomething(bridge.get());
  testing::Mock::VerifyAndClearExpectations(mock_logger_ptr);
  EXPECT_FALSE(bridge->IsAccelerated());
  EXPECT_FALSE(bridge->IsHibernating());
  EXPECT_TRUE(bridge->IsValid());

  // Unhide
  bridge->SetIsInHiddenPage(false);
  testing::Mock::VerifyAndClearExpectations(mock_logger_ptr);
  EXPECT_TRUE(
      bridge->IsAccelerated());  // Becoming visible causes switch back to GPU
  EXPECT_FALSE(bridge->IsHibernating());
  EXPECT_TRUE(bridge->IsValid());
}

#if CANVAS2D_HIBERNATION_ENABLED
TEST_F(Canvas2DLayerBridgeTest, TeardownWhileHibernating)
#else
TEST_F(Canvas2DLayerBridgeTest, DISABLED_TeardownWhileHibernating)
#endif
{
  ScopedTestingPlatformSupport<GpuMemoryBufferTestPlatform> platform;
  std::unique_ptr<Canvas2DLayerBridge> bridge =
      MakeBridge(IntSize(300, 300), Canvas2DLayerBridge::kEnableAcceleration,
                 CanvasColorParams());
  bridge->DontUseIdleSchedulingForTesting();
  DrawSomething(bridge.get());

  // Register an alternate Logger for tracking hibernation events
  std::unique_ptr<MockLogger> mock_logger = std::make_unique<MockLogger>();
  MockLogger* mock_logger_ptr = mock_logger.get();
  bridge->SetLoggerForTesting(std::move(mock_logger));

  // Test entering hibernation
  EXPECT_CALL(
      *mock_logger_ptr,
      ReportHibernationEvent(Canvas2DLayerBridge::kHibernationScheduled));
  EXPECT_CALL(*mock_logger_ptr, DidStartHibernating()).Times(1);
  bridge->SetIsInHiddenPage(true);
  platform->RunUntilIdle();
  testing::Mock::VerifyAndClearExpectations(mock_logger_ptr);
  EXPECT_FALSE(bridge->IsAccelerated());
  EXPECT_TRUE(bridge->IsHibernating());
  EXPECT_TRUE(bridge->IsValid());

  // Tear down the bridge while hibernating
  EXPECT_CALL(*mock_logger_ptr,
              ReportHibernationEvent(
                  Canvas2DLayerBridge::kHibernationEndedWithTeardown));
  bridge.reset();
  testing::Mock::VerifyAndClearExpectations(mock_logger_ptr);
}

#if CANVAS2D_HIBERNATION_ENABLED
TEST_F(Canvas2DLayerBridgeTest, SnapshotWhileHibernating)
#else
TEST_F(Canvas2DLayerBridgeTest, DISABLED_SnapshotWhileHibernating)
#endif
{
  ScopedTestingPlatformSupport<GpuMemoryBufferTestPlatform> platform;
  std::unique_ptr<Canvas2DLayerBridge> bridge =
      MakeBridge(IntSize(300, 300), Canvas2DLayerBridge::kEnableAcceleration,
                 CanvasColorParams());
  bridge->DontUseIdleSchedulingForTesting();
  DrawSomething(bridge.get());

  // Register an alternate Logger for tracking hibernation events
  std::unique_ptr<MockLogger> mock_logger = std::make_unique<MockLogger>();
  MockLogger* mock_logger_ptr = mock_logger.get();
  bridge->SetLoggerForTesting(std::move(mock_logger));

  // Test entering hibernation
  EXPECT_CALL(
      *mock_logger_ptr,
      ReportHibernationEvent(Canvas2DLayerBridge::kHibernationScheduled));
  EXPECT_CALL(*mock_logger_ptr, DidStartHibernating()).Times(1);
  bridge->SetIsInHiddenPage(true);
  platform->RunUntilIdle();
  testing::Mock::VerifyAndClearExpectations(mock_logger_ptr);
  EXPECT_FALSE(bridge->IsAccelerated());
  EXPECT_TRUE(bridge->IsHibernating());
  EXPECT_TRUE(bridge->IsValid());

  // Take a snapshot and verify that it is not accelerated due to hibernation
  scoped_refptr<StaticBitmapImage> image =
      bridge->NewImageSnapshot(kPreferAcceleration);
  EXPECT_FALSE(image->IsTextureBacked());
  image = nullptr;

  // Verify that taking a snapshot did not affect the state of bridge
  EXPECT_FALSE(bridge->IsAccelerated());
  EXPECT_TRUE(bridge->IsHibernating());
  EXPECT_TRUE(bridge->IsValid());

  // End hibernation normally
  EXPECT_CALL(
      *mock_logger_ptr,
      ReportHibernationEvent(Canvas2DLayerBridge::kHibernationEndedNormally))
      .Times(1);
  bridge->SetIsInHiddenPage(false);
}

#if CANVAS2D_HIBERNATION_ENABLED
TEST_F(Canvas2DLayerBridgeTest, TeardownWhileHibernationIsPending)
#else
TEST_F(Canvas2DLayerBridgeTest, DISABLED_TeardownWhileHibernationIsPending)
#endif
{
  ScopedTestingPlatformSupport<GpuMemoryBufferTestPlatform> platform;
  std::unique_ptr<Canvas2DLayerBridge> bridge =
      MakeBridge(IntSize(300, 300), Canvas2DLayerBridge::kEnableAcceleration,
                 CanvasColorParams());
  bridge->DontUseIdleSchedulingForTesting();
  DrawSomething(bridge.get());

  // Register an alternate Logger for tracking hibernation events
  std::unique_ptr<MockLogger> mock_logger = std::make_unique<MockLogger>();
  MockLogger* mock_logger_ptr = mock_logger.get();
  bridge->SetLoggerForTesting(std::move(mock_logger));

  // Test entering hibernation
  EXPECT_CALL(
      *mock_logger_ptr,
      ReportHibernationEvent(Canvas2DLayerBridge::kHibernationScheduled));
  bridge->SetIsInHiddenPage(true);
  bridge.reset();
  // In production, we would expect a
  // HibernationAbortedDueToDestructionWhileHibernatePending event to be
  // fired, but that signal is lost in the unit test due to no longer having
  // a bridge to hold the mockLogger.
  platform->RunUntilIdle();
  // This test passes by not crashing, which proves that the WeakPtr logic
  // is sound.
}

#if CANVAS2D_HIBERNATION_ENABLED
TEST_F(Canvas2DLayerBridgeTest, HibernationAbortedDueToVisibilityChange)
#else
TEST_F(Canvas2DLayerBridgeTest,
       DISABLED_HibernationAbortedDueToVisibilityChange)
#endif
{
  ScopedTestingPlatformSupport<GpuMemoryBufferTestPlatform> platform;
  std::unique_ptr<Canvas2DLayerBridge> bridge =
      MakeBridge(IntSize(300, 300), Canvas2DLayerBridge::kEnableAcceleration,
                 CanvasColorParams());
  bridge->DontUseIdleSchedulingForTesting();
  DrawSomething(bridge.get());

  // Register an alternate Logger for tracking hibernation events
  std::unique_ptr<MockLogger> mock_logger = std::make_unique<MockLogger>();
  MockLogger* mock_logger_ptr = mock_logger.get();
  bridge->SetLoggerForTesting(std::move(mock_logger));

  // Test entering hibernation
  EXPECT_CALL(
      *mock_logger_ptr,
      ReportHibernationEvent(Canvas2DLayerBridge::kHibernationScheduled));
  EXPECT_CALL(
      *mock_logger_ptr,
      ReportHibernationEvent(
          Canvas2DLayerBridge::kHibernationAbortedDueToVisibilityChange))
      .Times(1);
  bridge->SetIsInHiddenPage(true);
  bridge->SetIsInHiddenPage(false);
  platform->RunUntilIdle();
  testing::Mock::VerifyAndClearExpectations(mock_logger_ptr);
  EXPECT_TRUE(bridge->IsAccelerated());
  EXPECT_FALSE(bridge->IsHibernating());
  EXPECT_TRUE(bridge->IsValid());
}

#if CANVAS2D_HIBERNATION_ENABLED
TEST_F(Canvas2DLayerBridgeTest, HibernationAbortedDueToLostContext)
#else
TEST_F(Canvas2DLayerBridgeTest, DISABLED_HibernationAbortedDueToLostContext)
#endif
{
  ScopedTestingPlatformSupport<GpuMemoryBufferTestPlatform> platform;
  std::unique_ptr<Canvas2DLayerBridge> bridge =
      MakeBridge(IntSize(300, 300), Canvas2DLayerBridge::kEnableAcceleration,
                 CanvasColorParams());
  bridge->DontUseIdleSchedulingForTesting();
  DrawSomething(bridge.get());

  // Register an alternate Logger for tracking hibernation events
  std::unique_ptr<MockLogger> mock_logger = std::make_unique<MockLogger>();
  MockLogger* mock_logger_ptr = mock_logger.get();
  bridge->SetLoggerForTesting(std::move(mock_logger));

  test_context_provider_->TestContextGL()->set_context_lost(true);

  // Test entering hibernation
  EXPECT_CALL(
      *mock_logger_ptr,
      ReportHibernationEvent(Canvas2DLayerBridge::kHibernationScheduled));
  EXPECT_CALL(*mock_logger_ptr,
              ReportHibernationEvent(
                  Canvas2DLayerBridge::kHibernationAbortedDueGpuContextLoss))
      .Times(1);

  bridge->SetIsInHiddenPage(true);
  platform->RunUntilIdle();
  testing::Mock::VerifyAndClearExpectations(mock_logger_ptr);
  EXPECT_FALSE(bridge->IsHibernating());
}

#if CANVAS2D_HIBERNATION_ENABLED
TEST_F(Canvas2DLayerBridgeTest, PrepareMailboxWhileHibernating)
#else
TEST_F(Canvas2DLayerBridgeTest, DISABLED_PrepareMailboxWhileHibernating)
#endif
{
  ScopedTestingPlatformSupport<GpuMemoryBufferTestPlatform> platform;
  std::unique_ptr<Canvas2DLayerBridge> bridge =
      MakeBridge(IntSize(300, 300), Canvas2DLayerBridge::kEnableAcceleration,
                 CanvasColorParams());
  bridge->DontUseIdleSchedulingForTesting();
  DrawSomething(bridge.get());

  // Register an alternate Logger for tracking hibernation events
  std::unique_ptr<MockLogger> mock_logger = std::make_unique<MockLogger>();
  MockLogger* mock_logger_ptr = mock_logger.get();
  bridge->SetLoggerForTesting(std::move(mock_logger));

  // Test entering hibernation
  EXPECT_CALL(
      *mock_logger_ptr,
      ReportHibernationEvent(Canvas2DLayerBridge::kHibernationScheduled));
  EXPECT_CALL(*mock_logger_ptr, DidStartHibernating()).Times(1);
  bridge->SetIsInHiddenPage(true);
  platform->RunUntilIdle();
  testing::Mock::VerifyAndClearExpectations(mock_logger_ptr);

  // Test PrepareTransferableResource() while hibernating
  viz::TransferableResource resource;
  std::unique_ptr<viz::SingleReleaseCallback> release_callback;
  EXPECT_FALSE(bridge->PrepareTransferableResource(nullptr, &resource,
                                                   &release_callback));
  EXPECT_TRUE(bridge->IsValid());

  // Tear down the bridge on the thread so that 'bridge' can go out of scope
  // without crashing due to thread checks
  EXPECT_CALL(*mock_logger_ptr,
              ReportHibernationEvent(
                  Canvas2DLayerBridge::kHibernationEndedWithTeardown));
  bridge.reset();
  testing::Mock::VerifyAndClearExpectations(mock_logger_ptr);
}

#if CANVAS2D_HIBERNATION_ENABLED && CANVAS2D_BACKGROUND_RENDER_SWITCH_TO_CPU
TEST_F(Canvas2DLayerBridgeTest, PrepareMailboxWhileBackgroundRendering)
#else
TEST_F(Canvas2DLayerBridgeTest, DISABLED_PrepareMailboxWhileBackgroundRendering)
#endif
{
  ScopedTestingPlatformSupport<GpuMemoryBufferTestPlatform> platform;
  std::unique_ptr<Canvas2DLayerBridge> bridge =
      MakeBridge(IntSize(300, 300), Canvas2DLayerBridge::kEnableAcceleration,
                 CanvasColorParams());
  DrawSomething(bridge.get());

  // Register an alternate Logger for tracking hibernation events
  std::unique_ptr<MockLogger> mock_logger = std::make_unique<MockLogger>();
  MockLogger* mock_logger_ptr = mock_logger.get();
  bridge->SetLoggerForTesting(std::move(mock_logger));

  // Test entering hibernation
  std::unique_ptr<base::WaitableEvent> hibernation_started_event =
      std::make_unique<base::WaitableEvent>();
  EXPECT_CALL(
      *mock_logger_ptr,
      ReportHibernationEvent(Canvas2DLayerBridge::kHibernationScheduled));
  EXPECT_CALL(*mock_logger_ptr, DidStartHibernating()).Times(1);
  bridge->SetIsInHiddenPage(true);
  platform->RunUntilIdle();
  testing::Mock::VerifyAndClearExpectations(mock_logger_ptr);

  // Rendering in the background -> temp switch to SW
  EXPECT_CALL(*mock_logger_ptr,
              ReportHibernationEvent(
                  Canvas2DLayerBridge::
                      kHibernationEndedWithSwitchToBackgroundRendering));
  DrawSomething(bridge.get());
  testing::Mock::VerifyAndClearExpectations(mock_logger_ptr);
  EXPECT_FALSE(bridge->IsAccelerated());
  EXPECT_FALSE(bridge->IsHibernating());
  EXPECT_TRUE(bridge->IsValid());

  // Test prepareMailbox while background rendering
  viz::TransferableResource resource;
  std::unique_ptr<viz::SingleReleaseCallback> release_callback;
  EXPECT_FALSE(bridge->PrepareTransferableResource(nullptr, &resource,
                                                   &release_callback));
  EXPECT_TRUE(bridge->IsValid());
}

TEST_F(Canvas2DLayerBridgeTest, ResourceRecycling) {
  ScopedCanvas2dImageChromiumForTest canvas_2d_image_chromium(true);
  const_cast<gpu::Capabilities&>(SharedGpuContext::ContextProviderWrapper()
                                     ->ContextProvider()
                                     ->GetCapabilities())
      .gpu_memory_buffer_formats.Add(gfx::BufferFormat::BGRA_8888);

  viz::TransferableResource resources[3];
  std::unique_ptr<viz::SingleReleaseCallback> callbacks[3];

  std::unique_ptr<Canvas2DLayerBridge> bridge = MakeBridge(
      IntSize(300, 150), Canvas2DLayerBridge::kForceAccelerationForTesting,
      CanvasColorParams());
  DrawSomething(bridge.get());
  ASSERT_TRUE(bridge->PrepareTransferableResource(nullptr, &resources[0],
                                                  &callbacks[0]));
  DrawSomething(bridge.get());
  ASSERT_TRUE(bridge->PrepareTransferableResource(nullptr, &resources[1],
                                                  &callbacks[1]));
  EXPECT_NE(resources[0].mailbox_holder.mailbox,
            resources[1].mailbox_holder.mailbox);

  // Now release the first resource and draw again. It should be reused due to
  // recycling.
  callbacks[0]->Run(gpu::SyncToken(), false);
  DrawSomething(bridge.get());
  ASSERT_TRUE(bridge->PrepareTransferableResource(nullptr, &resources[2],
                                                  &callbacks[2]));
  EXPECT_EQ(resources[0].mailbox_holder.mailbox,
            resources[2].mailbox_holder.mailbox);

  callbacks[1]->Run(gpu::SyncToken(), false);
  callbacks[2]->Run(gpu::SyncToken(), false);
}

TEST_F(Canvas2DLayerBridgeTest, NoResourceRecyclingWhenPageHidden) {
  ScopedCanvas2dImageChromiumForTest canvas_2d_image_chromium(true);
  const_cast<gpu::Capabilities&>(SharedGpuContext::ContextProviderWrapper()
                                     ->ContextProvider()
                                     ->GetCapabilities())
      .gpu_memory_buffer_formats.Add(gfx::BufferFormat::BGRA_8888);

  viz::TransferableResource resources[2];
  std::unique_ptr<viz::SingleReleaseCallback> callbacks[2];

  std::unique_ptr<Canvas2DLayerBridge> bridge = MakeBridge(
      IntSize(300, 150), Canvas2DLayerBridge::kForceAccelerationForTesting,
      CanvasColorParams());
  DrawSomething(bridge.get());
  ASSERT_TRUE(bridge->PrepareTransferableResource(nullptr, &resources[0],
                                                  &callbacks[0]));
  DrawSomething(bridge.get());
  ASSERT_TRUE(bridge->PrepareTransferableResource(nullptr, &resources[1],
                                                  &callbacks[1]));
  EXPECT_NE(resources[0].mailbox_holder.mailbox,
            resources[1].mailbox_holder.mailbox);

  // Now release the first resource and mark the page hidden. The recycled
  // resource should be dropped.
  callbacks[0]->Run(gpu::SyncToken(), false);
  EXPECT_EQ(test_context_provider_->TestContextGL()->NumTextures(), 2u);
  bridge->SetIsInHiddenPage(true);
  EXPECT_EQ(test_context_provider_->TestContextGL()->NumTextures(), 1u);

  // Release second frame, this resource is not released because its the current
  // render target for the canvas. It should only be released if the canvas is
  // hibernated.
  callbacks[1]->Run(gpu::SyncToken(), false);
  EXPECT_EQ(test_context_provider_->TestContextGL()->NumTextures(), 1u);
}

TEST_F(Canvas2DLayerBridgeTest, ReleaseResourcesAfterBridgeDestroyed) {
  ScopedCanvas2dImageChromiumForTest canvas_2d_image_chromium(true);
  const_cast<gpu::Capabilities&>(SharedGpuContext::ContextProviderWrapper()
                                     ->ContextProvider()
                                     ->GetCapabilities())
      .gpu_memory_buffer_formats.Add(gfx::BufferFormat::BGRA_8888);

  viz::TransferableResource resource;
  std::unique_ptr<viz::SingleReleaseCallback> release_callback;

  std::unique_ptr<Canvas2DLayerBridge> bridge = MakeBridge(
      IntSize(300, 150), Canvas2DLayerBridge::kForceAccelerationForTesting,
      CanvasColorParams());
  DrawSomething(bridge.get());
  bridge->PrepareTransferableResource(nullptr, &resource, &release_callback);

  // Tearing down the bridge does not destroy unreleased resources.
  bridge.reset();
  EXPECT_EQ(test_context_provider_->TestContextGL()->NumTextures(), 1u);
  constexpr bool lost_resource = false;
  release_callback->Run(gpu::SyncToken(), lost_resource);
  EXPECT_EQ(test_context_provider_->TestContextGL()->NumTextures(), 0u);
  release_callback = nullptr;
}

TEST_F(Canvas2DLayerBridgeTest, EnsureCCImageCacheUse) {
  auto color_params = CanvasColorParams(CanvasColorSpace::kSRGB,
                                        CanvasPixelFormat::kF16, kOpaque);

  std::unique_ptr<Canvas2DLayerBridge> bridge =
      MakeBridge(IntSize(300, 300), Canvas2DLayerBridge::kEnableAcceleration,
                 color_params);
  gfx::ColorSpace expected_color_space = gfx::ColorSpace::CreateSRGB();
  Vector<cc::DrawImage> images = {
      cc::DrawImage(cc::CreateDiscardablePaintImage(gfx::Size(10, 10)),
                    SkIRect::MakeWH(10, 10), kNone_SkFilterQuality,
                    SkMatrix::I(), 0u, expected_color_space),
      cc::DrawImage(cc::CreateDiscardablePaintImage(gfx::Size(20, 20)),
                    SkIRect::MakeWH(5, 5), kNone_SkFilterQuality, SkMatrix::I(),
                    0u, expected_color_space)};

  bridge->DrawingCanvas()->drawImage(images[0].paint_image(), 0u, 0u, nullptr);
  bridge->DrawingCanvas()->drawImageRect(
      images[1].paint_image(), SkRect::MakeWH(5u, 5u), SkRect::MakeWH(5u, 5u),
      nullptr, cc::PaintCanvas::kFast_SrcRectConstraint);
  bridge->NewImageSnapshot(kPreferAcceleration);

  EXPECT_EQ(image_decode_cache_.decoded_images(), images);
}

TEST_F(Canvas2DLayerBridgeTest, EnsureCCImageCacheUseWithColorConversion) {
  auto color_params = CanvasColorParams(
      CanvasColorSpace::kSRGB, CanvasColorParams::GetNativeCanvasPixelFormat(),
      kOpaque);
  std::unique_ptr<Canvas2DLayerBridge> bridge =
      MakeBridge(IntSize(300, 300), Canvas2DLayerBridge::kEnableAcceleration,
                 color_params);
  Vector<cc::DrawImage> images = {
      cc::DrawImage(cc::CreateDiscardablePaintImage(gfx::Size(10, 10)),
                    SkIRect::MakeWH(10, 10), kNone_SkFilterQuality,
                    SkMatrix::I(), 0u, color_params.GetStorageGfxColorSpace()),
      cc::DrawImage(cc::CreateDiscardablePaintImage(gfx::Size(20, 20)),
                    SkIRect::MakeWH(5, 5), kNone_SkFilterQuality, SkMatrix::I(),
                    0u, color_params.GetStorageGfxColorSpace())};

  bridge->DrawingCanvas()->drawImage(images[0].paint_image(), 0u, 0u, nullptr);
  bridge->DrawingCanvas()->drawImageRect(
      images[1].paint_image(), SkRect::MakeWH(5u, 5u), SkRect::MakeWH(5u, 5u),
      nullptr, cc::PaintCanvas::kFast_SrcRectConstraint);
  bridge->NewImageSnapshot(kPreferAcceleration);

  EXPECT_EQ(image_decode_cache_.decoded_images(), images);
}

TEST_F(Canvas2DLayerBridgeTest, ImagesLockedUntilCacheLimit) {
  auto color_params = CanvasColorParams(CanvasColorSpace::kSRGB,
                                        CanvasPixelFormat::kF16, kOpaque);
  std::unique_ptr<Canvas2DLayerBridge> bridge =
      MakeBridge(IntSize(300, 300), Canvas2DLayerBridge::kEnableAcceleration,
                 color_params);

  Vector<cc::DrawImage> images = {
      cc::DrawImage(cc::CreateDiscardablePaintImage(gfx::Size(10, 10)),
                    SkIRect::MakeWH(10, 10), kNone_SkFilterQuality,
                    SkMatrix::I(), 0u, color_params.GetStorageGfxColorSpace()),
      cc::DrawImage(cc::CreateDiscardablePaintImage(gfx::Size(20, 20)),
                    SkIRect::MakeWH(5, 5), kNone_SkFilterQuality, SkMatrix::I(),
                    0u, color_params.GetStorageGfxColorSpace()),
      cc::DrawImage(cc::CreateDiscardablePaintImage(gfx::Size(20, 20)),
                    SkIRect::MakeWH(5, 5), kNone_SkFilterQuality, SkMatrix::I(),
                    0u, color_params.GetStorageGfxColorSpace())};

  // First 2 images are budgeted, they should remain locked after the op.
  bridge->DrawingCanvas()->drawImage(images[0].paint_image(), 0u, 0u, nullptr);
  bridge->DrawingCanvas()->drawImage(images[1].paint_image(), 0u, 0u, nullptr);
  // TODO(jochin): Can just call provider::FlushSkia() once we move recorder_
  // to the resource provider. The following is a temp workaround.
  cc::PaintCanvas* canvas = bridge->GetOrCreateResourceProvider()->Canvas();
  canvas->drawPicture(bridge->record_for_testing());
  EXPECT_EQ(image_decode_cache_.num_locked_images(), 2);

  // Next image is not budgeted, we should unlock all images other than the last
  // image.
  image_decode_cache_.set_budget_exceeded(true);
  bridge->DrawingCanvas()->drawImage(images[2].paint_image(), 0u, 0u, nullptr);
  canvas->drawPicture(bridge->record_for_testing());
  EXPECT_EQ(image_decode_cache_.num_locked_images(), 1);

  // Ask the provider to release everything, no locked images should remain.
  bridge->GetOrCreateResourceProvider()->ReleaseLockedImages();
  EXPECT_EQ(image_decode_cache_.num_locked_images(), 0);
}

TEST_F(Canvas2DLayerBridgeTest, QueuesCleanupTaskForLockedImages) {
  auto color_params = CanvasColorParams(CanvasColorSpace::kSRGB,
                                        CanvasPixelFormat::kF16, kOpaque);
  std::unique_ptr<Canvas2DLayerBridge> bridge =
      MakeBridge(IntSize(300, 300), Canvas2DLayerBridge::kEnableAcceleration,
                 color_params);

  auto image =
      cc::DrawImage(cc::CreateDiscardablePaintImage(gfx::Size(10, 10)),
                    SkIRect::MakeWH(10, 10), kNone_SkFilterQuality,
                    SkMatrix::I(), 0u, color_params.GetStorageGfxColorSpace());
  bridge->DrawingCanvas()->drawImage(image.paint_image(), 0u, 0u, nullptr);

  // TODO(jochin): Can just call provider::FlushSkia() once we move recorder_
  // to the resource provider. The following is a temp workaround.
  cc::PaintCanvas* canvas = bridge->GetOrCreateResourceProvider()->Canvas();
  canvas->drawPicture(bridge->record_for_testing());
  EXPECT_EQ(image_decode_cache_.num_locked_images(), 1);

  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(image_decode_cache_.num_locked_images(), 0);
}

TEST_F(Canvas2DLayerBridgeTest, ImageCacheOnContextLost) {
  auto color_params = CanvasColorParams(CanvasColorSpace::kSRGB,
                                        CanvasPixelFormat::kF16, kOpaque);
  std::unique_ptr<Canvas2DLayerBridge> bridge =
      MakeBridge(IntSize(300, 300), Canvas2DLayerBridge::kEnableAcceleration,
                 color_params);
  PaintFlags flags;
  Vector<cc::DrawImage> images = {
      cc::DrawImage(cc::CreateDiscardablePaintImage(gfx::Size(10, 10)),
                    SkIRect::MakeWH(10, 10), kNone_SkFilterQuality,
                    SkMatrix::I(), 0u, color_params.GetStorageGfxColorSpace()),
      cc::DrawImage(cc::CreateDiscardablePaintImage(gfx::Size(20, 20)),
                    SkIRect::MakeWH(5, 5), kNone_SkFilterQuality, SkMatrix::I(),
                    0u, color_params.GetStorageGfxColorSpace())};
  bridge->DrawingCanvas()->drawImage(images[0].paint_image(), 0u, 0u, nullptr);

  // Lose the context and ensure that the image provider is not used.
  bridge->GetOrCreateResourceProvider()->OnContextDestroyed();
  // We should unref all images on the cache when the context is destroyed.
  EXPECT_EQ(image_decode_cache_.num_locked_images(), 0);
  image_decode_cache_.set_disallow_cache_use(true);
  bridge->DrawingCanvas()->drawImage(images[1].paint_image(), 0u, 0u, &flags);
}

TEST_F(Canvas2DLayerBridgeTest,
       PrepareTransferableResourceTracksCanvasChanges) {
  IntSize size = IntSize(300, 300);
  std::unique_ptr<Canvas2DLayerBridge> bridge = MakeBridge(
      size, Canvas2DLayerBridge::kEnableAcceleration, CanvasColorParams());

  bridge->DrawingCanvas()->clear(SK_ColorRED);
  DrawSomething(bridge.get());
  ASSERT_TRUE(bridge->layer_for_testing());

  viz::TransferableResource resource;
  std::unique_ptr<viz::SingleReleaseCallback> release_callback;
  EXPECT_TRUE(bridge->PrepareTransferableResource(nullptr, &resource,
                                                  &release_callback));
  bridge->layer_for_testing()->SetTransferableResource(
      resource, std::move(release_callback));

  std::unique_ptr<viz::SingleReleaseCallback> release_callback2;
  EXPECT_FALSE(bridge->PrepareTransferableResource(nullptr, &resource,
                                                   &release_callback2));
  EXPECT_EQ(release_callback2, nullptr);
}
class CustomFakeCanvasResourceHost : public FakeCanvasResourceHost {
 public:
  explicit CustomFakeCanvasResourceHost(const IntSize& size)
      : FakeCanvasResourceHost(size) {}
  void RestoreCanvasMatrixClipStack(cc::PaintCanvas* canvas) const override {
    // Alter canvas' matrix to emulate a restore
    canvas->translate(5, 0);
  }
};

TEST_F(Canvas2DLayerBridgeTest, WritePixelsRestoresClipStack) {
  CanvasColorParams color_params(CanvasColorSpace::kSRGB,
                                 CanvasPixelFormat::kF16, kOpaque);
  IntSize size = IntSize(300, 300);
  auto host = std::make_unique<CustomFakeCanvasResourceHost>(size);
  std::unique_ptr<Canvas2DLayerBridge> bridge =
      MakeBridge(size, Canvas2DLayerBridge::kEnableAcceleration, color_params,
                 std::move(host));
  PaintFlags flags;

  EXPECT_EQ(bridge->DrawingCanvas()->getTotalMatrix().get(SkMatrix::kMTransX),
            0);

  cc::PaintCanvas* canvas = bridge->GetOrCreateResourceProvider()->Canvas();
  bridge->WritePixels(SkImageInfo::MakeN32Premul(10, 10), nullptr, 10, 0, 0);
  // Recording canvas maintain clip stack, while underlying SkCanvas should not.
  EXPECT_EQ(canvas->getTotalMatrix().get(SkMatrix::kMTransX), 0);
  EXPECT_EQ(bridge->DrawingCanvas()->getTotalMatrix().get(SkMatrix::kMTransX),
            5);

  bridge->DrawingCanvas()->drawLine(0, 0, 2, 2, flags);
  // Flush recording. Recording canvas should maintain matrix, while SkCanvas
  // should not.
  DrawSomething(bridge.get());
  EXPECT_EQ(bridge->DrawingCanvas()->getTotalMatrix().get(SkMatrix::kMTransX),
            5);
  EXPECT_EQ(canvas->getTotalMatrix().get(SkMatrix::kMTransX), 0);
}

TEST_F(Canvas2DLayerBridgeTest, DisplayedCanvasIsRateLimited) {
  std::unique_ptr<Canvas2DLayerBridge> bridge = MakeBridge(
      IntSize(300, 150), Canvas2DLayerBridge::kForceAccelerationForTesting,
      CanvasColorParams());
  EXPECT_TRUE(bridge->IsValid());
  bridge->SetIsBeingDisplayed(true);
  EXPECT_FALSE(bridge->HasRateLimiterForTesting());
  bridge->FinalizeFrame();
  bridge->FinalizeFrame();
  EXPECT_TRUE(bridge->HasRateLimiterForTesting());
}

TEST_F(Canvas2DLayerBridgeTest, NonDisplayedCanvasIsNotRateLimited) {
  std::unique_ptr<Canvas2DLayerBridge> bridge = MakeBridge(
      IntSize(300, 150), Canvas2DLayerBridge::kForceAccelerationForTesting,
      CanvasColorParams());
  EXPECT_TRUE(bridge->IsValid());
  bridge->SetIsBeingDisplayed(true);
  bridge->FinalizeFrame();
  bridge->FinalizeFrame();
  EXPECT_TRUE(bridge->HasRateLimiterForTesting());
  bridge->SetIsBeingDisplayed(false);
  EXPECT_FALSE(bridge->HasRateLimiterForTesting());
  bridge->FinalizeFrame();
  bridge->FinalizeFrame();
  EXPECT_FALSE(bridge->HasRateLimiterForTesting());
}

// Test if we skip dirty rect calculation for canvas smaller than 256x256.
TEST_F(Canvas2DLayerBridgeTest, SkipDirtyRectForSmallCanvas) {
  PaintFlags flags;
  std::unique_ptr<Canvas2DLayerBridge> bridge =
      MakeBridge(IntSize(100, 100), Canvas2DLayerBridge::kEnableAcceleration,
                 CanvasColorParams());
  bridge->FinalizeFrame();
  base::HistogramTester histogram_tester;
  cc::PaintCanvas* canvas = bridge->DrawingCanvas();
  canvas->drawRect(SkRect::MakeWH(100, 100), flags);
  DrawSomething(bridge.get());
  histogram_tester.ExpectTotalCount("Canvas.Repaint.Bounds.Percentage", 0);
  histogram_tester.ExpectTotalCount("Canvas.Repaint.Region.Percentage", 0);
}

// Test if we can correctly calculate dirty rect for region with complexity 1.
TEST_F(Canvas2DLayerBridgeTest, SmallDirtyRectCalculation) {
  PaintFlags flags;
  std::unique_ptr<Canvas2DLayerBridge> bridge =
      MakeBridge(IntSize(300, 300), Canvas2DLayerBridge::kEnableAcceleration,
                 CanvasColorParams());
  bridge->FinalizeFrame();
  base::HistogramTester histogram_tester;
  cc::PaintCanvas* canvas = bridge->DrawingCanvas();
  canvas->drawRect(SkRect::MakeWH(100, 100), flags);
  DrawSomething(bridge.get());
  // Dirty rect: (-1, -1, 102x102) & canvas size: 302x302. Dirty percentage:
  // (102x102)/(302x302) = 11. (1 pixel is added around the rect for anti-alias
  // effect.)
  histogram_tester.ExpectTotalCount("Canvas.Repaint.Bounds.Percentage", 1);
  histogram_tester.ExpectUniqueSample("Canvas.Repaint.Bounds.Percentage", 11,
                                      1);
  histogram_tester.ExpectTotalCount("Canvas.Repaint.Region.Percentage", 1);
  histogram_tester.ExpectUniqueSample("Canvas.Repaint.Region.Percentage", 11,
                                      1);
}

TEST_F(Canvas2DLayerBridgeTest, BigDirtyRectCalculation) {
  PaintFlags flags;
  std::unique_ptr<Canvas2DLayerBridge> bridge =
      MakeBridge(IntSize(300, 300), Canvas2DLayerBridge::kEnableAcceleration,
                 CanvasColorParams());
  bridge->FinalizeFrame();
  base::HistogramTester histogram_tester;
  cc::PaintCanvas* canvas = bridge->DrawingCanvas();
  canvas->drawRect(SkRect::MakeWH(300, 300), flags);
  DrawSomething(bridge.get());
  // Dirty rect: (-1, -1, 302x302) & canvas size: 302x302. Dirty percentage:
  // (302x302)/(302x302) = 100. (1 pixel is added around the rect for anti-alias
  // effect.)
  histogram_tester.ExpectTotalCount("Canvas.Repaint.Bounds.Percentage", 1);
  histogram_tester.ExpectUniqueSample("Canvas.Repaint.Bounds.Percentage", 100,
                                      1);
  histogram_tester.ExpectTotalCount("Canvas.Repaint.Region.Percentage", 1);
  histogram_tester.ExpectUniqueSample("Canvas.Repaint.Region.Percentage", 100,
                                      1);
}

// Test if we can correctly calculate dirty rect for region with complexity 2;
// where dirty bounds and dirty region have different areas.
TEST_F(Canvas2DLayerBridgeTest, TwoRegionDirtyRectCalculation) {
  PaintFlags flags;
  std::unique_ptr<Canvas2DLayerBridge> bridge =
      MakeBridge(IntSize(300, 300), Canvas2DLayerBridge::kEnableAcceleration,
                 CanvasColorParams());
  bridge->FinalizeFrame();
  base::HistogramTester histogram_tester;
  cc::PaintCanvas* canvas = bridge->DrawingCanvas();
  canvas->drawRect(SkRect::MakeWH(300, 30), flags);
  canvas->drawRect(SkRect::MakeWH(30, 300), flags);
  DrawSomething(bridge.get());
  // Dirty region: (-1, -1, 302x32) Union (-1, 31, 32x270) & canvas size:
  // 302x302. Dirty percentage: (302x31)/(31x271) = 20. (1 pixel is added
  // around the rect for anti-alias effect.)
  histogram_tester.ExpectTotalCount("Canvas.Repaint.Region.Percentage", 1);
  histogram_tester.ExpectUniqueSample("Canvas.Repaint.Region.Percentage", 20,
                                      1);
  // Dirty region: (-1, -1, 302x32) Union (-1, 31, 32x270) = (-1, -1, 302x302)
  // & canvas size: 302x302. Dirty percentage: (302x302)/(302x302) = 100. (1
  // pixel is added around the rect for anti-alias effect.)
  histogram_tester.ExpectTotalCount("Canvas.Repaint.Bounds.Percentage", 1);
  histogram_tester.ExpectUniqueSample("Canvas.Repaint.Bounds.Percentage", 100,
                                      1);
}

// Test dirty rect calculation for canvas with scale transforms.
TEST_F(Canvas2DLayerBridgeTest, TransformedCanvasDirtyRect) {
  PaintFlags flags;
  std::unique_ptr<Canvas2DLayerBridge> bridge =
      MakeBridge(IntSize(500, 500), Canvas2DLayerBridge::kEnableAcceleration,
                 CanvasColorParams());
  bridge->FinalizeFrame();
  base::HistogramTester histogram_tester;
  cc::PaintCanvas* canvas = bridge->DrawingCanvas();
  canvas->scale(0.5f, 0.5f);
  canvas->drawRect(SkRect::MakeWH(500, 500), flags);
  DrawSomething(bridge.get());
  // Dirty region: 252x252 (scale transform reduces the height and width by
  // half) & canvas size: 502x502, Dirty percentage: (252x252)/(502x502) = 25.
  histogram_tester.ExpectTotalCount("Canvas.Repaint.Region.Percentage", 1);
  histogram_tester.ExpectUniqueSample("Canvas.Repaint.Region.Percentage", 25,
                                      1);
  histogram_tester.ExpectTotalCount("Canvas.Repaint.Bounds.Percentage", 1);
  histogram_tester.ExpectUniqueSample("Canvas.Repaint.Bounds.Percentage", 25,
                                      1);
}

// Test dirty rect calculation for canvas with rotation transforms.
TEST_F(Canvas2DLayerBridgeTest, RotationCanvasDirtyRect) {
  PaintFlags flags;
  std::unique_ptr<Canvas2DLayerBridge> bridge =
      MakeBridge(IntSize(200, 600), Canvas2DLayerBridge::kEnableAcceleration,
                 CanvasColorParams());
  bridge->FinalizeFrame();
  base::HistogramTester histogram_tester;
  cc::PaintCanvas* canvas = bridge->DrawingCanvas();
  canvas->rotate(90);
  SkRect dirty_rect;
  dirty_rect.setXYWH(50, -100, 60, 60);
  canvas->drawRect(dirty_rect, flags);
  DrawSomething(bridge.get());
  // After rotation, the canvas is at (-600, 0, 600x200) at 90
  // degree. Dirty Region: 62x62 & Canvas size: 202x602, dirty percentage:
  // (62x62)/(202x602) = 3.
  histogram_tester.ExpectTotalCount("Canvas.Repaint.Region.Percentage", 1);
  histogram_tester.ExpectUniqueSample("Canvas.Repaint.Region.Percentage", 3, 1);
  histogram_tester.ExpectTotalCount("Canvas.Repaint.Bounds.Percentage", 1);
  histogram_tester.ExpectUniqueSample("Canvas.Repaint.Bounds.Percentage", 3, 1);
}

// Test dirty rect calculation for canvas with translation transforms.
TEST_F(Canvas2DLayerBridgeTest, TranslationCanvasDirtyRect) {
  PaintFlags flags;
  std::unique_ptr<Canvas2DLayerBridge> bridge =
      MakeBridge(IntSize(500, 500), Canvas2DLayerBridge::kEnableAcceleration,
                 CanvasColorParams());
  bridge->FinalizeFrame();
  base::HistogramTester histogram_tester;
  cc::PaintCanvas* canvas = bridge->DrawingCanvas();
  canvas->translate(20, 50);
  SkRect dirty_rect;
  dirty_rect.setXYWH(50, 70, 60, 60);
  canvas->drawRect(dirty_rect, flags);
  DrawSomething(bridge.get());
  // After translation, the canvas is at (20, 50, 500x500). Dirty
  // Region: 62x62 & Canvas size: 502x502, dirty percentage:
  // (62x62)/(502x502)=1.
  histogram_tester.ExpectTotalCount("Canvas.Repaint.Region.Percentage", 1);
  histogram_tester.ExpectUniqueSample("Canvas.Repaint.Region.Percentage", 1, 1);
  histogram_tester.ExpectTotalCount("Canvas.Repaint.Bounds.Percentage", 1);
  histogram_tester.ExpectUniqueSample("Canvas.Repaint.Bounds.Percentage", 1, 1);
}

// Test dirty rect calculation for canvas with clip rect.
TEST_F(Canvas2DLayerBridgeTest, ClipRectCanvasDirtyRect) {
  PaintFlags flags;
  std::unique_ptr<Canvas2DLayerBridge> bridge =
      MakeBridge(IntSize(500, 500), Canvas2DLayerBridge::kEnableAcceleration,
                 CanvasColorParams());
  bridge->FinalizeFrame();
  base::HistogramTester histogram_tester;
  cc::PaintCanvas* canvas = bridge->DrawingCanvas();
  canvas->clipRect(SkRect::MakeWH(100, 100));
  canvas->drawRect(SkRect::MakeWH(200, 200), flags);
  DrawSomething(bridge.get());
  // Dirty region: 102x102 (clip rect restrict the dirty to be in 102x202) &
  // canvas size: 502x502, dirty percentage:
  // (102x102)/(502x502) = 4.
  histogram_tester.ExpectTotalCount("Canvas.Repaint.Region.Percentage", 1);
  histogram_tester.ExpectUniqueSample("Canvas.Repaint.Region.Percentage", 4, 1);
  histogram_tester.ExpectTotalCount("Canvas.Repaint.Bounds.Percentage", 1);
  histogram_tester.ExpectUniqueSample("Canvas.Repaint.Bounds.Percentage", 4, 1);
}

// Test if we can correctly calculate dirty rect for canvas with paintrecord;
TEST_F(Canvas2DLayerBridgeTest, PaintRecordDirtyRect) {
  PaintFlags flags;
  std::unique_ptr<Canvas2DLayerBridge> bridge =
      MakeBridge(IntSize(500, 500), Canvas2DLayerBridge::kEnableAcceleration,
                 CanvasColorParams());
  bridge->FinalizeFrame();
  base::HistogramTester histogram_tester;
  cc::PaintCanvas* canvas = bridge->DrawingCanvas();
  PaintRecorder recorder;
  recorder.beginRecording(SkRect::MakeWH(50, 50))
      ->drawRect(SkRect::MakeWH(50, 50), flags);
  canvas->drawPicture(recorder.finishRecordingAsPicture());
  DrawSomething(bridge.get());
  // Dirty region: 52x52 &
  // canvas size: 502x502, dirty percentage: (52x52)/(502x502) = 1.
  histogram_tester.ExpectTotalCount("Canvas.Repaint.Region.Percentage", 1);
  histogram_tester.ExpectUniqueSample("Canvas.Repaint.Region.Percentage", 1, 1);
  histogram_tester.ExpectTotalCount("Canvas.Repaint.Bounds.Percentage", 1);
  histogram_tester.ExpectUniqueSample("Canvas.Repaint.Bounds.Percentage", 1, 1);
}

}  // namespace blink
