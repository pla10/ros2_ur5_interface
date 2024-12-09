#include <rclcpp/rclcpp.hpp>
#include <control_msgs/action/follow_joint_trajectory.hpp>
#include <control_msgs/msg/joint_tolerance.hpp>
#include <trajectory_msgs/msg/joint_trajectory.hpp>
#include <rclcpp_action/rclcpp_action.hpp>
#include <std_srvs/srv/trigger.hpp>
#include <chrono>
#include <vector>

using namespace std::chrono_literals;

class TrajectoryActionClient : public rclcpp::Node
{
public:
    using FollowJointTrajectory = control_msgs::action::FollowJointTrajectory;
    using GoalHandleFollowJointTrajectory = rclcpp_action::ClientGoalHandle<FollowJointTrajectory>;

    TrajectoryActionClient() : Node("trajectory_action_client")
    {
        // Define joint names
        joint_names_ = {"shoulder_pan_joint", "shoulder_lift_joint", "elbow_joint", "wrist_1_joint", "wrist_2_joint", "wrist_3_joint"};

        action_client_ = rclcpp_action::create_client<FollowJointTrajectory>(
            this, "/scaled_joint_trajectory_controller/follow_joint_trajectory");

        open_gripper_client_ = this->create_client<std_srvs::srv::Trigger>("open_gripper");
        close_gripper_client_ = this->create_client<std_srvs::srv::Trigger>("close_gripper");

        if (!action_client_->wait_for_action_server(10s))
        {
            RCLCPP_ERROR(this->get_logger(), "Action server not available after waiting");
            rclcpp::shutdown();
            return;
        }

        if (!open_gripper_client_->wait_for_service(10s))
        {
            RCLCPP_ERROR(this->get_logger(), "Gripper service not available after waiting");
            rclcpp::shutdown();
            return;
        }

        time_between_points_ = 0.5; // Time between points in seconds

        // Call the gripper service based on the trajectory index
        auto request = std::make_shared<std_srvs::srv::Trigger::Request>();
        auto future = open_gripper_client_->async_send_request(request);
        std::this_thread::sleep_for(1s);

        // Prepare the sequence of trajectories
        prepare_trajectories();

        // Start executing the first trajectory
        current_trajectory_index_ = 0;
        send_next_trajectory();
    }

private:
    rclcpp_action::Client<FollowJointTrajectory>::SharedPtr action_client_;
    rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr open_gripper_client_;
    rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr close_gripper_client_;
    double time_between_points_;
    std::vector<trajectory_msgs::msg::JointTrajectory> trajectories_;
    size_t current_trajectory_index_;
    std::vector<std::string> joint_names_;

    trajectory_msgs::msg::JointTrajectory generate_trajectory_segment(
        const std::vector<double>& start_config,
        const std::vector<double>& end_config,
        int num_points)
    {
        trajectory_msgs::msg::JointTrajectory traj_msg;
        traj_msg.joint_names = joint_names_;

        // Total interpolation time (N points * time_between_points_)
        double total_time = num_points * time_between_points_;

        for (int i = 0; i <= num_points; i++)
        {
            trajectory_msgs::msg::JointTrajectoryPoint point;
            double t = i * time_between_points_;

            for (size_t j = 0; j < start_config.size(); j++)
            {
                double interpolated_position = start_config[j] + (t / total_time) * (end_config[j] - start_config[j]);
                point.positions.push_back(interpolated_position);
            }

            point.time_from_start = rclcpp::Duration::from_seconds(t);
            traj_msg.points.push_back(point);
        }

        return traj_msg;
    }


    void prepare_trajectories()
    {
        // Define the trajectories
        // Trajectory 1
        trajectory_msgs::msg::JointTrajectory traj1;
        traj1 = generate_trajectory_segment(
            {-1.60, -1.72, -2.20, -0.81, 1.60, 0.0},
            {-1.41, -0.96, -1.8, -1.96, -1.60, 0.0},
            10);
        trajectories_.push_back(traj1);

        // Trajectory 2
        trajectory_msgs::msg::JointTrajectory traj2;
        traj2 = generate_trajectory_segment(
            {-1.41, -0.96, -1.8, -1.96, -1.60, 0.0},
            {-1.60, -1.72, -2.20, -0.81, 1.60, 0.0},
            10);
        trajectories_.push_back(traj2);

        // Add additional trajectories as needed
    }

    void send_next_trajectory()
    {
        if (current_trajectory_index_ >= trajectories_.size())
        {
            RCLCPP_INFO(this->get_logger(), "All trajectories executed successfully");
            return;
        }

        auto goal_msg = FollowJointTrajectory::Goal();
        goal_msg.trajectory = trajectories_[current_trajectory_index_];
        goal_msg.goal_time_tolerance.nanosec = 500000000;

        RCLCPP_INFO(this->get_logger(), "Sending trajectory goal %zu", current_trajectory_index_ + 1);

        auto send_goal_options = rclcpp_action::Client<FollowJointTrajectory>::SendGoalOptions();
        send_goal_options.goal_response_callback =
            [this](const GoalHandleFollowJointTrajectory::SharedPtr &goal_handle) {
                if (!goal_handle)
                {
                    RCLCPP_ERROR(this->get_logger(), "Goal was rejected by the server");
                }
                else
                {
                    RCLCPP_INFO(this->get_logger(), "Goal accepted by the server, waiting for result");
                }
            };

        send_goal_options.result_callback =
            [this](const GoalHandleFollowJointTrajectory::WrappedResult &result) {
                switch (result.code)
                {
                case rclcpp_action::ResultCode::SUCCEEDED:
                    RCLCPP_INFO(this->get_logger(), "Goal %zu succeeded", current_trajectory_index_ + 1);
                    handle_trajectory_success();
                    break;
                case rclcpp_action::ResultCode::ABORTED:
                    RCLCPP_ERROR(this->get_logger(), "Goal was aborted");
                    break;
                case rclcpp_action::ResultCode::CANCELED:
                    RCLCPP_WARN(this->get_logger(), "Goal was canceled");
                    break;
                default:
                    RCLCPP_ERROR(this->get_logger(), "Unknown result code");
                    break;
                }
            };

        action_client_->async_send_goal(goal_msg, send_goal_options);
    }

    void handle_trajectory_success()
    {
        // Call the gripper service based on the trajectory index
        auto request = std::make_shared<std_srvs::srv::Trigger::Request>();
        auto future = close_gripper_client_->async_send_request(request);

        if (future.wait_for(5s) == std::future_status::ready)
        {
            auto response = future.get();
            if (response->success)
            {
                RCLCPP_INFO(this->get_logger(), "Gripper service call succeeded: %s", response->message.c_str());
            }
            else
            {
                RCLCPP_WARN(this->get_logger(), "Gripper service call failed: %s", response->message.c_str());
            }
        }
        else
        {
            RCLCPP_ERROR(this->get_logger(), "Gripper service call timed out");
        }

        // Proceed to the next trajectory
        current_trajectory_index_++;
        send_next_trajectory();
    }
};

int main(int argc, char **argv)
{
    rclcpp::init(argc, argv);
    rclcpp::spin(std::make_shared<TrajectoryActionClient>());
    rclcpp::shutdown();
    return 0;
}