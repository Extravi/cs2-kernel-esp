#include <iostream>
#include <Windows.h>
#include <thread>
#include <TlHelp32.h>
#include <vector>
#include <Psapi.h>
#include <string>
#include <tchar.h>
#include <cstdio>

//imgui stuff
#include <dwmapi.h>
#include <d3d11.h>
#include <windowsx.h>
#include "./ImGui/imgui.h"
#include "./ImGui/imgui_impl_dx11.h"
#include "./ImGui/imgui_impl_win32.h"
#include "vector.h"
#include "render.h"

// int width, int height
int screenWidth = GetSystemMetrics(SM_CXSCREEN);
int screenHeight = GetSystemMetrics(SM_CYSCREEN);

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, w_param, l_param)) {
        return 0L;
    }

    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0L;
    }

    switch (message)
    {
    case WM_NCHITTEST:
    {
        const LONG borderWidth = GetSystemMetrics(SM_CXSIZEFRAME);
        const LONG titleBarHeight = GetSystemMetrics(SM_CYCAPTION);
        POINT cursorPos = { GET_X_LPARAM(w_param), GET_Y_LPARAM(l_param) };
        RECT windowRect;
        GetWindowRect(window, &windowRect);

        if (cursorPos.y >= windowRect.top && cursorPos.y < windowRect.top + titleBarHeight)
            return HTCAPTION;
        
        break;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(window, message, w_param, l_param);
}

int GetProcessIdByName(const wchar_t* processName) {
    PROCESSENTRY32 entry;
    entry.dwSize = sizeof(PROCESSENTRY32);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);

    if (Process32First(snapshot, &entry) == TRUE) {
        while (Process32Next(snapshot, &entry) == TRUE) {
            if (_wcsicmp(entry.szExeFile, processName) == 0) {
                CloseHandle(snapshot);
                return entry.th32ProcessID;
            }
        }
    }
    CloseHandle(snapshot);
    return 0;
}

DWORD_PTR GetModuleBaseAddress(DWORD dwPid, const wchar_t* moduleName) {
    MODULEENTRY32 moduleEntry = { sizeof(MODULEENTRY32) };
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, dwPid);
    if (hSnapshot == INVALID_HANDLE_VALUE)
        return 0;

    if (Module32First(hSnapshot, &moduleEntry)) {
        do {
            if (!_wcsicmp(moduleEntry.szModule, moduleName)) {
                CloseHandle(hSnapshot);
                return (DWORD_PTR)moduleEntry.modBaseAddr;
            }
        } while (Module32Next(hSnapshot, &moduleEntry));
    }
    CloseHandle(hSnapshot);
    return 0;
}

namespace driver {
    namespace codes {
        // used to setup the driver
        constexpr ULONG attach =
            CTL_CODE(FILE_DEVICE_UNKNOWN, 0x696, METHOD_BUFFERED, FILE_SPECIAL_ACCESS);

        // read process memory from um application 
        constexpr ULONG read =
            CTL_CODE(FILE_DEVICE_UNKNOWN, 0x697, METHOD_BUFFERED, FILE_SPECIAL_ACCESS);

        // read process memory from um application 
        constexpr ULONG write =
            CTL_CODE(FILE_DEVICE_UNKNOWN, 0x698, METHOD_BUFFERED, FILE_SPECIAL_ACCESS);
    } // namespace codes

    // shared between user mode and kernel mode
    struct Request {
        HANDLE process_id;

        PVOID target;
        PVOID buffer;

        SIZE_T size;
        SIZE_T return_size;
    };

    bool attach_to_process(HANDLE driver_handle, const DWORD pid) {
        Request r;
        r.process_id = reinterpret_cast<HANDLE>(pid);

        return DeviceIoControl(driver_handle, codes::attach, &r, sizeof(r), &r, sizeof(r), nullptr, nullptr);
    }

    template <class T>
    T read_memory(HANDLE driver_handle, const std::uintptr_t addr) {
        T temp = {};

        Request r;
        r.target = reinterpret_cast<PVOID>(addr);
        r.buffer = &temp;
        r.size = sizeof(T);

        DeviceIoControl(driver_handle, codes::read, &r, sizeof(r), &r, sizeof(r), nullptr, nullptr);

        return temp;
    }

    template <class T>
    void write_memory(HANDLE driver_handle, const std::uintptr_t addr, const T& value) {
        Request r;
        r.target = reinterpret_cast<PVOID>(addr);
        r.buffer = (PVOID)&value;
        r.size = sizeof(T);

        DeviceIoControl(driver_handle, codes::write, &r, sizeof(r), &r, sizeof(r), nullptr, nullptr);
    }
} // namespace driver

INT APIENTRY WinMain(HINSTANCE instance, HINSTANCE, PSTR, INT cmd_show) {
    const wchar_t* targetModuleName = L"cs2.exe";

    int pid = GetProcessIdByName(targetModuleName);
    if (pid == 0) {
        MessageBoxW(NULL, L"Process not found", L"Error", MB_OK | MB_ICONERROR);
        return 1; // failure
    }

    // create driver handle
    const HANDLE driver = CreateFile(L"\\\\.\\km", GENERIC_READ, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (driver == INVALID_HANDLE_VALUE) {
        MessageBoxW(NULL, L"Failed to create our driver handle", L"Error", MB_OK | MB_ICONERROR);
        return 1; // failure
    }

    // offsets

    // offsets.hpp
    constexpr std::ptrdiff_t dwEntityList = 0x18C1DB8;
    constexpr std::ptrdiff_t dwViewMatrix = 0x19231B0;
    constexpr std::ptrdiff_t dwLocalPlayerPawn = 0x17361E8;

    // client.dll.hpp
    constexpr std::ptrdiff_t m_vOldOrigin = 0x127C; // Vector
    constexpr std::ptrdiff_t m_iTeamNum = 0x3CB; // uint8
    constexpr std::ptrdiff_t m_lifeState = 0x338; // uint8
    constexpr std::ptrdiff_t m_hPlayerPawn = 0x7E4; // CHandle<C_CSPlayerPawn>
    constexpr std::ptrdiff_t m_vecViewOffset = 0xC58; // CNetworkViewOffsetVector
    constexpr std::ptrdiff_t m_iHealth = 0x334; // int32

    if (driver::attach_to_process(driver, pid) == true) {
        if (const std::uintptr_t client = GetModuleBaseAddress(pid, L"client.dll"); client != 0) {
            MessageBoxW(NULL, L"Attachment successful.", L"Success", MB_OK | MB_ICONINFORMATION);

            // create window
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(WNDCLASSEXW);
            wc.style = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc = window_procedure;
            wc.hInstance = instance;
            wc.lpszClassName = L"^^";

            RegisterClassExW(&wc);

            const HWND overlay = CreateWindowExW(
                WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED,
                wc.lpszClassName,
                L"^^",
                WS_POPUP,
                0,
                0,
                screenWidth,
                screenHeight,
                nullptr,
                nullptr,
                wc.hInstance,
                nullptr
            );

            SetLayeredWindowAttributes(overlay, RGB(0, 0, 0), BYTE(255), LWA_ALPHA);

            {
                RECT client_area{};
                GetClientRect(overlay, &client_area);

                RECT window_area{};
                GetWindowRect(overlay, &window_area);

                POINT diff{};
                ClientToScreen(overlay, &diff);

                const MARGINS margins{
                    window_area.left + (diff.x - window_area.left),
                    window_area.top + (diff.y - window_area.top),
                    client_area.right,
                    client_area.bottom,
                };
                DwmExtendFrameIntoClientArea(overlay, &margins);
            }

            DXGI_SWAP_CHAIN_DESC sd{};
            sd.BufferDesc.RefreshRate.Numerator = 75U; // fps
            sd.BufferDesc.RefreshRate.Numerator = 1U;
            sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.SampleDesc.Count = 1U;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.BufferCount = 2U;
            sd.OutputWindow = overlay;
            sd.Windowed = TRUE;
            sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

            constexpr D3D_FEATURE_LEVEL levels[2]{
                D3D_FEATURE_LEVEL_11_0,
                D3D_FEATURE_LEVEL_10_0,
            };

            ID3D11Device* device{ nullptr };
            ID3D11DeviceContext* device_context{ nullptr };
            IDXGISwapChain* swap_chain{ nullptr };
            ID3D11RenderTargetView* render_target_view{ nullptr };
            D3D_FEATURE_LEVEL level{};

            // create device
            D3D11CreateDeviceAndSwapChain(
                nullptr,
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,
                0U,
                levels,
                2U,
                D3D11_SDK_VERSION,
                &sd,
                &swap_chain,
                &device,
                &level,
                &device_context
            );

            ID3D11Texture2D* back_buffer{ nullptr };
            swap_chain->GetBuffer(0U, IID_PPV_ARGS(&back_buffer));

            if (back_buffer) {
                device->CreateRenderTargetView(back_buffer, nullptr, &render_target_view);
                back_buffer->Release();
            }
            else
                return 1;

            ShowWindow(overlay, cmd_show);
            UpdateWindow(overlay);

            ImGui::CreateContext();
            ImGui::StyleColorsClassic();

            ImGui_ImplWin32_Init(overlay);
            ImGui_ImplDX11_Init(device, device_context);

            bool running = true;

            while (running)
            {
                MSG msg;
                while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);

                    if (msg.message == WM_QUIT)
                    {
                        running = false;
                    }
                }
                if (!running)
                    break;

                // offsets
                uintptr_t localPlayer = driver::read_memory<uintptr_t>(driver, client + dwLocalPlayerPawn);
                Vector3 Localorgin = driver::read_memory<Vector3>(driver, localPlayer + m_vOldOrigin);
                view_matrix_t view_matrix = driver::read_memory<view_matrix_t>(driver, client + dwViewMatrix);
                uintptr_t entity_list = driver::read_memory<uintptr_t>(driver, client + dwEntityList);
                int localTeam = driver::read_memory<int>(driver, localPlayer + m_iTeamNum);

                ImGui_ImplDX11_NewFrame();
                ImGui_ImplWin32_NewFrame();
                ImGui::NewFrame();

                for (int playerIndex = 1; playerIndex < 32; ++playerIndex) {
                    uintptr_t listentry = driver::read_memory<uintptr_t>(driver, entity_list + (8 * (playerIndex & 0x7FFF) >> 9) + 16);

                    if (!listentry)
                        continue;

                    uintptr_t player = driver::read_memory<uintptr_t>(driver, listentry + 120 * (playerIndex & 0x1FF));

                    if (!player)
                        continue;

                    int playerTeam = driver::read_memory<int>(driver, player + m_iTeamNum);

                    if (playerTeam == localTeam)
                        continue;

                    uint32_t playerPawn = driver::read_memory<uint32_t>(driver, player + m_hPlayerPawn);

                    uintptr_t listentry2 = driver::read_memory<uintptr_t>(driver, entity_list + 0x8 * ((playerPawn & 0x7FFF) >> 9) + 16);

                    if (!listentry2)
                        continue;

                    uintptr_t pCSPlayerPawn = driver::read_memory<uintptr_t>(driver, listentry2 + 120 * (playerPawn & 0x1FF));

                    if (!pCSPlayerPawn)
                        continue;

                    int health = driver::read_memory<int>(driver, pCSPlayerPawn + m_iHealth);
                    
                    if (health <= 0 || health > 100)
                        continue;

                    if (pCSPlayerPawn == localPlayer)
                        continue;

                    Vector3 orgian = driver::read_memory<Vector3>(driver, pCSPlayerPawn + m_vOldOrigin);
                    Vector3 head = { orgian.x, orgian.y, orgian.z + 75.f };

                    Vector3 screenPos = orgian.WTS(view_matrix);
                    Vector3 screenHead = head.WTS(view_matrix);

                    float height = screenPos.y - screenHead.y;
                    float width = height / 2.4f;

                    RGB enmey = { 128, 0, 128 };

                    Render::DrawRect(
                        screenHead.x - width / 2,
                        screenHead.y,
                        width,
                        height,
                        enmey,
                        1.5
                    );
                }

                ImGui::Render();
                float color[4]{ 0,0,0,0 };
                device_context->OMSetRenderTargets(1U, &render_target_view, nullptr);
                device_context->ClearRenderTargetView(render_target_view, color);

                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

                swap_chain->Present(0U, 0U);
            }

            // exiting
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplDX11_Shutdown();

            ImGui::DestroyContext();

            if (swap_chain) {
                swap_chain->Release();
            }

            if (device_context) {
                device_context->Release();
            }

            if (device) {
                device->Release();
            }

            if (render_target_view)
            {
                render_target_view->Release();
            }

            DestroyWindow(overlay);
            UnregisterClassW(wc.lpszClassName, wc.hInstance);
        }
    }

    // close handle to driver
    CloseHandle(driver);

    return 0; // successful execution
}