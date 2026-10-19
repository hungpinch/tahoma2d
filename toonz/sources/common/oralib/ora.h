#pragma once

#ifndef ORA_INCLUDED
#define ORA_INCLUDED

// OpenRaster (.ora) container access. An .ora file is a zip archive holding
// "stack.xml" (the layer tree with per-layer offsets, opacity and visibility)
// plus one PNG per layer and, usually, "mergedimage.png".
// Spec: https://www.openraster.org/baseline/file-layout-spec.html

#include "tcommon.h"
#include "tfilepath.h"

#include <map>
#include <string>
#include <vector>

#undef DVAPI
#undef DVVAR

#ifdef TNZCORE_EXPORTS
#define DVAPI DV_EXPORT_API
#define DVVAR DV_EXPORT_VAR
#else
#define DVAPI DV_IMPORT_API
#define DVVAR DV_IMPORT_VAR
#endif

//! Minimal read-only zip archive: stored and deflate entries, no zip64,
//! no encryption. Enough for every .ora writer in the wild (Krita, MyPaint,
//! GIMP, Pinta, the suite's own tests).
class DVAPI TOraZip {
  struct Entry {
    int method;
    unsigned int csize, usize, offset;
  };
  std::vector<unsigned char> m_data;
  std::map<std::string, Entry> m_entries;

public:
  bool open(const TFilePath &path);
  bool has(const std::string &name) const { return m_entries.count(name) > 0; }
  bool read(const std::string &name, std::vector<unsigned char> &out) const;
};

//! One leaf <layer> of stack.xml with its enclosing <stack>s folded in.
struct TOraLayer {
  std::wstring name;        //!< layer name (never empty)
  std::wstring groupPath;   //!< "group/subgroup" of enclosing stacks, or empty
  std::string src;          //!< zip entry holding the layer's PNG
  int x = 0, y = 0;         //!< top-left of the PNG in canvas pixels
  double opacity = 1.0;     //!< effective opacity, parents multiplied in
  bool visible = true;      //!< effective visibility, hidden parent hides
  std::string compositeOp;  //!< e.g. "svg:src-over"
};

//! Parses an .ora file's stack.xml.
//! Layers are listed in document order: index 0 is the topmost layer and
//! getLayer(getLayerCount() - 1) is the bottom one.
//! A path of the form "name#<index>.ora" addresses one layer (same convention
//! as "name#<layerId>.psd"); basePath() strips that suffix.
class DVAPI TOraParser {
  TFilePath m_path;
  TOraZip m_zip;
  int m_width, m_height;
  double m_xres, m_yres;
  bool m_hasMergedImage;
  std::vector<TOraLayer> m_layers;

public:
  //! Opens the archive and parses stack.xml. Throws TImageException.
  explicit TOraParser(const TFilePath &path);

  static TFilePath basePath(const TFilePath &path);
  //! The layer index encoded in the path, or -1 for the flat image.
  static int layerIndex(const TFilePath &path);
  static TFilePath layerPath(const TFilePath &base, int index);

  const TFilePath &getPath() const { return m_path; }
  int getWidth() const { return m_width; }
  int getHeight() const { return m_height; }
  double getXRes() const { return m_xres; }
  double getYRes() const { return m_yres; }
  bool hasMergedImage() const { return m_hasMergedImage; }

  int getLayerCount() const { return (int)m_layers.size(); }
  const TOraLayer &getLayer(int index) const { return m_layers[index]; }

  //! Raw bytes of a zip entry, e.g. a layer's PNG.
  bool readEntry(const std::string &name,
                 std::vector<unsigned char> &out) const {
    return m_zip.read(name, out);
  }
};

#endif  // ORA_INCLUDED
