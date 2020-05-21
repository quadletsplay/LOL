// Aseprite Document Library
// Copyright (c) 2020 Igara Studio S.A.
// Copyright (c) 2001-2016 David Capello
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifndef DOC_COLOR_H_INCLUDED
#define DOC_COLOR_H_INCLUDED
#pragma once

#include "base/ints.h"

#include <algorithm>

namespace doc {

  // The greatest int type to storage a color for an image in the
  // available pixel formats.
  typedef uint32_t color_t;

  //////////////////////////////////////////////////////////////////////
  // RGBA

  const uint32_t rgba_r_shift = 0;
  const uint32_t rgba_g_shift = 8;
  const uint32_t rgba_b_shift = 16;
  const uint32_t rgba_a_shift = 24;

  const uint32_t rgba_r_mask = 0x000000ff;
  const uint32_t rgba_g_mask = 0x0000ff00;
  const uint32_t rgba_b_mask = 0x00ff0000;
  const uint32_t rgba_rgb_mask = 0x00ffffff;
  const uint32_t rgba_a_mask = 0xff000000;

  inline uint8_t rgba_getr(uint32_t c) {
    return (c >> rgba_r_shift) & 0xff;
  }

  inline uint8_t rgba_getg(uint32_t c) {
    return (c >> rgba_g_shift) & 0xff;
  }

  inline uint8_t rgba_getb(uint32_t c) {
    return (c >> rgba_b_shift) & 0xff;
  }

  inline uint8_t rgba_geta(uint32_t c) {
    return (c >> rgba_a_shift) & 0xff;
  }

  inline uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((r << rgba_r_shift) |
            (g << rgba_g_shift) |
            (b << rgba_b_shift) |
            (a << rgba_a_shift));
  }

  inline int rgb_luma(int r, int g, int b) {
    // gamma correction of 2.2 would be ideal but 2.0 is way faster
    return (unsigned) (r*r*13933 + g*g*46871 + b*b*4732) >> 24;
  }

  inline uint8_t rgba_luma(uint32_t c) {
    return rgb_luma(rgba_getr(c), rgba_getg(c), rgba_getb(c));
  }

  //////////////////////////////////////////////////////////////////////
  // Grayscale

  const uint16_t graya_v_shift = 0;
  const uint16_t graya_a_shift = 8;

  const uint16_t graya_v_mask = 0x00ff;
  const uint16_t graya_a_mask = 0xff00;

  inline uint8_t graya_getv(uint16_t c) {
    return (c >> graya_v_shift) & 0xff;
  }

  inline uint8_t graya_geta(uint16_t c) {
    return (c >> graya_a_shift) & 0xff;
  }

  inline uint16_t graya(uint8_t v, uint8_t a) {
    return ((v << graya_v_shift) |
            (a << graya_a_shift));
  }

  inline uint16_t gray(uint8_t v) {
    return graya(v, 255);
  }

  //////////////////////////////////////////////////////////////////////
  // Conversions

  typedef color_t (*rgba_to_graya_func)(const color_t c);

  inline color_t rgba_to_graya_using_hsv(const color_t c) {
    const uint8_t M = std::max(rgba_getr(c),
                               std::max(rgba_getg(c),
                                        rgba_getb(c)));
    return graya(M,
                 rgba_geta(c));
  }

  inline color_t rgba_to_graya_using_hsl(const color_t c) {
    const int m = std::min(rgba_getr(c),
                           std::min(rgba_getg(c),
                                    rgba_getb(c)));
    const int M = std::max(rgba_getr(c),
                           std::max(rgba_getg(c),
                                    rgba_getb(c)));
    return graya((M + m) / 2,
                 rgba_geta(c));
  }

  inline color_t rgba_to_graya_using_luma(const color_t c) {
    return graya(rgb_luma(rgba_getr(c),
                          rgba_getg(c),
                          rgba_getb(c)),
                 rgba_geta(c));
  }

} // namespace doc

#endif
