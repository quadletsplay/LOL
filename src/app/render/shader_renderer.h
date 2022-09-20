// Aseprite
// Copyright (C) 2022  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifndef APP_RENDER_SHADER_RENDERER_H_INCLUDED
#define APP_RENDER_SHADER_RENDERER_H_INCLUDED
#pragma once

#if SK_ENABLE_SKSL

#include "app/render/renderer.h"

#include "include/core/SkRefCnt.h"

class SkCanvas;
class SkRuntimeEffect;

namespace app {

  // Use SkSL to compose images with Skia shaders on the CPU (with the
  // SkSL VM) or GPU-accelerated (with native OpenGL/Metal/etc. shaders).
  class ShaderRenderer : public Renderer {
  public:
    ShaderRenderer();
    ~ShaderRenderer();

    const Properties& properties() const override { return m_properties; }

    void setRefLayersVisiblity(const bool visible) override;
    void setNonactiveLayersOpacity(const int opacity) override;
    void setNewBlendMethod(const bool newBlend) override;
    void setBgOptions(const render::BgOptions& bg) override;
    void setProjection(const render::Projection& projection) override;

    void setSelectedLayer(const doc::Layer* layer) override;
    void setPreviewImage(const doc::Layer* layer,
                         const doc::frame_t frame,
                         const doc::Image* image,
                         const doc::Tileset* tileset,
                         const gfx::Point& pos,
                         const doc::BlendMode blendMode) override;
    void removePreviewImage() override;
    void setExtraImage(render::ExtraType type,
                       const doc::Cel* cel,
                       const doc::Image* image,
                       const doc::BlendMode blendMode,
                       const doc::Layer* currentLayer,
                       const doc::frame_t currentFrame) override;
    void removeExtraImage() override;
    void setOnionskin(const render::OnionskinOptions& options) override;
    void disableOnionskin() override;

    void renderSprite(os::Surface* dstSurface,
                      const doc::Sprite* sprite,
                      const doc::frame_t frame,
                      const gfx::ClipF& area) override;
    void renderCheckeredBackground(os::Surface* dstSurface,
                                   const doc::Sprite* sprite,
                                   const gfx::Clip& area) override;
    void renderImage(doc::Image* dstImage,
                     const doc::Image* srcImage,
                     const doc::Palette* pal,
                     const int x,
                     const int y,
                     const int opacity,
                     const doc::BlendMode blendMode) override;

  private:
    void drawLayerGroup(SkCanvas* canvas,
                        const doc::Sprite* sprite,
                        const doc::LayerGroup* group,
                        const doc::frame_t frame,
                        const gfx::ClipF& area);

    Properties m_properties;
    render::BgOptions m_bgOptions;
    render::Projection m_proj;
    sk_sp<SkRuntimeEffect> m_bgEffect;
  };

} // namespace app

#endif // SK_ENABLE_SKSL

#endif