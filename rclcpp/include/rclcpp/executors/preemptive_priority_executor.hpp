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
#include <unordered_set>
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


// Forward class declarations
class PreemptivePriorityExecutor;
class Callback;

// Directives
using Callback_Ptr = std::shared_ptr<Callback>;


// Range of priorities the executor may use for threads: [l_bound, u_bound]
typedef struct {
	int l_bound;
	int u_bound;
} thread_priority_range_t;


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

	// Subscription
	rclcpp::SubscriptionBase::SharedPtr d_subscription;

	// Timer
	rclcpp::TimerBase::SharedPtr d_timer;

	// Subscription metadata
	rclcpp::MessageInfo d_subscription_message_info;
	std::shared_ptr<void> d_subscription_message;

};


/*\
 * The consumer class is named after the classic producer-consumer problem. 
 * As implied by the name, the class is designed to provide supporting 
 * infrastructure for a consuming process. This process should:
 * (a) Process items as long as they are present in the queue
 * (b) Sleep when no items are in the queue, to be awakened when more arrives.
 * Unlike the classical problem, we don't want our producer process to be 
 * awoken when all items have been consumed. So it is only a one-way activation.
 * 
 * This class is designed to be used to manage a single FIFO item queue to 
 * which a single work-thread is dedicated. Because member class functions may 
 * not be executed as the body of a thread unless they are static, the thread
 * task is provided seperately, along with a callback to be run on items once 
 * dequeued.
 *
 * In order to support testing, a duration may be provided after which the 
 * work-thread will stop. In order to make sure the thread doesn't get stuck
 * asleep (hence ignoring the expiration date), a timeout is also available 
 * as a constructor parameter (defaulting to 100ms). The thread will wake 
 * periodically to check if it needs to stop in accordance with the timeout. 
 * This also means that the thread may halt up to 
 *                      max(timeout, <duration_of_current_callback>)
 * Since it is unable to check the expiration date if busy running user code. 
 *
 * The mechanism for managing the consumer is a condition variable and mutex. 
 * The mutex guards access to the work queue, and the condition variable can 
 * be notified in order to awaken the consumer thread. You may read more about
 * how it works at: https://en.cppreference.com/w/cpp/thread/condition_variable
\*/
template<class T>
class Consumer {
private:

	// Desired lifetime of consumer process
	std::chrono::nanoseconds d_duration;

	// Granularity at which process checks for expired duration
	std::chrono::nanoseconds d_timeout;

	// Item queue
	std::queue<T> d_queue;

	// Queue control mutex
	std::mutex d_mutex;

	// Consumer process condition variable (wakes sleeping process)
	std::condition_variable d_condition_variable;

public:

	// Constructor
	Consumer (std::chrono::nanoseconds duration = std::chrono::nanoseconds(0),
		std::chrono::nanoseconds timeout = std::chrono::nanoseconds(100000000)):
		d_duration(duration),
		d_timeout(timeout)
	{}

	// Destructor
	~Consumer () = default;

	// Pushes an item to the internal queue
	void enqueue (T item)
	{
		d_queue.push(item);
	}

	// Returns nonzero on success. Places item at item_ref
	int dequeue (T *item_ptr)
	{
		if (d_queue.size() > 0) {
			*item_ptr = d_queue.front();
			d_queue.pop();
			return 1;
		}
		return 0;
	}

	// Returns a reference to the internal mutex
	std::mutex *mutex ()
	{ 
		return &d_mutex; 
	}

	// Returns a reference to the internal condition variable
	std::condition_variable *condition_variable ()
	{
		return &d_condition_variable;
	}

	// Returns a copy of the set duration
	std::chrono::nanoseconds duration()
	{
		return d_duration;
	}

	// Returns a copy of the set timeout
	std::chrono::nanoseconds timeout()
	{
		return d_timeout; 
	}
};


/*
 *******************************************************************************
 *                             Class Declarations                              *
 *******************************************************************************
*/


/// A multi-threaded executor with support for preemption and callback priority.
/**
 * This class declares the preemptive-priority executor. This executor is
 * multi-threaded and designed to be run on two cores. 
 *
 * All threads are setup to run with the Linux FIFIO scheduler, and in the
 * following configuration:
 *
 * Core 0: Multiplexer thread, at maximum priority P
 * Core 1: Work threads, at priorities < P
 *
 * The number of work threads depends on how many priority values you provide
 * to the executor in the constructor. Given a range [a,b], then priority b will 
 * be reserved by the multiplexer thread, and priorities [a,b) given dedicated 
 * consumer threads. These threads normally sleep, unless work is placed into 
 * their work queues. If this is done, they then execute the work and go back
 * to sleep. 
 *
 * The work threads themselves take on a priority in range [a,b), and thus work
 * of different importance can be scheduled to different slots. The executor 
 * relies on the OS scheduler to provide preemption. 
 * Work to be done with equal priority is executed in FIFO order
 */
class PreemptivePriorityExecutor : public rclcpp::Executor
{
public:
	RCLCPP_SMART_PTR_DEFINITIONS(PreemptivePriorityExecutor)

	/*\
	 * Creates an instance of the preemptive-priority executor.
	 * \param options Common executor options
	 * \param priority_range Range of OS priorities to use
	\*/
	RCLCPP_PUBLIC
	PreemptivePriorityExecutor (
		const rclcpp::ExecutorOptions &options = rclcpp::ExecutorOptions(),
		thread_priority_range_t priority_range = {95,99});

	/*\
	 * Destructor
	\*/
	RCLCPP_PUBLIC
	~PreemptivePriorityExecutor();

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
	 * Main loop function for a consumer thread. This 
	 * thread sleeps unless woken to consume work in a 
	 * queue. If woken, it then executes all work found
	 * and returns to idle.
	\*/
	RCLCPP_PUBLIC
	void run (Consumer<Callback_Ptr> *c, int thread_id);

	/*\
	 * Main loop function for an executor thread. This
	 * thread multiplexes callbacks and places them into
	 * the right consumer queues depending on their given
	 * priority value.
	\*/
	RCLCPP_PUBLIC
	void multiplex (std::chrono::nanoseconds max_duration,
		std::vector<Consumer<Callback_Ptr> *> *consumers_p);


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

#endif