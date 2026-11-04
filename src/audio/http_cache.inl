// Each partial-cache record authenticates its byte range and payload. A torn
// final record never advertises absent bytes. The old complete cache stays readable.
struct CacheHash {
    HCRYPTPROV provider{};HCRYPTHASH hash{};
    CacheHash() {
        Check(CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT));
        if(!CryptCreateHash(provider,CALG_SHA_256,0,0,&hash)) {
            const auto error=GetLastError();CryptReleaseContext(provider,0);provider=0;
            throw Failure{HRESULT_FROM_WIN32(error)};
        }
    }
    ~CacheHash(){if(hash) CryptDestroyHash(hash);if(provider) CryptReleaseContext(provider,0);}
    void Add(const void* bytes,DWORD size){Check(CryptHashData(hash,static_cast<const BYTE*>(bytes),size,0));}
    std::array<BYTE,32> Finish(){std::array<BYTE,32> bytes{};DWORD n=32;Check(CryptGetHashParam(hash,HP_HASHVAL,bytes.data(),&n,0));return bytes;}
};
struct CacheHeader {char magic[8];uint64_t bytes;std::array<BYTE,32> digest;};
struct CacheRecord {uint64_t offset;uint32_t bytes,reserved;std::array<BYTE,32> digest;};
static_assert(sizeof(CacheHeader)==48 && sizeof(CacheRecord)==48);
void MergeCoverage(std::map<uint64_t,uint64_t>& coverage,uint64_t first,uint64_t last) {
    auto it=coverage.lower_bound(first);
    if(it!=coverage.begin() && std::prev(it)->second>=first) --it;
    while(it!=coverage.end() && it->first<=last) {
        first=std::min(first,it->first);last=std::max(last,it->second);it=coverage.erase(it);
    }
    coverage[first]=last;
}
void FilePosition(HANDLE file,uint64_t offset) {
    LARGE_INTEGER position{};position.QuadPart=offset;Check(SetFilePointerEx(file,position,nullptr,FILE_BEGIN));
}
void FileWrite(HANDLE file,const void* bytes,DWORD count) {
    DWORD wrote{};Check(WriteFile(file,bytes,count,&wrote,nullptr));Require(wrote==count);
}
bool FileRead(HANDLE file,void* bytes,DWORD count) {
    DWORD got{};return ReadFile(file,bytes,count,&got,nullptr) && got==count;
}
std::array<BYTE,32> RecordDigest(const CacheRecord& record,const BYTE* bytes) {
    CacheHash hash;hash.Add(&record,16);hash.Add(bytes,record.bytes);return hash.Finish();
}
// The directory lease serializes quota accounting across processes. An active
// journal denies delete and a second writer; eviction cannot remove it.
bool CacheRoom(const std::filesystem::path& folder,uint64_t extra,uint64_t quota) {
    struct Entry {std::filesystem::path path;uint64_t size;std::filesystem::file_time_type time;};
    std::vector<Entry> entries;uint64_t used{};std::error_code error;
    for(const auto& entry:std::filesystem::directory_iterator(folder,error)) {
        const auto ext=entry.path().extension();
        if((ext!=L".ttpcache" && ext!=L".ttppart") || !entry.is_regular_file(error)) continue;
        // Directory metadata can lag behind an open writer on Windows.
        File live{CreateFileW(entry.path().c_str(),GENERIC_READ,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr)};
        LARGE_INTEGER actual{};
        if(live.value==INVALID_HANDLE_VALUE || !GetFileSizeEx(live.value,&actual) || actual.QuadPart<0) continue;
        const auto size=static_cast<uint64_t>(actual.QuadPart);
        const auto time=entry.last_write_time(error);if(error){error.clear();continue;}
        used+=size;entries.push_back({entry.path(),size,time});
    }
    std::sort(entries.begin(),entries.end(),[](const Entry& a,const Entry& b){return a.time<b.time;});
    for(const auto& entry:entries) {
        if(used+extra<=quota) break;
        File file{CreateFileW(entry.path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,
                             nullptr,OPEN_EXISTING,0,nullptr)};
        CacheHeader header{};
        if(file.value==INVALID_HANDLE_VALUE || !FileRead(file.value,&header,sizeof(header)) ||
           (memcmp(header.magic,"TTPHTTP2",8) && memcmp(header.magic,"TTPHTTP3",8))) continue;
        if(DeleteFileW(entry.path.c_str())) used-=entry.size;
    }
    return used+extra<=quota;
}
std::filesystem::path CacheTask::PersistentPath() const {
    if(!options_.network_cache_enabled || options_.network_cache_folder.empty() || sequential_ ||
       !total_ || validator_.empty() || options_.network_cache_space_mb<=0) return {};
    CacheHash hash;
    hash.Add(url_.data(),static_cast<DWORD>(url_.size()*sizeof(wchar_t)));
    const wchar_t delimiter=0;hash.Add(&delimiter,sizeof(delimiter));
    hash.Add(validator_.data(),static_cast<DWORD>(validator_.size()));hash.Add(&*total_,sizeof(*total_));
    std::wstring key;const auto digest=hash.Finish();
    for(auto b:digest){key+=L"0123456789abcdef"[b>>4];key+=L"0123456789abcdef"[b&15];}
    return options_.network_cache_folder/L"HttpAudio-v2"/(key+L".ttpcache");
}
void CacheTask::LoadPersistent() {
    // Fresh response headers have identified this resource. The URL and file
    // length alone are insufficient to splice a previously downloaded prefix.
    try {
        const auto path=PersistentPath();if(path.empty()) return;
        File file{CreateFileW(path.c_str(),GENERIC_READ|FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ,
                             nullptr,OPEN_EXISTING,0,nullptr)};
        if(file.value!=INVALID_HANDLE_VALUE) {
            LARGE_INTEGER size{};Check(GetFileSizeEx(file.value,&size));CacheHeader header{};
            Require(FileRead(file.value,&header,sizeof(header)) && !memcmp(header.magic,"TTPHTTP2",8) &&
                    header.bytes==*total_ && size.QuadPart>=0 && uint64_t(size.QuadPart)==header.bytes+sizeof(header));
            CacheHash hash;std::array<BYTE,65536> bytes{};uint64_t left=header.bytes;MoveCache(0);
            while(left) {
                Guard();const auto n=static_cast<DWORD>(std::min<uint64_t>(left,bytes.size()));
                Require(FileRead(file.value,bytes.data(),n));hash.Add(bytes.data(),n);
                FileWrite(cache_.value,bytes.data(),n);left-=n;
            }
            Require(hash.Finish()==header.digest);
            FILETIME now{};GetSystemTimeAsFileTime(&now);SetFileTime(file.value,nullptr,nullptr,&now);
            coverage_[0]=*total_;persisted_=true;UpdateProgress();CloseRequest();return;
        }
    } catch(...) {Guard();}
    try {
        auto path=PersistentPath();if(path.empty()) return;path.replace_extension(L".ttppart");
        const auto quota=uint64_t(std::clamp(options_.network_cache_space_mb,1,2000))*1024*1024;
        if(*total_+sizeof(CacheHeader)>quota) return;
        std::filesystem::create_directories(path.parent_path());
        File lease{CreateFileW((path.parent_path()/L"cache.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,
                              nullptr,OPEN_ALWAYS,FILE_FLAG_DELETE_ON_CLOSE,nullptr)};
        if(lease.value==INVALID_HANDLE_VALUE) return;
        const bool room=CacheRoom(path.parent_path(),std::filesystem::exists(path)?0:sizeof(CacheHeader),quota);
        persistent_.value=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,
                                     room?OPEN_ALWAYS:OPEN_EXISTING,0,nullptr);
        bool writable=persistent_.value!=INVALID_HANDLE_VALUE;
        if(!writable) persistent_.value=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,
                                                   nullptr,OPEN_EXISTING,0,nullptr);
        if(persistent_.value==INVALID_HANDLE_VALUE) return;
        persistent_path_=path;
        LARGE_INTEGER size{};Check(GetFileSizeEx(persistent_.value,&size));CacheHeader header{};
        const auto stem=path.stem().wstring();CacheHash identity;
        identity.Add(stem.data(),static_cast<DWORD>(stem.size()*sizeof(wchar_t)));const auto key=identity.Finish();
        uint64_t valid{};std::array<BYTE,16384> bytes{};
        if(FileRead(persistent_.value,&header,sizeof(header)) && !memcmp(header.magic,"TTPHTTP3",8) &&
           header.bytes==*total_ && header.digest==key && size.QuadPart>=sizeof(header) && uint64_t(size.QuadPart)<=quota) {
            valid=sizeof(header);
            for(unsigned records=0;records<262144 && valid<uint64_t(size.QuadPart);++records) {
                Guard();CacheRecord record{};
                if(!FileRead(persistent_.value,&record,sizeof(record)) || record.reserved || !record.bytes ||
                   record.bytes>bytes.size() || record.offset>*total_ || record.bytes>*total_-record.offset ||
                   !FileRead(persistent_.value,bytes.data(),record.bytes) || record.digest!=RecordDigest(record,bytes.data())) break;
                MoveCache(record.offset);FileWrite(cache_.value,bytes.data(),record.bytes);
                MergeCoverage(coverage_,record.offset,record.offset+record.bytes);
                valid+=sizeof(record)+record.bytes;
            }
        }
        stored_=coverage_;UpdateProgress();persistent_size_=valid;persistent_tick_=GetTickCount();
        if(!writable) {persistent_.Close();return;}
        FilePosition(persistent_.value,valid);Check(SetEndOfFile(persistent_.value));
        if(!valid) {
            memcpy(header.magic,"TTPHTTP3",8);header.bytes=*total_;header.digest=key;
            FileWrite(persistent_.value,&header,sizeof(header));persistent_size_=sizeof(header);
        }
        if(percent_==100) {CloseRequest();FlushPersistent();}
    } catch(...) {persistent_.Close();Guard();}
}
void CacheTask::SavePersistent(uint64_t begin,const BYTE* bytes,DWORD count) noexcept {
    if(persistent_.value==INVALID_HANDLE_VALUE || persisted_) return;
    try {
        File lease{CreateFileW((persistent_path_.parent_path()/L"cache.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,
                              nullptr,OPEN_ALWAYS,FILE_FLAG_DELETE_ON_CLOSE,nullptr)};
        if(lease.value==INVALID_HANDLE_VALUE) return;
        const auto quota=uint64_t(std::clamp(options_.network_cache_space_mb,1,2000))*1024*1024;
        const auto end=begin+count;
        for(auto position=begin;position<end;) {
            auto it=stored_.upper_bound(position);
            if(it!=stored_.begin() && std::prev(it)->second>position) {position=std::prev(it)->second;continue;}
            const auto last=it==stored_.end()?end:std::min(end,it->first);
            CacheRecord record{};record.offset=position;record.bytes=static_cast<uint32_t>(last-position);
            record.digest=RecordDigest(record,bytes+(position-begin));
            if(!CacheRoom(persistent_path_.parent_path(),sizeof(record)+record.bytes,quota)) return;
            FilePosition(persistent_.value,persistent_size_);
            FileWrite(persistent_.value,&record,sizeof(record));
            FileWrite(persistent_.value,bytes+(position-begin),record.bytes);
            persistent_size_+=sizeof(record)+record.bytes;unflushed_+=sizeof(record)+record.bytes;
            MergeCoverage(stored_,position,last);position=last;
        }
        if(unflushed_>=256*1024 || GetTickCount()-persistent_tick_>=1000 || percent_==100) FlushPersistent();
    } catch(...) {
        // A storage failure must not become a playback failure. The last valid
        // journal prefix remains usable on the next open.
        persistent_.Close();
    }
}
void CacheTask::FlushPersistent() noexcept {
    if(persistent_.value!=INVALID_HANDLE_VALUE) {
        FlushFileBuffers(persistent_.value);unflushed_=0;persistent_tick_=GetTickCount();
    }
}
