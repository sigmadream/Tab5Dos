#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

namespace tabdos {

class PcDiskImage {
public:
  enum class FlushPolicy {
    Immediate,
    Deferred,
  };

  struct Geometry {
    constexpr Geometry(uint16_t cylinderCount = 0, uint8_t headCount = 0, uint8_t sectorCount = 0)
      : cylinders(cylinderCount), heads(headCount), sectors(sectorCount) {}

    uint16_t cylinders;
    uint8_t heads;
    uint8_t sectors;
  };

  PcDiskImage();
  ~PcDiskImage();

  PcDiskImage(PcDiskImage const &) = delete;
  PcDiskImage & operator=(PcDiskImage const &) = delete;

  bool open(char const * path, Geometry geometry = {});
  void close();
  bool isOpen() const { return m_file != nullptr; }

  uint64_t sizeBytes() const { return m_sizeBytes; }
  uint64_t sectorCount() const { return m_sizeBytes / SectorSize; }
  Geometry geometry() const { return m_geometry; }

  bool readSectors(uint64_t lba, uint8_t count, void * dest);
  bool writeSectors(uint64_t lba, uint8_t count, void const * src);
  bool readChs(uint16_t cylinder, uint8_t head, uint8_t sector, uint8_t count, void * dest);
  bool writeChs(uint16_t cylinder, uint8_t head, uint8_t sector, uint8_t count, void const * src);
  bool flush();
  void setFlushPolicy(FlushPolicy policy) { m_flushPolicy = policy; }
  FlushPolicy flushPolicy() const { return m_flushPolicy; }
  bool hasPendingWrites() const { return m_dirty; }

  static constexpr size_t SectorSize = 512;
  static Geometry autoDetectGeometry(uint64_t sizeBytes);
  static bool chsToLba(Geometry geometry, uint16_t cylinder, uint8_t head, uint8_t sector, uint64_t * lba);

private:
  bool seekSector(uint64_t lba, uint8_t count);

  FILE * m_file;
  uint64_t m_sizeBytes;
  Geometry m_geometry;
  FlushPolicy m_flushPolicy;
  bool m_dirty;
};

} // namespace tabdos
