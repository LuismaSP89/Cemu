#pragma once
#include <atomic>
#include <mutex>
#include <shared_mutex>

// One HID output worker; input polling/discovery must yield to queued audio.
class WiimoteOutputGate
{
public:
    template<class Mutex> std::unique_lock<Mutex> LockInput(Mutex& mutex)
    {
        std::unique_lock<Mutex> lock(mutex, std::defer_lock);
        for (;;)
        {
            m_audioWaiting.wait(true, std::memory_order_acquire);
            lock.lock();
            if (!m_audioWaiting.load(std::memory_order_acquire)) return lock;
            lock.unlock();
        }
    }

    template<class Mutex> std::shared_lock<Mutex> LockOutput(Mutex& mutex, bool audio)
    {
        if (!audio) return std::shared_lock<Mutex>(mutex);
        m_audioWaiting.store(true, std::memory_order_release);
        try
        {
            std::shared_lock<Mutex> lock(mutex);
            ReleaseWaiters();
            return lock;
        }
        catch (...)
        {
            ReleaseWaiters();
            throw;
        }
    }

    bool AudioWaiting() const { return m_audioWaiting.load(std::memory_order_acquire); }

private:
    void ReleaseWaiters()
    {
        m_audioWaiting.store(false, std::memory_order_release);
        m_audioWaiting.notify_all();
    }
    std::atomic_bool m_audioWaiting{false};
};
