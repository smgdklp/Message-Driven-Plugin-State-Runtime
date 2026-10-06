/* ============================================================================
 *  mdpsr/test/queue/queue_test.cpp
 *  消息队列单元测试 (独立可执行)
 *
 *  验证 Queue 这条「可拓展环形字节缓冲」的帧语义:
 *      int32_t _len (= 12 + body) | uint64_t _handle | body[]
 *      - 先进先出, 二进制安全
 *      - 超过初始容量自动扩容且内容不错位
 *      - 环形回绕后依然正确
 *      - 非法帧被 push_frame 拒绝
 * ==========================================================================*/
#include "queue_map.h"

#include <cstdio>
#include <cstring>
#include <memory_resource>
#include <random>
#include <string>
#include <vector>

namespace {

int g_total = 0;
int g_failed = 0;

void check(bool ok, const std::string& name, const std::string& detail = "") {
    ++g_total;
    if (!ok) ++g_failed;
    std::printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.c_str());
    std::fflush(stdout);
}

} /* namespace */

int main() {
    ::SetConsoleOutputCP(CP_UTF8);
    std::printf("==========================================================\n");
    std::printf(" mdpsr 单元测试 (queue)\n");
    std::printf("==========================================================\n");

    std::pmr::memory_resource* res = std::pmr::new_delete_resource();

    /* ---- 1. 基本 FIFO + 二进制安全 ---- */
    {
        mdpsr::Queue q(res);
        const uint64_t h1 = 0x1111111111111111ull;
        const uint64_t h2 = 0x2222222222222222ull;
        const char* b1 = "hello";
        const unsigned char b2[] = { 0x00, 0xFF, 0x7F, 0x80, 0x00, 0x01 };

        q.push(h1, b1, std::strlen(b1));
        q.push(h2, b2, sizeof(b2));

        check(q.readable() == (12 + 5) + (12 + 6), "readable() 累计了帧头",
              std::to_string(q.readable()) + " 字节");

        uint64_t h = 0; const uint8_t* body = nullptr; size_t len = 0;
        bool ok = q.pop(&h, &body, &len);
        check(ok && h == h1 && len == 5 && std::memcmp(body, b1, 5) == 0, "第 1 条消息按序取出");

        ok = q.pop(&h, &body, &len);
        check(ok && h == h2 && len == sizeof(b2) && std::memcmp(body, b2, sizeof(b2)) == 0,
              "第 2 条消息二进制内容无损 (含 0x00/0xFF)");

        check(!q.pop(&h, &body, &len), "队列空时 pop 返回 false");
    }

    /* ---- 2. 超大载荷触发扩容 ---- */
    {
        mdpsr::Queue q(res);
        std::vector<uint8_t> big(200 * 1024);
        std::mt19937 rng(12345);
        for (auto& c : big) c = static_cast<uint8_t>(rng() & 0xFF);

        q.push(0xAABBCCDDull, big.data(), big.size());
        uint64_t h = 0; const uint8_t* body = nullptr; size_t len = 0;
        const bool ok = q.pop(&h, &body, &len);
        check(ok && h == 0xAABBCCDDull && len == big.size() &&
                  std::memcmp(body, big.data(), big.size()) == 0,
              "200KB 载荷扩容后内容一致", std::to_string(len) + " 字节");
    }

    /* ---- 3. 环形回绕 + 大量交错 ---- */
    {
        mdpsr::Queue q(res);
        std::mt19937 rng(999);
        std::vector<std::vector<uint8_t>> expect;
        std::vector<uint64_t> expect_h;
        int popped = 0;
        bool content_ok = true;
        bool order_ok = true;

        for (int i = 0; i < 500; ++i) {
            const size_t n = rng() % 300;
            std::vector<uint8_t> b(n);
            for (auto& c : b) c = static_cast<uint8_t>(rng() & 0xFF);
            const uint64_t h = 0x1000 + i;
            q.push(h, b.empty() ? nullptr : b.data(), b.size());
            expect.push_back(b);
            expect_h.push_back(h);

            /* 攒够 3 条就取走 2 条, 制造回绕 */
            if (expect.size() >= 3) {
                for (int k = 0; k < 2; ++k) {
                    uint64_t rh = 0; const uint8_t* rbody = nullptr; size_t rlen = 0;
                    if (!q.pop(&rh, &rbody, &rlen)) { order_ok = false; break; }
                    if (rh != expect_h.front()) order_ok = false;
                    if (rlen != expect.front().size() ||
                        (rlen && std::memcmp(rbody, expect.front().data(), rlen) != 0)) {
                        content_ok = false;
                    }
                    expect.erase(expect.begin());
                    expect_h.erase(expect_h.begin());
                    ++popped;
                }
            }
        }
        check(order_ok, "500 次交错收发 handle 顺序正确", "已取 " + std::to_string(popped) + " 条");
        check(content_ok, "回绕与扩容过程中内容不错位");
        check(q.pushed() == 500 && q.popped() == (uint64_t)popped, "收发计数一致",
              "pushed=" + std::to_string(q.pushed()) + " popped=" + std::to_string(q.popped()));
    }

    /* ---- 4. push_frame 校验整帧 ---- */
    {
        mdpsr::Queue q(res);
        /* 合法帧: _len=12+3, handle=0x42, body="abc" */
        uint8_t frame[15];
        const int32_t len = 15;
        const uint64_t handle = 0x42;
        std::memcpy(frame, &len, 4);
        std::memcpy(frame + 4, &handle, 8);
        std::memcpy(frame + 12, "abc", 3);
        check(q.push_frame(frame, sizeof(frame)), "push_frame 接受合法帧");

        uint64_t h = 0; const uint8_t* body = nullptr; size_t blen = 0;
        q.pop(&h, &body, &blen);
        check(h == 0x42 && blen == 3 && std::memcmp(body, "abc", 3) == 0, "push_frame 入队的帧可正确取出");

        check(!q.push_frame(frame, 10), "push_frame 拒绝长度不符的帧");
        uint8_t bad[15];
        std::memcpy(bad, frame, sizeof(frame));
        const int32_t tiny = 4;
        std::memcpy(bad, &tiny, 4);
        check(!q.push_frame(bad, sizeof(bad)), "push_frame 拒绝 _len 小于帧头的帧");
        check(q.readable() == 0, "被拒绝的帧没有污染队列");
    }

    /* ---- 5. 空 body ---- */
    {
        mdpsr::Queue q(res);
        q.push(0x7ull, nullptr, 0);
        uint64_t h = 0; const uint8_t* body = nullptr; size_t blen = 1;
        const bool ok = q.pop(&h, &body, &blen);
        check(ok && h == 0x7ull && blen == 0, "空 body 消息可以收发");
    }

    std::printf("\n----------------------------------------------------------\n");
    std::printf(" %d 项校验, 失败 %d 项\n", g_total, g_failed);
    std::printf("----------------------------------------------------------\n");
    return g_failed == 0 ? 0 : 1;
}
