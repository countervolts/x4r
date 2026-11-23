// SPDX-License-Identifier: GPL-3.0-only
// dxil-sign: validates and signs DXIL containers in place with DXC's validator (dxil.dll, e.g. the Windows SDK's), so native
// D3D12 accepts kernels compiled with -Vd on Linux (vkd3d-proton does not check signatures; Windows' D3D12 does).
// usage: dxil-sign <dxil.dll> <file.dxil>...    prints "ok" or the validator's messages per file; exit 1 if any failed
#include <windows.h>
#include <dxcapi.h>
#include <cstdio>
#include <fstream>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: dxil-sign <dxil.dll> <file.dxil>...\n"); return 2; }
    HMODULE dll = LoadLibraryA(argv[1]);
    auto create = dll ? reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(dll, "DxcCreateInstance")) : nullptr;
    IDxcValidator* val = nullptr;
    IDxcLibrary* lib = nullptr;
    if (!create || FAILED(create(CLSID_DxcValidator, IID_PPV_ARGS(&val)))) { std::fprintf(stderr, "no validator in %s\n", argv[1]); return 2; }
    HMODULE comp = LoadLibraryA("dxcompiler.dll");   // IDxcLibrary for blobs (dxcompiler next to dxil.dll, or on PATH)
    auto createc = comp ? reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(comp, "DxcCreateInstance")) : nullptr;
    if (!createc || FAILED(createc(CLSID_DxcLibrary, IID_PPV_ARGS(&lib)))) { std::fprintf(stderr, "no dxcompiler.dll\n"); return 2; }
    int failed = 0;
    for (int i = 2; i < argc; ++i) {
        std::ifstream f(argv[i], std::ios::binary);
        std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        f.close();
        IDxcBlobEncoding* blob = nullptr;
        lib->CreateBlobWithEncodingOnHeapCopy(data.data(), UINT32(data.size()), CP_ACP, &blob);
        IDxcOperationResult* res = nullptr;
        HRESULT status = E_FAIL;
        if (SUCCEEDED(val->Validate(blob, DxcValidatorFlags_InPlaceEdit, &res)) && res) res->GetStatus(&status);
        if (SUCCEEDED(status)) {
            // in place: the blob now carries the validator's hash
            std::ofstream(argv[i], std::ios::binary).write(static_cast<const char*>(blob->GetBufferPointer()), blob->GetBufferSize());
            std::printf("%s: ok\n", argv[i]);
        } else {
            ++failed;
            IDxcBlobEncoding* err = nullptr;
            if (res) res->GetErrorBuffer(&err);
            std::printf("%s: FAILED\n%.*s\n", argv[i], err ? int(err->GetBufferSize()) : 0, err ? static_cast<const char*>(err->GetBufferPointer()) : "");
            if (err) err->Release();
        }
        if (res) res->Release();
        blob->Release();
    }
    return failed ? 1 : 0;
}
