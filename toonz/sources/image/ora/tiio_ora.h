#pragma once

#ifndef TIIO_ORA_INCLUDED
#define TIIO_ORA_INCLUDED

#include "../../common/oralib/ora.h"
#include "tlevel_io.h"
#include "tthread.h"

// OpenRaster level reader. If path =
//   name.ora       the flat image: mergedimage.png when present, otherwise the
//                  visible layers composited bottom-to-top.
//   name#N.ora     layer N (document order, 0 = topmost) placed on the full
//                  canvas at its stack.xml offset with its opacity baked in,
//                  so every layer level has the canvas size and the parts
//                  line up in the xsheet.
// Either way the level has a single frame.

class TLevelReaderOra final : public TLevelReader {
public:
  TLevelReaderOra(const TFilePath &path);
  ~TLevelReaderOra();

  TImageReaderP getFrameReader(TFrameId fid) override;
  TLevelP loadInfo() override;

  const TImageInfo *getImageInfo(TFrameId fid) override { return m_info; }
  const TImageInfo *getImageInfo() override { return m_info; }

  TImageP load(int shrink);

  static TLevelReader *create(const TFilePath &f) {
    return new TLevelReaderOra(f);
  }

private:
  TOraParser *m_parser;
  int m_layerIndex;  // -1: flat image
  TThread::Mutex m_mutex;
};

#endif  // TIIO_ORA_INCLUDED
