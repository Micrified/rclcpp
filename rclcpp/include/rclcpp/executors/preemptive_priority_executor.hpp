/*
 *******************************************************************************
 *              (C) Copyright 2020 Delft University of Technology              *
 * Created: 30/08/2020                                                         *
 *                                                                             *
 * Programmer(s):                                                              *
 * - Charles Randolph                                                          *
 *                                                                             *
 * Description:                                                                *
 *  Time predictable multi-threaded executor prototype                         *
 *                                                                             *
 *******************************************************************************
*/

#ifndef PREEMPTIVE_PRIORITY_EXECUTOR
#define PREEMPTIVE_PRIORITY_EXECUTOR

// Standard headers
#include <iostream>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <pthread.h>
#include <future>
#include <cstring>
#include <map>
#include <queue>
#include <functional>

// RCLCPP headers
#include "rclcpp/executor.hpp"
#include "rclcpp/macros.hpp"
#include "rclcpp/visibility_control.hpp"

/*
 *******************************************************************************
 *                             Class Declarations                              *
 *******************************************************************************
*/

namespace rclcpp
{
namespace executors
{

// Type names for the thread main function signature
using thread_func_t = void(int, int);
using thread_func_p = void(*)(int, int);

class TaskInstance {
public:
	TaskInstance (int task_priority, thread_func_p thread_func):
		d_task_priority(task_priority),
		d_thread_func(thread_func),
		d_task_ptr(nullptr),
		d_is_running(false)
	{
		// Create task object
		d_task_ptr = new std::packaged_task<thread_func_t>(thread_func);

		// Obtain future (may only be done once)
		d_task_future_ptr = std::make_shared<std::future<void>>(d_task_ptr->get_future());
	}

	TaskInstance (const TaskInstance &other)
	{
		d_task_priority = other.d_task_priority;
		d_thread_func = other.d_thread_func;
		d_task_ptr = other.d_task_ptr;
		d_is_running = other.d_is_running;
		d_task_future_ptr = other.d_task_future_ptr;
	}

	~TaskInstance ()
	{
		delete d_task_ptr;
	}

	std::packaged_task<thread_func_t> *get_task_ptr ()
	{
		return d_task_ptr;
	}

	std::shared_ptr<std::future<void>> get_future_ptr ()
	{
		return d_task_future_ptr;
	}

	void set_is_running (bool is_running)
	{
		d_is_running = is_running;
	}

	bool is_running ()
	{
		return d_is_running;
	}

	int task_priority ()
	{
		return d_task_priority;
	}

private:
	int d_task_priority;                                  // Priority given to task
	thread_func_p d_thread_func;                          // Main function of the thread
	std::packaged_task<thread_func_t> *d_task_ptr;        // Pointer to task instance
	bool d_is_running;                                    // Whether or not thread is running
	std::shared_ptr<std::future<void>> d_task_future_ptr; // Pointer to value to return when done
};

class CallbackPriorityMap {
public:
	CallbackPriorityMap ():
		d_map_p(nullptr)
	{
		d_map_p = new std::map<std::string, int>();
	}

	~CallbackPriorityMap ()
	{
		d_map_p->clear();
		delete d_map_p;
	}

	void set_priority_for_node_on_subscription (const char *node_name, 
		const char *topic_name, int priority)
	{
		std::string key(std::string(node_name) + std::string(topic_name));
		(*d_map_p)[key] = priority;
	}

	bool get_priority_for_node_on_subscription (const char *node_name,
		const char *topic_name, int *priority_ptr)
	{
		if (nullptr == priority_ptr) {
			return false;
		}
		std::string key(std::string(node_name) + std::string(topic_name));
		std::map<std::string, int>::iterator it;
		it = d_map_p->find(key);
		if (it == d_map_p->end()) {
			return false;
		} else {
			*priority_ptr = (*d_map_p)[key];
		}
		return true;
	}

private:
	std::map<std::string, int> *d_map_p;
};

class PreemptivePriorityExecutor : public rclcpp::Executor
{
public:
	RCLCPP_SMART_PTR_DEFINITIONS(PreemptivePriorityExecutor)

	/*\
	 * Creates an executor that schedules callbacks according to a
	 * priority mapping scheme. Priorities are stored in a map which
	 * maps a node to a map of topics and priorities. Each node has
	 * a priority assigned to a topic (upon reception) and hence applies
	 * it to the callback associated with the respective topic.
	 *
	 * Timers have a built in priority assigned during creation which is
	 * used for the evaluation. Services and responses are not supported
	 * and are executed with a minimum priority by default!
	 *
	 * \param priority_callback_map_p Pointer to the priority mapping table
	 * \param options Common options for all executors
	 * \param thread_count A suggestion for the maximum number of threads to
	 *                     create.
	 * \param timeout_ns Timeout in nanoseconds, which represents how long 
	 *                   the executor waits for new work
	 *                   value (-1) means indefinitely.
	\*/
	RCLCPP_PUBLIC 
	PreemptivePriorityExecutor (
		PriorityCallbackMap *priority_callback_map_p = nullptr,
		const rclcpp::ExecutorOptions &options = rclcpp::ExecutorOptions(),
		size_t thread_count = 0,
		std::chrono::nanoseconds timeout_ns = std::chrono::nanoseconds(-1))
	:
		d_thread_count(thread_count),
		d_timeout_ns(timeout_ns),
		d_priority_callback_map_p(priority_callback_map_p);
	{

	}

	/*\
	 * Destructor for the PreemptivePriorityExecutor
	\*/
	RCLCPP_PUBLIC
	~PreemptivePriorityExecutor()
	{
		delete d_priority_callback_map_p;
	}


	/*\
	 *  Runs the executor
	\*/
	void spin() override;

protected:

private:
	std::mutex d_io_mutex;
	std::mutex d_wait_mutex;
	size_t d_thread_count;
	std::chrono::nanoseconds d_timeout_ns;

	// Priority callback map
	PriorityCallbackMap *d_priority_callback_map_p;

	// Priority map for subscription callbacks
	// (node name x subscription topic x priority)

	// Timers have their own priority (assigned when created)

	// TODO: Remove entries from the map when a node is removed
	// TODO: Add entries to the map when a node is added (or topic)
	// TODO: ... 
}
}
}
#endif