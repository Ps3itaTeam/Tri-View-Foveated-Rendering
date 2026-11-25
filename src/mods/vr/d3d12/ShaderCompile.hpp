// ShaderCompile.hpp -- the one way the mod's shaders are compiled (defined in FoveaComposite.cpp).
//
// [BUILT-IN SHADERS fix145 23.11.2026] Every shader of the quad view, the supersampling TAA and the post passes goes
// through compile_shader: the code built into the DLL (ShaderTable.inl, made by tests/shaders: the same source text,
// entry point, target, flags and macros give the same key), else the code of this session, else the code kept in a file
// by an earlier session (ShaderFileCache.hpp), else D3DCompile. Before fix145 the first start compiled them all, with
// the picture stopped for minutes (153 s on a 7800X3D, log of fix121).
#pragma once

#include <d3dcommon.h>

#if defined(REVR_SHADER_GENERATOR)
#include <string>
#endif

namespace d3d12 {
HRESULT compile_shader(LPCVOID src, SIZE_T size, LPCSTR name, const D3D_SHADER_MACRO* defines, ID3DInclude* include, LPCSTR entry,
    LPCSTR target, UINT flags1, UINT flags2, ID3DBlob** code, ID3DBlob** errors);


#if defined(REVR_SHADER_GENERATOR)
// the generator (tests/shaders): every shader compiled, with its key
void shader_generator_record(const std::string& key, ID3DBlob* code);
// the eye pass's texts made after its root signature (FoveaComposite.cpp)
void eye_compose_generator_texts();
#endif
} // namespace d3d12
