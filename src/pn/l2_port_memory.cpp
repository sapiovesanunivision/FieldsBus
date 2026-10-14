// In-memory "cable" between two L2Ports, for tests without a NIC (pn_selftest).
#include "softpn/l2_port.hpp"

#include "pn_defs.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace softpn {
namespace {

// One direction of the cable.
struct Pipe {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::vector<uint8_t>> frames;
    bool interrupted = false;
};

class MemoryPort : public L2Port {
public:
    MemoryPort(AdapterInfo info, std::shared_ptr<Pipe> rx, std::shared_ptr<Pipe> tx)
        : info_(std::move(info)), rx_(std::move(rx)), tx_(std::move(tx))
    {
    }

    bool send(const uint8_t* frame, size_t len) override
    {
        std::vector<uint8_t> f(frame, frame + len);
        if (f.size() < kMinFrameSize)
            f.resize(kMinFrameSize, 0);
        {
            std::lock_guard<std::mutex> lock(tx_->m);
            if (tx_->frames.size() > 4096) // nobody is reading: behave like a full NIC buffer
                return false;
            tx_->frames.push_back(std::move(f));
        }
        tx_->cv.notify_one();
        return true;
    }

    bool receive(std::vector<uint8_t>& frame, int timeoutMs) override
    {
        std::unique_lock<std::mutex> lock(rx_->m);
        rx_->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                         [this] { return rx_->interrupted || !rx_->frames.empty(); });
        if (rx_->interrupted) {
            rx_->interrupted = false;
            return false;
        }
        if (rx_->frames.empty())
            return false;
        frame = std::move(rx_->frames.front());
        rx_->frames.pop_front();
        return true;
    }

    void interrupt() override
    {
        {
            std::lock_guard<std::mutex> lock(rx_->m);
            rx_->interrupted = true;
        }
        rx_->cv.notify_all();
    }

    const AdapterInfo& info() const override { return info_; }

private:
    AdapterInfo info_;
    std::shared_ptr<Pipe> rx_;
    std::shared_ptr<Pipe> tx_;
};

} // namespace

std::pair<std::unique_ptr<L2Port>, std::unique_ptr<L2Port>> makeMemoryCable(const AdapterInfo& a,
                                                                            const AdapterInfo& b)
{
    auto aToB = std::make_shared<Pipe>();
    auto bToA = std::make_shared<Pipe>();
    return {std::make_unique<MemoryPort>(a, bToA, aToB), std::make_unique<MemoryPort>(b, aToB, bToA)};
}

} // namespace softpn
