#pragma once
#include <vector>
#include <filesystem>
#include <stdexcept>
#include <source_location>
class ReplayRun
{
    ID3D12Device* device;
    ID3D12CommandQueue* queue;
    ID3D12CommandAllocator* alloc;
    ID3D12GraphicsCommandList* cmd;
    ID3D12Fence* fence;
    UINT64 serial = 1;
    ID3D12Resource* textures[6] {};
    ID3D12Resource* disable {};
    static void ensure(bool ok, const std::source_location at = std::source_location::current())
    {
        if (!ok)
        {
            printf("FAILED file=%s line=%u function=%s win32=%lu\n", at.file_name(), at.line(), at.function_name(),
                   GetLastError());
            throw std::runtime_error("replay operation failed");
        }
    }
    void flush()
    {
        ensure(SUCCEEDED(cmd->Close()));
        ID3D12CommandList* lists[] = { cmd };
        queue->ExecuteCommandLists(1, lists);
        ensure(SUCCEEDED(queue->Signal(fence, ++serial)));
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ensure(event != nullptr);
        ensure(SUCCEEDED(fence->SetEventOnCompletion(serial, event)));
        auto result = WaitForSingleObject(event, 30000);
        CloseHandle(event);
        ensure(result == WAIT_OBJECT_0);
        printf("DEVICE_STATUS %08lx\n", (unsigned long) device->GetDeviceRemovedReason());
        ensure(SUCCEEDED(device->GetDeviceRemovedReason()));
        ensure(SUCCEEDED(alloc->Reset()));
        ensure(SUCCEEDED(cmd->Reset(alloc, nullptr)));
    }
    static void transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES a,
                           D3D12_RESOURCE_STATES b)
    {
        D3D12_RESOURCE_BARRIER x {};
        x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        x.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b };
        cl->ResourceBarrier(1, &x);
    }
    ID3D12Resource* resource(D3D12_RESOURCE_DESC d, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state)
    {
        D3D12_HEAP_PROPERTIES h {};
        h.Type = type;
        h.CreationNodeMask = h.VisibleNodeMask = 1;
        ID3D12Resource* r = nullptr;
        ensure(SUCCEEDED(device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &d, state, nullptr,
                                                         __uuidof(ID3D12Resource), (void**) &r)));
        return r;
    }
    ID3D12Resource* buffer(UINT64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
    {
        D3D12_RESOURCE_DESC d {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = bytes;
        d.Height = d.DepthOrArraySize = d.MipLevels = d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags = flags;
        return resource(d, type,
                        type == D3D12_HEAP_TYPE_UPLOAD     ? D3D12_RESOURCE_STATE_GENERIC_READ
                        : type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
                                                           : D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    void transfer(ID3D12Resource* texture, const std::filesystem::path& file, bool upload)
    {
        auto d = texture->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
        UINT64 bytes {}, rowBytes {};
        UINT rows {};
        device->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &rowBytes, &bytes);
        auto b = buffer(bytes, upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_READBACK);
        void* data = nullptr;
        if (upload)
        {
            ensure(SUCCEEDED(b->Map(0, nullptr, &data)));
            FILE* f = _wfopen(file.c_str(), L"rb");
            ensure(f != nullptr);
            for (UINT y = 0; y < rows; y++)
                ensure(fread((char*) data + fp.Offset + y * fp.Footprint.RowPitch, 1, rowBytes, f) == rowBytes);
            ensure(fgetc(f) == EOF);
            fclose(f);
            b->Unmap(0, nullptr);
        }
        D3D12_TEXTURE_COPY_LOCATION tex {}, buf {};
        tex.pResource = texture;
        tex.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        buf.pResource = b;
        buf.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        buf.PlacedFootprint = fp;
        if (upload)
            cmd->CopyTextureRegion(&tex, 0, 0, 0, &buf, nullptr);
        else
            cmd->CopyTextureRegion(&buf, 0, 0, 0, &tex, nullptr);
        flush();
        if (!upload)
        {
            ensure(SUCCEEDED(b->Map(0, nullptr, &data)));
            FILE* f = _wfopen(file.c_str(), L"wb");
            ensure(f != nullptr);
            for (UINT y = 0; y < rows; y++)
                ensure(fwrite((char*) data + fp.Offset + y * fp.Footprint.RowPitch, 1, rowBytes, f) == rowBytes);
            fclose(f);
            b->Unmap(0, nullptr);
        }
        b->Release();
    }
    template <class T> static T read(FILE* f)
    {
        T v {};
        ensure(fread(&v, sizeof(v), 1, f) == 1);
        return v;
    }

  public:
    ReplayRun(ID3D12Device* d, ID3D12CommandQueue* q, ID3D12CommandAllocator* a, ID3D12GraphicsCommandList* c,
              ID3D12Fence* f)
        : device(d), queue(q), alloc(a), cmd(c), fence(f)
    {
    }
    int run(HMODULE module, void* handle, Params& p, const wchar_t* input, const wchar_t* packet, const wchar_t* output,
            const wchar_t* overrides = nullptr, const wchar_t* uiAlphaDirectory = nullptr)
    {
        std::filesystem::create_directories(output);
        // Creation command list has already been submitted, completed and closed.
        ensure(SUCCEEDED(alloc->Reset()));
        ensure(SUCCEEDED(cmd->Reset(alloc, nullptr)));
        DXGI_FORMAT formats[] = { DXGI_FORMAT_R8G8B8A8_UNORM,     DXGI_FORMAT_R8G8B8A8_TYPELESS,
                                  DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_TYPELESS,
                                  DXGI_FORMAT_R8G8B8A8_UNORM,     DXGI_FORMAT_R8G8B8A8_UNORM };
        for (unsigned i = 0; i < 6; i++)
        {
            D3D12_RESOURCE_DESC d {};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = (i == 2 || i == 3) ? renderWidth : outputWidth;
            d.Height = (i == 2 || i == 3) ? renderHeight : outputHeight;
            d.DepthOrArraySize = d.MipLevels = d.SampleDesc.Count = 1;
            d.Format = formats[i];
            d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            textures[i] = resource(d, D3D12_HEAP_TYPE_DEFAULT,
                                   i < 4 ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        disable = buffer(16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        ID3D12Resource* uiAlpha = nullptr;
        if (uiAlphaDirectory)
        {
            auto d = textures[0]->GetDesc();
            d.Format = DXGI_FORMAT_R8_UNORM;
            uiAlpha = resource(d, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
        }
        ID3D12Resource* phaseOutputs[3][2] { { textures[4], textures[5] }, {}, {} };
        for (unsigned i = 1; i < replayGenerated; i++)
            for (unsigned j = 0; j < 2; j++)
                phaseOutputs[i][j] = resource(textures[4 + j]->GetDesc(), D3D12_HEAP_TYPE_DEFAULT,
                                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        using Evaluate = unsigned (*)(ID3D12GraphicsCommandList*, const void*, const void*, void*);
        auto evaluate = (Evaluate) GetProcAddress(module, "NVSDK_NGX_D3D12_EvaluateFeature");
        ensure(evaluate != nullptr);
        FILE* f = _wfopen(packet, L"rb");
        ensure(f != nullptr);
        ensure(read<unsigned>(f) == 0x31524647);
        unsigned frames = read<unsigned>(f);
        ensure(frames > 0 && frames <= 4096 && frames == replayConfig.at("frames").get<unsigned>());
        const auto creationValues = p.values;
        for (unsigned frame = 0; frame < frames; frame++)
        {
            ensure(read<unsigned>(f) == frame);
            unsigned count = read<unsigned>(f);
            ensure(count < 1000);
            p.values = creationValues;
            std::map<std::string, std::vector<unsigned char>> payloads;
            for (unsigned k = 0; k < count; k++)
            {
                auto slot = read<unsigned>(f);
                auto result = read<unsigned>(f);
                auto bits = read<unsigned long long>(f);
                auto keySize = read<unsigned>(f);
                auto payloadKind = read<unsigned>(f);
                auto size = read<unsigned>(f);
                ensure(slot >= 8 && slot <= 15 && keySize < 256 && size <= 64);
                std::string key(keySize, '\0');
                ensure(fread(key.data(), 1, keySize, f) == keySize);
                std::vector<unsigned char> payload(size);
                ensure(fread(payload.data(), 1, size, f) == size);
                if (result != 1)
                {
                    p.values.erase(key);
                    continue;
                }
                Value v { bits, slot - 8 };
                if (size)
                {
                    ensure(payloadKind == 1 || payloadKind == 2);
                    payloads[key] = std::move(payload);
                    v.bits = (uintptr_t) payloads[key].data();
                }
                else if (slot <= 10 && bits)
                {
                    int i = -1;
                    const char* names[] = { "DLSSG.Backbuffer",         "DLSSG.HUDLess",   "DLSSG.MVecs", "DLSSG.Depth",
                                            "DLSSG.OutputInterpolated", "DLSSG.OutputReal" };
                    for (int j = 0; j < 6; j++)
                        if (key == names[j])
                            i = j;
                    if (i >= 0)
                        v.bits = (uintptr_t) textures[i];
                    else if (key == "DLSSG.OutputDisableInterpolation")
                        v.bits = (uintptr_t) disable;
                    else
                    {
                        printf("UNMAPPED_POINTER %s\n", key.c_str());
                        return 20;
                    }
                }
                p.values[key] = v;
            }
            phase = "replay_count_override";
            set<unsigned, 4>(&p, "DLSSG.MultiFrameCount", replayGenerated);
            set<unsigned, 4>(&p, "DLSSG.MultiFrameIndex", 1);
            if (frame == 0)
                set<unsigned, 4>(&p, "DLSSG.Reset", 1);
            const unsigned indices[] = { 0, 7, 4, 5 };
            for (unsigned i = 0; i < 4; i++)
            {
                if (i == 0 && frame)
                    transition(cmd, textures[i], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                               D3D12_RESOURCE_STATE_COPY_DEST);
                wchar_t name[64];
                swprintf(name, 64, L"frame-%02u-index-%u.bin", frame, indices[i]);
                auto source = std::filesystem::path(input) / name;
                // Optional MV/depth inputs only. All other recorded arguments stay identical.
                if (overrides && i >= 2)
                {
                    auto replacement = std::filesystem::path(overrides) / name;
                    if (std::filesystem::exists(replacement))
                    {
                        source = replacement;
                        printf("INPUT_OVERRIDE frame=%u index=%u\n", frame, indices[i]);
                    }
                }
                transfer(textures[i], source, true);
                if (i == 0)
                    transition(cmd, textures[i], D3D12_RESOURCE_STATE_COPY_DEST,
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            if (uiAlpha)
            {
                wchar_t name[64];
                swprintf(name, 64, L"frame-%02u-ui-alpha.bin", frame);
                transfer(uiAlpha, std::filesystem::path(uiAlphaDirectory) / name, true);
                phase = "configured_ui_alpha";
                set<void*, 0>(&p, "DLSSG.UIAlpha", uiAlpha);
                set<int, 3>(&p, "DLSSG.UIAlphaSubrectBaseX", 0);
                set<int, 3>(&p, "DLSSG.UIAlphaSubrectBaseY", 0);
                set<int, 3>(&p, "DLSSG.UIAlphaSubrectWidth", static_cast<int>(outputWidth));
                set<int, 3>(&p, "DLSSG.UIAlphaSubrectHeight", static_cast<int>(outputHeight));
            }
            for (unsigned index = 1; index <= replayGenerated; index++)
            {
                set<unsigned, 4>(&p, "DLSSG.MultiFrameIndex", index);
                auto interpolated = phaseOutputs[index - 1][0];
                p.values["DLSSG.OutputInterpolated"].bits = (uintptr_t) interpolated;
                p.values["DLSSG.OutputReal"].bits = (uintptr_t) phaseOutputs[index - 1][1];
                phase = "evaluate_replay";
                auto result = evaluate(cmd, handle, &p, nullptr);
                printf("REPLAY_EVAL frame=%u index=%u generated=%u result=%08x\n", frame, index, replayGenerated,
                       result);
                flush();
                if (result != 1)
                    return 21;
                transition(cmd, interpolated, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                wchar_t name[64];
                swprintf(name, 64, L"frame-%02u-generated-%02u.bin", frame, index * 100 / (replayGenerated + 1));
                transfer(interpolated, std::filesystem::path(output) / name, false);
                transition(cmd, interpolated, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                flush();
            }
        }
        ensure(fgetc(f) == EOF);
        fclose(f);
        for (auto r : textures)
            r->Release();
        for (unsigned i = 1; i < replayGenerated; i++)
            for (auto r : phaseOutputs[i])
                r->Release();
        disable->Release();
        if (uiAlpha)
            uiAlpha->Release();
        printf("REPLAY_DONE frames=%u generated_count=%u comparison_verified=0\n", frames, replayGenerated);
        return 0;
    }
};
