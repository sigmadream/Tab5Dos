#include "pc/pc_disk_image.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <vector>

using tabdos::PcDiskImage;

static void createImage(char const * path, size_t bytes)
{
  FILE * file = fopen(path, "wb");
  assert(file);
  std::vector<uint8_t> zero(PcDiskImage::SectorSize, 0);
  for (size_t written = 0; written < bytes; written += zero.size())
    assert(fwrite(zero.data(), 1, zero.size(), file) == zero.size());
  assert(fclose(file) == 0);
}

int main()
{
  char path[] = "/tmp/tabdos-disk-XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);
  close(fd);

  createImage(path, 1440 * 1024);

  {
    PcDiskImage image;
    assert(image.open(path));
    auto geometry = image.geometry();
    assert(geometry.cylinders == 80);
    assert(geometry.heads == 2);
    assert(geometry.sectors == 18);
    assert(image.sectorCount() == 2880);

    uint8_t sector[PcDiskImage::SectorSize] = {};
    memcpy(sector, "TABDOS", 6);
    assert(image.writeChs(0, 0, 1, 1, sector));

    memset(sector, 0, sizeof(sector));
    assert(image.readSectors(0, 1, sector));
    assert(memcmp(sector, "TABDOS", 6) == 0);

    memset(sector, 0xa5, sizeof(sector));
    assert(image.writeSectors(2879, 1, sector));
    assert(!image.writeSectors(2880, 1, sector));
    assert(!image.hasPendingWrites());
    image.setFlushPolicy(PcDiskImage::FlushPolicy::Deferred);
    assert(image.flushPolicy() == PcDiskImage::FlushPolicy::Deferred);
    memset(sector, 0x5a, sizeof(sector));
    assert(image.writeSectors(2, 1, sector));
    assert(image.hasPendingWrites());
    assert(image.flush());
    assert(!image.hasPendingWrites());
  }

  {
    PcDiskImage reopened;
    assert(reopened.open(path));
    uint8_t sector[PcDiskImage::SectorSize] = {};
    assert(reopened.readChs(0, 0, 1, 1, sector));
    assert(memcmp(sector, "TABDOS", 6) == 0);
    assert(reopened.readSectors(2879, 1, sector));
    for (auto byte : sector)
      assert(byte == 0xa5);
  }

  unlink(path);

  auto small = PcDiskImage::autoDetectGeometry(PcDiskImage::SectorSize * 32);
  assert(small.cylinders == 1);
  assert(small.heads == 1);
  assert(small.sectors == 32);

  uint64_t lba = 0;
  assert(PcDiskImage::chsToLba({80, 2, 18}, 1, 1, 1, &lba));
  assert(lba == 54);
  assert(!PcDiskImage::chsToLba({80, 2, 18}, 0, 2, 1, &lba));

  printf("pc_disk_image_test passed\n");
  return 0;
}
