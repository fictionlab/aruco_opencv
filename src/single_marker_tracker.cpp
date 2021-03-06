#include <opencv2/aruco.hpp>
#include <opencv2/calib3d.hpp>

#include <nodelet/nodelet.h>
#include <pluginlib/class_list_macros.h>
#include <ros/ros.h>

#include <cv_bridge/cv_bridge.h>
#include <dynamic_reconfigure/server.h>
#include <image_transport/camera_common.h>
#include <image_transport/image_transport.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>

#include <aruco_opencv/ArucoDetectorConfig.h>
#include <aruco_opencv/MarkerDetection.h>

namespace aruco_opencv {

inline geometry_msgs::Pose convert_rvec_tvec(const cv::Vec3d &rvec, const cv::Vec3d &tvec) {
  geometry_msgs::Pose pose_out;

  cv::Mat rot(3, 3, CV_64FC1);
  cv::Rodrigues(rvec, rot);

  tf2::Matrix3x3 tf_rot(rot.at<double>(0, 0), rot.at<double>(0, 1), rot.at<double>(0, 2),
                        rot.at<double>(1, 0), rot.at<double>(1, 1), rot.at<double>(1, 2),
                        rot.at<double>(2, 0), rot.at<double>(2, 1), rot.at<double>(2, 2));
  tf2::Quaternion tf_quat;
  tf_rot.getRotation(tf_quat);

  tf2::Vector3 tf_orig(tvec[0], tvec[1], tvec[2]);

  tf2::toMsg(tf_orig, pose_out.position);
  pose_out.orientation = tf2::toMsg(tf_quat);

  return pose_out;
}

class SingleMarkerTracker : public nodelet::Nodelet {

  // Parameters
  std::string cam_base_topic_;
  double marker_size_;
  int image_queue_size_;

  // ROS
  ros::Publisher detection_pub_;
  ros::Subscriber cam_info_sub_;
  bool cam_info_retrieved_ = false;
  image_transport::ImageTransport *it_;
  image_transport::ImageTransport *pit_;
  image_transport::Subscriber img_sub_;
  image_transport::Publisher debug_pub_;
  dynamic_reconfigure::Server<aruco_opencv::ArucoDetectorConfig> *dyn_srv_;

  // Aruco
  cv::Mat camera_matrix_;
  cv::Mat distortion_coeffs_;
  cv::Mat marker_obj_points_;
  cv::Ptr<cv::aruco::DetectorParameters> detector_parameters_;
  cv::Ptr<cv::aruco::Dictionary> dictionary_;

public:
  SingleMarkerTracker()
      : camera_matrix_(3, 3, CV_64FC1), distortion_coeffs_(4, 1, CV_64FC1),
        marker_obj_points_(4, 1, CV_32FC3) {}

private:
  void onInit() override {
    auto &nh = getNodeHandle();
    auto &pnh = getPrivateNodeHandle();

    retrieve_parameters(pnh);

    detector_parameters_ = cv::aruco::DetectorParameters::create();
    dictionary_ = cv::aruco::getPredefinedDictionary(cv::aruco::DICT_ARUCO_ORIGINAL);

    dyn_srv_ = new dynamic_reconfigure::Server<aruco_opencv::ArucoDetectorConfig>(pnh);
    dyn_srv_->setCallback(boost::bind(&SingleMarkerTracker::reconfigure_callback, this, _1, _2));

    // set coordinate system in the middle of the marker, with Z pointing out
    marker_obj_points_.ptr<cv::Vec3f>(0)[0] = cv::Vec3f(-marker_size_ / 2.f, marker_size_ / 2.f, 0);
    marker_obj_points_.ptr<cv::Vec3f>(0)[1] = cv::Vec3f(marker_size_ / 2.f, marker_size_ / 2.f, 0);
    marker_obj_points_.ptr<cv::Vec3f>(0)[2] = cv::Vec3f(marker_size_ / 2.f, -marker_size_ / 2.f, 0);
    marker_obj_points_.ptr<cv::Vec3f>(0)[3] =
        cv::Vec3f(-marker_size_ / 2.f, -marker_size_ / 2.f, 0);

    it_ = new image_transport::ImageTransport(nh);
    pit_ = new image_transport::ImageTransport(pnh);

    detection_pub_ = nh.advertise<aruco_opencv::MarkerDetection>("marker_detections", 5);
    debug_pub_ = pit_->advertise("debug", 1);

    NODELET_INFO("Waiting for camera info...");

    std::string cam_info_topic = image_transport::getCameraInfoTopic(cam_base_topic_);
    cam_info_sub_ =
        nh.subscribe(cam_info_topic, 1, &SingleMarkerTracker::callback_camera_info, this);

    img_sub_ = it_->subscribe(cam_base_topic_, image_queue_size_,
                              &SingleMarkerTracker::callback_image, this);
  }

  void retrieve_parameters(ros::NodeHandle &pnh) {
    pnh.param<std::string>("cam_base_topic", cam_base_topic_, "camera/image_raw");
    pnh.param<double>("marker_size", marker_size_, 0.15);
    pnh.param<int>("image_queue_size", image_queue_size_, 5);
  }

  void reconfigure_callback(aruco_opencv::ArucoDetectorConfig &config, uint32_t level) {
    if (config.adaptiveThreshWinSizeMax < config.adaptiveThreshWinSizeMin)
      config.adaptiveThreshWinSizeMax = config.adaptiveThreshWinSizeMin;

    if (config.maxMarkerPerimeterRate < config.minMarkerPerimeterRate)
      config.maxMarkerPerimeterRate = config.minMarkerPerimeterRate;

    detector_parameters_->adaptiveThreshWinSizeMin = config.adaptiveThreshWinSizeMin;
    detector_parameters_->adaptiveThreshWinSizeMax = config.adaptiveThreshWinSizeMax;
    detector_parameters_->adaptiveThreshWinSizeStep = config.adaptiveThreshWinSizeStep;
    detector_parameters_->adaptiveThreshConstant = config.adaptiveThreshConstant;
    detector_parameters_->minMarkerPerimeterRate = config.minMarkerPerimeterRate;
    detector_parameters_->maxMarkerPerimeterRate = config.maxMarkerPerimeterRate;
    detector_parameters_->polygonalApproxAccuracyRate = config.polygonalApproxAccuracyRate;
    detector_parameters_->minCornerDistanceRate = config.minCornerDistanceRate;
    detector_parameters_->minDistanceToBorder = config.minDistanceToBorder;
    detector_parameters_->minMarkerDistanceRate = config.minMarkerDistanceRate;
    detector_parameters_->markerBorderBits = config.markerBorderBits;
    detector_parameters_->perspectiveRemovePixelPerCell = config.perspectiveRemovePixelPerCell;
    detector_parameters_->perspectiveRemoveIgnoredMarginPerCell =
        config.perspectiveRemoveIgnoredMarginPerCell;
    detector_parameters_->maxErroneousBitsInBorderRate = config.maxErroneousBitsInBorderRate;
    detector_parameters_->minOtsuStdDev = config.minOtsuStdDev;
    detector_parameters_->errorCorrectionRate = config.errorCorrectionRate;
    detector_parameters_->cornerRefinementMethod = config.cornerRefinementMethod;
    detector_parameters_->cornerRefinementWinSize = config.cornerRefinementWinSize;
    detector_parameters_->cornerRefinementMaxIterations = config.cornerRefinementMaxIterations;
    detector_parameters_->cornerRefinementMinAccuracy = config.cornerRefinementMinAccuracy;
  }

  void callback_camera_info(const sensor_msgs::CameraInfo &cam_info) {
    NODELET_INFO("Camera info retrieved.");

    cv::Mat cameraMatrixFromK(3, 3, CV_64FC1, 0.0);
    for (int i = 0; i < 9; ++i)
      cameraMatrixFromK.at<double>(i % 3, i - (i % 3) * 3) = cam_info.K[i];
    cameraMatrixFromK.copyTo(camera_matrix_(cv::Rect(0, 0, 3, 3)));

    for (int i = 0; i < 4; ++i)
      distortion_coeffs_.at<double>(i, 0) = cam_info.D[i];

    cam_info_retrieved_ = true;
    cam_info_sub_.shutdown();
  }

  void callback_image(const sensor_msgs::ImageConstPtr &img_msg) {
    if (!cam_info_retrieved_)
      return;

    auto callback_start_time = ros::Time::now();

    // Convert the image
    auto cv_ptr = cv_bridge::toCvShare(img_msg);

    std::vector<int> marker_ids;
    std::vector<std::vector<cv::Point2f>> marker_corners;

    cv::aruco::detectMarkers(cv_ptr->image, dictionary_, marker_corners, marker_ids,
                             detector_parameters_);

    int n_markers = marker_ids.size();
    std::vector<cv::Vec3d> rvec_final(n_markers), tvec_final(n_markers);

    aruco_opencv::MarkerDetection detection;
    detection.header.frame_id = img_msg->header.frame_id;
    detection.header.stamp = img_msg->header.stamp;

    for (size_t i = 0; i < n_markers; i++) {
      int id = marker_ids[i];

      std::vector<cv::Vec3d> rvecs(n_markers), tvecs(n_markers);

      cv::solvePnPGeneric(marker_obj_points_, marker_corners[i], camera_matrix_, distortion_coeffs_,
                          rvecs, tvecs, false, cv::SOLVEPNP_IPPE_SQUARE);

      // Choose the solution with lower reprojection error
      rvec_final[i] = rvecs[0];
      tvec_final[i] = tvecs[0];

      MarkerPose mpose;
      mpose.marker_id = id;
      mpose.pose = convert_rvec_tvec(rvec_final[i], tvec_final[i]);
      detection.markers.push_back(mpose);
    }

    detection_pub_.publish(detection);

    if (debug_pub_.getNumSubscribers() > 0) {
      auto debug_cv_ptr = cv_bridge::toCvCopy(img_msg, "bgr8");
      cv::aruco::drawDetectedMarkers(debug_cv_ptr->image, marker_corners, marker_ids);
      for (size_t i = 0; i < marker_ids.size(); i++) {
        cv::drawFrameAxes(debug_cv_ptr->image, camera_matrix_, distortion_coeffs_, rvec_final[i],
                          tvec_final[i], 0.2, 3);
      }
      debug_pub_.publish(debug_cv_ptr->toImageMsg());
    }

    auto callback_end_time = ros::Time::now();
    double whole_callback_duration = (callback_end_time - callback_start_time).toSec();
    double image_send_duration = (callback_start_time - img_msg->header.stamp).toSec();

    NODELET_DEBUG("Image callback completed. The callback started %.4f s after "
                  "the image frame was "
                  "grabbed and completed its execution in %.4f s.",
                  image_send_duration, whole_callback_duration);
  }
};

} // namespace aruco_opencv

PLUGINLIB_EXPORT_CLASS(aruco_opencv::SingleMarkerTracker, nodelet::Nodelet)
