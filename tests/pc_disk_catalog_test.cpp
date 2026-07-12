#include "pc/pc_disk_catalog.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <unistd.h>

using tabdos::PcDiskCatalog;

static void writeFile(std::string const & path, char const * text)
{
  FILE * f = fopen(path.c_str(), "wb");
  assert(f);
  assert(fwrite(text, 1, strlen(text), f) == strlen(text));
  assert(fclose(f) == 0);
}

int main()
{
  assert(PcDiskCatalog::isDiskImageName("fd0.img"));
  assert(PcDiskCatalog::isDiskImageName("BOOT.IMA"));
  assert(!PcDiskCatalog::isDiskImageName("readme.txt"));

  char dirTemplate[] = "/tmp/tabdos-dos-dir-XXXXXX";
  char * dir = mkdtemp(dirTemplate);
  assert(dir);
  writeFile(std::string(dir) + "/readme.txt", "ignore");
  writeFile(std::string(dir) + "/hd0.img", "hd");
  writeFile(std::string(dir) + "/A_freedos.img", "fd");
  writeFile(std::string(dir) + "/hd20_DOSPROG.img", "dosprog");

  std::vector<PcDiskCatalog::Entry> entries = PcDiskCatalog::listImages(dir);
  assert(entries.size() == 3);
  assert(entries[0].name == "hd20_DOSPROG.img");
  assert(entries[0].sizeBytes == 7);
  assert(entries[1].name == "hd0.img");
  assert(entries[2].name == "A_freedos.img");

  std::string chosen;
  assert(PcDiskCatalog::chooseDefaultImage(dir, &chosen));
  assert(chosen == std::string(dir) + "/hd20_DOSPROG.img");

  unlink((std::string(dir) + "/readme.txt").c_str());
  unlink((std::string(dir) + "/hd0.img").c_str());
  unlink((std::string(dir) + "/A_freedos.img").c_str());
  unlink((std::string(dir) + "/hd20_DOSPROG.img").c_str());
  rmdir(dir);

  printf("pc_disk_catalog_test passed\n");
  return 0;
}
