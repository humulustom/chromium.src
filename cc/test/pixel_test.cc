// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/test/pixel_test.h"

#include <memory>
#include <utility>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/command_line.h"
#include "base/memory/read_only_shared_memory_region.h"
#include "base/memory/shared_memory_mapping.h"
#include "base/path_service.h"
#include "base/run_loop.h"
#include "base/test/scoped_feature_list.h"
#include "base/threading/thread_task_runner_handle.h"
#include "build/build_config.h"
#include "cc/base/switches.h"
#include "cc/raster/raster_buffer_provider.h"
#include "cc/test/fake_output_surface_client.h"
#include "cc/test/pixel_test_output_surface.h"
#include "cc/test/pixel_test_utils.h"
#include "components/viz/client/client_resource_provider.h"
#include "components/viz/common/frame_sinks/begin_frame_source.h"
#include "components/viz/common/frame_sinks/copy_output_request.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "components/viz/common/quads/compositor_frame_metadata.h"
#include "components/viz/common/resources/bitmap_allocation.h"
#include "components/viz/common/resources/shared_bitmap.h"
#include "components/viz/service/display/display_resource_provider.h"
#include "components/viz/service/display/gl_renderer.h"
#include "components/viz/service/display/output_surface_client.h"
#include "components/viz/service/display/software_output_device.h"
#include "components/viz/service/display/software_renderer.h"
#include "components/viz/service/display_embedder/skia_output_surface_dependency_impl.h"
#include "components/viz/service/display_embedder/skia_output_surface_impl.h"
#include "components/viz/service/gl/gpu_service_impl.h"
#include "components/viz/test/paths.h"
#include "components/viz/test/test_in_process_context_provider.h"
#include "components/viz/test/test_shared_bitmap_manager.h"
#include "gpu/command_buffer/client/gles2_interface.h"
#include "gpu/command_buffer/client/shared_memory_limits.h"
#include "gpu/command_buffer/service/gpu_switches.h"
#include "gpu/command_buffer/service/service_utils.h"
#include "gpu/config/gpu_feature_type.h"
#include "gpu/config/gpu_finch_features.h"
#include "gpu/config/gpu_info.h"
#include "gpu/ipc/gpu_in_process_thread_service.h"
#include "gpu/ipc/service/gpu_memory_buffer_factory.h"
#include "services/viz/privileged/mojom/gl/gpu_host.mojom.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace cc {

PixelTest::PixelTest(bool enable_vulkan)
    : device_viewport_size_(gfx::Size(200, 200)),
      disable_picture_quad_image_filtering_(false),
      output_surface_client_(std::make_unique<FakeOutputSurfaceClient>()) {
  // Keep texture sizes exactly matching the bounds of the RenderPass to avoid
  // floating point badness in texcoords.
  renderer_settings_.dont_round_texture_sizes_for_pixel_tests = true;
  if (enable_vulkan) {
    auto* command_line = base::CommandLine::ForCurrentProcess();
    bool use_gpu = command_line->HasSwitch(::switches::kUseGpuInTests);
    command_line->AppendSwitchASCII(
        ::switches::kUseVulkan,
        use_gpu ? ::switches::kVulkanImplementationNameNative
                : ::switches::kVulkanImplementationNameSwiftshader);
    scoped_feature_list_ = std::make_unique<base::test::ScopedFeatureList>();
    scoped_feature_list_->InitAndEnableFeature(features::kVulkan);
  }
}

PixelTest::~PixelTest() = default;

bool PixelTest::RunPixelTest(viz::RenderPassList* pass_list,
                             const base::FilePath& ref_file,
                             const PixelComparator& comparator) {
  return RunPixelTestWithReadbackTarget(pass_list, pass_list->back().get(),
                                        ref_file, comparator);
}

bool PixelTest::RunPixelTestWithReadbackTarget(
    viz::RenderPassList* pass_list,
    viz::RenderPass* target,
    const base::FilePath& ref_file,
    const PixelComparator& comparator) {
  return RunPixelTestWithReadbackTargetAndArea(
      pass_list, target, ref_file, comparator, nullptr);
}

bool PixelTest::RunPixelTestWithReadbackTargetAndArea(
    viz::RenderPassList* pass_list,
    viz::RenderPass* target,
    const base::FilePath& ref_file,
    const PixelComparator& comparator,
    const gfx::Rect* copy_rect) {
  base::RunLoop run_loop;

  std::unique_ptr<viz::CopyOutputRequest> request =
      std::make_unique<viz::CopyOutputRequest>(
          viz::CopyOutputRequest::ResultFormat::RGBA_BITMAP,
          base::BindOnce(&PixelTest::ReadbackResult, base::Unretained(this),
                         run_loop.QuitClosure()));
  if (copy_rect)
    request->set_area(*copy_rect);
  target->copy_requests.push_back(std::move(request));

  if (software_renderer_) {
    software_renderer_->SetDisablePictureQuadImageFiltering(
        disable_picture_quad_image_filtering_);
  }

  renderer_->DecideRenderPassAllocationsForFrame(*pass_list);
  float device_scale_factor = 1.f;
  renderer_->DrawFrame(pass_list, device_scale_factor, device_viewport_size_);

  // Wait for the readback to complete.
  if (output_surface_->context_provider())
    output_surface_->context_provider()->ContextGL()->Finish();
  run_loop.Run();

  return PixelsMatchReference(ref_file, comparator);
}

bool PixelTest::RunPixelTest(viz::RenderPassList* pass_list,
                             std::vector<SkColor>* ref_pixels,
                             const PixelComparator& comparator) {
  base::RunLoop run_loop;
  viz::RenderPass* target = pass_list->back().get();

  std::unique_ptr<viz::CopyOutputRequest> request =
      std::make_unique<viz::CopyOutputRequest>(
          viz::CopyOutputRequest::ResultFormat::RGBA_BITMAP,
          base::BindOnce(&PixelTest::ReadbackResult, base::Unretained(this),
                         run_loop.QuitClosure()));
  target->copy_requests.push_back(std::move(request));

  if (software_renderer_) {
    software_renderer_->SetDisablePictureQuadImageFiltering(
        disable_picture_quad_image_filtering_);
  }

  renderer_->DecideRenderPassAllocationsForFrame(*pass_list);
  float device_scale_factor = 1.f;
  renderer_->DrawFrame(pass_list, device_scale_factor, device_viewport_size_);

  // Wait for the readback to complete.
  if (output_surface_->context_provider())
    output_surface_->context_provider()->ContextGL()->Finish();
  run_loop.Run();

  // Need to wrap |ref_pixels| in a SkBitmap.
  DCHECK_EQ(ref_pixels->size(), static_cast<size_t>(result_bitmap_->width() *
                                                    result_bitmap_->height()));
  SkBitmap ref_pixels_bitmap;
  ref_pixels_bitmap.installPixels(
      SkImageInfo::MakeN32Premul(result_bitmap_->width(),
                                 result_bitmap_->height()),
      ref_pixels->data(), result_bitmap_->width() * sizeof(SkColor));
  bool result = comparator.Compare(*result_bitmap_, ref_pixels_bitmap);
  if (!result) {
    std::string res_bmp_data_url = GetPNGDataUrl(*result_bitmap_);
    std::string ref_bmp_data_url = GetPNGDataUrl(ref_pixels_bitmap);
    LOG(ERROR) << "Pixels do not match!";
    LOG(ERROR) << "Actual: " << res_bmp_data_url;
    LOG(ERROR) << "Expected: " << ref_bmp_data_url;
  }
  return result;
}

void PixelTest::ReadbackResult(base::OnceClosure quit_run_loop,
                               std::unique_ptr<viz::CopyOutputResult> result) {
  ASSERT_FALSE(result->IsEmpty());
  EXPECT_EQ(result->format(), viz::CopyOutputResult::Format::RGBA_BITMAP);
  result_bitmap_ = std::make_unique<SkBitmap>(result->AsSkBitmap());
  EXPECT_TRUE(result_bitmap_->readyToDraw());
  std::move(quit_run_loop).Run();
}

bool PixelTest::PixelsMatchReference(const base::FilePath& ref_file,
                                     const PixelComparator& comparator) {
  base::FilePath test_data_dir;
  if (!base::PathService::Get(viz::Paths::DIR_TEST_DATA, &test_data_dir))
    return false;

  // If this is false, we didn't set up a readback on a render pass.
  if (!result_bitmap_)
    return false;

  base::CommandLine* cmd = base::CommandLine::ForCurrentProcess();
  if (cmd->HasSwitch(switches::kCCRebaselinePixeltests))
    return WritePNGFile(*result_bitmap_, test_data_dir.Append(ref_file), true);

  return MatchesPNGFile(
      *result_bitmap_, test_data_dir.Append(ref_file), comparator);
}

base::WritableSharedMemoryMapping PixelTest::AllocateSharedBitmapMemory(
    const viz::SharedBitmapId& id,
    const gfx::Size& size) {
  base::MappedReadOnlyRegion shm =
      viz::bitmap_allocation::AllocateSharedBitmap(size, viz::RGBA_8888);
  this->shared_bitmap_manager_->ChildAllocatedSharedBitmap(shm.region.Map(),
                                                           id);
  return std::move(shm.mapping);
}

viz::ResourceId PixelTest::AllocateAndFillSoftwareResource(
    const gfx::Size& size,
    const SkBitmap& source) {
  viz::SharedBitmapId shared_bitmap_id = viz::SharedBitmap::GenerateId();
  base::WritableSharedMemoryMapping mapping =
      AllocateSharedBitmapMemory(shared_bitmap_id, size);

  SkImageInfo info = SkImageInfo::MakeN32Premul(size.width(), size.height());
  source.readPixels(info, mapping.memory(), info.minRowBytes(), 0, 0);

  return child_resource_provider_->ImportResource(
      viz::TransferableResource::MakeSoftware(shared_bitmap_id, size,
                                              viz::RGBA_8888),
      viz::SingleReleaseCallback::Create(base::DoNothing()));
}

void PixelTest::SetUpGLWithoutRenderer(bool flipped_output_surface) {
  enable_pixel_output_ = std::make_unique<gl::DisableNullDrawGLBindings>();

  auto context_provider =
      base::MakeRefCounted<viz::TestInProcessContextProvider>(
          /*enable_oop_rasterization=*/false, /*support_locking=*/false);
  gpu::ContextResult result = context_provider->BindToCurrentThread();
  DCHECK_EQ(result, gpu::ContextResult::kSuccess);
  output_surface_ = std::make_unique<PixelTestOutputSurface>(
      std::move(context_provider), flipped_output_surface);
  output_surface_->BindToClient(output_surface_client_.get());

  shared_bitmap_manager_ = std::make_unique<viz::TestSharedBitmapManager>();
  resource_provider_ = std::make_unique<viz::DisplayResourceProvider>(
      viz::DisplayResourceProvider::kGpu, output_surface_->context_provider(),
      shared_bitmap_manager_.get());

  child_context_provider_ =
      base::MakeRefCounted<viz::TestInProcessContextProvider>(
          /*enable_oop_rasterization=*/false, /*support_locking=*/false);
  result = child_context_provider_->BindToCurrentThread();
  DCHECK_EQ(result, gpu::ContextResult::kSuccess);
  child_resource_provider_ = std::make_unique<viz::ClientResourceProvider>();
}

void PixelTest::SetUpGLRenderer(bool flipped_output_surface) {
  SetUpGLWithoutRenderer(flipped_output_surface);
  renderer_ = std::make_unique<viz::GLRenderer>(
      &renderer_settings_, output_surface_.get(), resource_provider_.get(),
      nullptr, base::ThreadTaskRunnerHandle::Get());
  renderer_->Initialize();
  renderer_->SetVisible(true);
}

void PixelTest::SetUpSkiaRenderer(bool flipped_output_surface) {
  enable_pixel_output_ = std::make_unique<gl::DisableNullDrawGLBindings>();
  // Set up the GPU service.
  gpu_service_holder_ = viz::TestGpuServiceHolder::GetInstance();

  // Set up the skia renderer.
  output_surface_ = viz::SkiaOutputSurfaceImpl::Create(
      std::make_unique<viz::SkiaOutputSurfaceDependencyImpl>(
          gpu_service(), gpu::kNullSurfaceHandle),
      renderer_settings_);
  output_surface_->BindToClient(output_surface_client_.get());
  static_cast<viz::SkiaOutputSurfaceImpl*>(output_surface_.get())
      ->SetCapabilitiesForTesting(flipped_output_surface);
  resource_provider_ = std::make_unique<viz::DisplayResourceProvider>(
      viz::DisplayResourceProvider::kGpu,
      /*compositor_context_provider=*/nullptr,
      /*shared_bitmap_manager=*/nullptr);
  renderer_ = std::make_unique<viz::SkiaRenderer>(
      &renderer_settings_, output_surface_.get(), resource_provider_.get(),
      nullptr, static_cast<viz::SkiaOutputSurface*>(output_surface_.get()),
      viz::SkiaRenderer::DrawMode::DDL);
  renderer_->Initialize();
  renderer_->SetVisible(true);

  // Set up the client side context provider, etc
  child_context_provider_ =
      base::MakeRefCounted<viz::TestInProcessContextProvider>(
          /*enable_oop_rasterization=*/false, /*support_locking=*/false);
  gpu::ContextResult result = child_context_provider_->BindToCurrentThread();
  DCHECK_EQ(result, gpu::ContextResult::kSuccess);
  child_resource_provider_ = std::make_unique<viz::ClientResourceProvider>();
}

void PixelTest::TearDown() {
  // Tear down the client side context provider, etc.
  child_resource_provider_->ShutdownAndReleaseAllResources();
  child_resource_provider_.reset();
  child_context_provider_.reset();

  // Tear down the skia renderer.
  renderer_.reset();
  resource_provider_.reset();
  output_surface_.reset();
}

void PixelTest::EnableExternalStencilTest() {
  static_cast<PixelTestOutputSurface*>(output_surface_.get())
      ->set_has_external_stencil_test(true);
}

void PixelTest::SetUpSoftwareRenderer() {
  output_surface_.reset(new PixelTestOutputSurface(
      std::make_unique<viz::SoftwareOutputDevice>()));
  output_surface_->BindToClient(output_surface_client_.get());
  shared_bitmap_manager_ = std::make_unique<viz::TestSharedBitmapManager>();
  resource_provider_ = std::make_unique<viz::DisplayResourceProvider>(
      viz::DisplayResourceProvider::kSoftware, nullptr,
      shared_bitmap_manager_.get());
  child_resource_provider_ = std::make_unique<viz::ClientResourceProvider>();

  auto renderer = std::make_unique<viz::SoftwareRenderer>(
      &renderer_settings_, output_surface_.get(), resource_provider_.get(),
      nullptr);
  software_renderer_ = renderer.get();
  renderer_ = std::move(renderer);
  renderer_->Initialize();
  renderer_->SetVisible(true);
}

}  // namespace cc
