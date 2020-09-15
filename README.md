# RCLCPP - Preemptive-Priority Extension

This repository is a fork of the ROS Client for C++ package. It contains the preemptive-priority-executor (PPE) extension. This extension: 

1. Extends `TimerBase` and related classes to allow priority values to be assigned to instances of these classes.
2. Extends `SubscriptionBase` and related classes to allow priority values to be assigned to instances of these classes.
3. Adds a new executor sub-class, called `PreemptivePriorityExecutor`

---

## Installation 

### Source code

To use the PPE, you need to be building ROS2 from source. Simply add this repository as a remote in `ros2_<version>/src/ros2/rclcpp`, and then pull/fetch from the new remote's master branch!

### Building

Build this as you would ROS2. If you want to rebuild ROS2 entirely, then run `colcon build --symlink-install` from your top-level `ros2_<version>` directory. 

It is faster to simply specify the `rclcpp` package. Do so as follows: `colcon build --packages-select rclcpp --symlink-install`

---

## Usage

Use `#include "rclcpp/rclcpp.hpp"` to access common elements of the ROS2 system. You can also visit the [rclcpp API documentation](http://docs.ros2.org/latest/api/rclcpp/). 

No special headers are needed to use the extension! Simple declare a preemptive priority executor and use it like any other!

```
// Init the executor
rclcpp::executors::PreemptivePriorityExecutor my_exec;

// Add node
my_exec.add_node(node);

// Spin
my_exec.spin();
```

**Note**: You *need* to be root to use the PPE. It's best to start a root shell before running your ROS2 application (e.g. `sudo bash`). You will need to source the ROS2 scripts again though (`. install/setup.sh`)

**Note**: PPE needs to be run on a system with at least *two* cores. This is because the executor scheduling thread is too busy to allow executable entities to run between blocking periods.

### Testing System Configuration

 * **OS**: Ubuntu 18.04.5 LTS
 * **CPU**: Intel(R) Core(TM) i5-8600K (6 physical cores)
 * **Memory**: 8GB 

---
## Examples

The ROS 2 tutorials [Writing a simple publisher and subscriber](https://index.ros.org/doc/ros2/Tutorials/Writing-A-Simple-Cpp-Publisher-And-Subscriber/)
and [Writing a simple service and client](https://index.ros.org/doc/ros2/Tutorials/Writing-A-Simple-Cpp-Service-And-Client/)
contain some examples of rclcpp APIs in use.

For practical examples using the PPE, see [the examples repository](https://github.com/Micrified/RCLCPP-PPE-Examples).

