// Lists every constant-buffer variable of every dumped shader (from
// [Debug] DumpShaders=1), for the camera-matrix audit in docs/frame-analysis.md.
// Usage: cb_scan <DS3_DLSS_shaders dir> > cbuffers.tsv
// Columns: hash stage cbuffer register cb_bytes variable offset bytes type used
#include <windows.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <wrl/client.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: cb_scan <shader dump dir>\n");
    return 1;
  }
  int files = 0, failed = 0;
  for (const auto& entry : std::filesystem::directory_iterator(argv[1])) {
    const std::string name = entry.path().filename().string();  // hex hashes: ASCII
    if (name.size() < 8 || name.substr(name.size() - 4) != ".cso") continue;
    std::ifstream file(entry.path(), std::ios::binary);
    const std::vector<char> code((std::istreambuf_iterator<char>(file)), {});
    ComPtr<ID3D11ShaderReflection> refl;
    if (FAILED(D3DReflect(code.data(), code.size(), IID_PPV_ARGS(&refl)))) {
      ++failed;
      continue;
    }
    ++files;
    // "<HASH>.<stage>.cso"
    const std::string hash(name.begin(), name.begin() + name.find('.'));
    const std::string stage(name.begin() + name.find('.') + 1, name.end() - 4);
    D3D11_SHADER_DESC sd;
    refl->GetDesc(&sd);
    for (UINT b = 0; b < sd.BoundResources; ++b) {
      D3D11_SHADER_INPUT_BIND_DESC bind;
      refl->GetResourceBindingDesc(b, &bind);
      if (bind.Type != D3D_SIT_CBUFFER) continue;
      ID3D11ShaderReflectionConstantBuffer* cb = refl->GetConstantBufferByName(bind.Name);
      D3D11_SHADER_BUFFER_DESC cbd;
      if (FAILED(cb->GetDesc(&cbd))) continue;
      for (UINT v = 0; v < cbd.Variables; ++v) {
        ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(v);
        D3D11_SHADER_VARIABLE_DESC vd;
        var->GetDesc(&vd);
        D3D11_SHADER_TYPE_DESC td;
        var->GetType()->GetDesc(&td);
        std::printf("%s\t%s\t%s\tb%u\t%u\t%s\t%u\t%u\t%s\t%d\n", hash.c_str(), stage.c_str(),
                    cbd.Name, bind.BindPoint, cbd.Size, vd.Name, vd.StartOffset, vd.Size,
                    td.Name ? td.Name : "?", (vd.uFlags & D3D_SVF_USED) ? 1 : 0);
      }
    }
  }
  std::fprintf(stderr, "%d shaders read, %d failed\n", files, failed);
  return 0;
}
