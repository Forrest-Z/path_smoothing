//
// Created by yangt on 19-2-25.
//

#ifndef PATH_SMOOTHING_PATH_SMOOTHING_IN_HPP
#define PATH_SMOOTHING_PATH_SMOOTHING_IN_HPP

namespace path_smoothing {

template<class PathElement>
PathSmoothing *PathSmoothing::createSmoother(const Options &options,
                                             const std::vector<PathElement> &path) {
    CHECK_GT(path.size(), 2) << "path contains less than 3 points!";
    switch (options.smoother_type) {
        case CONJUGATE_GRADIENT_METHOD: {
            return new CgSmoothing(options, path);
        }
        case NON_DERIVATIVE_METHOD: {
            return new NonDerivativeSmoothing(options,
                                              convertToCirclePath(options,
                                                                  path));
        }
        case GAUSS_PROCESS_METHOD: {
#ifdef GPMP2_SMOOTHING_ENABLE
            return new GpSmoothing(options, path);
#else
            LOG(ERROR)
                    << "gpmp2 smoothing is not supported unless you have installed gtsam and gpmp2 libraries!!";
            return nullptr;
#endif
        }
    }
}

template<class PathElement>
CgSmoothing::CgSmoothing(const Options &options,
                         const std::vector<PathElement> &path)
        : PathSmoothing(path.size()) {
    settings_.heading_term_coe = options.cg_heading_term_coe;
    settings_.curvature_term_coe = options.cg_curvature_term_coe;
    settings_.obstacle_term_coe = options.cg_obstacle_term_coe;
    settings_.type = options.cg_difference_type;
    settings_.degree = 2;
    settings_.param_num = (path.size() - 2) * settings_.degree;
    settings_.start.resize(settings_.degree);
    settings_.start << getX<double>(path.front()), getY<double>(path.front());
    settings_.end.resize(settings_.degree);
    settings_.end << getX<double>(path.back()), getY<double>(path.back());
    settings_.function_ = options.function;

    params_.resize(settings_.param_num);
    for (int i(1); i < path.size() - 1; ++i) {
        const int j = i - 1;
        params_(j * settings_.degree) = getX<double>(path.at(i));
        params_(j * settings_.degree + 1) = getY<double>(path.at(i));
    }
}

template<class PathElement>
std::vector<hmpl::Circle> PathSmoothing::convertToCirclePath(
        const Options &options, const std::vector<PathElement> &path) {
    DistanceFunction2D *distance_func = options.function;
    std::vector<hmpl::Circle> circle_path;
    hmpl::Circle circle;
    for (int i(0); i < path.size(); ++i) {
        grid_map::Position
                pos(getX<double>(path.at(i)), getY<double>(path.at(i)));
        circle.position.x = pos(0);
        circle.position.y = pos(1);
        circle.r = distance_func->getObstacleDistance(pos);
        if (circle_path.empty()) {
            circle_path.push_back(circle);
        } else if (circle_path.back().position.Distance(circle.position)
                >= circle_path.back().r) {
            circle_path.push_back(circle);
        }
    }
}

template<class PathElement>
void PathSmoothing::getPointPath(std::vector<PathElement> *path) {
    path->clear();
    PathElement point;
    for (int i = 0; i < pathSize(); ++i) {
        point.x = x(i);
        point.y = y(i);
        path->push_back(point);
    }
}

template<class PoseType>
void PathSmoothing::getPosePath(std::vector<PoseType> *path) {
    path->clear();
    PoseType pose;
    for (int i = 0; i < pathSize(); ++i) {
        pose.position.x = x(i);
        pose.position.y = y(i);
        path->push_back(pose);
    }
}

#ifdef GPMP2_SMOOTHING_ENABLE
template<class PathElement>
GpSmoothing::GpSmoothing(const Options &options,
                         const std::vector<PathElement> &path)
        : PathSmoothing(path.size()) {
    // set robot model
    gtsam::Vector3 zero_vec(0.0, 0.0, 0.0);
    gpmp2::BodySphere sphere(0, 1.2, gtsam::Point3(zero_vec));
    gpmp2::BodySphereVector sphere_vec;
    sphere_vec.push_back(sphere);
    gpmp2::Pose2MobileBaseModel robot(gpmp2::Pose2MobileBase(), sphere_vec);

    //set noise model:
    auto Qc_model =
            gtsam::noiseModel::Gaussian::Covariance(Eigen::Matrix3d::Identity());
    auto pose_fix = gtsam::noiseModel::Isotropic::Sigma(robot.dof(), 1e-5);
    auto vel_fix = gtsam::noiseModel::Isotropic::Sigma(robot.dof(), 1e-5);

    gtsam::Vector avg_vel(3);
    avg_vel << getX<double>(path.back()) - getX<double>(path.front()),
            getY(path.back()) - getY(path.front()), 0;
    avg_vel = avg_vel / pathSize();

    //set initial values and build graph
    for (int i = 0; i < pathSize(); ++i) {
        gtsam::Key pose_key = i;
        gtsam::Key vel_key = i + pathSize();
        double heading;
        if (i == pathSize() - 1) {
            heading = atan2(getY<double>(path.at(i)) - getY<double>(path.at(i - 2)),
                            getX<double>(path.at(i)) - getX<double>(path.at(i - 2)));
        } else {
            heading = atan2(getY<double>(path.at(i + 1)) - getY<double>(path.at(i)),
                            getX<double>(path.at(i + 1)) - getX<double>(path.at(i)));
        }
        gtsam::Pose2 current_pose(getX<double>(path.at(i)), getY<double>(path.at(i)), heading);
        initial_guess.insert(pose_key, current_pose);
        initial_guess.insert(vel_key, avg_vel);
        // start and goal fix factor
        if (i == 0) {
            graph_.add(gtsam::PriorFactor<gtsam::Pose2>(pose_key,
                                                        current_pose,
                                                        pose_fix));
            graph_.add(gtsam::PriorFactor<gtsam::Vector>(vel_key,
                                                         gtsam::Vector::Zero(3),
                                                         vel_fix));
        } else if (i == pathSize() - 1) {
            graph_.add(gtsam::PriorFactor<gtsam::Pose2>(pose_key,
                                                        current_pose,
                                                        pose_fix));
            graph_.add(gtsam::PriorFactor<gtsam::Vector>(vel_key,
                                                         gtsam::Vector::Zero(3),
                                                         vel_fix));
        }
        // obstacle factor
        graph_.add(gpmp2::ObstacleFactor<gpmp2::Pose2MobileBaseModel>(pose_key,
                                                                      robot,
                                                                      *(options.function),
                                                                      options.gp_obs_sigma));
        // vehicle dynamic:
        graph_.add(gpmp2::VehicleDynamicsFactorPose2(pose_key,
                                                     vel_key,
                                                     options.gp_vehicle_dynamic_sigma));
        if (i > 0) {
            gtsam::Key pose_key0 = pose_key - 1;
            gtsam::Key vel_key0 = vel_key - 1;
            //GP priors
            graph_.add(gpmp2::GaussianProcessPriorPose2(pose_key0,
                                                        vel_key0,
                                                        pose_key,
                                                        vel_key,
                                                        options.gp_dt,
                                                        Qc_model));
        }
    }
}
#endif

}

#endif //PATH_SMOOTHING_PATH_SMOOTHING_IN_HPP