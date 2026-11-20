#include "image.h"

#ifdef _WIN32

#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cstring>
#include <sstream>

namespace fc {

namespace {

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    T** address() {
        reset();
        return &ptr_;
    }
    T* get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

    void reset() {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

private:
    T* ptr_ = nullptr;
};

class ComScope {
public:
    ComScope() {
        hr_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        initialized_ = SUCCEEDED(hr_);
    }
    ~ComScope() {
        if (initialized_) {
            CoUninitialize();
        }
    }
    bool ok() const { return initialized_ || hr_ == RPC_E_CHANGED_MODE; }

private:
    HRESULT hr_ = E_FAIL;
    bool initialized_ = false;
};

std::string hresult_text(HRESULT hr) {
    std::ostringstream out;
    out << "HRESULT 0x" << std::hex << static_cast<unsigned long>(hr);
    return out.str();
}

bool create_factory(ComPtr<IWICImagingFactory>& factory, std::string& error) {
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.address()));
    if (FAILED(hr)) {
        error = "无法创建 WIC 工厂: " + hresult_text(hr);
        return false;
    }
    return true;
}

bool decode_with_wic(IWICImagingFactory* factory, const fs::path& path, Image& image,
                     std::string& error) {
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = factory->CreateDecoderFromFilename(path.wstring().c_str(), nullptr, GENERIC_READ,
                                                    WICDecodeMetadataCacheOnDemand,
                                                    decoder.address());
    if (FAILED(hr)) {
        error = "系统解码器无法打开文件: " + hresult_text(hr);
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, frame.address());
    if (FAILED(hr)) {
        error = "无法读取图片帧: " + hresult_text(hr);
        return false;
    }
    UINT width = 0;
    UINT height = 0;
    hr = frame->GetSize(&width, &height);
    if (FAILED(hr) || width == 0 || height == 0) {
        error = "图片尺寸无效";
        return false;
    }
    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(converter.address());
    if (FAILED(hr)) {
        error = "无法创建 WIC 格式转换器: " + hresult_text(hr);
        return false;
    }
    hr = converter->Initialize(frame.get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                               nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        error = "WIC 像素格式转换失败: " + hresult_text(hr);
        return false;
    }
    image.width = static_cast<int>(width);
    image.height = static_cast<int>(height);
    image.rgba.resize(static_cast<size_t>(width) * height * 4);
    hr = converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(image.rgba.size()),
                               image.rgba.data());
    if (FAILED(hr)) {
        error = "WIC 读取像素失败: " + hresult_text(hr);
        return false;
    }
    return true;
}

std::vector<uint8_t> to_bgr24(const Image& image) {
    std::vector<uint8_t> rgb(static_cast<size_t>(image.width) * image.height * 3);
    for (size_t i = 0, j = 0; i < image.rgba.size(); i += 4, j += 3) {
        rgb[j] = image.rgba[i + 2];
        rgb[j + 1] = image.rgba[i + 1];
        rgb[j + 2] = image.rgba[i];
    }
    return rgb;
}

bool encode_with_wic(const Image& image, const fs::path& path, const std::string& format,
                     std::string& error) {
    ComPtr<IWICImagingFactory> factory;
    if (!create_factory(factory, error)) {
        return false;
    }

    GUID container = GUID_ContainerFormatTiff;
    bool alpha = false;
    if (format == "heif") {
        container = GUID_ContainerFormatHeif;
        alpha = true;
    } else if (format == "avif") {
        error = "Windows WIC 没有通用 AVIF 编码器，请改用 PNG/WebP";
        return false;
    } else if (format != "tiff") {
        error = "不支持的 WIC 输出格式";
        return false;
    }

    ComPtr<IWICStream> stream;
    HRESULT hr = factory->CreateStream(stream.address());
    if (FAILED(hr)) {
        error = "无法创建输出流: " + hresult_text(hr);
        return false;
    }
    hr = stream->InitializeFromFilename(path.wstring().c_str(), GENERIC_WRITE);
    if (FAILED(hr)) {
        error = "无法创建输出文件: " + hresult_text(hr);
        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(container, nullptr, encoder.address());
    if (FAILED(hr)) {
        error = "系统没有 " + format + " 编码器: " + hresult_text(hr);
        return false;
    }
    hr = encoder->Initialize(stream.get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        error = "初始化编码器失败: " + hresult_text(hr);
        return false;
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    hr = encoder->CreateNewFrame(frame.address(), properties.address());
    if (FAILED(hr)) {
        error = "创建编码帧失败: " + hresult_text(hr);
        return false;
    }
    hr = frame->Initialize(properties.get());
    if (FAILED(hr)) {
        error = "初始化编码帧失败: " + hresult_text(hr);
        return false;
    }
    frame->SetSize(static_cast<UINT>(image.width), static_cast<UINT>(image.height));
    frame->SetResolution(96.0, 96.0);

    ComPtr<IWICBitmap> bitmap;
    if (alpha) {
        WICPixelFormatGUID pixel_format = GUID_WICPixelFormat32bppRGBA;
        hr = frame->SetPixelFormat(&pixel_format);
        if (FAILED(hr)) {
            error = "编码器不接受 RGBA 像素格式: " + hresult_text(hr);
            return false;
        }
        hr = factory->CreateBitmapFromMemory(static_cast<UINT>(image.width),
                                             static_cast<UINT>(image.height),
                                             GUID_WICPixelFormat32bppRGBA,
                                             static_cast<UINT>(image.stride()),
                                             static_cast<UINT>(image.rgba.size()),
                                             const_cast<BYTE*>(image.rgba.data()), bitmap.address());
    } else {
        std::vector<uint8_t> bgr = to_bgr24(image);
        WICPixelFormatGUID pixel_format = GUID_WICPixelFormat24bppBGR;
        hr = frame->SetPixelFormat(&pixel_format);
        if (FAILED(hr)) {
            error = "编码器不接受 BGR 像素格式: " + hresult_text(hr);
            return false;
        }
        UINT stride = static_cast<UINT>(image.width) * 3;
        hr = factory->CreateBitmapFromMemory(static_cast<UINT>(image.width),
                                             static_cast<UINT>(image.height),
                                             GUID_WICPixelFormat24bppBGR, stride,
                                             static_cast<UINT>(bgr.size()), bgr.data(),
                                             bitmap.address());
    }
    if (FAILED(hr)) {
        error = "创建编码位图失败: " + hresult_text(hr);
        return false;
    }

    hr = frame->WriteSource(bitmap.get(), nullptr);
    if (FAILED(hr)) {
        error = "写入编码像素失败: " + hresult_text(hr);
        return false;
    }
    hr = frame->Commit();
    if (FAILED(hr)) {
        error = "提交编码帧失败: " + hresult_text(hr);
        return false;
    }
    hr = encoder->Commit();
    if (FAILED(hr)) {
        error = "提交编码文件失败: " + hresult_text(hr);
        return false;
    }
    return true;
}

}  // namespace

bool platform_decode_image(const fs::path& path, Image& image, std::string& error) {
    ComScope com;
    if (!com.ok()) {
        error = "COM 初始化失败";
        return false;
    }
    ComPtr<IWICImagingFactory> factory;
    if (!create_factory(factory, error)) {
        return false;
    }
    return decode_with_wic(factory.get(), path, image, error);
}

bool platform_encode_image(const Image& image, const fs::path& path, const std::string& format,
                           std::string& error) {
    ComScope com;
    if (!com.ok()) {
        error = "COM 初始化失败";
        return false;
    }
    return encode_with_wic(image, path, format, error);
}

}  // namespace fc

#endif  // _WIN32