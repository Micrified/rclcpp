
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


/*
 *******************************************************************************
 *                            Forward Declarations                             *
 *******************************************************************************
*/


static bool task_compare (TaskInstance *a, TaskInstance *b);


// I didn't want to do this but the type is just too long
typedef std::priority_queue<TaskInstance *, std::vector<TaskInstance *>, std::function<bool(TaskInstance *, TaskInstance *)>> TaskPriorityQueue;

/*
 *******************************************************************************
 *                           Constructor/Destructor                            *
 *******************************************************************************
*/

PreemptivePriorityExecutor::PreemptivePriorityExecutor (
	const rclcpp::ExecutorOptions &options,
	size_t thread_count,
	std::chrono::nanoseconds timeout_ns,
	CallbackPriorityMap *callback_priority_map_p)
:
	rclcpp::Executor(options),
	d_thread_count(thread_count),
	d_timeout_ns(timeout_ns),
	d_callback_priority_map_p(callback_priority_map_p)
{

}


PreemptivePriorityExecutor::~PreemptivePriorityExecutor ()
{
	delete d_callback_priority_map_p;
}


/*
 *******************************************************************************
 *                               Public Methods                                *
 *******************************************************************************
*/


void PreemptivePriorityExecutor::spin ()
{
	sched_param sch, sch_old;
	int policy, policy_old;
	AnyExecutable any_executable;

	// TaskInstance priority queue
	TaskPriorityQueue *task_queue_p = new TaskPriorityQueue(task_compare);

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
		throw std::runtime_error(std::string("pthread_setschedparam: ") +
		 std::string(std::strerror(errno)));
	}

	// While should spin ...
	while (rclcpp::ok(this->context_) && spinning.load()) {

		// Wait for work ... 
		wait_for_work(d_timeout_ns);

		// Clear the task queue of completed tasks
		task_queue_p = this->filter_completed_tasks(task_queue_p);

		// Exit loop if spinning disabled
		if (false == spinning.load()) {
			break;
		}

		// If a new executable is waiting: Place it into the priority queue
		if (true == this->get_next_ready_executable(any_executable)) {

			// Compute a priority for this callback
			int new_task_priority = this->get_executable_priority(any_executable);

			// Create a new task instance
			TaskInstance *new_task_ptr = new TaskInstance(new_task_priority,
				PreemptivePriorityExecutor::run, std::move(any_executable));

			// Insert the callback into the priority heap
			task_queue_p->push(new_task_ptr);
		}

		// Dequeue the highest priority callback to run
		TaskInstance *highest_priority_callback = task_queue_p->top();

		// If this task is busy running - do nothing
		if (!(std::future_status::ready == 
			highest_priority_callback->get_future_ptr()->wait_for(std::chrono::seconds(0))))
		{
			continue;
		}

		// Fetch the executable
		any_executable = highest_priority_callback->any_executable();

		// Otherwise, launch this task
		std::thread new_task_thread(std::move(*(highest_priority_callback->get_task_ptr())),
			this, 
			highest_priority_callback->task_priority(),
			any_executable);

		// Assign the priority level to the thread
		int base_thread_priority_level = 50;
		pthread_getschedparam(new_task_thread.native_handle(), &policy, &sch);
		sch.sched_priority = base_thread_priority_level + highest_priority_callback->task_priority();
		if (pthread_setschedparam(new_task_thread.native_handle(), SCHED_FIFO, &sch)) {
			throw std::runtime_error(std::string("pthread_setschedparam: ") +
			 std::string(std::strerror(errno)));
		}

		// Detach the thread
		new_task_thread.detach();
	}

	// Busy wait for any outstanding work to finish (in case of spinning set to false)
	while (0 < task_queue_p->size()) {
		task_queue_p = this->filter_completed_tasks(task_queue_p);
	}

	// Free allocated objects
	delete task_queue_p;

	// Restore old policy
	if (0 != pthread_setschedparam(pthread_self(), policy_old, &sch_old)) {
		throw std::runtime_error(std::string("thread_setschedparam: ") + 
			std::string(std::strerror(errno)));
	}
}

/*
 *******************************************************************************
 *                               Support Methods                               *
 *******************************************************************************
*/


static bool task_compare (TaskInstance *a, TaskInstance *b)
{
	return (a->task_priority() < b->task_priority());
}


TaskPriorityQueue *PreemptivePriorityExecutor::filter_completed_tasks (TaskPriorityQueue *queue)
{
	TaskPriorityQueue *filtered_queue = new TaskPriorityQueue(task_compare);

	// Pop elements
	while (false == queue->empty()) {
		TaskInstance *task_p = queue->top();
		queue->pop();

		// If the task has finished, then destroy it and move on
		if (std::future_status::ready == task_p->get_future_ptr()->wait_for(std::chrono::seconds(0))) {
			delete task_p;
			continue;
		}

		// Otherwise: Push it into the new priority queue
		filtered_queue->push(task_p);
	}

	// Destroy the old queue
	delete queue;

	return filtered_queue;
}


int PreemptivePriorityExecutor::get_executable_priority (AnyExecutable &any_executable)
{
	int priority = -1;

	// If it is a timer, then use the supplied value (set via the timer constructor)
	if (nullptr != any_executable.timer) {
		return any_executable.callback_priority;
	}

	// If no callback priority map was provided, simply return now
	if (nullptr == d_callback_priority_map_p) {
		return priority;
	}

	// If it is a subscription, lookup the priority in the map
	if (nullptr != any_executable.subscription) {
		const char *node_name = any_executable.node_base->get_name();
		const char *subscription_topic = any_executable.subscription->get_topic_name();
		if (false == d_callback_priority_map_p->get_priority_for_node_on_subscription(
			node_name, subscription_topic, &priority))
		{
			priority = -1;
		}
	}

	return priority;
}


void PreemptivePriorityExecutor::run (rclcpp::executors::PreemptivePriorityExecutor *executor, int priority, AnyExecutable any_executable)
{
	sched_param sch;
	int policy;
	int thread_id = pthread_self();

	// Get thread info
	pthread_getschedparam(thread_id, &policy, &sch);

	// Print: Start
	{
		std::cout << "[" << thread_id << "]<" << priority << "> " << "Starting ..." 
				  << std::endl;
	}

	// Work
	// TODO ...
	executor->execute_any_executable(any_executable);


	// Print: End
	{
		std::cout << "[" << thread_id << "]<" << priority << "> " << "Done " 
				  << std::endl;
	}
}