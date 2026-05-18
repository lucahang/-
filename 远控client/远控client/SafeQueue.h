#pragma once // 防止头文件被重复包含
#include <queue> // 基础队列容器
#include <mutex> // 互斥锁，用于多线程同步
#include <utility> // 提供 std::move 移动语义支持

template<typename T> // 模板类，T 可以是任何类型（如 int, cv::Mat, std::vector 等）
class SafeQueue {
private:
    std::queue<T> q;       // 标准库容器，存储实际数据
    std::mutex m;          // 互斥锁，保护 q 的并发访问

    // 关键点：限制队列最大容量。
    // 在远控系统中，如果网络卡顿产生堆积，我们宁愿丢弃旧帧（延迟），也不要显示过时的画面。
    const size_t max_size = 2;

public:
    // 获取队列当前大小
    // 必须加锁，因为 std::queue::size() 本身不是线程安全的
    int size() {
        std::lock_guard<std::mutex> lock(m); // 自动上锁，函数结束自动解锁
        return (int)q.size();
    }

    // 生产者：向队列压入数据
    void push(T val) {
        std::lock_guard<std::mutex> lock(m);

        // 策略：如果队列满了，循环弹出最旧的数据（队头）
        // 这保证了队列里永远是最新的数据，从而降低画面延迟
        while (q.size() >= max_size) {
            q.pop(); // 丢弃最旧帧
        }

        // 使用 emplace + move 减少对象拷贝开销，提高性能
        q.emplace(std::move(val));
    }

    // 消费者：从队列取出数据
    // 返回 bool 值表示是否成功获取
    bool pop(T& val) {
        std::lock_guard<std::mutex> lock(m);

        if (q.empty()) return false; // 队列为空直接返回

        // 使用移动语义将队头元素转移给外部变量 val，避免大内存拷贝
        val = std::move(q.front());
        q.pop(); // 移除队头
        return true;
    }

    // 判断队列是否为空
    bool empty() {
        std::lock_guard<std::mutex> lock(m);
        return q.empty();
    }

    // 清空队列
    // 常用于网络重连或切换视频源时，防止旧数据干扰
    void clear() {
        std::lock_guard<std::mutex> lock(m);
        // 一种高效的清空 queue 的技巧：与一个空的 queue 进行交换
        std::queue<T>().swap(q);
    }
};