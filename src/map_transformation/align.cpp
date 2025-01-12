// Copyright 2023 Maximilian Leitenstern
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// ========================================== //
// Author: Maximilian Leitenstern (TUM)
// Date: 20.03.2023
// ========================================== //
//
//
#include "align.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

/**************/
/*Constructors*/
/**************/

calign::calign()
{
}

/****************/
/*public methods*/
/****************/

/*********************************************************************
 * Calculate transformation between two linestrings by the selected
 * algorithm
 *********************************************************************/
bool calign::get_transformation(
  rclcpp::Node & node, const lanelet::ConstLineString3d & src,
  const lanelet::ConstLineString3d & target, Eigen::Matrix3d & trans, const std::string & type)
{
  const std::string node_name = node.get_parameter("node_name").as_string();
  if (type == "ICP") {
    point_transformation_icp(src, target, trans);
  } else if (type == "Umeyama") {
    point_transformation_umeyama(src, target, trans, node);
  } else {
    RCLCPP_ERROR(rclcpp::get_logger(node_name), "!! Registration method not supported !!");
    return false;
  }
  return true;
}

/*******************************************************************
 * Transform whole map according to transformation matrix
 ********************************************************************/
bool calign::transform_map(const lanelet::LaneletMapPtr & map_ptr, const Eigen::Matrix3d & trans)
{
  for (auto & pt : map_ptr->pointLayer) {
    // Transform point
    transform_pt(pt, trans);
  }
  return true;
}

/*******************************************************************
 * Transform linestring according to given transformation (2D)
 ********************************************************************/
bool calign::transform_ls(
  lanelet::ConstLineString3d & ls, lanelet::ConstLineString3d & ls_trans,
  const Eigen::Matrix3d & trans)
{
  lanelet::LineString3d ls_t(lanelet::utils::getId(), {});
  // Transform geometry
  for (auto & pt : ls) {
    const Eigen::Vector3d point(pt.x(), pt.y(), 1.0);
    const Eigen::Vector3d pt_trans = trans.inverse() * point;
    lanelet::Point3d pt_t(lanelet::utils::getId(), {pt_trans(0), pt_trans(1), 0.0});
    ls_t.push_back(pt_t);
  }
  ls_trans = ls_t;
  return true;
}

/*****************/
/*private methods*/
/*****************/

/*************************************************************************
 * Calculate transformation matrix according to Umeyama algorithm
 * => interpolate linestrings to get same amount of points
 **************************************************************************/
bool calign::point_transformation_umeyama(
  const lanelet::ConstLineString3d & src, const lanelet::ConstLineString3d & target,
  Eigen::Matrix3d & trans, rclcpp::Node & node)
{
  // Convert to eigen-matrices to use eigen-functions
  const int num_inter_ume = node.get_parameter("align_num_inter_ume").as_int();
  const Eigen::MatrixXd src_mat = ls2interp_mat2d(src, num_inter_ume);
  const Eigen::MatrixXd target_mat = ls2interp_mat2d(target, num_inter_ume);

  // Umeyama Transformation without scaling
  trans = Eigen::umeyama(src_mat, target_mat, false);

  // Calculate scaling factor between poses and GPS trajectory
  // Apply Umeyama-algorithm with scaling => R_scal = c * R
  const Eigen::MatrixXd trans_scaling = Eigen::umeyama(src_mat, target_mat);
  const double scale = trans_scaling(0, 0) / trans(0, 0);

  if (scale < 0.95) {
    std::cout << "\033[1;31m!! High scaling factor between poses and GPS data. "
              << "Are you sure they belong together? !!\033[0m" << std::endl;
  }
  return true;
}

/***********************************************************************
 * Calculate transformation matrix according to ICP algorithm
 * => from PCL (prior conversion of linestrings to point clouds)
 ************************************************************************/
bool calign::point_transformation_icp(
  const lanelet::ConstLineString3d & src, const lanelet::ConstLineString3d & target,
  Eigen::Matrix3d & trans)
{
  int s_sz = src.size();
  int t_sz = target.size();

  // Create point clouds for application of pcl library and set points
  // only 2d information of points used, since no 3d information from trajectory
  pcl::PointCloud<pcl::PointXYZ>::Ptr src_cloud(new pcl::PointCloud<pcl::PointXYZ>(s_sz, 1));
  pcl::PointCloud<pcl::PointXYZ>::Ptr target_cloud(new pcl::PointCloud<pcl::PointXYZ>(t_sz, 1));
  pcl::PointCloud<pcl::PointXYZ> res;

  ls2PC2d(src, src_cloud);
  ls2PC2d(target, target_cloud);

  // Apply ICP-algorithm from pcl
  pcl::IterativeClosestPoint<pcl::PointXYZ, pcl::PointXYZ> icp;
  icp.setInputSource(src_cloud);
  icp.setInputTarget(target_cloud);
  // Set convergence criteria
  // icp.setMaximumIterations(30);
  // icp.setTransformationEpsilon (1e-8);
  // icp.setEuclideanFitnessEpsilon (1);
  icp.align(res);

  if (!icp.hasConverged()) {
    PCL_ERROR("!! ICP has not converged - Continuing... !!");
  }

  // Convert to 3d matrix (ICP only working for 3d => 4x4 matrix)
  const Eigen::Matrix4d trans_icp = icp.getFinalTransformation().cast<double>();
  trans.block(0, 0, 2, 2) = trans_icp.block(0, 0, 2, 2);
  trans.block(0, 2, 2, 1) = trans_icp.block(0, 3, 2, 1);
  trans.block(2, 0, 1, 3) << 0, 0, 1;
  return true;
}

/*************************************************************************************
 * Transforming the coordinates of a point according to transformation matrix (2D)
 **************************************************************************************/
void calign::transform_pt(lanelet::Point3d & pt, const Eigen::Matrix3d & trans)
{
  const Eigen::Vector3d point(pt.x(), pt.y(), 1.0);
  // const Eigen::Vector3d point(-pt.y(), pt.x(), 1.0);
  const Eigen::Vector3d pt_trans = trans.inverse() * point;
  pt.x() = pt_trans(0);
  pt.y() = pt_trans(1);
  pt.z() = pt.z();
}

/*********************************************************************************
 * Interpolation of linestring to linestring with defined number of points
 **********************************************************************************/
Eigen::MatrixXd calign::ls2interp_mat2d(const lanelet::ConstLineString3d & ls, const int & num)
{
  // Initialize number of interpolation points and output matrix
  Eigen::MatrixXd mat(2, num);

  // Convert to 2D, since GPS trajectory has only 2D information
  const lanelet::ConstLineString2d ls_2d = lanelet::utils::to2D(ls);

  // Interpolate trajectory and poses so that they have the same length
  const double ls_len = lanelet::geometry::length(ls_2d);
  Eigen::VectorXd pt_interp;
  pt_interp.setLinSpaced(num, 0.0, ls_len);

  for (int i = 0; i < num; ++i) {
    lanelet::BasicPoint2d pt = lanelet::geometry::interpolatedPointAtDistance(ls_2d, pt_interp(i));
    mat.col(i) << pt.x(), pt.y();
  }
  return mat;
}

/*********************************************************
 * Convert linestring to pointcloud from pcl
 **********************************************************/
bool calign::ls2PC2d(
  const lanelet::ConstLineString3d & ls, pcl::PointCloud<pcl::PointXYZ>::Ptr & pc)
{
  const int sz = ls.size();

  // Set values
  for (int i = 0; i < sz; i++) {
    pc->points[i].x = ls[i].x();
    pc->points[i].y = ls[i].y();
    pc->points[i].z = 0.0;
  }
  return true;
}

/************************************************************************************
 * Extract intersection nodes (nodes with valence > 2) from openstreetmap-network
 *************************************************************************************/
void calign::get_intersection_nodes(
  const lanelet::ConstLineStrings3d & osm_ls, lanelet::ConstPoints3d & pt)
{
  // Initialize point vector
  lanelet::ConstPoints3d ls_pts;
  int is_intersection = 0;

  // Iterate throughh ls and check Ids
  for (const auto ls : osm_ls) {
    for (const auto point : ls) {
      for (const auto ls_pt : ls_pts) {
        if (point.id() == ls_pt.id()) {
          is_intersection = 1;
        }
      }
      if (is_intersection) {
        pt.push_back(point);
      } else {
        ls_pts.push_back(point);
      }
      is_intersection = 0;
    }
  }
}
