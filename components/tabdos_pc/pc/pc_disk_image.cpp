#include "pc_disk_image.h"

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace tabdos {

PcDiskImage::PcDiskImage()
  : m_file(nullptr),
    m_sizeBytes(0),
    m_geometry(),
    m_flushPolicy(FlushPolicy::Immediate),
    m_dirty(false)
{
}

PcDiskImage::~PcDiskImage()
{
  close();
}

bool PcDiskImage::open(char const * path, Geometry geometry)
{
  close();

  m_file = fopen(path, "r+b");
  if (!m_file)
    return false;

  if (fseek(m_file, 0, SEEK_END) != 0) {
    close();
    return false;
  }

  long size = ftell(m_file);
  if (size < 0 || (size % SectorSize) != 0) {
    close();
    return false;
  }

  m_sizeBytes = static_cast<uint64_t>(size);
  if (geometry.cylinders == 0 || geometry.heads == 0 || geometry.sectors == 0)
    m_geometry = autoDetectGeometry(m_sizeBytes);
  else
    m_geometry = geometry;

  return m_geometry.cylinders > 0 && m_geometry.heads > 0 && m_geometry.sectors > 0;
}

void PcDiskImage::close()
{
  if (m_file) {
    flush();
    fclose(m_file);
  }
  m_file = nullptr;
  m_sizeBytes = 0;
  m_geometry = {};
  m_dirty = false;
}

bool PcDiskImage::readSectors(uint64_t lba, uint8_t count, void * dest)
{
  if (!seekSector(lba, count))
    return false;
  return fread(dest, SectorSize, count, m_file) == count;
}

bool PcDiskImage::writeSectors(uint64_t lba, uint8_t count, void const * src)
{
  if (!seekSector(lba, count))
    return false;
  if (fwrite(src, SectorSize, count, m_file) != count)
    return false;

  m_dirty = true;
  if (m_flushPolicy == FlushPolicy::Immediate)
    return flush();
  return true;
}

bool PcDiskImage::readChs(uint16_t cylinder, uint8_t head, uint8_t sector, uint8_t count, void * dest)
{
  uint64_t lba = 0;
  return chsToLba(m_geometry, cylinder, head, sector, &lba) && readSectors(lba, count, dest);
}

bool PcDiskImage::writeChs(uint16_t cylinder, uint8_t head, uint8_t sector, uint8_t count, void const * src)
{
  uint64_t lba = 0;
  return chsToLba(m_geometry, cylinder, head, sector, &lba) && writeSectors(lba, count, src);
}

bool PcDiskImage::flush()
{
  if (!m_file)
    return true;
  if (!m_dirty)
    return true;
  if (fflush(m_file) != 0)
    return false;

#if defined(_WIN32)
  int fd = _fileno(m_file);
  if (fd >= 0 && _commit(fd) != 0)
    return false;
#else
  int fd = fileno(m_file);
  if (fd >= 0 && fsync(fd) != 0)
    return false;
#endif
  m_dirty = false;
  return true;
}

PcDiskImage::Geometry PcDiskImage::autoDetectGeometry(uint64_t sizeBytes)
{
  struct FloppyFormat {
    uint16_t tracks;
    uint8_t sectors;
    uint8_t heads;
  };

  static constexpr FloppyFormat FloppyFormats[] = {
    { 40,  8, 1 },
    { 40,  9, 1 },
    { 40,  8, 2 },
    { 40,  9, 2 },
    { 80,  9, 2 },
    { 80, 15, 2 },
    { 80, 18, 2 },
    { 80, 36, 2 },
  };

  for (auto const & format : FloppyFormats) {
    if (SectorSize * static_cast<uint64_t>(format.tracks) * format.sectors * format.heads == sizeBytes)
      return { format.tracks, format.heads, format.sectors };
  }

  constexpr uint16_t MaxCylinders = 1024;
  constexpr uint8_t MaxHeads = 16;
  constexpr uint8_t MaxSectors = 63;

  uint64_t totalSectors = sizeBytes / SectorSize;
  if (totalSectors == 0)
    return {};

  Geometry geometry;
  geometry.sectors = totalSectors < MaxSectors ? static_cast<uint8_t>(totalSectors) : MaxSectors;
  uint64_t headsNeeded = (totalSectors + geometry.sectors - 1) / geometry.sectors;
  geometry.heads = headsNeeded < MaxHeads ? static_cast<uint8_t>(headsNeeded) : MaxHeads;
  uint64_t cylindersNeeded = (totalSectors + static_cast<uint64_t>(geometry.heads) * geometry.sectors - 1) /
                             (static_cast<uint64_t>(geometry.heads) * geometry.sectors);
  geometry.cylinders = cylindersNeeded < MaxCylinders ? static_cast<uint16_t>(cylindersNeeded) : MaxCylinders;
  return geometry;
}

bool PcDiskImage::chsToLba(Geometry geometry, uint16_t cylinder, uint8_t head, uint8_t sector, uint64_t * lba)
{
  if (!lba || geometry.cylinders == 0 || geometry.heads == 0 || geometry.sectors == 0)
    return false;
  if (cylinder >= geometry.cylinders || head >= geometry.heads || sector == 0 || sector > geometry.sectors)
    return false;

  *lba = (static_cast<uint64_t>(cylinder) * geometry.heads + head) * geometry.sectors + (sector - 1);
  return true;
}

bool PcDiskImage::seekSector(uint64_t lba, uint8_t count)
{
  if (!m_file || count == 0)
    return false;
  uint64_t offset = lba * SectorSize;
  uint64_t bytes = static_cast<uint64_t>(count) * SectorSize;
  if (offset + bytes > m_sizeBytes)
    return false;
  return fseek(m_file, static_cast<long>(offset), SEEK_SET) == 0;
}

} // namespace tabdos
