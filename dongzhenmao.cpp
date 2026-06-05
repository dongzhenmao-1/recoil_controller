#include <iostream>
#include <interception.h>

// 定义 ESC 键的扫描码 (Scan Code)
#define SCANCODE_ESC 0x01

int main() {
    // 1. 创建 Interception 上下文
    InterceptionContext context = interception_create_context();
    if (!context) {
        std::cerr << "[ERROR] 无法创建 Interception 上下文。请确保已安装驱动并以管理员权限运行！" << std::endl;
        return 1;
    }

    // 2. 设置过滤器
    // 仅拦截键盘的“按下”事件（用来捕获 ESC 退出）
    interception_set_filter(context, interception_is_keyboard, INTERCEPTION_FILTER_KEY_DOWN);
    // 仅拦截鼠标的“左键按下”事件（降低不必要的事件处理开销）
    interception_set_filter(context, interception_is_mouse, INTERCEPTION_FILTER_MOUSE_LEFT_BUTTON_DOWN);

    std::cout << "=========================================" << std::endl;
    std::cout << "  Interception 鼠标平移 Demo 已启动" << std::endl;
    std::cout << "=========================================" << std::endl;
    std::cout << "  * 操作：按下鼠标左键，鼠标将向左平移 10 个单位。" << std::endl;
    std::cout << "  * 退出：在任意界面按下 [ESC] 键退出程序。" << std::endl;
    std::cout << "=========================================" << std::endl;

    InterceptionDevice device;
    InterceptionStroke stroke;

    // 3. 事件循环
    while (interception_receive(context, device = interception_wait(context), &stroke, 1) > 0) {
        
        // 处理键盘事件
        if (interception_is_keyboard(device)) {
            InterceptionKeyStroke &kstroke = *(InterceptionKeyStroke *)&stroke;
            
            // 如果按下的是 ESC 键，跳出循环退出
            if (kstroke.code == SCANCODE_ESC) {
                std::cout << "[INFO] 检测到 ESC 按下，程序正在退出..." << std::endl;
                // 放行 ESC 键，避免系统吃掉这个按键
                interception_send(context, device, &stroke, 1);
                break;
            }
            
            // 默认放行其他键盘事件
            interception_send(context, device, &stroke, 1);
        }
        
        // 处理鼠标事件
        else if (interception_is_mouse(device)) {
            InterceptionMouseStroke &mstroke = *(InterceptionMouseStroke *)&stroke;

            // 如果是鼠标左键按下
            if (mstroke.state & INTERCEPTION_MOUSE_LEFT_BUTTON_DOWN) {
                // a. 先把原始的“左键按下”事件发送出去（保证点击依然有效）
                interception_send(context, device, &stroke, 1);

                // b. 构造一个相对移动事件：向左移动 10 个单位
                InterceptionMouseStroke moveStroke = {};
                moveStroke.flags = INTERCEPTION_MOUSE_MOVE_RELATIVE; // 0 表示相对移动
                moveStroke.x = -10;                             // X 轴负方向代表向左
                moveStroke.y = 0;                               // Y 轴不变
                moveStroke.state = 0;                           // 此时不需要携带按键状态

                // c. 发送移动信号
                interception_send(context, device, (InterceptionStroke *)&moveStroke, 1);
            } 
            else {
                // 放行其他鼠标事件
                interception_send(context, device, &stroke, 1);
            }
        }
    }

    // 4. 销毁上下文，释放资源
    interception_destroy_context(context);
    std::cout << "[OK] 资源已释放，程序安全退出。" << std::endl;
    return 0;
}