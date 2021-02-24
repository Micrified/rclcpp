#ifndef EXECUTOR_BASE
#define EXECUTOR_BASE

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

}
}

#endif