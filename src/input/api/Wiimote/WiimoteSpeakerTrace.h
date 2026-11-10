#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace WiimoteSpeakerTrace
{
    enum class Stage : std::uint8_t { Source = 1, Queued = 2, Wire = 3, Timing = 4, Probe = 5 };
    inline bool Enabled()
    {
        static const bool enabled = std::getenv("CEMU_WIIMOTE_SPEAKER_SAMPLE_TRACE") != nullptr;
        return enabled;
    }
    inline std::uint64_t Microseconds()
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    template<class Buffer> void Little(Buffer& bytes, std::uint64_t value, unsigned width)
    {
        for (unsigned i = 0; i < width; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
    }
    inline std::vector<std::uint8_t> Header(Stage stage, std::uint8_t flags, std::uint64_t time,
                                           std::uint64_t key, std::uint32_t count, std::uint32_t length)
    {
        std::vector<std::uint8_t> bytes;
        bytes.reserve(32);
        Little(bytes, 0x314b5053, 4); Little(bytes, 1, 2);
        bytes.push_back(static_cast<std::uint8_t>(stage)); bytes.push_back(flags);
        Little(bytes, time, 8); Little(bytes, key, 8); Little(bytes, count, 4); Little(bytes, length, 4);
        return bytes;
    }
    class Recorder
    {
    public:
        static constexpr std::size_t Limit = 8 * 1024 * 1024;
        Recorder()
        {
            if (!Enabled()) return;
            m_path = std::filesystem::current_path() / ("wiimote-speaker-" + std::to_string(Microseconds()) + "-" +
                std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) + ".spk");
            m_bytes.reserve(Limit);
        }
        ~Recorder()
        {
            Flush();
        }
        void Flush()
        {
            std::vector<std::uint8_t> bytes;
            {
                std::scoped_lock lock(m_mutex);
                if (m_closed) return;
                m_closed = true;
                bytes = std::move(m_bytes);
            }
            // Also called explicitly at app shutdown: HID workers can outlive OnExit.
            if (bytes.empty()) return;
            try
            {
                std::ofstream output(m_path, std::ios::binary | std::ios::trunc);
                output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            }
            catch (...) {}
        }
        void Write(Stage stage, std::uint64_t key, std::uint8_t flags, std::uint32_t count,
                   std::span<const std::uint8_t> data)
        {
            std::scoped_lock lock(m_mutex);
            if (m_closed) return;
            if (data.size() + 32 > Limit - m_bytes.size()) return;
            const auto header = Header(stage, flags, Microseconds(), key, count, static_cast<std::uint32_t>(data.size()));
            m_bytes.insert(m_bytes.end(), header.begin(), header.end());
            m_bytes.insert(m_bytes.end(), data.begin(), data.end());
        }
    private:
        std::filesystem::path m_path;
        std::vector<std::uint8_t> m_bytes;
        std::mutex m_mutex;
        bool m_closed = false;
    };
    struct Registry
    {
        std::mutex mutex;
        std::vector<std::weak_ptr<Recorder>> recorders;
    };
    inline Registry& AllRecorders()
    {
        // The process has an explicit fast-exit path; avoid static destruction order.
        static auto* registry = new Registry;
        return *registry;
    }
    inline std::shared_ptr<Recorder> NewRecorder()
    {
        auto recorder = std::make_shared<Recorder>();
        auto& registry = AllRecorders();
        std::scoped_lock lock(registry.mutex);
        std::erase_if(registry.recorders, [](const auto& entry) { return entry.expired(); });
        registry.recorders.push_back(recorder);
        return recorder;
    }
    inline void FlushAll()
    {
        if (!Enabled()) return;
        std::vector<std::shared_ptr<Recorder>> recorders;
        auto& registry = AllRecorders();
        {
            std::scoped_lock lock(registry.mutex);
            for (auto& entry : registry.recorders)
                if (auto recorder = entry.lock()) recorders.push_back(std::move(recorder));
        }
        for (auto& recorder : recorders) recorder->Flush();
    }
    inline Recorder& ThreadRecorder()
    {
        thread_local auto recorder = NewRecorder();
        return *recorder;
    }
    inline void Packet(Stage stage, std::uint64_t device, bool success, std::span<const std::uint8_t> bytes,
                       std::uint32_t duration = 0)
    {
        if (!Enabled()) return;
        ThreadRecorder().Write(stage, device, success ? 1 : 0, duration, bytes);
    }
    inline void Timing(std::uint64_t device, std::chrono::steady_clock::duration wait,
                       std::chrono::steady_clock::duration registry, std::chrono::steady_clock::duration selection)
    {
        if (!Enabled()) return;
        std::vector<std::uint8_t> data;
        data.reserve(24);
        for (auto duration : {wait, registry, selection})
            Little(data, std::chrono::duration_cast<std::chrono::microseconds>(duration).count(), 8);
        ThreadRecorder().Write(Stage::Timing, device, 1, 0, data);
    }
    inline void Source(std::uint64_t key, unsigned mode, std::int32_t predictor, std::int32_t step,
                       std::int32_t afterPredictor, std::int32_t afterStep,
                       std::span<const std::uint8_t> pcmBigEndian, std::span<const std::uint8_t> encoded)
    {
        if (!Enabled()) return;
        std::vector<std::uint8_t> data;
        data.reserve(16 + pcmBigEndian.size() + encoded.size());
        for (auto value : {predictor, step, afterPredictor, afterStep}) Little(data, static_cast<std::uint32_t>(value), 4);
        data.insert(data.end(), pcmBigEndian.begin(), pcmBigEndian.end());
        data.insert(data.end(), encoded.begin(), encoded.end());
        ThreadRecorder().Write(Stage::Source, key, static_cast<std::uint8_t>(mode), static_cast<std::uint32_t>(pcmBigEndian.size() / 2), data);
    }
}
