#include "shell/browser/osr/osr_external_shm.h"

#include <io.h>  // CreateFileMappingA, OpenFileMappingA, etc.

namespace barmco {

SHM::SHM(std::string path, size_t size) : path_(path), size_(size) {}

SHMError SHM::CreateOrOpen(bool create) {
  if (create) {
    DWORD size_high_order = 0;
    DWORD size_low_order = static_cast<DWORD>(size_);
    handle_ = CreateFileMappingA(INVALID_HANDLE_VALUE,  // use paging file
                                 NULL,                  // default security
                                 PAGE_READWRITE,        // read/write access
                                 size_high_order, size_low_order,
                                 path_.c_str()  // name of mapping object
    );

    if (!handle_) {
      return kErrorCreationFailed;
    }
  } else {
    handle_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS,  // fully accessible
                               FALSE,                // do not inherit the name
                               path_.c_str()         // name of mapping object
    );

    if (!handle_) {
      return kErrorOpeningFailed;
    }
  }

  DWORD access = FILE_MAP_ALL_ACCESS;

  data_ = static_cast<uint8_t*>(MapViewOfFile(handle_, access, 0, 0, size_));

  if (!data_) {
    return kErrorMappingFailed;
  }

  return kOK;
}

/**
 * Destructor
 */
SHM::~SHM() {
  if (data_) {
    UnmapViewOfFile(data_);
    data_ = nullptr;
  }

  CloseHandle(handle_);
}

}  // namespace barmco
