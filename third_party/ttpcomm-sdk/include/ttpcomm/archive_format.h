#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>

namespace ttpcomm::archive_format {
enum class Kind { unknown,zip,rar,seven,tar,gzip,targzip };
struct VolumeName {
    Kind kind{Kind::unknown}; std::wstring base; unsigned index{}, width{}; bool numeric{};
    explicit operator bool() const { return kind != Kind::unknown; }
};
inline VolumeName Volume(const wchar_t* path) {
    VolumeName result;if(!path)return result;
    const std::wstring name(path);const auto dot=name.rfind(L'.');if(dot==name.npos)return result;
    auto digits=name.substr(dot+1);bool numeric=true;
    if(digits.size()>=3 && (digits[0]==L'z' || digits[0]==L'Z')){numeric=false;digits.erase(0,1);}
    if(digits.size()<(numeric?3u:2u) || digits.size()>4)return result;
    unsigned index=0;for(auto c:digits){if(c<L'0' || c>L'9')return result;index=index*10+c-L'0';}
    if(!index || index>4096)return result;
    const auto base=name.substr(0,dot);
    if(numeric) {
        const auto n=base.size();
        if(n>=4 && !_wcsicmp(base.c_str()+n-4,L".zip"))result.kind=Kind::zip;
        else if(n>=3 && !_wcsicmp(base.c_str()+n-3,L".7z"))result.kind=Kind::seven;
        else return result;
    } else result.kind=Kind::zip;
    result.base=base;result.index=index;result.width=numeric?3:2;result.numeric=numeric;return result;
}
inline std::wstring Canonical(const wchar_t* path) {
    const auto part=Volume(path);if(!part)return path?path:L"";
    return part.base+(part.numeric?L".001":L".zip");
}
inline Kind Suffix(const wchar_t* path) {
    if(!path)return Kind::unknown;const size_t n=wcslen(path);
    const auto ends=[&](const wchar_t* s){const size_t k=wcslen(s);return n>=k && !_wcsicmp(path+n-k,s);};
    if(ends(L".tar.gz") || ends(L".tgz"))return Kind::targzip;
    if(ends(L".zip"))return Kind::zip;if(ends(L".rar"))return Kind::rar;
    if(ends(L".7z"))return Kind::seven;if(ends(L".tar"))return Kind::tar;
    if(ends(L".gz"))return Kind::gzip;return Volume(path).kind;
}
// Strong signatures win over suffixes. Never scan executables for embedded
// archives or infer TAR from just its name/magic: validate the header checksum.
inline Kind Header(const wchar_t* path) {
    if(!path)return Kind::unknown;
    const HANDLE f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE)return Kind::unknown;
    unsigned char b[512]{};DWORD n{};const bool okay=ReadFile(f,b,sizeof(b),&n,nullptr)!=FALSE;CloseHandle(f);
    if(!okay)return Kind::unknown;
    if(n>=4 && b[0]=='P' && b[1]=='K' && ((b[2]==3 && b[3]==4)||(b[2]==5 && b[3]==6)||(b[2]==7 && b[3]==8)))return Kind::zip;
    if(n>=7 && !memcmp(b,"Rar!\x1a\x07\x00",7))return Kind::rar;
    if(n>=8 && !memcmp(b,"Rar!\x1a\x07\x01\x00",8))return Kind::rar;
    if(n>=6 && !memcmp(b,"7z\xbc\xaf\x27\x1c",6))return Kind::seven;
    if(n>=3 && b[0]==0x1f && b[1]==0x8b && b[2]==8)return Kind::gzip;
    if(n==512 && b[0]) {
        uint32_t declared=0,sum=0;bool digit=false,ended=false;
        for(unsigned i=148;i<156;++i){
            if(b[i]>='0' && b[i]<='7'){if(ended)return Kind::unknown;digit=true;declared=declared*8+b[i]-'0';}
            else if(b[i]==0 || b[i]==' '){if(digit)ended=true;}
            else return Kind::unknown;
        }
        for(unsigned i=0;i<512;++i)sum+=(i>=148 && i<156)?32:b[i];
        if(digit && sum==declared)return Kind::tar;
    }
    return Kind::unknown;
}
inline Kind Detect(const wchar_t* path) {const auto k=Header(path);return k==Kind::unknown?Suffix(path):k;}
}
