# A distinct Release build tree is required: current debug CRTs cannot run on XP.
if(NOT MSVC)
  message(FATAL_ERROR "TTPLAYER_LEGACY_WINDOWS requires MSVC and the Win32 generator")
endif()
set(CMAKE_CONFIGURATION_TYPES "Release" CACHE STRING "Legacy distribution configuration" FORCE)
set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Legacy distribution configuration" FORCE)
include("${CMAKE_CURRENT_LIST_DIR}/xp_runtime.cmake")
add_compile_definitions(TTPLAYER_LEGACY_WINDOWS=1)
# 兼容版把视频层静态编进主 EXE：所有 TU 都必须把 TTPLAYER_VIDEO_API 展开为
# 空，否则头文件里的 __declspec(dllimport) 会要求一个并不存在的 DLL。
add_compile_definitions(TTPLAYER_VIDEO_STATIC=1)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/legacy-licenses")
configure_file("${ttplayer_yy_thunks_SOURCE_DIR}/LICENSE"
  "${CMAKE_CURRENT_BINARY_DIR}/legacy-licenses/YY-Thunks-LICENSE.txt" COPYONLY)
