#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <utility>

namespace salacia {

// 无锁单生产者/单消费者（SPSC）环形缓冲区
//
// 设计约束（工业级多线程规范红线）：
//  - 仅允许一个生产者线程 push、一个消费者线程 pop。核心场景：
//    GStreamer 解码线程（生产者）-> AI 推理线程（消费者）的视频帧通道；
//  - 无锁且 wait-free：push/pop 恒不阻塞调用线程；
//  - head_/tail_ 各自独占缓存行（alignas(64)），消除伪共享；
//  - 缓冲满时 push 返回 false（低延迟优先，丢弃策略由调用方决定），
//    缓冲空时 pop 返回 false；
//  - T 需可默认构造、可移动/复制（如承载 std::vector 的帧结构）；
//    RingBuffer 本身不可复制、不可移动（含原子成员）。
//
// 内存序说明：索引的 release 写与对侧的 acquire 读配对，保证槽位内
// 数据的写入先于索引推进对消费侧可见；同侧读取自身索引用 relaxed。
template <typename T, std::size_t Capacity>
class RingBuffer
{
    static_assert(Capacity >= 2U, "Capacity must be >= 2");
    static_assert((Capacity & (Capacity - 1U)) == 0U,
                  "Capacity must be a power of two");

public:
    RingBuffer() = default;
    ~RingBuffer() = default;

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&&) = delete;
    RingBuffer& operator=(RingBuffer&&) = delete;

    // 生产者线程调用；缓冲满返回 false（未写入）
    bool push(T&& item)
    {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);

        if ((head - tail) == Capacity) {
            return false; // 满：调用方按策略丢弃
        }

        slots_[head & kIndexMask] = std::move(item);
        head_.store(head + 1U, std::memory_order_release);
        return true;
    }

    bool push(const T& item)
    {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);

        if ((head - tail) == Capacity) {
            return false;
        }

        slots_[head & kIndexMask] = item;
        head_.store(head + 1U, std::memory_order_release);
        return true;
    }

    // 消费者线程调用；缓冲空返回 false（未读出）
    bool pop(T& out)
    {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);

        if (tail == head) {
            return false; // 空
        }

        out = std::move(slots_[tail & kIndexMask]);
        tail_.store(tail + 1U, std::memory_order_release);
        return true;
    }

    // 近似计数：仅用于统计/诊断显示，禁止用于同步决策
    std::size_t size() const
    {
        return head_.load(std::memory_order_acquire)
             - tail_.load(std::memory_order_acquire);
    }

    bool empty() const { return size() == 0U; }

    static constexpr std::size_t capacity() { return Capacity; }

private:
    static constexpr std::size_t kIndexMask = Capacity - 1U;

    alignas(64) std::atomic<std::size_t> head_{0}; // 写指针：仅生产者写
    alignas(64) std::atomic<std::size_t> tail_{0}; // 读指针：仅消费者写
    alignas(64) std::array<T, Capacity> slots_{};  // 数据槽
};

} // namespace salacia
