// Copyright 2015 Open Source Robotics Foundation, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "rclcpp/executors/multi_threaded_executor.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <vector>

#include "rclcpp/utilities.hpp"
#include "rclcpp/scope_exit.hpp"

#define DEBUG

#ifdef DEBUG
#include <thread>
#include <pthread.h>
#include <sched.h>
#include <cstring>
#endif

using rclcpp::executors::MultiThreadedExecutor;

MultiThreadedExecutor::MultiThreadedExecutor(
  const rclcpp::ExecutorOptions & options,
  size_t number_of_threads,
  bool yield_before_execute,
  std::chrono::nanoseconds next_exec_timeout)
: rclcpp::Executor(options),
  yield_before_execute_(yield_before_execute),
  next_exec_timeout_(next_exec_timeout)
{

#ifdef DEBUG
  cpu_set_t cpu_set;
  std::cout << std::string("\033[1;33m") + "MultiThreadedExecutor: Pinned to cores {0,1}" + 
    std::string("\033[0m\n");
  CPU_ZERO(&cpu_set);
  CPU_SET(0, &cpu_set);
  CPU_SET(1, &cpu_set);
  if (0 != pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpu_set)) {
    throw std::runtime_error(std::string("pthread_setaffinity_np: ") + 
      std::string(std::strerror(errno)));
  }

#endif
  number_of_threads_ = number_of_threads ? number_of_threads : std::thread::hardware_concurrency();
  if (number_of_threads_ == 0) {
    number_of_threads_ = 1;
  }
}

MultiThreadedExecutor::~MultiThreadedExecutor() {}

void
MultiThreadedExecutor::spin()
{
  spin_some(std::chrono::nanoseconds(0));
}

void
MultiThreadedExecutor::spin_some(std::chrono::nanoseconds max_duration)
{
  // Check that the executor isn't already spinning
  if (spinning.exchange(true)) {
    throw std::runtime_error("spin_some() called while already spinning");
  }
  RCLCPP_SCOPE_EXIT(this->spinning.store(false); );
  std::vector<std::thread> threads;

  // Create all threads
  size_t thread_id = 0;
  {
    std::lock_guard<std::mutex> wait_lock(wait_mutex_);
    for (; thread_id < (number_of_threads_ - 1); ++thread_id) {
      auto func = std::bind(&MultiThreadedExecutor::run_some, this, thread_id, max_duration);
      threads.emplace_back(func);
    }
  }

  run_some(thread_id, max_duration);
  for (auto & thread : threads) {
    thread.join();
  }
}

size_t
MultiThreadedExecutor::get_number_of_threads()
{
  return number_of_threads_;
}

void 
MultiThreadedExecutor::run_some(size_t, std::chrono::nanoseconds max_duration)
{

  // Fetch and store current timestamp
  auto start = std::chrono::steady_clock::now();

  // Callback to determine if the duration has expired
  auto callback_should_still_spin = [max_duration, start]()
  {
    // Case: Spin forever
    if (std::chrono::nanoseconds(0) == max_duration) {
      return true;
    }

    // Case: Spin for duration
    return ((std::chrono::steady_clock::now() - start) < max_duration);
  };

  while (rclcpp::ok(this->context_) && spinning.load() && 
    true == callback_should_still_spin())
  {
    rclcpp::AnyExecutable any_exec;
    {
      std::lock_guard<std::mutex> wait_lock(wait_mutex_);
      if (!rclcpp::ok(this->context_) || !spinning.load()) {
        return;
      }
      if (!get_next_executable(any_exec, next_exec_timeout_)) {
        continue;
      }
      if (any_exec.timer) {
        // Guard against multiple threads getting the same timer.
        if (scheduled_timers_.count(any_exec.timer) != 0) {
          // Make sure that any_exec's callback group is reset before
          // the lock is released.
          if (any_exec.callback_group) {
            any_exec.callback_group->can_be_taken_from().store(true);
          }
          continue;
        }
        scheduled_timers_.insert(any_exec.timer);
      }
    }
    if (yield_before_execute_) {
      std::this_thread::yield();
    }

    execute_any_executable(any_exec);

    if (any_exec.timer) {
      std::lock_guard<std::mutex> wait_lock(wait_mutex_);
      auto it = scheduled_timers_.find(any_exec.timer);
      if (it != scheduled_timers_.end()) {
        scheduled_timers_.erase(it);
      }
    }
    // Clear the callback_group to prevent the AnyExecutable destructor from
    // resetting the callback group `can_be_taken_from`
    any_exec.callback_group.reset();
  }
}
