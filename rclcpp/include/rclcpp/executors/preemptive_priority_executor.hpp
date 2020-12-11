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

// Enumeration of (supported) callback types
typedef enum {
	CALLBACK_TIMER,
	CALLBACK_SUBSCRIPTION,
} callback_t;

/*
 *******************************************************************************
 *                         Complete Class Definitions                          *
 *******************************************************************************
*/

class Callback {
public:
	Callback (rclcpp::SubscriptionBase::SharedPtr s, 
		      rclcpp::CallbackGroup::SharedPtr callback_group,
		      rclcpp::node_interfaces::NodeBaseInterface::SharedPtr node_base):
		d_callback_type(CALLBACK_SUBSCRIPTION),
		d_callback_group(callback_group),
		d_node_base(node_base),
		d_subscription(s)
	{

		// TODO: Add support for serialized and loaned messages
		if (s->is_serialized() || s->can_loan_messages()) {
			throw std::runtime_error(std::string("Received serialized or loan(able) message!"));
		}

		// Get message metadata (timestamps, etc)
		d_subscription_message_info.get_rmw_message_info().from_intra_process = false;

		// Obtain a dynamically allocated message using the allocator strategy
		d_subscription_message = s->create_message();

		try {
			s->take_type_erased(d_subscription_message.get(), d_subscription_message_info);
		} catch (const rclcpp::exceptions::RCLError &rcl_error) {
			throw std::runtime_error(std::string("Executor failed to take copied message for topic: \"") +
				std::string(s->get_topic_name()) + std::string("\" RCL: ") + 
				std::string(rcl_error.what()));
		}

		// Calls: handle_message and return_message will be invoked when execution needed
	}

	Callback (rclcpp::TimerBase::SharedPtr t,
		      rclcpp::CallbackGroup::SharedPtr callback_group,
		      rclcpp::node_interfaces::NodeBaseInterface::SharedPtr node_base):
		d_callback_type(CALLBACK_TIMER),
		d_callback_group(callback_group),
		d_node_base(node_base),
		d_subscription(nullptr),
		d_timer(t)
	{
		// execute_callback() in class GenericTimer in timer.hpp implements the CB for RCL timers
		// it calls rcl_timer_call, but you should only call that if the period has elapsed yet
		// and are responsible to tracking this.
		// I don't want to make the call yet, so postpone this decision
	}

	// Move constructor
	Callback (const Callback &&other)
	{
		d_callback_type = other.d_callback_type;
		d_callback_group = std::move(other.d_callback_group);
		d_node_base = std::move(other.d_node_base);
		d_subscription = std::move(other.d_subscription);
		d_timer = std::move(other.d_timer);
		d_subscription_message_info = other.d_subscription_message_info;
		d_subscription_message = std::move(other.d_subscription_message);
	}

	void execute ()
	{
		switch (d_callback_type) {
			case CALLBACK_TIMER: {
				this->execute_timer();
			}
			break;

			case CALLBACK_SUBSCRIPTION: {
				this->execute_subscription();
			}
			break;
		}
	}

	std::string description ()
	{
		switch (d_callback_type) {
			case CALLBACK_TIMER: {
				return std::string("timer");
			}
			case CALLBACK_SUBSCRIPTION: {
				return std::string("sub(") + std::string(d_subscription->get_topic_name())
					+ std::string(")");
			}
		}
		return std::string("unknown");
	}

	callback_t callback_type ()
	{
		return d_callback_type;
	}

	rclcpp::CallbackGroup::SharedPtr callback_group ()
	{
		return d_callback_group;
	}

	rclcpp::node_interfaces::NodeBaseInterface::SharedPtr node_base ()
	{
		return d_node_base;
	}

	rclcpp::SubscriptionBase::SharedPtr subscription ()
	{
		return d_subscription;
	}

	rclcpp::TimerBase::SharedPtr timer ()
	{
		return d_timer;
	}

protected:
	void execute_subscription ()
	{
		d_subscription->handle_message(d_subscription_message, d_subscription_message_info);
		d_subscription->return_message(d_subscription_message);
	}

	void execute_timer ()
	{
		d_timer->execute_callback();
	}

private:

	// Identifies type of callback
	callback_t d_callback_type;

	// Executable metadata
	rclcpp::CallbackGroup::SharedPtr d_callback_group;
	rclcpp::node_interfaces::NodeBaseInterface::SharedPtr d_node_base;

	// Subscribion
	rclcpp::SubscriptionBase::SharedPtr d_subscription;

	// Timer
	rclcpp::TimerBase::SharedPtr d_timer;

	// Subscription metadata
	rclcpp::MessageInfo d_subscription_message_info;
	std::shared_ptr<void> d_subscription_message;

};

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
		d_is_running(false),
		d_is_finished(false),
		d_callback_ptr(nullptr)
	{
		if (nullptr != any_executable.timer) {
			d_callback_ptr = std::make_shared<Callback>(any_executable.timer, any_executable.callback_group,
				any_executable.node_base);
			return;
		}
		if (nullptr != any_executable.subscription) {
			d_callback_ptr = std::make_shared<Callback>(any_executable.subscription, any_executable.callback_group,
				any_executable.node_base);
			return;
		}

		throw std::runtime_error("Invalid executable type provided!");
	}

	Job (const Job &&other)
	{
		d_callback_priority = other.d_callback_priority;
		d_is_running = other.d_is_running;
		d_is_finished.store(other.d_is_finished.load());
		d_callback_ptr = other.d_callback_ptr;
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

	std::string description ()
	{
		return std::string("[") + std::to_string(d_callback_priority)
			+ std::string("]:") + d_callback_ptr->description();
	}

	// AnyExecutable any_executable ()
	// {
	// 	return d_any_executable;
	// }

	std::shared_ptr<Callback> get_callback_ptr ()
	{
		return d_callback_ptr;
	}

private:
	int d_callback_priority;
	//AnyExecutable d_any_executable;

	bool d_is_running;

	// Protected as accessed by more than one thread
	std::atomic<bool> d_is_finished;

	// The callback
	std::shared_ptr<Callback> d_callback_ptr;
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
		thread_priority_range_t priority_range = {50,90},
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

	/*\
	 * Multiplexes callbacks according to the scheduling policy set in the 
	 * constructor. Runs until the given duration has expired. 
	 * Note: Threads busy running jobs will not check the timer until their next
	 *       job
	 * \param duration Duration to spin (ns). If zero, then spin indefinitely
	\*/
	RCLCPP_PUBLIC
	void spin_some (std::chrono::nanoseconds max_duration = std::chrono::nanoseconds(0)) override;

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
	 * Returns a pointer to the subscription mutex (used for thread access control)
	 * \return Pointer to mutex
	\*/
	RCLCPP_PUBLIC
	std::mutex *sub_mutex ();

	/*\
	 * Returns pointer to the scheduled timers set (set of active timer instances)
	 * \return Pointer to scheduled timer set
	\*/
	RCLCPP_PUBLIC
	std::set<TimerBase::SharedPtr> *scheduled_timers ();

	/*\
	 * Returns pointer to the scheduled subscription set (set of active subscription instances)
	 * \return Pointer to scheduled subscription set
	\*/
	RCLCPP_PUBLIC
	std::set<SubscriptionBase::SharedPtr> *scheduled_subscriptions ();

	/*\
	 * Returns a string object describing the executor mode and priority ranges
	 * \return String describing executor
	\*/
	std::string description ();

private:

	// Configuration fields
	thread_priority_range_t d_priority_range;
	scheduling_policy_t d_scheduling_policy;

	// Control mutexes
	std::mutex d_io_mutex;
	std::mutex d_wait_mutex;
	std::mutex d_sub_mutex;

	// Scheduled timers 
	std::set<TimerBase::SharedPtr> d_scheduled_timers;

	// Pending callbacks hashmap
	std::set<SubscriptionBase::SharedPtr> d_scheduled_subscriptions;


	// Thread counter
	std::atomic<int> d_thread_count;
};

}
}
#endif