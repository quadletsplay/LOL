// Aseprite
// Copyright (C) 2017-2018  David Capello
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "app/load_matrix.h"

#include "app/context.h"
#include "app/doc.h"
#include "app/file/file.h"
#include "doc/layer.h"
#include "doc/sprite.h"
#include "render/dithering_matrix.h"

namespace app {

bool load_dithering_matrix_from_sprite(
  const std::string& filename,
  render::DitheringMatrix& matrix)
{
  std::unique_ptr<Doc> doc(load_document(nullptr, filename));
  if (!doc)
    return false;

  doc::Sprite* spr = doc->sprite();
  const doc::Layer* lay = (spr && spr->root() ? spr->root()->firstLayer():
                                                nullptr);
  const doc::Image* img = (lay && lay->cel(0) ? lay->cel(0)->image():
                                                nullptr);
  if (img) {
    const int w = spr->width();
    const int h = spr->height();
    matrix = render::DitheringMatrix(h, w, 0x1000000);
    for (int i=0; i<h; ++i)
      for (int j=0; j<w; ++j) {
        color_t px = img->getPixel(i, j);
        matrix(i, j) = rgba_getr(px) << 16 | rgba_getg(px) << 8 | rgba_getb(px);
      }
  }
  else {
    matrix = render::DitheringMatrix();
  }

  return true;
}

} // namespace app
