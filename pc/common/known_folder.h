// Windows folders (Program Files, ProgramData) from the shell rather than from environment
// variables, which whoever starts the process can change.
#pragma once

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <string>

namespace mobcam {

// The folder's path, or an empty string if Windows can't tell.
inline std::wstring KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR path = nullptr;
  std::wstring out;
  if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path))) out = path;
  CoTaskMemFree(path);
  return out;
}

}  // namespace mobcam
