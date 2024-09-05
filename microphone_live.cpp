#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <iostream>
#include <vector>
#include <thread>
#include <RtAudio.h>
#include <nlohmann/json.hpp>
#include <boost/log/trivial.hpp>
#include <condition_variable>
#include <mutex>
#include <signal.h>
#include <samplerate.h>
#include <vector>

using json = nlohmann::json;
using namespace std;
using namespace boost::beast;
using namespace boost::asio;
using ip::tcp;
namespace websocket = boost::beast::websocket;

std::condition_variable cv;
std::mutex cv_m;
bool audio_finished = false;

// 一个用于重采样的函数
std::vector<float> resampleAudio(const std::vector<float>& input, int inputSampleRate, int outputSampleRate, int channels) {
    if (input.empty()) return {};

    // 计算重采样后的长度
    long long outputLength = (long long) input.size() * outputSampleRate / inputSampleRate;

    std::vector<float> output(outputLength * channels);
    SRC_DATA srcData;
    srcData.data_in = input.data();
    srcData.input_frames = input.size() / channels;
    srcData.data_out = output.data();
    srcData.output_frames = outputLength / channels;
    srcData.src_ratio = double(outputSampleRate) / inputSampleRate;

    // 使用SRC_SINC_FASTEST模式进行重采样，也可根据需求更换其他模式
    src_simple(&srcData, SRC_SINC_FASTEST, channels);

    // 调整输出大小到实际重采样后的长度
    output.resize(srcData.output_frames_gen * channels);
    return output;
}

// 音频回调函数，用于处理捕获的音频数据
extern "C" int audioCallback(void *outputBuffer, void *inputBuffer, unsigned int nBufferFrames,
                             double streamTime, RtAudioStreamStatus status, void *userData) {
    auto *ws = static_cast<websocket::stream<tcp::socket> *>(userData);
    int16_t *input = static_cast<int16_t*>(inputBuffer);
    std::vector<float> floatBuffer(nBufferFrames);

    for (unsigned int i = 0; i < nBufferFrames; i++) {
        floatBuffer[i] = input[i] / 32768.0f;
    }

    // 从44100Hz重采样到16000Hz
    auto resampledBuffer = resampleAudio(floatBuffer, 44100, 16000, 1);

    // 使用正确的数据类型发送
    ws->binary(true);  // 确保设置为发送二进制数据
    ws->async_write(boost::asio::buffer(resampledBuffer.data(), resampledBuffer.size() * sizeof(float)),
        [ws](boost::system::error_code ec, std::size_t bytes_transferred) {
            if (ec) {
                std::cerr << "WebSocket send error: " << ec.message() << std::endl;
            }
        });

    return 0;  // 返回0以继续流式处理
}

class ClientState {
public:
    enum State {
        CONNECTING,   // 正在连接
        CONNECTED,    // 已连接
        DISCONNECTED, // 已断开
        ERROR         // 错误
    };

private:
    State currentState;            // 当前状态
    int errorCount;                // 错误计数
    std::string lastErrorMessage;  // 最后一次错误信息

public:
    ClientState() : currentState(CONNECTING), errorCount(0) {}

    void logError(const std::string& message) {
        BOOST_LOG_TRIVIAL(error) << message;
        errorCount++;
        lastErrorMessage = message;
        currentState = ERROR;
    }

    void setState(State newState) {
        currentState = newState;
        BOOST_LOG_TRIVIAL(info) << "State changed to: " << newState;
    }

    bool hasErrorOccurred() const {
        return currentState == ERROR;
    }

    int getErrorCount() const {
        return errorCount;
    }

    std::string getLastError() const {
        return lastErrorMessage;
    }
};

// 开始音频流处理函数
void start_audio_stream(RtAudio& dac, websocket::stream<tcp::socket>& ws, ClientState& state) {
    RtAudio::StreamParameters parameters;                  // 音频流参数配置
    parameters.deviceId = dac.getDefaultInputDevice();     // 获取默认输入设备ID
    parameters.nChannels = 1;                              // 单声道
    unsigned int sampleRate = 44100;                       // 采样率44.1kHz
    unsigned int bufferFrames = 8192;                      // 缓冲区大小8192帧

    try {
        dac.openStream(nullptr, &parameters, RTAUDIO_SINT16, sampleRate, &bufferFrames,
                       audioCallback, &ws); // 打开音频流并设置回调函数
        dac.startStream();
        BOOST_LOG_TRIVIAL(info) << "Audio stream started.";

        // 继续流式处理，直到外部信号或条件停止
        while (!audio_finished && !state.hasErrorOccurred()) {
            std::this_thread::sleep_for(std::chrono::seconds(1)); // 继续流式处理
        }

        dac.stopStream();                        // 停止音频流
        if (dac.isStreamOpen()) {
            dac.closeStream();                   // 关闭音频流
        }
        std::lock_guard<std::mutex> lock(cv_m);
        audio_finished = true;
        cv.notify_one();
    } catch (RtAudioError& e) {
        e.printMessage();
        state.logError("Failed to start audio stream: " + e.getMessage());  // 记录启动失败的错误
    }
}

// 处理语音识别结果函数
void process_segment(const nlohmann::json& segment) {
    if (segment.contains("text") && segment["text"].is_string()) {
        std::string text = segment["text"].get<std::string>();
        BOOST_LOG_TRIVIAL(info) << "Received text: " << text;
    }
    // if (segment.contains("start") && segment.contains("end")) {
    //     BOOST_LOG_TRIVIAL(info) << "Segment duration: "
    //         << segment["start"].get<std::string>() << " to "
    //         << segment["end"].get<std::string>();
    // }
}

// 处理接收WebSocket消息
void handle_receive(websocket::stream<tcp::socket>& ws, boost::beast::multi_buffer& buffer, ClientState& state) {
    ws.async_read(buffer, [&ws, &buffer, &state](boost::system::error_code ec, std::size_t bytes_transferred) {
        // BOOST_LOG_TRIVIAL(info) << "Data received, size: " << bytes_transferred;
        if (!ec) {
            std::string message = boost::beast::buffers_to_string(buffer.data());
            // BOOST_LOG_TRIVIAL(debug) << "Attempting to parse JSON message: " << message;
            try {
                auto json_message = json::parse(message);
                // BOOST_LOG_TRIVIAL(info) << "Parsed JSON successfully.";
                
                if (json_message.contains("segments") && json_message["segments"].is_array()) {
                    for (auto& segment : json_message["segments"]) {
                        // BOOST_LOG_TRIVIAL(debug) << "Debug: Entered process_segment function";
                        process_segment(segment);  // 处理每个段落
                    }
                }
                
            } catch (const json::exception& e) {
                BOOST_LOG_TRIVIAL(error) << "JSON parsing error: " << e.what();
            }
            buffer.consume(buffer.size());
            handle_receive(ws, buffer, state);  // 继续读取消息
        } else {
            BOOST_LOG_TRIVIAL(error) << "Receive failed: " << ec.message();
            state.logError("Receive failed: " + ec.message());
            boost::system::error_code ecc;
            ws.close(websocket::close_reason(websocket::close_code::normal), ecc);
            if (ecc) {
                BOOST_LOG_TRIVIAL(error) << "WebSocket close failed: " << ecc.message();
            }
        }
    });
}

// 程序主入口
int main() {
    RtAudio dac;                                    // 创建RtAudio对象
    if (dac.getDeviceCount() < 1) {
        std::cerr << "No audio devices found!\n";
        return 1;                                  // 如果没有音频设备，退出程序
    }

    io_context ioc;                               // 创建asio io_context对象
    tcp::resolver resolver(ioc);                  // 创建解析器
    auto endpoints = resolver.resolve("localhost", "9090");

    websocket::stream<tcp::socket> ws(ioc);
    auto& tcp_layer = ws.next_layer();
    boost::asio::connect(tcp_layer, endpoints.begin(), endpoints.end());
    ws.handshake("localhost", "/");

    BOOST_LOG_TRIVIAL(info) << "WebSocket handshake successful.";

    // 创建初始化消息
    json init_message = {
        {"language", "zh"},
        {"task", "transcribe"},
        {"uid", "user123"},
        {"use_vad", false},
        {"model", "small"}
    };

    ws.text(true);                                       // 设置为文本模式
    ws.write(boost::asio::buffer(init_message.dump()));  // 发送初始化消息
    BOOST_LOG_TRIVIAL(info) << "Sent initial message.";

    ClientState state;                  // 创建客户端状态对象
    boost::beast::multi_buffer buffer;  // 创建用于接收消息的缓冲区
    // BOOST_LOG_TRIVIAL(info) << "Starting receive loop.";
    handle_receive(ws, buffer, state);  // 开始异步接收
    // BOOST_LOG_TRIVIAL(info) << "Done receive loop.";

    std::thread audio_thread([&]() { start_audio_stream(dac, ws, state); }); // 创建音频流线程

    ioc.run();  // 运行io_context，直到所有异步操作完成

    // 等待音频完成
    std::unique_lock<std::mutex> lock(cv_m);
    cv.wait(lock, [] { return audio_finished; });

    if (audio_thread.joinable()) {
        audio_thread.join();                      // 如果音频线程可连接，则加入
    }

    // Cleanup
    boost::system::error_code ec;
    ws.close(websocket::close_reason(websocket::close_code::normal), ec);
    if (ec) {
        std::cerr << "WebSocket close failed: " << ec.message() << std::endl;
        return 1;
    }

    BOOST_LOG_TRIVIAL(info) << "Client disconnected cleanly.";

    return 0;
}

