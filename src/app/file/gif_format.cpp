// Aseprite
// Copyright (C) 2018-2020  Igara Studio S.A.
// Copyright (C) 2001-2018  David Capello
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "app/color_spaces.h"
#include "app/console.h"
#include "app/context.h"
#include "app/doc.h"
#include "app/file/file.h"
#include "app/file/file_format.h"
#include "app/file/format_options.h"
#include "app/file/gif_format.h"
#include "app/file/gif_options.h"
#include "app/modules/gui.h"
#include "app/pref/preferences.h"
#include "app/util/autocrop.h"
#include "base/file_handle.h"
#include "base/fs.h"
#include "doc/doc.h"
#include "gfx/clip.h"
#include "render/dithering.h"
#include "render/ordered_dither.h"
#include "render/quantization.h"
#include "render/render.h"
#include "ui/button.h"

#include "gif_options.xml.h"

#include <gif_lib.h>

#ifdef _WIN32
  #include <io.h>
  #define posix_lseek  _lseek
#else
  #include <unistd.h>
  #define posix_lseek  lseek
#endif

#if GIFLIB_MAJOR < 5
#define GifMakeMapObject MakeMapObject
#define GifFreeMapObject FreeMapObject
#define GifBitSize       BitSize
#endif

#define GIF_TRACE(...)

// GifBitSize can return 9 (it's a bug in giflib)
#define GifBitSizeLimited(v) (MIN(GifBitSize(v), 8))

namespace app {

using namespace base;

enum class DisposalMethod {
  NONE,
  DO_NOT_DISPOSE,
  RESTORE_BGCOLOR,
  RESTORE_PREVIOUS,
};

class GifFormat : public FileFormat {

  const char* onGetName() const override {
    return "gif";
  }

  void onGetExtensions(base::paths& exts) const override {
    exts.push_back("gif");
  }

  dio::FileFormat onGetDioFormat() const override {
    return dio::FileFormat::GIF_ANIMATION;
  }

  int onGetFlags() const override {
    return
      FILE_SUPPORT_LOAD |
      FILE_SUPPORT_SAVE |
      FILE_SUPPORT_RGB |
      FILE_SUPPORT_RGBA |
      FILE_SUPPORT_GRAY |
      FILE_SUPPORT_GRAYA |
      FILE_SUPPORT_INDEXED |
      FILE_SUPPORT_FRAMES |
      FILE_SUPPORT_PALETTES |
      FILE_SUPPORT_GET_FORMAT_OPTIONS;
  }

  bool onLoad(FileOp* fop) override;
#ifdef ENABLE_SAVE
  bool onSave(FileOp* fop) override;
#endif
  FormatOptionsPtr onAskUserForFormatOptions(FileOp* fop) override;
};

FileFormat* CreateGifFormat()
{
  return new GifFormat;
}

static int interlaced_offset[] = { 0, 4, 2, 1 };
static int interlaced_jumps[] = { 8, 8, 4, 2 };

// TODO this should be part of a GifEncoder instance
// True if the GifEncoder should save the animation for Twitter:
// * Frames duration >= 2, and
// * Last frame 1/4 of its duration
static bool fix_last_frame_duration = false;

GifEncoderDurationFix::GifEncoderDurationFix(bool state)
{
  fix_last_frame_duration = state;
}

GifEncoderDurationFix::~GifEncoderDurationFix()
{
  fix_last_frame_duration = false;
}

struct GifFilePtr {
public:
#if GIFLIB_MAJOR >= 5
  typedef int (*CloseFunc)(GifFileType*, int*);
#else
  typedef int (*CloseFunc)(GifFileType*);
#endif

  GifFilePtr(GifFileType* ptr, CloseFunc closeFunc) :
    m_ptr(ptr), m_closeFunc(closeFunc) {
  }

  ~GifFilePtr() {
#if GIFLIB_MAJOR >= 5
    int errCode;
    m_closeFunc(m_ptr, &errCode);
#else
    m_closeFunc(m_ptr);
#endif
  }

  operator GifFileType*() {
    return m_ptr;
  }

  GifFileType* operator->() {
    return m_ptr;
  }

private:
  GifFileType* m_ptr;
  CloseFunc m_closeFunc;
};

static void process_disposal_method(const Image* previous,
                                    Image* current,
                                    const DisposalMethod disposal,
                                    const gfx::Rect& frameBounds,
                                    const color_t clearColor)
 {
  switch (disposal) {

    case DisposalMethod::NONE:
    case DisposalMethod::DO_NOT_DISPOSE:
      // Do nothing
      break;

    case DisposalMethod::RESTORE_BGCOLOR:
      fill_rect(current,
                frameBounds.x,
                frameBounds.y,
                frameBounds.x+frameBounds.w-1,
                frameBounds.y+frameBounds.h-1,
                clearColor);
      break;

    case DisposalMethod::RESTORE_PREVIOUS:
      current->copy(previous, gfx::Clip(frameBounds));
      break;
  }
}

static inline doc::color_t colormap2rgba(ColorMapObject* colormap, int i) {
  return doc::rgba(
    colormap->Colors[i].Red,
    colormap->Colors[i].Green,
    colormap->Colors[i].Blue, 255);
}

// Decodes a GIF file trying to keep the image in Indexed format. If
// it's not possible to handle it as Indexed (e.g. it contains more
// than 256 colors), the file will be automatically converted to RGB.
//
// This is a complex process because GIF files are made to be composed
// over RGB output. Each frame is composed over the previous frame,
// and combinations of local colormaps can output any number of
// colors, not just 256. So previous RGB colors must be kept and
// merged with new colormaps.
class GifDecoder {
public:
  GifDecoder(FileOp* fop, GifFileType* gifFile, int fd, size_t filesize)
    : m_fop(fop)
    , m_gifFile(gifFile)
    , m_fd(fd)
    , m_filesize(filesize)
    , m_sprite(nullptr)
    , m_spriteBounds(0, 0, m_gifFile->SWidth, m_gifFile->SHeight)
    , m_frameNum(0)
    , m_opaque(false)
    , m_disposalMethod(DisposalMethod::NONE)
    , m_bgIndex(m_gifFile->SBackGroundColor >= 0 ? m_gifFile->SBackGroundColor: 0)
    , m_localTransparentIndex(-1)
    , m_frameDelay(1)
    , m_remap(256)
    , m_hasLocalColormaps(false)
    , m_firstLocalColormap(nullptr) {
    GIF_TRACE("GIF: background index=%d\n", (int)m_gifFile->SBackGroundColor);
    GIF_TRACE("GIF: global colormap=%d, ncolors=%d\n",
              (m_gifFile->SColorMap ? 1: 0),
              (m_gifFile->SColorMap ? m_gifFile->SColorMap->ColorCount: 0));
  }

  ~GifDecoder() {
    if (m_firstLocalColormap)
      GifFreeMapObject(m_firstLocalColormap);
  }

  Sprite* releaseSprite() {
    return m_sprite.release();
  }

  bool decode() {
    GifRecordType recType;

    // Read record by record
    while ((recType = readRecordType()) != TERMINATE_RECORD_TYPE) {
      readRecord(recType);

      // Just one frame?
      if (m_fop->isOneFrame() && m_frameNum > 0)
        break;

      if (m_fop->isStop())
        break;

      if (m_filesize > 0) {
        int pos = posix_lseek(m_fd, 0, SEEK_CUR);
        m_fop->setProgress(double(pos) / double(m_filesize));
      }
    }

    if (m_sprite) {
      // Add entries to include the transparent color
      if (m_bgIndex >= m_sprite->palette(0)->size())
        m_sprite->palette(0)->resize(m_bgIndex+1);

      switch (m_sprite->pixelFormat()) {

        case IMAGE_INDEXED: {
          // Use the original global color map
          ColorMapObject* global = m_gifFile->SColorMap;
          if (!global)
            global = m_firstLocalColormap;
          if (global &&
              global->ColorCount >= m_sprite->palette(0)->size() &&
              !m_hasLocalColormaps) {
            remapToGlobalColormap(global);
          }
          break;
        }

        case IMAGE_RGB:
          // Avoid huge color palettes
          if (m_sprite->palette(0)->size() > 256) {
            reduceToAnOptimizedPalette();
          }
          break;
      }

      if (m_layer && m_opaque)
        m_layer->configureAsBackground();

      // sRGB is the default color space for GIF files
      m_sprite->setColorSpace(gfx::ColorSpace::MakeSRGB());

      return true;
    }
    else
      return false;
  }

private:

  GifRecordType readRecordType() {
    GifRecordType type;
    if (DGifGetRecordType(m_gifFile, &type) == GIF_ERROR)
      throw Exception("Invalid GIF record in file.\n");

    return type;
  }

  void readRecord(GifRecordType recordType) {
    switch (recordType) {

      case IMAGE_DESC_RECORD_TYPE:
        readImageDescRecord();
        break;

      case EXTENSION_RECORD_TYPE:
        readExtensionRecord();
        break;
    }
  }

  void readImageDescRecord() {
    if (DGifGetImageDesc(m_gifFile) == GIF_ERROR)
      throw Exception("Invalid GIF image descriptor.\n");

    // These are the bounds of the image to read.
    gfx::Rect frameBounds(
      m_gifFile->Image.Left,
      m_gifFile->Image.Top,
      m_gifFile->Image.Width,
      m_gifFile->Image.Height);

#if 0 // Generally GIF files should contain frame bounds inside the
      // canvas bounds (in other case the GIF will contain pixels that
      // are not visible). In case that some app creates an invalid
      // GIF files with bounds outside the canvas, we should support
      // to load the GIF file anyway (which is what is done by other
      // apps).
    if (!m_spriteBounds.contains(frameBounds))
      throw Exception("Image %d is out of sprite bounds.\n", (int)m_frameNum);
#endif

    // Create sprite if this is the first frame
    if (!m_sprite)
      createSprite();

    // Add a frame if it's necessary
    if (m_sprite->lastFrame() < m_frameNum)
      m_sprite->addFrame(m_frameNum);

    // Create a temporary image loading the frame pixels from the GIF file
    std::unique_ptr<Image> frameImage;
    // We don't know if a GIF file could contain empty bounds (width
    // or height=0), but we check this just in case.
    if (!frameBounds.isEmpty())
      frameImage.reset(readFrameIndexedImage(frameBounds));

    GIF_TRACE("GIF: Frame[%d] transparent index = %d\n", (int)m_frameNum, m_localTransparentIndex);

    if (m_frameNum == 0) {
      if (m_localTransparentIndex >= 0)
        m_opaque = false;
      else
        m_opaque = true;
    }

    // Merge this frame colors with the current palette
    if (frameImage)
      updatePalette(frameImage.get());

    // Convert the sprite to RGB if we have more than 256 colors
    if ((m_sprite->pixelFormat() == IMAGE_INDEXED) &&
        (m_sprite->palette(m_frameNum)->size() > 256)) {
      GIF_TRACE("GIF: Converting to RGB because we have %d colors\n",
                m_sprite->palette(m_frameNum)->size());

      convertIndexedSpriteToRgb();
    }

    // Composite frame with previous frame
    if (frameImage) {
      if (m_sprite->pixelFormat() == IMAGE_INDEXED) {
        compositeIndexedImageToIndexed(frameBounds, frameImage.get());
      }
      else {
        compositeIndexedImageToRgb(frameBounds, frameImage.get());
      }
    }

    // Create cel
    createCel();

    // Dispose/clear frame content
    process_disposal_method(m_previousImage.get(),
                            m_currentImage.get(),
                            m_disposalMethod,
                            frameBounds,
                            m_bgIndex);

    // Copy the current image into previous image
    copy_image(m_previousImage.get(), m_currentImage.get());

    // Set frame delay (1/100th seconds to milliseconds)
    if (m_frameDelay >= 0)
      m_sprite->setFrameDuration(m_frameNum, m_frameDelay*10);

    // Reset extension variables
    m_disposalMethod = DisposalMethod::NONE;
    m_localTransparentIndex = -1;
    m_frameDelay = 1;

    // Next frame
    ++m_frameNum;
  }

  Image* readFrameIndexedImage(const gfx::Rect& frameBounds) {
    std::unique_ptr<Image> frameImage(
      Image::create(IMAGE_INDEXED, frameBounds.w, frameBounds.h));

    IndexedTraits::address_t addr;

    if (m_gifFile->Image.Interlace) {
      // Need to perform 4 passes on the image
      for (int i=0; i<4; ++i)
        for (int y = interlaced_offset[i]; y < frameBounds.h; y += interlaced_jumps[i]) {
          addr = frameImage->getPixelAddress(0, y);
          if (DGifGetLine(m_gifFile, addr, frameBounds.w) == GIF_ERROR)
            throw Exception("Invalid interlaced image data.");
        }
    }
    else {
      for (int y = 0; y < frameBounds.h; ++y) {
        addr = frameImage->getPixelAddress(0, y);
        if (DGifGetLine(m_gifFile, addr, frameBounds.w) == GIF_ERROR)
          throw Exception("Invalid image data (%d).\n"
#if GIFLIB_MAJOR >= 5
                          , m_gifFile->Error
#else
                          , GifLastError()
#endif
                          );
      }
    }

    return frameImage.release();
  }

  ColorMapObject* getFrameColormap() {
    ColorMapObject* global = m_gifFile->SColorMap;
    ColorMapObject* colormap = m_gifFile->Image.ColorMap;

    if (!colormap) {
      // Doesn't have local map, use the global one
      colormap = global;
    }
    else if (!m_hasLocalColormaps) {
      if (!global) {
        if (!m_firstLocalColormap)
          m_firstLocalColormap = GifMakeMapObject(colormap->ColorCount,
                                                  colormap->Colors);
        global = m_firstLocalColormap;
      }

      if (global->ColorCount != colormap->ColorCount)
        m_hasLocalColormaps = true;
      else {
        for (int i=0; i<colormap->ColorCount; ++i) {
          if (global->Colors[i].Red   != colormap->Colors[i].Red ||
              global->Colors[i].Green != colormap->Colors[i].Green ||
              global->Colors[i].Blue  != colormap->Colors[i].Blue) {
            m_hasLocalColormaps = true;
            break;
          }
        }
      }
    }

    if (!colormap)
      throw Exception("There is no color map.");

    return colormap;
  }

  // Adds colors used in the GIF frame so we can draw it over
  // m_currentImage. If the frame contains a local colormap, we try to
  // find them in the current sprite palette (using
  // Palette::findExactMatch()) so we don't add duplicated entries.
  // To do so we use a Remap (m_remap variable) which matches the
  // original GIF frame colors with the current sprite colors.
  void updatePalette(const Image* frameImage) {
    ColorMapObject* colormap = getFrameColormap();
    int ncolors = colormap->ColorCount;
    bool isLocalColormap = (m_gifFile->Image.ColorMap ? true: false);

    GIF_TRACE("GIF: Local colormap=%d, ncolors=%d\n", isLocalColormap, ncolors);

    // We'll calculate the list of used colormap indexes in this
    // frameImage.
    PalettePicks usedEntries(ncolors);
    if (isLocalColormap) {
      // With this we avoid discarding the transparent index when a
      // frame indicates that it uses a specific index as transparent
      // but the image is completely opaque anyway.
      if (m_localTransparentIndex >= 0 &&
          m_localTransparentIndex < ncolors) {
        usedEntries[m_localTransparentIndex] = true;
      }

      for (const auto& i : LockImageBits<IndexedTraits>(frameImage)) {
        if (i >= 0 && i < ncolors)
          usedEntries[i] = true;
      }
    }
    // Mark all entries as used if the colormap is global.
    else {
      usedEntries.all();
    }

    // Number of colors (indexes) used in the frame image.
    int usedNColors = usedEntries.picks();

    // Check if we need an extra color equal to the bg color in a
    // transparent frameImage.
    bool needsExtraBgColor = false;
    if (m_sprite->pixelFormat() == IMAGE_INDEXED &&
        !m_opaque && m_bgIndex != m_localTransparentIndex) {
      for (const auto& i : LockImageBits<IndexedTraits>(frameImage)) {
        if (i == m_bgIndex &&
            i != m_localTransparentIndex) {
          needsExtraBgColor = true;
          break;
        }
      }
    }

    std::unique_ptr<Palette> palette;
    if (m_frameNum == 0)
      palette.reset(new Palette(m_frameNum, usedNColors + (needsExtraBgColor ? 1: 0)));
    else {
      palette.reset(new Palette(*m_sprite->palette(m_frameNum-1)));
      palette->setFrame(m_frameNum);
    }
    resetRemap(MAX(ncolors, palette->size()));

    // Number of colors in the colormap that are part of the current
    // sprite palette.
    int found = 0;
    if (m_frameNum > 0) {
      for (int i=0; i<ncolors; ++i) {
        if (!usedEntries[i])
          continue;

        int j = palette->findExactMatch(
          colormap->Colors[i].Red,
          colormap->Colors[i].Green,
          colormap->Colors[i].Blue, 255,
          (m_opaque ? -1: m_bgIndex));
        if (j >= 0) {
          m_remap.map(i, j);
          ++found;
        }
      }
    }

    // All needed colors in the colormap are present in the current
    // palette.
    if (found == usedNColors)
      return;

    // In other case, we need to add the missing colors...

    // First index that acts like a base for new colors in palette.
    int base = (m_frameNum == 0 ? 0: palette->size());

    // Number of colors in the image that aren't in the palette.
    int missing = (usedNColors - found);

    GIF_TRACE("GIF: Bg index=%d,\n"
              "  Local transparent index=%d,\n"
              "  Need extra index to show bg color=%d,\n  "
              "  Found colors in palette=%d,\n"
              "  Used colors in local pixels=%d,\n"
              "  Base for new colors in palette=%d,\n"
              "  Colors in the image missing in the palette=%d,\n"
              "  New palette size=%d\n",
              m_bgIndex, m_localTransparentIndex, needsExtraBgColor,
              found, usedNColors, base, missing,
              base + missing + (needsExtraBgColor ? 1: 0));

    Palette oldPalette(*palette);
    palette->resize(base + missing + (needsExtraBgColor ? 1: 0));
    resetRemap(MAX(ncolors, palette->size()));

    for (int i=0; i<ncolors; ++i) {
      if (!usedEntries[i])
        continue;

      int j = -1;

      if (m_frameNum > 0) {
        j = oldPalette.findExactMatch(
          colormap->Colors[i].Red,
          colormap->Colors[i].Green,
          colormap->Colors[i].Blue, 255,
          (m_opaque ? -1: m_bgIndex));
      }

      if (j < 0) {
        j = base++;
        palette->setEntry(j, colormap2rgba(colormap, i));
      }
      m_remap.map(i, j);
    }

    if (needsExtraBgColor) {
      int i = m_bgIndex;
      int j = base++;
      palette->setEntry(j, colormap2rgba(colormap, i));
      m_remap.map(i, j);
    }

    ASSERT(base == palette->size());
    m_sprite->setPalette(palette.get(), false);
  }

  void compositeIndexedImageToIndexed(const gfx::Rect& frameBounds,
                                      const Image* frameImage) {
    gfx::Clip clip(frameBounds.x, frameBounds.y, 0, 0,
                   frameBounds.w, frameBounds.h);
    if (!clip.clip(m_currentImage->width(),
                   m_currentImage->height(),
                   frameImage->width(),
                   frameImage->height()))
      return;

    const LockImageBits<IndexedTraits> srcBits(frameImage, clip.srcBounds());
    LockImageBits<IndexedTraits> dstBits(m_currentImage.get(), clip.dstBounds());

    auto srcIt = srcBits.begin(), srcEnd = srcBits.end();
    auto dstIt = dstBits.begin(), dstEnd = dstBits.end();

    // Compose the frame image with the previous frame
    for (; srcIt != srcEnd && dstIt != dstEnd; ++srcIt, ++dstIt) {
      color_t i = *srcIt;
      if (int(i) == m_localTransparentIndex)
        continue;

      i = m_remap[i];
      *dstIt = i;
    }

    ASSERT(srcIt == srcEnd);
    ASSERT(dstIt == dstEnd);
  }

  void compositeIndexedImageToRgb(const gfx::Rect& frameBounds,
                                  const Image* frameImage) {
    gfx::Clip clip(frameBounds.x, frameBounds.y, 0, 0,
                   frameBounds.w, frameBounds.h);
    if (!clip.clip(m_currentImage->width(),
                   m_currentImage->height(),
                   frameImage->width(),
                   frameImage->height()))
      return;

    const LockImageBits<IndexedTraits> srcBits(frameImage, clip.srcBounds());
    LockImageBits<RgbTraits> dstBits(m_currentImage.get(), clip.dstBounds());

    auto srcIt = srcBits.begin(), srcEnd = srcBits.end();
    auto dstIt = dstBits.begin(), dstEnd = dstBits.end();

    ColorMapObject* colormap = getFrameColormap();

    // Compose the frame image with the previous frame
    for (; srcIt != srcEnd && dstIt != dstEnd; ++srcIt, ++dstIt) {
      color_t i = *srcIt;
      if (int(i) == m_localTransparentIndex)
        continue;

      i = rgba(
        colormap->Colors[i].Red,
        colormap->Colors[i].Green,
        colormap->Colors[i].Blue, 255);

      *dstIt = i;
    }

    ASSERT(srcIt == srcEnd);
    ASSERT(dstIt == dstEnd);
  }

  void createCel() {
    Cel* cel = new Cel(m_frameNum, ImageRef(0));
    try {
      ImageRef celImage(Image::createCopy(m_currentImage.get()));
      try {
        cel->data()->setImage(celImage);
      }
      catch (...) {
        throw;
      }
      m_layer->addCel(cel);
    }
    catch (...) {
      delete cel;
      throw;
    }
  }

  void readExtensionRecord() {
    int extCode;
    GifByteType* extension;
    if (DGifGetExtension(m_gifFile, &extCode, &extension) == GIF_ERROR)
      throw Exception("Invalid GIF extension record.\n");

    if (extCode == GRAPHICS_EXT_FUNC_CODE) {
      if (extension[0] >= 4) {
        m_disposalMethod        = (DisposalMethod)((extension[1] >> 2) & 7);
        m_localTransparentIndex = (extension[1] & 1) ? extension[4]: -1;
        m_frameDelay            = (extension[3] << 8) | extension[2];

        GIF_TRACE("GIF: Disposal method: %d\n  Transparent index: %d\n  Frame delay: %d\n",
                  m_disposalMethod, m_localTransparentIndex, m_frameDelay);
      }
    }

    while (extension) {
      if (DGifGetExtensionNext(m_gifFile, &extension) == GIF_ERROR)
        throw Exception("Invalid GIF extension record.\n");
    }
  }

  void createSprite() {
    ColorMapObject* colormap = nullptr;
    if (m_gifFile->SColorMap) {
      colormap = m_gifFile->SColorMap;
    }
    else if (m_gifFile->Image.ColorMap) {
      colormap = m_gifFile->Image.ColorMap;
    }
    int ncolors = (colormap ? colormap->ColorCount: 1);
    int w = m_spriteBounds.w;
    int h = m_spriteBounds.h;

    m_sprite.reset(new Sprite(ImageSpec(ColorMode::INDEXED, w, h), ncolors));
    m_sprite->setTransparentColor(m_bgIndex);

    m_currentImage.reset(Image::create(IMAGE_INDEXED, w, h));
    m_previousImage.reset(Image::create(IMAGE_INDEXED, w, h));
    m_currentImage->setMaskColor(m_bgIndex);
    m_previousImage->setMaskColor(m_bgIndex);
    clear_image(m_currentImage.get(), m_bgIndex);
    clear_image(m_previousImage.get(), m_bgIndex);

    m_layer = new LayerImage(m_sprite.get());
    m_sprite->root()->addLayer(m_layer);
  }

  void resetRemap(int ncolors) {
    m_remap = Remap(ncolors);
    for (int i=0; i<ncolors; ++i)
      m_remap.map(i, i);
  }

  // Converts the whole sprite read so far because it contains more
  // than 256 colors at the same time.
  void convertIndexedSpriteToRgb() {
    for (Cel* cel : m_sprite->uniqueCels()) {
      Image* oldImage = cel->image();
      ImageRef newImage(
        render::convert_pixel_format
        (oldImage, NULL, IMAGE_RGB,
         render::Dithering(),
         nullptr,
         m_sprite->palette(cel->frame()),
         m_opaque,
         m_bgIndex));

      m_sprite->replaceImage(oldImage->id(), newImage);
    }

    m_currentImage.reset(
      render::convert_pixel_format
      (m_currentImage.get(), NULL, IMAGE_RGB,
       render::Dithering(),
       nullptr,
       m_sprite->palette(m_frameNum),
       m_opaque,
       m_bgIndex));

    m_previousImage.reset(
      render::convert_pixel_format
      (m_previousImage.get(), NULL, IMAGE_RGB,
       render::Dithering(),
       nullptr,
       m_sprite->palette(MAX(0, m_frameNum-1)),
       m_opaque,
       m_bgIndex));

    m_sprite->setPixelFormat(IMAGE_RGB);
  }

  void remapToGlobalColormap(ColorMapObject* colormap) {
    Palette* oldPalette = m_sprite->palette(0);
    Palette newPalette(0, colormap->ColorCount);

    for (int i=0; i<colormap->ColorCount; ++i) {
      newPalette.setEntry(i, colormap2rgba(colormap, i));;
    }

    Remap remap = create_remap_to_change_palette(
      oldPalette, &newPalette, m_bgIndex,
      m_opaque); // We cannot remap the transparent color if the
                 // sprite isn't opaque, because we
                 // cannot write the header again

    for (Cel* cel : m_sprite->uniqueCels())
      doc::remap_image(cel->image(), remap);

    m_sprite->setPalette(&newPalette, false);
  }

  void reduceToAnOptimizedPalette() {
    render::PaletteOptimizer optimizer;
    const Palette* palette = m_sprite->palette(0);

    // Feed the palette optimizer with pixels inside frameBounds
    for (int i=0; i<palette->size(); ++i) {
      optimizer.feedWithRgbaColor(palette->getEntry(i));
    }

    Palette newPalette(0, 256);
    optimizer.calculate(&newPalette, m_bgIndex);
    m_sprite->setPalette(&newPalette, false);
  }

  FileOp* m_fop;
  GifFileType* m_gifFile;
  int m_fd;
  size_t m_filesize;
  std::unique_ptr<Sprite> m_sprite;
  gfx::Rect m_spriteBounds;
  LayerImage* m_layer;
  int m_frameNum;
  bool m_opaque;
  DisposalMethod m_disposalMethod;
  int m_bgIndex;
  int m_localTransparentIndex;
  int m_frameDelay;
  ImageRef m_currentImage;
  ImageRef m_previousImage;
  Remap m_remap;
  bool m_hasLocalColormaps;     // Indicates that this fila contains local colormaps

  // This is a copy of the first local color map. It's used to see if
  // all local colormaps are the same, so we can use it as a global
  // colormap.
  ColorMapObject* m_firstLocalColormap;
};

bool GifFormat::onLoad(FileOp* fop)
{
  // The filesize is used only to report some progress when we decode
  // the GIF file.
  size_t filesize = base::file_size(fop->filename());

#if GIFLIB_MAJOR >= 5
  int errCode = 0;
#endif
  int fd = open_file_descriptor_with_exception(fop->filename(), "rb");
  GifFilePtr gif_file(DGifOpenFileHandle(fd
#if GIFLIB_MAJOR >= 5
                                         , &errCode
#endif
                                         ), &DGifCloseFile);

  if (!gif_file) {
    fop->setError("Error loading GIF header.\n");
    return false;
  }

  GifDecoder decoder(fop, gif_file, fd, filesize);
  if (decoder.decode()) {
    fop->createDocument(decoder.releaseSprite());
    return true;
  }
  else
    return false;
}

#ifdef ENABLE_SAVE

// Our stragegy to encode GIF files depends of the sprite color mode:
//
// 1) If the sprite is indexed, we have two paths:
//    * For opaque an opaque sprite we can save it as it is (with the
//      same indexes/pixels and same color palette). This brings us
//      the best possible to compress the GIF file (using the best
//      disposal method to update only the differences between each
//      frame).
//    * For transparent sprites we offer to the user the option to
//      preserve the original palette or not
//      (m_preservePaletteOrders). If the palette must be preserve,
//      some level of compression will be sacrificed.
//
// 2) For RGB sprites the palette is created on each frame depending
//    on the updated rectangle between frames, i.e. each to new frame
//    incorporates a minimal rectangular region with changes from the
//    previous frame, we can calculate the palette required for this
//    rectangle and use it as a local colormap for the frame (if each
//    frame uses previous color in the palette there is no need to
//    introduce a new palette).
//
// Note: In the following algorithm you will find the "pixel clearing"
// term, this happens when we need to clear an opaque color with the
// gif transparent bg color. This is the worst possible case, because
// on transparent gif files, the only way to get the transparent color
// (bg color) is using the RESTORE_BGCOLOR disposal method (so we lost
// the chance to use DO_NOT_DISPOSE in these cases).
//
class GifEncoder {
public:
  typedef int gifframe_t;

  GifEncoder(FileOp* fop, GifFileType* gifFile)
    : m_fop(fop)
    , m_gifFile(gifFile)
    , m_document(fop->document())
    , m_sprite(fop->document()->sprite())
    , m_spriteBounds(m_sprite->bounds())
    , m_hasBackground(m_sprite->isOpaque())
    , m_bitsPerPixel(1)
    , m_globalColormap(nullptr)
    , m_globalColormapPalette(*m_sprite->palette(0)) {

    const auto gifOptions = std::static_pointer_cast<GifOptions>(fop->formatOptions());

    LOG("GIF: Saving with options: interlaced=%d loop=%d\n",
        gifOptions->interlaced(), gifOptions->loop());

    m_interlaced = gifOptions->interlaced();
    m_loop = (gifOptions->loop() ? 0: -1);

    if (m_sprite->pixelFormat() == PixelFormat::IMAGE_INDEXED) {
      // For indexed and opaque sprite, we can preserve the exact
      // palette order without lossing compression rate.
      if (m_hasBackground) {
        m_preservePaletteOrder = true;
      }
      // Only for transparent indexed images the user can choose to
      // preserve or not the palette order.
      else {
        m_preservePaletteOrder = gifOptions->preservePaletteOrder();
      }
    }
    // For RGB images we don't preserve the palette order (palettes
    // will be generated as they are needed to optimize the file
    // size).
    else {
      m_preservePaletteOrder = false;
    }

    m_lastFrameBounds = m_spriteBounds;
    m_lastDisposal = DisposalMethod::NONE;

    if (m_sprite->pixelFormat() == IMAGE_INDEXED) {
      for (Palette* palette : m_sprite->getPalettes()) {
        int bpp = GifBitSizeLimited(palette->size());
        m_bitsPerPixel = MAX(m_bitsPerPixel, bpp);
      }
    }
    else {
      m_bitsPerPixel = 8;
    }

    if (m_sprite->pixelFormat() == IMAGE_INDEXED &&
        m_sprite->getPalettes().size() == 1) {
      // If some layer has opacity < 255 or a different blend mode, we
      // need to create color palettes.
      bool quantizeColormaps = false;
      for (const Layer* layer : m_sprite->allVisibleLayers()) {
        if (layer->isVisible() && layer->isImage()) {
          const LayerImage* imageLayer = static_cast<const LayerImage*>(layer);
          if (imageLayer->opacity() < 255 ||
              imageLayer->blendMode() != BlendMode::NORMAL) {
            quantizeColormaps = true;
            break;
          }
        }
      }

      if (!quantizeColormaps) {
        m_globalColormap = createColorMap(&m_globalColormapPalette);
        m_bgIndex = m_sprite->transparentColor();
      }
      else
        m_bgIndex = 0;
    }
    else {
      m_bgIndex = 0;
    }

    // This is the transparent index to use as "local transparent"
    // index for each gif frame. In case that we use a global colormap
    // (and we don't need to preserve the original palette), we can
    // try to find a place for a global transparent index.
    m_transparentIndex = (m_hasBackground ? -1: m_bgIndex);
    if (m_globalColormap) {
      // The variable m_globalColormap is != nullptr only on indexed images
      ASSERT(m_sprite->pixelFormat() == IMAGE_INDEXED);

      const Palette* pal = m_sprite->palette(0);
      bool maskColorFounded = false;
      for (int i=0; i<pal->size(); i++) {
        if (doc::rgba_geta(pal->getEntry(i)) == 0) {
          maskColorFounded = true;
          m_transparentIndex = i;
          break;
        }
      }

#if 0
      // If the palette contains room for one extra color for the
      // mask, we can use that index.
      if (!maskColorFounded && pal->size() < 256) {
        maskColorFounded = true;

        Palette newPalette(*pal);
        newPalette.addEntry(0);
        ASSERT(newPalette.size() <= 256);

        m_transparentIndex = newPalette.size() - 1;
        m_globalColormapPalette = newPalette;
        m_globalColormap = createColorMap(&m_globalColormapPalette);
      }
      else
#endif
      if (// If all colors are opaque/used in the sprite
        !maskColorFounded &&
        // We aren't obligated to preserve the original palette
        !m_preservePaletteOrder &&
        // And the sprite is transparent
        !m_hasBackground) {
        // We create a new palette with 255 colors + one extra entry
        // for the transparent color
        Palette newPalette(0, 255);
        render::create_palette_from_sprite(
          m_sprite,
          0,
          totalFrames()-1,
          false,
          &newPalette,
          nullptr,
          m_fop->newBlend(),
          false); // Do not add the transparent color yet

        // We will use the last palette entry (e.g. index=255) as the
        // transparent index
        newPalette.addEntry(0);
        ASSERT(newPalette.size() <= 256);

        m_transparentIndex = newPalette.size() - 1;
        m_globalColormapPalette = newPalette;
        m_globalColormap = createColorMap(&m_globalColormapPalette);
      }
    }

    // Create the 3 temporary images (previous/current/next) to
    // compare pixels between them.
    for (int i=0; i<3; ++i)
      m_images[i].reset(Image::create(IMAGE_RGB,
                                      m_spriteBounds.w,
                                      m_spriteBounds.h));
  }

  ~GifEncoder() {
    if (m_globalColormap)
      GifFreeMapObject(m_globalColormap);
  }

  bool encode() {
    writeHeader();
    if (m_loop >= 0)
      writeLoopExtension();

    // Previous and next images are used to decide the best disposal
    // method (e.g. if it's more convenient to restore the background
    // color or to restore the previous frame to reach the next one).
    m_previousImage = m_images[0].get();
    m_currentImage = m_images[1].get();
    m_nextImage = m_images[2].get();

    auto frame_beg = m_fop->roi().selectedFrames().begin();
#if _DEBUG
    auto frame_end = m_fop->roi().selectedFrames().end();
#endif
    auto frame_it = frame_beg;

    // In this code "gifFrame" will be the GIF frame, and "frame" will
    // be the doc::Sprite frame.
    gifframe_t nframes = totalFrames();
    for (gifframe_t gifFrame=0; gifFrame<nframes; ++gifFrame) {
      ASSERT(frame_it != frame_end);
      frame_t frame = *frame_it;
      ++frame_it;

      if (gifFrame == 0)
        renderFrame(frame, m_nextImage);
      else
        std::swap(m_previousImage, m_currentImage);

      // Render next frame
      std::swap(m_currentImage, m_nextImage);
      if (gifFrame+1 < nframes)
        renderFrame(*frame_it, m_nextImage);

      gfx::Rect frameBounds = m_spriteBounds;
      DisposalMethod disposal = DisposalMethod::DO_NOT_DISPOSE;

      // Creation of the deltaImage (difference image result respect
      // to current VS previous frame image).  At the same time we
      // must scan the next image, to check if some pixel turns to
      // transparent (0), if the case, we need to force disposal
      // method of the current image to RESTORE_BG.  Further, at the
      // same time, we must check if we can go without color zero (0).

      calculateDeltaImageFrameBoundsDisposal(gifFrame, frameBounds, disposal);

      writeImage(gifFrame, frame, frameBounds, disposal,
                 // Only the last frame in the animation needs the fix
                 (fix_last_frame_duration && gifFrame == nframes-1));

      m_fop->setProgress(double(gifFrame+1) / double(nframes));
    }
    return true;
  }

private:

  void calculateDeltaImageFrameBoundsDisposal(gifframe_t gifFrame,
                                              gfx::Rect& frameBounds,
                                              DisposalMethod& disposal) {
    if (gifFrame == 0) {
      m_deltaImage.reset(Image::createCopy(m_currentImage));
      frameBounds = m_spriteBounds;

      // The first frame (frame 0) is good to force to disposal = DO_NOT_DISPOSE,
      // but when the next frame (frame 1) has a "pixel clearing",
      // we must change disposal to RESTORE_BGCOLOR.

      // "Pixel clearing" detection:
      if (!m_hasBackground) {
        const LockImageBits<RgbTraits> bits2(m_currentImage);
        const LockImageBits<RgbTraits> bits3(m_nextImage);
        typename LockImageBits<RgbTraits>::const_iterator it2, it3, end2, end3;
        for (it2 = bits2.begin(), end2 = bits2.end(),
             it3 = bits3.begin(), end3 = bits3.end();
              it2 != end2 && it3 != end3; ++it2, ++it3) {
          if (*it2 != 0 && *it3 == 0) {
            disposal = DisposalMethod::RESTORE_BGCOLOR;
            break;
          }
        }
      }
    }
    else {
      int x1 = 0;
      int y1 = 0;
      int x2 = 0;
      int y2 = 0;
      // When m_lastDisposal was RESTORE_BGBOLOR it implies
      // we will have to cover with colors the entire previous frameBounds plus
      // the current frameBounds due to color changes, so we must start with
      // a frameBounds equal to the previous frame iteration (saved in m_lastFrameBounds).
      // Then we must cover all the resultant frameBounds with full color
      // in m_currentImage, the output image will be saved in deltaImage.
      if (m_lastDisposal == DisposalMethod::RESTORE_BGCOLOR) {
        x1 = m_lastFrameBounds.x;
        y1 = m_lastFrameBounds.y;
        x2 = m_lastFrameBounds.x + m_lastFrameBounds.w - 1;
        y2 = m_lastFrameBounds.y + m_lastFrameBounds.h - 1;
      }
      else {
        x1 = m_spriteBounds.w - 1;
        y1 = m_spriteBounds.h - 1;
      }

      int i = 0;
      int x, y;
      const LockImageBits<RgbTraits> bits1(m_previousImage);
      const LockImageBits<RgbTraits> bits2(m_currentImage);
      const LockImageBits<RgbTraits> bits3(m_nextImage);
      m_deltaImage.reset(Image::create(PixelFormat::IMAGE_RGB, m_spriteBounds.w, m_spriteBounds.h));
      clear_image(m_deltaImage.get(), 0);
      LockImageBits<RgbTraits> deltaBits(m_deltaImage.get());
      typename LockImageBits<RgbTraits>::iterator deltaIt;
      typename LockImageBits<RgbTraits>::const_iterator it1, it2, it3, end1, end2, end3, deltaEnd;

      for (it1 = bits1.begin(), end1 = bits1.end(),
           it2 = bits2.begin(), end2 = bits2.end(),
           it3 = bits3.begin(), end2 = bits3.end(),
           deltaIt = deltaBits.begin();
           it1 != end1 && it2 != end2; ++it1, ++it2, ++it3, ++deltaIt, ++i) {
        x = i % m_spriteBounds.w;
        y = i / m_spriteBounds.w;
        // While we are checking color differences,
        // we enlarge the frameBounds where the color differences take place
        if (*it1 != *it2 || *it3 == 0) {
          *deltaIt = *it2;
          if (x < x1) x1 = x;
          if (x > x2) x2 = x;
          if (y < y1) y1 = y;
          if (y > y2) y2 = y;
        }

        // We need to change disposal mode DO_NOT_DISPOSE to RESTORE_BGCOLOR only
        // if we found a "pixel clearing" in the next Image. RESTORE_BGCOLOR is
        // our way to clear pixels.
        if (*it2 != 0 && *it3 == 0) {
          disposal = DisposalMethod::RESTORE_BGCOLOR;
        }
      }

      if (m_preservePaletteOrder)
        disposal = DisposalMethod::RESTORE_BGCOLOR;
      frameBounds = gfx::Rect(x1, y1, x2-x1+1, y2-y1+1);

      // We need to conditionate the deltaImage to the next step: 'writeImage()'
      // To do it, we need to crop deltaImage in frameBounds.
      // If disposal method changed to RESTORE_BGCOLOR deltaImage we need to reproduce ALL the colors of m_currentImage
      // contained in frameBounds (so, we will overwrite delta image with a cropped current image).
      // In the other hand, if disposal is still DO_NOT_DISPOSAL, delta image will be a cropped image
      // from itself in frameBounds.
      if (disposal == DisposalMethod::RESTORE_BGCOLOR || m_lastDisposal == DisposalMethod::RESTORE_BGCOLOR) {
        m_deltaImage.reset(crop_image(m_currentImage, frameBounds, 0));
        m_lastFrameBounds = frameBounds;
      }
      else {
        m_deltaImage.reset(crop_image(m_deltaImage.get(), frameBounds, 0));
        disposal = DisposalMethod::DO_NOT_DISPOSE;
      }
    }

    // TODO We could join both frames in a longer one (with more duration)
    if (frameBounds.isEmpty())
      frameBounds = gfx::Rect(0, 0, 1, 1);

    m_lastDisposal = disposal;
  }

  doc::frame_t totalFrames() const {
    return m_fop->roi().frames();
  }

  void writeHeader() {
    if (EGifPutScreenDesc(m_gifFile,
                          m_spriteBounds.w,
                          m_spriteBounds.h,
                          m_bitsPerPixel,
                          m_bgIndex, m_globalColormap) == GIF_ERROR)
      throw Exception("Error writing GIF header.\n");
  }

  void writeLoopExtension() {
#if GIFLIB_MAJOR >= 5
    if (EGifPutExtensionLeader(m_gifFile, APPLICATION_EXT_FUNC_CODE) == GIF_ERROR)
      throw Exception("Error writing GIF graphics extension record (header section).");

    unsigned char extension_bytes[11];
    memcpy(extension_bytes, "NETSCAPE2.0", 11);
    if (EGifPutExtensionBlock(m_gifFile, 11, extension_bytes) == GIF_ERROR)
      throw Exception("Error writing GIF graphics extension record (first block).");

    extension_bytes[0] = 1;
    extension_bytes[1] = (m_loop & 0xff);
    extension_bytes[2] = (m_loop >> 8) & 0xff;
    if (EGifPutExtensionBlock(m_gifFile, 3, extension_bytes) == GIF_ERROR)
      throw Exception("Error writing GIF graphics extension record (second block).");

    if (EGifPutExtensionTrailer(m_gifFile) == GIF_ERROR)
      throw Exception("Error writing GIF graphics extension record (trailer section).");

#else
    unsigned char extension_bytes[11];

    memcpy(extension_bytes, "NETSCAPE2.0", 11);
    if (EGifPutExtensionFirst(m_gifFile, APPLICATION_EXT_FUNC_CODE, 11, extension_bytes) == GIF_ERROR)
      throw Exception("Error writing GIF graphics extension record.\n");

    extension_bytes[0] = 1;
    extension_bytes[1] = (m_loop & 0xff);
    extension_bytes[2] = (m_loop >> 8) & 0xff;
    if (EGifPutExtensionNext(m_gifFile, APPLICATION_EXT_FUNC_CODE, 3, extension_bytes) == GIF_ERROR)
      throw Exception("Error writing GIF graphics extension record.\n");

    if (EGifPutExtensionLast(m_gifFile, APPLICATION_EXT_FUNC_CODE, 0, NULL) == GIF_ERROR)
      throw Exception("Error writing GIF graphics extension record.\n");
#endif
  }

  // Writes graphics extension record (to save the duration of the
  // frame and maybe the transparency index).
  void writeExtension(const gifframe_t gifFrame,
                      const frame_t frame,
                      const int transparentIndex,
                      const DisposalMethod disposalMethod,
                      const bool fixDuration) {
    unsigned char extension_bytes[5];
    int frameDelay = m_sprite->frameDuration(frame) / 10;

    // Fix duration for Twitter. It looks like the last frame must be
    // 1/4 of its duration for some strange reason in the Twitter
    // conversion from GIF to video.
    if (fixDuration)
      frameDelay = MAX(2, frameDelay/4);
    if (fix_last_frame_duration)
      frameDelay = MAX(2, frameDelay);

    extension_bytes[0] = (((int(disposalMethod) & 7) << 2) |
                          (transparentIndex >= 0 ? 1: 0));
    extension_bytes[1] = (frameDelay & 0xff);
    extension_bytes[2] = (frameDelay >> 8) & 0xff;
    extension_bytes[3] = (transparentIndex >= 0 ? transparentIndex: 0);

    if (EGifPutExtension(m_gifFile, GRAPHICS_EXT_FUNC_CODE, 4, extension_bytes) == GIF_ERROR)
      throw Exception("Error writing GIF graphics extension record for frame %d.\n", gifFrame);
  }

  static gfx::Rect calculateFrameBounds(Image* a, Image* b) {
    gfx::Rect frameBounds;
    int x1, y1, x2, y2;

    if (get_shrink_rect2(&x1, &y1, &x2, &y2, a, b)) {
      frameBounds.x = x1;
      frameBounds.y = y1;
      frameBounds.w = x2 - x1 + 1;
      frameBounds.h = y2 - y1 + 1;
    }

    return frameBounds;
  }


  void writeImage(const gifframe_t gifFrame,
                  const frame_t frame,
                  const gfx::Rect& frameBounds,
                  const DisposalMethod disposal,
                  const bool fixDuration) {
    Palette framePalette;
    if (m_globalColormap)
      framePalette = m_globalColormapPalette;
    else
      framePalette = calculatePalette(frameBounds, disposal);

    RgbMap rgbmap;
    rgbmap.regenerate(&framePalette, m_transparentIndex);

    ImageRef frameImage(Image::create(IMAGE_INDEXED,
                                      frameBounds.w,
                                      frameBounds.h,
                                      m_frameImageBuf));

    // Every frame might use a small portion of the global palette,
    // to optimize the gif file size, we will analize which colors
    // will be used in each processed frame.
    PalettePicks usedColors(framePalette.size());

    {
      const LockImageBits<RgbTraits> srcBits(m_deltaImage.get());
      LockImageBits<IndexedTraits> dstBits(frameImage.get());

      auto srcIt = srcBits.begin();
      auto dstIt = dstBits.begin();

      for (int y=0; y<frameBounds.h; ++y) {
        for (int x=0; x<frameBounds.w; ++x, ++srcIt, ++dstIt) {
          ASSERT(srcIt != srcBits.end());
          ASSERT(dstIt != dstBits.end());

          color_t color = *srcIt;
          int i;

          if (rgba_geta(color) >= 128) {
            i = framePalette.findExactMatch(
              rgba_getr(color),
              rgba_getg(color),
              rgba_getb(color),
              255,
              m_transparentIndex);
            if (i < 0)
              i = rgbmap.mapColor(rgba_getr(color),
                                  rgba_getg(color),
                                  rgba_getb(color), 255);
          }
          else {
            if (m_transparentIndex >= 0)
              i = m_transparentIndex;
            else
              i = m_bgIndex;
          }

          ASSERT(i >= 0);

          // This can happen when transparent color is outside the
          // palette range (TODO something that shouldn't be possible
          // from the program).
          if (i >= usedColors.size())
            usedColors.resize(i+1);
          usedColors[i] = true;

          *dstIt = i;
        }
      }
    }

    int usedNColors = usedColors.picks();

    Remap remap(256);
    for (int i=0; i<remap.size(); ++i)
      remap.map(i, i);

    int localTransparent = m_transparentIndex;
    ColorMapObject* colormap = m_globalColormap;
    if (!colormap) {
      Palette reducedPalette(0, usedNColors);

      for (int i=0, j=0; i<framePalette.size(); ++i) {
        if (usedColors[i]) {
          reducedPalette.setEntry(j, framePalette.getEntry(i));
          remap.map(i, j);
          ++j;
        }
      }

      colormap = createColorMap(&reducedPalette);
      if (localTransparent >= 0)
        localTransparent = remap[localTransparent];
    }

    if (localTransparent >= 0 && m_transparentIndex != localTransparent)
      remap.map(m_transparentIndex, localTransparent);

    // Write extension record.
    writeExtension(gifFrame, frame, localTransparent,
                   disposal, fixDuration);

    // Write the image record.
    if (EGifPutImageDesc(m_gifFile,
                         frameBounds.x, frameBounds.y,
                         frameBounds.w, frameBounds.h,
                         m_interlaced ? 1: 0,
                         (colormap != m_globalColormap ? colormap: nullptr)) == GIF_ERROR) {
      throw Exception("Error writing GIF frame %d.\n", gifFrame);
    }

    std::vector<uint8_t> scanline(frameBounds.w);

    // Write the image data (pixels).
    if (m_interlaced) {
      // Need to perform 4 passes on the images.
      for (int i=0; i<4; ++i)
        for (int y=interlaced_offset[i]; y<frameBounds.h; y+=interlaced_jumps[i]) {
          IndexedTraits::address_t addr =
            (IndexedTraits::address_t)frameImage->getPixelAddress(0, y);

          for (int i=0; i<frameBounds.w; ++i, ++addr)
            scanline[i] = remap[*addr];

          if (EGifPutLine(m_gifFile, &scanline[0], frameBounds.w) == GIF_ERROR)
            throw Exception("Error writing GIF image scanlines for frame %d.\n", gifFrame);
        }
    }
    else {
      // Write all image scanlines (not interlaced in this case).
      for (int y=0; y<frameBounds.h; ++y) {
        IndexedTraits::address_t addr =
          (IndexedTraits::address_t)frameImage->getPixelAddress(0, y);

        for (int i=0; i<frameBounds.w; ++i, ++addr)
          scanline[i] = remap[*addr];

        if (EGifPutLine(m_gifFile, &scanline[0], frameBounds.w) == GIF_ERROR)
          throw Exception("Error writing GIF image scanlines for frame %d.\n", gifFrame);
      }
    }

    if (colormap && colormap != m_globalColormap)
      GifFreeMapObject(colormap);
  }

  Palette calculatePalette(const gfx::Rect& frameBounds,
                           const DisposalMethod disposal) {
    // First, we must check the palette color count in m_deltaImage (our best shot
    // to find the smaller palette color count)
    Palette pal(createOptimizedPalette(m_deltaImage.get(), m_deltaImage->bounds(), 256));
    if (pal.size() == 256) {
      // Here the palette has 256 colors, there is no place to include
      // the 0 color (createOptimizedPalette() doesn't create an entry
      // for it).
      //
      // We have two paths:
      // 1- Giving a try to palette generation on m_currentImage in frameBouns limits.
      // 2- If the previous step is not possible (color count > 256), we will to start
      //    to approximate colors from m_deltaImage with some criterion. Final target:
      //    to approximate the palette to 255 colors + clear color (0)).

      // 1- Giving a try to palette generation on m_currentImage in frameBouns limits.
      // if disposal == RESTORE_BGCOLOR m_deltaImage already is a cropped copy of m_currentImage.
      Palette auxPalette;
      if (disposal == DisposalMethod::DO_NOT_DISPOSE)
        auxPalette = createOptimizedPalette(m_currentImage, frameBounds, 257);
      else
        auxPalette = pal;

      if (auxPalette.size() <= 256) {
        // We are fine with color count in m_currentImage contained in
        // frameBounds (we got 256 or less colors):
        m_transparentIndex = -1;
        pal = auxPalette;
        if (disposal == DisposalMethod::DO_NOT_DISPOSE)
          m_deltaImage.reset(crop_image(m_currentImage, frameBounds, 0));
      }
      else {
        // 2- If the previous step fails, we will to start to approximate colors from m_deltaImage
        //    with some criterion:

        // Final target: to approximate the palette to 255 colors + clear color (0)).
        // CRITERION:
        // TODO: develop a better criterion, based on big color areas, or the opposite: do not take count of lone pixels.
        // Find a palette of high precision 220 colors in the inner border square
        // into m_deltaImage, then find 35 more truncated colors in the center square.
        //
        //  m_currentImage__      __ m_deltaImage (same rectangle size as frameBounds)
        //                 |    |
        //   --------------*----|------------
        //  |                   |           |
        //  |     --------------*-          |
        //  |    |    ________    |         |
        //  |    |   |        |   |         |
        //  |    |   |        | *---------------inner border square (we will collect
        //  |    |   |________|   |         |   high precision colors from this area)
        //  |    |________________|         |
        //  |                               |
        //  |_______________________________|
        //

        const gfx::Size deltaSize = m_deltaImage->size();
        int thicknessTop = deltaSize.h / 4;
        int thicknessLeft = deltaSize.w / 4;
        int lastThicknessTop = thicknessTop;
        int lastThicknessLeft = thicknessLeft;
        pal.resize(255);
        while (true) {

          render::PaletteOptimizer optimizer;
          gfx::Rect auxRect(0, 0, deltaSize.w, thicknessTop);
          optimizer.feedWithImage(m_deltaImage.get(), auxRect, false);
          //      ----------------
          //     |________________|
          //     |   |        |   |
          //     |   |        |   |
          //     |   |________|   |
          //     |________________|

          auxRect = gfx::Rect(0, deltaSize.h - thicknessTop - 1, deltaSize.w, thicknessTop);
          optimizer.feedWithImage(m_deltaImage.get(), auxRect, false);
          //      ----------------
          //     |    ________    |
          //     |   |        |   |
          //     |   |        |   |
          //     |___|________|___|
          //     |________________|

          auxRect = gfx::Rect(0, thicknessTop, thicknessLeft, deltaSize.h - 2 * thicknessTop);
          optimizer.feedWithImage(m_deltaImage.get(), auxRect, false);
          //      ----------------
          //     |____________    |
          //     |   |        |   |
          //     |   |        |   |
          //     |___|________|   |
          //     |________________|

          auxRect = gfx::Rect(deltaSize.w - thicknessLeft - 1, thicknessTop, thicknessLeft, deltaSize.h - 2 * thicknessTop);
          optimizer.feedWithImage(m_deltaImage.get(), auxRect, false);
          //      ----------------
          //     |   _____________|
          //     |   |        |   |
          //     |   |        |   |
          //     |   |________|___|
          //     |________________|

          if (optimizer.isHighPrecision()) {
            if (optimizer.highPrecisionSize() >= 220) { // 220 colors is an arbitrary number
              lastThicknessTop = thicknessTop;
              lastThicknessLeft = thicknessLeft;
              // Put the high precision colors to the palette:
              optimizer.calculate(&pal, -1);
              break;
            }
            else if (deltaSize.h - thicknessTop * 2 <= deltaSize.h / 4 ||
                     deltaSize.w - thicknessLeft * 2 <= deltaSize.w / 4) {
              // Put the high precision colors to the palette:
              optimizer.calculate(&pal, -1);
              break;
            }
            if (thicknessTop * 3 >= deltaSize.h || thicknessLeft * 3 >= deltaSize.w)
              break;
            thicknessTop += thicknessTop / 2;
            thicknessLeft += thicknessLeft / 2;
          }
          else {
            if (thicknessTop <= deltaSize.h / 16 ||
                thicknessLeft <= deltaSize.w / 16) {
              // TODO: we need to catch this LAST posibility.
              // Put the high precision colors to the palette:
              optimizer.calculate(&pal, -1);
              break;
            }
            thicknessTop -= thicknessTop / 2;
            thicknessLeft -= thicknessLeft / 2;
          }

          lastThicknessTop = thicknessTop;
          lastThicknessLeft = thicknessLeft;
        }
        gfx::Rect centerRect(lastThicknessLeft,
                             lastThicknessTop,
                             deltaSize.w - 2 * lastThicknessLeft,
                             deltaSize.h - 2 * lastThicknessTop);
        // Find the center colors (aproximation colors)
        if (pal.size() < 255) {
          Palette centerPalette(0, 255 - pal.size());
          centerPalette = createOptimizedPalette(m_deltaImage.get(),
                                                 centerRect, 255 - pal.size());

          // Adding the center colors to pal + transparent color
          for (int i=0; i < centerPalette.size(); i++)
            pal.addEntry(centerPalette.getEntry(i));
        }
        ASSERT(pal.size() <= 255);
        pal.addEntry(0);
        m_transparentIndex = pal.size() - 1;
      }
    }
    // We are fine, we got 255 or less, there is room for the transparent color
    else if (pal.size() <= 255) {
      pal.addEntry(0);
      m_transparentIndex = pal.size() - 1;
    }
    return pal;
  }

  static Palette createOptimizedPalette(const Image* image,
                                        const gfx::Rect& bounds,
                                        const int ncolors) {
    render::PaletteOptimizer optimizer;

    // Feed the palette optimizer with pixels inside the given bounds
    for (const auto& color : LockImageBits<RgbTraits>(image, bounds)) {
      if (rgba_geta(color) >= 128) // Note: the mask color won't be part of the final palette
        optimizer.feedWithRgbaColor(
          rgba(rgba_getr(color),
               rgba_getg(color),
               rgba_getb(color), 255));
    }

    Palette palette(0, ncolors);
    optimizer.calculate(&palette, -1);
    return palette;
  }

  void renderFrame(frame_t frame, Image* dst) {
    render::Render render;
    render.setNewBlend(m_fop->newBlend());

    render.setBgType(render::BgType::NONE);
    clear_image(dst, 0);
    render.renderSprite(dst, m_sprite, frame);
  }

private:

  ColorMapObject* createColorMap(const Palette* palette) {
    int n = 1 << GifBitSizeLimited(palette->size());
    ColorMapObject* colormap = GifMakeMapObject(n, nullptr);

    // Color space conversions
    ConvertCS convert = convert_from_custom_to_srgb(
      m_document->osColorSpace());

    for (int i=0; i<n; ++i) {
      color_t color;
      if (i < palette->size())
        color = palette->getEntry(i);
      else
        color = rgba(0, 0, 0, 255);

      color = convert(color);

      colormap->Colors[i].Red   = rgba_getr(color);
      colormap->Colors[i].Green = rgba_getg(color);
      colormap->Colors[i].Blue  = rgba_getb(color);
    }

    return colormap;
  }

  FileOp* m_fop;
  GifFileType* m_gifFile;
  const Doc* m_document;
  const Sprite* m_sprite;
  gfx::Rect m_spriteBounds;
  bool m_hasBackground;
  int m_bgIndex;
  int m_transparentIndex;
  int m_bitsPerPixel;
  // Global palette to use on all frames, or nullptr in case that we
  // have to quantize the palette on each frame.
  ColorMapObject* m_globalColormap;
  Palette m_globalColormapPalette;
  bool m_interlaced;
  int m_loop;
  bool m_preservePaletteOrder;
  gfx::Rect m_lastFrameBounds;
  DisposalMethod m_lastDisposal;
  ImageBufferPtr m_frameImageBuf;
  ImageRef m_images[3];
  Image* m_previousImage;
  Image* m_currentImage;
  Image* m_nextImage;
  std::unique_ptr<Image> m_deltaImage;
};

bool GifFormat::onSave(FileOp* fop)
{
#if GIFLIB_MAJOR >= 5
  int errCode = 0;
#endif
  int fd = base::open_file_descriptor_with_exception(fop->filename(), "wb");
  GifFilePtr gif_file(EGifOpenFileHandle(fd
#if GIFLIB_MAJOR >= 5
                                         , &errCode
#endif
                                         ), &EGifCloseFile);

  if (!gif_file)
    throw Exception("Error creating GIF file.\n");

  GifEncoder encoder(fop, gif_file);
  bool result = encoder.encode();
  if (result)
    base::sync_file_descriptor(fd);
  return result;
}

#endif  // ENABLE_SAVE

FormatOptionsPtr GifFormat::onAskUserForFormatOptions(FileOp* fop)
{
  auto opts = fop->formatOptionsOfDocument<GifOptions>();
#ifdef ENABLE_UI
  if (fop->context() && fop->context()->isUIAvailable()) {
    try {
      auto& pref = Preferences::instance();

      if (pref.isSet(pref.gif.interlaced))
        opts->setInterlaced(pref.gif.interlaced());
      if (pref.isSet(pref.gif.loop))
        opts->setLoop(pref.gif.loop());
      if (pref.isSet(pref.gif.preservePaletteOrder))
        opts->setPreservePaletteOrder(pref.gif.preservePaletteOrder());

      if (pref.gif.showAlert()) {
        app::gen::GifOptions win;
        win.interlaced()->setSelected(opts->interlaced());
        win.loop()->setSelected(opts->loop());
        win.preservePaletteOrder()->setSelected(opts->preservePaletteOrder());

        if (fop->document()->sprite()->pixelFormat() == PixelFormat::IMAGE_INDEXED &&
            !fop->document()->sprite()->isOpaque())
          win.preservePaletteOrder()->setEnabled(true);
        else {
          win.preservePaletteOrder()->setEnabled(false);
          if (fop->document()->sprite()->pixelFormat() == PixelFormat::IMAGE_INDEXED && fop->document()->sprite()->isOpaque())
            win.preservePaletteOrder()->setSelected(true);
          else
            win.preservePaletteOrder()->setSelected(false);
        }

        win.openWindowInForeground();

        if (win.closer() == win.ok()) {
          pref.gif.interlaced(win.interlaced()->isSelected());
          pref.gif.loop(win.loop()->isSelected());
          pref.gif.preservePaletteOrder(win.preservePaletteOrder()->isSelected());
          pref.gif.showAlert(!win.dontShow()->isSelected());

          opts->setInterlaced(pref.gif.interlaced());
          opts->setLoop(pref.gif.loop());
          opts->setPreservePaletteOrder(pref.gif.preservePaletteOrder());
        }
        else {
          opts.reset();
        }
      }
    }
    catch (std::exception& e) {
      Console::showException(e);
      return std::shared_ptr<GifOptions>(nullptr);
    }
  }
#endif // ENABLE_UI
  return opts;
}

} // namespace app
