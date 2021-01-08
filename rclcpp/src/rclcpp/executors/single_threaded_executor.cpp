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

#include "rclcpp/executors/single_threaded_executor.hpp"
#include "rclcpp/any_executable.hpp"
#include "rclcpp/scope_exit.hpp"

using rclcpp::executors::SingleThreadedExecutor;

SingleThreadedExecutor::SingleThreadedExecutor(const rclcpp::ExecutorOptions & options)
: rclcpp::Executor(options) {}

SingleThreadedExecutor::~SingleThreadedExecutor() {}

void
SingleThreadedExecutor::spin()
{
  if (spinning.exchange(true)) {
    throw std::runtime_error("spin() called while already spinning");
  }
  RCLCPP_SCOPE_EXIT(this->spinning.store(false); );
  while (rclcpp::ok(this->context_) && spinning.load()) {
    rclcpp::AnyExecutable any_executable;
    if (get_next_executable(any_executable)) {
      execute_any_executable(any_executable);
    }
  }
}


#define DEBUG


#ifdef DEBUG
#include <thread>
#include <pthread.h>
#include <sched.h>
#include <cstring>


#endif

void
SingleThreadedExecutor::spin_some(std::chrono::nanoseconds max_duration)
{
  // Fetch and store current timestamp
  auto start = std::chrono::steady_clock::now();

#ifdef DEBUG
  cpu_set_t cpu_set;
  std::cout << std::string("\033[1;33m") + "SingleThreadedExecutor: Pinned to cores {0,1}" + 
    std::string("\033[0m\n");

  CPU_ZERO(&cpu_set);
  CPU_SET(0, &cpu_set);
  CPU_SET(1, &cpu_set);
  if (0 != pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpu_set)) {
    throw std::runtime_error(std::string("pthread_setaffinity_np: ") + 
      std::string(std::strerror(errno)));
  }

#endif

  // Create callback
  auto callback_should_still_spin = [max_duration, start]()
  {
    // Case: Spin forever
    if (std::chrono::nanoseconds(0) == max_duration) {
      return true;
    }

    // Case: Spin for duration
    return ((std::chrono::steady_clock::now() - start) < max_duration);
  };

  // Check if already spinning
  if (true == spinning.exchange(true)) {
    throw std::runtime_error("spin_some() called while already spinning!");
  }

  RCLCPP_SCOPE_EXIT(this->spinning.store(false); );

  while (rclcpp::ok(context_) && (true == spinning.load()) &&
    true == callback_should_still_spin())
  {
    AnyExecutable any_exec;
    
    // Non-preemptable call
    if (get_next_executable(any_exec)) {
      execute_any_executable(any_exec);
    }
  }
}
