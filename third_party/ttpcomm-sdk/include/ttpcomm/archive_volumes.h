#pragma once
#include "archive_format.h"
#include "archive_api.h"
#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

// Read-only concatenated input. ZIP disk-relative offsets are translated
// separately; numeric 7-Zip volumes already use global concatenated offsets.
namespace ttpcomm::archive_volumes {
inline void Check(bool ok,HRESULT hr=HRESULT_FROM_WIN32(ERROR_INVALID_DATA)){if(!ok)throw hr;}
inline uint16_t U16(const unsigned char* p){return uint16_t(p[0])|(uint16_t(p[1])<<8);}
inline uint32_t U32(const unsigned char* p){return uint32_t(U16(p))|(uint32_t(U16(p+2))<<16);}
inline uint64_t U64(const unsigned char* p){return uint64_t(U32(p))|(uint64_t(U32(p+4))<<32);}
inline uint64_t ChangeTime(HANDLE file){
    // FileBasicInformation is available on XP's native file API as well.
    // Resolve it dynamically; do not add a Vista-only static import.
    struct Basic {LARGE_INTEGER created,accessed,written,changed;ULONG attributes;};
    struct IoStatus {ULONG_PTR status,information;};
    using Query=LONG (NTAPI*)(HANDLE,IoStatus*,void*,ULONG,ULONG);
    // Do not use a dynamically initialized function-local static here: the
    // compiler's TLS initialization guard can skip it in an XP-loaded DLL.
    const auto query=reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryInformationFile"));
    Basic basic{};IoStatus status{};
    return query && query(file,&status,&basic,sizeof(basic),4)>=0 && basic.changed.QuadPart>0 ?
        static_cast<uint64_t>(basic.changed.QuadPart):0;
}
struct File {
    HANDLE handle{INVALID_HANDLE_VALUE}; uint64_t size{}; BY_HANDLE_FILE_INFORMATION stamp{};
    uint64_t change_time{};
    std::wstring path;
    explicit File(std::wstring name):path(std::move(name)) {
        handle=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(handle==INVALID_HANDLE_VALUE)throw HRESULT_FROM_WIN32(GetLastError());
        if(!GetFileInformationByHandle(handle,&stamp) || stamp.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY){
            CloseHandle(handle);handle=INVALID_HANDLE_VALUE;throw HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }
        size=(uint64_t(stamp.nFileSizeHigh)<<32)|stamp.nFileSizeLow;
        change_time=ChangeTime(handle);
    }
    ~File(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
    File(const File&)=delete;
    void Read(uint64_t at,void* bytes,DWORD count) const {
        Check(at<=size && count<=size-at,HRESULT_FROM_WIN32(ERROR_HANDLE_EOF));
        LARGE_INTEGER offset{};offset.QuadPart=static_cast<LONGLONG>(at);DWORD read{};
        if(!SetFilePointerEx(handle,offset,nullptr,FILE_BEGIN) || !ReadFile(handle,bytes,count,&read,nullptr))throw HRESULT_FROM_WIN32(GetLastError());
        Check(read==count,HRESULT_FROM_WIN32(ERROR_HANDLE_EOF));
    }
};
inline std::wstring Number(unsigned n,unsigned width){auto s=std::to_wstring(n);if(s.size()<width)s.insert(0,width-s.size(),L'0');return s;}
class Set {
    std::vector<std::unique_ptr<File>> files_;
    std::vector<uint64_t> starts_;
    uint64_t length_{};bool disks_{},reliable_{true};
    std::wstring path_,key_;
    std::function<bool()> canceled_;
    std::function<std::wstring(const std::wstring&)> resolve_;
    void Poll() const {if(canceled_ && canceled_())throw HRESULT_FROM_WIN32(ERROR_CANCELLED);}
    void Add(std::wstring name,bool required) {
        Poll();std::unique_ptr<File> file;
        Check(files_.size()<4096,TTPCOMM_ARCHIVE_UNSUPPORTED);
        if(resolve_)name=resolve_(name);
        try{file=std::make_unique<File>(std::move(name));}
        catch(HRESULT hr){if(required && (hr==HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr==HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)))throw TTPCOMM_ARCHIVE_MISSING_VOLUME;throw;}
        for(const auto& old:files_)Check(old->stamp.dwVolumeSerialNumber!=file->stamp.dwVolumeSerialNumber ||
            old->stamp.nFileIndexHigh!=file->stamp.nFileIndexHigh || old->stamp.nFileIndexLow!=file->stamp.nFileIndexLow,
            HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
        Check(file->size<=uint64_t(INT64_MAX)-length_,HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE));
        starts_.push_back(length_);length_+=file->size;
        key_+=L"|"+file->path;const auto& i=file->stamp;
        for(DWORD v:{i.dwVolumeSerialNumber,i.nFileIndexHigh,i.nFileIndexLow,i.nFileSizeHigh,i.nFileSizeLow,
            i.ftLastWriteTime.dwHighDateTime,i.ftLastWriteTime.dwLowDateTime})key_+=L"|"+std::to_wstring(v);
        key_+=L"|ct="+std::to_wstring(file->change_time);reliable_=reliable_ && file->change_time!=0;
        files_.push_back(std::move(file));
    }
    bool ZipEnd() const {
        if(length_<22)return false;
        const auto n=static_cast<DWORD>(std::min<uint64_t>(length_,65557));
        std::vector<unsigned char> tail(n);Read(length_-n,tail.data(),n);
        for(size_t p=n-22;;--p){
            if(U32(tail.data()+p)==0x06054b50 && p+22+U16(tail.data()+p+20)==n)return true;
            if(!p)break;
        }
        return false;
    }
    static unsigned ZipDisks(const File& file) {
        if(file.size<22)return 1;
        const auto n=static_cast<DWORD>(std::min<uint64_t>(file.size,65535+42));
        std::vector<unsigned char> tail(n);file.Read(file.size-n,tail.data(),n);
        for(size_t p=n-22;;--p) {
            if(U32(tail.data()+p)==0x06054b50 && p+22+U16(tail.data()+p+20)==n) {
                unsigned count=unsigned(U16(tail.data()+p+4))+1;
                if(count==65536 && p>=20 && U32(tail.data()+p-20)==0x07064b50)count=U32(tail.data()+p-4);
                Check(count && count<=4096,TTPCOMM_ARCHIVE_UNSUPPORTED);return count;
            }
            if(!p)break;
        }
        return 1;
    }
public:
    explicit Set(const wchar_t* input,const std::function<bool()>& canceled={},
        const std::function<std::wstring(const std::wstring&)>& resolve={}):canceled_(canceled),resolve_(resolve) {
        Poll();Check(input && *input,E_INVALIDARG);
        const auto canonical=archive_format::Canonical(input);
        const DWORD n=GetFullPathNameW(canonical.c_str(),0,nullptr,nullptr);Check(n!=0,HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME));
        std::vector<wchar_t> full(n);Check(GetFullPathNameW(canonical.c_str(),n,full.data(),nullptr)!=0,HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME));path_=full.data();
        const auto part=archive_format::Volume(path_.c_str());
        if(part && part.numeric) {
            // Discover the highest conventional sibling, then require every
            // intervening volume. No merging, extraction, or unbounded buffers.
            unsigned last=1;WIN32_FIND_DATAW data{};
            const auto pattern=part.base+L".*";HANDLE find=FindFirstFileW(pattern.c_str(),&data);
            if(find!=INVALID_HANDLE_VALUE){
                struct End{HANDLE h;~End(){FindClose(h);}}end{find};
                do{Poll();const auto item=archive_format::Volume(data.cFileName);
                    const auto base=part.base.substr(part.base.find_last_of(L"/\\")+1);
                    if(item && item.numeric && !_wcsicmp(item.base.c_str(),base.c_str()) &&
                        std::wstring(data.cFileName)==item.base+L"."+Number(item.index,3))last=std::max(last,item.index);
                }while(FindNextFileW(find,&data));
            }
            const auto requested=archive_format::Volume(input);if(requested)last=std::max(last,requested.index);
            for(unsigned i=1;i<=last;++i)Add(part.base+L"."+Number(i,3),true);
            if(part.kind==archive_format::Kind::seven){
                unsigned char head[32]{};Check(length_>=sizeof(head),TTPCOMM_ARCHIVE_MISSING_VOLUME);Read(0,head,sizeof(head));
                if(!memcmp(head,"7z\xbc\xaf\x27\x1c",6)){
                    const auto offset=U64(head+12),bytes=U64(head+20);
                    Check(offset<=UINT64_MAX-32 && bytes<=UINT64_MAX-32-offset);
                    while(32+offset+bytes>length_){
                        Check(bool(resolve_),TTPCOMM_ARCHIVE_MISSING_VOLUME);
                        Add(part.base+L"."+Number(++last,3),true);
                    }
                }
            }
            if(part.kind==archive_format::Kind::zip && resolve_)
                while(!ZipEnd())Add(part.base+L"."+Number(++last,3),true);
        } else if(archive_format::Suffix(path_.c_str())==archive_format::Kind::zip) {
            std::unique_ptr<File> final;
            try{final=std::make_unique<File>(resolve_?resolve_(path_):path_);}catch(HRESULT hr){if(archive_format::Volume(input) &&
                (hr==HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr==HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)))throw TTPCOMM_ARCHIVE_MISSING_VOLUME;throw;}
            const auto count=ZipDisks(*final);final.reset();disks_=count>1;
            const auto base=path_.substr(0,path_.size()-4);
            for(unsigned i=1;i<count;++i)Add(base+L".z"+Number(i,2),true);
            Add(path_,disks_);
        } else Add(path_,false);
    }
    // Already discovered files (notably UnRAR's actual volume notifications).
    // Keep the logical first path while retaining every actual file identity.
    Set(std::wstring logical,const std::vector<std::wstring>& files,const std::function<bool()>& canceled={})
        :path_(std::move(logical)),canceled_(canceled){Check(!files.empty(),E_INVALIDARG);for(const auto& p:files)Add(p,true);}
    uint64_t Length() const {return length_;}
    const std::wstring& Path() const {return path_;}
    const std::wstring& Key() const {return key_;}
    bool Reliable() const {return reliable_;}
    void ForgetCallbacks(){canceled_={};resolve_={};}
    std::vector<std::wstring> Paths() const {std::vector<std::wstring> out;for(const auto& f:files_)out.push_back(f->path);return out;}
    bool Split() const {return files_.size()>1 || bool(archive_format::Volume(path_.c_str()));}
    unsigned Disks() const {return disks_?static_cast<unsigned>(files_.size()):1;}
    uint64_t DiskOffset(uint32_t disk,uint64_t offset) const {
        Check(disk<Disks(),TTPCOMM_ARCHIVE_MISSING_VOLUME);
        if(!disks_){Check(offset<=length_);return offset;}
        Check(offset<=files_[disk]->size);return starts_[disk]+offset;
    }
    void Read(uint64_t at,void* bytes,DWORD count) const {
        Check(at<=length_ && count<=length_-at,HRESULT_FROM_WIN32(ERROR_HANDLE_EOF));
        auto* out=static_cast<unsigned char*>(bytes);
        while(count){Poll();const auto upper=std::upper_bound(starts_.begin(),starts_.end(),at);
            const auto index=static_cast<size_t>(upper-starts_.begin()-1);const auto offset=at-starts_[index];
            const DWORD n=static_cast<DWORD>(std::min<uint64_t>(count,files_[index]->size-offset));Check(n!=0);
            files_[index]->Read(offset,out,n);out+=n;at+=n;count-=n;
        }
    }
};
}
