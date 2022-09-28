#include "imu_utility.h"
#include "ndt_utility.h"

#include <thread>

int main(int argc, char** argv){
    ros::init(argc, argv, "ndt_imu_localization");
    ros::console::set_logger_level(ROSCONSOLE_DEFAULT_NAME, ros::console::levels::Debug);

    ROS_INFO("\033[1;32m----> NDT IMU Localization Started.\033[0m");

    NdtMatching ndt_matching;
    std::thread global_map_thread(&NdtMatching::LoadGlobalMap, &ndt_matching);
    // ImuPreintegration imu_preintegration;
    
    ros::MultiThreadedSpinner spinner(4);
    spinner.spin();
    global_map_thread.join();
}