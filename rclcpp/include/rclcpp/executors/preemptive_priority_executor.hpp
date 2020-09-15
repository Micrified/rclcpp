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


namespace rclcpp
{
namespace executors
{


/*
 *******************************************************************************
 *                              Type Definitions                               *
 *******************************************************************************
*/

// Forward declarations
class PreemptivePriorityExecutor;
class Job;

// Type names for the thread main function signature
using thread_func_t = void(rclcpp::executors::PreemptivePriorityExecutor *, rclcpp::executors::Job *);
using thread_func_p = void(*)(rclcpp::executors::PreemptivePriorityExecutor *, rclcpp::executors::Job *);

// I didn't want to do this but the type is just too long
typedef std::priority_queue<Job *, std::vector<Job *>, std::function<bool(Job *, Job *)>> JobPriorityQueue;

// Range of priorities the executor may use for threads: [l_bound, u_bound]
typedef struct {
	int l_bound;
	int u_bound;
} thread_priority_range_t;

// Characteristic of the executor scheduler
typedef enum {
	P_FP,          // Preemptive fixed-priority
	NP_FP,         // Non-preemptive fixed-priority
	P_EDF,         // Preemptive earliest-deadline-first
	NP_EDF         // Non-preemptive earliest-deadline-first
} scheduling_policy_t;


/*
 *******************************************************************************
 *                         Complete Class Definitions                          *
 *******************************************************************************
*/


/// Instance of a running task. A task doesn't quite exist in ROS, so this wraps a callback instead
/**
 * This class is simply a wrapper around the metadata needed to manage a ready callback. 
 * Jobs have the following fields
 * - callback_priority : A priority value used to sort this class within a queue
 * - any_executable    : A wrapper around a ROS executable entity (timer/subscription/etc ...)
 *
 * Jobs have the following state flags: 
 * - running: If dispatched onto a thread. Otherwise held in a priority queue
 * - finished: If marked as such by the assigned thread
 */
class Job {
public:
	Job (int callback_priority, AnyExecutable any_executable):
		d_callback_priority(callback_priority),
		d_any_executable(any_executable),
		d_is_running(false),
		d_is_finished(false)
	{}

	Job (const Job &other)
	{
		d_callback_priority = other.d_callback_priority;
		d_is_running = other.d_is_running;
		d_is_finished.store(other.d_is_finished.load());
	}

	~Job ()
	{}

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

	int callback_priority ()
	{
		return d_callback_priority;
	}

	AnyExecutable any_executable ()
	{
		return d_any_executable;
	}

private:
	int d_callback_priority;
	AnyExecutable d_any_executable;
	bool d_is_running;

	// Protected as accessed by more than one thread
	std::atomic<bool> d_is_finished;
};


/*
 *******************************************************************************
 *                             Class Declarations                              *
 *******************************************************************************
*/


/// A multi-threaded executor with support for preemption and callback priority
/**
 * This class declares the preemptive priority executor. This executor is 
 * multi-threaded and designed to be run on two cores. 
 * 
 * Both the executor thread itself as well as the supporting work threads are
 * designed to be run with the Linux FIFO scheduler, with at least two avail-
 * able cores in the following configuration
 * 
 * Core 0: Executor thread, at maximum priority P
 * Core 1: All other work threads, at priorities < P
 *
 * The executor will perform the configuration itself, but requires two cores
 * to be available. 
 *
 * Operation: This executor waits for work to become available and performs
 * the following: 
 * - If the work has a higher priority than the current busy job, then
 *   a new thread with a higher priority is launched and the current 
 *   job on core 1 is preempted automatically by the kernel
 * - If the work has a lower priority, then it is queued in the priority
 *   queue according to its priority. It will be run later when dequeued
 */
class PreemptivePriorityExecutor : public rclcpp::Executor
{
public:
	RCLCPP_SMART_PTR_DEFINITIONS(PreemptivePriorityExecutor)

	/*\
	 * Creates an instance of the preemptive priority executor.
	 * \param options Common options for all executors
	 * \param priority_range Range of allowed priorities for threads.
	 * \param scheduler_policy Scheduling policy to use
	\*/  
	RCLCPP_PUBLIC 
	PreemptivePriorityExecutor (
		const rclcpp::ExecutorOptions &options = rclcpp::ExecutorOptions(),
		thread_priority_range_t priority_range = {50,99},
		scheduling_policy_t scheduling_policy = P_FP);

	/*\
	 * Destructor for the PreemptivePriorityExecutor
	\*/
	RCLCPP_PUBLIC
	~PreemptivePriorityExecutor();

	/*\
	 * Multiplexes callbacks according to scheduling policy set in constructor
	\*/
	RCLCPP_PUBLIC
	void spin() override;

protected:

	/*\
	 * [DEBUG] Prints an ASCII form of the given AnyExecutable instance to STDOUT
	 * \param any_executable Pointer to the executable container
	\*/
	RCLCPP_PUBLIC
	void show_any_executable (AnyExecutable *any_executable);

	/*\
	 * Allocates a fresh priority queue, and transfers unfinished jobs over to it.
	 * Will also save the number of jobs that are still running to a provided
	 * pointer if set 
	 * \param queue Pointer to the JobPriorityQueue instance to filter
	 * \param n_running_jobs_p Pointer at which to store number of running jobs
	 * \return Pointer to newly allocated priority queue with running instances
	\*/
	RCLCPP_PUBLIC
	JobPriorityQueue *clear_finished_jobs (JobPriorityQueue *queue, 
		int *n_running_jobs_p);


	/*\
	 * Returns priority assigned to given executable. 
	 * Note: Services and clients do not yet have priorities. Returns -1 for them
	 * \param any_executable Reference to executable 
	 * \return Integer describing priority
	\*/
	RCLCPP_PUBLIC
	int get_executable_priority (AnyExecutable &any_executable);

	/*\
	 * Runs a callback. This should be run within a worker thread at a lower priority
	 * than the main executor thread
	 * \param priority The priority assigned to this callback
	 * \param job_p Pointer to the job instance to run
	\*/
	RCLCPP_PUBLIC
	static void run (rclcpp::executors::PreemptivePriorityExecutor *executor, Job *job_p);

	/*\
	 * Returns a pointer to the wait mutex (used for thread access control)
	 * \return Pointer to mutex
	\*/
	RCLCPP_PUBLIC
	std::mutex *wait_mutex ();

	/*\
	 * Returns pointer to the scheduled timers set (set of active timer instances)
	 * \return Pointer to scheduled timer set
	\*/
	RCLCPP_PUBLIC
	std::set<TimerBase::SharedPtr> *scheduled_timers ();

private:

	// Configuration fields
	thread_priority_range_t d_priority_range;
	scheduling_policy_t d_scheduling_policy;

	// Control mutexes
	std::mutex d_io_mutex;
	std::mutex d_wait_mutex;

	// Scheduled timers 
	std::set<TimerBase::SharedPtr> d_scheduled_timers;
};

}
}
#endif