#include "camera_texture.h"

#include <cstring>

CameraTexture::CameraTexture(flutter::TextureRegistrar* registrar)
    : registrar_(registrar) {}

CameraTexture::~CameraTexture() {
  Unregister();
}

int64_t CameraTexture::Register() {
  texture_variant_ = std::make_unique<flutter::TextureVariant>(
      flutter::PixelBufferTexture(
          [this](size_t w, size_t h) -> const FlutterDesktopPixelBuffer* {
            return ObtainPixelBuffer(w, h);
          }));
  texture_id_ = registrar_->RegisterTexture(texture_variant_.get());
  return texture_id_;
}

void CameraTexture::Update(const uint8_t* bgra, int width, int height) {
  const size_t required = static_cast<size_t>(width) * height * 4;

  std::lock_guard<std::mutex> lock(mutex_);

  // Reallocate all three buffers when dimensions change.
  if (width != width_ || height != height_) {
    for (auto& buf : bufs_) {
      buf.resize(required);
    }
    width_ = width;
    height_ = height;
  }

  // Copy into write buffer.
  std::memcpy(bufs_[write_idx_].data(), bgra, required);

  // Swap write ↔ ready (capture thread owns both until this swap).
  std::swap(write_idx_, ready_idx_);
  has_new_frame_ = true;
}

const FlutterDesktopPixelBuffer* CameraTexture::ObtainPixelBuffer(
    size_t /*width*/, size_t /*height*/) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (width_ == 0 || height_ == 0) return nullptr;

  // Swap ready ↔ read if a new frame arrived.
  if (has_new_frame_) {
    std::swap(ready_idx_, read_idx_);
    has_new_frame_ = false;
  }

  pixel_buffer_.buffer = bufs_[read_idx_].data();
  pixel_buffer_.width = static_cast<size_t>(width_);
  pixel_buffer_.height = static_cast<size_t>(height_);
  pixel_buffer_.release_callback = nullptr;
  pixel_buffer_.release_context = nullptr;

  return &pixel_buffer_;
}

void CameraTexture::Unregister() {
  if (texture_id_ >= 0 && registrar_) {
    registrar_->UnregisterTexture(texture_id_);
    texture_id_ = -1;
  }
  texture_variant_.reset();
}
