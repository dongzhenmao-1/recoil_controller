#define NOMINMAX // 阻止 Windows 头文件注入破坏 std::min
#include "scancode.hpp"
#include "my_algorithm.hpp"

#include <iostream>
#include <fstream>
#include <filesystem>
#include <thread>
#include <mutex>
#include <chrono>
#include <atomic>
#include <vector>
#include <string>
#include <algorithm>

// 移除了 interception.h 依赖
#include <windows.h>
#include <nlohmann/json.hpp>

// ImGui 及图形 API 依赖
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <d3d11.h>
#include <tchar.h>

#pragma comment(lib, "d3d11.lib")

// ==========================================
// 数据结构与槽位定义
// ==========================================
struct WeaponSlot {
    std::string file_name = "None";
    std::string weapon_name = "None";
    std::vector<mtd::point2d> raw_pattern;
    std::vector<mtd::point2i> pattern;
    float fire_rate = 10.0f;
    int delay_ms = 0;
    float hr = 1.0f;
    float vr = 1.0f;
};

// 对应三个槽位：0: 主武器, 1: 副武器, 2: 手枪
WeaponSlot slots[3]; 

// 当前激活的槽位：-1 为未开启/刀（关闭）, 0: 主武器, 1: 副武器, 2: 手枪
std::atomic<int> active_slot{-1}; 

// ==========================================
// 新增：标准的串口物理硬件通信类
// ==========================================
class SerialHardware {
    HANDLE hSerial = INVALID_HANDLE_VALUE;
public:
    bool connect(const std::string& port) {
        close();
        std::string fullPort = "\\\\.\\" + port; // 兼容 COM10 以上端口
        hSerial = CreateFileA(fullPort.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hSerial == INVALID_HANDLE_VALUE) return false;

        DCB dcbSerialParams = { 0 };
        dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
        if (!GetCommState(hSerial, &dcbSerialParams)) {
            close();
            return false;
        }
        dcbSerialParams.BaudRate = CBR_115200;
        dcbSerialParams.ByteSize = 8;
        dcbSerialParams.StopBits = ONESTOPBIT;
        dcbSerialParams.Parity = NOPARITY;
        if (!SetCommState(hSerial, &dcbSerialParams)) {
            close();
            return false;
        }

        COMMTIMEOUTS timeouts = { 0 };
        timeouts.ReadIntervalTimeout = 50;
        timeouts.ReadTotalTimeoutConstant = 50;
        timeouts.ReadTotalTimeoutMultiplier = 10;
        timeouts.WriteTotalTimeoutConstant = 50;
        timeouts.WriteTotalTimeoutMultiplier = 10;
        SetCommTimeouts(hSerial, &timeouts);

        return true;
    }

    void move(int x, int y) {
        if (hSerial == INVALID_HANDLE_VALUE) return;
        std::string cmd = std::to_string(x) + "," + std::to_string(y) + "\n";
        DWORD written;
        WriteFile(hSerial, cmd.c_str(), (DWORD)cmd.length(), &written, NULL);
    }

    bool isOpen() const { return hSerial != INVALID_HANDLE_VALUE; }
    void close() {
        if (hSerial != INVALID_HANDLE_VALUE) {
            CloseHandle(hSerial);
            hSerial = INVALID_HANDLE_VALUE;
        }
    }
    ~SerialHardware() { close(); }
};

SerialHardware g_hw;
bool use_hardware = false; // 是否启用物理硬件模式
std::string com_port = "COM3";
char com_buf[16] = "COM3";

// ==========================================
// 系统配置与按键映射 (升级为标准 Virtual Keys 键码)
// ==========================================
struct Hotkeys {
    int toggle_macro = VK_F8;
    int reload_config = VK_F9;
    int exit_app = VK_F7;
    int primary = '1';
    int secondary = '2';
    int melee = '3';   
    int pistol = '4';  
} hotkeys;

// 线程与同步控制
std::atomic<bool> is_firing{false};
std::atomic<bool> is_running{true};
std::atomic<bool> is_open{false};
std::mutex data_mutex;

std::vector<std::string> weapon_files; 

// 按键绑定状态管理
std::atomic<bool> binding_mode{false};
int* target_bind = nullptr; 

// 边缘触发状态辅助数据结构
struct KeyState {
    bool is_down = false;
    bool pressed = false; 
};
KeyState keys_state[256];

void update_keys() {
    for (int vk = 1; vk < 256; ++vk) {
        bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        keys_state[vk].pressed = (down && !keys_state[vk].is_down);
        keys_state[vk].is_down = down;
    }
}

// ==========================================
// 文件 IO 操作
// ==========================================

void scan_data_folder() {
    weapon_files.clear();
    weapon_files.push_back("None"); 
    try {
        std::filesystem::create_directories("config/data");
        for (const auto& entry : std::filesystem::directory_iterator("config/data")) {
            if (entry.is_regular_file()) {
                std::string ext = entry.path().extension().string();
                if (ext == ".json" || ext == ".txt") {
                    weapon_files.push_back(entry.path().filename().string());
                }
            }
        }
    } catch (...) {
        std::cerr << "[Error] Failed to scan config/data folder\n";
    }
}

void apply_ratios_to_slot(int slot_idx) {
    std::lock_guard<std::mutex> lock(data_mutex);
    slots[slot_idx].pattern.clear();
    for (const auto& p : slots[slot_idx].raw_pattern) {
        slots[slot_idx].pattern.push_back(mtd::point2i({
            static_cast<int>(p.x * slots[slot_idx].hr),
            static_cast<int>(p.y * slots[slot_idx].vr)
        }));
    }
}

bool load_weapon_to_slot(int slot_idx, const std::string& file_name) {
    if (file_name == "None" || file_name.empty()) {
        std::lock_guard<std::mutex> lock(data_mutex);
        slots[slot_idx].file_name = "None";
        slots[slot_idx].weapon_name = "None";
        slots[slot_idx].raw_pattern.clear();
        slots[slot_idx].pattern.clear();
        return true;
    }

    std::string full_path = "config/data/" + file_name;
    if (!std::filesystem::exists(full_path)) return false;

    try {
        std::ifstream fin(full_path);
        nlohmann::json j = nlohmann::json::parse(fin);

        std::lock_guard<std::mutex> lock(data_mutex);
        slots[slot_idx].file_name = file_name;
        slots[slot_idx].weapon_name = j.value("name", "Unknown");
        slots[slot_idx].delay_ms = int(j.value("delay", 0.0) * 1000);
        slots[slot_idx].fire_rate = j.value("fire_rate", 10.0f);
        slots[slot_idx].hr = j.value("horizontal_ratio", 1.0f);
        slots[slot_idx].vr = j.value("vertical_ratio", 1.0f);
        slots[slot_idx].raw_pattern = j["spray_pattern"].get<std::vector<mtd::point2d>>();
        
        slots[slot_idx].pattern.clear();
        for (const auto& p : slots[slot_idx].raw_pattern) {
            slots[slot_idx].pattern.push_back(mtd::point2i({
                static_cast<int>(p.x * slots[slot_idx].hr),
                static_cast<int>(p.y * slots[slot_idx].vr)
            }));
        }
        return true;
    } catch (...) {
        return false;
    }
}

void save_weapon_to_json(int slot_idx) {
    if (slots[slot_idx].file_name == "None" || slots[slot_idx].file_name.empty()) return;
    std::string full_path = "config/data/" + slots[slot_idx].file_name;
    try {
        nlohmann::json j;
        {
            std::lock_guard<std::mutex> lock(data_mutex);
            j["name"] = slots[slot_idx].weapon_name;
            j["delay"] = double(slots[slot_idx].delay_ms) / 1000.0;
            j["fire_rate"] = slots[slot_idx].fire_rate;
            j["horizontal_ratio"] = slots[slot_idx].hr;
            j["vertical_ratio"] = slots[slot_idx].vr;
            j["spray_pattern"] = slots[slot_idx].raw_pattern;
        }
        std::ofstream fout(full_path);
        fout << j.dump(4);
    } catch (...) {}
}

void load_global_config() {
    std::filesystem::create_directories("config");
    if (!std::filesystem::exists("config/config.json")) {
        nlohmann::json j;
        j["com_port"] = com_port;
        j["use_hardware"] = use_hardware;

        j["hotkeys"]["toggle_macro"] = hotkeys.toggle_macro;
        j["hotkeys"]["reload_config"] = hotkeys.reload_config;
        j["hotkeys"]["exit_app"] = hotkeys.exit_app;
        j["hotkeys"]["primary"] = hotkeys.primary;
        j["hotkeys"]["secondary"] = hotkeys.secondary;
        j["hotkeys"]["melee"] = hotkeys.melee;
        j["hotkeys"]["pistol"] = hotkeys.pistol;
        std::ofstream fout("config/config.json");
        fout << j.dump(4);
        return;
    }

    try {
        std::ifstream fin("config/config.json");
        nlohmann::json j = nlohmann::json::parse(fin);

        com_port = j.value("com_port", "COM3");
        strcpy_s(com_buf, sizeof(com_buf), com_port.c_str());
        use_hardware = j.value("use_hardware", false);

        if (j.contains("hotkeys")) {
            auto h = j["hotkeys"];
            hotkeys.toggle_macro = h.value("toggle_macro", VK_F8);
            hotkeys.reload_config = h.value("reload_config", VK_F9);
            hotkeys.exit_app = h.value("exit_app", VK_F7);
            hotkeys.primary = h.value("primary", '1');
            hotkeys.secondary = h.value("secondary", '2');
            hotkeys.melee = h.value("melee", '3');
            hotkeys.pistol = h.value("pistol", '4');
        }
    } catch (...) {}
}

void save_global_config() {
    try {
        nlohmann::json j;
        j["com_port"] = com_port;
        j["use_hardware"] = use_hardware;

        j["hotkeys"]["toggle_macro"] = hotkeys.toggle_macro;
        j["hotkeys"]["reload_config"] = hotkeys.reload_config;
        j["hotkeys"]["exit_app"] = hotkeys.exit_app;
        j["hotkeys"]["primary"] = hotkeys.primary;
        j["hotkeys"]["secondary"] = hotkeys.secondary;
        j["hotkeys"]["melee"] = hotkeys.melee;
        j["hotkeys"]["pistol"] = hotkeys.pistol;
        std::ofstream fout("config/config.json");
        fout << j.dump(4);
    } catch (...) {}
}

void load_latest_weapons() {
    if (!std::filesystem::exists("config/latest.json")) return;
    try {
        std::ifstream fin("config/latest.json");
        nlohmann::json j = nlohmann::json::parse(fin);
        load_weapon_to_slot(0, j.value("primary_weapon", "None"));
        load_weapon_to_slot(1, j.value("secondary_weapon", "None"));
        load_weapon_to_slot(2, j.value("pistol_weapon", "None"));
    } catch (...) {}
}

void save_latest_weapons() {
    try {
        nlohmann::json j;
        j["primary_weapon"] = slots[0].file_name;
        j["secondary_weapon"] = slots[1].file_name;
        j["pistol_weapon"] = slots[2].file_name;
        std::ofstream fout("config/latest.json");
        fout << j.dump(4);
    } catch (...) {}
}

std::string get_key_name(int vk) {
    switch (vk) {
        case '1': return "1"; case '2': return "2";
        case '3': return "3"; case '4': return "4";
        case 'A': return "A"; case 'B': return "B"; case 'C': return "C";
        case 'D': return "D"; case 'E': return "E"; case 'F': return "F";
        case 'G': return "G"; case 'S': return "S"; case 'W': return "W";
        case 'X': return "X"; case 'Z': return "Z";
        case VK_F1: return "F1"; case VK_F2: return "F2";
        case VK_F3: return "F3"; case VK_F4: return "F4";
        case VK_F5: return "F5"; case VK_F6: return "F6";
        case VK_F7: return "F7"; case VK_F8: return "F8";
        case VK_F9: return "F9"; case VK_F10: return "F10";
        case VK_ESCAPE: return "ESC";
        case VK_LCONTROL: return "L-Ctrl"; case VK_LMENU: return "L-Alt";
        case VK_LSHIFT: return "L-Shift"; case VK_SPACE: return "Space";
        case VK_MBUTTON: return "Mouse 3";
        case VK_XBUTTON1: return "Mouse 4";
        case VK_XBUTTON2: return "Mouse 5";
        default: return "VK " + std::to_string(vk);
    }
}

// ==========================================
// 线程 1: 压枪控制线程 (支持硬件串口输出)
// ==========================================
void recoil_controller() {
    while (is_running) {
        int current_slot = active_slot.load();
        if (is_firing && current_slot >= 0 && current_slot < 3) {
            std::vector<mtd::point2i> current_pattern;
            float current_fire_rate;
            std::chrono::milliseconds current_delay;
            std::chrono::milliseconds current_fire_cycle;
            {
                std::lock_guard<std::mutex> lock(data_mutex);
                current_pattern = slots[current_slot].pattern;
                current_fire_rate = slots[current_slot].fire_rate;
                current_delay = std::chrono::milliseconds(slots[current_slot].delay_ms);
                current_fire_cycle = std::chrono::milliseconds(int(1000 / current_fire_rate));
            }

            if (!current_pattern.empty()) {
                if (current_delay.count() > 0) std::this_thread::sleep_for(current_delay);
                
                mtd::point2i now_pos = {0, 0};
                std::chrono::steady_clock::time_point _start = std::chrono::steady_clock::now();
                std::chrono::milliseconds step_t(20);

                while (is_firing && active_slot.load() == current_slot) {
                    auto _now = std::chrono::steady_clock::now();
                    auto now_t = std::chrono::duration_cast<std::chrono::milliseconds>(_now - _start); 
                    
                    int now_step = std::ceil(current_fire_rate * now_t.count() / 1000.0 + 0.05);
                    if (now_step >= static_cast<int>(current_pattern.size())) break;

                    mtd::point2i nx_spray_pos = current_pattern[now_step];
                    std::chrono::milliseconds nx_t = current_fire_cycle * now_step;

                    double remaining_time = (nx_t - now_t).count();
                    double fraction = (remaining_time > 0.0 ? (std::min)(1.0, double(step_t.count()) / remaining_time) : 1.0);
                    
                    mtd::point2i we = mtd::point2d(nx_spray_pos - now_pos) * fraction;
                    now_pos = now_pos + we;

                    if (use_hardware && g_hw.isOpen()) {
                        // 🎯 方案一：向 RP2040 发送物理移动数据包
                        g_hw.move(-we.x, -we.y);
                    } else {
                        // 🎯 方案二：降级运行。由于完全卸载了 .sys，为了安全，请只保留硬件输出。
                        // 这里默认不调用系统底层虚拟 API，保持绝对安全。
                    }
                    std::this_thread::sleep_for(step_t);
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// ==========================================
// 线程 2: 纯用户态 GetAsyncKeyState 轮询线程
// ==========================================
void input_thread_loop() {
    while (is_running) {
        update_keys();

        // 🎯 处于物理改键模式时
        if (binding_mode.load() && target_bind != nullptr) {
            for (int vk = 1; vk < 256; ++vk) {
                // 排除左右鼠标键以免冲突
                if (vk == VK_LBUTTON || vk == VK_RBUTTON) continue;

                if (keys_state[vk].pressed) {
                    *target_bind = vk;
                    save_global_config();
                    binding_mode = false;
                    target_bind = nullptr;
                    break;
                }
            }
        } 
        else {
            // 🎯 处理全局热键与切枪按键响应
            if (keys_state[hotkeys.toggle_macro].pressed) {
                is_open = !is_open;
                if (!is_open) is_firing = false;
            } 
            else if (keys_state[hotkeys.reload_config].pressed) {
                scan_data_folder();
                load_latest_weapons();
            } 
            else if (keys_state[hotkeys.exit_app].pressed) {
                is_running = false;
            } 
            else if (keys_state[hotkeys.primary].pressed) {
                active_slot = 0;
            } 
            else if (keys_state[hotkeys.secondary].pressed) {
                active_slot = 1;
            } 
            else if (keys_state[hotkeys.melee].pressed) {
                active_slot = -1;
            } 
            else if (keys_state[hotkeys.pistol].pressed) {
                active_slot = 2;
            }
        }

        // 🎯 处理开火同步状态检测
        if (is_open.load() && active_slot.load() != -1) {
            if (keys_state[VK_LBUTTON].is_down) {
                is_firing = true;
            } else {
                is_firing = false;
            }
        } else {
            is_firing = false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5)); // 轻量睡眠避免过高 CPU 占用
    }
}

// ==========================================
// Win32 + DX11 + ImGui
// ==========================================
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
        case WM_SIZE:
            if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
                CleanupRenderTarget();
                g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                CreateRenderTarget();
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

int main() {
    load_global_config();
    scan_data_folder();
    load_latest_weapons();

    // 🎯 尝试后台静默物理连接
    if (use_hardware) {
        g_hw.connect(com_port);
    }

    std::thread th_recoil(recoil_controller);
    std::thread th_input(input_thread_loop);

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"RecoilManagerUI", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"Universal Recoil Dashboard", WS_OVERLAPPEDWINDOW, 150, 150, 800, 560, nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    MSG msg;
    ZeroMemory(&msg, sizeof(msg));
    while (msg.message != WM_QUIT && is_running) {
        if (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            continue;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("MainPanel", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBringToFrontOnFocus);

        // ==========================================
        // 顶部全局状态栏
        // ==========================================
        bool macro_on = is_open.load();
        if (ImGui::Checkbox("Enable Recoil Macro", &macro_on)) {
            is_open = macro_on;
            if (!macro_on) is_firing = false; 
        }
        ImGui::SameLine(250);
        
        int cur_active = active_slot.load();
        ImGui::Text("Active Status: ");
        ImGui::SameLine();
        if (!macro_on) {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "SYSTEM OFF");
        } else if (cur_active == -1) {
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.3f, 1.0f), "Knife / Disengaged");
        } else {
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Slot %d (%s)", cur_active + 1, slots[cur_active].weapon_name.c_str());
        }
        
        ImGui::Separator();

        // ==========================================
        // 选项卡系统
        // ==========================================
        if (ImGui::BeginTabBar("MainTabs")) {
            
            // ----------------------------------------
            // 标签页 1：武器与弹道仪表盘
            // ----------------------------------------
            if (ImGui::BeginTabItem("Dashboard & Weapons")) {
                ImGui::Spacing();
                ImGui::Columns(2, "WeaponLayout", false);
                ImGui::SetColumnWidth(0, 400.0f);

                ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "WEAPON SLOTS SELECTION");
                ImGui::Spacing();
                bool selection_changed = false;

                for (int i = 0; i < 3; ++i) {
                    std::string slot_title;
                    if (i == 0) slot_title = "Slot 1 (Primary Weapon)";
                    else if (i == 1) slot_title = "Slot 2 (Secondary Weapon)";
                    else if (i == 2) slot_title = "Slot 4 (Pistol Weapon)";

                    ImGui::PushID(i);
                    ImGui::Text("%s", slot_title.c_str());
                    ImGui::SetNextItemWidth(350.0f);
                    if (ImGui::BeginCombo("##WeaponCombo", slots[i].file_name.c_str())) {
                        for (const auto& file : weapon_files) {
                            bool is_selected = (slots[i].file_name == file);
                            if (ImGui::Selectable(file.c_str(), is_selected)) {
                                if (slots[i].file_name != file) {
                                    load_weapon_to_slot(i, file);
                                    selection_changed = true;
                                }
                            }
                            if (is_selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::PopID();
                    ImGui::Spacing();
                }

                if (selection_changed) save_latest_weapons();

                // ----------------------------------------
                // 右侧栏：传统的当前物理选中枪支调参区
                // ----------------------------------------
                ImGui::NextColumn();
                ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "LIVE PARAMETERS TUNING");
                ImGui::Spacing();

                if (cur_active >= 0 && cur_active < 3) {
                    if (slots[cur_active].file_name != "None") {
                        
                        char name_buf[128] = { 0 };
                        size_t len = slots[cur_active].weapon_name.copy(name_buf, sizeof(name_buf) - 1);
                        name_buf[len] = '\0';

                        ImGui::SetNextItemWidth(250.0f);
                        if (ImGui::InputText("Weapon Name", name_buf, sizeof(name_buf))) {
                            slots[cur_active].weapon_name = name_buf;
                        }

                        ImGui::SetNextItemWidth(250.0f);
                        int temp_delay = slots[cur_active].delay_ms;
                        if (ImGui::InputInt("Trigger Delay (ms)", &temp_delay, 10, 100)) {
                            if (temp_delay < 0) temp_delay = 0;
                            slots[cur_active].delay_ms = temp_delay;
                        }

                        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

                        bool ratio_changed = false;
                        float hr = slots[cur_active].hr;
                        float vr = slots[cur_active].vr;

                        ImGui::SetNextItemWidth(250.0f);
                        if (ImGui::SliderFloat("Horiz Multiplier", &hr, 0.1f, 5.0f, "%.2f")) {
                            slots[cur_active].hr = hr;
                            ratio_changed = true;
                        }
                        ImGui::SetNextItemWidth(250.0f);
                        if (ImGui::SliderFloat("Vert Multiplier", &vr, 0.1f, 5.0f, "%.2f")) {
                            slots[cur_active].vr = vr;
                            ratio_changed = true;
                        }

                        if (ratio_changed) {
                            apply_ratios_to_slot(cur_active);
                        }

                        ImGui::Spacing();
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.3f, 1.0f));
                        if (ImGui::Button("Save Changes to Weapon file (JSON/TXT)", ImVec2(280, 32))) {
                            save_weapon_to_json(cur_active);
                        }
                        ImGui::PopStyleColor();
                    } else {
                        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No weapon loaded in this active slot.\nSelect a weapon on the left first.");
                    }
                } else {
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Melee/Disabled slot active.\nPlease press hotkey 1, 2, or 4 to select a weapon slot to tune.");
                }
                
                ImGui::Columns(1);
                ImGui::EndTabItem();
            }

            // ----------------------------------------
            // 标签页 2：系统设置与改键
            // ----------------------------------------
            if (ImGui::BeginTabItem("Settings & Keybinds")) {
                ImGui::Spacing();
                
                // 🎯 串口物理硬件选项替换了原先的 Interception 设备设置
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "HARDWARE ACCELERATION (RP2040)");
                ImGui::Text("Enable this to completely bypass synthetic/software click sending.");
                
                if (ImGui::Checkbox("Use RP2040 Physical Controller", &use_hardware)) {
                    save_global_config();
                }

                ImGui::SetNextItemWidth(150.0f);
                if (ImGui::InputText("COM Port ID", com_buf, sizeof(com_buf))) {
                    com_port = com_buf;
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    save_global_config();
                }
                
                ImGui::SameLine();
                if (g_hw.isOpen()) {
                    if (ImGui::Button("Disconnect")) {
                        g_hw.close();
                    }
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "CONNECTED");
                } else {
                    if (ImGui::Button("Connect Hardware")) {
                        if (g_hw.connect(com_port)) {
                            save_global_config();
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.8f, 0.3f, 0.3f, 1.0f), "DISCONNECTED");
                }

                ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
                
                ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "HOTKEYS REBIND PANEL");
                if (binding_mode.load()) {
                    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), ">> PRESS ANY KEY TO BIND <<");
                } else {
                    ImGui::Text("Click a button below to bind keys:");
                }

                auto RenderBindButton = [&](const char* label, int* key_var) {
                    std::string btn_label = std::string(label) + " [" + get_key_name(*key_var) + "]##" + label;
                    if (ImGui::Button(btn_label.c_str(), ImVec2(300, 26))) {
                        target_bind = key_var;
                        binding_mode = true;
                    }
                };

                ImGui::Columns(2, "BindsLayout", false);
                RenderBindButton("Toggle Macro Key", &hotkeys.toggle_macro);
                RenderBindButton("Reload Setup Key", &hotkeys.reload_config);
                RenderBindButton("Exit Manager Key", &hotkeys.exit_app);
                
                ImGui::NextColumn();
                RenderBindButton("Slot 1 (Primary) Key", &hotkeys.primary);
                RenderBindButton("Slot 2 (Secondary) Key", &hotkeys.secondary);
                RenderBindButton("Slot 3 (Knife/Off) Key", &hotkeys.melee);
                RenderBindButton("Slot 4 (Pistol) Key", &hotkeys.pistol);
                
                ImGui::Columns(1);
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
        ImGui::End();

        ImGui::Render();
        const float clear_color[4] = { 0.15f, 0.15f, 0.15f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    is_running = false;
    if (th_recoil.joinable()) th_recoil.join();
    if (th_input.joinable()) th_input.join(); 

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext) != S_OK)
        return false;
    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}