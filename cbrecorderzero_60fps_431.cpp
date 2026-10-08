// cbrecorderzero_60fps_framefill8.cpp
//
// BUILD:
// g++ -std=c++17 -O2 -I/c/Users/user/Downloads/ffmpeg-5.1.10 -o cbrecorderzero_60fps_framefill8.exe cbrecorderzero_60fps_framefill8.cpp /c/Users/user/Downloads/ffmpeg-5.1.10/libavcodec/libavcodec.dll.a /c/Users/user/Downloads/ffmpeg-5.1.10/libavformat/libavformat.dll.a /c/Users/user/Downloads/ffmpeg-5.1.10/libavutil/libavutil.dll.a -ld3d11 -ldxgi -lole32

#include <windows.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <iostream>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <csignal>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/error.h>
}

using Microsoft::WRL::ComPtr;

std::atomic<bool> stopRecording(false);

void signalHandler(int)
{
    stopRecording = true;
}

std::string getFilename()
{
    auto now = std::chrono::system_clock::now();
    auto timeT = std::chrono::system_clock::to_time_t(now);

    std::tm tm;
    localtime_s(&tm, &timeT);

    std::ostringstream oss;

    oss << "dxgi_output_"
        << std::put_time(&tm, "%Y%m%d_%H%M%S")
        << ".mp4";

    return oss.str();
}

class FramePool
{
public:

    FramePool(int count)
    {
        for (int i = 0; i < count; ++i)
        {
            AVFrame* f = av_frame_alloc();

            if (f)
                freeFrames.push_back(f);
        }
    }

    ~FramePool()
    {
        for (AVFrame* f : freeFrames)
        {
            av_frame_free(&f);
        }

        freeFrames.clear();
    }

    AVFrame* acquire()
    {
        std::lock_guard<std::mutex> lock(mtx);

        if (freeFrames.empty())
            return av_frame_alloc();

        AVFrame* f = freeFrames.back();

        freeFrames.pop_back();

        return f;
    }

    void release(AVFrame* f)
    {
        if (!f)
            return;

        av_frame_unref(f);

        std::lock_guard<std::mutex> lock(mtx);

        freeFrames.push_back(f);
    }

private:

    std::vector<AVFrame*> freeFrames;
    std::mutex mtx;
};

static inline uint8_t clampByte(int value)
{
    if (value < 0)
        return 0;

    if (value > 255)
        return 255;

    return static_cast<uint8_t>(value);
}

void convertBGRAtoYUV420P(
    const uint8_t* src,
    int srcStride,
    AVFrame* frame,
    int width,
    int height)
{
    uint8_t* yPlane = frame->data[0];
    uint8_t* uPlane = frame->data[1];
    uint8_t* vPlane = frame->data[2];

    int yStride = frame->linesize[0];
    int uStride = frame->linesize[1];
    int vStride = frame->linesize[2];

    for (int y = 0; y < height; ++y)
    {
        const uint8_t* row =
            src + y * srcStride;

        uint8_t* yOut =
            yPlane + y * yStride;

        for (int x = 0; x < width; ++x)
        {
            int b = row[x * 4 + 0];
            int g = row[x * 4 + 1];
            int r = row[x * 4 + 2];

            int Y =
                ((66 * r +
                  129 * g +
                  25 * b +
                  128) >> 8) + 16;

            yOut[x] = clampByte(Y);
        }
    }

    for (int y = 0; y < height; y += 2)
    {
        const uint8_t* row0 =
            src + y * srcStride;

        const uint8_t* row1 =
            src + (y + 1) * srcStride;

        uint8_t* uOut =
            uPlane + (y / 2) * uStride;

        uint8_t* vOut =
            vPlane + (y / 2) * vStride;

        for (int x = 0; x < width; x += 2)
        {
            int rSum = 0;
            int gSum = 0;
            int bSum = 0;

            for (int dy = 0; dy < 2; ++dy)
            {
                const uint8_t* row =
                    (dy == 0) ? row0 : row1;

                for (int dx = 0; dx < 2; ++dx)
                {
                    int px = x + dx;

                    int b = row[px * 4 + 0];
                    int g = row[px * 4 + 1];
                    int r = row[px * 4 + 2];

                    rSum += r;
                    gSum += g;
                    bSum += b;
                }
            }

            int r = rSum / 4;
            int g = gSum / 4;
            int b = bSum / 4;

            int U =
                ((-38 * r -
                  74 * g +
                  112 * b +
                  128) >> 8) + 128;

            int V =
                ((112 * r -
                  94 * g -
                  18 * b +
                  128) >> 8) + 128;

            uOut[x / 2] =
                clampByte(U);

            vOut[x / 2] =
                clampByte(V);
        }
    }
}

int main()
{
    signal(SIGINT, signalHandler);

    const int width = 1600;
    const int height = 900;
    const int fps = 60;

    av_log_set_level(AV_LOG_ERROR);

    /*
        ------------------------------------------------------------
        D3D11 DEVICE
        ------------------------------------------------------------
    */

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;

    D3D_FEATURE_LEVEL featureLevel;

    if (FAILED(D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &device,
        &featureLevel,
        &context)))
    {
        std::cerr
            << "D3D11CreateDevice failed\n";

        return -1;
    }

    /*
        ------------------------------------------------------------
        DXGI OUTPUT DUPLICATION
        ------------------------------------------------------------
    */

    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIOutput> output;
    ComPtr<IDXGIOutput1> output1;
    ComPtr<IDXGIOutputDuplication> duplication;

    ComPtr<IDXGIDevice> dxgiDevice;

    if (FAILED(device.As(&dxgiDevice)))
    {
        std::cerr
            << "DXGI device failed\n";

        return -1;
    }

    if (FAILED(dxgiDevice->GetAdapter(&adapter)))
    {
        std::cerr
            << "GetAdapter failed\n";

        return -1;
    }

    if (FAILED(adapter->EnumOutputs(0, &output)))
    {
        std::cerr
            << "EnumOutputs failed\n";

        return -1;
    }

    if (FAILED(output.As(&output1)))
    {
        std::cerr
            << "Output1 failed\n";

        return -1;
    }

    if (FAILED(output1->DuplicateOutput(
        device.Get(),
        &duplication)))
    {
        std::cerr
            << "DuplicateOutput failed\n";

        return -1;
    }

    /*
        ------------------------------------------------------------
        OUTPUT FILE
        ------------------------------------------------------------
    */

    std::string filename = getFilename();

    AVFormatContext* outCtx = nullptr;

    if (avformat_alloc_output_context2(
        &outCtx,
        nullptr,
        "mp4",
        filename.c_str()) < 0)
    {
        std::cerr
            << "Could not create output context\n";

        return -1;
    }

    /*
        ------------------------------------------------------------
        NVENC
        ------------------------------------------------------------
    */

    const AVCodec* codec =
        avcodec_find_encoder_by_name("h264_nvenc");

    if (!codec)
    {
        std::cerr
            << "h264_nvenc not found\n";

        return -1;
    }

    AVCodecContext* codecCtx =
        avcodec_alloc_context3(codec);

    if (!codecCtx)
    {
        std::cerr
            << "Could not allocate codec context\n";

        return -1;
    }

    codecCtx->width = width;
    codecCtx->height = height;

    codecCtx->pix_fmt =
        AV_PIX_FMT_YUV420P;

    codecCtx->bit_rate =
        18 * 1000 * 1000;

    codecCtx->gop_size = 120;

    codecCtx->max_b_frames = 0;

    /*
        FIXED 60 FPS CFR

        PTS remains simple and sequential:

        0, 1, 2, 3, 4...

        with a 1/60 second time base.
    */

    codecCtx->time_base =
        AVRational{1, fps};

    codecCtx->framerate =
        AVRational{fps, 1};

    if (outCtx->oformat->flags &
        AVFMT_GLOBALHEADER)
    {
        codecCtx->flags |=
            AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    AVStream* stream =
        avformat_new_stream(outCtx, codec);

    if (!stream)
    {
        std::cerr
            << "Could not create stream\n";

        return -1;
    }

    stream->time_base =
        codecCtx->time_base;

    stream->avg_frame_rate =
        AVRational{fps, 1};

    stream->r_frame_rate =
        AVRational{fps, 1};

    if (avcodec_open2(
        codecCtx,
        codec,
        nullptr) < 0)
    {
        std::cerr
            << "Could not open NVENC\n";

        return -1;
    }

    if (avcodec_parameters_from_context(
        stream->codecpar,
        codecCtx) < 0)
    {
        std::cerr
            << "Could not copy codec parameters\n";

        return -1;
    }

    if (!(outCtx->oformat->flags &
          AVFMT_NOFILE))
    {
        if (avio_open(
            &outCtx->pb,
            filename.c_str(),
            AVIO_FLAG_WRITE) < 0)
        {
            std::cerr
                << "Could not open output file\n";

            return -1;
        }
    }

    if (avformat_write_header(
        outCtx,
        nullptr) < 0)
    {
        std::cerr
            << "Could not write header\n";

        return -1;
    }

    /*
        ------------------------------------------------------------
        STAGING TEXTURE
        ------------------------------------------------------------
    */

    D3D11_TEXTURE2D_DESC desc{};

    desc.Width = width;
    desc.Height = height;

    desc.MipLevels = 1;
    desc.ArraySize = 1;

    desc.Format =
        DXGI_FORMAT_B8G8R8A8_UNORM;

    desc.SampleDesc.Count = 1;

    desc.Usage =
        D3D11_USAGE_STAGING;

    desc.CPUAccessFlags =
        D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> cpuTex;

    if (FAILED(device->CreateTexture2D(
        &desc,
        nullptr,
        &cpuTex)))
    {
        std::cerr
            << "Could not create staging texture\n";

        return -1;
    }

    /*
        ------------------------------------------------------------
        LATEST FRAME
        ------------------------------------------------------------
    */

    AVFrame* latestFrame =
        av_frame_alloc();

    if (!latestFrame)
    {
        std::cerr
            << "Could not allocate latest frame\n";

        return -1;
    }

    latestFrame->format =
        codecCtx->pix_fmt;

    latestFrame->width =
        width;

    latestFrame->height =
        height;

    if (av_frame_get_buffer(
        latestFrame,
        32) < 0)
    {
        std::cerr
            << "Could not allocate latest frame buffer\n";

        return -1;
    }

    std::mutex latestMutex;

    bool latestReady = false;

    /*
        ------------------------------------------------------------
        FRAME POOL
        ------------------------------------------------------------
    */

    FramePool pool(8);

    /*
        ------------------------------------------------------------
        ENCODER QUEUE
        ------------------------------------------------------------
    */

    std::mutex encodeMutex;

    std::condition_variable encodeCV;

    std::deque<AVFrame*> encodeQueue;

    const size_t maxEncodeQueue = 4;

    /*
        ------------------------------------------------------------
        CAPTURE THREAD
        ------------------------------------------------------------
    */

    std::thread captureThread([&]()
    {
        while (!stopRecording.load())
        {
            DXGI_OUTDUPL_FRAME_INFO frameInfo{};

            ComPtr<IDXGIResource> resource;

            HRESULT hr =
                duplication->AcquireNextFrame(
                    100,
                    &frameInfo,
                    &resource);

            if (hr == DXGI_ERROR_WAIT_TIMEOUT)
            {
                continue;
            }

            if (hr == DXGI_ERROR_ACCESS_LOST)
            {
                std::cerr
                    << "DXGI access lost\n";

                break;
            }

            if (FAILED(hr))
            {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(2));

                continue;
            }

            ComPtr<ID3D11Texture2D> desktopTexture;

            if (FAILED(resource.As(
                &desktopTexture)))
            {
                duplication->ReleaseFrame();

                continue;
            }

            context->CopyResource(
                cpuTex.Get(),
                desktopTexture.Get());

            D3D11_MAPPED_SUBRESOURCE mapped{};

            if (SUCCEEDED(context->Map(
                cpuTex.Get(),
                0,
                D3D11_MAP_READ,
                0,
                &mapped)))
            {
                {
                    std::lock_guard<std::mutex> lock(
                        latestMutex);

                    if (av_frame_make_writable(
                        latestFrame) >= 0)
                    {
                        convertBGRAtoYUV420P(
                            static_cast<const uint8_t*>(
                                mapped.pData),

                            static_cast<int>(
                                mapped.RowPitch),

                            latestFrame,

                            width,
                            height);

                        latestReady = true;
                    }
                }

                context->Unmap(
                    cpuTex.Get(),
                    0);
            }

            duplication->ReleaseFrame();
        }
    });

    /*
        ------------------------------------------------------------
        ENCODER THREAD
        ------------------------------------------------------------
    */

    std::thread encoder([&]()
    {
        AVPacket* packet =
            av_packet_alloc();

        while (true)
        {
            AVFrame* frame = nullptr;

            {
                std::unique_lock<std::mutex> lock(
                    encodeMutex);

                encodeCV.wait(
                    lock,
                    [&]()
                    {
                        return stopRecording.load() ||
                               !encodeQueue.empty();
                    });

                if (encodeQueue.empty())
                {
                    if (stopRecording.load())
                        break;

                    continue;
                }

                frame =
                    encodeQueue.front();

                encodeQueue.pop_front();
            }

            int ret =
                avcodec_send_frame(
                    codecCtx,
                    frame);

            pool.release(frame);

            if (ret < 0)
                continue;

            while (true)
            {
                ret =
                    avcodec_receive_packet(
                        codecCtx,
                        packet);

                if (ret == AVERROR(EAGAIN) ||
                    ret == AVERROR_EOF)
                {
                    break;
                }

                if (ret < 0)
                    break;

                packet->stream_index =
                    stream->index;

                av_packet_rescale_ts(
                    packet,
                    codecCtx->time_base,
                    stream->time_base);

                av_interleaved_write_frame(
                    outCtx,
                    packet);

                av_packet_unref(packet);
            }
        }

        /*
            Flush encoder.
        */

        avcodec_send_frame(
            codecCtx,
            nullptr);

        while (avcodec_receive_packet(
            codecCtx,
            packet) == 0)
        {
            packet->stream_index =
                stream->index;

            av_packet_rescale_ts(
                packet,
                codecCtx->time_base,
                stream->time_base);

            av_interleaved_write_frame(
                outCtx,
                packet);

            av_packet_unref(packet);
        }

        av_packet_free(&packet);
    });

    /*
        ------------------------------------------------------------
        QPC CLOCK
        ------------------------------------------------------------

        Start the recording clock HERE, immediately before
        the output scheduler starts.

        This avoids starting the 60 FPS timeline several
        seconds before the worker threads are ready.
    */

    LARGE_INTEGER qpcFrequency{};

    if (!QueryPerformanceFrequency(
        &qpcFrequency))
    {
        std::cerr
            << "QueryPerformanceFrequency failed\n";

        stopRecording = true;
    }

    LARGE_INTEGER recordingStartCounter{};

    if (!stopRecording.load())
    {
        QueryPerformanceCounter(
            &recordingStartCounter);
    }

    /*
        ------------------------------------------------------------
        OUTPUT THREAD
        ------------------------------------------------------------
    */

    std::thread outputThread([&]()
    {
        int64_t frameNumber = 0;

        const double ticksPerFrame =
            static_cast<double>(
                qpcFrequency.QuadPart) /
            static_cast<double>(fps);

        while (!stopRecording.load())
        {
            /*
                Exact 60 FPS target time.
            */

            const double targetTicks =
                static_cast<double>(
                    recordingStartCounter.QuadPart) +
                static_cast<double>(
                    frameNumber) *
                ticksPerFrame;

            LARGE_INTEGER nowCounter{};

            QueryPerformanceCounter(
                &nowCounter);

            double remainingTicks =
                targetTicks -
                static_cast<double>(
                    nowCounter.QuadPart);

            if (remainingTicks > 0.0)
            {
                double remainingMilliseconds =
                    remainingTicks * 1000.0 /
                    static_cast<double>(
                        qpcFrequency.QuadPart);

                if (remainingMilliseconds > 2.0)
                {
                    DWORD sleepMilliseconds =
                        static_cast<DWORD>(
                            remainingMilliseconds - 1.0);

                    if (sleepMilliseconds > 0)
                    {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(
                                sleepMilliseconds));
                    }
                }
                else
                {
                    std::this_thread::yield();
                }

                continue;
            }

            /*
                Get the newest available desktop frame.
            */

            AVFrame* outputFrame =
                pool.acquire();

            if (!outputFrame)
                break;

            bool gotFrame = false;

            {
                std::lock_guard<std::mutex> lock(
                    latestMutex);

                if (latestReady)
                {
                    if (av_frame_ref(
                        outputFrame,
                        latestFrame) >= 0)
                    {
                        /*
                            CFR PTS.

                            0 = 0.000000
                            1 = 0.016666
                            2 = 0.033333
                            etc.
                        */

                        outputFrame->pts =
                            frameNumber;

                        gotFrame = true;
                    }
                }
            }

            if (gotFrame)
            {
                std::lock_guard<std::mutex> lock(
                    encodeMutex);

                /*
                    Keep encoder latency very small.

                    Never allow seconds of stale frames
                    to accumulate.
                */

                if (encodeQueue.size() <
                    maxEncodeQueue)
                {
                    encodeQueue.push_back(
                        outputFrame);

                    encodeCV.notify_one();
                }
                else
                {
                    pool.release(
                        outputFrame);
                }
            }
            else
            {
                pool.release(
                    outputFrame);
            }

            ++frameNumber;
        }
    });

    /*
        ------------------------------------------------------------
        START
        ------------------------------------------------------------
    */

    std::cout
        << "CBRecorderZero framefill8 running\n"
        << "1600x900 @ 60 FPS CFR\n"
        << "NVENC 18 Mbps\n"
        << "Latest-frame capture\n"
        << "Small encoder queue\n"
        << "Press CTRL+C to stop\n"
        << "\n";

    while (!stopRecording.load())
    {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(100));
    }

    /*
        ------------------------------------------------------------
        STOP
        ------------------------------------------------------------
    */

    stopRecording = true;

    encodeCV.notify_all();

    captureThread.join();

    outputThread.join();

    /*
        Release anything remaining in the queue.
    */

    {
        std::lock_guard<std::mutex> lock(
            encodeMutex);

        while (!encodeQueue.empty())
        {
            AVFrame* f =
                encodeQueue.front();

            encodeQueue.pop_front();

            pool.release(f);
        }
    }

    encodeCV.notify_all();

    encoder.join();

    av_frame_free(
        &latestFrame);

    av_write_trailer(
        outCtx);

    avcodec_free_context(
        &codecCtx);

    if (!(outCtx->oformat->flags &
          AVFMT_NOFILE))
    {
        avio_close(
            outCtx->pb);
    }

    avformat_free_context(
        outCtx);

    std::cout
        << "\nDONE: "
        << filename
        << "\n";

    return 0;
}