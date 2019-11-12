//
// Created by bob on 18-10-19.
//

#include "Communication.h"

Communication::Communication(ros::NodeHandle nroshndl) {
    vel_pub = nroshndl.advertise<geometry_msgs::Twist>("/navigation/cmd_vel", 1);
    amcl_pose_sub = nroshndl.subscribe<geometry_msgs::PoseWithCovarianceStamped>("/amcl_pose", 1,
                                                                                 &Communication::getAmclCallback,
                                                                                 this);
    odom_sub = nroshndl.subscribe<nav_msgs::Odometry>("/navigation/odom", 1, &Communication::getOdomCallback, this);
    obstacles_sub = nroshndl.subscribe<ed_gui_server::objsPosVel>("/ed/gui/objectPosVel", 1,
                                                                  &Communication::getObstaclesCallback, this);
    //ros::Subscriber goal_cmd_sub = nroshndl.subscribe<geometry_msgs::PoseStamped>("/route_navigation/simple_goal", 10, simpleGoalCallback);
    ropod_debug_plan_sub = nroshndl.subscribe<ropod_ros_msgs::RoutePlannerResult>("/ropod/debug_route_plan", 1,
                                                                                  &Communication::getDebugRoutePlanCallback,
                                                                                  this);
    scan_sub = nroshndl.subscribe<sensor_msgs::LaserScan>("/navigation/scan", 1,
                                                          &Communication::getLaserScanCallback, this);

    nroshndl.getParam("/napoleon_navigation/global_frame_id", globalFrame);
    nroshndl.getParam("/napoleon_navigation/base_frame_id", baseFrame);
    nroshndl.getParam("/napoleon_navigation/odom_frame_id", odomFrame);

    vector<double> footprint;
    nroshndl.getParam("/napoleon_navigation/footprint_param", footprint);
    for (int i = 0; i < footprint.size() / 2; i++) {
        footprint_param.emplace_back(Vector2D(footprint[i * 2], footprint[i * 2 + 1]));
    }
    cout << "Footprint:" << endl;
    for (Vector2D &v : footprint_param) { v.print(" -"); }
    vector<double> footprintMiddlePose;
    nroshndl.getParam("/napoleon_navigation/footprintMiddlePose_param", footprintMiddlePose);
    footprintMiddlePose_param = Pose2D(footprintMiddlePose[0], footprintMiddlePose[1], footprintMiddlePose[2]);
    footprintMiddlePose_param.print("Footprint middle pose:");
    nroshndl.getParam("/napoleon_navigation/maxSpeed_param", maxSpeed_param);
    cout << "Max speed: " << maxSpeed_param << endl;
    nroshndl.getParam("/napoleon_navigation/maxAcceleration_param", maxAcceleration_param);
    cout << "Max Acceleration: " << maxAcceleration_param << endl;
    nroshndl.getParam("/napoleon_navigation/wheelDistanceMiddle_param", wheelDistanceMiddle_param);
    cout << "Wheel distance middle: " << wheelDistanceMiddle_param << endl;
    nroshndl.getParam("/napoleon_navigation/tubeWallOffset_param", tubeWallOffset_param);
    cout << "Tube wall offset: " << tubeWallOffset_param << endl;
    nroshndl.getParam("/napoleon_navigation/tubeExtraSpace_param", tubeExtraSpace_param);
    cout << "Tube Extra space: " << tubeExtraSpace_param << endl;
    nroshndl.getParam("/napoleon_navigation/nTries_param", nTries_param);
    cout << "Number of prediction tries: " << nTries_param << endl;
    nroshndl.getParam("/napoleon_navigation/predictionTime_param", predictionTime_param);
    cout << "Prediction time: " << predictionTime_param << endl;
    nroshndl.getParam("/napoleon_navigation/minPredictionDistance_param", minPredictionDistance_param);
    cout << "Minimal prediction distance: " << minPredictionDistance_param << endl;
}

void Communication::getOdomCallback(const nav_msgs::OdometryConstPtr &odom_msg){

    bool validPose = false;
    tf::StampedTransform poseTransform;
    try {
        tf_listener.lookupTransform(globalFrame, baseFrame, ros::Time(0), poseTransform);
        tf::Quaternion q = poseTransform.getRotation();
        tf::Matrix3x3 matrix ( q );
        double roll, pitch, yaw;
        matrix.getRPY ( roll, pitch, yaw );
        measuredPose = Pose2D(poseTransform.getOrigin().x(), poseTransform.getOrigin().y(), yaw);
        validPose = true;
    } catch(tf::TransformException &exception) {
        if(initializedOdometry) {ROS_ERROR("%s", exception.what());}
    }

//    bool validTwist = false;
//    geometry_msgs::Twist odomTwist;
//    try{
//        tf_listener.lookupTwist(baseFrame, odomFrame, ros::Time(0), ros::Duration(1), odomTwist);
//        measuredVelocity = Pose2D(odomTwist.linear.x, odomTwist.linear.y, odomTwist.angular.z);
//        validTwist = true;
//    } catch (tf::TransformException &exception){
//        if(initializedOdometry) {ROS_ERROR("%s", exception.what());}
//    }

    geometry_msgs::Twist twist_msg = odom_msg->twist.twist;
    if(measuredVelocityList.size() >= velocityAverageSamples){measuredVelocityList.pop_front();}
    measuredVelocityList.emplace_back(Pose2D(twist_msg.linear.x, twist_msg.linear.y, twist_msg.angular.z));
    Pose2D addedVelocities;
    for(Pose2D &v : measuredVelocityList){
        addedVelocities = addedVelocities + v;
    }
    measuredVelocity = addedVelocities / measuredVelocityList.size();

    if(!initializedOdometry && validPose){
        measuredVelocity.print("Initial velocity:");
        measuredPose.print("Initial Pose:");
        //cout << "Uncertainty: " << a << " " << b << " " << c << " " << d << endl;
        initializedOdometry = true;
        checkInitialized();
    }
    odometryUpdated = true;
}

void Communication::getAmclCallback(const geometry_msgs::PoseWithCovarianceStampedConstPtr &pose_msg){

    geometry_msgs::Quaternion q_msg = pose_msg->pose.pose.orientation;
    geometry_msgs::Point p_msg = pose_msg->pose.pose.position;
    tf::Quaternion q;
    tf::quaternionMsgToTF(q_msg, q);
    tf::Matrix3x3 matrix ( q );
    double roll, pitch, yaw;
    matrix.getRPY ( roll, pitch, yaw );
    measuredPoseAmcl = Pose2D(p_msg.x, p_msg.y, yaw);

    geometry_msgs::PoseWithCovariance pose = pose_msg->pose;
    double a = pose.covariance[0];
    double b = pose.covariance[1];
    double c = pose.covariance[6];
    double d = pose.covariance[7];

    double D = a*d-b*c;
    double T = a+d;

    //eigenvalues X Y
    double L1 = T/2 + sqrt((T*T)/4-D);
    double L2 = T/2 - sqrt((T*T)/4-D);

    //eigenvectors X Y
    Vector2D E1, E2;
    if(abs(c) > 0.001){
        E1 = Vector2D(L1-d, c);
        E2 = Vector2D(L2-d, c);
    }else if(abs(b) > 0.001){
        E1 = Vector2D(b, L1-a);
        E2 = Vector2D(b, L2-a);
    }else if(abs(b) <= 0.001 && abs(c) <= 0.001){
        E1 = Vector2D(1, 0);
        E2 = Vector2D(0, 1);
    }

    E1.unitThis();
    E2.unitThis();

    poseUncertainty = Ellipse(measuredPoseAmcl.toVector(), sqrt(L1), sqrt(L2), E1.angle());

    if (!initializedPositionAmcl) {
        cout << "Initial pose Amcl: " << measuredPoseAmcl.x << " " << measuredPoseAmcl.y << " " << measuredPoseAmcl.a << endl;
        initializedPositionAmcl = true;
        checkInitialized();
    }
    positionAmclUpdated = true;
}

void Communication::getObstaclesCallback(const ed_gui_server::objsPosVelConstPtr &obstacles_msg) {
    obstacles.obstacles.clear();
    for(auto & obstacle : obstacles_msg->objects){
        if(obstacle.rectangle.probability > obstacle.circle.probability) {
            double x = obstacle.rectangle.pose.position.x;
            double y = obstacle.rectangle.pose.position.y;
            double angle = obstacle.rectangle.yaw;

            Pose2D pose = Pose2D(x, y, angle);
            Vector2D p1 = Vector2D(-obstacle.rectangle.width / 2, -obstacle.rectangle.depth / 2);
            Vector2D p2 = Vector2D(obstacle.rectangle.width / 2, -obstacle.rectangle.depth / 2);
            Vector2D p3 = Vector2D(obstacle.rectangle.width / 2, obstacle.rectangle.depth / 2);
            Vector2D p4 = Vector2D(-obstacle.rectangle.width / 2, obstacle.rectangle.depth / 2);
            Polygon shape = Polygon({p1, p2, p3, p4}, Closed);

            Obstacle obs = Obstacle(shape, pose, Dynamic);
            obs.movement = Pose2D(obstacle.rectangle.vel.x, obstacle.rectangle.vel.y, 0);
            obstacles.obstacles.emplace_back(obs);
        }else{
            double x = obstacle.circle.pose.position.x;
            double y = obstacle.circle.pose.position.y;
            double r = obstacle.circle.radius;

            Pose2D pose = Pose2D(x,y,0);
            Circle circle(Vector2D(0,0),r);
            Polygon shape = circle.toPoints(8);

            Obstacle obs = Obstacle(shape, pose, Dynamic);
            obs.movement = Pose2D(obstacle.rectangle.vel.x, obstacle.rectangle.vel.y, 0);
            obstacles.obstacles.emplace_back(obs);
        }
    }
}

void Communication::setVel(geometry_msgs::Twist cmd_vel_msg){
    vel_pub.publish(cmd_vel_msg);
}

void Communication::getDebugRoutePlanCallback(const ropod_ros_msgs::RoutePlannerResultConstPtr &routeData){
    route = *routeData;
    ROS_INFO("new debug plan received");
    planUpdated = true;
}

void Communication::getLaserScanCallback(const sensor_msgs::LaserScan::ConstPtr& scan_msg){
    bool valid = false;
    try{// Determine absolute laser pose based on TF
        tf::StampedTransform t_sensor_pose;
        tf_listener.lookupTransform(globalFrame, scan_msg->header.frame_id, scan_msg->header.stamp, t_sensor_pose);

        tf::Quaternion q = t_sensor_pose.getRotation();
        tf::Matrix3x3 matrix ( q );
        double rollSensor, pitchSensor, yawSensor;
        matrix.getRPY ( rollSensor, pitchSensor, yawSensor );

        double scan_size =  scan_msg->ranges.size();
        laserPoints.clear();
        for(unsigned int iScan = 0; iScan < scan_size; iScan ++){
            double angle = yawSensor + scan_msg->angle_min + scan_msg->angle_increment*iScan;
            double x = t_sensor_pose.getOrigin().x() + scan_msg->ranges[iScan]*cos( angle );
            double y = t_sensor_pose.getOrigin().y() + scan_msg->ranges[iScan]*sin( angle );
            laserPoints.emplace_back(Vector2D(x,y));
        }
        valid = true;
    }
    catch(tf::ConnectivityException& exc){
        //ROS_WARN("ConnectivityException LaserScan > %s", exc.what());
    }
    catch(tf::ExtrapolationException& exc){
        //ROS_WARN("ExtrapolationException LaserScan > %s", exc.what());
    }
    catch(tf::TransformException& exc){
        //ROS_WARN("TransformException LaserScan > %s", exc.what());
    }

    if(!laserPoints.empty() && valid){
        obstacles.obstacles.clear();
        int counter = 0;
        Vector2D p1, p2;
        for(int i = 0; i < laserPoints.size()-1; i++){
            if(counter == 0){p1 = laserPoints[i];}
            if((laserPoints[i]-laserPoints[i+1]).length() < 0.1 && counter <= 20){
                counter++;
                p2 = laserPoints[i+1];
            }else{
                if(counter >= 5){
                    obstacles.obstacles.emplace_back(Obstacle(Polygon({p1, p2}, Open), Pose2D((p1+p2)/2, 0), Static));
                }
                counter = 0;
            }
        }
        if(!initializedScan){
            initializedScan = true;
            cout << "Scan initialized" << endl;
            checkInitialized();
        }
    }
}

void Communication::checkInitialized(){
    initialized = initializedOdometry && initializedScan;
}

bool Communication::newAmclPosition(){
    bool temp = positionAmclUpdated;
    positionAmclUpdated = false;
    return temp;
}

bool Communication::newOdometry(){
    bool temp = odometryUpdated;
    odometryUpdated = false;
    return temp;
}

bool Communication::newPlan(){
    bool temp = planUpdated;
    planUpdated = false;
    return temp;
}
