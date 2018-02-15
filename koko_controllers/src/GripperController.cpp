#include <koko_controllers/GripperController.h>
#include <pluginlib/class_list_macros.h>
#include <angles/angles.h>
#include <kdl/frames.hpp>
#include <vector>
#include <iterator>
#include <unistd.h>
#include <algorithm>
#include <math.h>
#include <numeric>
namespace koko_controllers{

  GripperController::~GripperController() {
  }

  bool GripperController::init(hardware_interface::EffortJointInterface *robot, ros::NodeHandle &n)
  {

    ros::NodeHandle node;
    std::string robot_desc_string;
    if (!node.getParam("robot_dyn_description", robot_desc_string)) {
      ROS_ERROR("No robot_dyn_description given, %s", node.getNamespace().c_str());
    }

    KDL::Tree my_tree;

    if(!kdl_parser::treeFromString(robot_desc_string, my_tree)){
      ROS_ERROR("Failed to contruct kdl tree");
      return false;
    }

    std::string endlink;
    if (!n.getParam("endlink", endlink)) {
      ROS_ERROR("No endlink given node namespace %s", n.getNamespace().c_str());
    }


    KDL::Chain dummyChain;

    if (!my_tree.getChain("base_link", endlink, dummyChain)) {
      ROS_ERROR("Failed to construct kdl chain");
      return false;
    }
    int filter_length;
    if (!n.getParam("filter_length", filter_length)) {
      ROS_ERROR("No filter_length given node namespace %s", n.getNamespace().c_str());
    }

    int ns = dummyChain.getNrOfSegments();
    ROS_INFO("ns = %d", ns);
    for(int i = 0; i < ns; i++){
      KDL::Segment seg = dummyChain.segments[i];

      if (seg.getJoint().getType() != 8)
      {
        JointPD* jointPD = new JointPD();
        std::string jointName = seg.getJoint().getName();
        joint_names.push_back(jointName);
        chain.addSegment(seg);
        jointPD->joint = robot->getHandle(jointName);
        double p_gain;
        double d_gain;
        double max_torque;
        double min_torque;
        double min_angle;
        double max_angle;
        if (!n.getParam(jointName + "/p", p_gain)) {
          ROS_ERROR("No %s/p given (namespace: %s)", jointName.c_str(), n.getNamespace().c_str());
          return false;
        }
        if (!n.getParam(jointName + "/d", d_gain)) {
          ROS_ERROR("No %s/d given (namespace: %s)", jointName.c_str(), n.getNamespace().c_str());
          return false;
        }
        if (!n.getParam(jointName + "/max_torque", max_torque)) {
          ROS_ERROR("No %s/max_torque given (namespace: %s)", jointName.c_str(), n.getNamespace().c_str());
          return false;
        }
        if (!n.getParam(jointName + "/min_torque", min_torque)) {
          ROS_ERROR("No %s/min_torque given (namespace: %s)", jointName.c_str(), n.getNamespace().c_str());
          return false;
        }
        if (!n.getParam(jointName + "/min_angle", min_angle)) {
          ROS_ERROR("No %s/min_angle given (namespace: %s)", jointName.c_str(), n.getNamespace().c_str());
          return false;
        }
        if (!n.getParam(jointName + "/max_angle", max_angle)) {
          ROS_ERROR("No %s/max_angle given (namespace: %s)", jointName.c_str(), n.getNamespace().c_str());
          return false;
        }

        jointPD->max_torque = max_torque;
        jointPD->min_torque = min_torque;
        jointPD->max_angle = max_angle;
        jointPD->min_angle = min_angle;
        jointPD->p_gain = p_gain;
        jointPD->d_gain = d_gain;
        jointPD->joint_name = jointName;
        jointPD->err_dot_filter_length = filter_length;
        joint_vector.push_back(jointPD);

      }
    }

    std::string root_name;
    if (!n.getParam("root_name", root_name)) {
      ROS_ERROR("No root_name given (namespace: %s)", n.getNamespace().c_str());
      return false;
    }

    sub_command = n.subscribe("command", 1, &GripperController::setCommand, this);
    sub_joint = node.subscribe("/" + root_name  + "/joint_states", 1000, &GripperController::jointCallback, this);

    // ROS_INFO("nj %d", chain.getNrOfJoints());

    id_torques = KDL::JntArray(chain.getNrOfJoints());
    commandPub = node.advertise<std_msgs::Float64MultiArray>("/commandPub", 1);
    deltaPub = node.advertise<std_msgs::Float64MultiArray>("/deltaPub", 1);

    return true;
  }

  void GripperController::starting(const ros::Time& time)
  {
    for (int i = 0; i < joint_vector.size(); i++) {
      joint_vector[i]->cmd = joint_vector[i]->joint.getPosition();

      //ROS_INFO("command start %f", joint_vector[i].cmd);
    }
  }


  void GripperController::setCommand(const std_msgs::Float64MultiArrayConstPtr& pos_commands) {
    std::vector<double> commands = pos_commands->data;
    //ROS_INFO("desired positions before wrap: %f, %f, %f, %f", commands[0], commands[1], commands[2], commands[3]);


    for (int i = 0; i < commands.size(); i++)  {
      //account for wrap around
      commands[i] = fmod(commands[i] + 3.14159265359, 6.28318530718);
      if (commands[i] < 0) commands[i] += 6.28318530718;
      commands[i] = commands[i] - 3.1415926535;
      commands[i] = std::min(std::max(commands[i], joint_vector[i]->min_angle), joint_vector[i]->max_angle);

      joint_vector[i]->cmd = commands[i];
    }

    //ROS_INFO("desired positions: %f, %f, %f, %f", joint_vector[0].cmd, joint_vector[1].cmd, joint_vector[2].cmd, joint_vector[3].cmd);

  }

  void GripperController::update(const ros::Time& time, const ros::Duration& period)
  {
    ROS_ERROR("a_update");
    std_msgs::Float64MultiArray commandMsg;
    for (int i = 0; i < joint_vector.size(); i++) {
      commandMsg.data.push_back(joint_vector[i]->cmd);
    }
    ROS_ERROR("A_update");
    commandPub.publish(commandMsg);
    ROS_ERROR("B_update");

    std_msgs::Float64MultiArray deltaMsg;

    std::vector<double> commands(joint_vector.size());
    for (int i = 0; i < joint_vector.size(); i++) {
      double current_position = joint_vector[i]->joint.getPosition();
      double current_velocity = joint_vector[i]->joint.getVelocity();
      // ROS_ERROR("%f", current_velocity);
      double error = angles::shortest_angular_distance(current_position, joint_vector[i]->cmd); // TODO: don't use wraparound here
      deltaMsg.data.push_back(error);
      //ROS_INFO("Joint pos %f, joint cmd %f", current_position, joint_vector[i].cmd);
      //ROS_INFO("Joint %d error: %f", i, error);
      double commanded_effort = computeCommand(error, period, i, current_velocity);

      commands[i] = commanded_effort;
    }
    ROS_ERROR("C_update");
    deltaPub.publish(deltaMsg);
    ROS_ERROR("C1_update");


    //ROS_INFO("joint name order: %s, %s, %s, %s", joint_vector[0].joint_name.c_str(), joint_vector[1].joint_name.c_str(), joint_vector[2].joint_name.c_str(), joint_vector[3].joint_name.c_str());
    //ROS_INFO("current positions: %f, %f, %f, %f", joint_vector[0].joint.getPosition(), joint_vector[1].joint.getPosition(), joint_vector[2].joint.getPosition(), joint_vector[3].joint.getPosition());
    //ROS_INFO("inverse dynamics torques: %f %f %f", id_torques(0), id_torques(1), id_torques(2));



    ROS_ERROR("D_update");
    for (int i = 0; i < joint_vector.size(); i++) {
      // ROS_INFO("command %d: %f", i, commands[i]);
      joint_vector[i]->joint.setCommand(commands[i]);
    }
    ROS_ERROR("E_update");
  }



  double GripperController::computeCommand(double error, ros::Duration dt, int index, double vel)
  {
    if (dt == ros::Duration(0.0) || std::isnan(error) || std::isinf(error))
      return 0.0;

    // double error_dot = joint_vector[index].d_error;
    // if (dt.toSec() > 0.0)  {
    //   error_dot = (error - joint_vector[index].p_error_last) / dt.toSec();
    //   joint_vector[index].p_error_last = error;
    // }
    // if (std::isnan(error_dot) || std::isinf(error_dot))
    //   return 0.0;
    double p_term, d_term;
    // joint_vector[index].d_error = error_dot;
    // joint_vector[index].err_dot_history.push_back(error_dot);
    // if (joint_vector[index].err_dot_history.size() > joint_vector[index].err_dot_filter_length) {
    //   joint_vector[index].err_dot_history.erase(joint_vector[index].err_dot_history.begin());
    // }
    // double err_dot_average = std::accumulate(joint_vector[index].err_dot_history.begin(),
    //                             joint_vector[index].err_dot_history.end(),
    //                             0.0) / joint_vector[index].err_dot_history.size();
    // //ROS_INFO("error_dot_average %f", err_dot_average);
    // ROS_ERROR("%f %d, error difference and index", vel+err_dot_average, index);
    p_term = joint_vector[index]->p_gain * error;
    d_term = joint_vector[index]->d_gain * -vel;

    //return 0.0;
    return (p_term + d_term);
    //return p_term + d_term + 0.3 * id_torques(index);
  }

  void GripperController::jointCallback(const sensor_msgs::JointState msg)
  {
    ROS_ERROR("abc");

    unsigned int nj = chain.getNrOfJoints();
    KDL::JntArray jointPositions(nj);
    KDL::JntArray jointVelocities(nj);
    KDL::JntArray jointAccelerations(nj);
    KDL::Wrenches f_ext;
    for (int i = 0; i < nj; i++) {
      for (int index = 0; index < nj; index++) {
        if (msg.name[i].compare(joint_names[index]) == 0) {
          jointPositions(index) = msg.position[i];
          jointVelocities(index) = msg.velocity[i];
          jointAccelerations(index) = 0.0;
          f_ext.push_back(KDL::Wrench());

          break;
        } else if (index == nj - 1){
           ROS_ERROR("No joint %s for controller", msg.name[i].c_str());
        }
      }
    }


    //ROS_INFO("nr_segments = %d", ns);
    /**for(int i = 0; i < ns; i++){
      KDL::Segment seg = chain.segments[i];
      ROS_INFO("seg %d name: %s", i, seg.getName().c_str());
      ROS_INFO("joint name: %s type: %d", seg.getJoint().getName().c_str(), seg.getJoint().getType());
    } **/
    //ROS_INFO("id torques =  %f", id_torques(0));

    ROS_ERROR("bca");
  }

}

PLUGINLIB_EXPORT_CLASS(koko_controllers::GripperController, controller_interface::ControllerBase)