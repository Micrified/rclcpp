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
#include <set>
#include <functional>
#include <sched.h>

// RCLCPP headers
#include "rclcpp/executor.hpp"
#include "rclcpp/macros.hpp"
#include "rclcpp/visibility_control.hpp"


/*
 *******************************************************************************
 *                         Complete Class Definitions                          *
 *******************************************************************************
*/


namespace rclcpp
{
namespace executors
{

class PreemptivePriorityExecutor;
class TaskInstance;

// Type names for the thread main function signature
using thread_func_t = void(rclcpp::executors::PreemptivePriorityExecutor *, rclcpp::executors::TaskInstance *);
using thread_func_p = void(*)(rclcpp::executors::PreemptivePriorityExecutor *, rclcpp::executors::TaskInstance *);

class TaskInstance {
public:
	TaskInstance (int task_priority, AnyExecutable any_executable):
		d_task_priority(task_priority),
		d_any_executable(any_executable),
		d_is_running(false),
		d_is_finished(false)
	{

	}

	TaskInstance (const TaskInstance &other)
	{
		d_task_priority = other.d_task_priority;
		d_is_running = other.d_is_running;
		d_is_finished.store(other.d_is_finished.load());
	}

	~TaskInstance ()
	{
	}

	void set_is_running (bool is_running)
	{
		d_is_running = is_running;
	}

	bool is_running ()
	{
		return d_is_running;
	}

	void set_is_finished (bool is_finished) {
		d_is_finished.store(is_finished);
	}

	bool is_finished ()
	{
		return d_is_finished.load();
	}

	int task_priority ()
	{
		return d_task_priority;
	}

	AnyExecutable any_executable ()
	{
		return d_any_executable;
	}

private:
	int d_task_priority;                                  // Priority given to task
	AnyExecutable d_any_executable;                       // Copy of the executable object
	bool d_is_running;                                    // Whether or not thread is running
	std::atomic<bool> d_is_finished;                      // Whether or not the thread is finished
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

/*
 *******************************************************************************
 *                              Type Definitions                               *
 *******************************************************************************
*/


// I didn't want to do this but the type is just too long
typedef std::priority_queue<TaskInstance *, std::vector<TaskInstance *>, std::function<bool(TaskInstance *, TaskInstance *)>> TaskPriorityQueue;

/*
 *******************************************************************************
 *                             Class Declarations                              *
 *******************************************************************************
*/


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
		const rclcpp::ExecutorOptions &options = rclcpp::ExecutorOptions(),
		size_t thread_count = 0,
		std::chrono::nanoseconds timeout_ns = std::chrono::nanoseconds(-1),
		CallbackPriorityMap *callback_priority_map_p = nullptr);

	/*\
	 * Destructor for the PreemptivePriorityExecutor
	\*/
	RCLCPP_PUBLIC
	~PreemptivePriorityExecutor();

	/*\
	 *  Runs the executor
	\*/
	void spin() override;


protected:

	void show_any_executable (AnyExecutable *any_executable);

	/*\
	 * Allocates a new max priority queue, and transfers items from queue into it if they haven't
	 * expired. 
	 * \param queue Pointer to the TaskPriorityQueue to filter
	\*/
	std::priority_queue<TaskInstance *, std::vector<TaskInstance *>, std::function<bool(TaskInstance *, TaskInstance *)>> *filter_completed_tasks (std::priority_queue<TaskInstance *, std::vector<TaskInstance *>, std::function<bool(TaskInstance *, TaskInstance *)>> *queue);

	/*\
	 * Returns the priority with which to execute the given executable. Uses the internal 
	 * lookup table (if set) to do this
	 * Note: This doesn't work for services and clients
	 * \param any_executable Reference to the executable for which to lookup the priority
	\*/
	int get_executable_priority (AnyExecutable &any_executable);

	/*\
	 * Runs a callback. This should be run within a worker thread at a lower priority
	 * than the main thread
	 * \param priority The priority assigned to this callback
	 * \param any_executable The executable to run (copied)
	\*/
	RCLCPP_PUBLIC
	static void run (rclcpp::executors::PreemptivePriorityExecutor *executor, TaskInstance *task_p);

	/*\
	 * Returns a pointer to the wait mutex
	\*/
	std::mutex *wait_mutex ();

	/*\
	 * Returns pointer to the scheduled timers set
	\*/
	std::set<TimerBase::SharedPtr> *scheduled_timers ();

	/*\
	 * Returns the context
	\*/
	std::shared_ptr<rclcpp::Context> get_context ();

	/*\
	 * Returns true if spinning or not
	\*/
	bool get_spinning ();

private:
	std::mutex d_io_mutex;
	std::mutex d_wait_mutex;
	size_t d_thread_count;
	std::chrono::nanoseconds d_timeout_ns;

	// Priority callback map
	CallbackPriorityMap *d_callback_priority_map_p;

	// Scheduled timers 
	std::set<TimerBase::SharedPtr> d_scheduled_timers;

	// Priority map for subscription callbacks
	// (node name x subscription topic x priority)

	// Timers have their own priority (assigned when created)

	// TODO: Remove entries from the map when a node is removed
	// TODO: Add entries to the map when a node is added (or topic)
	// TODO: ... 
};


}
}
#endif