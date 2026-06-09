#include "scancode.hpp"
#include "my_algorithm.hpp"

#include <iostream>
#include <thread>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>
#include <condition_variable>
#include <atomic>
#include <sstream>
#include <coroutine> // 防止实验性协程断言阻断

// C++/WinRT 投影头文件
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

// COM 互操作与图形头文件
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <d3d11.h>
#include <dxgi1_2.h>

// 🎯 DWM 用于窗口透明
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

// OpenCV 4 头文件
#include <opencv2/opencv.hpp>

// ONNX Runtime 头文件
#include <onnxruntime_cxx_api.h>

// Dear ImGui 头文件及后端
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

// 🎯 Interception 驱动级输入库
#include <interception.h>
#pragma comment(lib, "interception.lib")

// 自动链接库
#pragma comment(lib, "windowsapp")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

// 声明 ImGui 的 Win32 消息处理函数
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ==========================================
// YOLO26-Pose 结构体与全局变量
// ==========================================
const std::vector<std::pair<int, int>> SKELETON_CONNECTIONS = {
    {0, 1}, {0, 2}, {1, 3}, {2, 4},           // 脸部
    {5, 6}, {5, 7}, {7, 9}, {6, 8}, {8, 10},  // 双臂
    {11, 12}, {5, 11}, {6, 12},               // 躯干
    {11, 13}, {13, 15}, {12, 14}, {14, 16}    // 双腿
};

struct Keypoint {
    float x;
    float y;
    float confidence;
};

struct PoseDetection {
    cv::Rect2f box;
    float score;
    int classId;
    std::vector<Keypoint> keypoints;
};

// ==========================================
// 模块 1: 通用工具箱 (Utils)
// ==========================================
namespace Utils {
    HWND FindWindowByTitleKeywords(const std::vector<std::string>& keywords) {
        for (HWND hwnd = GetTopWindow(NULL); hwnd != NULL; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
            if (IsWindowVisible(hwnd)) {
                char title[512];
                if (GetWindowTextA(hwnd, title, sizeof(title)) > 0) {
                    std::string titleStr(title);
                    for (const auto& keyword : keywords) {
                        if (titleStr.find(keyword) != std::string::npos) {
                            return hwnd;
                        }
                    }
                }
            }
        }
        return nullptr;
    }
}

// ==========================================
// 模块 2: 异步多线程透明悬浮覆层类
// ==========================================
class WindowOverlay {
public:
    WindowOverlay(HWND targetHwnd) : m_targetHwnd(targetHwnd) {
        WNDCLASSEXA wc = { sizeof(WNDCLASSEXA) };
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = "OverlayViewerClass";
        RegisterClassExA(&wc);
        
        m_viewerHwnd = CreateWindowExA(
            WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED,
            "OverlayViewerClass", "YOLO26-Pose Overlay",
            WS_POPUP,
            0, 0, 100, 100,
            nullptr, nullptr, wc.hInstance, this
        );

        SetLayeredWindowAttributes(m_viewerHwnd, 0, 255, LWA_ALPHA);

        MARGINS margins = { -1, -1, -1, -1 };
        DwmExtendFrameIntoClientArea(m_viewerHwnd, &margins);
    }

    int is_running = true, is_chasing;

    void Start() {
        std::cout << "初始化 D3D11 设备与渲染资源...\n";
        InitGraphics();

        std::cout << "初始化 ImGui 环境...\n";
        InitImGui();

        std::cout << "初始化 YOLO26-Pose ONNX 引擎...\n";
        InitYOLO();

        // 🎯 优化：启动独立的后台推理工作线程
        m_workerRunning = true;
        m_workerThread = std::thread(&WindowOverlay::WorkerThreadLoop, this);

        // 🎯 新增：启动独立的 Interception 驱动按键检测线程
        m_inputRunning = true;
        m_inputThread = std::thread(&WindowOverlay::InputThreadLoop, this);

        std::thread function(&WindowOverlay::recoil_controller, this);

        std::cout << "启动窗口捕获...\n";
        StartCapture();

        ShowWindow(m_viewerHwnd, SW_SHOWNOACTIVATE);
        UpdateWindow(m_viewerHwnd);

        MSG msg;
        ZeroMemory(&msg, sizeof(msg));
        while (msg.message != WM_QUIT) {
            if (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
                continue;
            }

            UpdateOverlayPosition();
            RenderFrame();
            if (roi_change.x != -1) {
                roi = roi_change;
                roi_change = {-1, -1};
            }

            // 🎯 核心优化 1：彻底删除这里的 sleep_for 延迟！
            // 因为 RenderFrame() 内部最后的 Present(1, 0) 是开启硬件垂直同步（VSync）的
            // 它已经是一个硬件级、极度精准的线程休眠阻断。这里如果保留 sleep 会严重干扰系统调度，造成掉帧和极强滞后感。
        }

        is_running = false;

        std::cout << "正在清理资源退出...\n";
        StopCapture();

        if (function.joinable()) function.join();

        // 🎯 新增：关闭 Interception 上下文以安全解除按键线程的阻塞
        if (m_interceptionContext) {
            interception_destroy_context(m_interceptionContext);
            m_interceptionContext = nullptr;
        }
        m_inputRunning = false;
        if (m_inputThread.joinable()) {
            m_inputThread.join();
        }

        // 优化：平滑退出后台推理线程
        {
            std::lock_guard<std::mutex> lock(m_workerMutex);
            m_workerRunning = false;
            m_workerCv.notify_one();
        }
        if (m_workerThread.joinable()) {
            m_workerThread.join();
        }

        CleanupImGui();
    }

private:
    struct TargetOffset {
        int w, h;
        float dx = 0.0f;       // 距离准星水平偏移（像素）
        float dy = 0.0f;       // 距离准星垂直偏移（像素）
        float distance = 0.0f; // 真实的欧氏距离（像素）
        bool found = false;    // 是否成功寻找到目标
    };

    // 🎯 获取距离准星最近的头部相对坐标
    TargetOffset GetClosestHeadOffset(
        const std::vector<PoseDetection>& detections, 
        int clientW, 
        int clientH, 
        int offsetX, 
        int offsetY,
        float confidence_threshold = 0.5f
    ) {
        TargetOffset bestTarget;
        
        // 安全防护
        if (detections.empty() || clientW <= 1 || clientH <= 1) {
            return bestTarget;
        }

        // 1. 计算游戏准星中心点
        float centerX = clientW / 2.0f;
        float centerY = clientH / 2.0f;
        
        // 初始化最小距离平方为最大浮点数
        float minDistanceSq = std::numeric_limits<float>::max(); 

        // 2. 遍历所有检测到的人
        for (const auto& det : detections) {
            
            // 调用我们之前定义的 GetHeadCenter 获取头部中心
            Point2D head = GetHeadCenter(det, confidence_threshold);
            if (!head.valid) {
                continue;
            }

            // 3. 转换到游戏画面内部坐标
            float head_cx = head.x - offsetX;
            float head_cy = head.y - offsetY;

            // 4. 计算当前头部相对于准星中心的相对距离
            float dx = head_cx - centerX;
            float dy = head_cy - centerY;

            // 🎯 优化：使用距离平方 (dx^2 + dy^2) 进行大小对比，免去 sqrt 的性能开销
            float distSq = dx * dx + dy * dy;

            // 5. 寻找最近的目标
            if (distSq < minDistanceSq) {
                minDistanceSq = distSq;
                bestTarget.dx = dx;
                bestTarget.dy = dy;
                bestTarget.w = det.box.width;
                bestTarget.h = det.box.height;
                bestTarget.distance = std::sqrt(distSq); // 仅在确定是最近目标时才算一次开方
                bestTarget.found = true;
            }
        }

        return bestTarget;
    }
    
    InterceptionDevice mouse_device = 12;
    InterceptionDevice keyboard_device = 4;

    void recoil_controller() {
        while (is_running) {
            if (is_chasing) {
                std::cout << "[Controller] 动态自适应追踪启动...\n";
                while (is_chasing) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(12));

                    std::vector<PoseDetection> currentDetections; 
                    {
                        std::lock_guard<std::mutex> lock(m_detectionsMutex);
                        currentDetections = m_sharedDetections;
                    }
                    
                    int offsetX, offsetY, clientW, clientH;
                    get_wh(offsetX, offsetY, clientW, clientH);
                    TargetOffset target = GetClosestHeadOffset(currentDetections, clientW, clientH, offsetX, offsetY);

                    // 🎯 优化 1：一旦丢失目标，立刻“瞬间拉满视野”到 640x640，重新大范围捕捉！
                    // 彻底避免慢慢变大导致空档期锁定到“路人”身上。
                    if (target.found == false) {
                        roi_change = {640, 640};
                        continue;
                    }

                    // 🎯 优化 2：基于严谨几何学计算 ROI（半径必须包容 |dx| + w/2）
                    // opt = 2 * |dx| + w + 额外安全缓冲
                    int opt_w = 2 * std::abs(target.dx) + target.w + 60;
                    int opt_h = 2 * std::abs(target.dy) + target.h + 60;
                    int opt = std::max(opt_w, opt_h);

                    // 🎯 优化 3：限制安全区间 [320, 640]
                    // 320 能够保证放大 2 倍时画质依然清晰，且留下足够宽敞的后坐力/移动缓冲带，绝不丢标。
                    opt = std::clamp(opt, 320, 640);
                    roi_change = {opt, opt};

                    // 🎯 优化 4：使用手感极佳的平滑对称舍入鼠标控制
                    float speed_factor = 0.15f; // 平滑平移速度系数
                    float rx = target.dx * speed_factor;
                    float ry = target.dy * speed_factor;

                    int move_x = 0;
                    int move_y = 0;

                    if (std::abs(rx) > 0.01f) {
                        move_x = static_cast<int>(std::round(rx));
                        if (move_x == 0) move_x = (rx > 0) ? 1 : -1; // 1 像素强制微调
                    }
                    if (std::abs(ry) > 0.01f) {
                        move_y = static_cast<int>(std::round(ry));
                        if (move_y == 0) move_y = (ry > 0) ? 1 : -1;
                    }

                    InterceptionMouseStroke move = {};
                    move.flags = INTERCEPTION_MOUSE_MOVE_RELATIVE;
                    move.x = move_x;
                    move.y = move_y;

                    interception_send(m_interceptionContext, mouse_device, (InterceptionStroke*)&move, 1);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
            return true;

        WindowOverlay* viewer = nullptr;
        if (msg == WM_NCCREATE) {
            CREATESTRUCTA* pCreate = reinterpret_cast<CREATESTRUCTA*>(lParam);
            viewer = reinterpret_cast<WindowOverlay*>(pCreate->lpCreateParams);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(viewer));
        } else {
            viewer = reinterpret_cast<WindowOverlay*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        }

        if (viewer) {
            switch (msg) {
                case WM_SIZE:
                    viewer->OnResize(LOWORD(lParam), HIWORD(lParam));
                    return 0;
                case WM_DESTROY:
                    PostQuitMessage(0);
                    return 0;
            }
        }

        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }

    void UpdateOverlayPosition() {
        if (!IsWindow(m_targetHwnd)) {
            std::cout << "目标播放器窗口已关闭，准备退出...\n";
            PostQuitMessage(0);
            return;
        }

        // 1. 获取当前系统正处于最前端激活（Focused）的窗口句柄
        HWND activeHwnd = GetForegroundWindow();
        
        // 2. 判定：只有当活跃窗口是“游戏窗口”或“我们自己的覆层”时，才视为聚焦在游戏上
        bool isFocused = (activeHwnd == m_targetHwnd || activeHwnd == m_viewerHwnd);

        // 3. 如果没聚焦在游戏上，或者游戏被最小化了，则彻底隐身，不干涉其它软件
        if (!isFocused || IsIconic(m_targetHwnd) || !IsWindowVisible(m_targetHwnd)) {
            ShowWindow(m_viewerHwnd, SW_HIDE);
        } 
        else {
            // 4. 只有聚焦在游戏上时，才显示悬浮窗并进行位置对齐
            ShowWindow(m_viewerHwnd, SW_SHOWNOACTIVATE);

            RECT rect;
            GetClientRect(m_targetHwnd, &rect);
            
            POINT topLeft = { rect.left, rect.top };
            ClientToScreen(m_targetHwnd, &topLeft);

            int w = rect.right - rect.left;
            int h = rect.bottom - rect.top;

            // 保持吸附并置顶（因为此时游戏就在前台，所以置顶表现为刚好贴在游戏表面）
            SetWindowPos(m_viewerHwnd, HWND_TOPMOST, topLeft.x, topLeft.y, w, h, SWP_NOACTIVATE);
        }
    }

    void InitGraphics() {
        winrt::com_ptr<ID3D11Device> d3dDevice;
        winrt::com_ptr<ID3D11DeviceContext> d3dContext;
        winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, 
            d3dDevice.put(), nullptr, d3dContext.put()));
        
        d3dDevice.as<ID3D10Multithread>()->SetMultithreadProtected(TRUE);
        m_d3dDevice = d3dDevice;
        m_d3dContext = d3dContext;

        winrt::com_ptr<IInspectable> inspectableDevice;
        winrt::check_hresult(
            CreateDirect3D11DeviceFromDXGIDevice(d3dDevice.as<IDXGIDevice>().get(), inspectableDevice.put())
        );
        m_winrtDevice = inspectableDevice.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

        winrt::com_ptr<IDXGIAdapter> adapter;
        d3dDevice.as<IDXGIDevice>()->GetAdapter(adapter.put());
        winrt::com_ptr<IDXGIFactory> dxgiFactory;
        adapter->GetParent(winrt::guid_of<IDXGIFactory>(), dxgiFactory.put_void());

        RECT rect;
        GetClientRect(m_viewerHwnd, &rect);

        DXGI_SWAP_CHAIN_DESC desc = {};
        desc.BufferDesc.Width = std::max<LONG>(1, rect.right - rect.left);
        desc.BufferDesc.Height = std::max<LONG>(1, rect.bottom - rect.top);
        desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.BufferDesc.RefreshRate.Numerator = 60;
        desc.BufferDesc.RefreshRate.Denominator = 1;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 1;
        desc.OutputWindow = m_viewerHwnd;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        winrt::com_ptr<IDXGISwapChain> swapChain;
        winrt::check_hresult(dxgiFactory->CreateSwapChain(d3dDevice.get(), &desc, swapChain.put()));
        
        m_swapChain = swapChain.as<IDXGISwapChain1>();
        
        UpdateRenderTarget();
    }

    void UpdateRenderTarget() {
        m_rtv = nullptr;
        winrt::com_ptr<ID3D11Texture2D> backBuffer;
        winrt::check_hresult(m_swapChain->GetBuffer(0, winrt::guid_of<ID3D11Texture2D>(), backBuffer.put_void()));
        winrt::check_hresult(m_d3dDevice->CreateRenderTargetView(backBuffer.get(), nullptr, m_rtv.put()));
    }

    void OnResize(int w, int h) {
        if (m_swapChain && w > 0 && h > 0) {
            m_rtv = nullptr;
            m_swapChain->ResizeBuffers(1, w, h, DXGI_FORMAT_UNKNOWN, 0);
            UpdateRenderTarget();
        }
    }

    void InitImGui() {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO(); (void)io;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        
        ImGui::StyleColorsDark();

        ImGui_ImplWin32_Init(m_viewerHwnd);
        ImGui_ImplDX11_Init(m_d3dDevice.get(), m_d3dContext.get());
    }

    void CleanupImGui() {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }

    void InitYOLO() {
        try {
            m_ortEnv = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "YOLO26-Pose");
            Ort::SessionOptions session_options;
            session_options.SetIntraOpNumThreads(1); // 🎯 GPU 模式下，CPU 辅助线程设为 1 即可，防止过多的 CPU 上下文切换导致延迟
            session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

            std::vector<std::string> available_providers = Ort::GetAvailableProviders();
            auto cuda_it = std::find(available_providers.begin(), available_providers.end(), "CUDAExecutionProvider");
            
            if (cuda_it != available_providers.end()) {
                OrtCUDAProviderOptions cuda_options;
                cuda_options.device_id = 0;
                cuda_options.cudnn_conv_algo_search = OrtCudnnConvAlgoSearchHeuristic; // 🎯 优化为启发式搜索，减少初次加载卡顿
                cuda_options.gpu_mem_limit = SIZE_MAX;
                // 开启 ONNX Runtime 自带的底层优化
                session_options.AppendExecutionProvider_CUDA(cuda_options);
                session_options.AddConfigEntry("enable_cuda_graph", "1"); // 🎯 启用 CUDA Graph 减少 CPU-GPU 调度开销
                std::cout << "[YOLO] 成功启用 CUDA 并开启极致延迟优化！\n";
            } else {
                std::cout << "[YOLO] 未检测到本地 GPU/CUDA 环境。已自动回退到 CPU 推理。\n";
            }

            const std::string model_path = "yolo26n-pose.onnx";
#ifdef _WIN32
            std::wstring model_path_w(model_path.begin(), model_path.end());
            m_ortSession = std::make_unique<Ort::Session>(*m_ortEnv, model_path_w.c_str(), session_options);
#else
            m_ortSession = std::make_unique<Ort::Session>(*m_ortEnv, model_path.c_str(), session_options);
#endif
            auto input_name_ptr = m_ortSession->GetInputNameAllocated(0, m_ortAllocator);
            auto output_name_ptr = m_ortSession->GetOutputNameAllocated(0, m_ortAllocator);
            m_inputName = input_name_ptr.get();
            m_outputName = output_name_ptr.get();
            m_yoloInitialized = true;
            std::cout << "YOLO26-Pose ONNX 引擎初始化成功!\n";
        } catch (const std::exception& e) {
            std::cerr << "YOLO26-Pose 加载失败: " << e.what() << "\n";
            m_yoloInitialized = false;
        }
    }

    bool CopyTextureToMat(ID3D11Texture2D* srcTexture, int width, int height, cv::Mat& outMat) {
        D3D11_TEXTURE2D_DESC srcDesc;
        srcTexture->GetDesc(&srcDesc);

        bool recreate = false;
        if (!m_stagingTexture) {
            recreate = true;
        } else {
            D3D11_TEXTURE2D_DESC stageDesc;
            m_stagingTexture->GetDesc(&stageDesc);
            if (stageDesc.Width != srcDesc.Width || stageDesc.Height != srcDesc.Height) {
                recreate = true;
            }
        }

        if (recreate) {
            D3D11_TEXTURE2D_DESC desc = srcDesc;
            desc.BindFlags = 0;
            desc.MiscFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.Usage = D3D11_USAGE_STAGING;
            HRESULT hr = m_d3dDevice->CreateTexture2D(&desc, nullptr, m_stagingTexture.put());
            if (FAILED(hr)) return false;
        }

        m_d3dContext->CopyResource(m_stagingTexture.get(), srcTexture);

        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = m_d3dContext->Map(m_stagingTexture.get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) return false;

        cv::Mat bgraMat(srcDesc.Height, srcDesc.Width, CV_8UC4, mapped.pData, mapped.RowPitch);
        
        cv::Rect _roi(0, 0, width, height);
        bgraMat(_roi).copyTo(outMat);

        m_d3dContext->Unmap(m_stagingTexture.get(), 0);
        return true;
    }

    mtd::point2i roi{640, 640};
    mtd::point2i roi_change{-1, -1};

    std::vector<PoseDetection> ProcessYOLO(const cv::Mat &src_img) {
        std::vector<PoseDetection> detections;
        if (!m_yoloInitialized || src_img.empty()) return detections;

        int img_w = src_img.cols;
        int img_h = src_img.rows;
        
        // 1. 计算中心 roi_w * roi_h 区域的裁剪偏移量（以屏幕/游戏中心为锚点）
        auto [_roi_w, _roi_h] = roi;

        int crop_x = std::max(0, (img_w - _roi_w) / 2);
        int crop_y = std::max(0, (img_h - _roi_h) / 2);
        
        // 边界安全防护，防止低分辨率下越界
        _roi_w = std::min(_roi_w, img_w - crop_x);
        _roi_h = std::min(_roi_h, img_h - crop_y);

        // 2. 🎯 直接裁剪出中心画面，无需 Letterbox
        cv::Rect roi_rect(crop_x, crop_y, _roi_w, _roi_h);
        cv::Mat cropped_img = src_img(roi_rect);

        // 3. 将 1:1 裁剪的高清画面打包成 Tensor
        // 🎯 优化：当尺寸本就是 640*640 时（最普遍情况），Size() 传入空，防止 OpenCV 内部进行多余的 Resize
        cv::Size blob_size = (cropped_img.cols == 640 && cropped_img.rows == 640) ? cv::Size() : cv::Size(640, 640);
        cv::Mat blob = cv::dnn::blobFromImage(cropped_img, 1.0 / 255.0, blob_size, cv::Scalar(0, 0, 0), true, false, CV_32F);

        std::vector<int64_t> input_shape = {1, 3, 640, 640};
        Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        
        try {
            Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
                memory_info, 
                (float*)blob.data, 
                blob.total() * blob.channels(), 
                input_shape.data(), 
                input_shape.size()
            );

            const char* input_name = m_inputName.c_str();
            const char* output_name = m_outputName.c_str();

            auto output_tensors = m_ortSession->Run(
                Ort::RunOptions{nullptr}, 
                &input_name, 
                &input_tensor, 
                1, 
                &output_name, 
                1
            );

            float* raw_output = output_tensors[0].GetTensorMutableData<float>();
            auto output_shape = output_tensors[0].GetTensorTypeAndShapeInfo().GetShape();
            int num_predictions = output_shape[1]; // 300
            int num_elements = output_shape[2];    // 57

            // 计算缩放比（以防在极端低分辨率屏幕上 roi 被迫缩小）
            float scale_x = (float)cropped_img.cols / 640.0f;
            float scale_y = (float)cropped_img.rows / 640.0f;

            for (int i = 0; i < num_predictions; ++i) {
                float* data_ptr = raw_output + i * num_elements;
                float x1 = data_ptr[0];
                float y1 = data_ptr[1];
                float x2 = data_ptr[2];
                float y2 = data_ptr[3];
                float confidence = data_ptr[4];
                int class_id = (int)data_ptr[5];

                if (confidence < 0.5f) { 
                    continue;
                }

                // 🎯 还原坐标：先还原到裁剪区比例，然后加上 X 和 Y 的偏移量还原到整个屏幕
                float orig_x1 = x1 * scale_x + crop_x;
                float orig_y1 = y1 * scale_y + crop_y;
                float orig_x2 = x2 * scale_x + crop_x;
                float orig_y2 = y2 * scale_y + crop_y;

                PoseDetection det;
                det.box = cv::Rect2f(orig_x1, orig_y1, orig_x2 - orig_x1, orig_y2 - orig_y1);
                det.score = confidence;
                det.classId = class_id;

                det.keypoints.resize(17);
                for (int k = 0; k < 17; ++k) {
                    float kpt_x = data_ptr[6 + k * 3];
                    float kpt_y = data_ptr[6 + k * 3 + 1];
                    float kpt_conf = data_ptr[6 + k * 3 + 2];
                    
                    // 同理，加上中心偏移量
                    det.keypoints[k].x = kpt_x * scale_x + crop_x;
                    det.keypoints[k].y = kpt_y * scale_y + crop_y;
                    det.keypoints[k].confidence = kpt_conf;
                }
                detections.push_back(det);
            }
        } catch (const std::exception& e) {
            std::cerr << "Inference error: " << e.what() << "\n";
        }

        return detections;
    }

    struct Point2D {
        float x = 0.0f;
        float y = 0.0f;
        bool valid = false;
    };

    // 🎯 计算头部的物理中心点（带背向遮挡自动兜底）
    Point2D GetHeadCenter(const PoseDetection& det, float confidence_threshold = 0.5f) {
        float sum_x = 0.0f;
        float sum_y = 0.0f;
        int count = 0;

        // COCO 头部关键点索引: 0(鼻), 1(左眼), 2(右眼), 3(左耳), 4(右耳)
        for (int i = 0; i <= 4; ++i) {
            if (det.keypoints[i].confidence > confidence_threshold) {
                sum_x += det.keypoints[i].x;
                sum_y += det.keypoints[i].y;
                count++;
            }
        }

        Point2D head;
        if (count > 0) {
            // 1. 如果有可见的关键点，求均值作为头部中心点
            head.x = sum_x / count;
            head.y = sum_y / count;
            head.valid = true;
        } else {
            // 2. 兜底方案：如果全部遮挡（如背对镜头），使用检测框顶部向下 12% 处作为近似头部
            head.x = det.box.x + det.box.width / 2.0f;
            head.y = det.box.y + det.box.height * 0.14f; 
            head.valid = true;
        }
        return head;
    }

    void StartCapture() {
        auto factory = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(
            factory->CreateForWindow(m_targetHwnd, winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(), winrt::put_abi(m_item))
        );

        auto size = m_item.Size();

        m_framePool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
            m_winrtDevice, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        
        m_frameArrivedToken = m_framePool.FrameArrived({ this, &WindowOverlay::OnFrameArrived });
        
        m_session = m_framePool.CreateCaptureSession(m_item);
        m_session.StartCapture();
    }

    void StopCapture() {
        if (m_session) m_session.Close();
        if (m_framePool) {
            m_framePool.FrameArrived(m_frameArrivedToken);
            m_framePool.Close();
        }
    }

    // 🎯 核心优化 2：解决 WGC 帧池积压引起的延迟积累问题
    void OnFrameArrived(winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender, 
        winrt::Windows::Foundation::IInspectable const&) {
        
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame latestFrame{ nullptr };

        // 使用快速 While 循环，直接“榨干”丢弃当前缓冲区里的所有历史积压旧帧，只保留最新的一帧
        // 这能瞬间解决因游戏大作的高 FPS 导致的捕获队列积压滞后
        while (auto frame = sender.TryGetNextFrame()) {
            latestFrame = frame;
        }

        if (!latestFrame) return;

        auto contentSize = latestFrame.ContentSize();

        auto access = latestFrame.Surface().as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> frameTexture;
        winrt::check_hresult(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), frameTexture.put_void()));

        std::lock_guard<std::mutex> lock(m_mutex);
        m_pendingTexture = frameTexture;
        m_pendingWidth = contentSize.Width;
        m_pendingHeight = contentSize.Height;
    }

    // 🎯 优化：运行 OpenCV 预处理和 YOLO ONNX 推理的后台工作线程循环
    void WorkerThreadLoop() {
        while (m_workerRunning) {
            cv::Mat frame;
            int width = 0, height = 0;

            {
                std::unique_lock<std::mutex> lock(m_workerMutex);
                m_workerCv.wait(lock, [this]() { return !m_workerRunning || m_hasNewFrame; });
                if (!m_workerRunning) break;

                frame = std::move(m_frameToProcess);
                width = m_processWidth;
                height = m_processHeight;
                m_hasNewFrame = false;
            }

            if (!frame.empty()) {
                auto detections = ProcessYOLO(frame);

                // 更新共享结果缓冲区
                {
                    std::lock_guard<std::mutex> lock(m_detectionsMutex);
                    m_sharedDetections = std::move(detections);
                    m_sharedWidth = width;
                    m_sharedHeight = height;
                }
            }
        }
    }

    void InputThreadLoop() {
        m_interceptionContext = interception_create_context();
        if (!m_interceptionContext) {
            std::cerr << "[Interception] 无法启动驱动级键盘监听，请确保您已在系统正确安装了 Interception 驱动程序！\n";
            return;
        }

        // 设置按键过滤器（只监听键盘按下事件，防止按键抬起时二次触发）
        interception_set_filter(m_interceptionContext, interception_is_keyboard, INTERCEPTION_FILTER_KEY_ALL);

        InterceptionDevice device;
        InterceptionStroke stroke;

        std::cout << "[Interception] 驱动级按键监听成功运行。按下 F8 键可切换 YOLO 裁剪识别区显示状态。\n";

        while (m_inputRunning && (device = interception_wait(m_interceptionContext)) > 0) {
            interception_receive(m_interceptionContext, device, &stroke, 1);
            interception_send(m_interceptionContext, device, &stroke, 1);
            if (interception_is_keyboard(device)) {
                InterceptionKeyStroke &kstroke = *(InterceptionKeyStroke*)&stroke;
                
                if (kstroke.code == SCANCODE_F8 && kstroke.state == INTERCEPTION_KEY_DOWN) {
                    m_showCropZone = !m_showCropZone.load(); // 切换状态
                    std::cout << "[Interception] 检测到 F8 键按下，当前 YOLO 识别区方框显示状态: " 
                                << (m_showCropZone.load() ? "开启" : "关闭") << "\n";
                }
                if (kstroke.code == SCANCODE_F9 && kstroke.state == INTERCEPTION_KEY_DOWN) {
                    if (roi.x != 640) {
                        roi_change = {640, 640};
                    } else if (roi.x != 320) {
                        roi_change = {320, 320};
                    }
                }
                if (kstroke.code == SCANCODE_F10 && kstroke.state == INTERCEPTION_KEY_DOWN) {
                    is_chasing = !is_chasing;
                    std::cout << is_chasing << "\n";
                }
            }

        }
    }

    void get_wh(int &offsetX, int &offsetY, int &clientW, int &clientH) {
        offsetX =offsetY = 0, clientW = clientH = 1;
        if (IsWindow(m_targetHwnd)) {
            RECT safeWindowRect;
            HRESULT hr = DwmGetWindowAttribute(m_targetHwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &safeWindowRect, sizeof(safeWindowRect));

            POINT clientTopLeft = { 0, 0 };
            ClientToScreen(m_targetHwnd, &clientTopLeft); // 游戏内容区的左上角屏幕坐标

            RECT clientRect;
            GetClientRect(m_targetHwnd, &clientRect);
            clientW = clientRect.right - clientRect.left;
            clientH = clientRect.bottom - clientRect.top;

            if (SUCCEEDED(hr)) {
                // 用真实的物理可视边界计算偏差，完美消除 7~8 像素的偏左误差
                offsetX = clientTopLeft.x - safeWindowRect.left;
                offsetY = clientTopLeft.y - safeWindowRect.top;
            } else {
                // 备用方案（如果 DWM 读取失败，退回到普通获取）
                RECT windowRect;
                GetWindowRect(m_targetHwnd, &windowRect);
                offsetX = clientTopLeft.x - windowRect.left;
                offsetY = clientTopLeft.y - windowRect.top;
            }
        }
    }

    // 主线程轻量化绘制渲染
    void RenderFrame() {
        if (!m_rtv) return;

        winrt::com_ptr<ID3D11Texture2D> newTexture; 
        int updateWidth = 0, updateHeight = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_pendingTexture) {
                newTexture = m_pendingTexture;
                m_pendingTexture = nullptr;
                updateWidth = m_pendingWidth;
                updateHeight = m_pendingHeight;
            }
        }

        // 极速 GPU -> CPU 内存提取
        if (newTexture) {
            bool worker_busy = false;
            {
                std::lock_guard<std::mutex> lock(m_workerMutex);
                worker_busy = m_hasNewFrame; // 如果 m_hasNewFrame 为 true，说明上一帧还没处理完
            }

            if (newTexture && !worker_busy) { // 只有后台空闲时，主线程才进行 GPU->CPU 拷贝
                cv::Mat cpu_frame;
                if (CopyTextureToMat(newTexture.get(), updateWidth, updateHeight, cpu_frame)) {
                    {
                        std::lock_guard<std::mutex> lock(m_workerMutex);
                        m_frameToProcess = std::move(cpu_frame);
                        m_processWidth = updateWidth;
                        m_processHeight = updateHeight;
                        m_hasNewFrame = true;
                    }
                    m_workerCv.notify_one();
                }
            }
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImDrawList* drawList = ImGui::GetBackgroundDrawList();

        ImVec2 display_size = ImGui::GetIO().DisplaySize;
        float overlayW = display_size.x;
        float overlayH = display_size.y;

        // 1. 读取最新的检测数据
        std::vector<PoseDetection> currentDetections;
        int currentWidth = 0, currentHeight = 0;
        {
            std::lock_guard<std::mutex> lock(m_detectionsMutex);
            currentDetections = m_sharedDetections;
            currentWidth = m_sharedWidth;
            currentHeight = m_sharedHeight;
        }

        // 2. 🎯 计算目标窗口的 整个窗口区 (Window) 与 客户游戏画面区 (Client) 之间的像素偏差
        int offsetX, offsetY, clientW, clientH;
        get_wh(offsetX, offsetY, clientW, clientH);

        if (m_showCropZone.load() && currentWidth > 0 && currentHeight > 0 && clientW > 0 && clientH > 0) {
            ImU32 boxColor = IM_COL32(0, 255, 0, 255);
            ImU32 kptColor = IM_COL32(255, 0, 0, 255);
            ImU32 lineColor = IM_COL32(255, 255, 255, 220);

            const float kpt_threshold = 0.5f;

            for (const auto& det : currentDetections) {
                // 将 YOLO 坐标减去偏差值，还原为纯游戏画面内部的坐标
                float c_x1 = det.box.x - offsetX;
                float c_y1 = det.box.y - offsetY;
                float c_w = det.box.width;
                float c_h = det.box.height;

                // 比例映射到覆层
                ImVec2 p1 = ImVec2((c_x1 / clientW) * overlayW, (c_y1 / clientH) * overlayH);
                ImVec2 p2 = ImVec2(((c_x1 + c_w) / clientW) * overlayW, ((c_y1 + c_h) / clientH) * overlayH);

                drawList->AddRect(p1, p2, boxColor, 0.0f, 0, 2.0f);

                std::string label = "Person: " + std::to_string(det.score).substr(0, 4);
                drawList->AddText(ImVec2(p1.x, p1.y - 15.0f), boxColor, label.c_str());

                // 绘制骨骼连接线
                for (const auto& conn : SKELETON_CONNECTIONS) {
                    const auto& kpt1 = det.keypoints[conn.first];
                    const auto& kpt2 = det.keypoints[conn.second];

                    if (kpt1.confidence > kpt_threshold && kpt2.confidence > kpt_threshold) {
                        float kpt1_cx = kpt1.x - offsetX;
                        float kpt1_cy = kpt1.y - offsetY;
                        float kpt2_cx = kpt2.x - offsetX;
                        float kpt2_cy = kpt2.y - offsetY;

                        ImVec2 pt1 = ImVec2((kpt1_cx / clientW) * overlayW, (kpt1_cy / clientH) * overlayH);
                        ImVec2 pt2 = ImVec2((kpt2_cx / clientW) * overlayW, (kpt2_cy / clientH) * overlayH);
                        drawList->AddLine(pt1, pt2, lineColor, 2.0f);
                    }
                }

                // 绘制关键点圆点
                for (size_t i = 0; i < det.keypoints.size(); ++i) {
                    const auto& kpt = det.keypoints[i];
                    if (kpt.confidence > kpt_threshold) {
                        float kpt_cx = kpt.x - offsetX;
                        float kpt_cy = kpt.y - offsetY;

                        ImVec2 pt = ImVec2((kpt_cx / clientW) * overlayW, (kpt_cy / clientH) * overlayH);
                        drawList->AddCircleFilled(pt, 4.0f, kptColor);
                    }
                }

                Point2D head = GetHeadCenter(det, 0.5f);

                if (head.valid && clientW > 1 && clientH > 1) {
                    // a. 将头部坐标转换至游戏画面（客户区）绝对像素坐标
                    float head_cx = head.x - offsetX;
                    float head_cy = head.y - offsetY;

                    // b. 计算游戏画面的中心点（您的游戏准星位置）
                    float centerX = clientW / 2.0f;
                    float centerY = clientH / 2.0f;

                    // c. 计算相对于中心的相对像素位移量 (dx, dy)
                    float dx = head_cx - centerX;
                    float dy = head_cy - centerY;

                    // 🎯 此时 (dx, dy) 就是您所需要的“相对于准星的相对坐标”！
                    
                    // (可选测试)：在 Overlay 界面上绘制一条从游戏准星(中心)指向人物头部的指示线和文字
                    ImVec2 scr_center = ImVec2(overlayW / 2.0f, overlayH / 2.0f);
                    ImVec2 scr_head = ImVec2((head_cx / clientW) * overlayW, (head_cy / clientH) * overlayH);
                    
                    // 画一条明黄色的线连向头部
                    drawList->AddLine(scr_center, scr_head, IM_COL32(255, 235, 59, 200), 1.5f);

                    // 实时在头上显示相对像素距离
                    std::string offset_text = "Offset: (" + std::to_string((int)dx) + ", " + std::to_string((int)dy) + ")";
                    drawList->AddText(ImVec2(scr_head.x - 40.0f, scr_head.y - 30.0f), IM_COL32(255, 235, 59, 255), offset_text.c_str());
                }
            
            }
        }

        if (m_showCropZone.load() && clientW > 1 && clientH > 1) {
            auto [_roi_w, _roi_h] = roi;

            float crop_x = std::max(0.0f, (clientW - _roi_w) / 2.0f);
            float crop_y = std::max(0.0f, (clientH - _roi_h) / 2.0f);
            float crop_w = std::min(_roi_w, clientW);
            float crop_h = std::min(_roi_h, clientH);

            ImVec2 cp1 = ImVec2((crop_x / clientW) * overlayW, (crop_y / clientH) * overlayH);
            ImVec2 cp2 = ImVec2(((crop_x + crop_w) / clientW) * overlayW, ((crop_y + crop_h) / clientH) * overlayH);

            // 绘制黄色半透明边框，让用户直观地看到 640x640 ROI 工作视野
            drawList->AddRect(cp1, cp2, IM_COL32(255, 235, 59, 130), 0.0f, 0, 1.5f);

            std::ostringstream os;
            os << "[YOLO Active Region (" << _roi_w << " * " << _roi_h << ")]";
            drawList->AddText(ImVec2(cp1.x + 6.0f, cp1.y + 6.0f), IM_COL32(255, 235, 59, 180), os.str().c_str());
        }

        // 提交渲染
        ImGui::Render();
        ID3D11RenderTargetView* rtvList[] = { m_rtv.get() };
        m_d3dContext->OMSetRenderTargets(1, rtvList, nullptr);
        
        const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        m_d3dContext->ClearRenderTargetView(m_rtv.get(), clearColor);
        
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        m_swapChain->Present(1, 0);
    }

private:
    HWND m_targetHwnd = nullptr;
    HWND m_viewerHwnd = nullptr;
    std::mutex m_mutex;

    // Direct3D 11 资源
    winrt::com_ptr<ID3D11Device> m_d3dDevice;
    winrt::com_ptr<ID3D11DeviceContext> m_d3dContext;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_winrtDevice{ nullptr };
    winrt::com_ptr<IDXGISwapChain1> m_swapChain;
    winrt::com_ptr<ID3D11RenderTargetView> m_rtv;

    // 后台推理线程与主渲染线程图像缓存
    winrt::com_ptr<ID3D11Texture2D> m_pendingTexture;
    winrt::com_ptr<ID3D11Texture2D> m_stagingTexture;

    // 捕获相关资源
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem m_item{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_framePool{ nullptr };
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession m_session{ nullptr };
    winrt::event_token m_frameArrivedToken;

    int m_pendingWidth = 0;
    int m_pendingHeight = 0;
    int m_currentWidth = 0;
    int m_currentHeight = 0;

    // YOLO26-Pose 成员
    std::unique_ptr<Ort::Env> m_ortEnv;
    std::unique_ptr<Ort::Session> m_ortSession;
    Ort::AllocatorWithDefaultOptions m_ortAllocator;
    std::string m_inputName;
    std::string m_outputName;
    bool m_yoloInitialized = false;

    // 优化：异步推理工作线程控制变量
    std::thread m_workerThread;
    std::mutex m_workerMutex;
    std::condition_variable m_workerCv;
    bool m_workerRunning = false;
    cv::Mat m_frameToProcess;
    int m_processWidth = 0;
    int m_processHeight = 0;
    bool m_hasNewFrame = false;

    // 🎯 新增：Interception 按键监听线程管理变量
    std::thread m_inputThread;
    InterceptionContext m_interceptionContext = nullptr;
    std::atomic<bool> m_inputRunning{ false };
    std::atomic<bool> m_showCropZone{ true }; // 默认为开启状态，控制 黄色检测区域的绘制

    // 优化：线程安全的结果共享缓冲区
    std::vector<PoseDetection> m_sharedDetections;
    std::mutex m_detectionsMutex;
    int m_sharedWidth = 0;
    int m_sharedHeight = 0;
};

// ==========================================
// 模块 3: 主入口
// ==========================================
int main() {
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    try {
        std::vector<std::string> player_keywords = {
            "电影和电视",       
            "Media Player",     
            "PotPlayer",        
            "VLC media player", 
            "ActiveMovie Window",
            "媒体播放器",
            "三角洲行动"
        };

        HWND targetHwnd = Utils::FindWindowByTitleKeywords(player_keywords);
        if (!targetHwnd) {
            std::cerr << "未检测到支持的视频播放器或游戏运行，请先运行播放器或三角洲行动！\n";
            return 1;
        }

        char window_title[256];
        GetWindowTextA(targetHwnd, window_title, sizeof(window_title));
        std::cout << "目标锁定窗口: \"" << window_title << "\" (句柄: " << targetHwnd << ")\n";
        
        WindowOverlay overlay(targetHwnd);
        overlay.Start();

    } catch (winrt::hresult_error const& ex) {
        std::cerr << "WinRT 异常: " << winrt::to_string(ex.message()) << std::endl;
    } catch (std::exception const& ex) {
        std::cerr << "标准异常: " << ex.what() << std::endl;
    }
    
    return 0;
}