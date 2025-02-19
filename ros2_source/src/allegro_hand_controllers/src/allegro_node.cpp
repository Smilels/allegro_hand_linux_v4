// Common allegro node code used by any node. Each node that implements an
// AllegroNode must define the computeDesiredTorque() method.

#include "allegro_node.h"
#include "allegro_hand_driver/AllegroHandDrv.h"

std::string jointNames[DOF_JOINTS] =
        {
                "joint_0.0", "joint_1.0", "joint_2.0", "joint_3.0",
                "joint_4.0", "joint_5.0", "joint_6.0", "joint_7.0",
                "joint_8.0", "joint_9.0", "joint_10.0", "joint_11.0",
                "joint_12.0", "joint_13.0", "joint_14.0", "joint_15.0"
        };

AllegroNode::AllegroNode(const std::string nodeName, bool sim /* = false */)
    : Node(nodeName)
{
  mutex = new boost::mutex();

  // Create arrays 16 long for each of the four joint state components
  current_joint_state.position.resize(DOF_JOINTS);
  current_joint_state.velocity.resize(DOF_JOINTS);
  current_joint_state.effort.resize(DOF_JOINTS);
  current_joint_state.name.resize(DOF_JOINTS);

  // Initialize values: joint names should match URDF, desired torque and
  // velocity are both zero.
  for (int i = 0; i < DOF_JOINTS; i++) {
    current_joint_state.name[i] = jointNames[i];
    desired_torque[i] = 0.0;
    current_velocity[i] = 0.0;
    current_position_filtered[i] = 0.0;
    current_velocity_filtered[i] = 0.0;
  }


  // Get Allegro Hand information from parameter server
  // This information is found in the Hand-specific "zero.yaml" file from the allegro_hand_description package
  std::string robot_name, manufacturer, origin, serial, can_id, version;
  declare_parameter("~hand_info/robot_name", "");
  get_parameter("~hand_info/robot_name", robot_name);

  declare_parameter("~hand_info/which_hand", "");
  get_parameter("~hand_info/which_hand", whichHand);
  declare_parameter("~hand_info/manufacturer", "");
  get_parameter("~hand_info/manufacturer", manufacturer);
  declare_parameter("~hand_info/origin", "");
  get_parameter("~hand_info/origin", origin);
  declare_parameter("~hand_info/serial", "");
  get_parameter("~hand_info/serial", serial);
  declare_parameter("~hand_info/version", "");
  get_parameter("~hand_info/version", version);

  declare_parameter("~comm/CAN_CH", "can0");
  get_parameter("~comm/CAN_CH", can_id);
  // Initialize CAN device
  canDevice = 0;
  if(!sim) {
    canDevice = new allegro::AllegroHandDrv();
    if (canDevice->init(can_id)) {
        usleep(3000);
    }
    else {
        delete canDevice;
        canDevice = 0;
    }
  }

  // Start ROS time
  tstart = get_clock()->now();

  // Advertise current joint state publisher and subscribe to desired joint
  // states.
  joint_state_pub = this->create_publisher<sensor_msgs::msg::JointState>(JOINT_STATE_TOPIC, 3);
  joint_cmd_sub = this->create_subscription<sensor_msgs::msg::JointState>(DESIRED_STATE_TOPIC, 1, // queue size
                                                                          std::bind(&AllegroNode::desiredStateCallback, this, std::placeholders::_1));
}

AllegroNode::~AllegroNode() {
  if (canDevice) delete canDevice;
  delete mutex;
  rclcpp::shutdown();
}

void AllegroNode::desiredStateCallback(const sensor_msgs::msg::JointState &msg)
{
  mutex->lock();
  desired_joint_state = msg;
  mutex->unlock();
}

void AllegroNode::publishData() {
  // current position, velocity and effort (torque) published
  current_joint_state.header.stamp = tnow;
  for (int i = 0; i < DOF_JOINTS; i++) {
    current_joint_state.position[i] = current_position_filtered[i];
    current_joint_state.velocity[i] = current_velocity_filtered[i];
    current_joint_state.effort[i] = desired_torque[i];
  }
  joint_state_pub->publish(current_joint_state);
}

void AllegroNode::updateController() {

  // Calculate loop time;
  tnow = get_clock()->now();
  dt = 1e-9 * (tnow - tstart).nanoseconds();
  // dt = 1e-9 * (tnow - tstart).nsec;

  // When running gazebo, sometimes the loop gets called *too* often and dt will
  // be zero. Ensure nothing bad (like divide-by-zero) happens because of this.
  if(dt <= 0) {
    RCLCPP_DEBUG_STREAM_THROTTLE(rclcpp::get_logger("allegro_node"), *get_clock(), 1000, "AllegroNode::updateController dt is zero.");
    return;
  }

  tstart = tnow;


  if (canDevice)
  {
    // try to update joint positions through CAN comm:
    lEmergencyStop = canDevice->readCANFrames();

    // check if all positions are updated:
    if (lEmergencyStop == 0 && canDevice->isJointInfoReady())
    {
      // back-up previous joint positions:
      for (int i = 0; i < DOF_JOINTS; i++) {
        previous_position[i] = current_position[i];
        previous_position_filtered[i] = current_position_filtered[i];
        previous_velocity[i] = current_velocity[i];
      }

      // update joint positions:
      canDevice->getJointInfo(current_position);

      // low-pass filtering:
      for (int i = 0; i < DOF_JOINTS; i++) {
        current_position_filtered[i] = (0.6 * current_position_filtered[i]) +
                                       (0.198 * previous_position[i]) +
                                       (0.198 * current_position[i]);
        current_velocity[i] =
                (current_position_filtered[i] - previous_position_filtered[i]) / dt;
        current_velocity_filtered[i] = (0.6 * current_velocity_filtered[i]) +
                                       (0.198 * previous_velocity[i]) +
                                       (0.198 * current_velocity[i]);
        current_velocity[i] = (current_position[i] - previous_position[i]) / dt;
      }

      // calculate control torque:
      computeDesiredTorque();

      // set & write torque to each joint:
      canDevice->setTorque(desired_torque);
      lEmergencyStop = canDevice->writeJointTorque();

      // reset joint position update flag:
      canDevice->resetJointInfoReady();

      // publish joint positions to ROS topic:
      publishData();

      frame++;
    }
  }

  if (lEmergencyStop < 0) {
    // Stop program when Allegro Hand is switched off
    RCLCPP_ERROR(rclcpp::get_logger("allegro_node"), "Allegro Hand Node is Shutting Down! (Emergency Stop)");
    rclcpp::shutdown();
  }
}

// Interrupt-based control is not recommended by SimLab. I have not tested it.
void AllegroNode::timerCallback() {
  updateController();
}
using namespace std::chrono_literals;

rclcpp::TimerBase::SharedPtr AllegroNode::startTimerCallback()
{
  auto timer = this->create_wall_timer(1ms, std::bind(&AllegroNode::timerCallback, this));
  return timer;
}
