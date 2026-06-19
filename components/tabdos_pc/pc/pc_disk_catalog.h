#pragma once

#include <stdint.h>
#include <string>
#include <vector>

namespace tabdos {

class PcDiskCatalog {
public:
  struct Entry {
    std::string name;
    std::string path;
    uint64_t sizeBytes;
  };

  static constexpr char const * DefaultDirectory = "/sdcard/dos";

  static bool isDiskImageName(char const * name);
  static std::vector<Entry> listImages(char const * directory = DefaultDirectory);
  static bool chooseDefaultImage(char const * directory, std::string * path);
};

} // namespace tabdos
