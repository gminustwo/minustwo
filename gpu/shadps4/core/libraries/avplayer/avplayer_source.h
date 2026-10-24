// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string_view>
#include <utility>
#include <vector>

#include "common/assert.h"
#include "core/libraries/avplayer/avplayer.h"
#include "core/libraries/avplayer/avplayer_common.h"
#include "core/libraries/avplayer/avplayer_data_streamer.h"
#include "core/libraries/kernel/threads.h"

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVIOContext;
struct AVPacket;
struct SwrContext;
struct SwsContext;

namespace Libraries::AvPlayer {

class AvPlayerStateCallback {
public:
    virtual ~AvPlayerStateCallback() = default;

    virtual AvPlayerAvSyncMode GetSyncMode() = 0;

    virtual void OnWarning(u32 id) = 0;
    virtual void OnError() = 0;
    virtual void OnEOF() = 0;
};

class GuestBuffer {
public:
    GuestBuffer(const AvPlayerMemAllocator& memory_replacement, u32 align, u32 size,
                bool is_texture) noexcept
        : m_memory_replacement(memory_replacement),
          m_data(is_texture ? AllocateTexture(memory_replacement, align, size)
                            : Allocate(memory_replacement, align, size)),
          m_size(size), m_is_texture(is_texture) {
        ASSERT_MSG(m_data, "Could not allocate frame buffer.");
    }

    ~GuestBuffer() {
        if (m_data != nullptr) {
            if (m_is_texture) {
                DeallocateTexture(m_memory_replacement, m_data);
            } else {
                Deallocate(m_memory_replacement, m_data);
            }
            m_data = {};
        }
    }

    GuestBuffer(const GuestBuffer&) noexcept = delete;
    GuestBuffer& operator=(const GuestBuffer&) noexcept = delete;

    GuestBuffer(GuestBuffer&& r) noexcept
        : m_memory_replacement(r.m_memory_replacement), m_data(r.m_data), m_size(r.m_size),
          m_is_texture(r.m_is_texture) {
        r.m_data = nullptr;
    };

    GuestBuffer& operator=(GuestBuffer&& r) noexcept {
        std::swap(m_data, r.m_data);
        return *this;
    }

    u8* GetBuffer() const noexcept {
        return m_data;
    }

    u64 Size() const noexcept {
        return m_size;
    }

private:
    static u8* Allocate(const AvPlayerMemAllocator& memory_replacement, u32 align, u32 size) {
        return reinterpret_cast<u8*>(
            memory_replacement.allocate(memory_replacement.object_ptr, align, size));
    }

    static void Deallocate(const AvPlayerMemAllocator& memory_replacement, void* ptr) {
        memory_replacement.deallocate(memory_replacement.object_ptr, ptr);
    }

    static u8* AllocateTexture(const AvPlayerMemAllocator& memory_replacement, u32 align,
                               u32 size) {
        return reinterpret_cast<u8*>(
            memory_replacement.allocate_texture(memory_replacement.object_ptr, align, size));
    }

    static void DeallocateTexture(const AvPlayerMemAllocator& memory_replacement, void* ptr) {
        memory_replacement.deallocate_texture(memory_replacement.object_ptr, ptr);
    }

    const AvPlayerMemAllocator& m_memory_replacement;
    u8* m_data = nullptr;
    u64 m_size = 0;
    bool m_is_texture = false;
};

struct Frame {
    GuestBuffer buffer;
    AvPlayerFrameInfoEx info;
};

/// minustwo: a decoded video frame waiting in host memory (NV12 as the game gets it); it is
/// copied to one of the game's frame buffers when it is handed out.
struct VideoFrame {
    std::vector<u8> data;
    AvPlayerFrameInfoEx info;
    /// The presentation time in milliseconds, unrounded (info.timestamp is whole ms).
    double time_ms = 0.0;
};

class EventCV {
public:
    template <class Pred>
    void Wait(Pred pred) {
        std::unique_lock lock(m_mutex);
        m_cv.wait(lock, std::move(pred));
    }

    template <class Pred>
    bool Wait(std::stop_token stop, Pred pred) {
        std::unique_lock lock(m_mutex);
        return m_cv.wait(lock, std::move(stop), std::move(pred));
    }

    template <class Pred, class Rep, class Period>
    bool WaitFor(std::chrono::duration<Rep, Period> timeout, Pred pred) {
        std::unique_lock lock(m_mutex);
        return m_cv.wait_for(lock, timeout, std::move(pred));
    }

    void Notify() {
        std::unique_lock lock(m_mutex);
        m_cv.notify_all();
    }

private:
    std::mutex m_mutex{};
    std::condition_variable_any m_cv{};
};

class AvPlayerSource {
public:
    AvPlayerSource(AvPlayerStateCallback& state);
    ~AvPlayerSource();

    bool Init(const AvPlayerInitData& init_data, std::string_view path);
    bool FindStreams();
    s32 GetStreamCount();
    bool GetStreamInfo(u32 stream_index, AvPlayerStreamInfo& info);
    bool EnableStream(u32 stream_index);
    void SetLooping(bool is_looping);
    std::optional<bool> HasFrames(u32 num_frames);
    bool Start();
    bool Stop();
    void Pause();
    void Resume();
    bool GetAudioData(AvPlayerFrameInfo& audio_info);
    bool GetVideoData(AvPlayerFrameInfo& video_info);
    bool GetVideoData(AvPlayerFrameInfoEx& video_info);
    u64 CurrentTime();
    /// minustwo: the playback clock in milliseconds at `at` (wall time since the start or the
    /// last loop, without pauses).
    double ClockMs(std::chrono::high_resolution_clock::time_point at) const;
    bool IsActive();

private:
    u64 DurationMillis() const;
    AvPlayerStreamInfo CreateStreamInfo(u32 stream_index);

    static void ReleaseAVPacket(AVPacket* packet);
    static void ReleaseAVFrame(AVFrame* frame);
    static void ReleaseAVCodecContext(AVCodecContext* context);
    static void ReleaseSWRContext(SwrContext* context);
    static void ReleaseSWSContext(SwsContext* context);
    static void ReleaseAVFormatContext(AVFormatContext* context);

    using AVPacketPtr = std::unique_ptr<AVPacket, decltype(&ReleaseAVPacket)>;
    using AVFramePtr = std::unique_ptr<AVFrame, decltype(&ReleaseAVFrame)>;
    using AVCodecContextPtr = std::unique_ptr<AVCodecContext, decltype(&ReleaseAVCodecContext)>;
    using SWRContextPtr = std::unique_ptr<SwrContext, decltype(&ReleaseSWRContext)>;
    using SWSContextPtr = std::unique_ptr<SwsContext, decltype(&ReleaseSWSContext)>;
    using AVFormatContextPtr = std::unique_ptr<AVFormatContext, decltype(&ReleaseAVFormatContext)>;

    void DemuxerThread(std::stop_token stop);
    void VideoDecoderThread(std::stop_token stop);
    void AudioDecoderThread(std::stop_token stop);

    bool HasRunningThreads() const;

    AVFramePtr ConvertAudioFrame(const AVFrame& frame);
    AVFramePtr ConvertVideoFrame(const AVFrame& frame);

    Frame PrepareAudioFrame(GuestBuffer buffer, const AVFrame& frame);
    VideoFrame PrepareVideoFrame(const AVFrame& frame);

    AvPlayerStateCallback& m_state;

    struct Stream {
        size_t ffmpeg_index = 0;
        AvPlayerStreamInfo info;
    };

    std::vector<Stream> m_streams;
    u64 m_duration = 0;

    AvPlayerMemAllocator m_memory_replacement{};
    u32 m_max_num_video_framebuffers{};

    std::atomic_bool m_is_looping = false;
    std::atomic_bool m_is_paused = false;
    std::atomic_bool m_is_eof = false;

    std::unique_ptr<IDataStreamer> m_up_data_streamer;

    AvPlayerQueue<GuestBuffer> m_audio_buffers;
    // minustwo: the game's video frame buffers, handed out in turn: the buffer of a frame is
    // written again only after the others were (see GetVideoData); the size of one.
    std::vector<GuestBuffer> m_video_buffers;
    size_t m_next_video_buffer = 0;
    u32 m_video_buffer_size = 0;

    AvPlayerQueue<AVPacketPtr> m_audio_packets;
    AvPlayerQueue<AVPacketPtr> m_video_packets;

    AvPlayerQueue<Frame> m_audio_frames;
    AvPlayerQueue<VideoFrame> m_video_frames;

    std::optional<Frame> m_current_audio_frame;

    std::optional<s32> m_video_stream_index{};
    std::optional<s32> m_audio_stream_index{};

    EventCV m_audio_packets_cv{};
    EventCV m_audio_frames_cv{};
    EventCV m_audio_buffers_cv{};

    EventCV m_video_packets_cv{};
    EventCV m_video_frames_cv{};
    EventCV m_video_buffers_cv{};

    std::mutex m_state_mutex{};
    Kernel::Thread m_demuxer_thread{};
    Kernel::Thread m_video_decoder_thread{};
    Kernel::Thread m_audio_decoder_thread{};

    AVFormatContextPtr m_avformat_context{nullptr, &ReleaseAVFormatContext};
    AVCodecContextPtr m_video_codec_context{nullptr, &ReleaseAVCodecContext};
    AVCodecContextPtr m_audio_codec_context{nullptr, &ReleaseAVCodecContext};
    SWRContextPtr m_swr_context{nullptr, &ReleaseSWRContext};
    SWSContextPtr m_sws_context{nullptr, &ReleaseSWSContext};

    std::optional<std::chrono::high_resolution_clock::time_point> m_start_time{};
    std::chrono::high_resolution_clock::time_point m_pause_time{};
    std::chrono::high_resolution_clock::duration m_pause_duration{};
    // minustwo: the clock's origin (stream time at m_start_time); how far it was set back for
    // audio that fell behind, and that it was (both set by the audio side); the video clock the
    // game's asks read (stream ms) and the instant of the ask it last stepped at (filtered); the
    // last frame handed out.
    u64 m_clock_base_ts = 0;
    std::atomic<s64> m_audio_lag_ns{0};
    std::atomic_bool m_clock_resync{false};
    std::optional<double> m_video_clock{};
    std::chrono::high_resolution_clock::time_point m_video_clock_time{};
    std::optional<u64> m_last_video_ts{};
};

} // namespace Libraries::AvPlayer
