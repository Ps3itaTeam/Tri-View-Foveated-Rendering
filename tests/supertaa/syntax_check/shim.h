// forced include of the syntax check with the cross compiler: what MSVC has and GCC does not
#pragma once
#ifdef __cplusplus
#define __try if (1)
#define __except(x) else
#define __finally
struct _EXCEPTION_POINTERS;
typedef void (*_se_translator_function)(unsigned int, struct _EXCEPTION_POINTERS*);
inline _se_translator_function _set_se_translator(_se_translator_function f) { return f; }
#endif
