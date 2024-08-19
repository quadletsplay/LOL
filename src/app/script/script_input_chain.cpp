// Aseprite
// Copyright (C) 2024  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "app/app.h"
#include "app/cmd/remap_colors.h"
#include "app/context_access.h"
#include "app/script/script_input_chain.h"
#include "app/site.h"
#include "app/tx.h"
#include "app/util/clipboard.h"
#include "doc/mask.h"
#include "doc/layer.h"
#include "doc/primitives.h"

#include <cstring>
#include <limits>
#include <memory>

namespace app {

ScriptInputChain::~ScriptInputChain() { }

void ScriptInputChain::onNewInputPriority(InputChainElement* element,
                                          const ui::Message* msg) { }

bool ScriptInputChain::onCanCut(Context* ctx)
{
  return ctx->activeDocument() && ctx->activeDocument()->mask();
}

bool ScriptInputChain::onCanCopy(Context* ctx)
{
  return onCanCut(ctx);
}

bool ScriptInputChain::onCanPaste(Context* ctx)
{
  const Clipboard* clipboard(ctx->clipboard());
  if (!clipboard)
    return false;
  return clipboard->format() == ClipboardFormat::Image &&
         ctx->activeSite().layer() &&
         ctx->activeSite().layer()->type() == ObjectType::LayerImage;
}

bool ScriptInputChain::onCanClear(Context* ctx)
{
  return onCanCut(ctx);
}

bool ScriptInputChain::onCut(Context* ctx)
{
  ContextWriter writer(ctx);
  Clipboard* clipboard = ctx->clipboard();
  if (!clipboard)
    return false;
  if (writer.document() && ctx->activeDocument() &&
      ctx->activeDocument()->mask() &&
      !ctx->activeDocument()->mask()->isEmpty()) {
    clipboard->cut(writer);
    return true;
  }
  return false;
}

bool ScriptInputChain::onCopy(Context* ctx)
{
  ContextReader reader(ctx);
  Clipboard* clipboard = ctx->clipboard();
  if (!clipboard)
    return false;
  if (reader.document() && ctx->activeDocument() &&
      ctx->activeDocument()->mask() &&
      !ctx->activeDocument()->mask()->isEmpty()) {
    clipboard->copy(reader);
    return true;
  }
  return false;
}

bool ScriptInputChain::onPaste(Context* ctx)
{
  Clipboard* clipboard = ctx->clipboard();
  if (!clipboard)
    return false;
  if (clipboard->format() == ClipboardFormat::Image) {
    clipboard->paste(ctx, false);
    return true;
  }
  return false;
}

bool ScriptInputChain::onClear(Context* ctx)
{
  // TODO This code is similar to DocView::onClear() and Clipboard::cut()
  ContextWriter writer(ctx);
  Doc* document = ctx->activeDocument();
  if (writer.document() && document &&
      document->mask() && !document->mask()->isEmpty()) {
    ctx->clipboard()->clearContent();
    CelList cels;
    const Site site = ctx->activeSite();
    cels.push_back(site.cel());
    if (cels.empty())            // No cels to modify
      return false;
    Tx tx(writer, "Clear");
    ctx->clipboard()->clearMaskFromCels(
      tx, document, site, cels, true);
    tx.commit();
    return true;
  }
  return false;
}

void ScriptInputChain::onCancel(Context* ctx)
{
}

} // namespace app
