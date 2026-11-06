#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <iostream>
#include <vector>

int main() {
    std::vector<unsigned char> buffer(2 * 16, 0);
    cv::Mat view(2, 4, CV_8UC3, buffer.data(), 16);
    cv::Mat alias = view;
    cv::Mat owned = view.clone();
    view.at<cv::Vec3b>(0, 0) = cv::Vec3b(10, 20, 30);
    std::cout << "shared=" << (view.data == alias.data)
              << " cloned=" << (view.data != owned.data)
              << " step=" << view.step[0]
              << " continuous=" << view.isContinuous() << '\n';
    std::cout << "alias=" << alias.at<cv::Vec3b>(0, 0)
              << " owned=" << owned.at<cv::Vec3b>(0, 0) << '\n';

    cv::Mat raw(8, 8, CV_8UC1);
    for (int y = 0; y < raw.rows; ++y)
        for (int x = 0; x < raw.cols; ++x)
            raw.at<unsigned char>(y, x) = y % 2 == 0
                ? (x % 2 == 0 ? 200 : 100) : (x % 2 == 0 ? 100 : 20);
    cv::Mat bgr;
    cv::cvtColor(raw, bgr, cv::COLOR_BayerRGGB2BGR);
    std::cout << "RGGB to BGR=" << bgr.at<cv::Vec3b>(3, 3) << '\n';
    return 0;
}
