#include <ndt_utility.h>

NdtMatching::NdtMatching(){
    nh.param<std::string>("localization/pointCloudTopic", pointCloudTopic, "points_raw");
    nh.param<std::string>("localization/imuOdomTopic", imuOdomTopic, "odometry/imu");
    nh.param<std::string>("localization/mapPath", map_path_, "");
    nh.param<std::string>("localization/ndtOdomTopic", ndtOdomTopic, "ndt/current_pose");
    nh.param<std::string>("localization/lidarFrame", lidarFrame_, "lidar1");
    nh.param<bool>("localization/use_imu", use_imu_, true);
    ROS_INFO_STREAM("IMU USE STATUS: " << use_imu_ << "??? " << false);

    nh.param<double>("localization/NDTEpsilon", ndt_epsilon_, 0.01);
    nh.param<double>("localization/NDTStepSize", ndt_step_size_, 0.05);
    nh.param<int>("localization/NDTMaxIteration", ndt_max_iteration_, 35);
    nh.param<double>("localization/NDTResolution", ndt_resolution_, 2.0);
    nh.param<double>("localization/NDTCurrentPointsLeafSize", ndt_currentpoints_leafsize_, 0.5);
    
    subImuOdom = nh.subscribe<nav_msgs::Odometry>(imuOdomTopic, 1, &NdtMatching::OdometryHandler, this, ros::TransportHints().tcpNoDelay());
    subPointCloud = nh.subscribe<sensor_msgs::PointCloud2>(pointCloudTopic, 1, &NdtMatching::PointCloudHandler, this, ros::TransportHints().tcpNoDelay());
    sub_initial_pose_ = nh.subscribe<geometry_msgs::PoseWithCovarianceStamped>("/initialpose", 1, &NdtMatching::InitialPoseHandler, this, ros::TransportHints().tcpNoDelay());
    sub_fake_initial_pose_ = nh.subscribe<geometry_msgs::PoseStamped>("/ndt/current_pose_fake", 1, &NdtMatching::FakeInitialPoseHandler, this, ros::TransportHints().tcpNoDelay());

    pubNdtOdometry = nh.advertise<nav_msgs::Odometry>(ndtOdomTopic, 2000);
    pubNdtInitialGuess_ = nh.advertise<nav_msgs::Odometry>("/ndt/initial_guess", 2000);
    pubGlobalMap = nh.advertise<sensor_msgs::PointCloud2>("global_map", 2000);
    pubCurPointCloud = nh.advertise<sensor_msgs::PointCloud2>("cur_pointcloud", 2000);

    global_map_.reset(new pcl::PointCloud<PointType>());
    global_map_downsample_.reset(new pcl::PointCloud<PointType>());
    current_pointcloud_.reset(new pcl::PointCloud<PointType>());
    initial_guess_ = Eigen::Matrix4f::Identity();

    ndt_.setTransformationEpsilon(ndt_epsilon_);
    ndt_.setStepSize(ndt_step_size_);
    ndt_.setMaximumIterations(ndt_max_iteration_);
    ndt_.setResolution(ndt_resolution_);
}


void NdtMatching::InitialPoseHandler(const geometry_msgs::PoseWithCovarianceStamped::ConstPtr& odom_msg){
    odom_timestamp_ = odom_msg->header.stamp.toSec();
    auto& t = odom_msg->pose.pose.position;
    auto& quaternion = odom_msg->pose.pose.orientation;
    Eigen::Quaternionf rotation(quaternion.w, quaternion.x, quaternion.y, quaternion.z);
    Eigen::Translation3f translation(t.x, t.y, t.z);

    // update initial guess
    initial_guess_.block<3, 3>(0, 0) = rotation.toRotationMatrix();
    initial_guess_.block<3, 1>(0, 3) = translation.vector();
    initial_pose_set_ = true;
    initial_pose_used_ = false;
    ROS_INFO_STREAM("Initial pose has been set!");
}

void NdtMatching::FakeInitialPoseHandler(const geometry_msgs::PoseStamped::ConstPtr& odom_msg){
    if(initial_pose_set_)
        return;
    odom_timestamp_ = odom_msg->header.stamp.toSec();
    auto& t = odom_msg->pose.position;
    auto& quaternion = odom_msg->pose.orientation;
    Eigen::Quaternionf rotation(quaternion.w, quaternion.x, quaternion.y, quaternion.z);
    Eigen::Translation3f translation(t.x, t.y, t.z);

    // update initial guess
    initial_guess_.block<3, 3>(0, 0) = rotation.toRotationMatrix();
    initial_guess_.block<3, 1>(0, 3) = translation.vector();
    initial_pose_set_ = true;
    initial_pose_used_ = false;
    ROS_INFO_STREAM("Initial pose has been set!");
}

void NdtMatching::OdometryHandler(const nav_msgs::Odometry::ConstPtr& odom_msg){
    if(!initial_pose_set_ || !use_imu_)
        return;
    odom_timestamp_ = odom_msg->header.stamp.toSec();
    auto& t = odom_msg->pose.pose.position;
    auto& quaternion = odom_msg->pose.pose.orientation;
    Eigen::Quaternionf rotation(quaternion.w, quaternion.x, quaternion.y, quaternion.z);
    Eigen::Translation3f translation(t.x, t.y, t.z);

    std::lock_guard<std::mutex> lck(initial_guess_mutux_);
    // update initial guess
    initial_guess_.block<3, 3>(0, 0) = rotation.toRotationMatrix();
    initial_guess_.block<3, 1>(0, 3) = translation.vector();
    initial_pose_used_ = false;
}

void NdtMatching::PointCloudHandler(const sensor_msgs::PointCloud2::ConstPtr& pointcloud_msg){
    if(!map_load_ || !initial_pose_set_)
        return;

    // if (ndt_.getInputTarget() == nullptr) {
    //     ROS_WARN_STREAM_THROTTLE(1, "No MAP!");
    //     return;
    // }

    // for calculate avg time and iteraton
    static int iteration_sum = 0, num = 0;
    static double time_sum = 0;
    double start_time = ros::WallTime::now().toSec();
    pcl::PointCloud<PointType>::Ptr filtered_current_pointcloud(new pcl::PointCloud<PointType>()); 
    pcl::fromROSMsg(*pointcloud_msg, *current_pointcloud_);

    // downsample
    // ApproximateVoxelGrid -- faster but less precious
    // pcl::ApproximateVoxelGrid<PointType> filter;
    pcl::VoxelGrid<PointType> filter;
    filter.setLeafSize(ndt_currentpoints_leafsize_, ndt_currentpoints_leafsize_, ndt_currentpoints_leafsize_);
    filter.setInputCloud(current_pointcloud_);
    filter.filter(*filtered_current_pointcloud);
    // ROS_DEBUG_STREAM("cur point cloud size after downsample: " << filtered_current_pointcloud->points.size());
    static Eigen::Matrix4f initial_value = Eigen::Matrix4f::Identity();
    if(use_imu_){
        if(!initial_pose_used_){
            std::lock_guard<std::mutex> lck(initial_guess_mutux_);
            initial_value = initial_guess_;
            initial_pose_used_ = true;
            ROS_DEBUG("In imu mode!");
        }
    }
    else{
        static Eigen::Matrix4f last_value = Eigen::Matrix4f::Identity();
        if(!initial_pose_used_){
            initial_value = initial_guess_;
            last_value = initial_guess_;
            initial_pose_used_ = true;
        }
        else{
            Eigen::Matrix4f temp = initial_value;
            initial_value = initial_value * last_value.inverse() * initial_value;
            last_value = temp;
            ROS_DEBUG("In no imu mode!");
        }
    }

    static Eigen::Matrix4f output = Eigen::Matrix4f::Identity();
    Eigen::Matrix4f save_initial_guess = initial_value;
    ndt_.setInputSource(filtered_current_pointcloud);
    pcl::PointCloud<PointType>::Ptr output_cloud(new pcl::PointCloud<PointType>);
    ndt_.align(initial_value);
    // ndt_.align(*output_cloud, initial_value);
    output = ndt_.getFinalTransformation();
    initial_value = output;

    // publish ndt odometry
    nav_msgs::Odometry ndt_initial_guess;
    static unsigned int count = 0;
    ndt_initial_guess.header.stamp = pointcloud_msg->header.stamp;
    ndt_initial_guess.header.frame_id = "map";
    ndt_initial_guess.header.seq = ++count;
    ndt_initial_guess.pose.pose.position.x = save_initial_guess(0,3);
    ndt_initial_guess.pose.pose.position.y = save_initial_guess(1,3);
    ndt_initial_guess.pose.pose.position.z = save_initial_guess(2,3);
    Eigen::Quaternionf quat_initial_guess(save_initial_guess.block<3, 3>(0, 0));
    ndt_initial_guess.pose.pose.orientation.w = quat_initial_guess.w();
    ndt_initial_guess.pose.pose.orientation.x = quat_initial_guess.x();
    ndt_initial_guess.pose.pose.orientation.y = quat_initial_guess.y();
    ndt_initial_guess.pose.pose.orientation.z = quat_initial_guess.z();
    pubNdtInitialGuess_.publish(ndt_initial_guess);

    // publish ndt odometry
    nav_msgs::Odometry ndt_odom;
    ndt_odom.header.stamp = pointcloud_msg->header.stamp;
    ndt_odom.header.frame_id = "map";
    ndt_odom.header.seq = count;
    ndt_odom.pose.pose.position.x = output(0,3);
    ndt_odom.pose.pose.position.y = output(1,3);
    ndt_odom.pose.pose.position.z = output(2,3);
    Eigen::Quaternionf quat_odom(output.block<3, 3>(0, 0));
    ndt_odom.pose.pose.orientation.w = quat_odom.w();
    ndt_odom.pose.pose.orientation.x = quat_odom.x();
    ndt_odom.pose.pose.orientation.y = quat_odom.y();
    ndt_odom.pose.pose.orientation.z = quat_odom.z();
    pubNdtOdometry.publish(ndt_odom);

    static tf::TransformBroadcaster broadcaster;
    tf::Quaternion quad(quat_odom.x(), quat_odom.y(), quat_odom.z(), quat_odom.w());
    tf::Transform trans;
    trans.setOrigin(tf::Vector3(output(0,3), output(1,3), output(2,3))); 
    trans.setRotation(quad);
    broadcaster.sendTransform(tf::StampedTransform(trans, pointcloud_msg->header.stamp, "map", lidarFrame_));

    double end_time = ros::WallTime::now().toSec();
    ROS_DEBUG_STREAM("current msg seq: " << pointcloud_msg->header.seq
        << " Normal Distributions Transform has converged: " << ndt_.hasConverged()
        << " score: " << ndt_.getFitnessScore() 
        << " iterations: " << ndt_.getFinalNumIteration() << "/" << ndt_.getMaximumIterations() 
        << " total time used: " << end_time - start_time);

    iteration_sum += ndt_.getFinalNumIteration();
    time_sum += end_time - start_time;
    num++;
    ROS_INFO_STREAM("avg time cosuming: " << time_sum/num << ", avg iterations: " << iteration_sum/num);
}

void NdtMatching::LoadGlobalMap(){
    if(map_path_.empty()){
        ROS_ERROR("Map path not set! cannot load global map in ndt module!");
        ros::shutdown();
    }
    if(pcl::io::loadPCDFile<PointType> (map_path_, *global_map_) == -1){
        ROS_ERROR("Cannot load global map in ndt module! map path: %s", map_path_.c_str());
        ros::shutdown();
    }
    ROS_INFO("Global Map has %ld points.", global_map_->size());

    // downsample
    pcl::ApproximateVoxelGrid<PointType> approximate_voxel_filter;
    approximate_voxel_filter.setLeafSize(0.2, 0.2, 0.2);
    approximate_voxel_filter.setInputCloud(global_map_);
    approximate_voxel_filter.filter(*global_map_downsample_);
    ROS_INFO("After downsample, global Map has %ld points.", global_map_->size());
    // global_map_downsample_.reset();
    // global_map_downsample_ = global_map_;


    ndt_.setInputTarget(global_map_);
    // pcl::PointCloud<PointType>::Ptr output_cloud(new pcl::PointCloud<PointType>);
    // ndt_.align(*output_cloud, Eigen::Matrix4f::Identity());
    map_load_ = true;

    sensor_msgs::PointCloud2 pc_to_pub;
    pcl::toROSMsg(*global_map_downsample_, pc_to_pub);
    pc_to_pub.header.frame_id = "map";

    ros::Rate rate(0.2);
    bool have_published = false;
    while(ros::ok()){
        if(pubGlobalMap.getNumSubscribers() != 0){
            if(have_published == false){   // rviz too slow !!!!
                pc_to_pub.header.stamp = ros::Time::now();
                pubGlobalMap.publish(pc_to_pub);
            }
            have_published = true;
        }
        else{
            have_published = false;
        }
        rate.sleep();
    }
}

/* 
// haven't finished
void NdtMatching::WritePoseToFile(const std::string& file_path_name, const std::unordered_map<int, Node>& nodes){
    // in TUM format
    std::ofstream outFile(file_path_name, std::ios::out);
    if(!outFile){
        ROS_ERROR_STREAM("Cannot write file : " << file_path_name);
        return;
    }
    for(int i = 0; i <nodes.size(); ++i){
        auto& node = nodes.at(i);
        outFile << node.time_stamp.toSec()
                << ' ' << node.opt_pose.position.x()
                << ' ' << node.opt_pose.position.y()
                << ' ' << node.opt_pose.position.z()
                << ' ' << node.opt_pose.rotation.x()
                << ' ' << node.opt_pose.rotation.y()
                << ' ' << node.opt_pose.rotation.z()
                << ' ' << node.opt_pose.rotation.w()
                << std::endl;
    }
    outFile.close();
}
*/