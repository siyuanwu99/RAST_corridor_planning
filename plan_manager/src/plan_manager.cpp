/* /**
 * @file plan_manager.cpp
 * @author Siyuan Wu (siyuanwu99@gmail.com)
 * @brief
 * @version 1.0
 * @date 2022-11-01
 *
 * @copyright Copyright (c) 2022
 *
 */

#include <plan_manager/plan_manager.h>

void FiniteStateMachine::run() {
  _nh.param("drone_id", _drone_id, 0);
  _nh.param("goal_x", _goal[0], 0.0);
  _nh.param("goal_y", _goal[1], 0.0);
  _nh.param("goal_z", _goal[2], 0.0);
  _nh.param("fsm/goal_tolerance", _cfgs.goal_tolerance, 1.0);
  _nh.param("fsm/replan_tolerance", _cfgs.replan_tolerance, 1.0);
  _nh.param("fsm/replan_duration", _cfgs.replan_duration, 0.1);

  /* Initialize planner */
  _planner.reset(new BaselinePlanner(_nh, BaselineParameters(_nh)));
  _planner->init();

  /* ROS publishers */
  _traj_pub           = _nh.advertise<traj_utils::BezierTraj>("trajectory", 1);
  _broadcast_traj_pub = _nh.advertise<traj_utils::BezierTraj>("/broadcast_traj", 1);
  _trigger_sub =
      _nh.subscribe("/traj_start_trigger", 1, &FiniteStateMachine::TriggerCallback, this);

  _is_goal_received       = false;
  _is_exec_triggered      = false;
  _is_odom_received       = false;
  _is_safety_mode_enabled = false;
  _is_map_updated         = false;

  _traj_idx = 0;

  _waypoints.push(_goal);

  _time = ros::Time::now().toSec();

  _status = FSM_STATUS::INIT; /* Initialize state machine */
  ROS_INFO("[FSM] Initialization complete");
  _fsm_timer = _nh.createTimer(ros::Duration(0.1), &FiniteStateMachine::FSMCallback, this);
}

/** ***********************************************************************************************
 * State Machine
 * ***********************************************************************************************/

/**
 * @brief finite state machine for planning
 *  States:
 * - INIT: waiting for input information
 * - WAIT_TARGET: waiting for target information
 * - NEW_PLAN: planning a new trajectory from zero velocity
 * - REPLAN: replanning at the end of current trajectory
 * - EXEC_TRAJ: executing the trajectory
 * - EMERGENCY_REPLAN: replan the trajectory from current position
 * - EXIT: exit the planner
 * @param event
 */
void FiniteStateMachine::FSMCallback(const ros::TimerEvent& event) {
  double risk = 0;
  switch (_status) {
    /* initialize */
    case FSM_STATUS::INIT:
      FSMChangeState(FSM_STATUS::WAIT_TARGET);
      break;

    /* wait for callback */
    case FSM_STATUS::WAIT_TARGET:
      if (!isInputLost() && _is_goal_received) {
        // globalPlan();
        FSMChangeState(FSM_STATUS::REPLAN);  // TODO(CHANGE TO NEW_PLAN)
      } else {
        ROS_INFO_ONCE("[FSM] Waiting for odom[%d] and future risk[%d] ", _is_odom_received,
                      _is_map_updated);
      }
      break;

    /* plan a new trajectory from current position */
    case FSM_STATUS::NEW_PLAN:
      if (isInputLost()) {
        FSMChangeState(FSM_STATUS::WAIT_TARGET);
      } else {
        bool is_success = false;
        if (checkTimeLapse(1.0)) {
          // is_success = localReplan(PLAN_TYPE::NEW);
        }
        // bool is_safe    = isTrajectorySafe(_traj);
        bool is_safe = true;         /** TODO: time delay !!! */
        if (is_success && is_safe) { /* publish trajectory */
          publishTrajectory();
          ROS_WARN("[FSM] %f", _traj_start_time.toSec());
        }

        if (_is_exec_triggered) { /* execute trajectory */
          FSMChangeState(FSM_STATUS::EXEC_TRAJ);
        }
      }
      break;

    /* execute the trajectory, replan when current traj is about to finish */
    case FSM_STATUS::EXEC_TRAJ:
      if (isInputLost()) {
        FSMChangeState(FSM_STATUS::WAIT_TARGET);
      } else {
        std::cout << termcolor::bright_red << "Target: " << _waypoints.front().transpose()
                  << " now " << _planner->getPos().transpose() << std::endl;
        bool is_safe = isTrajectorySafe();
        // TODO(@siyuan): rewrite logic
        // bool is_replan_required = executeTrajectory();
        // if (is_replan_required) { /* replan */
        //   FSMChangeState(FSM_STATUS::REPLAN);
        // } else if (!is_safe) {
        //   FSMChangeState(FSM_STATUS::EMERGENCY_REPLAN);

        // } else if (isGoalReached(_planner->getPos())) {
        //   FSMChangeState(FSM_STATUS::GOAL_REACHED);
        // }
        if (checkTimeLapse(_cfgs.replan_duration)) {
          FSMChangeState(FSM_STATUS::REPLAN);
        }

        if (isGoalReached(_planner->getPos())) {
          FSMChangeState(FSM_STATUS::GOAL_REACHED);
        }
      }
      break;

    /* replan based on current trajectory */
    case FSM_STATUS::REPLAN:
      if (isInputLost()) {
        FSMChangeState(FSM_STATUS::WAIT_TARGET);
      } else {
        _planner->setGoal(_goal);

        ros::Time t1         = ros::Time::now();
        bool      is_success = _planner->plan();
        ros::Time t2         = ros::Time::now();
        ROS_INFO("[FSM] cost: %f ms", (t2 - t1).toSec() * 1000);

        // bool is_safe    = true;  // TODO: isTrajectorySafe(_traj);
        // bool is_safe    = isTrajectorySafe(_traj);

        bool is_finished = isGoalReached(_planner->getPos());
        std::cout << termcolor::bright_red << "Target: " << _goal.transpose() << " now "
                  << _planner->getPos().transpose() << std::endl;
        if (is_success && !is_finished) {
          _prev_plan_time = ros::Time::now();
          _planner->getTrajStartTime(_traj_start_time);
          publishTrajectory();
          FSMChangeState(FSM_STATUS::EXEC_TRAJ);
        } else {
          if (is_finished) {
            FSMChangeState(FSM_STATUS::GOAL_REACHED);
          } else {
            ROS_WARN("[FSM] Replanning failed");
          }
        }
      }
      break;

    /* emergency replan */
    // case FSM_STATUS::EMERGENCY_REPLAN:
    //   if (!_is_safety_mode_enabled) {
    //     FSMChangeState(FSM_STATUS::NEW_PLAN);
    //   } else {
    //     bool is_success = localReplan(PLAN_TYPE::EMERGENCY);
    //     if (is_success) {
    //       publishTrajectory();
    //       FSMChangeState(FSM_STATUS::EXEC_TRAJ);
    //     } else {
    //       ROS_WARN("Emergency replanning failed");
    //     }
    //   }
    //   break;

    /* reached the goal, clear the buffer and wait new goal */
    case FSM_STATUS::GOAL_REACHED:
      _is_goal_received  = false;  // reset goal
      _is_exec_triggered = false;
      _waypoints.pop();
      // _traj.clear();
      FSMChangeState(FSM_STATUS::WAIT_TARGET);
      break;

    case FSM_STATUS::EXIT:
      break;

    default:
      ROS_ERROR("[FSM] Invalid FSM state");
      break;
  }
}

/**
 * @brief change the state of the finite state machine
 * @param state
 */
void FiniteStateMachine::FSMChangeState(FSM_STATUS new_state) {
  FSMPrintState(new_state);
  _status = new_state;
}

/**
 * @brief print the current state of the finite state machine via termcolor
 * This function is used for debugging purposes
 */
void FiniteStateMachine::FSMPrintState(FSM_STATUS new_state) {
  static string state_str[8] = {"INIT",      "WAIT_TARGET", "NEW_PLAN",     "REPLAN",
                                "EXEC_TRAJ", "EMERGENCY",   "GOAL_REACHED", "EXIT"};
  std::cout << termcolor::dark << termcolor::on_bright_green << "[UAV" << _drone_id
            << " FSM] status " << termcolor::bright_cyan << termcolor::on_white
            << state_str[static_cast<int>(_status)] << " >> "
            << state_str[static_cast<int>(new_state)] << termcolor::reset << std::endl;
}

/**
 * @brief Trigger can be used to start the planner and receive the goal position
 *
 * @param msg
 */
void FiniteStateMachine::TriggerCallback(const geometry_msgs::PoseStampedPtr& msg) {
  if (_is_exec_triggered) {
    ROS_INFO("[FSM] Execution has already triggered");
    return;
  }
  ROS_WARN("[FSM] trigger received");
  _is_exec_triggered = true;

  if (!_is_goal_received) {
    if (_waypoints.empty()) {
      _goal.x() = msg->pose.position.x;
      _goal.y() = msg->pose.position.y;
      _goal.z() = 1;
      _waypoints.push(_goal);
      ROS_INFO("[FSM] New goal received: %f, %f, %f", _goal.x(), _goal.y(), _goal.z());
    } else {
      _goal = _waypoints.front();
      ROS_INFO("[FSM] Existing waypoints: %f, %f, %f", _goal.x(), _goal.y(), _goal.z());
      ROS_INFO("[FSM] remaining waypoints: %d", (int)_waypoints.size());
    }
    _is_goal_received = true;
  }
}

/**********************************************************
 * Utility Functions
 * ********************************************************/

void FiniteStateMachine::publishTrajectory() {
  _traj_idx++;
  Trajectory traj = _planner->getTrajectory(); /* TODO: reduce copy */
  int        N    = traj.getOrder();
  TrajMsg    msg;

  msg.drone_id   = _drone_id;
  msg.traj_id    = _traj_idx;
  msg.start_time = _traj_start_time;
  msg.pub_time   = ros::Time::now();
  msg.order      = N;

  int piece_num = traj.getNumPieces();

  msg.duration.resize(piece_num);
  for (int i = 0; i < piece_num; i++) {
    msg.duration[i] = traj[i].getDuration();
  }

  Eigen::MatrixXd cpts;
  traj.getCtrlPoints(cpts);
  int R = cpts.rows();
  msg.cpts.resize(R);
  for (int i = 0; i < R; i++) {
    msg.cpts[i].x = cpts(i, 0);
    msg.cpts[i].y = cpts(i, 1);
    msg.cpts[i].z = cpts(i, 2);
  }

  _traj_pub.publish(msg);
  _broadcast_traj_pub.publish(msg);
}

bool FiniteStateMachine::isTrajectorySafe() { return true; }
