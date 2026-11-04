#include <ttpcomm/runtime_client.h>
#include <cstddef>
#include <cwchar>

namespace ttpcomm::host {
bool QueryArchiveGroups(HMODULE module,TtpCommArchiveGroupApi& api) noexcept {
    api={};api.size=sizeof(api);
    const auto query=module?reinterpret_cast<TtpCommQueryArchiveGroupFn>(GetProcAddress(module,"ttpcomm_query_archive_stream")):nullptr;
    if(!query)return false;
    __try{return query(TTPCOMM_ARCHIVE_GROUP_ABI,&api) && api.size>=sizeof(api) && api.abi_version==TTPCOMM_ARCHIVE_GROUP_ABI &&
        (api.capabilities&TTPCOMM_ARCHIVE_GROUP_REQUIRED)==TTPCOMM_ARCHIVE_GROUP_REQUIRED && api.enumerate && api.open_member && api.volumes && api.status && api.clear_cache;
    }__except(EXCEPTION_EXECUTE_HANDLER){api={};return false;}
}
const TtpCommArchiveGroupApi* ArchiveGroups() noexcept {
    struct State {
        HMODULE module{};TtpCommArchiveGroupApi api{};
        State() noexcept {
            if(!GetModuleHandleExW(0,L"ttpcomm.dll",&module)){
                if(!ArchiveStreams() || !GetModuleHandleExW(0,L"ttpcomm.dll",&module))return;
            }
            if(!QueryArchiveGroups(module,api)){FreeLibrary(module);module=nullptr;}
        }
    };
    static const State state;return state.module?&state.api:nullptr;
}
bool QueryArchiveStreams(HMODULE module,TtpCommArchiveStreamApi& api) noexcept {
    api={};api.size=sizeof(api);
    const auto query=module ? reinterpret_cast<TtpCommQueryArchiveStreamFn>(GetProcAddress(module,"ttpcomm_query_archive_stream")) : nullptr;
    if(!query)return false;
    __try {
        return query(TTPCOMM_ARCHIVE_STREAM_ABI,&api) && api.size>=sizeof(api) &&
            api.abi_version==TTPCOMM_ARCHIVE_STREAM_ABI &&
            (api.capabilities&TTPCOMM_ARCHIVE_STREAM_REQUIRED)==TTPCOMM_ARCHIVE_STREAM_REQUIRED &&
            api.enumerate && api.open_member && api.status && api.clear_cache;
    } __except(EXCEPTION_EXECUTE_HANDLER) {api={};return false;}
}
const TtpCommArchiveStreamApi* ArchiveStreams() noexcept {
    struct State {
        TtpCommArchiveStreamApi api{};HMODULE module{};
        State() noexcept {
            wchar_t path[32768]{};const DWORD n=GetModuleFileNameW(nullptr,path,32768);
            if(!n || n>=32768)return;auto slash=wcsrchr(path,L'\\');
            if(!slash || size_t(slash-path)+13>=32768)return;
            wcscpy_s(slash+1,32768-size_t(slash+1-path),L"ttpcomm.dll");
            module=LoadLibraryW(path);
            if(module && !QueryArchiveStreams(module,api)){FreeLibrary(module);module=nullptr;}
        }
    };
    static const State state;return state.module?&state.api:nullptr;
}
bool QueryArchive(HMODULE module, TtpCommArchiveApi& api) noexcept {
    api={}; api.size=sizeof(api);
    const auto query=module ? reinterpret_cast<TtpCommQueryArchiveFn>(GetProcAddress(module,"ttpcomm_query_archive")) : nullptr;
    if(!query) return false;
    __try {
        return query(TTPCOMM_ARCHIVE_ABI,&api) && api.size>=sizeof(api) &&
            api.abi_version==TTPCOMM_ARCHIVE_ABI && (api.capabilities&TTPCOMM_ARCHIVE_REQUIRED)==TTPCOMM_ARCHIVE_REQUIRED &&
            api.enumerate && api.open_member;
    } __except(EXCEPTION_EXECUTE_HANDLER) { api={};return false; }
}
const TtpCommArchiveApi* Archive() noexcept {
    struct ArchiveState {
        TtpCommArchiveApi api{};
        HMODULE module{};
        ArchiveState() noexcept {
            wchar_t path[32768]{};
            const DWORD size=GetModuleFileNameW(nullptr,path,32768);
            if(!size || size>=32768) return;
            wchar_t* slash=wcsrchr(path,L'\\');
            if(!slash || size_t(slash-path)+13>=32768) return;
            wcscpy_s(slash+1,32768-size_t(slash+1-path),L"ttpcomm.dll");
            module=LoadLibraryW(path);
            if(module && !QueryArchive(module,api)) {FreeLibrary(module);module=nullptr;}
        }
    };
    static const ArchiveState state;
    return state.module ? &state.api : nullptr;
}
bool QueryRuntime(HMODULE module, TtpCommRuntimeApi& api) noexcept {
    api={};api.size=sizeof(api);
    const auto query=module ? reinterpret_cast<TtpCommQueryRuntimeFn>(GetProcAddress(module,"ttpcomm_query_runtime")) : nullptr;
    if(!query) return false;
    __try {
        return query(TTPCOMM_RUNTIME_ABI,TTPCOMM_RUNTIME_REQUIRED,&api) && api.size>=sizeof(api) &&
            api.abi_version==TTPCOMM_RUNTIME_ABI && (api.capabilities&TTPCOMM_RUNTIME_REQUIRED)==TTPCOMM_RUNTIME_REQUIRED &&
            api.inflate_buffer && api.crc32_buffer && api.pcm_decode && api.pcm_encode_half && api.pcm_gain_half &&
            api.quantizer_create && api.quantizer_destroy && api.quantizer_encode && api.quantizer_reset &&
            api.tag_terminator && api.tag_unsynchronize && api.tag_frame_start && api.tag_next_frame &&
            api.tag_picture && api.tag_decode && api.tag_decoded_destroy;
    } __except(EXCEPTION_EXECUTE_HANDLER) { api={};return false; }
}
const TtpCommRuntimeApi* Runtime() noexcept {
    struct RuntimeState {
        TtpCommRuntimeApi api{};
        HMODULE module{};
        RuntimeState() noexcept {
            wchar_t path[32768]{};
            const DWORD size=GetModuleFileNameW(nullptr,path,32768);
            if(!size || size>=32768) return;
            wchar_t* slash=wcsrchr(path,L'\\');
            if(!slash || size_t(slash-path)+13>=32768) return;
            wcscpy_s(slash+1,32768-size_t(slash+1-path),L"ttpcomm.dll");
            module=LoadLibraryW(path);
            if(module && !QueryRuntime(module,api)) {FreeLibrary(module);module=nullptr;}
        }
        // Intentionally no FreeLibrary at static destruction: thread-local and
        // static PCM objects can still run destructors. The OS releases the
        // single process-lifetime reference at exit.
    };
    static const RuntimeState state;
    return state.module ? &state.api : nullptr;
}
}
