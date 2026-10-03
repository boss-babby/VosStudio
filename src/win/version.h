// The one place the version lives: the About/crash texts, the User-Agent and the .exe version resource all read it.
#pragma once
#define VOS_VERSION_STR "1.19.0"
#define VOS_VERSION_COMMA 1,19,0,0
#define VOS_WIDEN2(x) L##x
#define VOS_WIDEN(x) VOS_WIDEN2(x)
#define VOS_VERSION_WSTR VOS_WIDEN(VOS_VERSION_STR)
#ifdef __cplusplus
constexpr const char* kAppVersion = VOS_VERSION_STR;
#endif
