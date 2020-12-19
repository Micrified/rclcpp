// Copyright 2014 Open Source Robotics Foundation, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


// Standard headers
#include <algorithm>
#include <memory>
#include <string>
#include <type_traits>


// RCL headers
#include "rcl/allocator.h"
#include "rcl/error_handling.h"

// RCLCPP headers
#include "rclcpp/exceptions.hpp"
#include "rclcpp/executor.hpp"
#include "rclcpp/node.hpp"
#include "rclcpp/scope_exit.hpp"
#include "rclcpp/utilities.hpp"

// RCUtils headers
#include "rcutils/logging_macros.h"

// Debug enable
// #define DEBUG

// Using declarations
using rclcpp::exceptions::throw_from_rcl_error;
using rclcpp::AnyExecutable;
using rclcpp::Executor;
using rclcpp::ExecutorOptions;
using rclcpp::FutureReturnCode;

/*
 *******************************************************************************
 *                            Forward Declarations                             *
 *******************************************************************************
*/

static void take_and_do_error_handling (const char *action_description, 
	const char *topic_or_service_name, std::function<bool()> take_action,
	std::function<void()> handle_action);

/*
 *******************************************************************************
 *                           Constructor/Destructor                            *
 *******************************************************************************
*/


Executor::Executor (const rclcpp::ExecutorOptions &options)
: spinning(false), memory_strategy_(options.memory_strategy)
{
	rcl_ret_t ret;

	// Init guard condition options
	rcl_guard_condition_options_t guard_condition_options = 
		rcl_guard_condition_get_default_options();

	// Initialize guard condition
	if (RCL_RET_OK != (ret = rcl_guard_condition_init(&interrupt_guard_condition_,
		options.context->get_rcl_context().get(), guard_condition_options)))
	{
		throw_from_rcl_error(ret, "Failed to create interrupt guard condition!");
	} 

	// Note: Always at least two guard conditions. One for CTRL-C, one for
	//       executor's guard condition (interrupt_guard_condition_)

	// Put: Global CTRL-C guard condition in
	memory_strategy_->add_guard_condition(
		options.context->get_interrupt_guard_condition(&wait_set_));

	// Put: Executor guard condition in
	memory_strategy_->add_guard_condition(&interrupt_guard_condition_);

	// Fetch allocator
	rcl_allocator_t allocator = memory_strategy_->get_allocator();

	// Store: Context for later use
	context_ = options.context;

	// 
	if (RCL_RET_OK != (ret = rcl_wait_set_init(&wait_set_, 0, 2, 0, 0, 0, 0,
		context_->get_rcl_context().get(), allocator)))
	{
		RCUTILS_LOG_ERROR_NAMED("rclcpp", "Failed to create wait set: %s", 
			rcl_get_error_string().str);

		// Clear the error
		rcl_reset_error();

		// Attempt clean up
		if (RCL_RET_OK != rcl_guard_condition_fini(&interrupt_guard_condition_)) {
			RCUTILS_LOG_ERROR_NAMED("rclcpp", "Failed to destroy guard condition: %s",
				rcl_get_error_string().str);

			// Clear the error
			rcl_reset_error();
		}

		throw std::runtime_error("Failed to create executor wait set!");
	}
}

Executor::~Executor ()
{

	// Disassociate all nodes
	for (auto &weak_node : weak_nodes_) {
		auto node = weak_node.lock();

		// If non-null: Mark as not associated with this executor
		if (nullptr != node) {
			std::atomic_bool &has_executor = node->get_associated_with_executor_atomic();
			has_executor.store(false);
		}
	}

	// lear all weak references (the actual objects may exist elsewhere still)
	weak_nodes_.clear();

	// Disassociate all guard conditions
	for (auto &guard_condition : guard_conditions_) {
		memory_strategy_->remove_guard_condition(guard_condition);
	}

	// Clear all guard condition references
	guard_conditions_.clear();

	// Finalize the wait set
	if (RCL_RET_OK != rcl_wait_set_fini(&wait_set_)) {
		RCUTILS_LOG_ERROR_NAMED("rclcpp", "Failed to destroy wait set: %s",
			rcl_get_error_string().str);
		rcl_reset_error();
	}

	// Finalize interrupt guard condition
	if (RCL_RET_OK != rcl_guard_condition_fini(&interrupt_guard_condition_)) {
		RCUTILS_LOG_ERROR_NAMED("rclcpp", "Failed to destroy guard condition: %s",
			rcl_get_error_string().str);
		rcl_reset_error();
	}

	// Remove and release SIGINT guard condition
	memory_strategy_->remove_guard_condition(
		context_->get_interrupt_guard_condition(&wait_set_));
	context_->release_interrupt_guard_condition(&wait_set_, std::nothrow);
}


/*
 *******************************************************************************
 *                           Node Add/Remove Methods                           *
 *******************************************************************************
*/


void Executor::add_node (rclcpp::node_interfaces::NodeBaseInterface::SharedPtr node_ptr, 
	                     bool notify)
{

	// Check: Is the node already assigned to an executor? 
	std::atomic_bool &has_executor = node_ptr->get_associated_with_executor_atomic();
	if (true == has_executor.exchange(true)) {
		throw std::runtime_error("Node already associated with another executor!");
	}

	// Check: Node already added
	for (auto &weak_node : weak_nodes_) {
		auto node = weak_node.lock();
		if (node == node_ptr) {
			throw std::runtime_error("Node already associated with this executor!");
		}
	}

	// Register the node
	weak_nodes_.push_back(node_ptr);
	guard_conditions_.push_back(node_ptr->get_notify_guard_condition());

	// If notify, then waiting interrupt
	if (notify) {
		if (RCL_RET_OK != rcl_trigger_guard_condition(&interrupt_guard_condition_)) {
			throw std::runtime_error(rcl_get_error_string().str);
		}
	}

	// Add node notify condition to guard condition handles
	std::unique_lock<std::mutex> lock(memory_strategy_mutex_);
	memory_strategy_->add_guard_condition(node_ptr->get_notify_guard_condition());
}

void Executor::add_node (std::shared_ptr<rclcpp::Node> node_ptr, bool notify)
{
	this->add_node(node_ptr->get_node_base_interface(), notify);
}

void Executor::remove_node (rclcpp::node_interfaces::NodeBaseInterface::SharedPtr node_ptr, 
	                        bool notify)
{
	bool removed = false;

	// Remove matching node
	// TODO: Why doesn't it break if the node is found? Surely there is only one entry?
	// FIX: I am adding break
	{
		auto iterator_nodes  = weak_nodes_.begin();
		auto iterator_guards = guard_conditions_.begin();

		while (iterator_nodes != weak_nodes_.end()) {
			bool matched = (iterator_nodes->lock() == node_ptr);
			if (matched) {
				iterator_nodes = weak_nodes_.erase(iterator_nodes);
				iterator_guards = guard_conditions_.erase(iterator_guards);
				removed = true;
				break;
			} else {
				++iterator_nodes;
				++iterator_guards;
			}
		}
	}

	// Get boolean indicating if node is owned by executor and unset it
	std::atomic_bool &has_executor = node_ptr->get_associated_with_executor_atomic();
	has_executor.store(false);

	//  If notify is enabled, and then node was matched and removed ...
	if (notify == true && removed == true && 
		(RCL_RET_OK != rcl_trigger_guard_condition(&interrupt_guard_condition_))) {
		throw std::runtime_error(rcl_get_error_string().str);
	}

	// Lock the memory strategy mutex and remove the notify condition for node
	std::unique_lock<std::mutex> lock(memory_strategy_mutex_);
	memory_strategy_->remove_guard_condition(node_ptr->get_notify_guard_condition());
}

void Executor::remove_node (std::shared_ptr<rclcpp::Node> node_ptr, bool notify)
{
  this->remove_node(node_ptr->get_node_base_interface(), notify);
}

/*
 *******************************************************************************
 *                              Spinning Methods                               *
 *******************************************************************************
*/

void Executor::spin_node_some (rclcpp::node_interfaces::NodeBaseInterface::SharedPtr node)
{
  this->add_node(node, false);
  spin_some();
  this->remove_node(node, false);
}

void Executor::spin_node_some (std::shared_ptr<rclcpp::Node> node)
{
  this->spin_node_some(node->get_node_base_interface());
}

void Executor::spin_some (std::chrono::nanoseconds max_duration)
{
	// Fetch and store current timestamp
	auto start = std::chrono::steady_clock::now();

	// Create callback
	auto callback_should_still_spin = [max_duration, start]()
	{
		// Case: Spin forever
		if (std::chrono::nanoseconds(0) == max_duration) {
			return true;
		}

		// Case: Spin definitely and not expired
		if ((std::chrono::steady_clock::now() - start) < max_duration) {
			return true;
		}

		// Case: Spin definitely and expired
		return false;
	};

	// Check: Already spinning
	if (true == spinning.exchange(true)) {
		throw std::runtime_error("spin_some() called while already spinning!");
	}

	RCLCPP_SCOPE_EXIT(this->spinning.store(false););

	// Non-blocking call to pre-load available work 
	wait_for_work(std::chrono::milliseconds::zero());

	// While time remains and spinning is enabled, execute callbacks
	// TODO: Race condition notice -> Cannot stop executing a callback even if
	// if tell it to cancel!
	while (rclcpp::ok(context_) && (true == spinning.load()) && 
		true == callback_should_still_spin())
	{
		AnyExecutable any_exec;
		if (get_next_ready_executable(any_exec)) {
			execute_any_executable(any_exec);
		} else {
			break;
		}
	}
}

void Executor::spin_once_impl (std::chrono::nanoseconds timeout)
{
	AnyExecutable any_exec;
	if (false != get_next_executable(any_exec, timeout)) {
		execute_any_executable(any_exec);
	}
}

void Executor::spin_node_once_nanoseconds (rclcpp::node_interfaces::NodeBaseInterface::SharedPtr node, 
	std::chrono::nanoseconds timeout)
{
	this->add_node(node, false);
	spin_once(timeout);
	this->remove_node(node, false);
}

void Executor::spin_once (std::chrono::nanoseconds timeout)
{
	if (true == spinning.exchange(true)) {
		throw std::runtime_error("spin_once() called while already spinning!");
	}
	RCLCPP_SCOPE_EXIT(this->spinning.store(false););

	// I moved spin_once_impl below here
	AnyExecutable any_exec;
	if (get_next_executable(any_exec, timeout) != false) {
		execute_any_executable(any_exec);
	}
}

void Executor::cancel ()
{
	// Set the executor to not spin
	spinning.store(false);
	if (RCL_RET_OK != rcl_trigger_guard_condition(&interrupt_guard_condition_)) {
		throw std::runtime_error(rcl_get_error_string().str);
	}
}

void Executor::execute_any_executable (AnyExecutable &any_exec)
{
	size_t n_fields_set = 0;

	// Don't do anything if not spinning
	if (false == spinning.load()) {
		return;
	}

	// Ensure that only one field is set

	// If timer field set -> execute it
	if (any_exec.timer) {
		n_fields_set++;
		execute_timer(any_exec.timer);
	}

	// If subscription field set -> execute it
	if (any_exec.subscription) {
		n_fields_set++;
		execute_subscription(any_exec.subscription);
	}

	// If service field set -> execute it
	if (any_exec.service) {
		n_fields_set++;
		execute_service(any_exec.service);
	}

	// If client field set -> execute it
	if (any_exec.client) {
		n_fields_set++;
		execute_client(any_exec.client);
	}

	// If waitable field set -> execute it
	if (any_exec.waitable) {
		n_fields_set++;
		any_exec.waitable->execute();
	}

	// Reset the callback group (doesn't matter type executed - see subscription mut-ex)
	any_exec.callback_group->can_be_taken_from().store(true);

	// Awaken wait: Might need to be recalculated or work that was blocked is now ready
	if (RCL_RET_OK != rcl_trigger_guard_condition(&interrupt_guard_condition_)) {
		throw std::runtime_error(rcl_get_error_string().str);
	}

	// Check: Not more than one any_exec field was set
	if (n_fields_set > 1) {
		throw std::runtime_error("More than one executable type set in executable entity!");
	}
}



void Executor::execute_subscription (rclcpp::SubscriptionBase::SharedPtr subscription)
{
	rclcpp::MessageInfo message_info;

	// Initialize use of shared thread message data to false
	message_info.get_rmw_message_info().from_intra_process = false;

	// If the message is serialized - it comes from RMW via IPC
	if (subscription->is_serialized()) {
		return execute_serialized_subscription(subscription);
	}

	// If the message is loaned: It can be taken from RMW via IPC and given, then returned
	if (subscription->can_loan_messages()) {
		return execute_loaned_subscription(subscription);
	}

	// Otherwise the message is copied from RMW via IPC
	return execute_copied_subscription(subscription);
}

void Executor::execute_serialized_subscription (rclcpp::SubscriptionBase::SharedPtr s)
{
	rclcpp::MessageInfo message_info;
	message_info.get_rmw_message_info().from_intra_process = false;

	std::shared_ptr<SerializedMessage> serialized_message = s->create_serialized_message();
	take_and_do_error_handling("Taking serialized message", s->get_topic_name(),
	[&](){ return s->take_serialized(*serialized_message.get(), message_info); },
	[&]()
	{
		auto void_serialized_message = std::static_pointer_cast<void>(serialized_message);
		s->handle_message(void_serialized_message, message_info);
	});

	s->return_serialized_message(serialized_message);
}

void Executor::execute_loaned_subscription (rclcpp::SubscriptionBase::SharedPtr s)
{
	rclcpp::MessageInfo message_info;
	message_info.get_rmw_message_info().from_intra_process = false;
	void *loaned_message = nullptr;

	take_and_do_error_handling("Taking loaned message", s->get_topic_name(),
	[&](){
		rcl_ret_t ret = rcl_take_loaned_message(s->get_subscription_handle().get(),
			&loaned_message, &message_info.get_rmw_message_info(), nullptr);
		if (RCL_RET_SUBSCRIPTION_TAKE_FAILED == ret) {
			return false;
		}
		if (RCL_RET_OK != ret) {
			rclcpp::exceptions::throw_from_rcl_error(ret);
		}
		return true;
	},
	[&](){
		s->handle_loaned_message(loaned_message, message_info);
	});

	// Return loaned message from subscription
	rcl_ret_t ret = rcl_return_loaned_message_from_subscription(
		s->get_subscription_handle().get(), loaned_message);

	// If not OK, throw error
	if (RCL_RET_OK != ret) {
		RCLCPP_ERROR(rclcpp::get_logger("rclcpp"), 
		"rcl_return_loaned_message_from_subscription() failed on topic %s: %s",
		s->get_topic_name(), rcl_get_error_string().str);
	}

	// NULL the pointer
	loaned_message = nullptr;
}

void Executor::execute_copied_subscription (rclcpp::SubscriptionBase::SharedPtr s)
{
	rclcpp::MessageInfo message_info;
	message_info.get_rmw_message_info().from_intra_process = false;

	std::shared_ptr<void> message = s->create_message();

	take_and_do_error_handling("Taking copied message", s->get_topic_name(),
	[&](){ return s->take_type_erased(message.get(), message_info); },
	[&](){ s->handle_message(message, message_info); });

	s->return_message(message);
}

void Executor::execute_timer (rclcpp::TimerBase::SharedPtr timer)
{
	timer->execute_callback();
}

void Executor::execute_service (rclcpp::ServiceBase::SharedPtr service)
{
	auto request_header = service->create_request_header();
	std::shared_ptr<void> request = service->create_request();
	take_and_do_error_handling("Taking a service request", service->get_service_name(),
		[&](){ return service->take_type_erased_request(request.get(), *request_header); },
		[&](){ service->handle_request(request_header, request); });
}

void Executor::execute_client (rclcpp::ClientBase::SharedPtr client)
{
	auto request_header = client->create_request_header();
	std::shared_ptr<void> response = client->create_response();
	take_and_do_error_handling("Taking a service client response", client->get_service_name(),
		[&](){ return client->take_type_erased_response(response.get(), *request_header); },
		[&](){ client->handle_response(request_header, response); });
}

void Executor::wait_for_work (std::chrono::nanoseconds timeout)
{
	rcl_ret_t status;

	// Acquire lock (released outside of nested scope when object destroyed)
	{
		std::unique_lock<std::mutex> lock(memory_strategy_mutex_);

		// Collect subscriptions and waited-upon timers
		memory_strategy_->clear_handles();

		// Are there invalid weak nodes?
		bool invalid_weak_nodes = memory_strategy_->collect_entities(weak_nodes_);

		// If true -> clear them
		if (invalid_weak_nodes) {
			auto node_iterator            = weak_nodes_.begin();
			auto guard_condition_iterator = guard_conditions_.begin();

			while (node_iterator != weak_nodes_.end()) {
				if (node_iterator->expired()) {
					node_iterator = weak_nodes_.erase(node_iterator);
					memory_strategy_->remove_guard_condition(*guard_condition_iterator);
					guard_condition_iterator = guard_conditions_.erase(guard_condition_iterator);
				} else {
					++node_iterator; ++guard_condition_iterator;
				}
			}
		}

		// Clear the wait sets (sets all entries in underlyin RMW to NULL, and count to zero)
		if (RCL_RET_OK != rcl_wait_set_clear(&wait_set_)) {
			throw std::runtime_error("Could not clear wait set!");
		}

		// Deallocate and then reallocate memory for all entity sets. All values also nulled
		if (RCL_RET_OK != rcl_wait_set_resize(
			&wait_set_,
			memory_strategy_->number_of_ready_subscriptions(),
			memory_strategy_->number_of_guard_conditions(),
			memory_strategy_->number_of_ready_timers(),
			memory_strategy_->number_of_ready_clients(),
			memory_strategy_->number_of_ready_services(),
			memory_strategy_->number_of_ready_events()))
		{
			throw std::runtime_error(std::string("Wait set resize failed: ") + 
				rcl_get_error_string().str);
		}

		// Add all subs, clients, services, timers, guards, and waitables back in rcl_wait_set
		if (false == memory_strategy_->add_handles_to_wait_set(&wait_set_)) {
			throw std::runtime_error("Couldn't fill wait set!");
		}
	}

	// Convert timeout to nanoseconds
	auto timeout_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout).count();

	// Block until wait set is ready, or until timeout exceeded (this func passes to RMW)
	// Negative timeout value blocks indefinitely
	switch ((status = rcl_wait(&wait_set_, timeout_ns))) {

		case RCL_RET_WAIT_SET_EMPTY: {
			RCUTILS_LOG_WARN_NAMED("rclcpp", "Empty wait set in rcl_wait(). Unexpected!");
		}
		break;

		case RCL_RET_INVALID_ARGUMENT:
		case RCL_RET_WAIT_SET_INVALID: 
		case RCL_RET_ERROR: {
			rclcpp::exceptions::throw_from_rcl_error(status, "rcl_wait() failed!");
		}
		break;

		case RCL_RET_TIMEOUT: {
#ifdef DEBUG
			RCUTILS_LOG_WARN_NAMED("rclcpp", "Timeout expired prematurely!");
#endif
		}
		break;

		case RCL_RET_OK:
		default:
		break;
	}

	// Remove NULL handles (not ready )
	memory_strategy_->remove_null_handles(&wait_set_);
}

/*
 *******************************************************************************
 *                             Executable Fetching                             *
 *******************************************************************************
*/

bool Executor::get_next_ready_executable (AnyExecutable &any_executable)
{
	// TODO: Select next ready executable via a priority!

	// 1. Check if any timers are ready
	memory_strategy_->get_next_timer(any_executable, weak_nodes_);
	if (any_executable.timer) {
		return can_run_ready_executable(true, any_executable);
	}

	// 2. Check if any subscriptions are ready
	memory_strategy_->get_next_subscription(any_executable, weak_nodes_);
	if (any_executable.subscription) {
		return can_run_ready_executable(true, any_executable);
	}

	// 3. Check if any services are ready
	memory_strategy_->get_next_service(any_executable, weak_nodes_);
	if (any_executable.service) {
		return can_run_ready_executable(true, any_executable);
	}

	// 4. Check if any clients are ready
	memory_strategy_->get_next_client(any_executable, weak_nodes_);
	if (any_executable.client) {
		return can_run_ready_executable(true, any_executable);
	}

	// 5. Check for waitables
	memory_strategy_->get_next_waitable(any_executable, weak_nodes_);
	if (any_executable.waitable) {
		return can_run_ready_executable(true, any_executable);
	}

	return can_run_ready_executable(false, any_executable);
}

std::vector<AnyExecutable> *Executor::get_all_ready_executables ()
{
	std::vector<AnyExecutable> *ready_vector_p = new std::vector<AnyExecutable>();

	// Insert all ready timers
	memory_strategy_->get_all_ready_timers(ready_vector_p, weak_nodes_);

	// Insert all ready subscriptions
	memory_strategy_->get_all_ready_subscriptions(ready_vector_p, weak_nodes_);

	return ready_vector_p;
}

bool Executor::get_highest_priority_ready_executable (AnyExecutable &any_executable)
{
	memory_strategy_->get_highest_priority_timer_or_subscription(any_executable, weak_nodes_);
	bool ready = ((nullptr != any_executable.subscription) || (nullptr != any_executable.timer));
	return can_run_ready_executable(ready, any_executable);
}

bool Executor::can_run_ready_executable (bool ready, AnyExecutable &any_executable)
{
	// If nothing was ready - return immediately
	if (!ready) {
		return false;
	}

	// Otherwise: Check if group is mutually exclusive
	using callback_group::CallbackGroupType;
	if (any_executable.callback_group && 
		any_executable.callback_group->type() == CallbackGroupType::MutuallyExclusive)
	{
		// Ensure it is not taken
		assert(any_executable.callback_group->can_be_taken_from().load() == true);

		// Mark false as it will now be run. This is reset after exec or destruction
		any_executable.callback_group->can_be_taken_from().store(false);
	}

	return true;
}

bool Executor::get_next_executable (AnyExecutable &any_executable, 
	                                std::chrono::nanoseconds timeout)
{
	bool success = false;

	// TODO: Discontinue use of this method, and write own spin method
	//       that just gets the next ready executable and waits indef

	// Check for ready timers or subscriptions
	if ((success = get_next_ready_executable(any_executable)) == false) {

		// Wait until work arrives
		wait_for_work(timeout);

		// If not spinning (I guess), then return false
		if (!spinning.load()) {
			return false;
		}

		// Try again
		success = get_next_ready_executable(any_executable);
	}

	return success;
}

/*
 *******************************************************************************
 *                    Support Methods (Allocator Strategy)                     *
 *******************************************************************************
*/

rclcpp::node_interfaces::NodeBaseInterface::SharedPtr Executor::get_node_by_group (rclcpp::CallbackGroup::SharedPtr group)
{

	// Ignore nullptr input
	if (group == nullptr) {
		return nullptr;
	}

	// For all nodes
	for (auto &weak_node : weak_nodes_) {
		auto node = weak_node.lock();

		// Continue until valid node found
		if (node == nullptr) {
			continue;
		}

		// For all callback groups posessed by said node
		for (auto &weak_group : node->get_callback_groups()) {
			auto callback_group = weak_group.lock();

			// If matches the group being sought - return
			if (callback_group == group) {
				return node;
			}
		}
	}

	return nullptr;
}

rclcpp::CallbackGroup::SharedPtr Executor::get_group_by_timer (rclcpp::TimerBase::SharedPtr timer)
{

	// Ignore null timer ptr input
	if (timer == nullptr) {
		return rclcpp::CallbackGroup::SharedPtr();
	}

	// For all nodes
	for (auto &weak_node : weak_nodes_) {
		auto node = weak_node.lock();

		// Continue until valid node found
		if (node == nullptr) {
			continue;
		}

		// For all callback groups possessed by said node
		for (auto &weak_group : node->get_callback_groups()) {
			auto group = weak_group.lock();

			// Continue until a valid group is found
			if (group == nullptr) {
				continue;
			}

			// Search for a timer within this group that matches the given pointer
			auto timer_ref = group->find_timer_ptrs_if(
				[timer](const rclcpp::TimerBase::SharedPtr &timer_ptr) -> bool {
					return timer_ptr == timer;
			});

			// If something was found, return this callback group
			if (timer_ref != nullptr) {
				return group;
			}
		}
	}
	return rclcpp::CallbackGroup::SharedPtr();
}

static void take_and_do_error_handling (const char *action_description, 
	const char *topic_or_service_name, std::function<bool()> take_action,
	std::function<void()> handle_action)
{
	bool taken = false;
	try {
		taken = take_action();
	} catch (const rclcpp::exceptions::RCLError &rcl_error) {
		RCLCPP_ERROR(rclcpp::get_logger("rclcpp"),
			"executor %s '%s' failed unexpectedly with %s!",
			action_description,
			topic_or_service_name,
			rcl_error.what());
	}

	if (taken) {
		handle_action();
	} else {
		// Message/Service not taken (for some reason)
		// * Can be normal, middleware might need to interrupt
		// * and that is allowed
		// * No difference between wake up and entity having data
		// * until trying to take data
		RCLCPP_DEBUG(rclcpp::get_logger("rclcpp"),
			"executor %s '%s' failed to take anything!",
			action_description,
			topic_or_service_name);
	}
}

void Executor::set_memory_strategy(rclcpp::memory_strategy::MemoryStrategy::SharedPtr memory_strategy)
{
	if (nullptr == memory_strategy) {
		throw std::runtime_error("set_memory_strategy(nullptr) forbidden!");
	}
	memory_strategy_ = memory_strategy;
}