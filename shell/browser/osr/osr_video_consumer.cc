// Copyright (c) 2019 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/browser/osr/osr_video_consumer.h"

#include <cstring>
#include <memory>
#include <utility>

#include "base/logging.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "media/base/video_frame_metadata.h"
#include "media/capture/mojom/video_capture_types.mojom.h"
#include "shell/browser/osr/osr_render_widget_host_view.h"
#include "ui/gfx/skbitmap_operations.h"

namespace electron {

OffScreenVideoConsumer::OffScreenVideoConsumer(
    OffScreenRenderWidgetHostView* view,
    OnPaintCallback callback)
    : callback_(callback),
      view_(view),
      video_capturer_(view->CreateVideoCapturer()),
      weak_ptr_factory_(this) {
  video_capturer_->SetResolutionConstraints(view_->SizeInPixels(),
                                            view_->SizeInPixels(), true);
  video_capturer_->SetAutoThrottlingEnabled(false);
  video_capturer_->SetMinSizeChangePeriod(base::TimeDelta());
  video_capturer_->SetFormat(media::PIXEL_FORMAT_ARGB,
                             gfx::ColorSpace::CreateREC709());
  SetFrameRate(view_->GetFrameRate());
}

OffScreenVideoConsumer::~OffScreenVideoConsumer() = default;

void OffScreenVideoConsumer::SetActive(bool active) {
  if (active) {
    video_capturer_->Start(this);
  } else {
    video_capturer_->Stop();
  }
}

void OffScreenVideoConsumer::SetFrameRate(int frame_rate) {
  video_capturer_->SetMinCapturePeriod(base::TimeDelta::FromSeconds(1) /
                                       frame_rate);
}

void OffScreenVideoConsumer::SizeChanged() {
  video_capturer_->SetResolutionConstraints(view_->SizeInPixels(),
                                            view_->SizeInPixels(), true);
  video_capturer_->RequestRefreshFrame();
}

barmco::SHM* OffScreenVideoConsumer::GetExternalSharedMemory() const {
  return shm_external_.get();
}

void OffScreenVideoConsumer::OnFrameCaptured(
    base::ReadOnlySharedMemoryRegion data,
    ::media::mojom::VideoFrameInfoPtr info,
    const gfx::Rect& content_rect,
    mojo::PendingRemote<viz::mojom::FrameSinkVideoConsumerFrameCallbacks>
        callbacks) {
  if (!CheckContentRect(content_rect)) {
    gfx::Size view_size = view_->SizeInPixels();
    video_capturer_->SetResolutionConstraints(view_size, view_size, true);
    video_capturer_->RequestRefreshFrame();
    return;
  }

  mojo::Remote<viz::mojom::FrameSinkVideoConsumerFrameCallbacks>
      callbacks_remote(std::move(callbacks));

  if (!data.IsValid()) {
    callbacks_remote->Done();
    return;
  }
  base::ReadOnlySharedMemoryMapping mapping = data.Map();
  if (!mapping.IsValid()) {
    DLOG(ERROR) << "Shared memory mapping failed.";
    callbacks_remote->Done();
    return;
  }
  if (mapping.size() <
      media::VideoFrame::AllocationSize(info->pixel_format, info->coded_size)) {
    DLOG(ERROR) << "Shared memory size was less than expected.";
    callbacks_remote->Done();
    return;
  }

  // The SkBitmap's pixels will be marked as immutable, but the installPixels()
  // API requires a non-const pointer. So, cast away the const.
  void* const pixels = const_cast<void*>(mapping.memory());

  // Call installPixels() with a |releaseProc| that: 1) notifies the capturer
  // that this consumer has finished with the frame, and 2) releases the shared
  // memory mapping.
  struct FramePinner {
    // Keeps the shared memory that backs |frame_| mapped.
    base::ReadOnlySharedMemoryMapping mapping;
    // Prevents FrameSinkVideoCapturer from recycling the shared memory that
    // backs |frame_|.
    mojo::PendingRemote<viz::mojom::FrameSinkVideoConsumerFrameCallbacks>
        releaser;
  };

  SkBitmap bitmap;
  bitmap.installPixels(
      SkImageInfo::MakeN32(content_rect.width(), content_rect.height(),
                           kPremul_SkAlphaType),
      pixels,
      media::VideoFrame::RowBytes(media::VideoFrame::kARGBPlane,
                                  info->pixel_format, info->coded_size.width()),
      [](void* addr, void* context) {
        delete static_cast<FramePinner*>(context);
      },
      new FramePinner{std::move(mapping), callbacks_remote.Unbind()});
  bitmap.setImmutable();

  // barmco: expose-osr-frame-ext-shared-mem
  // shared memory section's start 1 byte represents transfer state
  // 0x00 mean not transferred (do not transfer bitmap to shared memory section)
  // 0xFF mean transferred (reset mark, and transfer bitmap to shared memory
  // section)

  if (shm_external_.get() == nullptr ||
      bitmap.computeByteSize() != shm_external_->Size() - 1) {
    LOG(INFO) << "shm_external refreshed: " << shm_external_->Path();

    shm_external_.reset(
        new barmco::SHM(base::NumberToString(base::RandUint64()) + "-osr",
                        bitmap.computeByteSize() + 1));
    shm_external_->Create();
    memset(shm_external_->Data(), 0xFF, 1);
  }

  if (CheckExternalSharedMemoryHasMarked()) {
    uint8_t* base = shm_external_->Data();
    memset(base, 0x00, 1);
    memcpy(static_cast<void*>(base + 1), pixels, bitmap.computeByteSize());
  }

  // barmco: expose-osr-frame-ext-shared-mem

  media::VideoFrameMetadata metadata;
  metadata.MergeInternalValuesFrom(info->metadata);
  gfx::Rect damage_rect;

  auto UPDATE_RECT = media::VideoFrameMetadata::CAPTURE_UPDATE_RECT;
  if (!metadata.GetRect(UPDATE_RECT, &damage_rect) || damage_rect.IsEmpty()) {
    damage_rect = content_rect;
  }

  callback_.Run(damage_rect, bitmap);
}

void OffScreenVideoConsumer::OnStopped() {}

void OffScreenVideoConsumer::OnLog(const std::string& message) {}

bool OffScreenVideoConsumer::CheckContentRect(const gfx::Rect& content_rect) {
  gfx::Size view_size = view_->SizeInPixels();
  gfx::Size content_size = content_rect.size();

  if (std::abs(view_size.width() - content_size.width()) > 2) {
    return false;
  }

  if (std::abs(view_size.height() - content_size.height()) > 2) {
    return false;
  }

  return true;
}

bool OffScreenVideoConsumer::CheckExternalSharedMemoryHasMarked() const {
  if (shm_external_.get() == nullptr)
    return true;

  return (*shm_external_->Data() & 0xFF) == 0xFF;
}

}  // namespace electron
