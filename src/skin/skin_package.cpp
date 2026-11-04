#include <ttpcomm/zip.h>
#include "ttplayer/skin/skin_package.h"

#include "ttpcomm_api.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <windows.h>
#include <shldisp.h>
#include <shlobj.h>
#include <shellapi.h>
#include <comdef.h>

namespace ttplayer::skin {
namespace {
uint16_t U16(const unsigned char* p){ return ttpcomm::bytes::Le16(p); }
uint32_t U32(const unsigned char* p){ return ttpcomm::bytes::Le32(p); }

bool ContainsRange(size_t size, size_t offset, size_t length) noexcept {
    return ttpcomm::bytes::Contains(size, offset, length);
}

bool SameEntryName(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    // Historical Windows ZIP writers use either separator. Materialization
    // uses generic paths, so retain both spellings when looking up an entry.
    for (size_t i = 0; i < left.size(); ++i) {
        const char a = left[i] == '\\' ? '/' : left[i];
        const char b = right[i] == '\\' ? '/' : right[i];
        if (_strnicmp(&a, &b, 1) != 0) return false;
    }
    return true;
}

std::wstring DecodeEntryName(std::string_view name, uint16_t flags) {
    if (name.empty() || name.find('\0') != name.npos || name.size() > INT_MAX) return {};
    // 0041F5BA uses OemToCharBuffW before comparing resource names.
    const UINT page = (flags & 0x800U) ? CP_UTF8 : CP_OEMCP;
    const DWORD options = page == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
    const int length = MultiByteToWideChar(page, options, name.data(), static_cast<int>(name.size()), nullptr, 0);
    if (!length) return {};
    std::wstring result(length, L'\0');
    if (!MultiByteToWideChar(page, options, name.data(), static_cast<int>(name.size()), result.data(), length)) return {};
    std::replace(result.begin(), result.end(), L'\\', L'/');
    return result;
}

bool SameDecodedEntryName(std::wstring_view left, std::wstring_view right) {
    if (left.empty() || left.size() != right.size() || left.size() > INT_MAX) return false;
    std::wstring normalized(right);
    std::replace(normalized.begin(), normalized.end(), L'\\', L'/');
    return CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, left.data(), static_cast<int>(left.size()),
        normalized.data(), static_cast<int>(normalized.size())) == CSTR_EQUAL;
}

template <typename Function>
Function TtpCommOrdinal(HMODULE module, WORD ordinal) noexcept {
    return module ? reinterpret_cast<Function>(
        GetProcAddress(module, MAKEINTRESOURCEA(ordinal))) : nullptr;
}
}
SkinPackage SkinPackage::Open(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary); if (!input) throw std::runtime_error("cannot open skin");
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(input)), {});
    return Parse(std::move(data), std::filesystem::absolute(path));
}

SkinPackage SkinPackage::OpenResource(HMODULE module, const wchar_t* name,
                                      const wchar_t* type) {
    if (!module || !name || !*name || !type || !*type)
        throw std::runtime_error("invalid skin resource");

    // TTPlayer 5.7.9 FUN_0041f73f: the archive source becomes a ZIP resource
    // when a module is supplied, using this exact Win32 resource sequence.
    const HRSRC resource = FindResourceW(module, name, type);
    if (!resource) throw std::runtime_error("skin ZIP resource not found");
    const DWORD size = SizeofResource(module, resource);
    if (!size) throw std::runtime_error("skin ZIP resource is empty");
    const HGLOBAL loaded = LoadResource(module, resource);
    if (!loaded) throw std::runtime_error("cannot load skin ZIP resource");
    const auto* bytes = static_cast<const unsigned char*>(LockResource(loaded));
    if (!bytes) throw std::runtime_error("cannot lock skin ZIP resource");
    return Parse(std::vector<unsigned char>(bytes, bytes + size));
}

SkinPackage SkinPackage::Parse(std::vector<unsigned char> data,
                               std::filesystem::path path) {
    const auto directory_end = ttpcomm::zip::FindDirectoryEnd({data.data(), data.size()},
        ttpcomm::zip::EndPolicy::legacy_skin);
    if (!directory_end) throw std::runtime_error("ZIP directory not found");
    const size_t eocd = *directory_end;
    const uint16_t count=U16(data.data()+eocd+10); size_t offset=U32(data.data()+eocd+16);
    SkinPackage skin;
    skin.path_ = std::move(path);
    skin.data_ = std::move(data);
    const auto& archive = skin.data_;
    for (uint16_t i=0;i<count;++i) {
        const auto record = ttpcomm::zip::ReadCentral({archive.data(), archive.size()}, offset);
        if (!record) throw std::runtime_error("invalid ZIP directory record");
        skin.entries_.push_back({std::string(record->name), record->packed, record->size,
            record->local, record->method, record->flags, DecodeEntryName(record->name, record->flags)});
        offset += record->record_size;
    }
    return skin;
}
bool SkinPackage::Has(std::string_view name) const {
    return std::ranges::any_of(entries_, [&](const auto& entry) {
        return SameEntryName(entry.name, name);
    });
}

std::vector<unsigned char> SkinPackage::ReadEntry(
    std::string_view name, HMODULE ttpcomm) const {
    const auto found = std::ranges::find_if(entries_, [&](const SkinEntry& entry) {
        return SameEntryName(entry.name, name);
    });
    if (found == entries_.end()) throw std::runtime_error("ZIP entry not found");
    return ReadEntry(*found, ttpcomm);
}

std::vector<unsigned char> SkinPackage::ReadEntry(
    const SkinEntry& entry, HMODULE ttpcomm) const {
    const auto* found = &entry;

    const size_t header = found->local_header_offset;
    if (!ContainsRange(data_.size(), header, 30) ||
        U32(data_.data() + header) != 0x04034b50) {
        throw std::runtime_error("invalid ZIP local header");
    }
    const uint16_t flags = U16(data_.data() + header + 6);
    if ((flags ^ found->flags) & 0x801U)
        throw std::runtime_error("ZIP filename encoding/encryption flags differ");
    const uint16_t compression = U16(data_.data() + header + 8);
    const uint16_t name_size = U16(data_.data() + header + 26);
    const uint16_t extra_size = U16(data_.data() + header + 28);
    const size_t variable_size = static_cast<size_t>(name_size) + extra_size;
    if (!ContainsRange(data_.size(), header + 30, variable_size))
        throw std::runtime_error("invalid ZIP local header fields");
    if (name_size != found->name.size() ||
        std::memcmp(data_.data() + header + 30, found->name.data(), name_size) != 0) {
        throw std::runtime_error("ZIP local and central names differ");
    }
    if ((flags & 1U) != 0) throw std::runtime_error("encrypted ZIP entry unsupported");
    if (compression != found->compression)
        throw std::runtime_error("ZIP compression fields differ");
    if ((flags & 8U) == 0 &&
        (U32(data_.data() + header + 18) != found->compressed_size ||
         U32(data_.data() + header + 22) != found->size)) {
        throw std::runtime_error("ZIP size fields differ");
    }

    const size_t payload = header + 30 + variable_size;
    if (!ContainsRange(data_.size(), payload, found->compressed_size))
        throw std::runtime_error("truncated ZIP entry");
    if (found->size > std::vector<unsigned char>{}.max_size())
        throw std::runtime_error("ZIP entry is too large");
    std::vector<unsigned char> output(found->size);
    if (compression == 0) {
        if (found->compressed_size != found->size)
            throw std::runtime_error("invalid stored ZIP entry size");
        if (!output.empty())
            std::memcpy(output.data(), data_.data() + payload, output.size());
        return output;
    }
    if (compression != 8) throw std::runtime_error("ZIP compression unsupported");

    const auto inflate_init = TtpCommOrdinal<TtpInflateInit2Fn>(ttpcomm, 80);
    const auto inflate = TtpCommOrdinal<TtpInflateFn>(ttpcomm, 81);
    const auto inflate_end = TtpCommOrdinal<TtpInflateEndFn>(ttpcomm, 82);
    if (!inflate_init || !inflate || !inflate_end)
        throw std::runtime_error("ttpcomm ZIP inflater unavailable");

    unsigned char empty_output{};
    TtpZStream stream{};
    stream.next_in = const_cast<unsigned char*>(data_.data() + payload);
    stream.avail_in = found->compressed_size;
    stream.next_out = output.empty() ? &empty_output : output.data();
    stream.avail_out = output.empty() ? 1U : found->size;
    constexpr int kRawDeflateWindowBits = -15;
    constexpr int kNoFlush = 0;
    constexpr int kOk = 0;
    constexpr int kStreamEnd = 1;
    if (inflate_init(&stream, kRawDeflateWindowBits, "1.1.4",
                     static_cast<int>(sizeof(stream))) != kOk) {
        throw std::runtime_error("cannot initialize ttpcomm ZIP inflater");
    }

    // FUN_0041F481 performs exactly one ordinal-81 call with Z_NO_FLUSH and
    // the central-directory output size.  Lines 32120-32126 normalize
    // Z_STREAM_END to zero and accept both that result and Z_OK.  This matters
    // for the old 1.1.4 raw inflater: when a buffer is filled exactly it can
    // return Z_OK even though all advertised output was produced.  Calling it
    // again (or requiring Z_STREAM_END) rejects valid bundled skins such as
    // A_LI/minimode_M.bmp and the default progress_fill_mini.bmp.
    const int result = inflate(&stream, kNoFlush);
    const std::string inflate_message = stream.msg ? stream.msg : "";
    const int end_result = inflate_end(&stream);
    if ((result != kOk && result != kStreamEnd) ||
        stream.total_out != found->size ||
        end_result != kOk) {
        throw std::runtime_error(
            "invalid deflated ZIP entry '" + found->name +
            "' (result=" + std::to_string(result) +
            ", end=" + std::to_string(end_result) +
            ", total_in=" + std::to_string(stream.total_in) +
            ", avail_in=" + std::to_string(stream.avail_in) +
            ", total_out=" + std::to_string(stream.total_out) +
            ", expected_out=" + std::to_string(found->size) +
            ", avail_out=" + std::to_string(stream.avail_out) +
            (inflate_message.empty() ? ")" : ", message=" + inflate_message + ")"));
    }
    return output;
}
bool SkinPackage::IsLegacyCompatible() const {
    // CSkinManager_LoadPackageXml (004A747D) requires Skin.xml, then
    // CSkinParser_ParsePlayerWindow (004A8536) resolves the background from
    // the player_window/image attribute.  The original never hard-codes
    // player_skin.bmp: valid supplied skins also use player.bmp and
    // Player-Window.bmp.
    return Has("Skin.xml");
}
uint64_t SkinPackage::Fingerprint() const noexcept {
    uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : data_) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

void SkinPackage::ExtractTo(const std::filesystem::path& directory) const {
    std::filesystem::create_directories(directory);
    const auto complete = [&] {
        return std::ranges::all_of(entries_, [&](const SkinEntry& entry) {
            if (entry.decoded_name.empty()) return false;
            const auto file = directory / std::filesystem::path(entry.decoded_name);
            std::error_code error;
            return std::filesystem::is_regular_file(file, error) && !error &&
                   std::filesystem::file_size(file, error) == entry.size && !error;
        });
    };
    if (complete()) return;

    auto shell_source = path_;
    if (path_.empty()) {
        // The original ZIP reader consumes LockResource memory directly. The
        // rebuild's extraction backend is Windows Compressed Folders, so only
        // this backend boundary is materialized; package discovery and parsing
        // still come directly from ttpres.dll.
        shell_source = directory / L"skin-resource.zip";
        std::error_code error;
        const bool current = std::filesystem::is_regular_file(shell_source, error) && !error &&
            std::filesystem::file_size(shell_source, error) == data_.size() && !error;
        if (!current) {
            std::ofstream output(shell_source, std::ios::binary | std::ios::trunc);
            if (!output || !output.write(reinterpret_cast<const char*>(data_.data()),
                                         static_cast<std::streamsize>(data_.size())))
                throw std::runtime_error("cannot materialize skin ZIP resource");
        }
    } else if (_wcsicmp(path_.extension().c_str(), L".zip") != 0) {
        shell_source = directory / L"skin-package.zip";
        if (!std::filesystem::exists(shell_source)) {
            std::error_code error;
            std::filesystem::create_hard_link(path_, shell_source, error);
            if (error) {
                error.clear();
                std::filesystem::copy_file(path_, shell_source,
                    std::filesystem::copy_options::overwrite_existing, error);
                if (error) throw std::runtime_error("cannot prepare skin ZIP alias");
            }
        }
    }

    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellDispatch* shell{};
    Folder* source{};
    Folder* target{};
    FolderItems* items{};
    auto release = [&] {
        if (items) items->Release();
        if (target) target->Release();
        if (source) source->Release();
        if (shell) shell->Release();
        if (SUCCEEDED(initialized)) CoUninitialize();
    };
    if (FAILED(CoCreateInstance(CLSID_Shell, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&shell)))) {
        release();
        throw std::runtime_error("Windows compressed-folder service unavailable");
    }
    _variant_t source_path(shell_source.wstring().c_str());
    _variant_t target_path(std::filesystem::absolute(directory).wstring().c_str());
    if (FAILED(shell->NameSpace(source_path, &source)) || !source ||
        FAILED(shell->NameSpace(target_path, &target)) || !target ||
        FAILED(source->Items(&items)) || !items) {
        release();
        throw std::runtime_error("cannot open skin as a compressed folder");
    }
    const HRESULT copied = [&] {
        // The dispatch VARIANT owns an additional reference to FolderItems.
        // Release it while the COM apartment is still initialized, including
        // on XP/Win7 where releasing Shell objects after CoUninitialize can
        // access already unloaded Shell code.
        _variant_t content(static_cast<IDispatch*>(items));
        _variant_t options(static_cast<long>(FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT));
        return target->CopyHere(content, options);
    }();
    if (FAILED(copied)) {
        release();
        throw std::runtime_error("skin extraction failed");
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (complete()) {
            release();
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    release();
    throw std::runtime_error("skin extraction timed out");
}

void SkinPackage::ExtractTo(const std::filesystem::path& directory,
                            HMODULE ttpcomm) const {
    std::filesystem::create_directories(directory);
    for (const auto& entry : entries_) {
        if (entry.name.empty())
            throw std::runtime_error("empty skin entry path");
        // Unflagged names retain the original OEM interpretation. Flagged
        // UTF-8 names are decoded once without an intervening ANSI roundtrip.
        const std::filesystem::path relative(entry.decoded_name);
        if (relative.empty() || relative.is_absolute() ||
            relative.has_root_name() || relative.has_root_directory()) {
            throw std::runtime_error("invalid absolute skin entry path");
        }
        for (const auto& component : relative) {
            if (component == L"..")
                throw std::runtime_error("skin entry escapes cache directory");
        }

        const auto target = directory / relative;
        if (entry.decoded_name.back() == L'/') {
            std::filesystem::create_directories(target);
            continue;
        }

        ExtractEntryTo(directory, std::wstring_view(entry.decoded_name), ttpcomm);
    }
}

bool SkinPackage::ExtractEntryTo(const std::filesystem::path& directory,
                                std::string_view name, HMODULE ttpcomm) const {
    const auto found = std::ranges::find_if(entries_, [&](const SkinEntry& entry) {
        return SameEntryName(entry.name, name);
    });
    // This overload is for callers holding a raw ZIP entry name. XML callers
    // use the wide overload, preserving characters outside the active ACP.
    if (found == entries_.end()) return false;
    if (found->decoded_name.empty()) throw std::runtime_error("invalid skin entry encoding");
    return ExtractEntryTo(directory, std::wstring_view(found->decoded_name), ttpcomm);
}

bool SkinPackage::ExtractEntryTo(const std::filesystem::path& directory,
                                std::wstring_view name, HMODULE ttpcomm) const {
    const std::filesystem::path relative(name);
    if (relative.empty() || relative.has_root_path() || name.find(L':') != name.npos || name.find(L'\0') != name.npos)
        throw std::runtime_error("invalid skin entry path");
    for (const auto& component : relative) {
        if (component == L"..") throw std::runtime_error("skin entry escapes cache directory");
    }
    const auto found = std::ranges::find_if(entries_, [&](const SkinEntry& entry) {
        return SameDecodedEntryName(entry.decoded_name, name);
    });
    if (found == entries_.end()) return false;
    // The original archive reader only opens referenced resources. Materialize
    // one entry at a time, and never trust a same-length cache file's contents.
    const auto contents = ReadEntry(*found, ttpcomm);
    const auto target = directory / relative;
    std::error_code error;
    if (std::filesystem::is_regular_file(target, error) && !error &&
        std::filesystem::file_size(target, error) == contents.size() && !error) {
        std::ifstream input(target, std::ios::binary);
        const std::vector<unsigned char> cached((std::istreambuf_iterator<char>(input)), {});
        if (input && cached == contents) return true;
    }
    std::filesystem::create_directories(target.parent_path());
    std::ofstream output(target, std::ios::binary | std::ios::trunc);
    if (!output || (!contents.empty() &&
        !output.write(reinterpret_cast<const char*>(contents.data()),
                      static_cast<std::streamsize>(contents.size()))))
        throw std::runtime_error("cannot materialize skin entry");
    output.close();
    if (!output) throw std::runtime_error("cannot finish skin entry");
    return true;
}
}
