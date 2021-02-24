#ifndef THREAD_DISPATCH_EXECUTOR
#define THREAD_DISPATCH_EXECUTOR


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
#include <unordered_set>
#include <functional>
#include <sched.h>

// RCLCPP headers
#include "rclcpp/executor.hpp"
#include "rclcpp/macros.hpp"
#include "rclcpp/visibility_control.hpp"

// Custom headers
#include "rclcpp/executor_base.hpp"

namespace rclcpp
{
namespace executors
{
namespace PPE 
{


/*
 *******************************************************************************
 *                              Type Definitions                               *
 *******************************************************************************
*/


// Forward class declarations
class ThreadDispatchExecutor;

// Directives
using rclcpp::executors::Callback;
using rclcpp::executors::thread_priority_range_t;
using rclcpp::executors::callback_t;
using Callback_Ptr = std::shared_ptr<Callback>;


/*
 *******************************************************************************
 *                             Class Declarations                              *
 *******************************************************************************
*/


/// A multi-threaded executor with support for preemption and callback priority.
/**
 * This class declares the thread dispatch executor. This executor is
 * multi-threaded and designed to be run on two cores. 
 *
 * All threads are setup to run with the Linux real-time FIFO scheduler, and 
 * in the following configuration:
 *
 * Core 0: Multiplexer thread, at maximum priority P
 * Core 1: Work threads, at priorities < P
 *
 * The executor launches work-threads with priority values in the range [a,b),
 * with it taking the highest priority value b. Each arriving callback has a 
 * dedicated thread dispatched for it, and that thread takes on the priority
 * value of the callback. Once the threads are finished, they destroy themselves.
 *
 * The executor relies on the OS scheduler to provide preemption. 
 * Work to be done with equal priority is executed in FIFO order
 */
class ThreadDispatchExecutor : public rclcpp::Executor
{
public:
	RCLCPP_SMART_PTR_DEFINITIONS(ThreadDispatchExecutor)

	/*\
	 * Creates an instance of the thread-dispatch executor.
	 * \param options Common executor options
	 * \param priority_range Range of OS priorities to use
	\*/
	RCLCPP_PUBLIC
	ThreadDispatchExecutor (
		const rclcpp::ExecutorOptions &options = rclcpp::ExecutorOptions(),
		thread_priority_range_t priority_range = {95,99});

	/*\
	 * Destructor
	\*/
	RCLCPP_PUBLIC
	~ThreadDispatchExecutor();

	/*\
	 * Multiplexes callbacks as described in class description
	\*/
	RCLCPP_PUBLIC
	void spin() override;

	/*\
	 * Multiplexes callbacks as described in class description,
	 * but runs only until the given duration has expired.
	 * Note: Work threads may take more time to finish, as they
	 * cannot stop when busy executing a callback, or may be 
	 * sleeping (in which case they close down after waking
	 * routinely according to a set timeout)
	\*/
	RCLCPP_PUBLIC
	void spin_some (std::chrono::nanoseconds max_duration =
		std::chrono::nanoseconds(0)) override;

protected:

	/*\
	 * Main loop function for a dispatch thread. This 
	 * thread executes its callback and then destroys
	 * itself
	\*/
	RCLCPP_PUBLIC
	static void run (rclcpp::executors::PPE::ThreadDispatchExecutor *executor, 
		Callback_Ptr callback);

	/*\
	 * Main loop function for an executor thread. This
	 * thread multiplexes callbacks and dispatches 
	 * threads to fulfill them
	\*/
	RCLCPP_PUBLIC
	void multiplex (std::chrono::nanoseconds max_duration);


	/*\
	 * Moves an AnyExecutable instance into a callback
	 * wrapper. The callback wrapper is only created
	 * if the executable (subscription/timer) has not
	 * already been handled. I'm not sure why this behavior
	 * exists, but without this duplicate callbacks can appear
	 * in the wait-set.
	\*/
	RCLCPP_PUBLIC
	Callback_Ptr executable_to_callback (AnyExecutable e);

	/*\
	 * Removes the given executable (timer/subscription)
	 * from the internal sets tracking which executables are
	 * currently already being handled.
	\*/
	RCLCPP_PUBLIC
	void remove_expired_executable (Callback_Ptr callback);


	/*\
	 * Returns a string describing the executor
	\*/
	RCLCPP_PUBLIC
	std::string description();

private:

	// Configuration fields
	thread_priority_range_t d_priority_range;

	// Control mutexes
	std::mutex d_timer_set_mutex;
	std::mutex d_subscription_set_mutex;

	// Set containing already-handled timers
	std::unordered_set<TimerBase::SharedPtr> d_handled_timers;

	// Set containing already-handled subscriptions
	std::unordered_set<SubscriptionBase::SharedPtr> d_handled_subscriptions;
};

}
}
}

#endif