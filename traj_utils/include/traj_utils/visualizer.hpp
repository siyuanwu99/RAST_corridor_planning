/**
 * @file visualizer.hpp
 * @author Siyuan Wu (siyuanwu99@gmail.com)
 * @brief based on GCOPTER(https://github.com/ZJU-FAST-Lab/GCOPTER) from Zhepei Wang
 * @version 1.0
 * @date 2022-07-15
 *
 * @copyright Copyright (c) 2022
 *
 */
#ifndef VISUALIZER_HPP
#define VISUALIZER_HPP

#include <geometry_msgs/Point.h>
#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/TwistStamped.h>
#include <ros/ros.h>
#include <std_msgs/Float32MultiArray.h>
#include <std_msgs/Float64.h>
#include <std_msgs/UInt8.h>
#include <visualization_msgs/Marker.h>
#include <visualization_msgs/MarkerArray.h>

#include <Eigen/Eigen>
#include <Eigen/Sparse>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <traj_utils/corridor.hpp>
#include <traj_utils/poly_traj.hpp>

#include "decomp_ros_utils/data_ros_utils.h"
using namespace std;
namespace visualizer {
/**
 * @brief jet color map
 * to display colorful velocity on trajectories
 * @param a [0, 1]
 * @return Eigen::Vector3d
 */
template <typename T>
inline Eigen::Vector3d jetColorMap(T a) {
  double          s = a * 4;
  Eigen::Vector3d c;  // [r, g, b]
  switch (static_cast<int>(floor(s))) {
    case 0:
      c << 0, 0, s;
      break;
    case 1:
      c << 0, s - 1, 1;
      break;
    case 2:
      c << s - 2, 1, 3 - s;
      break;
    case 3:
      c << 1, 4 - s, 0;
      break;
    default:
      c << 1, 0, 0;
      break;
  }
  return c;
}

template <typename T>
inline Eigen::Vector3d hotColorMap(T a) {
  double          s = a * 3;
  Eigen::Vector3d c;
  switch (static_cast<int>(floor(s))) {
    case 0:
      c << s, 0, 0;
      break;
    case 1:
      c << 1, s - 1, 0;
      break;
    case 2:
      c << 1, 1, s - 2;
      break;
    default:
      c << 1, 0, 0;
      break;
  }
  return c;
}

inline void displayTrajectory(const Eigen::Vector3d&        start_pos,
                              const polynomial::Trajectory& traj,
                              double                        max_vel,
                              const ros::Publisher&         traj_pub,
                              const std::string             frame_id = "world") {
  visualization_msgs::Marker traj_marker;
  traj_marker.header.frame_id    = frame_id;
  traj_marker.header.stamp       = ros::Time::now();
  traj_marker.type               = visualization_msgs::Marker::LINE_LIST;
  traj_marker.pose.orientation.w = 1.00;
  traj_marker.action             = visualization_msgs::Marker::ADD;
  traj_marker.id                 = 0;
  traj_marker.ns                 = "trajectory";
  traj_marker.color.r            = 0.00;
  traj_marker.color.g            = 0.50;
  traj_marker.color.b            = 1.00;
  traj_marker.scale.x            = 0.10;

  double          T     = 0.05;
  Eigen::Vector3d lastX = traj.getPos(0.0) + start_pos;
  for (double t = T; t < traj.getDuration(); t += T) {
    std_msgs::ColorRGBA c;
    Eigen::Vector3d     jets = jetColorMap(traj.getVel(t).norm() / max_vel);
    c.r                      = jets[0];
    c.g                      = jets[1];
    c.b                      = jets[2];

    geometry_msgs::Point point;
    Eigen::Vector3d      X = traj.getPos(t) + start_pos;
    point.x                = lastX(0);
    point.y                = lastX(1);
    point.z                = lastX(2);
    traj_marker.points.push_back(point);
    traj_marker.colors.push_back(c);
    point.x = X(0);
    point.y = X(1);
    point.z = X(2);
    traj_marker.points.push_back(point);
    traj_marker.colors.push_back(c);
    lastX = X;
  }
  traj_pub.publish(traj_marker);
}

inline void displayCorridors(const planner::Corridors& corridors,
                             const Eigen::Vector3d&    map_pose,
                             const ros::Publisher&     crd_pub,
                             const std::string         frame_id = "world") {
  vec_E<Polyhedron3D> polyhedra;
  polyhedra.reserve(corridors.size());
  for (const auto& crd : corridors) {
    Polyhedron3D poly;
    for (int i = 0; i < crd.cols(); i++) {
      poly.add(Hyperplane3D(crd.col(i).tail<3>(), crd.col(i).head<3>()));
    }
    polyhedra.push_back(poly);
  }
  decomp_ros_msgs::PolyhedronArray msg = DecompROS::polyhedron_array_to_ros(polyhedra);
  msg.header.frame_id                  = frame_id;
  msg.header.stamp                     = ros::Time::now();
  crd_pub.publish(msg);
}

class Visualizer {
 private:
  ros::NodeHandle _nh;
  ros::Publisher  _corridor_pub;
  ros::Publisher  _colorful_traj_pub;
  ros::Publisher  _astar_path_pub;
  ros::Publisher  _start_goal_pub;
  std::string     _frame_id;

 public:
  Visualizer(ros::NodeHandle& nh) : _nh(nh) {
    _corridor_pub      = _nh.advertise<decomp_ros_msgs::PolyhedronArray>("vis_corridor", 1);
    _colorful_traj_pub = _nh.advertise<visualization_msgs::Marker>("vis_color_traj", 1);
    _astar_path_pub    = _nh.advertise<visualization_msgs::Marker>("vis_astar_path", 1);
    _start_goal_pub    = _nh.advertise<visualization_msgs::Marker>("vis_start_goal", 1);
    _frame_id          = "world";
  }

  Visualizer(ros::NodeHandle& nh, std::string& frame_id) : _nh(nh), _frame_id(frame_id) {
    _corridor_pub      = _nh.advertise<decomp_ros_msgs::PolyhedronArray>("vis_corridor", 1);
    _colorful_traj_pub = _nh.advertise<visualization_msgs::Marker>("vis_color_traj", 1);
    _astar_path_pub    = _nh.advertise<visualization_msgs::Marker>("vis_astar_path", 1);
    _start_goal_pub    = _nh.advertise<visualization_msgs::Marker>("vis_start_goal", 1);
  }
  ~Visualizer() {}
  typedef std::shared_ptr<Visualizer> Ptr;

  void visualizeTrajectory(const Eigen::Vector3d&        start_pos,
                           const polynomial::Trajectory& traj,
                           double                        max_vel);
  void visualizeCorridors(const planner::Corridors& corridors, const Eigen::Vector3d& map_pose);
  void visualizeAstarPath(const std::vector<Eigen::Vector3d>& points);
  void visualizeStartGoal(const Eigen::Vector3d& center, int sg = 1);
};

}  // namespace visualizer
#endif  // VISUALIZER_HPP