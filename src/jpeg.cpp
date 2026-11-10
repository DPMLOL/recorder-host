#include "jpeg.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace {

std::wstring widen(const std::string &utf8) {
  const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
  std::wstring wide(len, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), wide.data(), len);
  return wide;
}

std::string hresult(const char *what, HRESULT hr) {
  char text[96];
  snprintf(text, sizeof(text), "%s failed (0x%08lx)", what, (unsigned long)hr);
  return text;
}

IWICImagingFactory *factory() {
  // One per thread: the encoder runs on the preview worker only.
  thread_local ComPtr<IWICImagingFactory> instance;
  if (!instance) {
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&instance));
  }
  return instance.Get();
}

bool encode(const std::wstring &path, const uint8_t *bgr, uint32_t width, uint32_t height, uint32_t stride,
            float quality, std::string &error) {
  IWICImagingFactory *wic = factory();
  if (!wic) {
    error = "WIC is not available";
    return false;
  }
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> options;
  HRESULT hr = wic->CreateStream(&stream);
  if (FAILED(hr)) {
    error = hresult("CreateStream", hr);
    return false;
  }
  if (FAILED(hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) {
    error = hresult("opening the file", hr);
    return false;
  }
  if (FAILED(hr = wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) ||
      FAILED(hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
      FAILED(hr = encoder->CreateNewFrame(&frame, &options))) {
    error = hresult("creating the JPEG encoder", hr);
    return false;
  }
  PROPBAG2 option{};
  option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
  VARIANT value;
  VariantInit(&value);
  value.vt = VT_R4;
  value.fltVal = quality;
  options->Write(1, &option, &value);

  WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
  if (FAILED(hr = frame->Initialize(options.Get())) || FAILED(hr = frame->SetSize(width, height)) ||
      FAILED(hr = frame->SetPixelFormat(&format))) {
    error = hresult("configuring the JPEG frame", hr);
    return false;
  }
  if (format != GUID_WICPixelFormat24bppBGR) {
    error = "the JPEG encoder refused 24-bit BGR";
    return false;
  }
  if (FAILED(hr = frame->WritePixels(height, stride, stride * height, const_cast<BYTE *>(bgr))) ||
      FAILED(hr = frame->Commit()) || FAILED(hr = encoder->Commit())) {
    error = hresult("writing the JPEG", hr);
    return false;
  }
  return true;
}

}  // namespace

bool write_jpeg(const std::string &path, const uint8_t *bgr, uint32_t width, uint32_t height, uint32_t stride,
                float quality, std::string &error) {
  const std::wstring target = widen(path);
  const std::wstring temp = target + L".tmp";
  // The stream is released (the file closed) before the rename.
  if (!encode(temp, bgr, width, height, stride, quality, error)) {
    DeleteFileW(temp.c_str());
    error = path + ": " + error;
    return false;
  }
  // A reader that opened the file without FILE_SHARE_DELETE blocks the replace for as long as it reads.
  for (int attempt = 0;; attempt++) {
    if (MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
      return true;
    }
    const DWORD code = GetLastError();
    if (attempt == 9 || (code != ERROR_ACCESS_DENIED && code != ERROR_SHARING_VIOLATION)) {
      DeleteFileW(temp.c_str());
      error = path + ": " + hresult("renaming the temp file", HRESULT_FROM_WIN32(code));
      return false;
    }
    Sleep(10);
  }
}
