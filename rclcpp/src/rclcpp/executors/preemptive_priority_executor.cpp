
// Main header
#include "rclcpp/executors/preemptive_priority_executor.hpp"

// Standard headers
#include <chrono>
#include <functional>
#include <vector>
#include <memory>

// RCLCPP headers
#include "rclcpp/utilities.hpp"
#include "rclcpp/scope_exit.hpp"


using rclcpp::executors::PreemptivePriorityExecutor;
using rclcpp::executors::TaskInstance;
using rclcpp::executors::CallbackPriorityMap;


PreemptivePriorityExecutor::PreemptivePriorityExecutor (
	PriorityCallbackMap *priority_callback_map_p,
	const rclcpp::ExecutorOptions &options,
	size_t thread_count,
	std::chrono::nanoseconds timeout_ns)
:
	d_thread_count(thread_count),
	d_timeout_ns(timeout_ns),
	d_priority_callback_map_p(priority_callback_map_p)
{

}


PreemptivePriorityExecutor::~PreemptivePriorityExecutor ()
{
	delete d_priority_callback_map_p;
}


void PreemptivePriorityExecutor::spin ()
{
	sched_param sch, sch_old;
	int policy, policy_old;
	AnyExecutable any_executable;

	// Lambda comparator for queue elements
	auto task_cmp = [](TaskInstance *left, TaskInstance *right)
	{
		return (left->d_task_priority < right->d_task_priority);
	};

	// TaskInstance priority queue
	std::priority_queue<TaskInstance *, std::vector<TaskInstance *>, 
		decltype(task_cmp)> *task_queue_p = new std::priority_queue<TaskInstance *, std::vector<TaskInstance *>, 
		decltype(task_cmp)>(task_cmp);

	// Check: Not already spinning
	if (true == spinning.exchange(true)) {
		throw std::runtime_error("May not call spin() while already spinning!");
	}

	// Defer: On exit, we want to stop spinning
	RCLCPP_SCOPE_EXIT(this->spinning.store(false););

	// Get the current policy and save it
	pthread_getschedparam(pthread_self(), &policy, &sch);
	sch_old = sch;
	policy_old = policy;

	// Apply new policy (scheduler runs at maximum priority)
	sch.sched_priority = 99;
	if (0 != pthread_setschedparam(pthread_self(), SCHED_FIFO, &sch)) {
		throw std::runtime_error("pthread_setschedparam: " + std::strerror(errno));
	}

	// While should spin ...
	while (rclcpp::ok(this->context_) && spinning.load()) {

		// Wait for work ... 
		wait_for_work(d_timeout_ns);

		// Check if should still spin
		if (false == spinning.load()) {
			break;
		}

		// TODO: Filter the heap
		// 1. Go through and remove all items, place into a new heap
		// 2. Remove heap elements that have finished executing

		// If no ready executable (though there ought to be - continue)
		if (false == this->get_next_ready_executable(any_executable)) {
			continue;
		}

		// Get priority:
		// 1. If it's a timer, it's built in already
		// 2. If it's a subscription, you need to use the table
		// 3. Otherwise it just gets a default priority of -1
		int new_task_priority = this->get_executable_priority(any_executable);

		// Build new task instance
		// TODO: Give correct thread pointer parameter!
		TaskInstance *new_task_p = new TaskInstance(new_task_priority, nullptr);

		// If there are pending tasks
		if (0 < task_queue_p->size()) {
			auto highest_priority_task = task_queue_p->top();

			// If current task priority is less than that of most important
			if (new_task_priority < highest_priority_task->task_priority()) {
				// Enqueue the task and wait for more work (or for work to end)
				task_queue_p->push(new_task);
				continue;
			}
		}

		// Otherwise:
		// 1. There are no other tasks running or pending
		// 2. All other tasks running or pending are of lower priority than new task

		// Create the new task thread
		std::thread new_task_thread(std::move(*(new_task_p->get_task_ptr())), new_task_priority, 
			new_task_priority);

		// Set the thread priority level
		int min_thread_priority = 50;
		pthread_getschedparam(new_task_thread.native_handle(), &policy, &sch);
		sch.sched_priority = min_thread_priority + new_task_priority;
		if (pthread_setschedparam(thread_1.native_handle(), SCHED_FIFO, &sch)) {
			std::cerr << "pthread_setschedparam: " << std::strerror(errno) 
			          << std::endl;
			return -1;
		}

		// Detach the thread (object now becomes invalid)
		new_task_thread.detach();
	}

	// Free allocated objects
	delete task_queue_p;

	// Restore old policy
	if (0 != pthread_setschedparam(pthread_self(), policy_old, &sch_old)) {
		throw std::runtime_error("thread_setschedparam: " + std::strerror(errno));
	}
}

int PreemptivePriorityExecutors::get_executable_priority (AnyExecutable &any_executable)
{
	int priority = -1;

	// If it is a timer, then use the supplied value (set via the timer constructor)
	if (nullptr != any_executable.timer) {
		return static_cast<off_t>(any_executable.callback_priority);
	}

	// If it is a subscription, lookup the priority in the map
	if (nullptr != any_executable.subscription) {
		const char *node_name = any_executable.node_base->get_name();
		const char *subscription_topic = any_executable.subscription->get_topic_name();
		if (false == d_priority_callback_map_p->get_priority_for_node_on_subscription(
			&priority, node_name, subscription_topic))
		{
			priority = -1;
		}
	}

	return priority;
}

void PreemptivePriorityExecutor::run (size_t thread_id, int priority)
{
	sched_param sch;
	int policy;

	// Get thread info
	pthread_getschedparam(pthread_self(), &policy, &sch);

	// Print: Start
	{
		std::lock<std::mutex> io_lock(d_io_mutex)
		std::cout << "[" << thread_id << "]<" << priority << "> " << "Starting ..." 
				  << std::endl;
	}

	// Work
	// TODO ...


	// Print: End
	{
		std::lock<std::mutex> io_lock(d_io_mutex);
		std::cout << "[" << thread_id << "]<" << priority << "> " << "Done " 
				  << std::endl;
	}
}