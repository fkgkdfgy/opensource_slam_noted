#include "utility.h"
#include "lio_sam/cloud_info.h"

#include <gtsam/geometry/Rot3.h>
#include <gtsam/geometry/Pose3.h>
#include <gtsam/slam/PriorFactor.h>
#include <gtsam/slam/BetweenFactor.h>
#include <gtsam/navigation/GPSFactor.h>
#include <gtsam/navigation/ImuFactor.h>
#include <gtsam/navigation/CombinedImuFactor.h>
#include <gtsam/nonlinear/NonlinearFactorGraph.h>
#include <gtsam/nonlinear/LevenbergMarquardtOptimizer.h>
#include <gtsam/nonlinear/Marginals.h>
#include <gtsam/nonlinear/Values.h>
#include <gtsam/inference/Symbol.h>

#include <gtsam/nonlinear/ISAM2.h>

using namespace gtsam;

using symbol_shorthand::X; // Pose3 (x,y,z,r,p,y)
using symbol_shorthand::V; // Vel   (xdot,ydot,zdot)
using symbol_shorthand::B; // Bias  (ax,ay,az,gx,gy,gz)
using symbol_shorthand::G; // GPS pose

/*
    * A point cloud type that has 6D pose info ([x,y,z,roll,pitch,yaw] intensity is time stamp)
    */
struct PointXYZIRPYT {
  PCL_ADD_POINT4D

  PCL_ADD_INTENSITY;                  // preferred way of adding a XYZ+padding
  float roll;
  float pitch;
  float yaw;
  double time;

  EIGEN_MAKE_ALIGNED_OPERATOR_NEW   // make sure our new allocators are aligned
} EIGEN_ALIGN16;                    // enforce SSE padding for correct memory alignment

POINT_CLOUD_REGISTER_POINT_STRUCT (PointXYZIRPYT,
                                   (float, x, x)(float, y, y)
                                       (float, z, z)(float, intensity, intensity)
                                       (float, roll, roll)(float, pitch, pitch)(float, yaw, yaw)
                                       (double, time, time))

typedef PointXYZIRPYT PointTypePose;

class mapOptimization : public ParamServer {

 public:

  mapOptimization() {
    ISAM2Params parameters;
    parameters.relinearizeThreshold = 0.1;
    parameters.relinearizeSkip = 1;
    isam = new ISAM2(parameters);

    // subscriber 主要订阅分类好的cloud_info以及gps,用于后端优化和回环检测,
    // 注意gps接受的是nav_msgs::Odometry消息, 是通过robot_localization节点融合imu和gps数据得到的
    //  callback里面只是装数据到队列
    subLaserCloudInfo = nh.subscribe<lio_sam::cloud_info>("lio_sam/feature/cloud_info", 1,
                                                          &mapOptimization::laserCloudInfoHandler, this,
                                                          ros::TransportHints().tcpNoDelay());
    subGPS = nh.subscribe<nav_msgs::Odometry>(gpsTopic, 200, &mapOptimization::gpsHandler, this,
                                              ros::TransportHints().tcpNoDelay());

    // publisher 发布一些odometry之类的
    pubKeyPoses = nh.advertise<sensor_msgs::PointCloud2>("lio_sam/mapping/trajectory", 1);
    pubLaserCloudSurround = nh.advertise<sensor_msgs::PointCloud2>("lio_sam/mapping/map_global", 1); // 全局地图
    pubOdomAftMappedROS = nh.advertise<nav_msgs::Odometry>("lio_sam/mapping/odometry", 1); // 优化后的odom
    pubPath = nh.advertise<nav_msgs::Path>("lio_sam/mapping/path", 1);

    // 回环检测相关的一些历史帧
    pubHistoryKeyFrames = nh.advertise<sensor_msgs::PointCloud2>("lio_sam/mapping/icp_loop_closure_history_cloud", 1);
    pubIcpKeyFrames = nh.advertise<sensor_msgs::PointCloud2>("lio_sam/mapping/icp_loop_closure_corrected_cloud", 1);

    //  local map
    pubRecentKeyFrames = nh.advertise<sensor_msgs::PointCloud2>("lio_sam/mapping/map_local", 1);
    pubRecentKeyFrame = nh.advertise<sensor_msgs::PointCloud2>("lio_sam/mapping/cloud_registered", 1);
    pubCloudRegisteredRaw = nh.advertise<sensor_msgs::PointCloud2>("lio_sam/mapping/cloud_registered_raw", 1);

    // 不同的特征进行滤波
    downSizeFilterCorner.setLeafSize(mappingCornerLeafSize, mappingCornerLeafSize, mappingCornerLeafSize);
    downSizeFilterSurf.setLeafSize(mappingSurfLeafSize, mappingSurfLeafSize, mappingSurfLeafSize);
    downSizeFilterICP.setLeafSize(mappingSurfLeafSize, mappingSurfLeafSize, mappingSurfLeafSize);
    downSizeFilterSurroundingKeyPoses.setLeafSize(surroundingKeyframeDensity, surroundingKeyframeDensity,
                                                  surroundingKeyframeDensity); // for surrounding key poses of scan-to-map optimization

    allocateMemory();
  }

  void allocateMemory() {
    // 初始化一些参数
    cloudKeyPoses3D.reset(new pcl::PointCloud<PointType>());
    cloudKeyPoses6D.reset(new pcl::PointCloud<PointTypePose>());

    kdtreeSurroundingKeyPoses.reset(new pcl::KdTreeFLANN<PointType>());
    kdtreeHistoryKeyPoses.reset(new pcl::KdTreeFLANN<PointType>());

    laserCloudCornerLast.reset(new pcl::PointCloud<PointType>()); // corner feature set from odoOptimization
    laserCloudSurfLast.reset(new pcl::PointCloud<PointType>()); // surf feature set from odoOptimization
    laserCloudCornerLastDS.reset(
        new pcl::PointCloud<PointType>()); // downsampled corner featuer set from odoOptimization
    laserCloudSurfLastDS.reset(
        new pcl::PointCloud<PointType>()); // downsampled surf featuer set from odoOptimization

    laserCloudOri.reset(new pcl::PointCloud<PointType>());
    coeffSel.reset(new pcl::PointCloud<PointType>());

    laserCloudOriCornerVec.resize(N_SCAN * Horizon_SCAN);
    coeffSelCornerVec.resize(N_SCAN * Horizon_SCAN);
    laserCloudOriCornerFlag.resize(N_SCAN * Horizon_SCAN);
    laserCloudOriSurfVec.resize(N_SCAN * Horizon_SCAN);
    coeffSelSurfVec.resize(N_SCAN * Horizon_SCAN);
    laserCloudOriSurfFlag.resize(N_SCAN * Horizon_SCAN);

    std::fill(laserCloudOriCornerFlag.begin(), laserCloudOriCornerFlag.end(), false);
    std::fill(laserCloudOriSurfFlag.begin(), laserCloudOriSurfFlag.end(), false);

    laserCloudCornerFromMap.reset(new pcl::PointCloud<PointType>());
    laserCloudSurfFromMap.reset(new pcl::PointCloud<PointType>());
    laserCloudCornerFromMapDS.reset(new pcl::PointCloud<PointType>());
    laserCloudSurfFromMapDS.reset(new pcl::PointCloud<PointType>());

    kdtreeCornerFromMap.reset(new pcl::KdTreeFLANN<PointType>());
    kdtreeSurfFromMap.reset(new pcl::KdTreeFLANN<PointType>());

    latestKeyFrameCloud.reset(new pcl::PointCloud<PointType>());
    nearHistoryKeyFrameCloud.reset(new pcl::PointCloud<PointType>());

    for (int i = 0; i < 6; ++i) {
      transformTobeMapped[i] = 0;
    }

    matP.setZero();
  }

  void laserCloudInfoHandler(const lio_sam::cloud_infoConstPtr &msgIn) {
    // extract time stamp
    timeLaserInfoStamp = msgIn->header.stamp;
    timeLaserCloudInfoLast = msgIn->header.stamp.toSec();

    // extract info and feature cloud
    // corner和surface点云
    cloudInfo = *msgIn;
    pcl::fromROSMsg(msgIn->cloud_corner, *laserCloudCornerLast);
    pcl::fromROSMsg(msgIn->cloud_surface, *laserCloudSurfLast);

    std::lock_guard<std::mutex> lock(mtx);

    // 0.15s更新一下map
    if (timeLaserCloudInfoLast - timeLastProcessing >= mappingProcessInterval) {

      timeLastProcessing = timeLaserCloudInfoLast;

      updateInitialGuess(); // imu预积分更新初始位姿

      extractSurroundingKeyFrames(); // 从关键帧里面提取附近回环候选帧

      downsampleCurrentScan();  // 不同的leaf size进行下采样，主要是corner cloud和surface cloud

      scan2MapOptimization(); // 构建点到平面、点到直线的残差, 用高斯牛顿法进行优化

      saveKeyFramesAndFactor(); // 添加factor,保存key pose之类的

      correctPoses();  // 更新位姿

      // publish odom
      publishOdometry(); // 发布增量平滑后的odom

      publishFrames();  // 发布一些关键帧点云之类的
    }
  }

  void gpsHandler(const nav_msgs::Odometry::ConstPtr &gpsMsg) {
    gpsQueue.push_back(*gpsMsg);
  }

  void pointAssociateToMap(PointType const *const pi, PointType *const po) {
    po->x = transPointAssociateToMap(0, 0) * pi->x + transPointAssociateToMap(0, 1) * pi->y +
        transPointAssociateToMap(0, 2) * pi->z + transPointAssociateToMap(0, 3);
    po->y = transPointAssociateToMap(1, 0) * pi->x + transPointAssociateToMap(1, 1) * pi->y +
        transPointAssociateToMap(1, 2) * pi->z + transPointAssociateToMap(1, 3);
    po->z = transPointAssociateToMap(2, 0) * pi->x + transPointAssociateToMap(2, 1) * pi->y +
        transPointAssociateToMap(2, 2) * pi->z + transPointAssociateToMap(2, 3);
    po->intensity = pi->intensity;
  }

  pcl::PointCloud<PointType>::Ptr
  transformPointCloud(pcl::PointCloud<PointType>::Ptr cloudIn, PointTypePose *transformIn) {
    pcl::PointCloud<PointType>::Ptr cloudOut(new pcl::PointCloud<PointType>());

    PointType *pointFrom;

    int cloudSize = cloudIn->size();
    cloudOut->resize(cloudSize);

    Eigen::Affine3f transCur = pcl::getTransformation(transformIn->x, transformIn->y, transformIn->z,
                                                      transformIn->roll, transformIn->pitch, transformIn->yaw);

    for (int i = 0; i < cloudSize; ++i) {

      pointFrom = &cloudIn->points[i];
      cloudOut->points[i].x =
          transCur(0, 0) * pointFrom->x + transCur(0, 1) * pointFrom->y + transCur(0, 2) * pointFrom->z +
              transCur(0, 3);
      cloudOut->points[i].y =
          transCur(1, 0) * pointFrom->x + transCur(1, 1) * pointFrom->y + transCur(1, 2) * pointFrom->z +
              transCur(1, 3);
      cloudOut->points[i].z =
          transCur(2, 0) * pointFrom->x + transCur(2, 1) * pointFrom->y + transCur(2, 2) * pointFrom->z +
              transCur(2, 3);
      cloudOut->points[i].intensity = pointFrom->intensity;
    }
    return cloudOut;
  }

  gtsam::Pose3 pclPointTogtsamPose3(PointTypePose thisPoint) {
    return gtsam::Pose3(gtsam::Rot3::RzRyRx(double(thisPoint.roll), double(thisPoint.pitch), double(thisPoint.yaw)),
                        gtsam::Point3(double(thisPoint.x), double(thisPoint.y), double(thisPoint.z)));
  }

  gtsam::Pose3 trans2gtsamPose(float transformIn[]) {
    return gtsam::Pose3(gtsam::Rot3::RzRyRx(transformIn[0], transformIn[1], transformIn[2]),
                        gtsam::Point3(transformIn[3], transformIn[4], transformIn[5]));
  }

  Eigen::Affine3f pclPointToAffine3f(PointTypePose thisPoint) {
    return pcl::getTransformation(thisPoint.x, thisPoint.y, thisPoint.z, thisPoint.roll, thisPoint.pitch,
                                  thisPoint.yaw);
  }

  Eigen::Affine3f trans2Affine3f(float transformIn[]) {
    return pcl::getTransformation(transformIn[3], transformIn[4], transformIn[5], transformIn[0], transformIn[1],
                                  transformIn[2]);
  }

  PointTypePose trans2PointTypePose(float transformIn[]) {
    PointTypePose thisPose6D;
    thisPose6D.x = transformIn[3];
    thisPose6D.y = transformIn[4];
    thisPose6D.z = transformIn[5];
    thisPose6D.roll = transformIn[0];
    thisPose6D.pitch = transformIn[1];
    thisPose6D.yaw = transformIn[2];
    return thisPose6D;
  }

  void visualizeGlobalMapThread() {
    // 按一定的频率发布全局地图
    ros::Rate rate(0.2);
    while (ros::ok()) {
      rate.sleep();
      publishGlobalMap();
    }

    //  下面是保存各种地图
    if (savePCD == false)
      return;

    cout << "****************************************************" << endl;
    cout << "Saving map to pcd files ..." << endl;
    // create directory and remove old files;
    savePCDDirectory = std::getenv("HOME") + savePCDDirectory;
    int unused = system((std::string("exec rm -r ") + savePCDDirectory).c_str());
    unused = system((std::string("mkdir ") + savePCDDirectory).c_str());
    // save key frame transformations
    pcl::io::savePCDFileASCII(savePCDDirectory + "trajectory.pcd", *cloudKeyPoses3D);
    pcl::io::savePCDFileASCII(savePCDDirectory + "transformations.pcd", *cloudKeyPoses6D);
    // extract global point cloud map
    pcl::PointCloud<PointType>::Ptr globalCornerCloud(new pcl::PointCloud<PointType>());
    pcl::PointCloud<PointType>::Ptr globalCornerCloudDS(new pcl::PointCloud<PointType>());
    pcl::PointCloud<PointType>::Ptr globalSurfCloud(new pcl::PointCloud<PointType>());
    pcl::PointCloud<PointType>::Ptr globalSurfCloudDS(new pcl::PointCloud<PointType>());
    pcl::PointCloud<PointType>::Ptr globalMapCloud(new pcl::PointCloud<PointType>());
    for (int i = 0; i < cloudKeyPoses3D->size(); i++) {
      *globalCornerCloud += *transformPointCloud(cornerCloudKeyFrames[i], &cloudKeyPoses6D->points[i]);
      *globalSurfCloud += *transformPointCloud(surfCloudKeyFrames[i], &cloudKeyPoses6D->points[i]);
      cout << "\r" << std::flush << "Processing feature cloud " << i << " of " << cloudKeyPoses6D->size()
           << " ...";
    }
    // down-sample and save corner cloud
    downSizeFilterCorner.setInputCloud(globalCornerCloud);
    downSizeFilterCorner.filter(*globalCornerCloudDS);
    pcl::io::savePCDFileASCII(savePCDDirectory + "cloudCorner.pcd", *globalCornerCloudDS);
    // down-sample and save surf cloud
    downSizeFilterSurf.setInputCloud(globalSurfCloud);
    downSizeFilterSurf.filter(*globalSurfCloudDS);
    pcl::io::savePCDFileASCII(savePCDDirectory + "cloudSurf.pcd", *globalSurfCloudDS);
    // down-sample and save global point cloud map
    *globalMapCloud += *globalCornerCloud;
    *globalMapCloud += *globalSurfCloud;
    pcl::io::savePCDFileASCII(savePCDDirectory + "cloudGlobal.pcd", *globalMapCloud);
    cout << "****************************************************" << endl;
    cout << "Saving map to pcd files completed" << endl;
  }

  void publishGlobalMap() {
    if (pubLaserCloudSurround.getNumSubscribers() == 0)
      return;

    // cloudKeyPoses3Dc存的是关键帧的位姿
    if (cloudKeyPoses3D->points.empty() == true)
      return;

    pcl::KdTreeFLANN<PointType>::Ptr kdtreeGlobalMap(new pcl::KdTreeFLANN<PointType>());;
    pcl::PointCloud<PointType>::Ptr globalMapKeyPoses(new pcl::PointCloud<PointType>());
    pcl::PointCloud<PointType>::Ptr globalMapKeyPosesDS(new pcl::PointCloud<PointType>());
    pcl::PointCloud<PointType>::Ptr globalMapKeyFrames(new pcl::PointCloud<PointType>());
    pcl::PointCloud<PointType>::Ptr globalMapKeyFramesDS(new pcl::PointCloud<PointType>());

    // kd-tree to find near key frames to visualize
    std::vector<int> pointSearchIndGlobalMap;
    std::vector<float> pointSearchSqDisGlobalMap;
    // search near key frames to visualize
    mtx.lock();
    kdtreeGlobalMap->setInputCloud(cloudKeyPoses3D);
    kdtreeGlobalMap->radiusSearch(cloudKeyPoses3D->back(), globalMapVisualizationSearchRadius,
                                  pointSearchIndGlobalMap, pointSearchSqDisGlobalMap, 0);
    mtx.unlock();

    // 找到附近的点云帧并发布出来
    for (int i = 0; i < pointSearchIndGlobalMap.size(); ++i)
      globalMapKeyPoses->push_back(cloudKeyPoses3D->points[pointSearchIndGlobalMap[i]]);
    // downsample near selected key frames
    pcl::VoxelGrid<PointType> downSizeFilterGlobalMapKeyPoses; // for global map visualization
    downSizeFilterGlobalMapKeyPoses.setLeafSize(globalMapVisualizationPoseDensity,
                                                globalMapVisualizationPoseDensity,
                                                globalMapVisualizationPoseDensity); // for global map visualization
    downSizeFilterGlobalMapKeyPoses.setInputCloud(globalMapKeyPoses);
    downSizeFilterGlobalMapKeyPoses.filter(*globalMapKeyPosesDS);

    // extract visualized and downsampled key frames
    for (int i = 0; i < globalMapKeyPosesDS->size(); ++i) {
      if (pointDistance(globalMapKeyPosesDS->points[i], cloudKeyPoses3D->back()) >
          globalMapVisualizationSearchRadius)
        continue;
      int thisKeyInd = (int) globalMapKeyPosesDS->points[i].intensity;
      *globalMapKeyFrames += *transformPointCloud(cornerCloudKeyFrames[thisKeyInd],
                                                  &cloudKeyPoses6D->points[thisKeyInd]);
      *globalMapKeyFrames += *transformPointCloud(surfCloudKeyFrames[thisKeyInd],
                                                  &cloudKeyPoses6D->points[thisKeyInd]);
    }
    // downsample visualized points
    pcl::VoxelGrid<PointType> downSizeFilterGlobalMapKeyFrames; // for global map visualization
    downSizeFilterGlobalMapKeyFrames.setLeafSize(globalMapVisualizationLeafSize, globalMapVisualizationLeafSize,
                                                 globalMapVisualizationLeafSize); // for global map visualization
    downSizeFilterGlobalMapKeyFrames.setInputCloud(globalMapKeyFrames);
    downSizeFilterGlobalMapKeyFrames.filter(*globalMapKeyFramesDS);
    publishCloud(&pubLaserCloudSurround, globalMapKeyFramesDS, timeLaserInfoStamp, "odom");
  }

  void loopClosureThread() {
    //  什么时候才进行回环检测？
    if (loopClosureEnableFlag == false)
      return;

    // 以一定的频率执行回环检测
    ros::Rate rate(0.2);
    while (ros::ok()) {
      rate.sleep();
      performLoopClosure();
    }
  }

  bool detectLoopClosure(int *latestID, int *closestID) {
    int latestFrameIDLoopCloure;
    int closestHistoryFrameID;

    latestKeyFrameCloud->clear();
    nearHistoryKeyFrameCloud->clear();

    std::lock_guard<std::mutex> lock(mtx);

    // find the closest history key frame
    std::vector<int> pointSearchIndLoop;
    std::vector<float> pointSearchSqDisLoop;
    kdtreeHistoryKeyPoses->setInputCloud(cloudKeyPoses3D);
    kdtreeHistoryKeyPoses->radiusSearch(cloudKeyPoses3D->back(), historyKeyframeSearchRadius, pointSearchIndLoop,
                                        pointSearchSqDisLoop, 0);

    //  两帧时间差也满足最小要求
    closestHistoryFrameID = -1;
    for (int i = 0; i < pointSearchIndLoop.size(); ++i) {
      int id = pointSearchIndLoop[i];
      if (abs(cloudKeyPoses6D->points[id].time - timeLaserCloudInfoLast) > historyKeyframeSearchTimeDiff) {
        closestHistoryFrameID = id;
        break;
      }
    }

    if (closestHistoryFrameID == -1)
      return false;

    if (cloudKeyPoses3D->size() - 1 == closestHistoryFrameID)
      return false;

    // save latest key frames
    latestFrameIDLoopCloure = cloudKeyPoses3D->size() - 1;
    *latestKeyFrameCloud += *transformPointCloud(cornerCloudKeyFrames[latestFrameIDLoopCloure],
                                                 &cloudKeyPoses6D->points[latestFrameIDLoopCloure]);
    *latestKeyFrameCloud += *transformPointCloud(surfCloudKeyFrames[latestFrameIDLoopCloure],
                                                 &cloudKeyPoses6D->points[latestFrameIDLoopCloure]);

    // save history near key frames
    bool nearFrameAvailable = false;
    for (int j = -historyKeyframeSearchNum; j <= historyKeyframeSearchNum; ++j) {
      if (closestHistoryFrameID + j < 0 || closestHistoryFrameID + j > latestFrameIDLoopCloure)
        continue;
      *nearHistoryKeyFrameCloud += *transformPointCloud(cornerCloudKeyFrames[closestHistoryFrameID + j],
                                                        &cloudKeyPoses6D->points[closestHistoryFrameID + j]);
      *nearHistoryKeyFrameCloud += *transformPointCloud(surfCloudKeyFrames[closestHistoryFrameID + j],
                                                        &cloudKeyPoses6D->points[closestHistoryFrameID + j]);
      nearFrameAvailable = true;
    }

    if (nearFrameAvailable == false)
      return false;

    *latestID = latestFrameIDLoopCloure;
    *closestID = closestHistoryFrameID;

    return true;
  }

  void performLoopClosure() {
    if (cloudKeyPoses3D->points.empty() == true)
      return;

    int latestFrameIDLoopCloure; // 关键帧队列中最新的关键帧id
    int closestHistoryFrameID;  // 最近的关键帧id
    if (detectLoopClosure(&latestFrameIDLoopCloure, &closestHistoryFrameID) == false)
      return;

    //  检测到了回环进入以下流程，将两帧点云进行icp配准得到最终的trans
    // ICP Settings
    pcl::IterativeClosestPoint<PointType, PointType> icp;
    icp.setMaxCorrespondenceDistance(100);
    icp.setMaximumIterations(100);
    icp.setTransformationEpsilon(1e-6);
    icp.setEuclideanFitnessEpsilon(1e-6);
    icp.setRANSACIterations(0);

    // Downsample map cloud
    pcl::PointCloud<PointType>::Ptr cloud_temp(new pcl::PointCloud<PointType>());
    downSizeFilterICP.setInputCloud(nearHistoryKeyFrameCloud);
    downSizeFilterICP.filter(*cloud_temp);
    *nearHistoryKeyFrameCloud = *cloud_temp;
    // publish history near key frames
    publishCloud(&pubHistoryKeyFrames, nearHistoryKeyFrameCloud, timeLaserInfoStamp, "odom");

    // Align clouds 将回环帧与local map进行匹配
    icp.setInputSource(latestKeyFrameCloud);
    icp.setInputTarget(nearHistoryKeyFrameCloud);
    pcl::PointCloud<PointType>::Ptr unused_result(new pcl::PointCloud<PointType>());
    icp.align(*unused_result);

    // 通过icp score阈值判断是否匹配成功
    // std::cout << "ICP converg flag:" << icp.hasConverged() << ". Fitness score: " << icp.getFitnessScore() << std::endl;
    if (icp.hasConverged() == false || icp.getFitnessScore() > historyKeyframeFitnessScore)
      return;

    // publish corrected cloud
    if (pubIcpKeyFrames.getNumSubscribers() != 0) {
      pcl::PointCloud<PointType>::Ptr closed_cloud(new pcl::PointCloud<PointType>());
      pcl::transformPointCloud(*latestKeyFrameCloud, *closed_cloud, icp.getFinalTransformation());
      publishCloud(&pubIcpKeyFrames, closed_cloud, timeLaserInfoStamp, "odom");
    }

    // Get pose transformation
    float x, y, z, roll, pitch, yaw;
    Eigen::Affine3f correctionLidarFrame;
    // icp得到的两帧之间的转换
    correctionLidarFrame = icp.getFinalTransformation();
    // transform from world origin to wrong pose
    Eigen::Affine3f tWrong = pclPointToAffine3f(cloudKeyPoses6D->points[latestFrameIDLoopCloure]);
    // transform from world origin to corrected pose
    Eigen::Affine3f tCorrect =
        correctionLidarFrame * tWrong;// pre-multiplying -> successive rotation about a fixed frame
    pcl::getTranslationAndEulerAngles(tCorrect, x, y, z, roll, pitch, yaw);

    // gtsam中添加回环的约束
    gtsam::Pose3 poseFrom = Pose3(Rot3::RzRyRx(roll, pitch, yaw), Point3(x, y, z));
    gtsam::Pose3 poseTo = pclPointTogtsamPose3(cloudKeyPoses6D->points[closestHistoryFrameID]);
    gtsam::Vector Vector6(6);
    float noiseScore = icp.getFitnessScore();
    Vector6 << noiseScore, noiseScore, noiseScore, noiseScore, noiseScore, noiseScore;
    noiseModel::Diagonal::shared_ptr constraintNoise = noiseModel::Diagonal::Variances(Vector6);

    // Add pose constraint
    std::lock_guard<std::mutex> lock(mtx);
    gtSAMgraph.add(BetweenFactor<Pose3>(latestFrameIDLoopCloure, closestHistoryFrameID, poseFrom.between(poseTo),
                                        constraintNoise));
    isam->update(gtSAMgraph);
    isam->update();
    isam->update();
    isam->update();
    isam->update();
    isam->update();
    gtSAMgraph.resize(0);

    aLoopIsClosed = true;
  }

  void updateInitialGuess() {
    // 更新初始位姿, 来源可以是GPS ODOM, 也可以是上一帧的位姿, 存在transformTobeMapped中
    // initialization
    if (cloudKeyPoses3D->points.empty()) {
      // 第一帧点云进来
      transformTobeMapped[0] = cloudInfo.imuRollInit;
      transformTobeMapped[1] = cloudInfo.imuPitchInit;
      transformTobeMapped[2] = cloudInfo.imuYawInit;

      if (!useImuHeadingInitialization)
        transformTobeMapped[2] = 0;

      // 获取初始的transform
      lastImuTransformation = pcl::getTransformation(0, 0, 0, cloudInfo.imuRollInit, cloudInfo.imuPitchInit,
                                                     cloudInfo.imuYawInit); // save imu before return;
      return;
    }

    // use imu pre-integration estimation for pose guess
    // odom可用的话, 使用mu odom作为初始位姿, 每个点在imuPreintexx.cpp中会实时进行预计分优化, 并存储其优化后的odom
    if (cloudInfo.odomAvailable == true && cloudInfo.imuPreintegrationResetId == imuPreintegrationResetId) {
      transformTobeMapped[0] = cloudInfo.initialGuessRoll;
      transformTobeMapped[1] = cloudInfo.initialGuessPitch;
      transformTobeMapped[2] = cloudInfo.initialGuessYaw;

      transformTobeMapped[3] = cloudInfo.initialGuessX;
      transformTobeMapped[4] = cloudInfo.initialGuessY;
      transformTobeMapped[5] = cloudInfo.initialGuessZ;

      lastImuTransformation = pcl::getTransformation(0, 0, 0, cloudInfo.imuRollInit, cloudInfo.imuPitchInit,
                                                     cloudInfo.imuYawInit); // save imu before return;
      return;
    }

    // use imu incremental estimation for pose guess (only rotation)
    // imu可用的话, 使用imu计算一个旋转增量, 这里？
    if (cloudInfo.imuAvailable == true) {
      Eigen::Affine3f transBack = pcl::getTransformation(0, 0, 0, cloudInfo.imuRollInit, cloudInfo.imuPitchInit,
                                                         cloudInfo.imuYawInit);
      Eigen::Affine3f transIncre = lastImuTransformation.inverse() * transBack;

      Eigen::Affine3f transTobe = trans2Affine3f(transformTobeMapped);
      Eigen::Affine3f transFinal = transTobe * transIncre;
      pcl::getTranslationAndEulerAngles(transFinal, transformTobeMapped[3], transformTobeMapped[4],
                                        transformTobeMapped[5],
                                        transformTobeMapped[0], transformTobeMapped[1], transformTobeMapped[2]);

      lastImuTransformation = pcl::getTransformation(0, 0, 0, cloudInfo.imuRollInit, cloudInfo.imuPitchInit,
                                                     cloudInfo.imuYawInit); // save imu before return;
      return;
    }
  }

  void extractForLoopClosure() {
    // 提取回环候选帧
    pcl::PointCloud<PointType>::Ptr cloudToExtract(new pcl::PointCloud<PointType>());
    int numPoses = cloudKeyPoses3D->size();
    for (int i = numPoses - 1; i >= 0; --i) {
      if (cloudToExtract->size() <= surroundingKeyframeSize)
        cloudToExtract->push_back(cloudKeyPoses3D->points[i]);
      else
        break;
    }

    extractCloud(cloudToExtract);
  }

  void extractNearby() {
    // 提取附近的点云帧, 包括corner和surface, cloudKeyPoses3D
    pcl::PointCloud<PointType>::Ptr surroundingKeyPoses(new pcl::PointCloud<PointType>());
    pcl::PointCloud<PointType>::Ptr surroundingKeyPosesDS(new pcl::PointCloud<PointType>());
    std::vector<int> pointSearchInd;
    std::vector<float> pointSearchSqDis;

    // extract all the nearby key poses and downsample them, 50m范围内的关键帧
    kdtreeSurroundingKeyPoses->setInputCloud(cloudKeyPoses3D); // create kd-tree
    kdtreeSurroundingKeyPoses->radiusSearch(cloudKeyPoses3D->back(), (double) surroundingKeyframeSearchRadius,
                                            pointSearchInd, pointSearchSqDis);
    // 将满足要求的点云帧加到surroundingKeyPoses中
    for (int i = 0; i < pointSearchInd.size(); ++i) {
      int id = pointSearchInd[i];
      surroundingKeyPoses->push_back(cloudKeyPoses3D->points[id]);
    }

    downSizeFilterSurroundingKeyPoses.setInputCloud(surroundingKeyPoses);
    downSizeFilterSurroundingKeyPoses.filter(*surroundingKeyPosesDS);

    // also extract some latest key frames in case the robot rotates in one position
    // 把10s内同方向的关键帧也加到surroundingKeyPosesDS中
    int numPoses = cloudKeyPoses3D->size();
    for (int i = numPoses - 1; i >= 0; --i) {
      // 10s内的位姿态都加进来
      if (timeLaserCloudInfoLast - cloudKeyPoses6D->points[i].time < 10.0)
        surroundingKeyPosesDS->push_back(cloudKeyPoses3D->points[i]);
      else
        break;
    }

    extractCloud(surroundingKeyPosesDS);
  }

  void extractCloud(pcl::PointCloud<PointType>::Ptr cloudToExtract) {
    // 根据pose提取点云
    std::vector<pcl::PointCloud<PointType>> laserCloudCornerSurroundingVec;
    std::vector<pcl::PointCloud<PointType>> laserCloudSurfSurroundingVec;

    laserCloudCornerSurroundingVec.resize(cloudToExtract->size());
    laserCloudSurfSurroundingVec.resize(cloudToExtract->size());

    // extract surrounding map
#pragma omp parallel for num_threads(numberOfCores)
    for (int i = 0; i < cloudToExtract->size(); ++i) {
      // 遍历每个位姿
      if (pointDistance(cloudToExtract->points[i], cloudKeyPoses3D->back()) > surroundingKeyframeSearchRadius)
        continue;
      int thisKeyInd = (int) cloudToExtract->points[i].intensity;
      laserCloudCornerSurroundingVec[i] = *transformPointCloud(cornerCloudKeyFrames[thisKeyInd],
                                                               &cloudKeyPoses6D->points[thisKeyInd]);
      laserCloudSurfSurroundingVec[i] = *transformPointCloud(surfCloudKeyFrames[thisKeyInd],
                                                             &cloudKeyPoses6D->points[thisKeyInd]);
    }

    // fuse the map
    // 构建local map
    laserCloudCornerFromMap->clear();
    laserCloudSurfFromMap->clear();
    for (int i = 0; i < cloudToExtract->size(); ++i) {
      *laserCloudCornerFromMap += laserCloudCornerSurroundingVec[i];
      *laserCloudSurfFromMap += laserCloudSurfSurroundingVec[i];
    }

    // Downsample the surrounding corner key frames (or map)
    downSizeFilterCorner.setInputCloud(laserCloudCornerFromMap);
    downSizeFilterCorner.filter(*laserCloudCornerFromMapDS);
    laserCloudCornerFromMapDSNum = laserCloudCornerFromMapDS->size();
    // Downsample the surrounding surf key frames (or map)
    downSizeFilterSurf.setInputCloud(laserCloudSurfFromMap);
    downSizeFilterSurf.filter(*laserCloudSurfFromMapDS);
    laserCloudSurfFromMapDSNum = laserCloudSurfFromMapDS->size();
  }

  void extractSurroundingKeyFrames() {
    if (cloudKeyPoses3D->points.empty() == true)
      return;

    // 检测到了回环就提取回环帧,否则提取附近点云
    // 第一次进来loopClosureEnableFlag = false, 直接提取附近关键帧
    if (loopClosureEnableFlag == true) {
      extractForLoopClosure();
    } else {
      extractNearby();
    }
  }

  void downsampleCurrentScan() {
    // Downsample cloud from current scan
    laserCloudCornerLastDS->clear();
    downSizeFilterCorner.setInputCloud(laserCloudCornerLast);
    downSizeFilterCorner.filter(*laserCloudCornerLastDS);
    laserCloudCornerLastDSNum = laserCloudCornerLastDS->size();

    laserCloudSurfLastDS->clear();
    downSizeFilterSurf.setInputCloud(laserCloudSurfLast);
    downSizeFilterSurf.filter(*laserCloudSurfLastDS);
    laserCloudSurfLastDSNum = laserCloudSurfLastDS->size();
  }

  void updatePointAssociateToMap() {
    // 根据初始位姿将点云转换到Map系下
    transPointAssociateToMap = trans2Affine3f(transformTobeMapped);
  }

  void cornerOptimization() {
    updatePointAssociateToMap(); // 将points转到地图系

#pragma omp parallel for num_threads(numberOfCores)
    // 遍历点云, 构建点到直线的约束
    for (int i = 0; i < laserCloudCornerLastDSNum; i++) {
      PointType pointOri, pointSel, coeff;
      std::vector<int> pointSearchInd;
      std::vector<float> pointSearchSqDis;

      // 在map中搜索当前点的5个紧邻点
      pointOri = laserCloudCornerLastDS->points[i];
      pointAssociateToMap(&pointOri, &pointSel);
      kdtreeCornerFromMap->nearestKSearch(pointSel, 5, pointSearchInd, pointSearchSqDis);

      cv::Mat matA1(3, 3, CV_32F, cv::Scalar::all(0));
      cv::Mat matD1(1, 3, CV_32F, cv::Scalar::all(0));
      cv::Mat matV1(3, 3, CV_32F, cv::Scalar::all(0));

      // 只有最近的点都在一定阈值内（1米）才进行计算
      if (pointSearchSqDis[4] < 1.0) {
        float cx = 0, cy = 0, cz = 0;
        for (int j = 0; j < 5; j++) {
          cx += laserCloudCornerFromMapDS->points[pointSearchInd[j]].x;
          cy += laserCloudCornerFromMapDS->points[pointSearchInd[j]].y;
          cz += laserCloudCornerFromMapDS->points[pointSearchInd[j]].z;
        }
        // 计算其算数平均值/均值
        cx /= 5;
        cy /= 5;
        cz /= 5;

        // 计算协方差
        float a11 = 0, a12 = 0, a13 = 0, a22 = 0, a23 = 0, a33 = 0;
        for (int j = 0; j < 5; j++) {
          float ax = laserCloudCornerFromMapDS->points[pointSearchInd[j]].x - cx;
          float ay = laserCloudCornerFromMapDS->points[pointSearchInd[j]].y - cy;
          float az = laserCloudCornerFromMapDS->points[pointSearchInd[j]].z - cz;

          a11 += ax * ax;
          a12 += ax * ay;
          a13 += ax * az;
          a22 += ay * ay;
          a23 += ay * az;
          a33 += az * az;
        }
        a11 /= 5;
        a12 /= 5;
        a13 /= 5;
        a22 /= 5;
        a23 /= 5;
        a33 /= 5;

        matA1.at<float>(0, 0) = a11;
        matA1.at<float>(0, 1) = a12;
        matA1.at<float>(0, 2) = a13;
        matA1.at<float>(1, 0) = a12;
        matA1.at<float>(1, 1) = a22;
        matA1.at<float>(1, 2) = a23;
        matA1.at<float>(2, 0) = a13;
        matA1.at<float>(2, 1) = a23;
        matA1.at<float>(2, 2) = a33;

        // 求协方差矩阵的特征值和特征向量, 特征值：matD1，特征向量：保存在矩阵matV1中。
        cv::eigen(matA1, matD1, matV1);

        // 其中一个特征值远远大于其他两个，则呈线状
        if (matD1.at<float>(0, 0) > 3 * matD1.at<float>(0, 1)) {
          float x0 = pointSel.x;
          float y0 = pointSel.y;
          float z0 = pointSel.z;
          float x1 = cx + 0.1 * matV1.at<float>(0, 0);
          float y1 = cy + 0.1 * matV1.at<float>(0, 1);
          float z1 = cz + 0.1 * matV1.at<float>(0, 2);
          float x2 = cx - 0.1 * matV1.at<float>(0, 0);
          float y2 = cy - 0.1 * matV1.at<float>(0, 1);
          float z2 = cz - 0.1 * matV1.at<float>(0, 2);

          // 与里程计的计算类似，计算到直线的距离
          float a012 = sqrt(((x0 - x1) * (y0 - y2) - (x0 - x2) * (y0 - y1)) *
              ((x0 - x1) * (y0 - y2) - (x0 - x2) * (y0 - y1))
                                + ((x0 - x1) * (z0 - z2) - (x0 - x2) * (z0 - z1)) *
                                    ((x0 - x1) * (z0 - z2) - (x0 - x2) * (z0 - z1))
                                + ((y0 - y1) * (z0 - z2) - (y0 - y2) * (z0 - z1)) *
                                    ((y0 - y1) * (z0 - z2) - (y0 - y2) * (z0 - z1)));

          float l12 = sqrt((x1 - x2) * (x1 - x2) + (y1 - y2) * (y1 - y2) + (z1 - z2) * (z1 - z2));

          float la = ((y1 - y2) * ((x0 - x1) * (y0 - y2) - (x0 - x2) * (y0 - y1))
              + (z1 - z2) * ((x0 - x1) * (z0 - z2) - (x0 - x2) * (z0 - z1))) / a012 / l12;

          float lb = -((x1 - x2) * ((x0 - x1) * (y0 - y2) - (x0 - x2) * (y0 - y1))
              - (z1 - z2) * ((y0 - y1) * (z0 - z2) - (y0 - y2) * (z0 - z1))) / a012 / l12;

          float lc = -((x1 - x2) * ((x0 - x1) * (z0 - z2) - (x0 - x2) * (z0 - z1))
              + (y1 - y2) * ((y0 - y1) * (z0 - z2) - (y0 - y2) * (z0 - z1))) / a012 / l12;

          float ld2 = a012 / l12;

          // 下面涉及到一个鲁棒核函数，作者简单地设计了这个核函数。
          float s = 1 - 0.9 * fabs(ld2);

          coeff.x = s * la;
          coeff.y = s * lb;
          coeff.z = s * lc;
          coeff.intensity = s * ld2;

          // 程序末尾根据s的值来判断是否将点云点放入点云集合laserCloudOri以及coeffSel中。
          if (s > 0.1) {
            laserCloudOriCornerVec[i] = pointOri;
            coeffSelCornerVec[i] = coeff;
            laserCloudOriCornerFlag[i] = true;
          }
        }
      }
    }
  }

  void surfOptimization() {
    updatePointAssociateToMap();

#pragma omp parallel for num_threads(numberOfCores)
    for (int i = 0; i < laserCloudSurfLastDSNum; i++) {
      PointType pointOri, pointSel, coeff;
      std::vector<int> pointSearchInd;
      std::vector<float> pointSearchSqDis;

      // 寻找5个紧邻点, 计算其特征值和特征向量
      pointOri = laserCloudSurfLastDS->points[i];
      pointAssociateToMap(&pointOri, &pointSel);
      kdtreeSurfFromMap->nearestKSearch(pointSel, 5, pointSearchInd, pointSearchSqDis);

      Eigen::Matrix<float, 5, 3> matA0;
      Eigen::Matrix<float, 5, 1> matB0;
      Eigen::Vector3f matX0;

      matA0.setZero();  // 5*3 存储5个紧邻点
      matB0.fill(-1);
      matX0.setZero();

      // 只考虑附近1.0m内
      if (pointSearchSqDis[4] < 1.0) {
        for (int j = 0; j < 5; j++) {
          matA0(j, 0) = laserCloudSurfFromMapDS->points[pointSearchInd[j]].x;
          matA0(j, 1) = laserCloudSurfFromMapDS->points[pointSearchInd[j]].y;
          matA0(j, 2) = laserCloudSurfFromMapDS->points[pointSearchInd[j]].z;
        }

        // 求maxA0中点构成的平面法向量
        matX0 = matA0.colPivHouseholderQr().solve(matB0);

        // 法向量参数 ax+by+cz +d = 0
        float pa = matX0(0, 0);
        float pb = matX0(1, 0);
        float pc = matX0(2, 0);
        float pd = 1;

        float ps = sqrt(pa * pa + pb * pb + pc * pc);
        pa /= ps;
        pb /= ps;
        pc /= ps;
        pd /= ps;

        // 这里再次判断求解的方向向量和每个点相乘，最后结果是不是在误差范围内。
        bool planeValid = true;
        for (int j = 0; j < 5; j++) {
          if (fabs(pa * laserCloudSurfFromMapDS->points[pointSearchInd[j]].x +
              pb * laserCloudSurfFromMapDS->points[pointSearchInd[j]].y +
              pc * laserCloudSurfFromMapDS->points[pointSearchInd[j]].z + pd) > 0.2) {
            planeValid = false;
            break;
          }
        }

        // 是有效的平面
        if (planeValid) {
          float pd2 = pa * pointSel.x + pb * pointSel.y + pc * pointSel.z + pd;

          float s = 1 - 0.9 * fabs(pd2) / sqrt(sqrt(pointSel.x * pointSel.x
                                                        + pointSel.y * pointSel.y + pointSel.z * pointSel.z));

          coeff.x = s * pa;
          coeff.y = s * pb;
          coeff.z = s * pc;
          coeff.intensity = s * pd2;

          // 误差在允许的范围内的话把这个点放到点云laserCloudOri中去，把对应的向量coeff放到coeffSel中
          if (s > 0.1) {
            laserCloudOriSurfVec[i] = pointOri;
            coeffSelSurfVec[i] = coeff;
            laserCloudOriSurfFlag[i] = true;
          }
        }
      }
    }
  }

  void combineOptimizationCoeffs() {
    // 把两类损失和协方差丢到laserCloudOri和coeffSel中, 后续进行联合优化
    // combine corner coeffs
    for (int i = 0; i < laserCloudCornerLastDSNum; ++i) {
      if (laserCloudOriCornerFlag[i] == true) {
        laserCloudOri->push_back(laserCloudOriCornerVec[i]);
        coeffSel->push_back(coeffSelCornerVec[i]);
      }
    }
    // combine surf coeffs
    for (int i = 0; i < laserCloudSurfLastDSNum; ++i) {
      if (laserCloudOriSurfFlag[i] == true) {
        laserCloudOri->push_back(laserCloudOriSurfVec[i]);
        coeffSel->push_back(coeffSelSurfVec[i]);
      }
    }
    // reset flag for next iteration 重置参数, 下一帧还要继续用
    std::fill(laserCloudOriCornerFlag.begin(), laserCloudOriCornerFlag.end(), false);
    std::fill(laserCloudOriSurfFlag.begin(), laserCloudOriSurfFlag.end(), false);
  }

  bool LMOptimization(int iterCount) {
    // This optimization is from the original loam_velodyne by Ji Zhang, need to cope with coordinate transformation
    // lidar <- camera      ---     camera <- lidar
    // x = z                ---     x = y
    // y = x                ---     y = z
    // z = y                ---     z = x
    // roll = yaw           ---     roll = pitch
    // pitch = roll         ---     pitch = yaw
    // yaw = pitch          ---     yaw = roll

    // 高斯牛顿优化, 参考LOAM
    // lidar -> camera
    float srx = sin(transformTobeMapped[1]);
    float crx = cos(transformTobeMapped[1]);
    float sry = sin(transformTobeMapped[2]);
    float cry = cos(transformTobeMapped[2]);
    float srz = sin(transformTobeMapped[0]);
    float crz = cos(transformTobeMapped[0]);

    // 初次优化时，特征值门限设置为50，小于这个值认为是退化了，修改matX，matX=matP*matX2
    int laserCloudSelNum = laserCloudOri->size();
    if (laserCloudSelNum < 50) {
      return false;
    }

    cv::Mat matA(laserCloudSelNum, 6, CV_32F, cv::Scalar::all(0));
    cv::Mat matAt(6, laserCloudSelNum, CV_32F, cv::Scalar::all(0));
    cv::Mat matAtA(6, 6, CV_32F, cv::Scalar::all(0));
    cv::Mat matB(laserCloudSelNum, 1, CV_32F, cv::Scalar::all(0));
    cv::Mat matAtB(6, 1, CV_32F, cv::Scalar::all(0));
    cv::Mat matX(6, 1, CV_32F, cv::Scalar::all(0));
    cv::Mat matP(6, 6, CV_32F, cv::Scalar::all(0));

    PointType pointOri, coeff;

    for (int i = 0; i < laserCloudSelNum; i++) {
      // lidar -> camera
      pointOri.x = laserCloudOri->points[i].y;
      pointOri.y = laserCloudOri->points[i].z;
      pointOri.z = laserCloudOri->points[i].x;
      // lidar -> camera
      coeff.x = coeffSel->points[i].y;
      coeff.y = coeffSel->points[i].z;
      coeff.z = coeffSel->points[i].x;
      coeff.intensity = coeffSel->points[i].intensity;
      // in camera
      // 计算雅克比
      float arx = (crx * sry * srz * pointOri.x + crx * crz * sry * pointOri.y - srx * sry * pointOri.z) * coeff.x
          + (-srx * srz * pointOri.x - crz * srx * pointOri.y - crx * pointOri.z) * coeff.y
          + (crx * cry * srz * pointOri.x + crx * cry * crz * pointOri.y - cry * srx * pointOri.z) *
              coeff.z;

      float ary = ((cry * srx * srz - crz * sry) * pointOri.x
          + (sry * srz + cry * crz * srx) * pointOri.y + crx * cry * pointOri.z) * coeff.x
          + ((-cry * crz - srx * sry * srz) * pointOri.x
              + (cry * srz - crz * srx * sry) * pointOri.y - crx * sry * pointOri.z) * coeff.z;

      float arz =
          ((crz * srx * sry - cry * srz) * pointOri.x + (-cry * crz - srx * sry * srz) * pointOri.y) * coeff.x
              + (crx * crz * pointOri.x - crx * srz * pointOri.y) * coeff.y
              +
                  ((sry * srz + cry * crz * srx) * pointOri.x + (crz * sry - cry * srx * srz) * pointOri.y) * coeff.z;
      // lidar -> camera
      matA.at<float>(i, 0) = arz;
      matA.at<float>(i, 1) = arx;
      matA.at<float>(i, 2) = ary;
      matA.at<float>(i, 3) = coeff.z;
      matA.at<float>(i, 4) = coeff.x;
      matA.at<float>(i, 5) = coeff.y;
      matB.at<float>(i, 0) = -coeff.intensity;
    }

    cv::transpose(matA, matAt);
    matAtA = matAt * matA;
    matAtB = matAt * matB;
    cv::solve(matAtA, matAtB, matX, cv::DECOMP_QR);

    if (iterCount == 0) {

      cv::Mat matE(1, 6, CV_32F, cv::Scalar::all(0));
      cv::Mat matV(6, 6, CV_32F, cv::Scalar::all(0));
      cv::Mat matV2(6, 6, CV_32F, cv::Scalar::all(0));

      cv::eigen(matAtA, matE, matV);
      matV.copyTo(matV2);

      isDegenerate = false;
      float eignThre[6] = {100, 100, 100, 100, 100, 100};
      for (int i = 5; i >= 0; i--) {
        if (matE.at<float>(0, i) < eignThre[i]) {
          for (int j = 0; j < 6; j++) {
            matV2.at<float>(i, j) = 0;
          }
          isDegenerate = true;
        } else {
          break;
        }
      }
      matP = matV.inv() * matV2;
    }

    if (isDegenerate) {
      cv::Mat matX2(6, 1, CV_32F, cv::Scalar::all(0));
      matX.copyTo(matX2);
      matX = matP * matX2;
    }

    transformTobeMapped[0] += matX.at<float>(0, 0);
    transformTobeMapped[1] += matX.at<float>(1, 0);
    transformTobeMapped[2] += matX.at<float>(2, 0);
    transformTobeMapped[3] += matX.at<float>(3, 0);
    transformTobeMapped[4] += matX.at<float>(4, 0);
    transformTobeMapped[5] += matX.at<float>(5, 0);

    float deltaR = sqrt(
        pow(pcl::rad2deg(matX.at<float>(0, 0)), 2) +
            pow(pcl::rad2deg(matX.at<float>(1, 0)), 2) +
            pow(pcl::rad2deg(matX.at<float>(2, 0)), 2));
    float deltaT = sqrt(
        pow(matX.at<float>(3, 0) * 100, 2) +
            pow(matX.at<float>(4, 0) * 100, 2) +
            pow(matX.at<float>(5, 0) * 100, 2));

    // 在判断是否是有效的优化时，要求旋转部分的模长小于0.05m，平移部分的模长也小于0.05度
    if (deltaR < 0.05 && deltaT < 0.05) {
      return true; // converged
    }
    return false; // keep optimizing
  }

  void scan2MapOptimization() {
    if (cloudKeyPoses3D->points.empty())
      return;

    //  特征需要满足一定要求才可以进行
    if (laserCloudCornerLastDSNum > edgeFeatureMinValidNum && laserCloudSurfLastDSNum > surfFeatureMinValidNum) {
      // 构建kdtree搜索的map, 两类
      kdtreeCornerFromMap->setInputCloud(laserCloudCornerFromMapDS);
      kdtreeSurfFromMap->setInputCloud(laserCloudSurfFromMapDS);

      // 迭代30次进行优化
      for (int iterCount = 0; iterCount < 30; iterCount++) {
        laserCloudOri->clear();
        coeffSel->clear();

        // 点到平面, 点到直线的残差, 这里写法还与aloam有点区别
        cornerOptimization();
        surfOptimization();

        // 联合两类的残差
        combineOptimizationCoeffs();

        // 高斯牛顿法迭代优化
        if (LMOptimization(iterCount) == true)
          break;
      }

      // 更新transform
      transformUpdate();
    } else {
      ROS_WARN("Not enough features! Only %d edge and %d planar features available.", laserCloudCornerLastDSNum,
               laserCloudSurfLastDSNum);
    }
  }

  void transformUpdate() {
    // IMU可用的话更新transformTobeMapped
    if (cloudInfo.imuAvailable == true) {
      if (std::abs(cloudInfo.imuPitchInit) < 1.4) {
        double imuWeight = 0.05;
        tf::Quaternion imuQuaternion;
        tf::Quaternion transformQuaternion;
        double rollMid, pitchMid, yawMid;

        // slerp roll
        transformQuaternion.setRPY(transformTobeMapped[0], 0, 0);
        imuQuaternion.setRPY(cloudInfo.imuRollInit, 0, 0);
        // 线性插值
        tf::Matrix3x3(transformQuaternion.slerp(imuQuaternion, imuWeight)).getRPY(rollMid, pitchMid, yawMid);
        transformTobeMapped[0] = rollMid;

        // slerp pitch
        transformQuaternion.setRPY(0, transformTobeMapped[1], 0);
        imuQuaternion.setRPY(0, cloudInfo.imuPitchInit, 0);
        tf::Matrix3x3(transformQuaternion.slerp(imuQuaternion, imuWeight)).getRPY(rollMid, pitchMid, yawMid);
        transformTobeMapped[1] = pitchMid;
      }
    }

    transformTobeMapped[0] = constraintTransformation(transformTobeMapped[0], rotation_tollerance);
    transformTobeMapped[1] = constraintTransformation(transformTobeMapped[1], rotation_tollerance);
    transformTobeMapped[5] = constraintTransformation(transformTobeMapped[5], z_tollerance);
  }

  float constraintTransformation(float value, float limit) {
    if (value < -limit)
      value = -limit;
    if (value > limit)
      value = limit;

    return value;
  }

  bool saveFrame() {
    if (cloudKeyPoses3D->points.empty())
      return true;

    Eigen::Affine3f transStart = pclPointToAffine3f(cloudKeyPoses6D->back());
    Eigen::Affine3f transFinal = pcl::getTransformation(transformTobeMapped[3], transformTobeMapped[4],
                                                        transformTobeMapped[5],
                                                        transformTobeMapped[0], transformTobeMapped[1],
                                                        transformTobeMapped[2]);
    Eigen::Affine3f transBetween = transStart.inverse() * transFinal;
    float x, y, z, roll, pitch, yaw;
    pcl::getTranslationAndEulerAngles(transBetween, x, y, z, roll, pitch, yaw);

    if (abs(roll) < surroundingkeyframeAddingAngleThreshold &&
        abs(pitch) < surroundingkeyframeAddingAngleThreshold &&
        abs(yaw) < surroundingkeyframeAddingAngleThreshold &&
        sqrt(x * x + y * y + z * z) < surroundingkeyframeAddingDistThreshold)
      return false;

    return true;
  }

  void addOdomFactor() {
    if (cloudKeyPoses3D->points.empty()) {
      // 第一帧进来时初始化gtsam参数
      noiseModel::Diagonal::shared_ptr priorNoise = noiseModel::Diagonal::Variances(
          (Vector(6) << 1e-2, 1e-2, M_PI * M_PI, 1e8, 1e8, 1e8).finished()); // rad*rad, meter*meter
          // 先验因子
      gtSAMgraph.add(PriorFactor<Pose3>(0, trans2gtsamPose(transformTobeMapped), priorNoise));
      initialEstimate.insert(0, trans2gtsamPose(transformTobeMapped));
    } else {
      noiseModel::Diagonal::shared_ptr odometryNoise = noiseModel::Diagonal::Variances(
          (Vector(6) << 1e-6, 1e-6, 1e-6, 1e-4, 1e-4, 1e-4).finished());
      gtsam::Pose3 poseFrom = pclPointTogtsamPose3(cloudKeyPoses6D->points.back());
      gtsam::Pose3 poseTo = trans2gtsamPose(transformTobeMapped);
      // 二元因子
      gtSAMgraph.add(
          BetweenFactor<Pose3>(cloudKeyPoses3D->size() - 1, cloudKeyPoses3D->size(), poseFrom.between(poseTo),
                               odometryNoise));
      initialEstimate.insert(cloudKeyPoses3D->size(), poseTo); // 添加值
    }
  }

  void addGPSFactor() {
    if (gpsQueue.empty())
      return;

    // wait for system initialized and settles down
    if (cloudKeyPoses3D->points.empty())
      return;
    else {
      if (pointDistance(cloudKeyPoses3D->front(), cloudKeyPoses3D->back()) < 5.0)
        return;
    }

    // pose covariance small, no need to correct
    if (poseCovariance(3, 3) < poseCovThreshold && poseCovariance(4, 4) < poseCovThreshold)
      return;

    // pose的协方差比较大的时候才去添加gps factor
    while (!gpsQueue.empty()) {
      // 时间戳对齐
      if (gpsQueue.front().header.stamp.toSec() < timeLaserCloudInfoLast - 0.2) {
        // message too old
        gpsQueue.pop_front();
      } else if (gpsQueue.front().header.stamp.toSec() > timeLaserCloudInfoLast + 0.2) {
        // message too new
        break;
      } else {
        nav_msgs::Odometry thisGPS = gpsQueue.front();
        gpsQueue.pop_front();

        // GPS too noisy, skip
        float noise_x = thisGPS.pose.covariance[0];
        float noise_y = thisGPS.pose.covariance[7];
        float noise_z = thisGPS.pose.covariance[14];
        if (noise_x > gpsCovThreshold || noise_y > gpsCovThreshold)
          continue;

        float gps_x = thisGPS.pose.pose.position.x;
        float gps_y = thisGPS.pose.pose.position.y;
        float gps_z = thisGPS.pose.pose.position.z;
        if (!useGpsElevation) {
          gps_z = transformTobeMapped[5];  // gps的z一般不可信
          noise_z = 0.01;
        }

        // GPS not properly initialized (0,0,0)
        if (abs(gps_x) < 1e-6 && abs(gps_y) < 1e-6)
          continue;

        // 添加GPS因子
        gtsam::Vector Vector3(3);
        Vector3 << noise_x, noise_y, noise_z;
        noiseModel::Diagonal::shared_ptr gps_noise = noiseModel::Diagonal::Variances(Vector3); // 噪声定义
        gtsam::GPSFactor gps_factor(cloudKeyPoses3D->size(), gtsam::Point3(gps_x, gps_y, gps_z), gps_noise);
        gtSAMgraph.add(gps_factor);

        aLoopIsClosed = true;
        break;
      }
    }
  }

  void saveKeyFramesAndFactor() {
    if (saveFrame() == false)
      return;
    // 添加各种factor、保存关键帧
    // odom factor
    addOdomFactor();

    // gps factor
    addGPSFactor();

    // cout << "****************************************************" << endl;
    // gtSAMgraph.print("GTSAM Graph:\n");

    // update iSAM
    isam->update(gtSAMgraph, initialEstimate);
    isam->update();

    // update multiple-times till converge
    if (aLoopIsClosed == true) {
      isam->update();
      isam->update();
      isam->update();
      isam->update();
      isam->update();
    }

    gtSAMgraph.resize(0);
    initialEstimate.clear();

    //save key poses
    PointType thisPose3D;
    PointTypePose thisPose6D;
    Pose3 latestEstimate;

    // 最新的pose
    isamCurrentEstimate = isam->calculateEstimate();
    latestEstimate = isamCurrentEstimate.at<Pose3>(isamCurrentEstimate.size() - 1);
    // cout << "****************************************************" << endl;
    // isamCurrentEstimate.print("Current estimate: ");

    // 这里不断的增加关键帧到cloudKeyPoses3D、cloudKeyPoses6D中
    thisPose3D.x = latestEstimate.translation().x();
    thisPose3D.y = latestEstimate.translation().y();
    thisPose3D.z = latestEstimate.translation().z();
    thisPose3D.intensity = cloudKeyPoses3D->size(); // this can be used as index
    cloudKeyPoses3D->push_back(thisPose3D);

    thisPose6D.x = thisPose3D.x;
    thisPose6D.y = thisPose3D.y;
    thisPose6D.z = thisPose3D.z;
    thisPose6D.intensity = thisPose3D.intensity; // this can be used as index
    thisPose6D.roll = latestEstimate.rotation().roll();
    thisPose6D.pitch = latestEstimate.rotation().pitch();
    thisPose6D.yaw = latestEstimate.rotation().yaw();
    thisPose6D.time = timeLaserCloudInfoLast;
    cloudKeyPoses6D->push_back(thisPose6D);

    // cout << "****************************************************" << endl;
    // cout << "Pose covariance:" << endl;
    // cout << isam->marginalCovariance(isamCurrentEstimate.size()-1) << endl << endl;
    // 边缘化得到每个变量的协方差
    poseCovariance = isam->marginalCovariance(isamCurrentEstimate.size() - 1);

    // save updated transform
    transformTobeMapped[0] = latestEstimate.rotation().roll();
    transformTobeMapped[1] = latestEstimate.rotation().pitch();
    transformTobeMapped[2] = latestEstimate.rotation().yaw();
    transformTobeMapped[3] = latestEstimate.translation().x();
    transformTobeMapped[4] = latestEstimate.translation().y();
    transformTobeMapped[5] = latestEstimate.translation().z();

    // save all the received edge and surf points
    pcl::PointCloud<PointType>::Ptr thisCornerKeyFrame(new pcl::PointCloud<PointType>());
    pcl::PointCloud<PointType>::Ptr thisSurfKeyFrame(new pcl::PointCloud<PointType>());
    pcl::copyPointCloud(*laserCloudCornerLastDS, *thisCornerKeyFrame);
    pcl::copyPointCloud(*laserCloudSurfLastDS, *thisSurfKeyFrame);

    // save key frame cloud
    cornerCloudKeyFrames.push_back(thisCornerKeyFrame);
    surfCloudKeyFrames.push_back(thisSurfKeyFrame);

    // save path for visualization
    updatePath(thisPose6D);
  }

  void correctPoses() {
    if (cloudKeyPoses3D->points.empty())
      return;

    if (aLoopIsClosed == true) {
      // clear path
      globalPath.poses.clear();
      // update key poses 更新位姿
      int numPoses = isamCurrentEstimate.size();
      for (int i = 0; i < numPoses; ++i) {
        cloudKeyPoses3D->points[i].x = isamCurrentEstimate.at<Pose3>(i).translation().x();
        cloudKeyPoses3D->points[i].y = isamCurrentEstimate.at<Pose3>(i).translation().y();
        cloudKeyPoses3D->points[i].z = isamCurrentEstimate.at<Pose3>(i).translation().z();

        cloudKeyPoses6D->points[i].x = cloudKeyPoses3D->points[i].x;
        cloudKeyPoses6D->points[i].y = cloudKeyPoses3D->points[i].y;
        cloudKeyPoses6D->points[i].z = cloudKeyPoses3D->points[i].z;
        cloudKeyPoses6D->points[i].roll = isamCurrentEstimate.at<Pose3>(i).rotation().roll();
        cloudKeyPoses6D->points[i].pitch = isamCurrentEstimate.at<Pose3>(i).rotation().pitch();
        cloudKeyPoses6D->points[i].yaw = isamCurrentEstimate.at<Pose3>(i).rotation().yaw();

        updatePath(cloudKeyPoses6D->points[i]);
      }

      aLoopIsClosed = false;
      // ID for reseting IMU pre-integration
      ++imuPreintegrationResetId;
    }
  }

  void updatePath(const PointTypePose &pose_in) {
    geometry_msgs::PoseStamped pose_stamped;
    pose_stamped.header.stamp = timeLaserInfoStamp;
    pose_stamped.header.frame_id = "odom";
    pose_stamped.pose.position.x = pose_in.x;
    pose_stamped.pose.position.y = pose_in.y;
    pose_stamped.pose.position.z = pose_in.z;
    tf::Quaternion q = tf::createQuaternionFromRPY(pose_in.roll, pose_in.pitch, pose_in.yaw);
    pose_stamped.pose.orientation.x = q.x();
    pose_stamped.pose.orientation.y = q.y();
    pose_stamped.pose.orientation.z = q.z();
    pose_stamped.pose.orientation.w = q.w();

    globalPath.poses.push_back(pose_stamped);
  }

  void publishOdometry() {
    // Publish odometry for ROS
    nav_msgs::Odometry laserOdometryROS;
    laserOdometryROS.header.stamp = timeLaserInfoStamp;
    laserOdometryROS.header.frame_id = "odom";
    laserOdometryROS.child_frame_id = "odom_mapping";
    laserOdometryROS.pose.pose.position.x = transformTobeMapped[3];
    laserOdometryROS.pose.pose.position.y = transformTobeMapped[4];
    laserOdometryROS.pose.pose.position.z = transformTobeMapped[5];
    laserOdometryROS.pose.pose.orientation = tf::createQuaternionMsgFromRollPitchYaw(transformTobeMapped[0],
                                                                                     transformTobeMapped[1],
                                                                                     transformTobeMapped[2]);
    laserOdometryROS.pose.covariance[0] = double(imuPreintegrationResetId);
    pubOdomAftMappedROS.publish(laserOdometryROS);
  }

  void publishFrames() {
    if (cloudKeyPoses3D->points.empty())
      return;
    // publish key poses
    publishCloud(&pubKeyPoses, cloudKeyPoses3D, timeLaserInfoStamp, "odom");
    // Publish surrounding key frames
    publishCloud(&pubRecentKeyFrames, laserCloudSurfFromMapDS, timeLaserInfoStamp, "odom");
    // publish registered key frame
    if (pubRecentKeyFrame.getNumSubscribers() != 0) {
      pcl::PointCloud<PointType>::Ptr cloudOut(new pcl::PointCloud<PointType>());
      PointTypePose thisPose6D = trans2PointTypePose(transformTobeMapped);
      *cloudOut += *transformPointCloud(laserCloudCornerLastDS, &thisPose6D);
      *cloudOut += *transformPointCloud(laserCloudSurfLastDS, &thisPose6D);
      publishCloud(&pubRecentKeyFrame, cloudOut, timeLaserInfoStamp, "odom");
    }
    // publish registered high-res raw cloud
    if (pubCloudRegisteredRaw.getNumSubscribers() != 0) {
      pcl::PointCloud<PointType>::Ptr cloudOut(new pcl::PointCloud<PointType>());
      pcl::fromROSMsg(cloudInfo.cloud_deskewed, *cloudOut);
      PointTypePose thisPose6D = trans2PointTypePose(transformTobeMapped);
      *cloudOut = *transformPointCloud(cloudOut, &thisPose6D);
      publishCloud(&pubCloudRegisteredRaw, cloudOut, timeLaserInfoStamp, "odom");
    }
    // publish path
    if (pubPath.getNumSubscribers() != 0) {
      globalPath.header.stamp = timeLaserInfoStamp;
      globalPath.header.frame_id = "odom";
      pubPath.publish(globalPath);
    }
  }

 public:
  // gtsam
  NonlinearFactorGraph gtSAMgraph;
  Values initialEstimate;
  Values optimizedEstimate;
  ISAM2 *isam;
  Values isamCurrentEstimate;
  Eigen::MatrixXd poseCovariance;

  ros::Publisher pubLaserCloudSurround;
  ros::Publisher pubOdomAftMappedROS;
  ros::Publisher pubKeyPoses;
  ros::Publisher pubPath;

  ros::Publisher pubHistoryKeyFrames;
  ros::Publisher pubIcpKeyFrames;
  ros::Publisher pubRecentKeyFrames;
  ros::Publisher pubRecentKeyFrame;
  ros::Publisher pubCloudRegisteredRaw;

  ros::Subscriber subLaserCloudInfo;
  ros::Subscriber subGPS;

  std::deque<nav_msgs::Odometry> gpsQueue;
  lio_sam::cloud_info cloudInfo;

  vector<pcl::PointCloud<PointType>::Ptr> cornerCloudKeyFrames;
  vector<pcl::PointCloud<PointType>::Ptr> surfCloudKeyFrames;

  pcl::PointCloud<PointType>::Ptr cloudKeyPoses3D;
  pcl::PointCloud<PointTypePose>::Ptr cloudKeyPoses6D;

  pcl::PointCloud<PointType>::Ptr laserCloudCornerLast; // corner feature set from odoOptimization
  pcl::PointCloud<PointType>::Ptr laserCloudSurfLast; // surf feature set from odoOptimization
  pcl::PointCloud<PointType>::Ptr laserCloudCornerLastDS; // downsampled corner featuer set from odoOptimization
  pcl::PointCloud<PointType>::Ptr laserCloudSurfLastDS; // downsampled surf featuer set from odoOptimization

  pcl::PointCloud<PointType>::Ptr laserCloudOri;
  pcl::PointCloud<PointType>::Ptr coeffSel;

  std::vector<PointType> laserCloudOriCornerVec; // corner point holder for parallel computation
  std::vector<PointType> coeffSelCornerVec;
  std::vector<bool> laserCloudOriCornerFlag;
  std::vector<PointType> laserCloudOriSurfVec; // surf point holder for parallel computation
  std::vector<PointType> coeffSelSurfVec;
  std::vector<bool> laserCloudOriSurfFlag;

  pcl::PointCloud<PointType>::Ptr laserCloudCornerFromMap;
  pcl::PointCloud<PointType>::Ptr laserCloudSurfFromMap;
  pcl::PointCloud<PointType>::Ptr laserCloudCornerFromMapDS;
  pcl::PointCloud<PointType>::Ptr laserCloudSurfFromMapDS;

  pcl::KdTreeFLANN<PointType>::Ptr kdtreeCornerFromMap;
  pcl::KdTreeFLANN<PointType>::Ptr kdtreeSurfFromMap;

  pcl::KdTreeFLANN<PointType>::Ptr kdtreeSurroundingKeyPoses;
  pcl::KdTreeFLANN<PointType>::Ptr kdtreeHistoryKeyPoses;

  pcl::PointCloud<PointType>::Ptr latestKeyFrameCloud;
  pcl::PointCloud<PointType>::Ptr nearHistoryKeyFrameCloud;

  pcl::VoxelGrid<PointType> downSizeFilterCorner;
  pcl::VoxelGrid<PointType> downSizeFilterSurf;
  pcl::VoxelGrid<PointType> downSizeFilterICP;
  pcl::VoxelGrid<PointType> downSizeFilterSurroundingKeyPoses; // for surrounding key poses of scan-to-map optimization

  ros::Time timeLaserInfoStamp;
  double timeLaserCloudInfoLast;

  float transformTobeMapped[6];

  std::mutex mtx;

  double timeLastProcessing = -1;

  bool isDegenerate = false;
  Eigen::Matrix<float, 6, 6> matP;

  int laserCloudCornerFromMapDSNum = 0;
  int laserCloudSurfFromMapDSNum = 0;
  int laserCloudCornerLastDSNum = 0;
  int laserCloudSurfLastDSNum = 0;

  bool aLoopIsClosed = false;
  int imuPreintegrationResetId = 0;

  nav_msgs::Path globalPath;

  Eigen::Affine3f transPointAssociateToMap;

  Eigen::Affine3f lastImuTransformation;

};

int main(int argc, char **argv) {
  ros::init(argc, argv, "lio_sam");

  mapOptimization MO;

  ROS_INFO("\033[1;32m----> Map Optimization Started.\033[0m");

  // 两个线程，一边按固定的频率进行回环检测、添加约束边，另外一边进行地图发布和保存
  std::thread loopthread(&mapOptimization::loopClosureThread, &MO);
  std::thread visualizeMapThread(&mapOptimization::visualizeGlobalMapThread, &MO);

  // 这里不间断的执行callback
  ros::spin();

  return 0;
}