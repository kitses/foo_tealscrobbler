#include "config.h"

#include <wincrypt.h>
#include <windows.h>

#include <vector>

namespace
{

constexpr char kEntropy[] = "foo_tealscrobbler/1";

DATA_BLOB entropy_blob()
{
    return DATA_BLOB{static_cast<DWORD>(sizeof(kEntropy) - 1), reinterpret_cast<BYTE *>(const_cast<char *>(kEntropy))};
}

std::string base64_encode(const BYTE *data, DWORD size)
{
    DWORD chars = 0;
    if (!CryptBinaryToStringA(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &chars))
    {
        return {};
    }
    std::string out(chars, '\0');
    if (!CryptBinaryToStringA(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &chars))
    {
        return {};
    }
    out.resize(chars);
    while (!out.empty() && out.back() == '\0')
    {
        out.pop_back();
    }
    return out;
}

std::vector<BYTE> base64_decode(std::string_view text)
{
    DWORD size = 0;
    if (!CryptStringToBinaryA(text.data(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64, nullptr, &size,
                              nullptr, nullptr))
    {
        return {};
    }
    std::vector<BYTE> data(size);
    if (!CryptStringToBinaryA(text.data(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64, data.data(), &size,
                              nullptr, nullptr))
    {
        return {};
    }
    data.resize(size);
    return data;
}

} // namespace

static constexpr GUID guid_cfg_enabled = {0x0119984c, 0x5f68, 0x452b, {0x98, 0x4f, 0x8c, 0xec, 0xb8, 0x1e, 0x41, 0x54}};
static constexpr GUID guid_cfg_handle = {0xb022bb8e, 0xc97b, 0x44e7, {0xae, 0xfa, 0xd7, 0x1c, 0xcb, 0x47, 0x01, 0x73}};
static constexpr GUID guid_cfg_password = {
    0x29acad57, 0x9022, 0x4245, {0xad, 0x17, 0x1b, 0x01, 0xc3, 0x4b, 0x6f, 0x5f}};
static constexpr GUID guid_cfg_media_library_only = {
    0x64cf5023, 0xcbb1, 0x4dba, {0xad, 0x75, 0x0a, 0x43, 0x55, 0x79, 0x9c, 0xa3}};
static constexpr GUID guid_cfg_dynamic_sources = {
    0x9261f1f1, 0x037f, 0x4fae, {0x98, 0xd3, 0x15, 0x5d, 0xfd, 0xec, 0x28, 0x7e}};
static constexpr GUID guid_cfg_artist = {0x4572ed5d, 0x1413, 0x4fbf, {0xb8, 0x45, 0x16, 0x2b, 0x6a, 0x41, 0x8c, 0x25}};
static constexpr GUID guid_cfg_album = {0x7e6742bd, 0x377c, 0x4124, {0xa3, 0x03, 0x01, 0x4d, 0x6b, 0xb7, 0x05, 0x37}};
static constexpr GUID guid_cfg_title = {0xf6b1824e, 0x5101, 0x4bd7, {0xb5, 0x58, 0x2c, 0xf9, 0xec, 0x43, 0x78, 0xa0}};
static constexpr GUID guid_cfg_mb_track = {
    0x82668385, 0x7802, 0x421d, {0x8f, 0xb2, 0x2f, 0xd5, 0x8f, 0xf4, 0x6c, 0x82}};
static constexpr GUID guid_cfg_queue = {0xded2679c, 0xfaf5, 0x40f0, {0x9f, 0xf0, 0x02, 0x59, 0x27, 0x80, 0x86, 0xf4}};

cfg_bool cfg_enabled(guid_cfg_enabled, true);
cfg_string cfg_handle(guid_cfg_handle, "");
cfg_string cfg_password(guid_cfg_password, "");
cfg_bool cfg_media_library_only(guid_cfg_media_library_only, false);
cfg_bool cfg_dynamic_sources(guid_cfg_dynamic_sources, true);
cfg_string cfg_artist(guid_cfg_artist, "[%artist%]");
cfg_string cfg_album(guid_cfg_album, "[%album%]");
cfg_string cfg_title(guid_cfg_title, "[%title%]");
cfg_string cfg_mb_track(guid_cfg_mb_track, "[%musicbrainz_trackid%]");
cfg_string cfg_queue(guid_cfg_queue, "[]");

std::string protect_secret(std::string_view value)
{
    if (value.empty())
    {
        return {};
    }
    DATA_BLOB input{static_cast<DWORD>(value.size()), reinterpret_cast<BYTE *>(const_cast<char *>(value.data()))};
    DATA_BLOB entropy = entropy_blob();
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"teal.fm app password", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                          &output))
    {
        return "plain:" + std::string(value);
    }
    std::string result = "dpapi:" + base64_encode(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
}

std::optional<std::string> unprotect_secret(std::string_view value)
{
    if (value.empty())
    {
        return std::string();
    }
    if (value.rfind("plain:", 0) == 0)
    {
        return std::string(value.substr(6));
    }
    if (value.rfind("dpapi:", 0) != 0)
    {
        return std::string(value);
    }
    const std::vector<BYTE> encoded = base64_decode(value.substr(6));
    if (encoded.empty())
    {
        return std::nullopt;
    }
    DATA_BLOB input{static_cast<DWORD>(encoded.size()), const_cast<BYTE *>(encoded.data())};
    DATA_BLOB entropy = entropy_blob();
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
    {
        return std::nullopt;
    }
    std::string result(reinterpret_cast<char *>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return result;
}

std::optional<std::string> get_app_password()
{
    return unprotect_secret(cfg_password.get().get_ptr());
}

void set_app_password(std::string_view value)
{
    cfg_password = protect_secret(value).c_str();
}
