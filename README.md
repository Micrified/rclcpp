# rclcpp - preemptive-priority-extension

This repository contains the source code for the ROS Client Library for C++ package. It also contains the preemptive priority extension which: 

1. Extends TimerBase, and affiliated timer source files to allow priority values to be assigned to timers
2. Refactors the Executor class, adjusting some of the original source code, and exposing more internal fields than the original does.
3. Introduces the preemptive priority executor, along with associated support classes

## Usage

`#include "rclcpp/rclcpp.hpp"` allows use of the most common elements of the ROS 2 system.

Visit the [rclcpp API documentation](http://docs.ros2.org/latest/api/rclcpp/) for a complete list of its main components.

Documentation for the extensions will be provided later in the wiki.

### Installation

If you want to use the preemptive priority extension, please add this repository as a remote in your ROS2's `rclcpp` directory within `src`. This means you must be building ROS from scratch. 

Then, switch to the master branch under the new remote and make sure to pull/fetch. 

You may build ROS2 again using: `colcon build --symlink-install` from the top-level ROS2 directory. 

It is faster to simply build RCLCPP again. To do this, just run: `colcon build --packages-select rclcpp --symlink-install` instead

### Examples

The ROS 2 tutorials [Writing a simple publisher and subscriber](https://index.ros.org/doc/ros2/Tutorials/Writing-A-Simple-Cpp-Publisher-And-Subscriber/)
and [Writing a simple service and client](https://index.ros.org/doc/ros2/Tutorials/Writing-A-Simple-Cpp-Service-And-Client/)
contain some examples of rclcpp APIs in use.

Use of the preemptive priority extension will be added later


You need to run the preemptive-priority-executor with **root** permissions because it is required to set thread priority with the kernel. I recommend simply running the entire process from a root shell. Begin a root shell with `sudo bash`. Make sure to source the ROS2 and workspace files again if you do this.
