
// File header
#include "rclcpp/executors/preemptive_priority_executor.hpp"

// Standard headers
#include <chrono>
#include <functional>
#include <vector>
#include <memory>

// RCLCPP headers
#include "rclcpp/utilities.hpp"
#include "rclcpp/scope_exit.hpp"

// Directives
using rclcpp::executors::PreemptivePriorityExecutor;
using rclcpp::executors::thread_priority_range_t;
using rclcpp::executors::Callback;
using Callback_Ptr = std::shared_ptr<Callback>;

/*
 *******************************************************************************
 *                             Symbolic Constants                              *
 *******************************************************************************
*/


#define CORE_EXEC        0        // Executor (scheduler) core
#define CORE_WORK        1        // Job-process core

//#define DEBUG                     // Performs cout logging if enabled

/*
 *******************************************************************************
 *                           Constructor/Destructor                            *
 *******************************************************************************
*/


PreemptivePriorityExecutor::PreemptivePriorityExecutor (
	const rclcpp::ExecutorOptions &options,
	thread_priority_range_t priority_range)
:
	rclcpp::Executor(options),
	d_priority_range(priority_range)
{
	// Check: Valid priority range
	if ((priority_range.u_bound - priority_range.l_bound) < 1) {
		throw std::invalid_argument(std::string("Bad priority range: ") +
			std::string("(u_bound - l_bound) must be >= 1"));
	}

	// Check: Range makes sense for linux
	if (priority_range.u_bound > 99 || priority_range.l_bound < 0) {
		throw std::invalid_argument(std::string("Bad priority range: ") +
			std::string("u_bound must be <= 99 and l_bound >= 0"));
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


void PreemptivePriorityExecutor::spin_some (std::chrono::nanoseconds max_duration)
{
	// Variables for core affinity and process priority
	sched_param sch, sch_old;
	int policy, policy_old;
	cpu_set_t cpu_set;

	// Variables for threading and job control
	std::vector<std::thread> threads;
	std::vector<Consumer<Callback_Ptr> *> consumers;

	// Print description
	std::cout << std::string("\033[1;33m") + this->description() + std::string("\033[0m\n");

	// Check: Not already spinning
	if (true == spinning.exchange(true)) {
		throw std::runtime_error("May not call spin() while already spinning!");
	}

	// Defer: On exit of this method scope, stop spinning
	RCLCPP_SCOPE_EXIT(this->spinning.store(false); );

	// Get the current policy and save it
	pthread_getschedparam(pthread_self(), &policy, &sch);
	policy_old = policy; 
	sch_old = sch;
	
	// Set core affinity and priority for current thread under FIFO policy
	pin_to_core(pthread_self(), CORE_EXEC, &cpu_set);
	set_thread_priority(pthread_self(), d_priority_range.u_bound, &policy, &sch);

	// Create the Callback_Ptr consumer objects 
	for (int i = d_priority_range.l_bound; i < d_priority_range.u_bound; ++i) {
		Consumer<Callback_Ptr> *c = new Consumer<Callback_Ptr>(max_duration);
		consumers.push_back(c);
#ifdef DEBUG
		std::cout << "[Executor]: Consumer created at priority: " << std::to_string(i)
			<< std::endl;
#endif
	}

	// Create the Callback_Ptr consumer threads (no wait lock needed due to job queue)
	for (size_t thread_id = 0; thread_id < consumers.size(); ++thread_id) {
		Consumer<Callback_Ptr> *c = consumers[thread_id];
		auto func = std::bind(&PreemptivePriorityExecutor::run, this, c, thread_id);
		threads.emplace_back(func);
	}

	// Set the Callback_Ptr consumer thread priorities
	for (size_t thread_id = 0; thread_id < threads.size(); ++thread_id) {
		int priority = d_priority_range.l_bound + thread_id;
		pin_to_core(threads[thread_id].native_handle(), CORE_WORK, &cpu_set);
		set_thread_priority(threads[thread_id].native_handle(), priority, &policy, &sch);
	}

	// Run the multiplexer
#ifdef DEBUG
	std::cout << "[Executor]: Now multiplexing ..." << std::endl;
#endif
	this->multiplex(max_duration, &consumers);

	// Collect threads
	for (auto &thread : threads) {
		thread.join();
	}

	// Free consumers
	for (size_t i = 0; i < consumers.size(); ++i) {
		delete consumers[i];
	}

	// Restore old policy
	if (0 != pthread_setschedparam(pthread_self(), policy_old, &sch_old)) {
		throw std::runtime_error(std::string("thread_setschedparam: ") + 
			std::string(std::strerror(errno)));
	}
}

void PreemptivePriorityExecutor::run (Consumer<Callback_Ptr> *c, int thread_id)
{
	(void)(thread_id);
	// Extract useful fields
	auto duration = c->duration();
	auto timeout  = c->timeout();
	auto m_p      = c->mutex();
	auto cv_p     = c->condition_variable();

	// Mark the start
	auto start    = std::chrono::steady_clock::now();

	while (duration == std::chrono::nanoseconds(0) ||
		(std::chrono::steady_clock::now() - start) < duration)
	{
		std::unique_lock<std::mutex> temp_lock(*m_p);

		// Timeout: Ensures thread does not sleep forever
		cv_p->wait_for(temp_lock, timeout);

		// Extract next AnyExecutable on wake: If none, cycle back around
		Callback_Ptr callback;
		while (c->dequeue(&callback) == 1) {
#ifdef DEBUG
			std::string str_id = std::to_string(thread_id);
			std::cout << "[Work-" << str_id << "]: Executing " << callback->description() << std::endl;
#endif
			// Otherwise execute the AnyExecutable
			callback->execute();

			// Remove the timer/subscription from the handling set
			remove_expired_executable(callback);

			// Reset callback group
			callback->callback_group().reset();			
		}
	}
}

void PreemptivePriorityExecutor::multiplex (std::chrono::nanoseconds max_duration,
	std::vector<Consumer<Callback_Ptr> *> *consumers_p)
{
	std::vector<AnyExecutable> ready_executables;
	int max_allowed_priority = d_priority_range.u_bound - 
		d_priority_range.l_bound - 1;

	// Mark start
	auto start = std::chrono::steady_clock::now();

	// Create callback for checking time
	auto should_still_spin = [max_duration, start]() {
		if (std::chrono::nanoseconds(0) == max_duration) return true;
		return ((std::chrono::steady_clock::now() - start) < max_duration);
	};

	// Spin indefinitely, or for a duration if specified
	while (rclcpp::ok(this->context_) && spinning.load() && 
		true == should_still_spin())
	{
		// Wait for work up to a timeout
		wait_for_work(std::chrono::nanoseconds(100000000));

		// Check if timeout occurred or should stop spinning
		if (false == spinning.load() || !should_still_spin()) {
			break;
		}

		// Clear and fetch new ready executables
		ready_executables.clear();
		memory_strategy_->get_all_ready_timers(&ready_executables, weak_nodes_);
		memory_strategy_->get_all_ready_subscriptions(&ready_executables, weak_nodes_);

		// Sort all ready executables into the correct consumer queues
		for (auto e : ready_executables) {
			Callback_Ptr item = executable_to_callback(e);

			// If nullptr, then it was already handled
			if (item == nullptr) {
				continue;
			}

			// Check the priority value
			if (e.callback_priority < 0 || e.callback_priority > 
				max_allowed_priority) {
				throw std::runtime_error("Executable had invalid priority: " +
					std::to_string(e.callback_priority) + ", only allowed [0," + 
					std::to_string(max_allowed_priority) + ")");
			}
			
			// Extract the consumer for the given callback priority
			Consumer<Callback_Ptr> *consumer = consumers_p->at(e.callback_priority);
			auto m_p  = consumer->mutex();
			auto cv_p = consumer->condition_variable();
			{
				std::lock_guard<std::mutex> temp_lock(*m_p);
				consumer->enqueue(item);
#ifdef DEBUG
				std::cout << "[Executor]: enqueued " << item->description() << std::endl; 
#endif
			}

			// Notify the work thread (lost if not sleeping)
			cv_p->notify_one();
		}
	}
}


Callback_Ptr PreemptivePriorityExecutor::executable_to_callback (AnyExecutable e)
{
	Callback_Ptr callback = nullptr;

	// Only make a callback if the timer isn't already in the handled set
	if (nullptr != e.timer) {
		std::lock_guard<std::mutex> temp_lock(d_timer_set_mutex);
		if (0 < d_handled_timers.count(e.timer)) {
			if (nullptr != e.callback_group) {
				e.callback_group->can_be_taken_from().store(true);
			}
		} else {
			callback = std::make_shared<Callback>(e.timer, e.callback_group, e.node_base);
			d_handled_timers.insert(e.timer);
		}

	// Only make a callback if the subscription isn't already in the handled set
	} else if (nullptr != e.subscription) {
		std::lock_guard<std::mutex> temp_lock(d_subscription_set_mutex);
		if (0 == d_handled_subscriptions.count(e.subscription)) {
			callback = std::make_shared<Callback>(e.subscription, e.callback_group, e.node_base);
			d_handled_subscriptions.insert(e.subscription);
		}
	} else {
		throw std::runtime_error(std::string("Unsupported executable type: ") +
			std::string("Only timers and subscription callbacks accepted"));
	}

	return callback;
}


void PreemptivePriorityExecutor::remove_expired_executable (Callback_Ptr callback)
{
	switch (callback->callback_type()) {
		case CALLBACK_TIMER: {
			std::lock_guard<std::mutex> temp_lock(d_timer_set_mutex);
			auto t = d_handled_timers.find(callback->timer());
			if (t != d_handled_timers.end()) {
				d_handled_timers.erase(t);
			}
		}
		break;

		case CALLBACK_SUBSCRIPTION: {
			std::lock_guard<std::mutex> temp_lock(d_subscription_set_mutex);
			auto s = d_handled_subscriptions.find(callback->subscription());
			if (s != d_handled_subscriptions.end()) {
				d_handled_subscriptions.erase(s);
			}
		}
		break;

		default: {
			throw std::runtime_error(
				std::string("Cannot remove unknown callback type!"));
		}
	}
}


std::string PreemptivePriorityExecutor::description ()
{
	std::string s("Preemptive-Priority-Executor {.prio_range = [");
	s += std::to_string(d_priority_range.l_bound);
	s += std::string(",");
	s += std::to_string(d_priority_range.u_bound);
	s += std::string(")}");
	return s;
}