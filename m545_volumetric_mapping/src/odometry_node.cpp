/*
 * mapping_node.cpp
 *
 *  Created on: Sep 1, 2021
 *      Author: jelavice
 */
#include <open3d/Open3D.h>
#include <open3d/pipelines/registration/Registration.h>
#include "m545_volumetric_mapping/Parameters.hpp"
#include "m545_volumetric_mapping/frames.hpp"
#include "m545_volumetric_mapping/helpers.hpp"
#include "m545_volumetric_mapping/Mapper.hpp"

#include "open3d_conversions/open3d_conversions.h"
#include <ros/ros.h>

#include <sensor_msgs/PointCloud2.h>
#include <eigen_conversions/eigen_msg.h>
#include <geometry_msgs/TransformStamped.h>
#include <tf2_ros/transform_broadcaster.h>
#include <nav_msgs/Odometry.h>

open3d::geometry::PointCloud cloud;
open3d::geometry::PointCloud cloudPrev;
ros::NodeHandlePtr nh;
bool isNewCloudReceived = false;
ros::Time timestamp;
std::shared_ptr<tf2_ros::TransformBroadcaster> tfBroadcaster;
Eigen::Matrix4d curentTransformation = Eigen::Matrix4d::Identity();
namespace registration = open3d::pipelines::registration;
ros::Publisher refPub;
ros::Publisher targetPub;
ros::Publisher registeredPub;
ros::Publisher mapPub;

m545_mapping::IcpParameters params;
std::shared_ptr<registration::TransformationEstimation> icpObjective;
std::shared_ptr<m545_mapping::Mapper> mapper;
m545_mapping::MapperParameters mapperParams;

void publishCloud(const open3d::geometry::PointCloud &cloud, const std::string &frame_id, ros::Publisher &pub) {
	sensor_msgs::PointCloud2 msg;
	open3d_conversions::open3dToRos(cloud, msg, frame_id);
	msg.header.stamp = timestamp;
	pub.publish(msg);
}

geometry_msgs::Pose getPose(const Eigen::MatrixXd &T) {
	geometry_msgs::Pose pose;

	// Fill pose
	Eigen::Affine3d eigenTr;
	eigenTr.matrix() = T;
	tf::poseEigenToMsg(eigenTr, pose);

	return pose;
}

geometry_msgs::TransformStamped toRos(const Eigen::Matrix4d &Mat, const ros::Time &time, const std::string &frame,
		const std::string &childFrame) {

	geometry_msgs::TransformStamped transformStamped;
	transformStamped.header.stamp = time;
	transformStamped.header.frame_id = frame;
	transformStamped.child_frame_id = childFrame;
	const auto pose = getPose(Mat);
	transformStamped.transform.translation.x = pose.position.x;
	transformStamped.transform.translation.y = pose.position.y;
	transformStamped.transform.translation.z = pose.position.z;
	transformStamped.transform.rotation = pose.orientation;
	return transformStamped;
}

void cloudCallback(const sensor_msgs::PointCloud2ConstPtr &msg) {
	cloud.Clear();
	open3d_conversions::rosToOpen3d(msg, cloud, true);
	timestamp = msg->header.stamp;
	isNewCloudReceived = true;

	if (cloudPrev.IsEmpty()) {
		cloudPrev = cloud;
		return;
	}
	const m545_mapping::Timer timer;
	const Eigen::Matrix4d init = Eigen::Matrix4d::Identity();
	auto criteria = open3d::pipelines::registration::ICPConvergenceCriteria();
	criteria.max_iteration_ = params.maxNumIter_;
	open3d::geometry::AxisAlignedBoundingBox bbox;
	bbox.min_bound_ = params.cropBoxLowBound_;
	bbox.max_bound_ = params.cropBoxHighBound_;
	auto croppedCloud = cloud.Crop(bbox);
	auto downSampledCloud = croppedCloud->RandomDownSample(params.downSamplingRatio_);
	cloud = *downSampledCloud;
	if (params.icpObjective_ == m545_mapping::IcpObjective::PointToPlane) {
		m545_mapping::estimateNormals(params.kNNnormalEstimation_, &cloud);
		cloud.NormalizeNormals();
	}
	auto result = open3d::pipelines::registration::RegistrationICP(cloudPrev, cloud, params.maxCorrespondenceDistance_,
			init, *icpObjective, criteria);

//	std::cout << "Scan to scan matching finished \n";
//	std::cout << "Time elapsed: " << timer.elapsedMsec() << " msec \n";
//	std::cout << "Fitness: " << result.fitness_ << "\n";
//	std::cout << "RMSE: " << result.inlier_rmse_ << "\n";
//	std::cout << "Transform: " << result.transformation_ << "\n";
//	std::cout << "target size: " << cloud.points_.size() << std::endl;
//	std::cout << "reference size: " << cloudPrev.points_.size() << std::endl;
//	std::cout << "\n \n";
	if (result.fitness_ <= 1e-2) {
		return;
	}
	curentTransformation *= result.transformation_.inverse();
	{
		geometry_msgs::TransformStamped transformStamped = toRos(curentTransformation, timestamp,
				m545_mapping::frames::odomFrame, m545_mapping::frames::rangeSensorFrame);
		tfBroadcaster->sendTransform(transformStamped);

	}

	auto registeredCloud = cloudPrev;
	registeredCloud.Transform(result.transformation_);

//	mapper->addRangeMeasurement(cloud, timestamp);
//	const auto endTime2 = std::chrono::steady_clock::now();
//	const double nMsec2 = std::chrono::duration_cast<std::chrono::microseconds>(endTime2 - startTime).count() / 1e3;
//	std::cout << "Total time elapsed: " << nMsec2 << " msec \n";
	std::thread t([]() {
		if (mapper->isMatchingInProgress()) {
			return;
		}
		const m545_mapping::Timer timer;
		mapper->addRangeMeasurement(cloud, timestamp);
		std::cout << "Mapping step finished \n";
		std::cout << "Time elapsed: " << timer.elapsedMsec() << " msec \n";
		{
			geometry_msgs::TransformStamped transformStamped = toRos(mapper->getMapToOdom().matrix(), timestamp,
					m545_mapping::frames::mapFrame, m545_mapping::frames::odomFrame);
			tfBroadcaster->sendTransform(transformStamped);
		}
//		{
//			geometry_msgs::TransformStamped transformStamped = toRos(mapper->getMapToRangeSensor().matrix(), timestamp,
//					m545_mapping::frames::mapFrame, m545_mapping::frames::rangeSensorFrame+"_check");
//			tfBroadcaster->sendTransform(transformStamped);
//		}
		open3d::geometry::PointCloud map = mapper->getMap();
		publishCloud(map, m545_mapping::frames::mapFrame, mapPub);
	});
	t.detach();

	publishCloud(cloud, m545_mapping::frames::rangeSensorFrame, refPub);
	cloudPrev = cloud;

}

int main(int argc, char **argv) {
	ros::init(argc, argv, "lidar_odometry_node");
	nh.reset(new ros::NodeHandle("~"));
	tfBroadcaster.reset(new tf2_ros::TransformBroadcaster());
	const std::string cloudTopic = nh->param<std::string>("cloud_topic", "");
	ros::Subscriber cloudSub = nh->subscribe(cloudTopic, 100, &cloudCallback);

	refPub = nh->advertise<sensor_msgs::PointCloud2>("reference", 1, true);
	targetPub = nh->advertise<sensor_msgs::PointCloud2>("target", 1, true);
	registeredPub = nh->advertise<sensor_msgs::PointCloud2>("registered", 1, true);
	mapPub = nh->advertise<sensor_msgs::PointCloud2>("map", 1, true);

	const std::string paramFile = nh->param<std::string>("parameter_file_path", "");
	std::cout << "loading params from: " << paramFile << "\n";
	m545_mapping::loadParameters(paramFile, &params);
	icpObjective = m545_mapping::icpObjectiveFactory(params.icpObjective_);

	mapper = std::make_shared<m545_mapping::Mapper>();
	m545_mapping::loadParameters(paramFile, &mapperParams);
	mapper->setParameters(mapperParams);

//	ros::AsyncSpinner spinner(4); // Use 4 threads
//	spinner.start();
//	ros::waitForShutdown();
	ros::spin();

	return 0;
}

