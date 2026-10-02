// $ g++ -std=c++17 -O2 -I/c/Users/user/Downloads/ffmpeg-5.1.10 -o cbrecorderzero_60fps_framefill7.exe cbrecorderzero_60fps_framefill7.cpp /c/Users/user/Downloads/ffmpeg-5.1.10/libavcodec/libavcodec.dll.a /c/Users/user/Downloads/ffmpeg-5.1.10/libavformat/libavformat.dll.a /c/Users/user/Downloads/ffmpeg-5.1.10/libavutil/libavutil.dll.a -ld3d11 -ldxgi -lole32





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

struct FrameItem
{
    AVFrame* frame;
    int64_t pts;
};

class FrameQueue
{
public:

    FrameQueue(size_t maximum)
        : maxSize(maximum)
    {
    }

    void push(FrameItem item)
    {
        std::unique_lock<std::mutex> lock(mtx);

        if (q.size() >= maxSize)
        {
            AVFrame* old = q.front().frame;
            q.pop();

            if (old)
                av_frame_free(&old);
        }

        q.push(item);

        cv.notify_one();
    }

    bool pop(FrameItem& item)
    {
        std::unique_lock<std::mutex> lock(mtx);

        while (q.empty() && !stopRecording.load())
        {
            cv.wait(lock);
        }

        if (q.empty())
            return false;

        item = q.front();
        q.pop();

        return true;
    }

    bool empty()
    {
        std::lock_guard<std::mutex> lock(mtx);
        return q.empty();
    }

    void wakeAll()
    {
        cv.notify_all();
    }

private:

    std::queue<FrameItem> q;

    std::mutex mtx;
    std::condition_variable cv;

    size_t maxSize;
};

class FramePool
{
public:

    FramePool(int count)
    {
        for (int i = 0; i < count; ++i)
        {
            AVFrame* f = av_frame_alloc();

            if (f)
                freeFrames.push(f);
        }
    }

    ~FramePool()
    {
        while (!freeFrames.empty())
        {
            AVFrame* f = freeFrames.front();
            freeFrames.pop();

            av_frame_free(&f);
        }
    }

    AVFrame* acquire()
    {
        std::lock_guard<std::mutex> lock(mtx);

        if (freeFrames.empty())
            return av_frame_alloc();

        AVFrame* f = freeFrames.front();

        freeFrames.pop();

        return f;
    }

    void release(AVFrame* f)
    {
        if (!f)
            return;

        av_frame_unref(f);

        std::lock_guard<std::mutex> lock(mtx);

        freeFrames.push(f);
    }

private:

    std::queue<AVFrame*> freeFrames;

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
        HIGH-RESOLUTION WINDOWS CLOCK

        We use QueryPerformanceCounter instead of a manually
        accumulated sleep interval for the recording timeline.

        The video remains exactly 60 FPS CFR, but the scheduler
        is tied to the actual Windows high-resolution clock.
    */

    LARGE_INTEGER qpcFrequency{};

    if (!QueryPerformanceFrequency(&qpcFrequency))
    {
        std::cerr
            << "QueryPerformanceFrequency failed\n";

        return -1;
    }

    LARGE_INTEGER recordingStartCounter{};

    if (!QueryPerformanceCounter(&recordingStartCounter))
    {
        std::cerr
            << "QueryPerformanceCounter failed\n";

        return -1;
    }

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
        EXACT 60 FPS VIDEO TIME BASE
    */

    codecCtx->time_base =
        AVRational{1, fps};

    codecCtx->framerate =
        AVRational{fps, 1};

    codecCtx->thread_count = 4;

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
        LAST CAPTURED FRAME

        The capture thread continuously replaces this
        with the newest available desktop image.
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

    std::condition_variable latestCV;

    bool latestReady = false;

    FrameQueue queue(120);

    FramePool pool(300);

    /*
        ENCODER THREAD
    */

    std::thread encoder([&]()
    {
        AVPacket* packet =
            av_packet_alloc();

        FrameItem item;

        while (!stopRecording.load() ||
               !queue.empty())
        {
            if (!queue.pop(item))
                continue;

            int ret =
                avcodec_send_frame(
                    codecCtx,
                    item.frame);

            pool.release(item.frame);

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
        CAPTURE THREAD

        DXGI captures frames whenever they actually
        become available.

        This thread has NO artificial 60 FPS clock.
    */

    std::thread captureThread([&]()
    {
        while (!stopRecording.load())
        {
            DXGI_OUTDUPL_FRAME_INFO frameInfo{};

            ComPtr<IDXGIResource> resource;

            HRESULT hr =
                duplication->AcquireNextFrame(
                    5,
                    &frameInfo,
                    &resource);

            if (hr == DXGI_ERROR_WAIT_TIMEOUT)
            {
                continue;
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

                latestCV.notify_one();
            }

            duplication->ReleaseFrame();
        }
    });

    /*
        OUTPUT THREAD

        IMPORTANT:

        The video remains exactly 60 FPS CFR.

        However, the schedule is now based on the
        Windows high-resolution performance counter.

        We NEVER reset the schedule to "now" when
        the thread falls behind.

        Each frame represents one exact 1/60-second
        position on the recording timeline.
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
                Calculate the exact QPC time at which
                this frame belongs.
            */

            const double targetTicksDouble =
                static_cast<double>(
                    recordingStartCounter.QuadPart) +
                static_cast<double>(frameNumber) *
                ticksPerFrame;

            LARGE_INTEGER nowCounter{};

            QueryPerformanceCounter(
                &nowCounter);

            double remainingTicks =
                targetTicksDouble -
                static_cast<double>(
                    nowCounter.QuadPart);

            /*
                Sleep while there is substantial time
                remaining.

                For the final small interval, yield
                instead of sleeping through the target.
            */

            if (remainingTicks > 0.0)
            {
                double remainingMilliseconds =
                    (remainingTicks * 1000.0) /
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

                    continue;
                }

                std::this_thread::yield();
                continue;
            }

            if (stopRecording.load())
                break;

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
                            PTS is the CFR frame number.

                            0 = 0.000000 sec
                            1 = 0.016666 sec
                            2 = 0.033333 sec
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
                queue.push({
                    outputFrame,
                    outputFrame->pts
                });
            }
            else
            {
                pool.release(outputFrame);
            }

            /*
                Advance exactly ONE 60 FPS timeline
                position.

                There is deliberately NO:

                    nextFrameTime = now + frameDuration

                because that was capable of changing
                the relationship between the recording
                timeline and real elapsed time.
            */

            ++frameNumber;
        }
    });

    std::cout
        << "CBRecorderZero running\n"
        << "1600x900 @ 60 FPS\n"
        << "NVENC 18 Mbps\n"
        << "High-resolution QPC timing\n"
        << "Press CTRL+C to stop\n"
        << "\n";

    while (!stopRecording.load())
    {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(100));
    }

    /*
        STOP
    */

    stopRecording = true;

    latestCV.notify_all();

    queue.wakeAll();

    captureThread.join();

    outputThread.join();

    queue.wakeAll();

    encoder.join();

    av_frame_free(&latestFrame);

    av_write_trailer(outCtx);

    avcodec_free_context(
        &codecCtx);

    if (!(outCtx->oformat->flags &
          AVFMT_NOFILE))
    {
        avio_close(outCtx->pb);
    }

    avformat_free_context(outCtx);

    std::cout
        << "\nDONE: "
        << filename
        << "\n";

    return 0;
}