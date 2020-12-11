
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
using rclcpp::executors::Job;
using rclcpp::executors::JobPriorityQueue;
using rclcpp::executors::thread_priority_range_t;
using rclcpp::executors::scheduling_policy_t;


/*
 *******************************************************************************
 *                             Symbolic Constants                              *
 *******************************************************************************
*/


#define EXECUTOR_THREAD_CORE        0
#define JOB_THREAD_CORE             1


/*
 *******************************************************************************
 *                            Forward Declarations                             *
 *******************************************************************************
*/


/*\
 * Prototype of the function provided for sorting the job priority queue.
 * \param a Pointer to a job instance
 * \param b Pointer to a job instance
 * \return True if 'a' should come before 'b'
\*/
static bool job_sort_comparator (Job *a, Job *b);


/*
 *******************************************************************************
 *                           Constructor/Destructor                            *
 *******************************************************************************
*/

PreemptivePriorityExecutor::PreemptivePriorityExecutor (
	const rclcpp::ExecutorOptions &options,
	thread_priority_range_t priority_range,
	scheduling_policy_t scheduling_policy)
:
	rclcpp::Executor(options),
	d_priority_range(priority_range),
	d_scheduling_policy(scheduling_policy)
{
	// TODO: Support more than one scheduling policy
	if (P_FP != scheduling_policy && NP_FP != scheduling_policy) {
		throw std::invalid_argument(std::string("Scheduling policy not supported!"));
	}

	// Check: Range is logical
	if ((d_priority_range.u_bound - d_priority_range.l_bound) <= 1) {
		throw std::invalid_argument(std::string("Bad priority range: ") + 
			std::string("(u_bound - l_bound) must be at least 2!"));
	}

	// Check: Range makes sense in terms of linux priorites
	if (d_priority_range.u_bound > 99 || d_priority_range.l_bound < 0) {
		throw std::invalid_argument(std::string("Priority range must be between [0,99]"));
	}
}


PreemptivePriorityExecutor::~PreemptivePriorityExecutor ()
{
}


/*
 *******************************************************************************
 *                               Public Methods                                *
 *******************************************************************************
*/

inline void pin_to_core (pthread_t thread, int core, cpu_set_t *cpu_set_p)
{
	CPU_ZERO(cpu_set_p);
	CPU_SET(core, cpu_set_p);
	if (0 != pthread_setaffinity_np(thread, sizeof(cpu_set_t), cpu_set_p)) {
		throw std::runtime_error(std::string("pthread_setaffinity_np: ") +
			std::string(std::strerror(errno)));
	}
}

inline void set_thread_priority (pthread_t thread, int priority, int *policy_p, 
	sched_param *sch_p)
{
	pthread_getschedparam(thread, policy_p, sch_p);
	sch_p->sched_priority = priority;
	if (0 != pthread_setschedparam(thread, SCHED_FIFO, sch_p)) {
		throw std::runtime_error(std::string("pthread_setschedparam: ") + 
			std::string(std::strerror(errno)));
	}
}


void PreemptivePriorityExecutor::spin ()
{
	spin_some(std::chrono::nanoseconds(0));
}

//#define debug

// TODO: Attempt to use the RCL interface to just fucking take the messages, and then
// execute them later :) 

void PreemptivePriorityExecutor::spin_some (std::chrono::nanoseconds max_duration)
{
	sched_param sch, sch_old;
	int policy, policy_old;
	cpu_set_t cpu_set;

	// Note: (u_bound - l_bound - 1 preemptions available)

	// Output mode
	std::cout << std::string("\033[1;33m") + this->description() + std::string("\033[0m\n");

	// Create job priority queue
	JobPriorityQueue *job_queue_p = new JobPriorityQueue(job_sort_comparator);

	// Check: Not already spinning
	if (true == spinning.exchange(true)) {
		throw std::runtime_error("May not call spin() while already spinning!");
	}

	// Defer: On exit, we want to stop spinning
	RCLCPP_SCOPE_EXIT(this->spinning.store(false););

	// Pin to CPU 0
	pin_to_core(pthread_self(), EXECUTOR_THREAD_CORE, &cpu_set);

	// Get the current policy and save it
	pthread_getschedparam(pthread_self(), &policy, &sch);
	sch_old = sch;
	policy_old = policy;

	// Apply FIFO policy with priority
	set_thread_priority(pthread_self(), d_priority_range.u_bound, &policy, &sch);

	// Mark the start time
	auto start = std::chrono::steady_clock::now();

	// Define a closure for checking if duration expired
	auto callback_should_still_spin = [max_duration, start]()
	{
		// Case: Spin forever
		if (std::chrono::nanoseconds(0) == max_duration) return true;
		return ((std::chrono::steady_clock::now() - start) < max_duration);
	};

	// TODO: Consider alternative architecture. Perhaps a pool of work
	//       threads like MultiThreadedExecutor that just pick up new
	//       work from a priority-queue when awoken by the executor
	//       main thread. Avoids overhead of spawning threads
	while (rclcpp::ok(this->context_) && spinning.load() && 
		true == callback_should_still_spin()) {
		AnyExecutable any_executable;
		int n_running_jobs = 0;

		// Wait for work ...
#ifdef debug
		//std::cout << "- - - - - - - - - - - - - Threads: " << std::to_string(d_thread_count)
		//          << std::endl;
#endif
		wait_for_work(std::chrono::nanoseconds(-1));

		// Check: Was spinning disabled while waiting
		if (false == spinning.load()) {
			break;
		}

		// Grab the vector of all ready executables
		std::vector<AnyExecutable> *ready_executables = this->get_all_ready_executables();

		// Process all elements
		for (auto e : *ready_executables) {
			bool is_new_job = true;

			// Case: Timer: Check if already handled
			if (nullptr != e.timer) {
				std::lock_guard<std::mutex> temp_lock(d_wait_mutex);

				// If already a busy timer: ignore. Else: insert
				if (0 < d_scheduled_timers.count(e.timer)) {

					// Reset this (done in MultiThreadedExecutor)
					if (nullptr != e.callback_group) {
						e.callback_group->can_be_taken_from().store(true);
					}

					// Not a new job
					is_new_job = false;
				} else {
					d_scheduled_timers.insert(e.timer);
				}
			}

			// Case: Subscription: Check if already handled
			if (nullptr != e.subscription) {
				std::lock_guard<std::mutex> temp_lock(d_sub_mutex);

				// If already a busy subscription: ignore. Else: insert
				if (0 < d_scheduled_subscriptions.count(e.subscription)) {
					is_new_job = false;
				} else {
					d_scheduled_subscriptions.insert(e.subscription);
				}
			}

			// If not a new job, then continue
			if (false == is_new_job) {
				continue;
			}

			// Otherwise insert into priority queue
			// TODO: get_executable_priority is redundant so remove
			Job *new_job_ptr = new Job(e.callback_priority,
				std::move(e));

			job_queue_p->push(new_job_ptr);

			//e.callback_group.reset();
		}

		// Destroy the set
		delete ready_executables;

		// Clear any finished jobs
		job_queue_p = this->clear_finished_jobs(job_queue_p, &n_running_jobs);

		// Reset and wait if no jobs to consider
		if (job_queue_p->size() == 0) {
			continue;
		}

		// Dequeue highest priority job
		Job *highest_priority_job = job_queue_p->top();

		// If it is busy, do nothing
		if (true == highest_priority_job->is_running()) {
#ifdef debug
			//AnyExecutable a = highest_priority_job->any_executable();
			//show_any_executable(&a);
			std::cout << highest_priority_job->description() << ": is_running -> continue" << std::endl;
#endif
			continue;
		}

		// If not busy but other threads are - do not preempt in non-preemptive mode!
		if (NP_FP == d_scheduling_policy && n_running_jobs > 0) {
			continue;
		}

#ifdef debug
		// AnyExecutable a = highest_priority_job->any_executable();
		// show_any_executable(&a);
		std::cout << highest_priority_job->description() << " launching" << std::endl;
#endif

		// Else: Mark it busy (we will now create and launch it)
		highest_priority_job->set_is_running(true);

		// Create a thread and run it
		std::thread new_job_thread(std::move(PreemptivePriorityExecutor::run),
			this, highest_priority_job);

		// Compute thread priority
		int job_thread_priority = d_priority_range.l_bound + highest_priority_job->callback_priority();

		// Ensure it doesn't exceed or equal the max bound
		if (job_thread_priority >= d_priority_range.u_bound) {
			throw std::runtime_error(std::string("Job level priority exceeds range bounds for the executor: ")
				+ std::string("(l_bound + ") +  std::to_string(job_thread_priority)
				+ std::string(") >= u_bound"));
		}

		// Apply thread priority
		set_thread_priority(new_job_thread.native_handle(), job_thread_priority, &policy, &sch);

		// Pin to CPU 1
		pin_to_core(new_job_thread.native_handle(), JOB_THREAD_CORE, &cpu_set);

		// Detach the thread
		new_job_thread.detach();

		// Increment counter
		d_thread_count++;
	}

	// Busy wait for any outstanding work to finish (in case of spinning set to false)
#ifdef debug
	std::cout << "Executor: Shutting down - waiting for outstanding work threads ... " << std::endl;
#endif

	off_t spin_count = 0;
	while (0 < job_queue_p->size()) {
		spin_count++;
		if (spin_count > 10) {
			break;
		}
		std::this_thread::sleep_for(std::chrono::seconds(1));
		job_queue_p = this->clear_finished_jobs(job_queue_p, nullptr);
	}

	// Free allocated objects
	delete job_queue_p;

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


static bool job_sort_comparator (Job *a, Job *b)
{
	return (a->callback_priority() < b->callback_priority());
}

void PreemptivePriorityExecutor::show_any_executable (AnyExecutable *any_executable)
{
	if (nullptr == any_executable) {
		std::cout << "<null>";
		return;
	}
	if (nullptr != any_executable->timer) {
		std::cout << "<timer[prio=" << any_executable->callback_priority << "]>";
		return;
	}
	if (nullptr != any_executable->subscription) {
		std::cout << "<sub[topic=" << any_executable->subscription->get_topic_name()
			<< ", prio=" << any_executable->callback_priority << "]>";
		return;
	}
	std::cout << "<other[?]>";
}


JobPriorityQueue *PreemptivePriorityExecutor::clear_finished_jobs (JobPriorityQueue *queue,
	int *n_running_jobs_p)
{
	int n_running_jobs = 0;
	JobPriorityQueue *filtered_queue = new JobPriorityQueue(job_sort_comparator);

	// Pop elements
#ifdef debug
	std::cout << "jobs[" << queue->size() << "] = {";
#endif

	while (false == queue->empty()) {
		Job *job_p = queue->top();
		queue->pop();

		// Remove job if finished
		if (job_p->is_finished()) {
			delete job_p;
			d_thread_count--;
			continue;
		}

#ifdef debug
		// AnyExecutable a = job_p->any_executable();
		// show_any_executable(&a);
		std::cout << job_p->description();
		if (queue->size() > 0) {
			std::cout << ", ";
		}
#endif

		// Increment running job counter if not finished
		if (job_p->is_running()) {
			n_running_jobs++;
		}

		// Otherwise: Push it into the new priority queue
		filtered_queue->push(job_p);
	}

#ifdef debug
	std::cout << "}" << std::endl;
#endif

	// Destroy the old queue
	delete queue;

	// Save number of running jobs counted if pointer provided
	if (nullptr != n_running_jobs_p) {
		*n_running_jobs_p = n_running_jobs;
	}

	return filtered_queue;
}

void PreemptivePriorityExecutor::run (rclcpp::executors::PreemptivePriorityExecutor *executor, Job *job_p)
{
	std::set<rclcpp::TimerBase::SharedPtr> *scheduled_timers = executor->scheduled_timers();
	std::set<rclcpp::SubscriptionBase::SharedPtr> *scheduled_subscriptions = 
		executor->scheduled_subscriptions();
	std::mutex *wait_mutex_p = executor->wait_mutex();
	std::mutex *sub_mutex_p = executor->sub_mutex();



	// Work
	// 	AnyExecutable any_executable = job_p->any_executable();
	// executor->execute_any_executable(any_executable);
	std::shared_ptr<Callback> callback_ptr = job_p->get_callback_ptr();

	callback_ptr->execute();

	// If it is a timer, don't forget to remove it from the scheduled timers set
	{
		std::lock_guard<std::mutex> temp_lock(*wait_mutex_p);
		//if (nullptr != any_executable.timer) {
		if (nullptr != callback_ptr->timer()) {
			auto it = scheduled_timers->find(callback_ptr->timer());
			if (it != scheduled_timers->end()) {
				scheduled_timers->erase(it);
			}
		}
	}

	// If it is a subscription, don't forget to remove it from the subscription set
	{
		std::lock_guard<std::mutex> temp_lock(*sub_mutex_p);
		//if (nullptr != any_executable.subscription) {
		if (nullptr != callback_ptr->subscription()) {
			auto it = scheduled_subscriptions->find(callback_ptr->subscription());
			if (it != scheduled_subscriptions->end()) {
				scheduled_subscriptions->erase(it);
			}
		}
	}

	// Clear the callback group
	//any_executable.callback_group.reset();
	callback_ptr->callback_group().reset();

    // Set value
	job_p->set_is_finished(true);
}

	
std::mutex *PreemptivePriorityExecutor::wait_mutex ()
{
	return &(d_wait_mutex);
}

std::mutex *PreemptivePriorityExecutor::sub_mutex ()
{
	return &(d_sub_mutex);
}
	
std::set<rclcpp::TimerBase::SharedPtr> *PreemptivePriorityExecutor::scheduled_timers ()
{
	return &(d_scheduled_timers);
}

std::set<rclcpp::SubscriptionBase::SharedPtr> *PreemptivePriorityExecutor::scheduled_subscriptions ()
{
	return &(d_scheduled_subscriptions);
}

std::string PreemptivePriorityExecutor::description ()
{
	std::string desc("Preemptive-Priority-Executor {.mode = ");
	switch (d_scheduling_policy) {
		case P_FP:   desc += std::string("P-FP");     break;
		case NP_FP:  desc += std::string("NP-FP");    break;
		case P_EDF:  desc += std::string("P-EDF");    break;
		case NP_EDF: desc += std::string("NP-EDF");   break;
	}
	desc += std::string(", .prio_range = [");
	desc += std::to_string(d_priority_range.l_bound);
	desc += std::string(",");
	desc += std::to_string(d_priority_range.u_bound);
	desc += std::string("]}");
	return desc;
}