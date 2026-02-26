#include "device_enumerator.h"

#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <codecvt>
#include <locale>
#include <string>

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string WideToUtf8(const std::wstring& w) {
  if (w.empty()) return {};
  int size = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                 nullptr, 0, nullptr, nullptr);
  std::string s(size, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                      s.data(), size, nullptr, nullptr);
  return s;
}

static std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return {};
  int size = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                                 nullptr, 0);
  std::wstring w(size, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                      w.data(), size);
  return w;
}

// ---------------------------------------------------------------------------
// DeviceEnumerator
// ---------------------------------------------------------------------------

std::vector<DeviceInfo> DeviceEnumerator::EnumerateVideoDevices() {
  std::vector<DeviceInfo> result;

  ComPtr<IMFAttributes> attrs;
  if (FAILED(MFCreateAttributes(&attrs, 1))) return result;
  if (FAILED(attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))) {
    return result;
  }

  IMFActivate** devices = nullptr;
  UINT32 count = 0;
  if (FAILED(MFEnumDeviceSources(attrs.Get(), &devices, &count))) return result;

  for (UINT32 i = 0; i < count; ++i) {
    WCHAR* friendly_name = nullptr;
    UINT32 fn_len = 0;
    WCHAR* symbolic_link = nullptr;
    UINT32 sl_len = 0;

    devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
                                   &friendly_name, &fn_len);
    devices[i]->GetAllocatedString(
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
        &symbolic_link, &sl_len);

    if (friendly_name && symbolic_link) {
      result.push_back({friendly_name, symbolic_link});
    }

    if (friendly_name) CoTaskMemFree(friendly_name);
    if (symbolic_link) CoTaskMemFree(symbolic_link);
    devices[i]->Release();
  }

  CoTaskMemFree(devices);
  return result;
}

std::wstring DeviceEnumerator::FindSymbolicLink(const std::string& name) {
  // Name format: "Friendly Name (symbolic_link)"
  // Extract the part inside the last pair of parentheses.
  auto last_open = name.rfind('(');
  auto last_close = name.rfind(')');
  if (last_open == std::string::npos || last_close == std::string::npos ||
      last_close < last_open) {
    return {};
  }
  std::string sym_utf8 = name.substr(last_open + 1, last_close - last_open - 1);
  return Utf8ToWide(sym_utf8);
}
