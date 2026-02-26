#pragma once

#include <windows.h>

#include <cstdio>
#include <sstream>
#include <string>

inline void DebugLog(const std::string& msg) {
  std::string line = "[camera_desktop/windows] " + msg + "\n";
  OutputDebugStringA(line.c_str());
  std::fputs(line.c_str(), stderr);
  std::fflush(stderr);
}

inline std::string HrToString(HRESULT hr) {
  std::ostringstream ss;
  ss << "0x" << std::hex << static_cast<unsigned long>(hr);
  return ss.str();
}
