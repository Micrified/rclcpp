
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
	cpu_set_t cpu_set;

	// TaskInstance priority queue
	TaskPriorityQueue *task_queue_p = new TaskPriorityQueue(task_compare);

	// Check: Not already spinning
	if (true == spinning.exchange(true)) {
		throw std::runtime_error("May not call spin() while already spinning!");
	}

	// Defer: On exit, we want to stop spinning
	RCLCPP_SCOPE_EXIT(this->spinning.store(false););

	// Allocate a single CPU
	CPU_ZERO(&cpu_set);
	CPU_SET(1, &cpu_set);

	// Set CPU affinity
	if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpu_set) != 0) {
		throw std::runtime_error(std::string("pthread_setaffinity_np: ") +
			std::string(std::strerror(errno)));
	}
	// if (sched_setaffinity(0, sizeof(cpu_set_t), &cpu_set) != 0) {
	// 	throw std::runtime_error(std::string("sched_setaffinity: ") +
	// 		std::string(std::strerror(errno)));
	// }

	// Get the current policy and save it
	pthread_getschedparam(pthread_self(), &policy, &sch);
	sch_old = sch;
	policy_old = policy;

	// Apply new policy (scheduler runs at maximum priority)
	sch.sched_priority = 60;
	if (0 != pthread_setschedparam(pthread_self(), SCHED_FIFO, &sch)) {
		throw std::runtime_error(std::string("pthread_setschedparam: ") +
		 std::string(std::strerror(errno)));
	}

	// While should spin ...
	// TODO: Consider whether generating a pool of work threads that
	//       simply take work according to their priority level or 
	//       sleep is a good idea. One thread can simply constantly
	//       wait and set condition variables to true in order to
	//       wake certain threads. Would need to know how many
	//       priorities are required beforehand
	off_t count = 0;
	while (rclcpp::ok(this->context_) && spinning.load()) {
		AnyExecutable any_executable;
		count++;
		//std::cout << "\rExecutor " << count;
		// Wait for work ...
		//std::cout << "Waiting for work" << std::endl; 
		wait_for_work(std::chrono::nanoseconds(-1));
		//std::cout << "Executor: Work has arrived!" << std::endl;

		// Clear the task queue of completed tasks
		task_queue_p = this->filter_completed_tasks(task_queue_p);

		// Exit loop if spinning disabled
		if (false == spinning.load()) {
			std::cout << "Executor: spinning false -> break!" << std::endl;
			break;
		}

		//std::cout << "Executor: About to check next ready executable ..." << std::endl;

		// If a new executable is waiting: Place it into the priority queue
		if (true == this->get_next_ready_executable(any_executable)) {

			//std::cout << "Executor: Executable is ready!" << std::endl;

			// If the executable is a timer, then do not allow reentrant behavior 
			if (nullptr != any_executable.timer) {
				std::lock_guard<std::mutex> temp_lock(d_wait_mutex);
				// If it is already accounted for then do not consider it
				if (0 < d_scheduled_timers.count(any_executable.timer)) {
					//std::cout << "Executor: Is a timer and already taken, will wait ..." << std::endl;
					// If it has a non-null callback group then reset it
					if (nullptr != any_executable.callback_group) {
						any_executable.callback_group->can_be_taken_from().store(true);
					}
					continue;
				}

				// Insert the timer into the timer set
				d_scheduled_timers.insert(any_executable.timer);
			}

			// Note: You have to remove the timer from the callback group after,
			//        the launched thread will do this

			// Compute a priority for this callback
			int new_task_priority = this->get_executable_priority(any_executable);

			//std::cout << "Executor: Ready executable with priority " << new_task_priority << std::endl;
			//std::cout << "Executor: Pushing new task to queue: ";
			//show_any_executable(&any_executable);
			//std::cout << std::endl;

			// Create a new task instance
			TaskInstance *new_task_ptr = new TaskInstance(new_task_priority, std::move(any_executable));

			// Insert the callback into the priority heap
			task_queue_p->push(new_task_ptr);
		}

		if (task_queue_p->size() == 0) {
			//std::cout << "Executor: Nothing to do -> going to wait again ..." << std::endl;
			continue;
		}

		//std::cout << "Executor: About to dequeue next ready executable ..." << std::endl;

		// Dequeue the highest priority callback to run
		TaskInstance *highest_priority_callback = task_queue_p->top();

		//std::cout << "Executor: About to check future of highest priority callback!" << std::endl;

		// If this task is busy running - do nothing
		if (highest_priority_callback->is_running() == true)
		{
			AnyExecutable any = highest_priority_callback->any_executable();
			//std::cout << "Executor: Highest prio callback: ";
			//show_any_executable(&any);
			//std::cout << " is busy still!" << std::endl;
			continue;
		} else {
			//std::cout << "Executor: New unlaunched thread created!" << std::endl;
			highest_priority_callback->set_is_running(true);
		}

		// Otherwise, launch this task
		std::thread new_task_thread(std::move(PreemptivePriorityExecutor::run), this, highest_priority_callback);

		// Assign the priority level to the thread
		int base_thread_priority_level = 50;
		pthread_getschedparam(new_task_thread.native_handle(), &policy, &sch);
		sch.sched_priority = base_thread_priority_level + highest_priority_callback->task_priority();
		if (pthread_setschedparam(new_task_thread.native_handle(), SCHED_FIFO, &sch)) {
			throw std::runtime_error(std::string("pthread_setschedparam: ") +
			 std::string(std::strerror(errno)));
		}

		// // Assign the thread to a dedicated CPU
		// CPU_ZERO(&cpu_set);
		// CPU_SET(2, &cpu_set);

		// // Set CPU affinity
		// if (pthread_setaffinity_np(new_task_thread.native_handle(), sizeof(cpu_set_t), &cpu_set) != 0) {
		// 	throw std::runtime_error(std::string("pthread_setaffinity_np: ") +
		// 		std::string(std::strerror(errno)));
		// }

		//std::cout << "Executor: Set thread priority and about to detach!" << std::endl;

		// Detach the thread
		new_task_thread.detach();

		//std::cout << "Executor: Detached!" << std::endl;
	}

	// Busy wait for any outstanding work to finish (in case of spinning set to false)
	std::cout << "Executor: Shutting down - waiting for outstanding work threads!" << std::endl;
	off_t spin_count = 0;
	while (0 < task_queue_p->size()) {
		spin_count++;
		if (spin_count > 10) {
			break;
		}
		std::this_thread::sleep_for (std::chrono::seconds(1));
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

void PreemptivePriorityExecutor::show_any_executable (AnyExecutable *any_executable)
{
	if (nullptr == any_executable) {
		std::cout << "<null>";
		return;
	}
	if (nullptr != any_executable->timer) {
		std::cout << "<timer[" << any_executable->callback_priority << "]>";
		return;
	}
	if (nullptr != any_executable->subscription) {
		std::cout << "<sub[" << any_executable->subscription->get_topic_name()
			<< ", " << any_executable->callback_priority << "]>";
		return;
	}
	std::cout << "<other[?]>";
}


TaskPriorityQueue *PreemptivePriorityExecutor::filter_completed_tasks (TaskPriorityQueue *queue)
{
	TaskPriorityQueue *filtered_queue = new TaskPriorityQueue(task_compare);

	//std::cout << "queue = {";
	// Pop elements
	while (false == queue->empty()) {
		TaskInstance *task_p = queue->top();
		queue->pop();

		// DEBUG:
		AnyExecutable any_executable = task_p->any_executable();
		//show_any_executable(&any_executable);

		// If the task has finished, then destroy it and move on
		// TODO: Maybe a race condition here
		//       Only the workthread can set is finished
		//       if it tries to access after this one deletes
		//       then it is a bad thing - but shouldn't happen
		//       since the last access must be to set is finished
		if (task_p->is_finished()) {
			//std::cout << "Executor: detected thread for ";
			//show_any_executable(&any_executable);
			//std::cout << " done!" << std::endl;
			delete task_p;
			continue;
		}

		//std::cout << ", ";

		// Otherwise: Push it into the new priority queue
		filtered_queue->push(task_p);
	}

	//std::cout << "}" << std::endl; 

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
		return any_executable.callback_priority;
		// const char *node_name = any_executable.node_base->get_name();
		// const char *subscription_topic = any_executable.subscription->get_topic_name();
		// if (false == d_callback_priority_map_p->get_priority_for_node_on_subscription(
		// 	node_name, subscription_topic, &priority))
		// {
		// 	priority = -1;
		// }
	}

	return priority;
}

void PreemptivePriorityExecutor::run (rclcpp::executors::PreemptivePriorityExecutor *executor, TaskInstance *task_p)
{
	//int thread_id = pthread_self();
	std::set<rclcpp::TimerBase::SharedPtr> *scheduled_timers = executor->scheduled_timers();
	//int priority = task_p->task_priority();
	AnyExecutable any_executable = task_p->any_executable();
	std::mutex *wait_mutex_p = executor->wait_mutex();

	// Get thread info
	// pthread_getschedparam(thread_id, &policy, &sch);

	// Print: Start
	// std::cout << "[" << thread_id << "]<" << priority << "> " << "Starting ..." 
	// 		  << std::endl;

	// Work
	executor->execute_any_executable(any_executable);

	// If it is a timer, don't forget to remove it from the scheduled timers set
	{
		std::lock_guard<std::mutex> temp_lock(*wait_mutex_p);
		if (nullptr != any_executable.timer) {
			auto it = scheduled_timers->find(any_executable.timer);
			if (it != scheduled_timers->end()) {
				scheduled_timers->erase(it);
			}
		}
	}

	// Clear the callback group
	any_executable.callback_group.reset();		

	// Print: End
	// std::cout << "[" << thread_id << "]<" << priority << "> " << "Done " 
	// 			<< std::endl;


    // Set value
	task_p->set_is_finished(true);
}

	
std::mutex *PreemptivePriorityExecutor::wait_mutex ()
{
	return &(d_wait_mutex);
}
	
std::set<rclcpp::TimerBase::SharedPtr> *PreemptivePriorityExecutor::scheduled_timers ()
{
	return &(d_scheduled_timers);
}

std::shared_ptr<rclcpp::Context> PreemptivePriorityExecutor::get_context ()
{
	return this->context_;
}

bool PreemptivePriorityExecutor::get_spinning ()
{
	return this->spinning.load();
}