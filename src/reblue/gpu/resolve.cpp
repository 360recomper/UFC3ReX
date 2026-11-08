/**
 * @file    gpu/resolve.cpp
 * @brief   EDRAM resolve emulation: surface-to-texture copy, the lazy resolve
 *          alias, and the fullscreen composite chain seeding.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/constant_buffers.h"
#include "gpu/frame.h"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>
#include <rex/cvar.h>

#include "core/profiling.h"

#include "core/logging.h"
#include "gpu/backend.h"
#include "gpu/format.h"
#if defined(SVR_NATIVE_RENDERER)
#include "ufc_resources.h"
#endif

namespace bd::gpu {

namespace {

inline const plume::RenderTexture *const kDepthFbKey =
    reinterpret_cast<const plume::RenderTexture *>(uintptr_t{1});

GuestTexture *ResolveSourceForFlagsLocked(VideoState &s, u32 flags) {
  if (flags & kResolveDepthStencil)
    return s.depth_stencil;
  if (s.render_target)
    return s.render_target;
  return s.back_buffer_surface;
}

bool CanAliasResolveLocked(const GuestTexture *src, const GuestTexture *dst) {
  return src && dst && src != dst && src->texture && dst->texture &&
         src->sampleCount == plume::RenderSampleCount::COUNT_1 &&
         src->width == dst->width && src->height == dst->height &&
         src->format == dst->format && dst->resolveScale == 1.0f &&
         dst->viewDimension !=
             plume::RenderTextureViewDimension::TEXTURE_CUBE &&
         dst->mipLevels <= 1 && src->descriptorIndex != kInvalidDescriptorIndex;
}

} // namespace

void DetachSourceSurfaceLocked(VideoState &s, GuestTexture *texture) {
  if (!texture || !texture->sourceSurface)
    return;
  GuestTexture *source = texture->sourceSurface;
  source->destinationTextures.erase(texture);
  texture->sourceSurface = nullptr;
  MarkAllTextureSlotsDirty();
}

bool CopySurfaceToTextureLocked(VideoState &s, GuestTexture *src,
                                GuestTexture *dst, const char *reason,
                                const ResolveRegion *region) {
  if (!dst || !dst->texture || !src || !src->texture || src == dst) {
    return false;
  }
  if (!s.ready)
    return false;
  if (dst->sampleCount != plume::RenderSampleCount::COUNT_1) {
    static std::atomic<u32> s_warn{0};
    if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
      BD_WARN("{}: MSAA destination {}x{} rejected (resolve targets must be "
              "single-sample)",
              reason, dst->width, dst->height);
    }
    return false;
  }

  BeginCommandList(s);
  if (!s.command_list_open)
    return false;

  s.draw_framebuffer_bound = false;

  const bool dims_match =
      src->width == dst->width && src->height == dst->height;
  const bool format_match = src->format == dst->format;
  const float resolve_scale = dst->resolveScale;
  const bool needs_scale = (resolve_scale != 1.0f);
  const bool dst_is_cube =
      dst->viewDimension == plume::RenderTextureViewDimension::TEXTURE_CUBE;

  const u32 dst_level = dst->resolveLevel;
  const u32 dst_face = dst_is_cube ? dst->resolveFace : 0u;
  const u32 level_width = std::max(1u, dst->width >> dst_level);
  const u32 level_height = std::max(1u, dst->height >> dst_level);
  // D3D12 copies depth only as whole subresources.
  const bool region_copyable =
      region && (!IsDepthFormat(dst->format) ||
                 (region->dst_x == 0 && region->dst_y == 0 && region->src.left == 0 &&
                  region->src.top == 0 && u32(region->src.right) == src->width &&
                  u32(region->src.bottom) == src->height && src->width == level_width &&
                  src->height == level_height));

  if (region_copyable && format_match && !needs_scale &&
      src->sampleCount == plume::RenderSampleCount::COUNT_1) {
    // Part of the surface, same format: a plain region copy.
    plume::RenderTextureBarrier pre[2] = {
        plume::RenderTextureBarrier(src->texture, plume::RenderTextureLayout::COPY_SOURCE),
        plume::RenderTextureBarrier(dst->texture, plume::RenderTextureLayout::COPY_DEST),
    };
    s.command_list->barriers(plume::RenderBarrierStage::COPY, pre, std::size(pre));
    src->layout = plume::RenderTextureLayout::COPY_SOURCE;
    dst->layout = plume::RenderTextureLayout::COPY_DEST;
    const plume::RenderBox box(region->src.left, region->src.top, region->src.right,
                               region->src.bottom);
    s.command_list->copyTextureRegion(
        plume::RenderTextureCopyLocation::Subresource(dst->texture, dst_level, dst_face),
        plume::RenderTextureCopyLocation::Subresource(src->texture, 0, 0), u32(region->dst_x),
        u32(region->dst_y), 0, &box);
  } else if (region && (dst_level != 0 || dst_is_cube)) {
    // The shader copy below can only write mip 0 of a 2D texture.
    static std::atomic<u32> s_warn{0};
    if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
      BD_WARN("{}: resolve to level {} face {} needs convert/scale (src={}x{} fmt={} dst fmt={} "
              "scale={}), skipping",
              reason, dst_level, dst_face, src->width, src->height, static_cast<int>(src->format),
              static_cast<int>(dst->format), resolve_scale);
    }
    return false;
  } else if (!region && dims_match && format_match && !needs_scale &&
      src->sampleCount == plume::RenderSampleCount::COUNT_1) {
    plume::RenderTextureBarrier pre[2] = {
        plume::RenderTextureBarrier(src->texture,
                                    plume::RenderTextureLayout::COPY_SOURCE),
        plume::RenderTextureBarrier(dst->texture,
                                    plume::RenderTextureLayout::COPY_DEST),
    };
    s.command_list->barriers(plume::RenderBarrierStage::COPY, pre,
                             std::size(pre));
    src->layout = plume::RenderTextureLayout::COPY_SOURCE;
    dst->layout = plume::RenderTextureLayout::COPY_DEST;
    if (dst_is_cube || dst->mipLevels > 1) {
      // CopyResource needs identical subresources on both sides, so by subresource here.
      const plume::RenderBox box(0, 0, i32(std::min(src->width, level_width)),
                                 i32(std::min(src->height, level_height)));
      s.command_list->copyTextureRegion(
          plume::RenderTextureCopyLocation::Subresource(dst->texture, dst_level, dst_face),
          plume::RenderTextureCopyLocation::Subresource(src->texture, 0, 0), 0, 0, 0, &box);
    } else {
      s.command_list->copyTexture(dst->texture, src->texture);
    }
  } else if (dst_is_cube) {
    static std::atomic<u32> s_warn{0};
    if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
      BD_WARN("{}: cube resolve dst needs convert/scale (src={}x{} fmt={} "
              "dst fmt={} scale={}), no per-face RTV, skipping",
              reason, src->width, src->height, static_cast<int>(src->format),
              static_cast<int>(dst->format), resolve_scale);
    }
    return false;
  } else {
    if (src->descriptorIndex == kInvalidDescriptorIndex) {
      BindTextureSRVLocked(s, src);
    }
    if (src->descriptorIndex == kInvalidDescriptorIndex) {
      static std::atomic<u32> s_warn{0};
      if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
        BD_ERROR("{}: source RT 0x{:x} not in bindless heap, can't "
                 "shader-resolve",
                 reason, reinterpret_cast<uintptr_t>(src));
      }
      return false;
    }

    const bool depth_dst = IsDepthFormat(dst->format);
    if (!depth_dst && !IsRenderTargetCapable(dst->format)) {
      static std::atomic<u32> s_warn{0};
      if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
        BD_ERROR("{}: dst format {} is not RT-capable, skipping shader "
                 "resolve (src={}x{} fmt={} dst={}x{} fmt={})",
                 reason, static_cast<int>(dst->format), src->width, src->height,
                 static_cast<int>(src->format), dst->width, dst->height,
                 static_cast<int>(dst->format));
      }
      return false;
    }

    const plume::RenderTexture *fb_key = depth_dst ? kDepthFbKey : nullptr;

    auto it = dst->framebuffers.find(fb_key);
    plume::RenderFramebuffer *dst_fb = nullptr;
    if (it != dst->framebuffers.end()) {
      dst_fb = it->second.get();
    } else {
      std::unique_ptr<plume::RenderFramebuffer> fb;
      if (depth_dst) {
        plume::RenderFramebufferDesc desc;
        desc.depthAttachment = dst->texture;
        desc.colorAttachmentsCount = 0;
        fb = s.device->createFramebuffer(desc);
      } else {
        const plume::RenderTexture *attachments[1] = {dst->texture};
        plume::RenderFramebufferDesc desc(attachments, 1);
        fb = s.device->createFramebuffer(desc);
      }
      if (!fb) {
        static std::atomic<u32> s_warn{0};
        if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
          BD_ERROR("{}: createFramebuffer failed (depth_dst={}), skipping "
                   "shader resolve (src={}x{} dst={}x{})",
                   reason, depth_dst ? 1 : 0, src->width, src->height,
                   dst->width, dst->height);
        }
        return false;
      }
      dst_fb = fb.get();
      dst->framebufferAttached = true;
      dst->framebuffers.emplace(fb_key, std::move(fb));
      s.framebuffer_owners.insert(dst);
    }

    const plume::RenderTextureLayout dst_write_layout =
        depth_dst ? plume::RenderTextureLayout::DEPTH_WRITE
                  : plume::RenderTextureLayout::COLOR_WRITE;
    const bool discard_dst =
        (dst->layout == plume::RenderTextureLayout::UNKNOWN);
    plume::RenderTextureBarrier pre[2] = {
        plume::RenderTextureBarrier(src->texture,
                                    plume::RenderTextureLayout::SHADER_READ),
        plume::RenderTextureBarrier(dst->texture, dst_write_layout),
    };
    s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, pre,
                             std::size(pre));
    src->layout = plume::RenderTextureLayout::SHADER_READ;
    dst->layout = dst_write_layout;
    if (discard_dst)
      s.command_list->discardTexture(dst->texture);

    // A region goes through the single-sample colour copy only (copy_color_ps's rect mode).
    const bool region_copy = region && !depth_dst &&
                             src->sampleCount == plume::RenderSampleCount::COUNT_1;
    if (region && !region_copy) {
      static std::atomic<u32> s_warn{0};
      if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8)
        BD_WARN("{}: partial resolve of a depth or multisampled surface copies the whole surface "
                "(src {}x{})", reason, src->width, src->height);
    }
    s.command_list->setFramebuffer(dst_fb);
    if (region_copy) {
      const i32 w = region->src.right - region->src.left;
      const i32 h = region->src.bottom - region->src.top;
      s.command_list->setViewports(plume::RenderViewport(
          float(region->dst_x), float(region->dst_y), float(w), float(h)));
      s.command_list->setScissors(plume::RenderRect(region->dst_x, region->dst_y,
                                                    region->dst_x + w, region->dst_y + h));
    } else {
      s.command_list->setViewports(
          plume::RenderViewport(0.0f, 0.0f, static_cast<float>(dst->width),
                                static_cast<float>(dst->height)));
      s.command_list->setScissors(plume::RenderRect(
          0, 0, static_cast<i32>(dst->width), static_cast<i32>(dst->height)));
    }
    plume::RenderPipeline *pipeline = nullptr;
    if (src->sampleCount != plume::RenderSampleCount::COUNT_1) {
      pipeline = GetOrCreateResolveMSAAPipeline(s, dst->format,
                                                src->sampleCount, depth_dst);
    } else if (depth_dst) {
      pipeline = GetOrCreateCopyDepthPipeline(s, dst->format);
    } else {
      pipeline = GetOrCreateResolvePipeline(s, dst->format);
    }
    if (!pipeline) {
      static std::atomic<u32> s_warn{0};
      if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
        BD_ERROR("{}: createGraphicsPipeline failed for dst format {}", reason,
                 static_cast<int>(dst->format));
      }
      return false;
    }
    s.command_list->setPipeline(pipeline);
    const u32 descriptor_index = src->descriptorIndex;

    u32 box_ratio = 0u;
    if (!depth_dst && src->sampleCount == plume::RenderSampleCount::COUNT_1 &&
        dst->width != 0u && dst->height != 0u && src->width > dst->width &&
        src->height > dst->height && (src->width % dst->width) == 0u &&
        (src->height % dst->height) == 0u &&
        (src->width / dst->width) == (src->height / dst->height)) {
      box_ratio = src->width / dst->width;
    }

    struct CopyPushConstants {
      u32 resourceDescriptorIndex;
      u32 resourceDescriptorIndex2;
      float param0;
      float param1;
    } push{descriptor_index, 0u, depth_dst ? 1.0f : resolve_scale,
           static_cast<float>(box_ratio)};
    if (region_copy) {
      // copy_color_ps rect mode: Param1 < 0, source texel = destination pixel + the offset packed
      // into the second (otherwise unused) descriptor slot as two signed 16-bit values.
      const i32 off_x = region->src.left - region->dst_x;
      const i32 off_y = region->src.top - region->dst_y;
      push.resourceDescriptorIndex2 = (u32(off_x) & 0xFFFFu) | (u32(off_y) << 16);
      push.param1 = -1.0f;
    }
    s.command_list->setGraphicsPushConstants(kCopyPushConstantRangeIndex, &push,
                                             kCopyPushConstantByteOffset,
                                             sizeof(push));
    s.command_list->drawInstanced(3, 1, 0, 0);

    s.command_list->setFramebuffer(nullptr);
    s.current_pso = nullptr;
    s.dirtyStates.pipelineState = true;
    s.draw_framebuffer_bound = false;
  }

  plume::RenderTextureBarrier post(dst->texture,
                                   plume::RenderTextureLayout::SHADER_READ);
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, &post, 1);
  dst->layout = plume::RenderTextureLayout::SHADER_READ;
  return true;
}

namespace {

bool ClearColorTargetLocked(VideoState &s, GuestTexture *dst,
                            plume::RenderColor color) {
  if (!dst || !dst->texture || !s.ready)
    return false;
  if (IsDepthFormat(dst->format) ||
      dst->sampleCount != plume::RenderSampleCount::COUNT_1)
    return false;
  BeginCommandList(s);
  if (!s.command_list_open)
    return false;
  s.draw_framebuffer_bound = false;

  plume::RenderFramebuffer *dst_fb = GetFramebuffer(s, dst, nullptr);
  if (!dst_fb)
    return false;

  plume::RenderTextureBarrier pre(dst->texture,
                                  plume::RenderTextureLayout::COLOR_WRITE);
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, &pre, 1);
  dst->layout = plume::RenderTextureLayout::COLOR_WRITE;
  s.command_list->setFramebuffer(dst_fb);
  s.command_list->setViewports(
      plume::RenderViewport(0.0f, 0.0f, static_cast<float>(dst->width),
                            static_cast<float>(dst->height)));
  s.command_list->setScissors(plume::RenderRect(
      0, 0, static_cast<i32>(dst->width), static_cast<i32>(dst->height)));
  s.command_list->clearColor(0, color);
  s.command_list->setFramebuffer(nullptr);
  s.draw_framebuffer_bound = false;

  plume::RenderTextureBarrier post(dst->texture,
                                   plume::RenderTextureLayout::SHADER_READ);
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, &post, 1);
  dst->layout = plume::RenderTextureLayout::SHADER_READ;
  return true;
}

} // namespace

void Video::ClearTexture(GuestTexture *texture, u32 color_argb) {
  if (!texture || !texture->texture)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  // A lazily resolved texture still reads its source surface: the fill has to win.
  DetachSourceSurfaceLocked(s, texture);
  ClearColorTargetLocked(s, texture, ArgbToRenderColor(color_argb));
}

bool MaterializeOutboundLocked(VideoState &s, GuestTexture *source,
                               bool aliasable_only) {
  if (!source || source->destinationTextures.empty())
    return false;
  bool recorded = false;
  std::vector<GuestTexture *> destinations(source->destinationTextures.begin(),
                                           source->destinationTextures.end());
  for (GuestTexture *dst : destinations) {
    if (!dst || dst == source || dst->sourceSurface != source)
      continue;
    if (dst->pendingDestroy) {
      DetachSourceSurfaceLocked(s, dst);
      continue;
    }
    const bool one_to_one =
        source->width == dst->width && source->height == dst->height &&
        source->format == dst->format &&
        source->sampleCount == plume::RenderSampleCount::COUNT_1 &&
        dst->resolveScale == 1.0f &&
        dst->viewDimension != plume::RenderTextureViewDimension::TEXTURE_CUBE;
    if (aliasable_only && !one_to_one)
      continue;
    if (CopySurfaceToTextureLocked(s, source, dst, "Materialize")) {
      recorded = true;
      DetachSourceSurfaceLocked(s, dst);
    }
  }
  return recorded;
}

void MaterializeInboundLocked(VideoState &s, GuestTexture *dst) {
  if (!dst || !dst->sourceSurface || dst->sourceSurface == dst)
    return;
  if (CopySurfaceToTextureLocked(s, dst->sourceSurface, dst, "Materialize")) {
    DetachSourceSurfaceLocked(s, dst);
  }
}

bool FullscreenChainClassLocked(const VideoState &s, const GuestTexture *t) {
  const GuestTexture *bb = s.back_buffer_surface;
  if (!t || !bb || t == bb || IsDepthFormat(t->format) || t->reflection)
    return false;
  constexpr u32 kDesignWidth = 1280u;
#if defined(SVR_NATIVE_RENDERER)
  // Host targets are scaled, the back buffer placeholder isn't: at 2x a half-size 640x360 target
  // is 1280 wide and would pass as full screen.
  const u32 min_full_width =
      SvrWholeScaleOnly() ? (bb->width > kDesignWidth ? kDesignWidth : bb->width)
                          : u32(SvrScalePx(i32(kDesignWidth), nullptr));
#else
  const u32 min_full_width =
      bb->width > kDesignWidth ? kDesignWidth : bb->width;
#endif
  if (t->width < min_full_width)
    return false;
  const u64 a = u64(t->width) * bb->height;
  const u64 b = u64(t->height) * bb->width;
  const u64 diff = a > b ? a - b : b - a;
  return diff * 100ull <= b * 3ull;
}

void Video::TrackResolveSource(u32 flags, GuestTexture *dst, u32 level,
                               u32 face) {
  if (!dst || !dst->texture)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!s.ready)
    return;
  dst->resolveLevel = level;
  dst->resolveFace = face;
  const int exp_bias = static_cast<i32>(flags) >> kResolveExponentBiasShift;
  dst->resolveScale = static_cast<float>(std::ldexp(1.0, exp_bias));
  GuestTexture *src = ResolveSourceForFlagsLocked(s, flags);
  if (!src || !src->texture || src == dst) {
    DetachSourceSurfaceLocked(s, dst);
    return;
  }
  if (dst->sourceSurface && dst->sourceSurface != src)
    dst->sourceSurface->destinationTextures.erase(dst);
  if (dst->sourceSurface != src)
    MarkAllTextureSlotsDirty();
  dst->sourceSurface = src;
  src->destinationTextures.insert(dst);
  BindTextureSRVLocked(s, src);
}

namespace {

void NoteTileContentLocked(VideoState &s, GuestTexture *dst) {
  if (!dst || !dst->texture || !FullscreenChainClassLocked(s, dst))
    return;
  s.fullscreen_chain_head = dst;
}

} // namespace

bool Video::ResolveRegionToTexture(u32 flags, GuestTexture *dst, const plume::RenderRect &src_rect,
                                   i32 dst_x, i32 dst_y, u32 level, u32 face) {
  if (!dst || !dst->texture)
    return false;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!s.ready)
    return false;
  GuestTexture *src = ResolveSourceForFlagsLocked(s, flags);
  if (!src || !src->texture || src == dst)
    return false;
  // A subresource dst doesn't have is an invalid copy, and that takes the device down.
  const bool dst_is_cube = dst->viewDimension == plume::RenderTextureViewDimension::TEXTURE_CUBE;
  if (level >= std::max(dst->mipLevels, 1u) || face >= (dst_is_cube ? 6u : 1u)) {
    static std::atomic<u32> s_warn{0};
    if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
      BD_WARN("ResolveRegion: dst {}x{} levels {} view {} has no level {} face {}, skipping",
              dst->width, dst->height, dst->mipLevels, static_cast<int>(dst->viewDimension), level,
              face);
    }
    return false;
  }
  // Whatever an earlier whole-surface resolve still owes dst is copied first, to the level it was
  // meant for: this one only rewrites part of it.
  MaterializeInboundLocked(s, dst);
  dst->resolveLevel = level;
  dst->resolveFace = face;
  const i32 level_width = i32(std::max(1u, dst->width >> level));
  const i32 level_height = i32(std::max(1u, dst->height >> level));
  // Clamp to both textures (UFC resolves with a negative top-left in CAF, NR-2 #9).
  ResolveRegion region{src_rect, dst_x, dst_y};
  if (region.src.left < 0) {
    region.dst_x -= region.src.left;
    region.src.left = 0;
  }
  if (region.src.top < 0) {
    region.dst_y -= region.src.top;
    region.src.top = 0;
  }
  if (region.dst_x < 0) {
    region.src.left -= region.dst_x;
    region.dst_x = 0;
  }
  if (region.dst_y < 0) {
    region.src.top -= region.dst_y;
    region.dst_y = 0;
  }
  region.src.right = std::min<i32>({region.src.right, i32(src->width),
                                    region.src.left + level_width - region.dst_x});
  region.src.bottom = std::min<i32>({region.src.bottom, i32(src->height),
                                     region.src.top + level_height - region.dst_y});
  if (region.src.right <= region.src.left || region.src.bottom <= region.src.top)
    return false;
  const int exp_bias = static_cast<i32>(flags) >> kResolveExponentBiasShift;
  dst->resolveScale = static_cast<float>(std::ldexp(1.0, exp_bias));
  if (!CopySurfaceToTextureLocked(s, src, dst, "ResolveRegion", &region))
    return false;
  s.last_resolved_dst = dst;
  DetachSourceSurfaceLocked(s, dst);
  return true;
}

namespace {

// One small copy-only texture per format and size, for FillHalfPixelGap's column / row.
GuestTexture *GapScratchLocked(VideoState &s, plume::RenderFormat format, u32 width, u32 height) {
  static std::unordered_map<u64, std::unique_ptr<GuestTexture>> scratch;
  const u64 key = u64(format) | (u64(width) << 16) | (u64(height) << 40);
  if (auto it = scratch.find(key); it != scratch.end())
    return it->second.get();
  plume::RenderTextureDesc desc;
  desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
  desc.width = width;
  desc.height = height;
  desc.depth = 1;
  desc.mipLevels = 1;
  desc.arraySize = 1;
  desc.format = format;
  auto holder = CreateHostTexture(s.device.get(), desc, "half-pixel-gap-scratch");
  if (!holder)
    return nullptr;
  auto t = std::make_unique<GuestTexture>();
  t->textureHolder = std::move(holder);
  t->texture = t->textureHolder.get();
  t->width = width;
  t->height = height;
  t->mipLevels = 1;
  t->format = format;
  t->viewDimension = plume::RenderTextureViewDimension::TEXTURE_2D;
  return scratch.emplace(key, std::move(t)).first->second.get();
}

}  // namespace

void Video::FillHalfPixelGap(const plume::RenderRect &rect, u32 gap) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  GuestTexture *rt = s.render_target;
  // A negative top-left (CAF composites, NR-2 #9) means the drawn area doesn't start at the rect.
  if (!gap || rect.left < 0 || rect.top < 0 || !s.ready || !rt || !rt->texture ||
      IsDepthFormat(rt->format) ||
      rt->sampleCount != plume::RenderSampleCount::COUNT_1 ||
      rt->viewDimension == plume::RenderTextureViewDimension::TEXTURE_CUBE)
    return;
  const i32 left = rect.left, top = rect.top;
  const i32 right = std::min(rect.right, i32(rt->width));
  const i32 bottom = std::min(rect.bottom, i32(rt->height));
  // The first covered column / row must be inside the rect.
  if (right - left <= i32(gap) || bottom - top <= i32(gap))
    return;
  BeginCommandList(s);
  if (!s.command_list_open)
    return;
  const u32 w = u32(right - left), h = u32(bottom - top);
  GuestTexture *column = GapScratchLocked(s, rt->format, 1, h);
  GuestTexture *row = GapScratchLocked(s, rt->format, w, 1);
  if (!column || !row)
    return;
  s.draw_framebuffer_bound = false;
  const auto copy = [&](GuestTexture *dst, u32 dx, u32 dy, GuestTexture *src, const plume::RenderBox &box) {
    plume::RenderTextureBarrier pre[2] = {
        plume::RenderTextureBarrier(src->texture, plume::RenderTextureLayout::COPY_SOURCE),
        plume::RenderTextureBarrier(dst->texture, plume::RenderTextureLayout::COPY_DEST),
    };
    s.command_list->barriers(plume::RenderBarrierStage::COPY, pre, std::size(pre));
    src->layout = plume::RenderTextureLayout::COPY_SOURCE;
    dst->layout = plume::RenderTextureLayout::COPY_DEST;
    s.command_list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(dst->texture, 0, 0),
                                      plume::RenderTextureCopyLocation::Subresource(src->texture, 0, 0),
                                      dx, dy, 0, &box);
  };
  // Columns first, then rows from the filled column, so the corner gets the first covered texel.
  const i32 cx = left + i32(gap), ry = top + i32(gap);
  copy(column, 0, 0, rt, plume::RenderBox(cx, top, cx + 1, bottom));
  for (u32 i = 0; i < gap; ++i)
    copy(rt, u32(left) + i, u32(top), column, plume::RenderBox(0, 0, 1, i32(h)));
  copy(row, 0, 0, rt, plume::RenderBox(left, ry, right, ry + 1));
  for (u32 i = 0; i < gap; ++i)
    copy(rt, u32(left), u32(top) + i, row, plume::RenderBox(0, 0, i32(w), 1));
  static std::atomic<u32> s_logs{0};
  if (s_logs.fetch_add(1, std::memory_order_relaxed) < 4)
    BD_INFO("[nrce1] half-pixel gap: {} host px filled at the left/top of {},{}-{},{} of {}x{} f{}", gap,
            left, top, right, bottom, rt->width, rt->height, u32(rt->format));
}

void Video::ResolveRtToTexture(GuestTexture *dst) {
  BD_CPU_ZONE("ResolveRtToTexture");
  if (!dst || !dst->texture)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!s.ready)
    return;
  GuestTexture *src = dst->sourceSurface;
  if (!src || src == dst || !src->texture)
    return;

  if (CanAliasResolveLocked(src, dst)) {
    s.last_resolved_dst = dst;
    NoteTileContentLocked(s, dst);
    s.draw_framebuffer_bound = false;
    return;
  }

  if (CopySurfaceToTextureLocked(s, src, dst, "Resolve")) {
    s.last_resolved_dst = dst;
    NoteTileContentLocked(s, dst);
    DetachSourceSurfaceLocked(s, dst);
  }
}

} // namespace bd::gpu
