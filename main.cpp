#include <MvCameraControl.h>
#include <opencv2/dnn.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <algorithm>
#include <cmath>
#include <vector>
#include <optional>
#include <fstream>
#include <iomanip>
#include <ctime>
#include <limits>
#include <streambuf>
#include <mutex>
// 将程序的 cout/cerr 同时写入终端和日志文件。
class TeeBuffer : public std::streambuf {
    std::streambuf* console;
    std::streambuf* file;
    std::mutex& mutex;
protected:
    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) {
            return traits_type::not_eof(value);
        }
        std::lock_guard<std::mutex> lock(mutex);
        const auto a = console->sputc(traits_type::to_char_type(value));
        const auto b = file->sputc(traits_type::to_char_type(value));
        return traits_type::eq_int_type(a, traits_type::eof()) ||
               traits_type::eq_int_type(b, traits_type::eof()) ? traits_type::eof() : value;
    }
    std::streamsize xsputn(const char* data, std::streamsize size) override {
        std::lock_guard<std::mutex> lock(mutex);
        const auto a = console->sputn(data, size);
        const auto b = file->sputn(data, size);
        return std::min(a, b);
    }
    int sync() override {
        std::lock_guard<std::mutex> lock(mutex);
        const int a = console->pubsync();
        const int b = file->pubsync();
        return a == 0 && b == 0 ? 0 : -1;
    }
public:
    TeeBuffer(std::streambuf* consoleBuffer, std::streambuf* fileBuffer,
              std::mutex& sharedMutex)
        : console(consoleBuffer), file(fileBuffer), mutex(sharedMutex) {}
};

class RunLog {
    std::ofstream file;
    std::mutex mutex;
    std::streambuf* originalOut;
    std::streambuf* originalErr;
    TeeBuffer outBuffer;
    TeeBuffer errBuffer;
    std::filesystem::path logPath;
public:
    RunLog()
        : originalOut(std::cout.rdbuf()), originalErr(std::cerr.rdbuf()),
          outBuffer(originalOut, file.rdbuf(), mutex),
          errBuffer(originalErr, file.rdbuf(), mutex) {
        const auto directory = std::filesystem::path(__FILE__).parent_path() / "logs";
        std::filesystem::create_directories(directory);
        const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        unsigned int sequence = 0;
        do {
            logPath = directory / ("run_" + std::to_string(timestamp) + "_" +
                                    std::to_string(++sequence) + ".log");
        } while (std::filesystem::exists(logPath));
        file.open(logPath);
        if (!file) throw std::runtime_error("Cannot create run log: " + logPath.string());
        // 最后才替换输出缓冲区，保证此前的构造失败不会留下悬空引用。
        std::cout.rdbuf(&outBuffer);
        std::cerr.rdbuf(&errBuffer);
    }
    ~RunLog() {
        try { std::cout.flush(); std::cerr.flush(); } catch (...) {}
        std::cout.rdbuf(originalOut);
        std::cerr.rdbuf(originalErr);
    }
    const std::filesystem::path& path() const { return logPath; }
};

// 未指定的参数保留设备现有设置，虚拟设备无需具备曝光/增益节点。
struct CameraOptions {
    std::string serial = "Vir53692095";
    std::optional<double> exposureUs;
    std::optional<double> gainDb;
    std::optional<std::string> pixelFormat;
    bool headless = false;
    bool enableYolo = true;
    unsigned int frameLimit = 0;
    std::string simulation;
};

void printUsage() {
    std::cout
        << "用法：\n"
        << "  work                                  默认虚拟相机\n"
        << "  work --serial SERIAL                  按序列号选择 USB 相机\n"
        << "  work --serial SERIAL --exposure 1000 --gain 0 --format BayerRG8\n"
        << "  work --video PATH                     测试视频\n"
        << "  work --video PATH --headless          测试视频，不打开窗口\n"
        << "  work --serial SERIAL --frames 300 --headless --no-yolo\n"
        << "性能：--frames N 限定帧数；--headless 不开窗口；--no-yolo 不运行识别。\n"
        << "相机 headless 模式必须指定 --frames。\n"
        << "异常模拟：--simulate timeout 或 --simulate format；在完成 3 帧后注入。\n"
        << "模拟测试若指定 --frames，必须大于 3；模拟结果不能当作真实相机故障实测。\n"
        << "参数：--exposure 单位 us；--gain 单位 dB。\n"
        << "格式：RGB8、BGR8、BayerRG8、Mono8；必须由设备支持。\n"
        << "未指定的曝光、增益、格式保持设备当前设置。\n";
}

double parseNumber(const std::string& text, const std::string& option) {
    std::size_t consumed = 0;
    double value = 0;
    try {
        value = std::stod(text, &consumed);
    } catch (const std::exception&) {
        throw std::invalid_argument("Invalid numeric value for " + option + ": " + text);
    }
    if (consumed != text.size() || !std::isfinite(value)) {
        throw std::invalid_argument("Invalid numeric value for " + option + ": " + text);
    }
    return value;
}

CameraOptions parseCameraOptions(int argc, char* argv[]) {
    CameraOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--headless") { options.headless = true; continue; }
        if (option == "--no-yolo") { options.enableYolo = false; continue; }
        if (option != "--serial" && option != "--exposure" &&
            option != "--gain" && option != "--format" && option != "--frames" && option != "--simulate") {
            throw std::invalid_argument("Unknown camera option: " + option);
        }
        if (i + 1 >= argc || std::string(argv[i + 1]).rfind("--", 0) == 0) {
            throw std::invalid_argument("Missing value for " + option);
        }
        const std::string value = argv[++i];
        if (option == "--serial") {
            if (value.empty()) throw std::invalid_argument("Camera serial cannot be empty");
            options.serial = value;
        } else if (option == "--exposure") {
            const double exposure = parseNumber(value, option);
            if (exposure <= 0) throw std::invalid_argument("Exposure must be greater than zero");
            options.exposureUs = exposure;
        } else if (option == "--gain") {
            options.gainDb = parseNumber(value, option);
        } else if (option == "--simulate") {
            if (value != "timeout" && value != "format") {
                throw std::invalid_argument("--simulate must be timeout or format");
            }
            options.simulation = value;
        } else if (option == "--frames") {
            if (value.empty() || !std::all_of(value.begin(), value.end(),
                [](unsigned char c) { return c >= '0' && c <= '9'; })) {
                throw std::invalid_argument("--frames must be a positive integer");
            }
            const auto count = std::stoull(value);
            if (count == 0 || count > std::numeric_limits<unsigned int>::max()) {
                throw std::invalid_argument("--frames is outside the supported range");
            }
            options.frameLimit = static_cast<unsigned int>(count);
        } else {
            if (value != "RGB8" && value != "BGR8" &&
                value != "BayerRG8" && value != "Mono8") {
                throw std::invalid_argument("Unsupported --format value: " + value);
            }
            options.pixelFormat = value;
        }
    }
    if (options.headless && options.frameLimit == 0) {
        throw std::invalid_argument("Camera --headless requires --frames N");
    }
    if (!options.simulation.empty() && options.frameLimit > 0 && options.frameLimit <= 3) {
        throw std::invalid_argument("Simulation requires --frames greater than 3");
    }
    return options;
}

// 输出 SDK 错误
void printSdkError(const char* operation, int errorCode) {
    std::cerr << "[ERROR] " << operation
              << " failed, code=0x"
              << std::hex
              << static_cast<unsigned int>(errorCode)
              << std::dec << '\n';
}

// 先读取设备允许的范围，再设置，最后回读实际值。
int setAndReadFloat(void* handle, const char* node,
                    double requested, const char* unit) {
    MVCC_FLOATVALUE value{};
    int ret = MV_CC_GetFloatValue(handle, node, &value);
    if (ret != MV_OK) {
        printSdkError(node, ret);
        return ret;
    }
    std::cout << node << " allowed range: [" << value.fMin << ", "
              << value.fMax << "] " << unit << '\n';
    if (!std::isfinite(requested) || requested < value.fMin || requested > value.fMax) {
        std::cerr << "[ERROR] " << node << " requested value " << requested
                  << " is outside the device range\n";
        return -1;
    }
    ret = MV_CC_SetFloatValue(handle, node, static_cast<float>(requested));
    if (ret != MV_OK) {
        printSdkError(node, ret);
        return ret;
    }
    ret = MV_CC_GetFloatValue(handle, node, &value);
    if (ret != MV_OK) {
        printSdkError(node, ret);
        return ret;
    }
    std::cout << node << " requested=" << requested
              << " actual=" << value.fCurValue << " " << unit << '\n';
    return MV_OK;
}

// 在开始采集前应用明确指定的参数；失败由调用者关闭设备并清理。
int configureCamera(void* handle, const CameraOptions& options) {
    if (options.pixelFormat) {
        MvGvspPixelType format = PixelType_Gvsp_RGB8_Packed;
        if (*options.pixelFormat == "BGR8") format = PixelType_Gvsp_BGR8_Packed;
        else if (*options.pixelFormat == "BayerRG8") format = PixelType_Gvsp_BayerRG8;
        else if (*options.pixelFormat == "Mono8") format = PixelType_Gvsp_Mono8;

        int ret = MV_CC_SetEnumValue(handle, "PixelFormat", static_cast<unsigned int>(format));
        if (ret != MV_OK) {
            printSdkError("Set PixelFormat", ret);
            std::cerr << "Requested native camera format was not accepted: "
                      << *options.pixelFormat << '\n';
            return ret;
        }
        MVCC_ENUMVALUE actual{};
        ret = MV_CC_GetEnumValue(handle, "PixelFormat", &actual);
        if (ret != MV_OK) {
            printSdkError("Read PixelFormat", ret);
            return ret;
        }
        std::cout << "Native PixelFormat requested=" << *options.pixelFormat
                  << " actual=0x" << std::hex << actual.nCurValue << std::dec << '\n';
        if (actual.nCurValue != static_cast<unsigned int>(format)) {
            std::cerr << "[ERROR] PixelFormat readback differs from request\n";
            return -1;
        }
    }

    if (options.exposureUs) {
        int ret = MV_CC_SetEnumValueByString(handle, "ExposureMode", "Timed");
        if (ret != MV_OK) {
            printSdkError("Set ExposureMode=Timed", ret);
            return ret;
        }
        ret = MV_CC_SetEnumValueByString(handle, "ExposureAuto", "Off");
        if (ret != MV_OK) {
            printSdkError("Set ExposureAuto=Off", ret);
            return ret;
        }
        ret = setAndReadFloat(handle, "ExposureTime", *options.exposureUs, "us");
        if (ret != MV_OK) return ret;
    }

    if (options.gainDb) {
        int ret = MV_CC_SetEnumValueByString(handle, "GainAuto", "Off");
        if (ret != MV_OK) {
            printSdkError("Set GainAuto=Off", ret);
            return ret;
        }
        ret = setAndReadFloat(handle, "Gain", *options.gainDb, "dB");
        if (ret != MV_OK) return ret;
    }

    if (!options.exposureUs && !options.gainDb && !options.pixelFormat) {
        std::cout << "Camera settings: keeping current device settings\n";
    }
    return MV_OK;
}

using SteadyClock = std::chrono::steady_clock;

double elapsedMilliseconds(SteadyClock::time_point start) {
    return std::chrono::duration<double, std::milli>(SteadyClock::now() - start).count();
}

struct ConversionTiming {
    double conversionMs = 0;
    double copyMs = 0;
};

struct CaptureStatistics {
    unsigned long long frames = 0;
    unsigned long long borrowed = 0;
    unsigned long long returned = 0;
    double grabMs = 0;
    double conversionMs = 0;
    double copyMs = 0;
    double yoloMs = 0;
    double pipelineMs = 0;
    SteadyClock::time_point startTime{};
    double elapsedSeconds = 0;
    std::clock_t cpuStart = static_cast<std::clock_t>(-1);
    double cpuPercent = std::numeric_limits<double>::quiet_NaN();
    bool started = false;
    unsigned int nativeFormat = 0;
    unsigned int width = 0;
    unsigned int height = 0;
    bool mixedLayout = false;
    bool simulationTriggered = false;

    void start() {
        startTime = SteadyClock::now();
        cpuStart = std::clock();
        started = true;
    }
    void finish() {
        if (!started) return;
        elapsedSeconds = std::chrono::duration<double>(SteadyClock::now() - startTime).count();
        const auto cpuEnd = std::clock();
        if (cpuStart != static_cast<std::clock_t>(-1) &&
            cpuEnd != static_cast<std::clock_t>(-1) && elapsedSeconds > 0) {
            cpuPercent = 100.0 * static_cast<double>(cpuEnd - cpuStart)
                         / CLOCKS_PER_SEC / elapsedSeconds;
        }
    }
    double fps() const { return elapsedSeconds > 0 ? frames / elapsedSeconds : 0; }
    double mean(double totalMs) const { return frames > 0 ? totalMs / frames : 0; }
};

std::string csvText(const std::string& value) {
    std::string result = "\"";
    for (char c : value) {
        if (c == '"') result += '"';
        result += c;
    }
    return result + '"';
}

// FPS 是本程序处理吞吐率；CPU 统计整个进程所有线程，单核满载为 100%。
std::filesystem::path savePerformanceSummary(
    const CaptureStatistics& stats, const CameraOptions& options, int status) {
    const auto directory = std::filesystem::path(__FILE__).parent_path() / "logs";
    std::filesystem::create_directories(directory);
    const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    unsigned int sequence = 0;
    std::filesystem::path path;
    do {
        path = directory / ("performance_" + std::to_string(timestamp) + "_" +
                            std::to_string(++sequence) + ".csv");
    } while (std::filesystem::exists(path));

    std::ofstream file(path);
    if (!file) throw std::runtime_error("Cannot create performance CSV: " + path.string());
    const std::string source = options.serial.rfind("Vir", 0) == 0
        ? "virtual_device" : "usb_device";
    const std::string format = stats.mixedLayout ? "mixed" :
        (stats.borrowed == 0 ? "unknown" : cv::format("0x%08x", stats.nativeFormat));
    file << "source,serial,native_pixel_format,width,height,yolo_enabled,display_enabled,"
            "requested_frames,processed_frames,borrowed_frames,returned_frames,elapsed_seconds,"
            "processing_fps,process_cpu_percent_one_core_100,mean_get_buffer_ms,"
            "mean_format_conversion_ms,mean_ownership_copy_ms,mean_yolo_pipeline_ms,"
            "mean_get_to_result_ms,status,simulation,simulation_triggered\n";
    file << std::setprecision(17) << csvText(source) << ',' << csvText(options.serial) << ','
         << csvText(format) << ',' << (stats.mixedLayout ? 0 : stats.width) << ','
         << (stats.mixedLayout ? 0 : stats.height) << ',' << options.enableYolo << ','
         << !options.headless << ',' << options.frameLimit << ',' << stats.frames << ','
         << stats.borrowed << ',' << stats.returned << ',' << stats.elapsedSeconds << ','
         << stats.fps() << ',' << stats.cpuPercent << ',' << stats.mean(stats.grabMs) << ','
         << stats.mean(stats.conversionMs) << ',' << stats.mean(stats.copyMs) << ','
         << stats.mean(stats.yoloMs) << ',' << stats.mean(stats.pipelineMs) << ',' << status << ','
         << csvText(options.simulation.empty() ? "none" : options.simulation) << ','
         << stats.simulationTriggered << '\n';
    file.close();
    if (!file) throw std::runtime_error("Failed to write performance CSV: " + path.string());
    return path;
}

// 根据 SDK 返回的实际像素格式，生成独立的 BGR 图像
cv::Mat convertFrameToBgr(const MV_FRAME_OUT& frame, ConversionTiming* timing = nullptr) {
    if (timing) *timing = ConversionTiming{};
    const auto& info = frame.stFrameInfo;

    if (frame.pBufAddr == nullptr ||
        info.nWidth == 0 || info.nHeight == 0) {
        throw std::runtime_error("Empty image buffer or invalid size");
    }

    // 判断原始图像每个像素占几个字节
    int bytesPerPixel = 0;

    switch (info.enPixelType) {
        case PixelType_Gvsp_RGB8_Packed:
        case PixelType_Gvsp_BGR8_Packed:
            bytesPerPixel = 3;
            break;

        case PixelType_Gvsp_BayerRG8:
        case PixelType_Gvsp_Mono8:
            bytesPerPixel = 1;
            break;

        default:
            throw std::runtime_error("Unsupported pixel format");
    }

    // 当前只接受紧密排列、不包含额外数据的图像
    const std::size_t rowBytes =
        static_cast<std::size_t>(info.nWidth) * bytesPerPixel;

    const std::size_t expectedBytes =
        rowBytes * info.nHeight;

    if (info.nFrameLen != expectedBytes) {
        throw std::runtime_error("Unexpected image buffer length");
    }

    // RGB/BGR 使用三通道；Bayer/Mono 使用单通道存储
    const int matType =
        bytesPerPixel == 3 ? CV_8UC3 : CV_8UC1;

    // 建立 SDK 内存的视图，没有复制像素
    cv::Mat source(
        info.nHeight,
        info.nWidth,
        matType,
        frame.pBufAddr,
        rowBytes
    );

    cv::Mat bgrImage;
    const auto operationStart = SteadyClock::now();

    switch (info.enPixelType) {
        case PixelType_Gvsp_RGB8_Packed:
            // RGB → BGR，交换红蓝通道
            cv::cvtColor(source, bgrImage, cv::COLOR_RGB2BGR);
            break;

        case PixelType_Gvsp_BGR8_Packed:
            // 已经是 BGR，但仍需要复制成独立内存
            bgrImage = source.clone();
            break;

        case PixelType_Gvsp_BayerRG8:
            // RGGB Bayer 数据 → 完整的 BGR 彩色图像
            cv::cvtColor(source, bgrImage, cv::COLOR_BayerRGGB2BGR);
            break;

        case PixelType_Gvsp_Mono8:
            // 灰度 → 三通道 BGR，三个通道的值相同
            cv::cvtColor(source, bgrImage, cv::COLOR_GRAY2BGR);
            break;

        default:
            throw std::runtime_error("Unsupported pixel format");
    }

    if (timing) {
        const double duration = elapsedMilliseconds(operationStart);
        if (info.enPixelType == PixelType_Gvsp_BGR8_Packed) timing->copyMs = duration;
        else timing->conversionMs = duration;
    }
    return bgrImage;
}
// 统计转换后的、未绘制检测框的 BGR8 图像。
struct ImageStatistics {
    double meanGray;
    int saturatedPixels;
    std::size_t totalPixels;
    double saturationPercent;
};

ImageStatistics calculateImageStatistics(const cv::Mat& image) {
    if (image.empty() || image.type() != CV_8UC3) {
        throw std::runtime_error("Image statistics require a nonempty BGR8 image");
    }
    cv::Mat gray;
    cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);

    cv::Mat saturatedMask;
    // 使用 >=，灰度恰好等于 250 的像素也计入。
    cv::compare(gray, 250, saturatedMask, cv::CMP_GE);
    const int saturated = cv::countNonZero(saturatedMask);
    const std::size_t total = gray.total();

    return {
        cv::mean(gray)[0],
        saturated,
        total,
        100.0 * static_cast<double>(saturated) / static_cast<double>(total)
    };
}

// 保存 PNG，同时保存同名 CSV，供后续实验比较。
// image 是转换后的、未绘制检测框的 BGR8 图像，不是传感器 Bayer 原始数据。
std::filesystem::path saveFrame(
    const cv::Mat& image,
    const std::filesystem::path& outputDirectory) {
    const auto statistics = calculateImageStatistics(image);
    std::filesystem::create_directories(outputDirectory);

    const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    static unsigned long long sequence = 0;
    std::filesystem::path outputPath;
    std::filesystem::path statisticsPath;
    do {
        ++sequence;
        outputPath = outputDirectory /
            ("frame_" + std::to_string(timestamp) + "_" +
             std::to_string(sequence) + ".png");
        statisticsPath = outputPath;
        statisticsPath.replace_extension(".csv");
    } while (std::filesystem::exists(outputPath) ||
             std::filesystem::exists(statisticsPath));

    if (!cv::imwrite(outputPath.string(), image)) {
        throw std::runtime_error("Failed to write PNG image");
    }

    std::ofstream record(statisticsPath);
    if (!record) {
        throw std::runtime_error("PNG saved, but cannot create statistics CSV: " +
                                 statisticsPath.string());
    }
    record << "image,width,height,mean_gray,saturated_pixels,total_pixels,"
              "saturation_percent,gray_threshold,saved_format\n";
    record << std::setprecision(17)
           << outputPath.filename().string() << ','
           << image.cols << ',' << image.rows << ','
           << statistics.meanGray << ','
           << statistics.saturatedPixels << ','
           << statistics.totalPixels << ','
           << statistics.saturationPercent << ",250,BGR8_PNG\n";
    record.close();
    if (!record) {
        throw std::runtime_error("PNG saved, but statistics CSV write failed: " +
                                 statisticsPath.string());
    }

    std::cout << "Image statistics: mean_gray="
              << cv::format("%.3f", statistics.meanGray)
              << ", saturation(gray>=250)="
              << cv::format("%.4f%%", statistics.saturationPercent) << '\n';
    std::cout << "Statistics saved: " << statisticsPath << '\n';
    return outputPath;
}

// 解析预测、按类别去重，并绘制检测框、类别和置信度
int drawDetections(cv::Mat& image, const cv::Mat& output,
                   double scale, int left, int top) {
    static const char* names[] = {"B1", "B2", "B3", "R1", "R2", "R3"};
    constexpr float confidenceThreshold = 0.35f;
    constexpr float iouThreshold = 0.70f;
    if (output.dims != 3 || output.size[0] != 1 || output.size[1] != 18 ||
        output.type() != CV_32F || scale <= 0) {
        throw std::runtime_error("Invalid YOLO output or resize scale");
    }

    std::vector<cv::Rect2d> boxes;
    std::vector<float> scores;
    std::vector<int> classes;
    for (int i = 0; i < output.size[2]; ++i) {
        int classId = -1;
        float confidence = 0;
        for (int c = 0; c < 6; ++c) {
            const float score = output.ptr<float>(0, 4 + c)[i];
            if (score > confidence) {
                confidence = score;
                classId = c;
            }
        }
        if (!std::isfinite(confidence) || confidence < confidenceThreshold) continue;
        const double cx = output.ptr<float>(0, 0)[i];
        const double cy = output.ptr<float>(0, 1)[i];
        const double width = output.ptr<float>(0, 2)[i];
        const double height = output.ptr<float>(0, 3)[i];
        if (!std::isfinite(cx) || !std::isfinite(cy) ||
            !std::isfinite(width) || !std::isfinite(height) ||
            width <= 0 || height <= 0) continue;
        boxes.emplace_back(cx - width / 2, cy - height / 2, width, height);
        scores.push_back(confidence);
        classes.push_back(classId);
    }

    // 每个类别分别去除重复框，避免不同类别互相抑制。
    std::vector<int> kept;
    for (int c = 0; c < 6; ++c) {
        std::vector<cv::Rect2d> classBoxes;
        std::vector<float> classScores;
        std::vector<int> mapping;
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            if (classes[i] == c) {
                classBoxes.push_back(boxes[i]);
                classScores.push_back(scores[i]);
                mapping.push_back(static_cast<int>(i));
            }
        }
        std::vector<int> indices;
        cv::dnn::NMSBoxes(classBoxes, classScores,
                         confidenceThreshold, iouThreshold, indices);
        for (int i : indices) kept.push_back(mapping[i]);
    }
    std::sort(kept.begin(), kept.end(), [&](int a, int b) { return scores[a] > scores[b]; });
    if (kept.size() > 300) kept.resize(300);

    int drawn = 0;
    const double fontScale = std::max(0.6, image.cols / 1600.0);
    const int thickness = std::max(1, cvRound(fontScale));
    for (int i : kept) {
        const auto& box = boxes[i];
        // 撤销补边和缩放，再限制到原图范围。
        const int x1 = static_cast<int>(std::floor(std::clamp(
            (box.x - left) / scale, 0.0, static_cast<double>(image.cols))));
        const int y1 = static_cast<int>(std::floor(std::clamp(
            (box.y - top) / scale, 0.0, static_cast<double>(image.rows))));
        const int x2 = static_cast<int>(std::ceil(std::clamp(
            (box.x + box.width - left) / scale, 0.0, static_cast<double>(image.cols))));
        const int y2 = static_cast<int>(std::ceil(std::clamp(
            (box.y + box.height - top) / scale, 0.0, static_cast<double>(image.rows))));
        if (x2 <= x1 || y2 <= y1) continue;

        const cv::Scalar color = classes[i] < 3
            ? cv::Scalar(255, 160, 0) : cv::Scalar(0, 0, 255);
        cv::rectangle(image, cv::Rect(x1, y1, x2 - x1, y2 - y1), color, thickness + 1);
        const std::string label = cv::format("%s %.2f", names[classes[i]], scores[i]);
        int baseline = 0;
        const auto textSize = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX,
                                             fontScale, thickness, &baseline);
        const int textX = std::min(x1, std::max(0, image.cols - textSize.width));
        const int textY = std::max(textSize.height + 2, y1 - 5);
        cv::putText(image, label, cv::Point(textX, textY), cv::FONT_HERSHEY_SIMPLEX,
                    fontScale, color, thickness, cv::LINE_AA);
        ++drawn;
    }
    return drawn;
}

// 从项目 models 目录加载模型；相机和视频模式共用。
cv::dnn::Net loadYoloModel() {
    const auto modelPath = std::filesystem::path(__FILE__).parent_path()
        / "models" / "best.onnx";
    auto net = cv::dnn::readNetFromONNX(modelPath.string());
    net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
    net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
    std::cout << "YOLO model loaded: " << modelPath << '\n';
    return net;
}

// 一帧图像的识别结果：标注画面和检测数量。
struct YoloResult {
    cv::Mat displayImage;
    int detectionCount;
};

// 相机和视频必须走同一套预处理、推理和画框逻辑。
YoloResult inferAndDraw(cv::dnn::Net& net, const cv::Mat& image) {
    if (image.empty() || image.type() != CV_8UC3) {
        throw std::runtime_error("YOLO input must be a nonempty BGR8 image");
    }
    // 等比例缩放到 640×640，空白处填充灰色
    const int inputSize = 640;

    const double scale = std::min(
        static_cast<double>(inputSize) / image.cols,
        static_cast<double>(inputSize) / image.rows
    );

    const int resizedWidth =
        std::max(1, cvRound(image.cols * scale));
    const int resizedHeight =
        std::max(1, cvRound(image.rows * scale));

    const int left = (inputSize - resizedWidth) / 2;
    const int top = (inputSize - resizedHeight) / 2;

    cv::Mat resized;
    cv::resize(
        image, resized,
        cv::Size(resizedWidth, resizedHeight)
    );

    cv::Mat padded;
    cv::copyMakeBorder(
        resized, padded,
        top, inputSize - resizedHeight - top,
        left, inputSize - resizedWidth - left,
        cv::BORDER_CONSTANT,
        cv::Scalar(114, 114, 114)
    );

    // BGR 转 RGB，像素除以 255，整理为网络输入
    cv::Mat blob = cv::dnn::blobFromImage(
        padded,
        1.0 / 255.0,
        cv::Size(inputSize, inputSize),
        cv::Scalar(),
        true,
        false,
        CV_32F
    );

    net.setInput(blob);
    cv::Mat predictions = net.forward();

    // 当前这份模型的输出应为 1×18×8400
    if (predictions.dims != 3 ||
        predictions.size[0] != 1 ||
        predictions.size[1] != 18 ||
        predictions.size[2] != 8400) {
        throw std::runtime_error("Unexpected YOLO output shape");
    }

    cv::Mat displayImage = image.clone();
    const int count = drawDetections(displayImage, predictions, scale, left, top);
    return {displayImage, count};
}

// 已有视频的离线验证；headless 供不打开窗口的完整视频检查使用。
int runVideo(const std::string& videoPath, bool headless) {
    cv::VideoCapture video;
    const char* windowName = "Video Detection";
    bool windowCreated = false;
    unsigned long long processedFrames = 0;
    unsigned long long detectionSum = 0;
    int exitCode = 0;

    try {
        std::cout << "Input source: VIDEO (offline test)\n";
        auto net = loadYoloModel();
        if (!video.open(videoPath)) {
            throw std::runtime_error("Cannot open video: " + videoPath);
        }
        std::cout << "Video opened: " << videoPath << '\n';
        const auto outputDirectory = std::filesystem::path(__FILE__).parent_path()
            / "captures" / "video_test";
        if (!headless) {
            cv::namedWindow(windowName, cv::WINDOW_NORMAL);
            windowCreated = true;
            cv::resizeWindow(windowName, 1024, 750);
            std::cout << "Click the image window: S saves a frame; Q or Esc exits\n";
        }

        cv::Mat image;
        while (video.read(image)) {
            const auto result = inferAndDraw(net, image);
            ++processedFrames;
            detectionSum += result.detectionCount;
            if (processedFrames == 1) {
                std::cout << "First video-frame detections: "
                          << result.detectionCount << '\n';
            }
            if (processedFrames % 50 == 0) {
                std::cout << "Video frames processed: " << processedFrames << '\n';
            }
            if (headless) continue;

            cv::imshow(windowName, result.displayImage);
            cv::setWindowTitle(windowName,
                "Video Detection | Frames: " + std::to_string(processedFrames) +
                " | Detections: " + std::to_string(result.detectionCount) +
                " | S: save | Q / Esc: exit");
            const int key = cv::waitKey(1);
            if (key == 's' || key == 'S') {
                try {
                    const auto path = saveFrame(image, outputDirectory);
                    std::cout << "Video test image saved: " << path << '\n';
                } catch (const std::exception& error) {
                    std::cerr << "[ERROR] Save video test image: " << error.what() << '\n';
                }
            }
            if (key == 27 || key == 'q' || key == 'Q') {
                std::cout << "Exit requested by keyboard\n";
                break;
            }
            if (cv::getWindowProperty(windowName, cv::WND_PROP_VISIBLE) < 1) {
                std::cout << "Image window closed\n";
                break;
            }
        }
        if (processedFrames == 0) {
            throw std::runtime_error("No video frames could be decoded");
        }
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] Video test: " << error.what() << '\n';
        exitCode = 1;
    }

    video.release();
    if (windowCreated) {
        try {
            cv::destroyAllWindows();
        } catch (const std::exception& error) {
            std::cerr << "[ERROR] Destroy video window: " << error.what() << '\n';
            exitCode = 1;
        }
    }
    std::cout << "Video frames processed total: " << processedFrames << '\n';
    std::cout << "Per-frame detection sum: " << detectionSum << '\n';
    std::cout << "Video released\n";
    return exitCode;
}

// 连续取帧；可关闭窗口和 YOLO，用固定帧数进行格式性能测试。
int captureLoop(void* handle, const CameraOptions& options) {
    cv::dnn::Net net;
    if (options.enableYolo) {
        try {
            net = loadYoloModel();
        } catch (const std::exception& error) {
            std::cerr << "[ERROR] Load YOLO model: " << error.what() << '\n';
            return -1;
        }
    } else {
        std::cout << "YOLO disabled: capture/format performance test\n";
    }

    int ret = MV_CC_SetEnumValueByString(handle, "AcquisitionMode", "Continuous");
    if (ret != MV_OK) { printSdkError("Set AcquisitionMode", ret); return ret; }
    ret = MV_CC_SetEnumValueByString(handle, "TriggerMode", "Off");
    if (ret != MV_OK) { printSdkError("Set TriggerMode", ret); return ret; }
    ret = configureCamera(handle, options);
    if (ret != MV_OK) return ret;
    ret = MV_CC_StartGrabbing(handle);
    if (ret != MV_OK) { printSdkError("StartGrabbing", ret); return ret; }

    std::cout << "Acquisition started\n";
    const char* windowName = "Camera Live";
    bool windowCreated = false;
    CaptureStatistics stats;
    try {
        const auto outputDirectory = std::filesystem::path(__FILE__).parent_path() / "captures";
        std::cout << "Image output directory: " << outputDirectory << '\n';
        if (!options.headless) {
            cv::namedWindow(windowName, cv::WINDOW_NORMAL);
            windowCreated = true;
            cv::resizeWindow(windowName, 1024, 750);
            std::cout << "Click the image window: S saves a frame; Q or Esc exits\n";
        }

        // 测量不包含模型加载和窗口创建，包含循环中的等待、显示及保存操作。
        stats.start();
        while (options.frameLimit == 0 || stats.frames < options.frameLimit) {
            const auto frameStart = SteadyClock::now();
            MV_FRAME_OUT frame{};
            const auto grabStart = SteadyClock::now();
            if (options.simulation == "timeout" && stats.frames >= 3) {
                stats.simulationTriggered = true;
                std::cout << "[SIMULATION] Injecting timeout; no new SDK buffer is borrowed\n";
                ret = static_cast<int>(MV_E_NODATA);
            } else {
                ret = MV_CC_GetImageBuffer(handle, &frame, 1000);
            }
            const double grabMs = elapsedMilliseconds(grabStart);
            if (ret != MV_OK) { printSdkError("GetImageBuffer", ret); break; }
            ++stats.borrowed;

            // 只复制元数据，不复制指向的图像内存。
            const auto info = frame.stFrameInfo;
            if (stats.borrowed == 1) {
                stats.nativeFormat = static_cast<unsigned int>(info.enPixelType);
                stats.width = info.nWidth;
                stats.height = info.nHeight;
            } else if (stats.nativeFormat != static_cast<unsigned int>(info.enPixelType) ||
                       stats.width != info.nWidth || stats.height != info.nHeight) {
                stats.mixedLayout = true;
            }
            cv::Mat image;
            ConversionTiming conversion;
            try {
                // 只修改传给转换函数的元数据副本，归还时仍使用原始 frame。
                auto conversionFrame = frame;
                if (options.simulation == "format" && stats.frames >= 3) {
                    stats.simulationTriggered = true;
                    conversionFrame.stFrameInfo.enPixelType = PixelType_Gvsp_Undefined;
                    std::cout << "[SIMULATION] Injecting unsupported conversion format\n";
                }
                image = convertFrameToBgr(conversionFrame, &conversion);
            } catch (const std::exception& error) {
                std::cerr << "[ERROR] Image conversion: " << error.what() << '\n';
                ret = -1;
            }
            const int freeRet = MV_CC_FreeImageBuffer(handle, &frame);
            if (freeRet != MV_OK) {
                printSdkError("FreeImageBuffer", freeRet);
                if (ret == MV_OK) ret = freeRet;
            } else {
                ++stats.returned;
            }
            if (ret != MV_OK) break;

            // SDK 缓冲区已归还；后续仅访问独立的 image。
            cv::Mat displayImage = image;
            int detectionCount = 0;
            double yoloMs = 0;
            if (options.enableYolo) {
                const auto yoloStart = SteadyClock::now();
                const auto result = inferAndDraw(net, image);
                yoloMs = elapsedMilliseconds(yoloStart);
                displayImage = result.displayImage;
                detectionCount = result.detectionCount;
            }
            const double pipelineMs = elapsedMilliseconds(frameStart);
            ++stats.frames;
            stats.grabMs += grabMs;
            stats.conversionMs += conversion.conversionMs;
            stats.copyMs += conversion.copyMs;
            stats.yoloMs += yoloMs;
            stats.pipelineMs += pipelineMs;
            if (stats.frames == 1 && options.enableYolo) {
                std::cout << "YOLO inference succeeded: output shape=(1,18,8400)\n";
                std::cout << "YOLO postprocess succeeded, detections: " << detectionCount << '\n';
            }

            if (!options.headless) {
                cv::imshow(windowName, displayImage);
                cv::setWindowTitle(windowName,
                    "Camera Live | Frames: " + std::to_string(stats.frames) +
                    " | Detections: " + std::to_string(detectionCount) +
                    " | S: save | Q / Esc: exit");
                const int key = cv::waitKey(1);
                if (key == 's' || key == 'S') {
                    try {
                        const auto path = saveFrame(image, outputDirectory);
                        std::cout << "Image saved: " << path << '\n';
                    } catch (const std::exception& error) {
                        std::cerr << "[ERROR] Save image: " << error.what() << '\n';
                    }
                }
                if (key == 27 || key == 'q' || key == 'Q') {
                    std::cout << "Exit requested by keyboard\n";
                    break;
                }
                if (cv::getWindowProperty(windowName, cv::WND_PROP_VISIBLE) < 1) {
                    std::cout << "Image window closed\n";
                    break;
                }
            }
        }
        if (options.frameLimit > 0 && stats.frames >= options.frameLimit) {
            std::cout << "Frame limit reached\n";
        }
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] Capture loop: " << error.what() << '\n';
        ret = -1;
    }
    stats.finish();
    if (!options.simulation.empty()) {
        std::cout << "Simulation mode: " << options.simulation
                  << "; triggered=" << stats.simulationTriggered << '\n';
    }
    const int stopRet = MV_CC_StopGrabbing(handle);
    if (stopRet != MV_OK) {
        printSdkError("StopGrabbing", stopRet);
        if (ret == MV_OK) ret = stopRet;
    } else {
        std::cout << "Acquisition stopped\n";
    }
    if (windowCreated) {
        try { cv::destroyAllWindows(); }
        catch (const std::exception& error) {
            std::cerr << "[ERROR] Destroy windows: " << error.what() << '\n';
            if (ret == MV_OK) ret = -1;
        }
    }
    std::cout << "Frames processed: " << stats.frames << '\n';
    std::cout << "Buffers borrowed=" << stats.borrowed << " returned=" << stats.returned << '\n';
    std::cout << "Capture performance: fps=" << stats.fps()
              << " process_cpu_percent(one core=100)=" << stats.cpuPercent << '\n';
    std::cout << "Mean time(ms): get_buffer=" << stats.mean(stats.grabMs)
              << " format_conversion=" << stats.mean(stats.conversionMs)
              << " ownership_copy=" << stats.mean(stats.copyMs)
              << " yolo_pipeline=" << stats.mean(stats.yoloMs) << '\n';
    try {
        const auto path = savePerformanceSummary(stats, options, ret);
        std::cout << "Performance summary saved: " << path << '\n';
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] Save performance summary: " << error.what() << '\n';
        if (ret == MV_OK) ret = -1;
    }
    return ret;
}

// 创建句柄、打开设备、运行采集循环，然后清理
int runCamera(MV_CC_DEVICE_INFO* device, const CameraOptions& options) {
    void* handle = nullptr;

    int ret = MV_CC_CreateHandle(&handle, device);

    if (ret != MV_OK) {
        printSdkError("CreateHandle", ret);
        return ret;
    }

    std::cout << "Camera handle created\n";

    ret = MV_CC_OpenDevice(handle);

    if (ret != MV_OK) {
        printSdkError("OpenDevice", ret);
    } else {
        std::cout << "Camera opened\n";

        ret = captureLoop(handle, options);

        const int closeRet = MV_CC_CloseDevice(handle);

        if (closeRet != MV_OK) {
            printSdkError("CloseDevice", closeRet);

            if (ret == MV_OK) {
                ret = closeRet;
            }
        } else {
            std::cout << "Camera closed\n";
        }
    }

    const int destroyRet = MV_CC_DestroyHandle(handle);

    if (destroyRet != MV_OK) {
        printSdkError("DestroyHandle", destroyRet);

        if (ret == MV_OK) {
            ret = destroyRet;
        }
    } else {
        std::cout << "Camera handle destroyed\n";
    }

    return ret;
}

int runApplication(int argc, char* argv[]) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        printUsage();
        return 0;
    }
    // 视频模式不初始化相机 SDK，也不修改相机设置。
    if (argc > 1 && std::string(argv[1]) == "--video") {
        const bool headless = argc == 4 && std::string(argv[3]) == "--headless";
        if ((argc != 3 && argc != 4) || (argc == 4 && !headless)) {
            printUsage();
            return 2;
        }
        RunLog runLog;
        std::cout << "Run log: " << runLog.path() << '\n';
        return runVideo(argv[2], headless);
    }

    CameraOptions options;
    try {
        options = parseCameraOptions(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] Camera arguments: " << error.what() << '\n';
        printUsage();
        return 2;
    }
    RunLog runLog;
    std::cout << "Run log: " << runLog.path() << '\n';
    const char* targetSerial = options.serial.c_str();
    std::cout << "Target camera serial: " << options.serial << '\n';

    int ret = MV_CC_Initialize();

    if (ret != MV_OK) {
        printSdkError("Initialize", ret);
        return 1;
    }

    std::cout << "SDK initialized\n";

    MV_CC_DEVICE_INFO_LIST devices{};
    MV_CC_DEVICE_INFO* selectedDevice = nullptr;
    int exitCode = 0;

    ret = MV_CC_EnumDevices(MV_USB_DEVICE, &devices);

    if (ret != MV_OK) {
        printSdkError("EnumDevices", ret);
        exitCode = 1;
    } else {
        std::cout << "USB camera count: "
                  << devices.nDeviceNum << '\n';

        for (unsigned int i = 0; i < devices.nDeviceNum; ++i) {
            auto* device = devices.pDeviceInfo[i];

            if (device == nullptr) {
                continue;
            }

            const auto& usb = device->SpecialInfo.stUsb3VInfo;

            const char* serial =
                reinterpret_cast<const char*>(usb.chSerialNumber);

            const char* model =
                reinterpret_cast<const char*>(usb.chModelName);

            std::cout << "[" << i << "] Model: " << model
                      << "  Serial: " << serial << '\n';

            if (std::strcmp(serial, targetSerial) == 0) {
                selectedDevice = device;
            }
        }

        if (selectedDevice == nullptr) {
            std::cerr << "Target camera not found: "
                      << targetSerial << '\n';
            exitCode = 1;
        } else {
            ret = runCamera(selectedDevice, options);

            if (ret != MV_OK) {
                exitCode = 1;
            }
        }
    }

    const int cleanupRet = MV_CC_Finalize();

    if (cleanupRet != MV_OK) {
        printSdkError("Finalize", cleanupRet);
        exitCode = 1;
    } else {
        std::cout << "SDK released\n";
    }

    return exitCode;
}

// 捕获启动日志等阶段的异常；此时尚未获取设备资源。
int main(int argc, char* argv[]) {
    try {
        return runApplication(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "[FATAL] " << error.what() << '\n';
        return 1;
    }
}
