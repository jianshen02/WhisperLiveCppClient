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

// 你的audioCallback函数中，调用重采样函数
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

    return 0;  // Return zero to continue streaming
}

class ClientState {
public:
    enum State {
        CONNECTING,
        CONNECTED,
        DISCONNECTED,
        ERROR
    };

private:
    State currentState;
    int errorCount;
    std::string lastErrorMessage;

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

void start_audio_stream(RtAudio& dac, websocket::stream<tcp::socket>& ws, ClientState& state) {
    RtAudio::StreamParameters parameters;
    parameters.deviceId = dac.getDefaultInputDevice();
    parameters.nChannels = 1;
    unsigned int sampleRate = 44100;
    unsigned int bufferFrames = 8192;

    try {
        dac.openStream(nullptr, &parameters, RTAUDIO_SINT16, sampleRate, &bufferFrames,
                       audioCallback, &ws); // Pass websocket stream to the callback
        dac.startStream();
        BOOST_LOG_TRIVIAL(info) << "Audio stream started.";

        // Continue streaming until an external signal or condition stops it
        while (!audio_finished && !state.hasErrorOccurred()) {
            std::this_thread::sleep_for(std::chrono::seconds(1)); // Keep streaming
        }

        dac.stopStream();
        if (dac.isStreamOpen()) {
            dac.closeStream();
        }
        std::lock_guard<std::mutex> lock(cv_m);
        audio_finished = true;
        cv.notify_one();
    } catch (RtAudioError& e) {
        e.printMessage();
        state.logError("Failed to start audio stream: " + e.getMessage());
    }
}

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
                        process_segment(segment);  // Process each segment individually
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

int main() {
    RtAudio dac;
    if (dac.getDeviceCount() < 1) {
        std::cerr << "No audio devices found!\n";
        return 1;
    }

    io_context ioc;
    tcp::resolver resolver(ioc);
    auto endpoints = resolver.resolve("localhost", "9090");

    websocket::stream<tcp::socket> ws(ioc);
    auto& tcp_layer = ws.next_layer();
    boost::asio::connect(tcp_layer, endpoints.begin(), endpoints.end());
    ws.handshake("localhost", "/");

    BOOST_LOG_TRIVIAL(info) << "WebSocket handshake successful.";

    json init_message = {
        {"language", "zh"},
        {"task", "transcribe"},
        {"uid", "user123"},
        {"use_vad", false},
        {"model", "small"}
    };

    ws.text(true);
    ws.write(boost::asio::buffer(init_message.dump()));
    BOOST_LOG_TRIVIAL(info) << "Sent initial message.";

    ClientState state;
    boost::beast::multi_buffer buffer;  // Buffer to hold incoming messages
    // BOOST_LOG_TRIVIAL(info) << "Starting receive loop.";
    handle_receive(ws, buffer, state);  // Start the asynchronous receive
    // BOOST_LOG_TRIVIAL(info) << "Done receive loop.";

    std::thread audio_thread([&]() { start_audio_stream(dac, ws, state); });

    ioc.run();  // This will block until all asynchronous operations have completed

    // Wait for audio to finish
    std::unique_lock<std::mutex> lock(cv_m);
    cv.wait(lock, [] { return audio_finished; });

    if (audio_thread.joinable()) {
        audio_thread.join();
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

