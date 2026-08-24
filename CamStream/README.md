# ./CamStream
## 概述
本文件夹主要负责摄像头数据采集及向主机推流，包含1080p+12fps/720p+20fps两种模式（GLM如此叙述）。

采用CMake。

代码功能细节尚待审阅。

## 编译
将`CamStream`文件夹内的代码文件拷至Linux开发板（可新建一文件夹），在文件夹内新建`build`文件夹。

在`build`文件夹下执行：
```bash
cmake .. && make -j$(nproc)
```

## 运行
### 1080p
编译完成后，在目录下执行：
```bash
./rgb_1080p
```
将开发板上生成的stream_1080p.sdp拷至主机后，在主机运行：
```bash
vlc --network-caching=100 stream_1080p.sdp
```
`--network-caching=100`将缓存设置为100ms，可根据情况调整。

`stream_1080p.sdp`文件的路径根据拷贝的位置做调整。
### 720p
编译完成后，在目录下执行：
```bash
./rgb_720p
```
将开发板上生成的stream_1080p.sdp拷至主机后，在主机运行：
```bash
vlc --network-caching=100 stream_720p.sdp
```
`--network-caching=100`将缓存设置为100ms，可根据情况调整。

`stream_720p.sdp`文件的路径根据拷贝的位置做调整。

## 注意事项
- 目前推流基于有线连接和NAT模式，主机IP为`192.168.137.1`。之后若主机IP有变化，需要对代码做对应修改。
- 建议推流前先测试开发板和主机间是否可Ping通。

