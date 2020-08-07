#ifndef SHELL_BROWSER_OSR_OSR_EXTERNAL_SHM_H_
#define SHELL_BROWSER_OSR_OSR_EXTERNAL_SHM_H_

#include <cstdint>
#include <string>

#if defined(WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef WIN32_LEAN_AND_MEAN
#endif  // WIN32

namespace barmco {

enum SHMError {
  kOK = 0,
  kErrorCreationFailed = 100,
  kErrorMappingFailed = 110,
  kErrorOpeningFailed = 120,
};

class SHM {
 public:
  // path should only contain alpha-numeric characters, and is normalized
  // on linux/macOS.
  explicit SHM(std::string path, size_t size);

  // create a shared memory area and open it for writing
  inline SHMError Create() { return CreateOrOpen(true); }

  // open an existing shared memory for reading
  inline SHMError Open() { return CreateOrOpen(false); }

  inline size_t Size() { return size_; }
  inline const std::string& Path() { return path_; }
  inline uint8_t* Data() { return data_; }

  ~SHM();

 private:
  SHMError CreateOrOpen(bool create);

  std::string path_;
  uint8_t* data_ = nullptr;
  size_t size_ = 0;

#if defined(WIN32)
  HANDLE handle_;
#else
  int fd_ = -1;
#endif
};

}  // namespace barmco

#endif  // SHELL_BROWSER_OSR_OSR_EXTERNAL_SHM_H_
