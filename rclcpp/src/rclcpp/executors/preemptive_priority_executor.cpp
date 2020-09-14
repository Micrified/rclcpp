
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
	if (P_FP != scheduling_policy) {
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
	sched_param sch, sch_old;
	int policy, policy_old;
	cpu_set_t cpu_set;

	// Note: (u_bound - l_bound - 1 preemptions available)

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


	// TODO: Consider alternative architecture. Perhaps a pool of work
	//       threads like MultiThreadedExecutor that just pick up new
	//       work from a priority-queue when awoken by the executor
	//       main thread. Avoids overhead of spawning threads
	while (rclcpp::ok(this->context_) && spinning.load()) {
		AnyExecutable any_executable;

		// Wait for work ...
		wait_for_work(std::chrono::nanoseconds(-1));

		// Clear any finished jobs
		job_queue_p = this->clear_finished_jobs(job_queue_p);

		// Check: Was spinning disabled while waiting
		if (false == spinning.load()) {
			break;
		}

		// If: A new executable instance is ready
		if (true == this->get_next_ready_executable(any_executable)) {
			bool is_new_job = true;

			// If: Timer, then check not already handled
			if (nullptr != any_executable.timer) {
				std::lock_guard<std::mutex> temp_lock(d_wait_mutex);

				// If: Busy timers, then check if one of them.
				if (0 < d_scheduled_timers.count(any_executable.timer)) {

					// Reset this (done in MultiThreadedExecutor)
					if (nullptr != any_executable.callback_group) {
						any_executable.callback_group->can_be_taken_from().store(true);
					}

					// Not a new job - already exists!
					is_new_job = false;
				} else {
					
				// Else: No busy timers - so insert this since it will be a new job
					d_scheduled_timers.insert(any_executable.timer);
				}
			}

			// Note: Must remove timer from callback group after: Launched thread does this


			// If: Is a new instance, then compute a priority for this callback
			if (is_new_job) {
				int new_job_priority = this->get_executable_priority(any_executable);

				// Create a new job
				Job *new_job_ptr = new Job(new_job_priority, std::move(any_executable));

				// Push job to priority queue
				job_queue_p->push(new_job_ptr);			
			}
		}

		// Reset and wait if no jobs to consider
		if (job_queue_p->size() == 0) {
			continue;
		}

		// Dequeue highest priority job
		Job *highest_priority_job = job_queue_p->top();

		// If it is busy, do nothing
		if (true == highest_priority_job->is_running()) {
			continue;
		}

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
	}

	// Busy wait for any outstanding work to finish (in case of spinning set to false)
	std::cout << "Executor: Shutting down - waiting for outstanding work threads ... " << std::endl;
	off_t spin_count = 0;
	while (0 < job_queue_p->size()) {
		spin_count++;
		if (spin_count > 10) {
			break;
		}
		std::this_thread::sleep_for(std::chrono::seconds(1));
		job_queue_p = this->clear_finished_jobs(job_queue_p);
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


JobPriorityQueue *PreemptivePriorityExecutor::clear_finished_jobs (JobPriorityQueue *queue)
{
	JobPriorityQueue *filtered_queue = new JobPriorityQueue(job_sort_comparator);

	//std::cout << "queue = {";
	// Pop elements
	while (false == queue->empty()) {
		Job *job_p = queue->top();
		queue->pop();

		// DEBUG:
		AnyExecutable any_executable = job_p->any_executable();
		//show_any_executable(&any_executable);

		// If the task has finished, then destroy it and move on
		// TODO: Maybe a race condition here
		//       Only the workthread can set is finished
		//       if it tries to access after this one deletes
		//       then it is a bad thing - but shouldn't happen
		//       since the last access must be to set is finished
		if (job_p->is_finished()) {
			//std::cout << "Executor: detected thread for ";
			//show_any_executable(&any_executable);
			//std::cout << " done!" << std::endl;
			//std::cout << "{x}";
			delete job_p;
			continue;
		}

		//std::cout << ", ";

		// Otherwise: Push it into the new priority queue
		filtered_queue->push(job_p);
	}

	//std::cout << "}" << std::endl; 

	// Destroy the old queue
	delete queue;

	return filtered_queue;
}


int PreemptivePriorityExecutor::get_executable_priority (AnyExecutable &any_executable)
{
	int priority = -1;
	size_t n_priorities = 0;

	// If it is a timer, then use the supplied value (set via the timer constructor)
	if (nullptr != any_executable.timer) {
		n_priorities++;
		priority = any_executable.callback_priority;
	}

	// If it is a subscription, lookup the priority in the map
	if (nullptr != any_executable.subscription) {
		n_priorities++;
		priority = any_executable.callback_priority;
	}


	if (n_priorities > 1) {
		throw std::runtime_error("An instance of AnyExecutable had more than one field set!");
	}

	return priority;
}

void PreemptivePriorityExecutor::run (rclcpp::executors::PreemptivePriorityExecutor *executor, Job *job_p)
{
	std::set<rclcpp::TimerBase::SharedPtr> *scheduled_timers = executor->scheduled_timers();
	AnyExecutable any_executable = job_p->any_executable();
	std::mutex *wait_mutex_p = executor->wait_mutex();

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

    // Set value
	job_p->set_is_finished(true);
}

	
std::mutex *PreemptivePriorityExecutor::wait_mutex ()
{
	return &(d_wait_mutex);
}
	
std::set<rclcpp::TimerBase::SharedPtr> *PreemptivePriorityExecutor::scheduled_timers ()
{
	return &(d_scheduled_timers);
}