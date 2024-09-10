# WhisperLiveCppClient

## 简介

WhisperLive只提供了python的客户端，我写了个c++客户端通过websocket与WhisperLive服务端进行通信。该客户端可以实时调用麦克风采集音频并上传到WhisperLive服务端，并可以接收WhisperLive服务端的语音识别结果。

## 文件说明

- microphone_live: C++客户端源码
- CMakeLists.txt: C++客户端CMake文件
- README.md: 本文件

## 使用方法

1. 运行WhisperLive服务端，参考https://github.com/collabora/WhisperLive/blob/main/README.md
2. 添加nlohmann/json、websocketpp到项目文件夹
3. 编译microphone_live，参考CMakeLists.txt
4. 运行microphone_live

## 参考链接

- WhisperLive: https://github.com/collabora/WhisperLive
- nlohmann/json: https://github.com/nlohmann/json

