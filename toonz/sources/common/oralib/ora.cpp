#include "ora.h"
#include "timage_io.h"  // TImageException

#include "zlib.h"

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QXmlStreamReader>

#include <cstring>

//=============================================================================
// TOraZip
//-----------------------------------------------------------------------------

namespace {

unsigned int rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
unsigned int rd32(const unsigned char *p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned int)p[3] << 24);
}

const unsigned int kLocalHeaderSig = 0x04034b50;
const unsigned int kCentralDirSig  = 0x02014b50;
const unsigned int kEndOfCentralDirSig = 0x06054b50;

}  // namespace

bool TOraZip::open(const TFilePath &path) {
  m_entries.clear();
  m_data.clear();

  QFile file(path.getQString());
  if (!file.open(QIODevice::ReadOnly)) return false;
  QByteArray bytes = file.readAll();
  m_data.assign(bytes.constData(), bytes.constData() + bytes.size());

  const size_t n = m_data.size();
  if (n < 22) return false;

  // End of central directory record: 22 bytes, possibly followed by a comment
  // of up to 64 KiB, so scan backwards for its signature.
  size_t eocd         = n;
  const size_t minPos = n > 22 + 65535 ? n - 22 - 65535 : 0;
  for (size_t i = n - 22;; --i) {
    if (rd32(&m_data[i]) == kEndOfCentralDirSig) {
      eocd = i;
      break;
    }
    if (i == minPos) break;
  }
  if (eocd == n) return false;

  unsigned int count    = rd16(&m_data[eocd + 10]);
  unsigned int cdSize   = rd32(&m_data[eocd + 12]);
  unsigned int cdOffset = rd32(&m_data[eocd + 16]);
  if ((size_t)cdOffset + cdSize > n) return false;

  size_t p = cdOffset;
  for (unsigned int k = 0; k < count; ++k) {
    if (p + 46 > n || rd32(&m_data[p]) != kCentralDirSig) return false;
    Entry e;
    e.method                = rd16(&m_data[p + 10]);
    e.csize                 = rd32(&m_data[p + 20]);
    e.usize                 = rd32(&m_data[p + 24]);
    unsigned int nameLen    = rd16(&m_data[p + 28]);
    unsigned int extraLen   = rd16(&m_data[p + 30]);
    unsigned int commentLen = rd16(&m_data[p + 32]);
    e.offset                = rd32(&m_data[p + 42]);
    if (p + 46 + nameLen > n) return false;
    std::string name((const char *)&m_data[p + 46], nameLen);
    m_entries[name] = e;
    p += 46 + nameLen + extraLen + commentLen;
  }
  return true;
}

//-----------------------------------------------------------------------------

bool TOraZip::read(const std::string &name,
                   std::vector<unsigned char> &out) const {
  out.clear();
  std::map<std::string, Entry>::const_iterator it = m_entries.find(name);
  if (it == m_entries.end()) return false;
  const Entry &e = it->second;

  const size_t n = m_data.size();
  size_t p       = e.offset;
  if (p + 30 > n || rd32(&m_data[p]) != kLocalHeaderSig) return false;
  unsigned int nameLen  = rd16(&m_data[p + 26]);
  unsigned int extraLen = rd16(&m_data[p + 28]);
  size_t dataStart      = p + 30 + nameLen + extraLen;
  if (dataStart + e.csize > n) return false;
  const unsigned char *src = &m_data[dataStart];

  if (e.method == 0) {  // stored
    out.assign(src, src + e.csize);
    return true;
  }
  if (e.method != 8) return false;  // only deflate is supported
  if (e.usize == 0) return true;

  out.resize(e.usize);
  z_stream strm;
  memset(&strm, 0, sizeof strm);
  if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) return false;  // raw deflate
  strm.next_in   = const_cast<Bytef *>(src);
  strm.avail_in  = e.csize;
  strm.next_out  = out.data();
  strm.avail_out = e.usize;
  int ret        = inflate(&strm, Z_FINISH);
  inflateEnd(&strm);
  if (ret != Z_STREAM_END) {
    out.clear();
    return false;
  }
  return true;
}

//=============================================================================
// TOraParser
//-----------------------------------------------------------------------------

TFilePath TOraParser::basePath(const TFilePath &path) {
  std::wstring name = path.getWideName();
  size_t i          = name.find(L'#');
  if (i == std::wstring::npos) return path;
  return path.withName(name.substr(0, i));
}

//-----------------------------------------------------------------------------

int TOraParser::layerIndex(const TFilePath &path) {
  std::wstring name = path.getWideName();
  size_t i          = name.find(L'#');
  if (i == std::wstring::npos) return -1;
  bool ok   = false;
  int index = QString::fromStdWString(name.substr(i + 1)).toInt(&ok);
  return ok ? index : -1;
}

//-----------------------------------------------------------------------------

TFilePath TOraParser::layerPath(const TFilePath &base, int index) {
  return base.withName(base.getWideName() + L"#" + std::to_wstring(index));
}

//-----------------------------------------------------------------------------

TOraParser::TOraParser(const TFilePath &path)
    : m_path(basePath(path))
    , m_width(0)
    , m_height(0)
    , m_xres(0)
    , m_yres(0)
    , m_hasMergedImage(false) {
  if (!m_zip.open(m_path))
    throw TImageException(m_path, "Can't open OpenRaster archive");

  std::vector<unsigned char> xml;
  if (!m_zip.read("stack.xml", xml))
    throw TImageException(m_path, "stack.xml missing from OpenRaster archive");
  m_hasMergedImage = m_zip.has("mergedimage.png");

  // Effective state inherited from the enclosing <stack> elements
  struct Frame {
    int x = 0, y = 0;
    double opacity = 1.0;
    bool visible   = true;
    std::wstring name;
  };
  std::vector<Frame> stack;
  const Frame root;

  QXmlStreamReader reader(QByteArray((const char *)xml.data(), (int)xml.size()));
  while (!reader.atEnd()) {
    QXmlStreamReader::TokenType token = reader.readNext();
    if (token == QXmlStreamReader::EndElement) {
      if (reader.name() == QLatin1String("stack") && !stack.empty())
        stack.pop_back();
      continue;
    }
    if (token != QXmlStreamReader::StartElement) continue;

    const QXmlStreamAttributes a = reader.attributes();
    const Frame &parent          = stack.empty() ? root : stack.back();

    if (reader.name() == QLatin1String("image")) {
      m_width  = a.value("w").toInt();
      m_height = a.value("h").toInt();
      m_xres   = a.value("xres").toDouble();
      m_yres   = a.value("yres").toDouble();
    } else if (reader.name() == QLatin1String("stack")) {
      Frame f;
      f.x       = parent.x + a.value("x").toInt();
      f.y       = parent.y + a.value("y").toInt();
      f.opacity = parent.opacity *
                  (a.hasAttribute("opacity") ? a.value("opacity").toDouble()
                                             : 1.0);
      f.visible = parent.visible &&
                  a.value("visibility") != QLatin1String("hidden");
      std::wstring groupName = a.value("name").toString().toStdWString();
      f.name                 = parent.name;
      if (!groupName.empty())
        f.name = f.name.empty() ? groupName : f.name + L"/" + groupName;
      stack.push_back(f);
    } else if (reader.name() == QLatin1String("layer")) {
      TOraLayer layer;
      layer.src = a.value("src").toString().toStdString();
      if (layer.src.empty()) continue;
      layer.name = a.value("name").toString().toStdWString();
      if (layer.name.empty())
        layer.name = L"layer_" + std::to_wstring(m_layers.size());
      layer.groupPath = parent.name;
      layer.x         = parent.x + a.value("x").toInt();
      layer.y         = parent.y + a.value("y").toInt();
      layer.opacity   = parent.opacity * (a.hasAttribute("opacity")
                                              ? a.value("opacity").toDouble()
                                              : 1.0);
      layer.visible = parent.visible &&
                      a.value("visibility") != QLatin1String("hidden");
      layer.compositeOp = a.value("composite-op").toString().toStdString();
      m_layers.push_back(layer);
    }
  }

  if (reader.hasError())
    throw TImageException(m_path, "Malformed stack.xml: " +
                                      reader.errorString().toStdString());
  if (m_width <= 0 || m_height <= 0)
    throw TImageException(m_path, "stack.xml has no canvas size");
}
