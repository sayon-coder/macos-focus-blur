#pragma once

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace focusglass
{
    class GpuCompositor
    {
    public:
        GpuCompositor(ID3D11Device* device, ID3D11DeviceContext* context,
            HWND targetWindow, int outputWidth, int outputHeight)
            : m_outputWidth(outputWidth), m_outputHeight(outputHeight)
        {
            if (!device || !context || !targetWindow || outputWidth <= 0 || outputHeight <= 0)
                throw std::invalid_argument("GPU compositor received invalid initialization parameters.");

            m_device.copy_from(device);
            m_context.copy_from(context);

            CreateShaders();
            CreateSampler();
            CreateConstantBuffers();
            CreateCompositionSwapChain(targetWindow);
            CreateCompositionVisual(targetWindow);
        }

        GpuCompositor(const GpuCompositor&) = delete;
        GpuCompositor& operator=(const GpuCompositor&) = delete;

        void Render(ID3D11Texture2D* capturedFrame, const RECT& monitorBounds,
            const std::vector<RECT>& clearRegions, int blurLevel)
        {
            if (!capturedFrame || clearRegions.empty())
                return;

            EnsureFrameResources(capturedFrame);
            m_context->CopyResource(m_sourceCopy.get(), capturedFrame);

            BlurConstants blur{};
            const int level = std::clamp(blurLevel, 1, 10);
            blur.sigma = 1.2f + static_cast<float>(level - 1) * 0.64f;

            blur.texelStep[0] = 1.0f / static_cast<float>(m_blurWidth);
            blur.texelStep[1] = 0.0f;
            m_context->UpdateSubresource(m_blurConstants.get(), 0, nullptr, &blur, 0, 0);
            DrawPass(m_downsamplePixelShader.get(), m_sourceSrv.get(), m_downsampleTarget.rtv.get(),
                m_blurWidth, m_blurHeight, nullptr, true);

            // Horizontal pass: downsampled source -> horizontal intermediate.
            DrawPass(m_blurPixelShader.get(), m_downsampleTarget.srv.get(), m_horizontalTarget.rtv.get(),
                m_blurWidth, m_blurHeight, m_blurConstants.get(), false);

            // Vertical pass: horizontal intermediate -> final blurred texture.
            blur.texelStep[0] = 0.0f;
            blur.texelStep[1] = 1.0f / static_cast<float>(m_blurHeight);
            m_context->UpdateSubresource(m_blurConstants.get(), 0, nullptr, &blur, 0, 0);
            DrawPass(m_blurPixelShader.get(), m_horizontalTarget.srv.get(), m_blurTarget.rtv.get(),
                m_blurWidth, m_blurHeight, m_blurConstants.get(), false);

            CompositeConstants composite{};
            composite.outputParams[0] = static_cast<float>(m_outputWidth);
            composite.outputParams[1] = static_cast<float>(m_outputHeight);
            composite.outputParams[2] = 0.93f; // Keep the restrained backdrop brightness from v0.1.x.
            composite.outputParams[3] = 10.0f;  // Rounded foreground cutout corner radius.
            composite.foregroundRect = ToLocalRect(clearRegions.front(), monitorBounds);

            // shadowParams: vertical offset, Gaussian sigma, opacity, reserved.
            composite.shadowParams[0] = 6.0f;
            composite.shadowParams[1] = 16.0f;
            composite.shadowParams[2] = 0.27f;
            composite.shadowParams[3] = 0.0f;

            const size_t availableClearRects = clearRegions.size() > 1 ? clearRegions.size() - 1 : 0;
            composite.clearCount = static_cast<UINT>(std::min<size_t>(availableClearRects, static_cast<size_t>(kMaxClearRects)));
            for (UINT i = 0; i < composite.clearCount; ++i)
                composite.clearRects[i] = ToLocalRect(clearRegions[i + 1], monitorBounds);

            m_context->UpdateSubresource(m_compositeConstants.get(), 0, nullptr, &composite, 0, 0);

            // Flip-model swap chains rotate buffers on Present. Render to the current index.
            const UINT currentBackBufferIndex = m_swapChain3->GetCurrentBackBufferIndex();
            winrt::com_ptr<ID3D11Texture2D> currentBackBuffer;
            winrt::com_ptr<ID3D11RenderTargetView> currentBackBufferRtv;
            winrt::check_hresult(m_swapChain->GetBuffer(
                currentBackBufferIndex, __uuidof(ID3D11Texture2D), currentBackBuffer.put_void()));
            winrt::check_hresult(m_device->CreateRenderTargetView(
                currentBackBuffer.get(), nullptr, currentBackBufferRtv.put()));
            ID3D11RenderTargetView* target = currentBackBufferRtv.get();
            m_context->OMSetRenderTargets(1, &target, nullptr);
            D3D11_VIEWPORT viewport{};
            viewport.Width = static_cast<float>(m_outputWidth);
            viewport.Height = static_cast<float>(m_outputHeight);
            viewport.MinDepth = 0.0f;
            viewport.MaxDepth = 1.0f;
            m_context->RSSetViewports(1, &viewport);
            m_context->IASetInputLayout(nullptr);
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_context->VSSetShader(m_vertexShader.get(), nullptr, 0);
            m_context->PSSetShader(m_compositePixelShader.get(), nullptr, 0);

            ID3D11ShaderResourceView* sourceView = m_blurTarget.srv.get();
            m_context->PSSetShaderResources(0, 1, &sourceView);
            ID3D11SamplerState* sampler = m_sampler.get();
            m_context->PSSetSamplers(0, 1, &sampler);
            ID3D11Buffer* constants = m_compositeConstants.get();
            m_context->PSSetConstantBuffers(0, 1, &constants);
            m_context->Draw(3, 0);

            ID3D11ShaderResourceView* nullView = nullptr;
            m_context->PSSetShaderResources(0, 1, &nullView);
            m_context->OMSetRenderTargets(0, nullptr, nullptr);

            const HRESULT presentResult = m_swapChain->Present(1, 0);
            if (FAILED(presentResult))
                winrt::check_hresult(presentResult);
        }

    private:
        static constexpr UINT kMaxClearRects = 32;

        struct RenderTarget
        {
            winrt::com_ptr<ID3D11Texture2D> texture;
            winrt::com_ptr<ID3D11ShaderResourceView> srv;
            winrt::com_ptr<ID3D11RenderTargetView> rtv;
        };

        struct BlurConstants
        {
            float texelStep[2]{};
            float sigma{};
            float padding{};
        };

        struct CompositeConstants
        {
            float outputParams[4]{};
            std::array<float, 4> foregroundRect{};
            float shadowParams[4]{};
            UINT clearCount{};
            float padding[3]{};
            std::array<std::array<float, 4>, kMaxClearRects> clearRects{};
        };

        static std::array<float, 4> ToLocalRect(const RECT& rect, const RECT& monitor)
        {
            return {
                static_cast<float>(rect.left - monitor.left),
                static_cast<float>(rect.top - monitor.top),
                static_cast<float>(rect.right - monitor.left),
                static_cast<float>(rect.bottom - monitor.top)
            };
        }

        static constexpr const char* ShaderSource()
        {
            return R"(
Texture2D SourceTexture : register(t0);
SamplerState LinearSampler : register(s0);

struct VertexOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VertexOutput VSMain(uint vertexId : SV_VertexID)
{
    VertexOutput output;
    if (vertexId == 0)
    {
        output.position = float4(-1.0,  1.0, 0.0, 1.0);
        output.uv = float2(0.0, 0.0);
    }
    else if (vertexId == 1)
    {
        output.position = float4( 3.0,  1.0, 0.0, 1.0);
        output.uv = float2(2.0, 0.0);
    }
    else
    {
        output.position = float4(-1.0, -3.0, 0.0, 1.0);
        output.uv = float2(0.0, 2.0);
    }
    return output;
}

float4 PSDownsample(VertexOutput input) : SV_TARGET
{
    uint sourceWidth, sourceHeight;
    SourceTexture.GetDimensions(sourceWidth, sourceHeight);
    float2 texel = 1.0 / float2(sourceWidth, sourceHeight);
    float4 accumulated = 0.0;

    // A 3x3 prefilter reduces shimmer before downsampling the monitor image.
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
            accumulated += SourceTexture.SampleLevel(LinearSampler, input.uv + texel * float2(x, y), 0);
    }

    return accumulated / 9.0;
}

cbuffer BlurParams : register(b0)
{
    float2 texelStep;
    float sigma;
    float blurPadding;
};

float4 PSBlur(VertexOutput input) : SV_TARGET
{
    float4 accumulated = 0.0;
    float totalWeight = 0.0;
    float safeSigma = max(sigma, 0.5);

    [unroll]
    for (int tap = -12; tap <= 12; ++tap)
    {
        float sampleWeight = exp(-0.5 * (tap * tap) / (safeSigma * safeSigma));
        float2 sampleUv = input.uv + texelStep * tap;
        accumulated += SourceTexture.SampleLevel(LinearSampler, sampleUv, 0) * sampleWeight;
        totalWeight += sampleWeight;
    }

    return accumulated / max(totalWeight, 0.0001);
}

cbuffer CompositeParams : register(b0)
{
    float4 outputParams;       // width, height, brightness, foreground corner radius
    float4 foregroundRect;     // left, top, right, bottom in monitor-local pixels
    float4 shadowParams;       // vertical offset, sigma, opacity, reserved
    uint clearCount;
    float3 compositePadding;
    float4 clearRects[32];
};

float RoundedRectDistance(float2 samplePosition, float4 boundsRect, float radius)
{
    float2 center = (boundsRect.xy + boundsRect.zw) * 0.5;
    float2 halfSize = max((boundsRect.zw - boundsRect.xy) * 0.5, float2(0.0, 0.0));
    float2 q = abs(samplePosition - center) - (halfSize - radius);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

float4 PSComposite(VertexOutput input) : SV_TARGET
{
    float2 pixel = input.uv * outputParams.xy;

    // Transparent cutout for the active window. The real window remains live and sharp.
    float foregroundDistance = RoundedRectDistance(pixel, foregroundRect, outputParams.w);
    if (foregroundDistance <= 0.0)
        return float4(0.0, 0.0, 0.0, 0.0);

    // Preserve only owned popups, taskbars and the configured top menu-bar safe area.
    [loop]
    for (uint index = 0; index < clearCount; ++index)
    {
        float4 rect = clearRects[index];
        if (pixel.x >= rect.x && pixel.x < rect.z && pixel.y >= rect.y && pixel.y < rect.w)
            return float4(0.0, 0.0, 0.0, 0.0);
    }

    float4 color = SourceTexture.SampleLevel(LinearSampler, input.uv, 0);

    // A medium soft shadow sits over the blurred backdrop and just outside the foreground.
    float4 shadowRect = foregroundRect + float4(0.0, shadowParams.x, 0.0, shadowParams.x);
    float shadowDistance = max(0.0, RoundedRectDistance(pixel, shadowRect, outputParams.w));
    float shadow = shadowParams.z * exp(
        -0.5 * (shadowDistance * shadowDistance) /
        max(1.0, shadowParams.y * shadowParams.y));

    color.rgb *= outputParams.z * (1.0 - shadow);
    return float4(color.rgb, 1.0);
}
)";
        }

        void CompileShader(const char* entryPoint, const char* profile,
            winrt::com_ptr<ID3DBlob>& bytecode)
        {
            winrt::com_ptr<ID3DBlob> diagnostics;
            const char* source = ShaderSource();
            const HRESULT hr = D3DCompile(source, std::strlen(source), "FocusGlassGpu.hlsl",
                nullptr, nullptr, entryPoint, profile, D3DCOMPILE_ENABLE_STRICTNESS,
                0, bytecode.put(), diagnostics.put());

            if (FAILED(hr))
            {
                std::string message = std::string("Could not compile GPU shader ") + entryPoint + ".";
                if (diagnostics && diagnostics->GetBufferPointer())
                {
                    message += " ";
                    message.append(static_cast<const char*>(diagnostics->GetBufferPointer()),
                        diagnostics->GetBufferSize());
                }
                throw std::runtime_error(message);
            }
        }

        void CreateShaders()
        {
            winrt::com_ptr<ID3DBlob> bytecode;
            CompileShader("VSMain", "vs_4_0", bytecode);
            winrt::check_hresult(m_device->CreateVertexShader(bytecode->GetBufferPointer(),
                bytecode->GetBufferSize(), nullptr, m_vertexShader.put()));

            bytecode = nullptr;
            CompileShader("PSDownsample", "ps_4_0", bytecode);
            winrt::check_hresult(m_device->CreatePixelShader(bytecode->GetBufferPointer(),
                bytecode->GetBufferSize(), nullptr, m_downsamplePixelShader.put()));

            bytecode = nullptr;
            CompileShader("PSBlur", "ps_4_0", bytecode);
            winrt::check_hresult(m_device->CreatePixelShader(bytecode->GetBufferPointer(),
                bytecode->GetBufferSize(), nullptr, m_blurPixelShader.put()));

            bytecode = nullptr;
            CompileShader("PSComposite", "ps_4_0", bytecode);
            winrt::check_hresult(m_device->CreatePixelShader(bytecode->GetBufferPointer(),
                bytecode->GetBufferSize(), nullptr, m_compositePixelShader.put()));
        }

        void CreateSampler()
        {
            D3D11_SAMPLER_DESC desc{};
            desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
            desc.MinLOD = 0.0f;
            desc.MaxLOD = D3D11_FLOAT32_MAX;
            winrt::check_hresult(m_device->CreateSamplerState(&desc, m_sampler.put()));
        }

        void CreateConstantBuffers()
        {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = sizeof(BlurConstants);
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            winrt::check_hresult(m_device->CreateBuffer(&desc, nullptr, m_blurConstants.put()));

            desc.ByteWidth = sizeof(CompositeConstants);
            winrt::check_hresult(m_device->CreateBuffer(&desc, nullptr, m_compositeConstants.put()));
        }

        void CreateCompositionSwapChain(HWND targetWindow)
        {
            winrt::com_ptr<IDXGIDevice> dxgiDevice;
            winrt::check_hresult(m_device->QueryInterface(__uuidof(IDXGIDevice), dxgiDevice.put_void()));

            winrt::com_ptr<IDXGIAdapter> adapter;
            winrt::check_hresult(dxgiDevice->GetAdapter(adapter.put()));

            winrt::com_ptr<IDXGIFactory2> factory;
            winrt::check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void()));

            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = static_cast<UINT>(m_outputWidth);
            desc.Height = static_cast<UINT>(m_outputHeight);
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.Stereo = FALSE;
            desc.SampleDesc.Count = 1;
            desc.SampleDesc.Quality = 0;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2;
            desc.Scaling = DXGI_SCALING_STRETCH;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
            desc.Flags = 0;

            winrt::check_hresult(factory->CreateSwapChainForComposition(
                m_device.get(), &desc, nullptr, m_swapChain.put()));

            // IDXGISwapChain3 exposes the current writable back-buffer index on Windows 10+.
            winrt::check_hresult(m_swapChain->QueryInterface(__uuidof(IDXGISwapChain3),
                m_swapChain3.put_void()));

            winrt::com_ptr<IDXGISwapChain2> swapChain2;
            if (SUCCEEDED(m_swapChain->QueryInterface(__uuidof(IDXGISwapChain2), swapChain2.put_void())))
                winrt::check_hresult(swapChain2->SetMaximumFrameLatency(1));

            winrt::com_ptr<ID3D11Texture2D> backBuffer;
            winrt::check_hresult(m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), backBuffer.put_void()));
            winrt::check_hresult(m_device->CreateRenderTargetView(backBuffer.get(), nullptr, m_swapChainTarget.put()));
            const float transparent[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            m_context->ClearRenderTargetView(m_swapChainTarget.get(), transparent);
            winrt::check_hresult(m_swapChain->Present(1, 0));
            // Release the initial RTV; Render() reacquires the active flip-model back buffer.
            m_swapChainTarget = nullptr;

            (void)targetWindow;
        }

        void CreateCompositionVisual(HWND targetWindow)
        {
            winrt::com_ptr<IDXGIDevice> dxgiDevice;
            winrt::check_hresult(m_device->QueryInterface(__uuidof(IDXGIDevice), dxgiDevice.put_void()));
            winrt::check_hresult(DCompositionCreateDevice(dxgiDevice.get(),
                __uuidof(IDCompositionDevice), m_compositionDevice.put_void()));
            winrt::check_hresult(m_compositionDevice->CreateTargetForHwnd(
                targetWindow, TRUE, m_compositionTarget.put()));
            winrt::check_hresult(m_compositionDevice->CreateVisual(m_visual.put()));
            winrt::check_hresult(m_visual->SetContent(m_swapChain.get()));
            winrt::check_hresult(m_compositionTarget->SetRoot(m_visual.get()));
            winrt::check_hresult(m_compositionDevice->Commit());
        }

        void CreateRenderTarget(UINT width, UINT height, RenderTarget& target)
        {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            winrt::check_hresult(m_device->CreateTexture2D(&desc, nullptr, target.texture.put()));
            winrt::check_hresult(m_device->CreateShaderResourceView(target.texture.get(), nullptr, target.srv.put()));
            winrt::check_hresult(m_device->CreateRenderTargetView(target.texture.get(), nullptr, target.rtv.put()));
        }

        void EnsureFrameResources(ID3D11Texture2D* capturedFrame)
        {
            D3D11_TEXTURE2D_DESC sourceDesc{};
            capturedFrame->GetDesc(&sourceDesc);
            if (sourceDesc.Width == 0 || sourceDesc.Height == 0 ||
                sourceDesc.ArraySize != 1 || sourceDesc.SampleDesc.Count != 1)
                throw std::runtime_error("Captured desktop texture has an unsupported size or sample layout.");

            const bool sourceChanged = !m_sourceCopy ||
                m_sourceWidth != sourceDesc.Width ||
                m_sourceHeight != sourceDesc.Height ||
                m_sourceFormat != sourceDesc.Format;

            if (sourceChanged)
            {
                m_sourceCopy = nullptr;
                m_sourceSrv = nullptr;

                D3D11_TEXTURE2D_DESC copyDesc = sourceDesc;
                copyDesc.MipLevels = 1;
                copyDesc.ArraySize = 1;
                copyDesc.Usage = D3D11_USAGE_DEFAULT;
                copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                copyDesc.CPUAccessFlags = 0;
                copyDesc.MiscFlags = 0;
                copyDesc.SampleDesc.Count = 1;
                copyDesc.SampleDesc.Quality = 0;
                winrt::check_hresult(m_device->CreateTexture2D(&copyDesc, nullptr, m_sourceCopy.put()));

                D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                srvDesc.Format = sourceDesc.Format;
                srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MostDetailedMip = 0;
                srvDesc.Texture2D.MipLevels = 1;
                winrt::check_hresult(m_device->CreateShaderResourceView(m_sourceCopy.get(), &srvDesc, m_sourceSrv.put()));

                m_sourceWidth = sourceDesc.Width;
                m_sourceHeight = sourceDesc.Height;
                m_sourceFormat = sourceDesc.Format;

                const double scale = std::min(1.0, 640.0 / static_cast<double>(m_sourceWidth));
                m_blurWidth = std::max(1u, static_cast<UINT>(m_sourceWidth * scale));
                m_blurHeight = std::max(1u, static_cast<UINT>(m_sourceHeight * scale));

                m_downsampleTarget = {};
                m_horizontalTarget = {};
                m_blurTarget = {};
                CreateRenderTarget(m_blurWidth, m_blurHeight, m_downsampleTarget);
                CreateRenderTarget(m_blurWidth, m_blurHeight, m_horizontalTarget);
                CreateRenderTarget(m_blurWidth, m_blurHeight, m_blurTarget);
            }
        }

        void DrawPass(ID3D11PixelShader* pixelShader, ID3D11ShaderResourceView* source,
            ID3D11RenderTargetView* target, UINT width, UINT height,
            ID3D11Buffer* constants, bool useDownsampleShader)
        {
            ID3D11ShaderResourceView* nullView = nullptr;
            m_context->PSSetShaderResources(0, 1, &nullView);

            ID3D11RenderTargetView* targetView = target;
            m_context->OMSetRenderTargets(1, &targetView, nullptr);
            D3D11_VIEWPORT viewport{};
            viewport.Width = static_cast<float>(width);
            viewport.Height = static_cast<float>(height);
            viewport.MinDepth = 0.0f;
            viewport.MaxDepth = 1.0f;
            m_context->RSSetViewports(1, &viewport);
            m_context->IASetInputLayout(nullptr);
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_context->VSSetShader(m_vertexShader.get(), nullptr, 0);
            m_context->PSSetShader(useDownsampleShader ? m_downsamplePixelShader.get() : pixelShader, nullptr, 0);

            ID3D11ShaderResourceView* sourceView = source;
            m_context->PSSetShaderResources(0, 1, &sourceView);
            ID3D11SamplerState* sampler = m_sampler.get();
            m_context->PSSetSamplers(0, 1, &sampler);
            if (constants)
            {
                ID3D11Buffer* buffer = constants;
                m_context->PSSetConstantBuffers(0, 1, &buffer);
            }
            m_context->Draw(3, 0);

            m_context->PSSetShaderResources(0, 1, &nullView);
            m_context->OMSetRenderTargets(0, nullptr, nullptr);
        }

        winrt::com_ptr<ID3D11Device> m_device;
        winrt::com_ptr<ID3D11DeviceContext> m_context;
        int m_outputWidth{};
        int m_outputHeight{};

        winrt::com_ptr<ID3D11VertexShader> m_vertexShader;
        winrt::com_ptr<ID3D11PixelShader> m_downsamplePixelShader;
        winrt::com_ptr<ID3D11PixelShader> m_blurPixelShader;
        winrt::com_ptr<ID3D11PixelShader> m_compositePixelShader;
        winrt::com_ptr<ID3D11SamplerState> m_sampler;
        winrt::com_ptr<ID3D11Buffer> m_blurConstants;
        winrt::com_ptr<ID3D11Buffer> m_compositeConstants;

        winrt::com_ptr<IDXGISwapChain1> m_swapChain;
        winrt::com_ptr<IDXGISwapChain3> m_swapChain3;
        winrt::com_ptr<ID3D11RenderTargetView> m_swapChainTarget;
        winrt::com_ptr<IDCompositionDevice> m_compositionDevice;
        winrt::com_ptr<IDCompositionTarget> m_compositionTarget;
        winrt::com_ptr<IDCompositionVisual> m_visual;

        winrt::com_ptr<ID3D11Texture2D> m_sourceCopy;
        winrt::com_ptr<ID3D11ShaderResourceView> m_sourceSrv;
        UINT m_sourceWidth{};
        UINT m_sourceHeight{};
        DXGI_FORMAT m_sourceFormat{ DXGI_FORMAT_UNKNOWN };
        UINT m_blurWidth{};
        UINT m_blurHeight{};
        RenderTarget m_downsampleTarget;
        RenderTarget m_horizontalTarget;
        RenderTarget m_blurTarget;
    };
}
