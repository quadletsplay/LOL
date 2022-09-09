// Aseprite
// Copyright (c) 2020-2022 Igara Studio S.A.
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "doc/octree_map.h"

#include "doc/palette.h"

#define MIN_LEVEL_OCTREE_DEEP 3
#define MIN_ALPHA_THRESHOLD 16

namespace doc {

//////////////////////////////////////////////////////////////////////
// OctreeNode

void OctreeNode::addColor(color_t c, int level, OctreeNode* parent,
                          int paletteIndex, int levelDeep)
{
  m_parent = parent;
  if (level >= levelDeep) {
    m_leafColor.add(c);
    m_paletteIndex = paletteIndex;
    return;
  }
  int index = getHextet(c, level);
  if (!m_children) {
    m_children.reset(new std::array<OctreeNode, 16>());
  }
  (*m_children)[index].addColor(c, level + 1, this, paletteIndex, levelDeep);
}

int OctreeNode::mapColor(int  r, int g, int b, int a, int mask_index, const Palette* palette, int level) const
{
  // New behavior: if mapColor do not have an exact rgba match, it must calculate which
  // color of the current palette is the bestfit and memorize the index in a octree leaf.
  if (level >= 8) {
    if (m_paletteIndex == -1)
      m_paletteIndex = palette->findBestfit(r, g, b, a, mask_index);
    return m_paletteIndex;
  }
  int index = getHextet(r, g, b, a, level);
  if (!m_children)
    m_children.reset(new std::array<OctreeNode, 16>());
  return (*m_children)[index].mapColor(r, g, b, a, mask_index, palette, level + 1);
}

void OctreeNode::collectLeafNodes(OctreeNodes& leavesVector, int& paletteIndex)
{
  for (int i=0; i<16; i++) {
    OctreeNode& child = (*m_children)[i];

    if (child.isLeaf()) {
      child.paletteIndex(paletteIndex);
      leavesVector.push_back(&child);
      paletteIndex++;
    }
    else if (child.hasChildren()) {
      child.collectLeafNodes(leavesVector, paletteIndex);
    }
  }
}

// removeLeaves(): remove leaves from a common parent
// auxParentVector: i/o addreess of an auxiliary parent leaf Vector from outside this function.
// rootLeavesVector: i/o address of the m_root->m_leavesVector
int OctreeNode::removeLeaves(OctreeNodes& auxParentVector,
                             OctreeNodes& rootLeavesVector)
{
  // Apply to OctreeNode which has children which are leaf nodes
  int result = 0;
  for (int i=15; i>=0; i--) {
    OctreeNode& child = (*m_children)[i];

    if (child.isLeaf()) {
      m_leafColor.add(child.leafColor());
      result++;
      if (rootLeavesVector[rootLeavesVector.size()-1] == &child)
        rootLeavesVector.pop_back();
    }
  }
  auxParentVector.push_back(this);
  return result - 1;
}

// static
int OctreeNode::getHextet(color_t c, int level)
{
  return ((c & (0x00000080 >> level)) ? 1 : 0) |
         ((c & (0x00008000 >> level)) ? 2 : 0) |
         ((c & (0x00800000 >> level)) ? 4 : 0) |
         ((c & (0x80000000 >> level)) ? 8 : 0);
}

int OctreeNode::getHextet(int r, int g, int b, int a, int level)
{
  return ((r & (0x80 >> level)) ? 1 : 0) |
         ((g & (0x80 >> level)) ? 2 : 0) |
         ((b & (0x80 >> level)) ? 4 : 0) |
         ((a & (0x80 >> level)) ? 8 : 0);
}

// static
color_t OctreeNode::hextetToBranchColor(int hextet, int level)
{
  return ((hextet & 1) ? 0x00000080 >> level : 0) |
         ((hextet & 2) ? 0x00008000 >> level : 0) |
         ((hextet & 4) ? 0x00800000 >> level : 0) |
         ((hextet & 8) ? 0x80000000 >> level : 0);
}

//////////////////////////////////////////////////////////////////////
// OctreeMap

// This constructor initializes 'm_maskIndex' and
// 'm_includeMaskColorInPalette' according to sprite's pixel format and
// background existence.
// These variables are needed before makePalette() and mapColor()
// execution.
OctreeMap::OctreeMap(const doc::Sprite* sprite)
{
  m_maskIndex = -1;
  if (sprite) {
    if (sprite->pixelFormat() == IMAGE_INDEXED) {
      m_maskIndex = sprite->transparentColor();
      m_includeMaskColorInPalette = false;
    }
    else if (sprite->backgroundLayer() &&
             sprite->allLayersCount() == 1)
      m_includeMaskColorInPalette = false;
    m_palette = sprite->palette(0);
    m_modifications = sprite->palette(0)->getModifications();
  }
};

bool OctreeMap::makePalette(Palette* palette,
                            int colorCount,
                            const int levelDeep)
{
  if (m_root.hasChildren()) {
    // We create paletteIndex to get a "global like" variable, in collectLeafNodes
    // function, the purpose is having a incremental variable in the stack memory
    // sharend between all recursive calls of collectLeafNodes.
    int paletteIndex = 0;
    m_root.collectLeafNodes(m_leavesVector, paletteIndex);
  }

  if (m_includeMaskColorInPalette)
    colorCount--;

  // If we can improve the octree accuracy, makePalette returns false, then
  // outside from this function we must re-construct the octreeMap all again with
  // deep level equal to 8.
  if (levelDeep == 7 && m_leavesVector.size() < colorCount)
    return false;


  OctreeNodes auxLeavesVector; // auxiliary collapsed node accumulator
  bool keepReducingMap = true;

  for (int level = levelDeep; level > -1; level--) {
    for (int i=m_leavesVector.size()-1; i>=0; i--) {
      if (m_leavesVector.size() + auxLeavesVector.size() <= colorCount) {
        for (int j=0; j < auxLeavesVector.size(); j++)
          m_leavesVector.push_back(auxLeavesVector[auxLeavesVector.size() - 1 - j]);
        keepReducingMap = false;
        break;
      }
      else if (m_leavesVector.size() == 0) {
        // When colorCount is < 16, auxLeavesVector->size() could reach the 16 size,
        // if this is true and we don't stop the regular removeLeaves algorithm,
        // the 16 remains colors will collapse in one.
        // So, we have to reduce color with other method:
        // Sort colors by pixelCount (most pixelCount on front of sortedVector),
        // then:
        // Blend in pairs from the least pixelCount colors.
        if (auxLeavesVector.size() <= 16 && colorCount < 16 && colorCount > 0) {
          // Sort colors:
          OctreeNodes sortedVector;
          int auxVectorSize = auxLeavesVector.size();
          for (int k=0; k < auxVectorSize; k++) {
            size_t maximumCount = auxLeavesVector[0]->leafColor().pixelCount();
            int maximumIndex = 0;
            for (int j=1; j < auxLeavesVector.size(); j++) {
              if (auxLeavesVector[j]->leafColor().pixelCount() > maximumCount) {
                maximumCount = auxLeavesVector[j]->leafColor().pixelCount();
                maximumIndex = j;
              }
            }
            sortedVector.push_back(auxLeavesVector[maximumIndex]);
            auxLeavesVector.erase(auxLeavesVector.begin() + maximumIndex);
          }
          // End Sort colors.
          // Blend colors:
          for (;;) {
            if (sortedVector.size() <= colorCount) {
              for (int k=0; k<sortedVector.size(); k++)
                m_leavesVector.push_back(sortedVector[k]);
              break;
            }
            sortedVector[sortedVector.size()-2]->leafColor()
              .add(sortedVector[sortedVector.size()-1]->leafColor());
            sortedVector.pop_back();
          }
          // End Blend colors:
          keepReducingMap = false;
          break;
        }
        else
          break;
      }

      m_leavesVector.back()->parent()->removeLeaves(auxLeavesVector, m_leavesVector);
    }
    if (keepReducingMap) {
      // Copy collapsed leaves to m_leavesVector
      int auxLeavesVectorSize = auxLeavesVector.size();
      for (int i=0; i<auxLeavesVectorSize; i++)
        m_leavesVector.push_back(auxLeavesVector[auxLeavesVector.size() - 1 - i]);
      auxLeavesVector.clear();
    }
    else
      break;
  }
  int leafCount = m_leavesVector.size();
  int aux = 0;
  if (m_includeMaskColorInPalette) {
    palette->resize(leafCount + 1);
    palette->setEntry(0, 0);
    aux = 1;
  }
  else
    palette->resize(leafCount);

  for (int i=0; i<leafCount; i++)
    palette->setEntry(i+aux,
                      m_leavesVector[i]->leafColor().rgbaColor());

  return true;
}

void OctreeMap::feedWithImage(const Image* image,
                              const bool withAlpha,
                              const int levelDeep)
{
  ASSERT(image);
  ASSERT(image->pixelFormat() == IMAGE_RGB || image->pixelFormat() == IMAGE_GRAYSCALE);
  color_t forceFullOpacity;
  int alpha = 0;
  const bool imageIsRGBA = image->pixelFormat() == IMAGE_RGB;

  auto add_color_to_octree =
    [this, &forceFullOpacity, &alpha, &levelDeep, &imageIsRGBA](color_t color) {
      alpha = (imageIsRGBA ? rgba_geta(color) : graya_geta(color));
      if (alpha >= MIN_ALPHA_THRESHOLD) { // Colors which alpha is less than
                                          // MIN_ALPHA_THRESHOLD will not registered
        color |= forceFullOpacity;
        color = (imageIsRGBA ? color : rgba(graya_getv(color),
                                            graya_getv(color),
                                            graya_getv(color),
                                            alpha));
        addColor(color, levelDeep);
      }
    };

  switch (image->pixelFormat()) {
    case IMAGE_RGB: {
      forceFullOpacity = (withAlpha) ? 0 : rgba_a_mask;
      doc::for_each_pixel<RgbTraits>(image, add_color_to_octree);
      break;
    }
    case IMAGE_GRAYSCALE: {
      forceFullOpacity = (withAlpha) ? 0 : graya_a_mask;
      doc::for_each_pixel<GrayscaleTraits>(image, add_color_to_octree);
      break;
    }
  }
}

int OctreeMap::mapColor(color_t rgba) const
{
  return m_root.mapColor(rgba_getr(rgba),
                         rgba_getg(rgba),
                         rgba_getb(rgba),
                         rgba_geta(rgba),
                         m_maskIndex,
                         m_palette, 0);
}

void OctreeMap::regenerateMap(const Palette* palette, const int maskIndex)
{
  ASSERT(palette);
  if (!palette)
    return;

  // Skip useless regenerations
  if (m_palette == palette &&
      m_modifications == palette->getModifications() &&
      m_maskIndex == maskIndex)
    return;

  m_root = OctreeNode();
  m_leavesVector.clear();
  m_maskIndex = maskIndex;

  for (int i=0; i<palette->size(); i++)
    m_root.addColor(palette->entry(i), 0, &m_root, i, 8);

  m_palette = palette;
  m_modifications = palette->getModifications();
}

} // namespace doc
