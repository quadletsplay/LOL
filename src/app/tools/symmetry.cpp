// Aseprite
// Copyright (C) 2021-2023  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "app/tools/symmetry.h"

 #include "app/tools/point_shape.h"
 #include "app/tools/tool_loop.h"

namespace app {
namespace tools {

void Symmetry::generateStrokes(const Stroke& stroke, Strokes& strokes,
                               ToolLoop* loop)
{
  Stroke stroke2;
  strokes.push_back(stroke);
  gen::SymmetryMode symmetryMode = loop->getSymmetry()->mode();
  switch (symmetryMode) {
    case gen::SymmetryMode::NONE:
      ASSERT(false);
      break;

    case gen::SymmetryMode::HORIZONTAL:
    case gen::SymmetryMode::VERTICAL:
      calculateSymmetricalStroke(stroke, stroke2, loop, symmetryMode);
      strokes.push_back(stroke2);
      break;

    case gen::SymmetryMode::BOTH: {
      calculateSymmetricalStroke(stroke, stroke2, loop, gen::SymmetryMode::HORIZONTAL);
      strokes.push_back(stroke2);

      Stroke stroke3;
      calculateSymmetricalStroke(stroke, stroke3, loop, gen::SymmetryMode::VERTICAL);
      strokes.push_back(stroke3);

      Stroke stroke4;
      calculateSymmetricalStroke(stroke3, stroke4, loop, gen::SymmetryMode::BOTH);
      strokes.push_back(stroke4);
      break;
    }

    case gen::SymmetryMode::RIGHT_DIAG: {
      calculateSymmetricalStroke(stroke, stroke2, loop, gen::SymmetryMode::RIGHT_DIAG);
      strokes.push_back(stroke2);
      break;
    }

    case gen::SymmetryMode::LEFT_DIAG: {
      calculateSymmetricalStroke(stroke, stroke2, loop, gen::SymmetryMode::LEFT_DIAG);
      strokes.push_back(stroke2);
      break;
    }

    case gen::SymmetryMode::BOTH_DIAG: {
      calculateSymmetricalStroke(stroke, stroke2, loop, gen::SymmetryMode::RIGHT_DIAG);
      strokes.push_back(stroke2);

      Stroke stroke3;
      calculateSymmetricalStroke(stroke, stroke3, loop, gen::SymmetryMode::LEFT_DIAG);
      strokes.push_back(stroke3);

      Stroke stroke4;
      calculateSymmetricalStroke(stroke, stroke4, loop, gen::SymmetryMode::BOTH_DIAG);
      strokes.push_back(stroke4);
      break;
    }

    case gen::SymmetryMode::ALL: {
      calculateSymmetricalStroke(stroke, stroke2, loop, gen::SymmetryMode::HORIZONTAL);
      strokes.push_back(stroke2);

      Stroke stroke3;
      calculateSymmetricalStroke(stroke, stroke3, loop, gen::SymmetryMode::VERTICAL);
      strokes.push_back(stroke3);

      Stroke stroke4;
      calculateSymmetricalStroke(stroke3, stroke4, loop, gen::SymmetryMode::BOTH);
      strokes.push_back(stroke4);

      Stroke stroke5;
      calculateSymmetricalStroke(stroke, stroke5, loop, gen::SymmetryMode::RIGHT_DIAG);
      strokes.push_back(stroke5);

      Stroke stroke6;
      calculateSymmetricalStroke(stroke5, stroke6, loop, gen::SymmetryMode::RIGHT_DIAG_REFLEX);
      strokes.push_back(stroke6);

      Stroke stroke7;
      calculateSymmetricalStroke(stroke, stroke7, loop, gen::SymmetryMode::LEFT_DIAG);
      strokes.push_back(stroke7);

      Stroke stroke8;
      calculateSymmetricalStroke(stroke7, stroke8, loop, gen::SymmetryMode::LEFT_DIAG_REFLEX);
      strokes.push_back(stroke8);
      break;
    }
  }
}

void Symmetry::calculateSymmetricalStroke(const Stroke& refStroke, Stroke& stroke,
                                          ToolLoop* loop, gen::SymmetryMode symmetryMode)
{
  gfx::Size brushSize(1, 1);
  gfx::Point brushCenter(0, 0);
  auto brush = loop->getBrush();
  if (!loop->getPointShape()->isFloodFill()) {
    if (symmetryMode == gen::SymmetryMode::NONE ||
        symmetryMode == gen::SymmetryMode::HORIZONTAL ||
        symmetryMode == gen::SymmetryMode::VERTICAL ||
        symmetryMode == gen::SymmetryMode::BOTH ||
        symmetryMode == gen::SymmetryMode::BOTH_DIAG) {
      brushSize = brush->bounds().size();
      brushCenter = brush->center();
    }
    else {
      brushSize = gfx::Size(brush->bounds().size().h,
                            brush->bounds().size().w);
      brushCenter = gfx::Point(brush->center().y,
                               brush->center().x);
    }
  }

  const bool isDynamic = loop->getDynamics().isDynamic();
  for (const auto& pt : refStroke) {
    if (isDynamic) {
      brushSize = gfx::Size(pt.size, pt.size);
      int center = (brushSize.w - brushSize.w % 2) / 2;
      brushCenter = gfx::Point(center, center);
    }
    Stroke::Pt pt2 = pt;
    pt2.symmetry = symmetryMode;
    switch (symmetryMode) {
      case gen::SymmetryMode::RIGHT_DIAG:
        pt2.x = -pt.y + m_x + m_y - (brushSize.w % 2 ? 1 : 0);
        pt2.y = -pt.x + m_x + m_y - (brushSize.h % 2 ? 1 : 0);
        break;
      case gen::SymmetryMode::LEFT_DIAG:
        pt2.x = pt.y + m_x - m_y + (m_x - int(m_x));
        pt2.y = pt.x - m_x + m_y + (m_y - int(m_y));
        break;
      case gen::SymmetryMode::BOTH_DIAG:
        pt2.x = 2 * (m_x + brushCenter.x) - pt.x - brushSize.w;
        pt2.y = 2 * (m_y + brushCenter.y) - pt.y - brushSize.h;
        break;
      case gen::SymmetryMode::RIGHT_DIAG_REFLEX:
      case gen::SymmetryMode::LEFT_DIAG_REFLEX:
        pt2.y = 2 * m_y - pt.y - (brushSize.h % 2 ? 1 : 0);
        break;
      case gen::SymmetryMode::HORIZONTAL:
      case gen::SymmetryMode::BOTH:
        pt2.x = 2 * (m_x + brushCenter.x) - pt2.x - brushSize.w;
        break;
      default:
        pt2.y = 2 * (m_y + brushCenter.y) - pt2.y - brushSize.h;
    }
    stroke.addPoint(pt2);
  }
}

} // namespace tools
} // namespace app
