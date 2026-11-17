#include "bg_tasks.h"
#include "activity.h"
#include "debug_log.h"
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>

namespace {
std::mutex g_mutex;
std::condition_variable g_cv;
std::deque<std::function<void()>> g_queue;
std::thread g_thread;
bool g_stop = false;

void run() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_cv.wait(lock, [] { return g_stop || !g_queue.empty(); });
            if (g_stop) return;
            job = std::move(g_queue.front());
            g_queue.pop_front();
        }
        try {
            job();
        } catch (const std::exception& e) {
            MBDB_LOG("background task failed: %s", e.what());
            mbdb_activity::add_event(mbdb_activity::lvl_error, (std::string("Background task failed: ") + e.what()).c_str());
        } catch (...) {
            mbdb_activity::add_event(mbdb_activity::lvl_error, "Background task failed");
        }
    }
}
} // namespace

void mbdb_run_async(std::function<void()> task) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_stop) return;
    if (!g_thread.joinable()) g_thread = std::thread(run);
    g_queue.push_back(std::move(task));
    g_cv.notify_one();
}

void mbdb_bg_shutdown() {
    std::thread t;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_stop = true;
        g_queue.clear();
        t = std::move(g_thread);
    }
    g_cv.notify_all();
    if (t.joinable()) t.join();
}
