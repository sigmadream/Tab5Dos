#include "pc_disk_catalog.h"

#include <algorithm>
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

namespace tabdos {

namespace {

static bool equalsIgnoreCase(char const *a, char const *b) {
  if (!a || !b)
    return false;
  while (*a && *b) {
    if (tolower(static_cast<unsigned char>(*a)) !=
        tolower(static_cast<unsigned char>(*b)))
      return false;
    ++a;
    ++b;
  }
  return *a == '\0' && *b == '\0';
}

static bool hasExtension(char const *name, char const *extension) {
  if (!name || !extension)
    return false;
  char const *dot = strrchr(name, '.');
  return dot && equalsIgnoreCase(dot, extension);
}

static std::string joinPath(char const *directory, char const *name) {
  std::string path = directory ? directory : "";
  if (!path.empty() && path.back() != '/')
    path += '/';
  path += name ? name : "";
  return path;
}

static int preferredRank(std::string const &name) {
  struct Preference {
    char const *name;
    int rank;
  };
  static constexpr Preference preferences[] = {
      {"fd0.img", 0},
      {"a.img", 1},
      {"hd20_DOSPROG.img", 2},
      // {"a_freedos.img", 2},
      {"floppy_freedos.img", 3},
      {"freedos.img", 4},
      {"hd0.img", 10},
  };

  for (auto const &preference : preferences) {
    if (equalsIgnoreCase(name.c_str(), preference.name))
      return preference.rank;
  }
  return 100;
}

} // namespace

bool PcDiskCatalog::isDiskImageName(char const *name) {
  return hasExtension(name, ".img") || hasExtension(name, ".ima");
}

std::vector<PcDiskCatalog::Entry>
PcDiskCatalog::listImages(char const *directory) {
  std::vector<Entry> entries;
  DIR *dir = opendir(directory ? directory : DefaultDirectory);
  if (!dir)
    return entries;

  while (dirent *item = readdir(dir)) {
    if (!isDiskImageName(item->d_name))
      continue;

    std::string path =
        joinPath(directory ? directory : DefaultDirectory, item->d_name);
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
      continue;

    Entry entry;
    entry.name = item->d_name;
    entry.path = path;
    entry.sizeBytes = static_cast<uint64_t>(st.st_size);
    entries.push_back(entry);
  }
  closedir(dir);

  std::sort(entries.begin(), entries.end(),
            [](Entry const &lhs, Entry const &rhs) {
              int lhsRank = preferredRank(lhs.name);
              int rhsRank = preferredRank(rhs.name);
              if (lhsRank != rhsRank)
                return lhsRank < rhsRank;
              return lhs.name < rhs.name;
            });
  return entries;
}

bool PcDiskCatalog::chooseDefaultImage(char const *directory,
                                       std::string *path) {
  std::vector<Entry> entries =
      listImages(directory ? directory : DefaultDirectory);
  if (entries.empty())
    return false;
  if (path)
    *path = entries.front().path;
  return true;
}

} // namespace tabdos
