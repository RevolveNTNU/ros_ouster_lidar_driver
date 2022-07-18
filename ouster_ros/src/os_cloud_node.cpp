/**
 * @file
 * @brief Example node to publish point clouds and imu topics
 */

#include <rdv_msgs/PpsCounterReset.h>
#include <ros/console.h>
#include <ros/ros.h>
#include <ros/service.h>
#include <sensor_msgs/Imu.h>
#include <sensor_msgs/PointCloud2.h>
#include <tf2_ros/static_transform_broadcaster.h>

#include <algorithm>
#include <chrono>
#include <memory>

#include "ouster/lidar_scan.h"
#include "ouster/types.h"
#include "ouster_ros/OSConfigSrv.h"
#include "ouster_ros/PacketMsg.h"
#include "ouster_ros/ros.h"
#include "std_srvs/Trigger.h"

using PacketMsg = ouster_ros::PacketMsg;
using Cloud = ouster_ros::Cloud;
using Point = ouster_ros::Point;
namespace sensor = ouster::sensor;

int main(int argc, char** argv) {
    ros::init(argc, argv, "os_cloud_node");
    ros::NodeHandle nh("~");

    auto tf_prefix = nh.param("tf_prefix", std::string{});
    if (!tf_prefix.empty() && tf_prefix.back() != '/') tf_prefix.append("/");
    auto sensor_frame = tf_prefix + "os_sensor";
    auto imu_frame = tf_prefix + "imu_1";
    auto lidar_frame = tf_prefix + "lidar_0";

    ouster_ros::OSConfigSrv cfg{};
    auto client = nh.serviceClient<ouster_ros::OSConfigSrv>("os_config");
    client.waitForExistence();
    if (!client.call(cfg)) {
        ROS_ERROR("Calling config service failed");
        return EXIT_FAILURE;
    }

    auto info = sensor::parse_metadata(cfg.response.metadata);
    uint32_t H = info.format.pixels_per_column;
    uint32_t W = info.format.columns_per_frame;
    auto udp_profile_lidar = info.format.udp_profile_lidar;

    const int n_returns =
        (udp_profile_lidar == sensor::UDPProfileLidar::PROFILE_LIDAR_LEGACY)
            ? 1
            : 2;
    auto pf = sensor::get_format(info);

    auto imu_pub = nh.advertise<sensor_msgs::Imu>("/sensor/imu_1", 100);

    // auto img_suffix = [](int ind) {
    //     if (ind == 0) return std::string();
    //     return std::to_string(ind + 1);  // need second return to return 2
    // };

    auto lidar_pubs = std::vector<ros::Publisher>();
    for (int i = 0; i < n_returns; i++) {
        auto pub =
            nh.advertise<sensor_msgs::PointCloud2>("/sensor/lidar_0", 10);
        lidar_pubs.push_back(pub);
    }

    auto xyz_lut = ouster::make_xyz_lut(info);

    ouster::LidarScan ls{W, H, udp_profile_lidar};
    Cloud cloud{W, H};

    ouster::ScanBatcher batch(W, pf);

    TimestampTranslator timestamp_translator{
        {std::chrono::seconds{2}, 1,
         TimestampTranslator::Method::kPpsToSystemClock}};
    ros::ServiceClient pps_reset_client =
        nh.serviceClient<rdv_msgs::PpsCounterReset>(
            "/vehicle_interface/reset_pps_counter");
    bool has_reset_pps_counter{false};

    auto trigger_reset_pps_second_counter =
        [&](std_srvs::Trigger::Request&, std_srvs::Trigger::Response& res) {
            has_reset_pps_counter = false;
            res.success = true;
            return static_cast<bool>(res.success);
        };

    ros::ServiceServer pps_reset_client_trigger =
        nh.advertiseService<std_srvs::Trigger::Request,
                            std_srvs::Trigger::Response>(
            "/lidar_driver/reset_pps_counter_trigger",
            trigger_reset_pps_second_counter);

    auto lidar_handler = [&](const PacketMsg& pm) mutable {
        using namespace std::chrono_literals;
        if (batch(pm.buf.data(), ls)) {
            auto h = std::find_if(
                ls.headers.begin(), ls.headers.end(), [](const auto& h) {
                    return h.timestamp != std::chrono::nanoseconds{0};
                });
            if (!has_reset_pps_counter && 300ms < h->timestamp &&
                h->timestamp < 500ms) {
                if (pps_reset_client.exists()) {
                    rdv_msgs::PpsCounterReset srv;
                    if (pps_reset_client.call(srv)) {
                        timestamp_translator.resetPpsSecondCounter(
                            std::chrono::nanoseconds{
                                srv.response.time_of_reset});
                        has_reset_pps_counter = true;
                        ROS_INFO("PPS second counter reset successful");
                    }
                }
            }
            if (h != ls.headers.end()) {
                for (int i = 0; i < n_returns; i++) {
                    scan_to_cloud(xyz_lut, h->timestamp, ls, cloud, i);
                    lidar_pubs[i].publish(ouster_ros::cloud_to_cloud_msg(
                        cloud, h->timestamp, sensor_frame,
                        timestamp_translator));
                }
            }
        }
    };

    auto imu_handler = [&](const PacketMsg& p) {
        imu_pub.publish(ouster_ros::packet_to_imu_msg(p, imu_frame, pf));
    };

    auto lidar_packet_sub = nh.subscribe<PacketMsg, const PacketMsg&>(
        "lidar_packets", 2048, lidar_handler);
    auto imu_packet_sub = nh.subscribe<PacketMsg, const PacketMsg&>(
        "imu_packets", 100, imu_handler);

    // publish transforms
    tf2_ros::StaticTransformBroadcaster tf_bcast{};

    tf_bcast.sendTransform(ouster_ros::transform_to_tf_msg(
        info.imu_to_sensor_transform, sensor_frame, imu_frame));

    tf_bcast.sendTransform(ouster_ros::transform_to_tf_msg(
        info.lidar_to_sensor_transform, sensor_frame, lidar_frame));

    ros::spin();

    return EXIT_SUCCESS;
}
