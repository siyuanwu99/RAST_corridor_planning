/**
 * @file fake_particle_risk_voxel.cpp
 * @author Siyuan Wu (siyuanwu99@gmail.com)
 * @brief
 * @version 1.0
 * @date 2022-11-22
 *
 * @copyright Copyright (c) 2022
 *
 */

#include <plan_env/fake_particle_risk_voxel.h>

/* ----- Definition of these functions ----- */
/**
 * @brief initialize the fake risk voxel, define subscriptions and publications
 *
 * @param nh
 */
void FakeParticleRiskVoxel::init(ros::NodeHandle &nh) {
  nh_ = nh;

  /* parameters */
  loadParameters();

  resolution_           = 0.1F;
  local_update_range_x_ = MAP_LENGTH_VOXEL_NUM / 2 * resolution_;
  local_update_range_y_ = MAP_WIDTH_VOXEL_NUM / 2 * resolution_;
  local_update_range_z_ = MAP_HEIGHT_VOXEL_NUM / 2 * resolution_;
  ROS_INFO("[FAKE_MAP] Local update range: %f, %f, %f", local_update_range_x_,
           local_update_range_y_, local_update_range_z_);
  ROS_INFO("[FAKE_MAP] Init fake risk voxel map");

  /* initialize inflated kernel */
  inf_step_ = clearance_ / resolution_;
  inflate_kernel_.reserve((2 * inf_step_ + 1) * (2 * inf_step_ + 1) * (2 * inf_step_ + 1));
  for (int x = -inf_step_; x <= inf_step_; x++) {
    for (int y = -inf_step_; y <= inf_step_; y++) {
      for (int z = -inf_step_; z <= inf_step_; z++) {
        inflate_kernel_.push_back(Eigen::Vector3i(x, y, z));
      }
    }
  }
  ROS_INFO("[MAP_BASE] Inflated kernel size: %d", (int)inflate_kernel_.size());

  /* subscribers */
  cloud_sub_ = nh_.subscribe("map/cloud", 1, &FakeParticleRiskVoxel::cloudCallback, this);
  gt_state_sub_ =
      nh_.subscribe("map/state", 1, &FakeParticleRiskVoxel::groundTruthStateCallback, this);
  odom_sub_ = nh_.subscribe("map/odom", 1, &FakeParticleRiskVoxel::odomCallback, this);
  pose_sub_ = nh_.subscribe("map/pose", 1, &FakeParticleRiskVoxel::poseCallback, this);

  /* publishers */
  cloud_pub_    = nh.advertise<sensor_msgs::PointCloud2>("map/occupancy_inflated", 1, true);
  obstacle_pub_ = nh.advertise<sensor_msgs::PointCloud2>("vis_obstacle", 1, true);
  risk_pub_     = nh.advertise<sensor_msgs::PointCloud2>("map/risk_map", 1);

  /* publish point clouds in 20 Hz */
  cloud_ = pcl::PointCloud<pcl::PointXYZ>::Ptr(new pcl::PointCloud<pcl::PointXYZ>);
  cloud_->points.reserve(80000);

  /* publish point clouds in 10 Hz */
  pub_timer_ = nh.createTimer(ros::Duration(0.10), &FakeParticleRiskVoxel::pubCallback, this);

  /* initialize odometry */
  pose_ = Eigen::Vector3f::Zero();
  q_    = Eigen::Quaternionf::Identity();

  last_update_time_ = ros::Time::now();
}

/**
 * @brief
 *
 * @param cloud_msg
 */
void FakeParticleRiskVoxel::updateMap(const sensor_msgs::PointCloud2::ConstPtr &cloud_msg) {
  /* ground truth starting time */
  last_update_time_ = ros::Time::now();
  ros::Time tic     = last_update_time_;

  pcl::fromROSMsg(*cloud_msg, *cloud_);
  pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_filtered(new pcl::PointCloud<pcl::PointXYZ>);
  /* Remove points out of range */
  pcl::PassThrough<pcl::PointXYZ> pass_x;
  pass_x.setInputCloud(cloud_);
  pass_x.setFilterFieldName("x");
  pass_x.setFilterLimits(pose_[0] - local_update_range_x_, pose_[0] + local_update_range_x_);
  pass_x.filter(*cloud_filtered);

  pcl::PassThrough<pcl::PointXYZ> pass_y;
  pass_y.setInputCloud(cloud_filtered);
  pass_y.setFilterFieldName("y");
  pass_y.setFilterLimits(pose_[1] - local_update_range_y_, pose_[1] + local_update_range_y_);
  pass_y.filter(*cloud_filtered);

  pcl::PassThrough<pcl::PointXYZ> pass_z;
  pass_z.setInputCloud(cloud_filtered);
  pass_z.setFilterFieldName("z");
  pass_z.setFilterLimits(pose_[2] - local_update_range_z_, pose_[2] + local_update_range_z_);
  pass_z.filter(*cloud_filtered);

  /* clear risk_maps_ */
  for (int i = 0; i < VOXEL_NUM; i++) {
    for (int j = 0; j < PREDICTION_TIMES; j++) {
      risk_maps_[i][j] = 0.0F;
    }
  }

  /* add points to map and inflation */
  for (auto &points : cloud_filtered->points) {
    Eigen::Vector3f pt = Eigen::Vector3f(points.x, points.y, points.z) - pose_;
    if (isInRange(pt)) {
      risk_maps_[getVoxelIndex(pt)][0] = 1.0F;
    }
  }
  std::vector<int> obs_idx_list(VOXEL_NUM);

  /* read ground truth and construct future maps */
  obs_idx_list.clear();
  for (int i = 0; i < VOXEL_NUM; i++) {
    if (risk_maps_[i][0] > risk_threshold_) {
      obs_idx_list.push_back(i);
    }
  }

  for (auto &i : obs_idx_list) {
    Eigen::Vector3f pt = getVoxelPosition(i);

    Eigen::Vector3f vel(0, 0, 0);
    for (auto &cyl : gt_cylinders_) {
      if (cyl.type == 3) {
        Eigen::Vector3f pt_cyl = Eigen::Vector3f(cyl.x, cyl.y, pt.z());
        float           dist   = (pt - pt_cyl).norm();
        if (dist <= cyl.w + clearance_) {
          vel = Eigen::Vector3f(cyl.vx, cyl.vy, 0.0F);
          break;
        }
      } else if (cyl.type == 2) {
        Eigen::Vector3f             pt_cyl = Eigen::Vector3f(cyl.x, cyl.y, cyl.z);
        Eigen::Quaternionf          q      = Eigen::Quaternionf(cyl.qw, cyl.qx, cyl.qy, cyl.qz);
        Eigen::Hyperplane<float, 3> plane  = Eigen::Hyperplane<float, 3>::Through(
            pt_cyl, pt_cyl + q * Eigen::Vector3f(0, 1, 0), pt_cyl + q * Eigen::Vector3f(1, 0, 0));
        Eigen::Vector3f b             = plane.projection(pt);
        float           dist_to_plane = plane.absDistance(pt);
        float           dist          = (pt_cyl - b).norm();
        if (abs(cyl.w / 2 - dist) < 2 * resolution_ && dist_to_plane < 2 * resolution_) {
          vel = Eigen::Vector3f(cyl.vx, cyl.vy, 0.0F);
          break;
        }
      } else {
        std::cout << "unknown type: " << cyl.type << std::endl;
      }
    }
    for (int k = 1; k < PREDICTION_TIMES; k++) {
      Eigen::Vector3f pt_pred = pt + vel * time_resolution_ * k - pose_;
      if (isInRange(pt_pred)) {
        risk_maps_[getVoxelIndex(pt_pred)][k] = 1.0F;
      }
    }
  }
  // ros::Time t4 = ros::Time::now();
  // ROS_INFO("construct future map time: %f", (t4 - t3).toSec());

  /* add other agents to the map */
  tic = ros::Time::now();

  if (is_multi_agents_) {
    std::vector<bool> is_swarm_traj_valid;
    is_swarm_traj_valid.resize(coordinator_->getNumAgents(), true);
    createEgoParticlesVoxel();
    int ego_id = coordinator_->getEgoID();
    int n_rbts = coordinator_->getNumAgents();

    for (int t_idx = 0; t_idx < PREDICTION_TIMES; t_idx++) {
      std::vector<Eigen::Vector3d> particles;

      double t = last_update_time_.toSec() + time_resolution_ * t_idx;
      for (int i = 0; i < n_rbts; i++) {
        if (i == ego_id || !is_swarm_traj_valid[i]) {
          continue;
        }
        bool is_traj_valid     = coordinator_->getWaypoints(particles, i, t);
        is_swarm_traj_valid[i] = is_traj_valid;
      }

      /* filter */
      for (auto &pt : particles) {
        pt = pt - pose_.cast<double>();
      }
      addObstaclesToRiskMap(particles, t_idx);
    }
  }
  ros::Time toc = ros::Time::now();
  std::cout << "[MAMapUpdate] adding obstacles costs: " << (toc - tic).toSec() * 1000 << "ms"
            << std::endl;
}

/**
 * @brief create ego particles voxel in the map coordinate, to reduce time when collision check
 */
void FakeParticleRiskVoxel::createEgoParticlesVoxel() {
  /* add ego particles to the map */
  std::vector<Eigen::Vector3d> particles = coordinator_->getEgoParticles();
  if (particles.size() != inflate_kernel_.size() && particles.size() != 0) {
    inflate_kernel_.clear();
    for (auto &pt : particles) {
      Eigen::Vector3f ptf = pt.cast<float>();
      inflate_kernel_.push_back(getVoxelRelIndex(ptf));
    }
    ROS_INFO("[MAMapUpdate]: inflate kernel size: %lu", inflate_kernel_.size());
  }
}

/**
 * @brief
 * @param state_msg
 */
void FakeParticleRiskVoxel::groundTruthStateCallback(
    const visualization_msgs::MarkerArray::ConstPtr &state_msg) {
  int n = state_msg->markers.size();
  gt_cylinders_.clear();
  gt_cylinders_.resize(n);

  for (auto &mk : state_msg->markers) {
    gt_cylinders_[mk.id].type = mk.type;
    gt_cylinders_[mk.id].x    = mk.pose.position.x;
    gt_cylinders_[mk.id].y    = mk.pose.position.y;
    gt_cylinders_[mk.id].z    = mk.pose.position.z;
    gt_cylinders_[mk.id].w    = mk.scale.x;
    gt_cylinders_[mk.id].h    = mk.points[0].z;
    gt_cylinders_[mk.id].vx   = mk.points[1].x - mk.points[0].x;
    gt_cylinders_[mk.id].vy   = mk.points[1].y - mk.points[0].y;
    gt_cylinders_[mk.id].qw   = mk.pose.orientation.w;
    gt_cylinders_[mk.id].qx   = mk.pose.orientation.x;
    gt_cylinders_[mk.id].qy   = mk.pose.orientation.y;
    gt_cylinders_[mk.id].qz   = mk.pose.orientation.z;
  }
}

/**
 * @brief add particles to map
 * @param pts  a list of particles
 * @param t_index time index
 */
void FakeParticleRiskVoxel::addObstaclesToRiskMap(const std::vector<Eigen::Vector3d> &pts,
                                                  int                                 t_index) {
  for (auto &pt : pts) {
    Eigen::Vector3f ptf = pt.cast<float>();
    if (!isInRange(ptf)) continue;
    int index                  = getVoxelIndex(ptf);
    risk_maps_[index][t_index] = 1.0;
  }
}

/**
 * @brief collision check on voxel map with clearance (none-inflated map)
 *
 * @param pos position in world frame
 * @param t index of the time frame
 * @return 0: free, 1: occupied, -1: out of bound
 */
int FakeParticleRiskVoxel::getClearOcccupancy(const Eigen::Vector3d &pos, int t) const {
  // to debug
  Eigen::Vector3f pos_f = pos.cast<float>() - pose_;
  Eigen::Vector3i pos_i = getVoxelRelIndex(pos_f);
  if (!isInRange(pos_i)) return -1;
  // if (risk_maps_[getVoxelIndex(pos_i)][t] > risk_threshold_) return 1;
  // return 0;

  float sum_risk = 0.0;
  // std::cout << "inflate kernel size: " << inflate_kernel_.size() << std::endl;
  for (auto &ego_i : inflate_kernel_) {
    Eigen::Vector3i p = pos_i + ego_i;
    // std::cout << "pos_i: " << pos_i.transpose() << ", ego_i: " << ego_i.transpose()
    //           << ", p: " << p.transpose() << std::endl;
    if (!isInRange(p)) continue;
    int idx = getVoxelIndex(p);
    sum_risk += risk_maps_[idx][t];
    if (sum_risk > risk_threshold_) return 1; /* collision */
  }
  return 0;
}

int FakeParticleRiskVoxel::getClearOcccupancy(const Eigen::Vector3d &pos) const {
  return getClearOcccupancy(pos, 0);
}

/**
 * @brief collision check on risk map
 * for a given double timestamp, check the collision status of two discrete time frames
 * @param pos query position in world frame
 * @param dt double timestamp
 * @return 0: free, 1: occupied, -1: out of bound
 */
int FakeParticleRiskVoxel::getClearOcccupancy(const Eigen::Vector3d &pos, double dt) const {
  int tc = ceil(dt / time_resolution_);
  tc     = tc > PREDICTION_TIMES ? PREDICTION_TIMES : tc;
  int tf = floor(dt / time_resolution_);
  tf     = tf > PREDICTION_TIMES ? PREDICTION_TIMES : tf;

  int ret0 = getClearOcccupancy(pos, tc);
  int ret1 = getClearOcccupancy(pos, tf);
  if (ret0 == -1 || ret1 == -1) return -1;
  if (ret0 == 0 && ret1 == 0) {
    return 0;
  } else {
    return 1;
  }
}
