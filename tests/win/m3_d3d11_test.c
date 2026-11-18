/*
 * D3D11 smoke test for the M3 chain: DXVK (ARM64EC) -> winevulkan ->
 * TITAN_VULKAN_LIB -> radv-xclipse. No swapchain: the window path is exercised
 * by the APK, this proves device creation, a render target and readback.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>

static int failures;

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static ID3D11Device *device;
static ID3D11DeviceContext *context;

static int create_device(void)
{
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got;
    HRESULT hr;

    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, levels, 1,
                           D3D11_SDK_VERSION, &device, &got, &context);
    if (FAILED(hr)) {
        printf("D3D11CreateDevice hr=%08lx\n", (unsigned long)hr);
        return 0;
    }
    printf("feature level %04x\n", (unsigned)got);
    return got == D3D_FEATURE_LEVEL_11_0;
}

/* Clears to `color` and reads the top-left pixel back through a staging copy. */
static int clear_and_readback(const FLOAT color[4], BYTE out[4])
{
    D3D11_TEXTURE2D_DESC desc = {0};
    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc = {0};
    D3D11_MAPPED_SUBRESOURCE map;
    ID3D11Texture2D *target = NULL, *staging = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    HRESULT hr;
    int ok = 0;

    desc.Width = 64;
    desc.Height = 64;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;

    hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &target);
    if (FAILED(hr)) goto done;

    rtv_desc.Format = desc.Format;
    rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    hr = ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)target, &rtv_desc, &rtv);
    if (FAILED(hr)) goto done;

    ID3D11DeviceContext_ClearRenderTargetView(context, rtv, color);
    ID3D11DeviceContext_Flush(context);

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging);
    if (FAILED(hr)) goto done;

    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging, (ID3D11Resource *)target);
    hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &map);
    if (FAILED(hr)) goto done;

    memcpy(out, map.pData, 4);
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
    ok = 1;

done:
    if (staging) ID3D11Texture2D_Release(staging);
    if (rtv) ID3D11RenderTargetView_Release(rtv);
    if (target) ID3D11Texture2D_Release(target);
    return ok;
}

int main(void)
{
    static const FLOAT red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    static const FLOAT green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    BYTE pixel[4];
    int i;

    if (!create_device()) {
        check(0, "D3D11CreateDevice(hardware, 11_0)");
        return 1;
    }
    check(1, "D3D11CreateDevice(hardware, 11_0)");

    check(clear_and_readback(red, pixel), "clear red and read back");
    check(pixel[0] > 250 && pixel[1] < 5 && pixel[2] < 5, "pixel is red");

    check(clear_and_readback(green, pixel), "clear green and read back");
    check(pixel[0] < 5 && pixel[1] > 250 && pixel[2] < 5, "pixel is green");

    for (i = 0; i < 60 && !failures; i++) {
        if (!clear_and_readback(red, pixel)) break;
    }
    check(i == 60, "60 clear/readback cycles");

    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    printf("m3_d3d11_test: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
