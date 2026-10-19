#include "tiio_ora.h"

#include "timage_io.h"
#include "timageinfo.h"
#include "tpixel.h"
#include "trasterimage.h"

#include <QImage>
#include <QPainter>

//-----------------------------------------------------------------------------

namespace {

QImage decodePng(const TOraParser &parser, const std::string &entry) {
  std::vector<unsigned char> bytes;
  if (!parser.readEntry(entry, bytes)) return QImage();
  QImage img = QImage::fromData(bytes.data(), (int)bytes.size(), "PNG");
  if (img.isNull()) return img;
  return img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

// Toonz rasters are premultiplied like Format_ARGB32_Premultiplied, but are
// stored bottom-up.
TRasterImageP toRasterImage(const QImage &canvas) {
  const int lx = canvas.width(), ly = canvas.height();
  TRaster32P ras(lx, ly);
  ras->lock();
  for (int y = 0; y < ly; ++y) {
    const QRgb *src = (const QRgb *)canvas.constScanLine(y);
    TPixel32 *dst   = ras->pixels(ly - 1 - y);
    for (int x = 0; x < lx; ++x) {
      QRgb c = src[x];
      dst[x] = TPixel32(qRed(c), qGreen(c), qBlue(c), qAlpha(c));
    }
  }
  ras->unlock();
  return TRasterImageP(ras);
}

}  // namespace

//=============================================================================
// TLevelReaderOra
//-----------------------------------------------------------------------------

TLevelReaderOra::TLevelReaderOra(const TFilePath &path)
    : TLevelReader(path)
    , m_parser(0)
    , m_layerIndex(TOraParser::layerIndex(path)) {
  m_parser = new TOraParser(path);  // throws TImageException
  if (m_layerIndex >= m_parser->getLayerCount()) {
    delete m_parser;
    m_parser = 0;
    throw TImageException(path, "OpenRaster layer index out of range");
  }

  m_info                   = new TImageInfo();
  m_info->m_lx             = m_parser->getWidth();
  m_info->m_ly             = m_parser->getHeight();
  m_info->m_dpix           = m_parser->getXRes() > 0 ? m_parser->getXRes() : 0;
  m_info->m_dpiy           = m_parser->getYRes() > 0 ? m_parser->getYRes() : 0;
  m_info->m_bitsPerSample  = 8;
  m_info->m_samplePerPixel = 4;
}

//-----------------------------------------------------------------------------

TLevelReaderOra::~TLevelReaderOra() { delete m_parser; }

//-----------------------------------------------------------------------------

TLevelP TLevelReaderOra::loadInfo() {
  TLevelP level;
  std::wstring name = m_layerIndex >= 0
                          ? m_parser->getLayer(m_layerIndex).name
                          : m_parser->getPath().getWideName();
  level->setName(QString::fromStdWString(name).toStdString());
  level->setFrame(TFrameId(1), TImageP());
  return level;
}

//-----------------------------------------------------------------------------

TImageP TLevelReaderOra::load(int shrink) {
  QMutexLocker sl(&m_mutex);

  const int w = m_parser->getWidth(), h = m_parser->getHeight();
  QImage canvas(w, h, QImage::Format_ARGB32_Premultiplied);
  canvas.fill(Qt::transparent);
  QPainter painter(&canvas);
  painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

  if (m_layerIndex >= 0) {
    const TOraLayer &layer = m_parser->getLayer(m_layerIndex);
    QImage png             = decodePng(*m_parser, layer.src);
    if (png.isNull())
      throw TImageException(m_path, "Can't decode OpenRaster layer '" +
                                        layer.src + "'");
    // Bake the layer's opacity into the pixels: column opacity only shows
    // in the viewer unless a render preference is enabled, and the render
    // must match what the painter saw.
    painter.setOpacity(layer.opacity);
    painter.drawImage(layer.x, layer.y, png);
  } else if (m_parser->hasMergedImage()) {
    QImage merged = decodePng(*m_parser, "mergedimage.png");
    if (!merged.isNull()) painter.drawImage(0, 0, merged);
  } else {
    for (int i = m_parser->getLayerCount() - 1; i >= 0; --i) {
      const TOraLayer &layer = m_parser->getLayer(i);
      if (!layer.visible) continue;
      QImage png = decodePng(*m_parser, layer.src);
      if (png.isNull()) continue;
      painter.setOpacity(layer.opacity);
      painter.drawImage(layer.x, layer.y, png);
    }
  }
  painter.end();

  if (shrink > 1)
    canvas = canvas.scaled(std::max(1, w / shrink), std::max(1, h / shrink),
                           Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

  return toRasterImage(canvas);
}

//=============================================================================
// TImageReaderOra
//-----------------------------------------------------------------------------

class TImageReaderOra final : public TImageReader {
  TLevelReaderOra *m_lr;
  TImageInfo *m_info;

public:
  TImageReaderOra(const TFilePath &path, TLevelReaderOra *lr, TImageInfo *info)
      : TImageReader(path), m_lr(lr), m_info(info) {
    m_lr->addRef();
  }
  ~TImageReaderOra() { m_lr->release(); }

  TImageP load() override { return m_lr->load(m_shrink); }
  const TImageInfo *getImageInfo() const override { return m_info; }

private:
  TImageReaderOra(const TImageReaderOra &);
  TImageReaderOra &operator=(const TImageReaderOra &);
};

//-----------------------------------------------------------------------------

TImageReaderP TLevelReaderOra::getFrameReader(TFrameId fid) {
  return TImageReaderP(new TImageReaderOra(m_path, this, m_info));
}
