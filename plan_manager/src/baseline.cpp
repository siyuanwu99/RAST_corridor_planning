/**
 * @file baseline.cpp
 * @author Siyuan Wu (siyuanwu99@gmail.com)
 * @brief
 * @version 1.0
 * @date 2022-10-24
 *
 * @copyright Copyright (c) 2022
 *
 */

#include <plan_manager/baseline.h>
#include <ros/ros.h>

void BaselinePlanner::init() {
  /*** INITIALIZE MAP ***/
  map_.reset(new RiskVoxel());
  map_->init(nh_);

  /*** INITIALIZE A STAR ***/
  a_star_.reset(new RiskHybridAstar());
  a_star_->setParam(nh_);
  a_star_->setEnvironment(map_);
  a_star_->init(odom_pos_, Eigen::Vector3d(10, 10, 4));

  /*** INITIALIZE BEZIER OPT ***/
  traj_optimizer_.reset(new traj_opt::BezierOpt());
  ROS_INFO("Trajectory optimizer initialized.");

  /*** INITIALIZE MADER DECONFLICTION ***/
  collision_avoider_.reset(new MADER(nh_));
  collision_avoider_->init();

  /*** INITIALIZE VISUALIZATION ***/
  std::string ns = "world";
  visualizer_.reset(new visualizer::Visualizer(nh_, ns));

  /*** INITIALIZE SUBSCRIBER ***/
  // click_sub_ = nh_.subscribe("/traj_start_trigger", 1, &BaselinePlanner::clickCallback, this);
  pose_sub_ = nh_.subscribe("pose", 10, &BaselinePlanner::PoseCallback, this);

  /*** INITIALIZE AUXILIARY VARIABLES ***/
  prev_pt_ = ros::Time::now().toSec();
  prev_px_ = 0.0;
  prev_py_ = 0.0;
  prev_pz_ = 0.0;
  prev_vt_ = ros::Time::now().toSec();
  prev_vx_ = 0.0;
  prev_vy_ = 0.0;
  prev_vz_ = 0.0;

  odom_vel_ = Eigen::Vector3d(0, 0, 0);
  odom_acc_ = Eigen::Vector3d(0, 0, 0);

  /*** BOOLEANS ***/
  is_map_updated_       = true;
  is_state_locked_      = false;
  is_velocity_received_ = false;
  ROS_INFO("Baseline planner initialized");
}

/**
 * @brief get current position and attitude from odometry
 * @param msg
 */
void BaselinePlanner::PoseCallback(const geometry_msgs::PoseStamped::ConstPtr& msg) {
  if (!is_state_locked_) {
    is_state_locked_ = true;

    odom_pos_.x() = msg->pose.position.x;
    odom_pos_.y() = msg->pose.position.y;
    odom_pos_.z() = msg->pose.position.z;

    odom_att_.x() = msg->pose.orientation.x;
    odom_att_.y() = msg->pose.orientation.y;
    odom_att_.z() = msg->pose.orientation.z;
    odom_att_.w() = msg->pose.orientation.w;

    is_odom_received_ = true;
  }
  is_state_locked_ = false;

  if (!is_velocity_received_) {
    double dt     = msg->header.stamp.toSec() - prev_pt_;
    odom_vel_.x() = (odom_pos_.x() - prev_px_) / dt;
    odom_vel_.y() = (odom_pos_.y() - prev_py_) / dt;
    odom_vel_.z() = (odom_pos_.z() - prev_pz_) / dt;

    prev_pt_ = msg->header.stamp.toSec();
    prev_px_ = odom_pos_.x();
    prev_py_ = odom_pos_.y();
    prev_pz_ = odom_pos_.z();
  }
}

void BaselinePlanner::clickCallback(const geometry_msgs::PoseStamped::ConstPtr& msg) {
  goal_pos_(0) = msg->pose.position.x;
  goal_pos_(1) = msg->pose.position.y;
  goal_pos_(2) = 1;
  ROS_INFO("Start position: (%f, %f, %f)", odom_pos_(0), odom_pos_(1), odom_pos_(2));
  ROS_INFO("End position: (%f, %f, %f)", goal_pos_(0), goal_pos_(1), goal_pos_(2));
  plan();
}

/**
 * @brief show A star results
 *
 */
void BaselinePlanner::showAstarPath() {
  std::vector<Eigen::Vector3d> path = a_star_->getPath(0.1);
  visualizer_->visualizeAstarPath(path);
}

bool BaselinePlanner::plan() {
  ROS_INFO("Planning...");

  /*----- Path Searching on DSP Static -----*/
  a_star_->reset();
  // a_star_->setMapCenter(odom_pos_);
  auto      t1 = ros::Time::now();
  ASTAR_RET rst =
      a_star_->search(odom_pos_, odom_vel_, odom_acc_, goal_pos_, Eigen::Vector3d(0, 0, 0), true);
  if (rst == 0) {
    a_star_->reset();
    rst = a_star_->search(odom_pos_, odom_vel_, odom_acc_, goal_pos_, Eigen::Vector3d(0, 0, 0),
                          false);
  }

  auto t2 = ros::Time::now();
  ROS_INFO("Time used: %f ms", (t2 - t1).toSec() * 1000);
  ROS_INFO("A star search finished with return code %d", rst);

  if (rst == NO_PATH) {
    ROS_WARN("No path found!");
    return false;
  }
  showAstarPath();

  /*----- Safety Corridor Generation -----*/

  std::vector<Eigen::Vector3d>  route = a_star_->getPath(cfg_.a_star_search_time_step);
  std::vector<Eigen::MatrixX4d> hPolys;
  std::vector<Eigen::Vector3d>  pc;

  if (route.size() > 4) {
    route.erase(route.begin() + 4, route.end());
  }

  t1 = ros::Time::now();
  pc.reserve(3000);
  map_->getObstaclePoints(pc);
  collision_avoider_->getObstaclePoints(pc, cfg_.a_star_search_time_step * (route.size() - 1));
  visualizer_->visualizeObstaclePoints(pc);

  Eigen::Vector3d lower_corner  = Eigen::Vector3d(-5, -5, -1) + odom_pos_;
  Eigen::Vector3d higher_corner = Eigen::Vector3d(5, 5, 3) + odom_pos_;

  sfc_gen::convexCover(route, pc, lower_corner, higher_corner, 7.0, 1.0, hPolys);
  // sfc_gen::shortCut(hPolys);  /* merge adjacent corridors */
  t2 = ros::Time::now();
  ROS_INFO("Decomps takes: %f ms", (t2 - t1).toSec() * 1000);
  visualizer_->visualizePolytope(hPolys);

  /*----- Trajectory Optimization -----*/

  std::cout << "hPolys size: " << hPolys.size() << std::endl;
  std::cout << "route size: " << route.size() << std::endl;

  /* Goal position and time allocation */
  Eigen::Vector3d     local_goal = route[route.size() - 1];
  std::vector<double> time_alloc;
  time_alloc.resize(hPolys.size(), cfg_.a_star_search_time_step);
  std::cout << "time_alloc size: " << time_alloc.size() << std::endl;
  traj_optimizer_.reset(new traj_opt::BezierOpt());
  Eigen::Matrix3d init_state, final_state;
  init_state.row(0)  = odom_pos_;
  init_state.row(1)  = odom_vel_;
  init_state.row(2)  = odom_acc_;
  final_state.row(0) = local_goal;  // TODO(@Oct 30): use distance to select the goal
  final_state.row(1) = Eigen::Vector3d(0, 0, 0);
  final_state.row(2) = Eigen::Vector3d(0, 0, 0);

  // std::cout << "init_state: " << init_state << std::endl;
  // std::cout << "final_state: " << final_state << std::endl;
  visualizer_->visualizeStartGoal(odom_pos_);
  visualizer_->visualizeStartGoal(local_goal);

  t1 = ros::Time::now();
  traj_optimizer_->setup(init_state, final_state, time_alloc, hPolys, cfg_.opt_max_vel,
                         cfg_.opt_max_acc);
  if (!traj_optimizer_->optimize()) {
    t2 = ros::Time::now();
    ROS_INFO("Time used: %f ms", (t2 - t1).toSec() * 1000);
    ROS_ERROR("Trajectory optimization failed!");
    return false;
  }
  t2 = ros::Time::now();
  ROS_INFO("TrajOpt takes: %f ms", (t2 - t1).toSec() * 1000);

  traj_optimizer_->getOptBezier(traj_);

  /*----- Trajectory Deconfliction -----*/
  if (!collision_avoider_->isSafeAfterOpt(traj_)) {
    ROS_ERROR("Trajectory collides after optimization!");
    return false;
  }

  if (!collision_avoider_->isSafeAfterChk()) {
    ROS_ERROR("Trajectory commited while checking!");
    return false;
  }

  /*----- Trajectory Visualization -----*/
  visualizer_->visualizeBezierCurve(Eigen::Vector3d::Zero(), traj_, 4.0);
  Eigen::MatrixXd cpts;
  traj_.getCtrlPoints(cpts);
  visualizer_->visualizeControlPoints(cpts);

  /* TODO: use current time as trajectory start time*/
  traj_start_time_ = ros::Time::now();
  return true;
}