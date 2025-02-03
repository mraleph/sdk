#include <algorithm>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <ranges>
#include <string>
#include <thread>
#include <vector>

#include "vm/os_thread.h"
#include "vm/thread_pool.h"
#include "platform/lockers.h"

namespace {

std::vector<std::string> read_lines_from(
    const std::filesystem::path& filepath) {
  std::vector<std::string> lines;

  std::ifstream file(filepath);

  std::string line;
  while (std::getline(file, line)) {
    lines.push_back(std::move(line));
  }

  return lines;
}

class Task : public dart::ThreadPool::Task {
 public:
  Task(std::function<void ()>&& f) : f(std::move(f)) {}

  virtual void Run() {
    f();
  }

 private:
  std::function<void ()> f;
};

}  // namespace

int main(int argc, char** argv) {
  dart::OSThread::Init();

  const auto all_files = read_lines_from("input.list");



  dart::ThreadPool thread_pool(32);

  dart::Mutex m;
  dart::ConditionVariable cv;

  for (size_t start_idx = 0, index = 0; start_idx < all_files.size(); start_idx += 1000, index++) {
    auto files = std::span(all_files.begin() + start_idx, std::min(1000UL, all_files.size() - start_idx));

    auto start = std::chrono::system_clock::now();

    std::atomic<size_t> total_bytes_read = 0;
    std::atomic<size_t> pending_files = files.size();
    for (const auto& file : files) {
      thread_pool.Run<Task>([&file, &total_bytes_read, &cv, &pending_files]() {
        //std::print(stderr, "reading file {}\n", file);
        std::ifstream is(file);
        is.seekg(0, std::ios_base::end);
        const auto size = is.tellg();
        if (size == -1) {
            UNREACHABLE();
        }
        is.seekg(0, std::ios_base::beg);

        std::vector<char> v(size);
        is.read(&v[0], size);
        is.close();


        total_bytes_read += v.size();
        if (--pending_files == 0) {
            cv.NotifyAll();
        }
      });
    }

    {
        dart::platform::MutexLocker ml(&m);
        while (pending_files.load() != 0) {
            cv.Wait(&m);
        }
    }

    auto end = std::chrono::system_clock::now();

    auto elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    fprintf(stderr, "[%zu],%lld,%zu,%.2f\n", index, elapsed_us.count(), total_bytes_read.load(), ((double)elapsed_us.count()) * 1024 / (double)total_bytes_read.load());
  }
}
