#include <opencv2/opencv.hpp>
#include <opencv2/core/utils/logger.hpp>
#include <iostream>
#include <vector>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <fstream>

namespace cv {
    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(cv::Point2d, x, y)
}

int main() {
    // 屏蔽 INFO 日志
    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_ERROR);

    std::string imagePath = "recoil.png"; // 请确保文件名正确
    cv::Mat img = cv::imread(imagePath);
    if (img.empty()) {
        std::cerr << "无法读取图片，请检查路径！" << "\n";
        return -1;
    }

    // 1. 转换到 HSV 并提取绿色 (使用你测试完美的阈值)
    cv::Mat hsv, mask;
    cv::cvtColor(img, hsv, cv::COLOR_BGR2HSV);
    cv::Scalar lower_green(30, 15, 15);   
    cv::Scalar upper_green(90, 255, 255);
    cv::inRange(hsv, lower_green, upper_green, mask);

    // 2. 🟢 核心：进行距离变换 (Distance Transform)
    // 每一个白色像素的值，都会被计算为它距离黑色背景的像素距离
    cv::Mat dist;
    cv::distanceTransform(mask, dist, cv::DIST_L2, 3);

    // 3. 🟢 寻找局部最大值 (Local Maxima) —— 即寻找“山峰”
    // 使用 3x3 膨胀滤波。只有身为局部最大值的像素，膨胀（找周围最大值）前后的数值才会保持相等
    cv::Mat dist_dilate;
    cv::dilate(dist, dist_dilate, cv::Mat()); 

    // 比较原图和膨胀后的图，相同的地方就是“山峰”（圆心）
    cv::Mat peak_mask;
    cv::compare(dist, dist_dilate, peak_mask, cv::CMP_EQ);

    // 4. 过滤噪点：只保留距离背景 > 1.5 像素的峰值，防止极微小的单像素噪点干扰
    cv::Mat thresh_32f, thresh_8u;
    cv::threshold(dist, thresh_32f, 1.5, 255, cv::THRESH_BINARY);
    thresh_32f.convertTo(thresh_8u, CV_8U);
    
    // 峰值掩膜与有效区域求交集，得到最终极度精准的圆心点掩膜
    cv::bitwise_and(peak_mask, thresh_8u, peak_mask);

    // 5. 提取这些峰值（圆心）的坐标
    std::vector<std::vector<cv::Point>> peak_contours;
    cv::findContours(peak_mask, peak_contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    std::vector<cv::Point2d> dots;
    for (const auto& contour : peak_contours) {
        cv::Moments m = cv::moments(contour);
        if (m.m00 > 0) {
            // 计算峰值区域的重心，作为高精度圆心
            dots.push_back(cv::Point2d(m.m10 / m.m00, m.m01 / m.m00));
        } else if (!contour.empty()) {
            // 边缘保护：如果峰值只有一个像素大小，直接用它的位置
            dots.push_back(contour[0]);
        }
    }

    if (dots.empty()) {
        std::cout << "未检测到任何有效点！" << std::endl;
        return 0;
    }

    // 6. 空间排序
    std::sort(dots.begin(), dots.end(), [](const cv::Point2d& a, const cv::Point2d& b) {
        return a.y > b.y;
    });

    // 7. 相对坐标计算与结果绘制
    cv::Point2d origin = dots[0];
    cv::Mat result = img.clone();
    
    std::cout << "提取成功！共识别出 " << dots.size() << " 个绿点。" << "\n";

    auto get_2f = [](double x) -> double {
        return int(x * 100) / 100.0;
    };

    for (int i = 0; i < dots.size(); i++) {
        double x = get_2f(dots[i].x - origin.x);
        double y = get_2f(dots[i].y - origin.y); 
        std::cout << "点 [" << i << "] : dx = " << x << ",\t dy = " << y << "\n";
        
        // 用红色实心圆圈绘制圆心
        cv::circle(result, dots[i], 3, cv::Scalar(0, 0, 255), -1);
        // 绘制编号
        cv::putText(result, std::to_string(i), dots[i] + cv::Point2d(10, 4),
                cv::FONT_HERSHEY_SIMPLEX, 0.4, cv::Scalar(0, 0, 255), 1);

        dots[i] = cv::Point2d(x, y);
    }

    // std::vector<cv::Point2d> recoil;
    // for (int i = 1; i < dots.size(); ++i) {
    //     recoil.push_back(dots[i] - dots[i - 1]);
    // }

    nlohmann::json json = dots;
    std::ofstream fout("out.json");
    fout << json.dump(4);
    fout.close();

    cv::imshow("3. Final Result", result);
    cv::waitKey(0);



    

    return 0;
}