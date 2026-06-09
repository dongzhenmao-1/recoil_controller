#include "scancode.hpp"
#include "my_algorithm.hpp"

#include <iostream>
#include <fstream>
#include <filesystem>
#include <thread>
#include <mutex>
#include <chrono>
#include <atomic>

#include <interception.h>
#include <nlohmann/json.hpp>

std::string name;
double horizontal_ratio, vertical_ratio, fire_rate;
std::vector<mtd::point2i> spray_pattern;

std::chrono::milliseconds delay, fire_cycle;

std::vector<mtd::point2i> trans_spray_pattern(std::vector<mtd::point2d> a) {
    std::vector<mtd::point2i> b;
    for (mtd::point2d x : a) {
        b.push_back(mtd::point2i({x.x * horizontal_ratio, x.y * vertical_ratio}));
    }
    return b;
}

void load(std::string path = "data.json") {
    std::ifstream fin(path);
    nlohmann::json j = nlohmann::json::parse(fin);
    // std::cout << j.dump(4);
    
    name = j["name"];
    delay = std::chrono::milliseconds(int(j["delay"] * 1000));
    fire_rate = j["fire_rate"];
    fire_cycle = std::chrono::milliseconds(int(1000 / fire_rate));

    horizontal_ratio = j["horizontal_ratio"], vertical_ratio = j["vertical_ratio"], 
    spray_pattern = trans_spray_pattern(j["spray_pattern"].get<std::vector<mtd::point2d>>());

    std::cout << j.dump(4) << "\n";
}

InterceptionContext context = interception_create_context();
InterceptionDevice mouse_device = -1;
InterceptionDevice keyboard_device = -1;

void init_filters() {
    interception_set_filter(context, interception_is_keyboard, INTERCEPTION_FILTER_KEY_ALL);
    interception_set_filter(context, interception_is_mouse, 
        INTERCEPTION_FILTER_MOUSE_LEFT_BUTTON_DOWN | INTERCEPTION_FILTER_MOUSE_LEFT_BUTTON_UP |
        INTERCEPTION_FILTER_MOUSE_RIGHT_BUTTON_DOWN | INTERCEPTION_FILTER_MOUSE_RIGHT_BUTTON_UP);
}

void get_device() {
    // InterceptionStroke stroke;
    // InterceptionDevice device;
    
    // while (mouse_device == -1 || keyboard_device == -1) {
    //     interception_receive(context, device = interception_wait(context), &stroke, 1);
    //     if (interception_is_keyboard(device)) {
    //         keyboard_device = device;
    //     } else if (interception_is_mouse(device)) {
    //         mouse_device = device;
    //     }
    //     interception_send(context, device, &stroke, 1);
    // }

    mouse_device = 12;
    keyboard_device = 4;

    std::cout << "I have got codes of this computer~" << "\n";
    std::cout << "mouse_device : " << mouse_device << "\n";
    std::cout << "keyboard_device : " << keyboard_device << "\n";
}


std::atomic<int> is_firing, is_running = 1, is_open = 0;

void recoil_controller() {
    while (is_running) {
        if (is_firing) {
            std::cout << "get in state" << "\n";

            if (delay.count() > 0) std::this_thread::sleep_for(delay);
            mtd::point2i now_pos = {0, 0};
            std::chrono::steady_clock::time_point _start = std::chrono::steady_clock::now();
            std::chrono::milliseconds step_t(20);
            while (is_firing) {
                auto _now = std::chrono::steady_clock::now();
                auto now_t = std::chrono::duration_cast<std::chrono::milliseconds>(_now - _start); 
                std::cout << now_t.count() / 1000 << "\n";
                int now_step = std::ceil(fire_rate * now_t.count() / 1000 + 0.05);
                
                if (now_step >= int(spray_pattern.size())) break;

                mtd::point2i nx_spray_pos = spray_pattern[now_step];
                std::chrono::milliseconds nx_t = fire_cycle * now_step;

                double remaining_time = (nx_t - now_t).count();
                double fraction = (remaining_time > 0.0 ? std::min(1.0, double(step_t.count()) / remaining_time) : 1.0);
                
                mtd::point2i we = mtd::point2d(nx_spray_pos - now_pos) * fraction;
                now_pos = now_pos + we;

                InterceptionMouseStroke move = {};
                move.flags = INTERCEPTION_MOUSE_MOVE_RELATIVE;
                move.x = -we.x, move.y = -we.y;
                interception_send(context, mouse_device, (InterceptionStroke*)&move, 1);
                
                // std::cout << now_step << "\n";
                printf("%3d %3d\n", now_pos.x, now_pos.y);
                std::this_thread::sleep_for(step_t);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

int main() {
    init_filters();
    get_device();

    load();

    std::thread function(recoil_controller);

    while (is_running) {
        InterceptionStroke stroke;
        InterceptionDevice device;
        interception_receive(context, device = interception_wait(context), &stroke, 1);
        interception_send(context, device, &stroke, 1);
        
        if (interception_is_keyboard(device)) {
            InterceptionKeyStroke &kstroke = *(InterceptionKeyStroke*)&stroke;
            if (kstroke.code == SCANCODE_F7) {
                std::cout << "F7? ok, I will exit" << "\n";
                is_running = false;
            }
            if (kstroke.code == SCANCODE_F8 && (kstroke.state == INTERCEPTION_KEY_DOWN)) {
                is_open ^= 1;
                std::cout << (is_open ? "I turn on it ~" : "I turn off it ~") << "\n";
            }
            if (kstroke.code == SCANCODE_F9 && (kstroke.state == INTERCEPTION_KEY_DOWN)) {
                load();
            }
        } else if (interception_is_mouse(device)) {
            InterceptionMouseStroke &mstroke = *(InterceptionMouseStroke*)&stroke;
            if (mstroke.state & INTERCEPTION_MOUSE_LEFT_BUTTON_DOWN) {
                is_firing = true & is_open;
                std::cout << "firing ! \n";
            } else if (mstroke.state & INTERCEPTION_MOUSE_LEFT_BUTTON_UP) {
                is_firing = false;
                std::cout << "not firing ~ \n";
            } else {
                // do nothing
            }
        }
    }

    if (function.joinable()) function.join();
    
    interception_destroy_context(context);
    


    return 0;
}