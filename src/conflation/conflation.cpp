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
// Date: 12.05.2023
// ========================================== //
//
//
#include "conflation.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

/**************/
/*Constructors*/
/**************/

cconflation::cconflation()
{
}

/****************/
/*public methods*/
/****************/

/************************************************************************
 * Remove tags from points that are auto-generated by VectorMapBuilder
 *************************************************************************/
bool cconflation::remove_tags(lanelet::LaneletMapPtr & map_ptr)
{
  for (auto & pt : map_ptr->pointLayer) {
    // Remove attributes that are no longer valid (if they exist)
    remove_attributes(pt, {"local_x", "local_y", "mgrs_code"});
  }
  return true;
}

/***********************************************************************************
 * Conflate information from OpenStreetMap into existing lanelet map:
 * -> map highway tag from osm to subtype and location tags in lanelet2
 * -> transfer maxspeed tag from osm to speed_limit tag
 * -> transfer name tag from osm to road_name tag
 * -> transfer surface tag from osm to road_surface tag
 * -> transfer oneway tag from osm to one_way tag
 * -> transfer lane_markings tag from osm to lane_markings tag
 * -> colorize lanelets based on accordance between adjacent lanelets (sharing
 * common boundary) and lanes tag in osm
 * remove lanelets that are likely to be wrong if more adjacent lanes than osm lanes tag
 ************************************************************************************/
bool cconflation::conflate_lanelet_OSM(
  lanelet::LaneletMapPtr & map_ptr, std::vector<s_match> & matches,
  std::vector<std::pair<lanelet::Id, std::string>> & cols, lanelet::ConstLanelets & deleted)
{
  const std::vector<std::string> target_keys = {"highway", "maxspeed",      "name",  "oneway",
                                                "surface", "lane_markings", "lanes", "shoulder"};

  // Itearate through matches
  for (auto & match : matches) {
    if (!match.target_pline().empty()) {
      std::vector<lanelet::ConstPoints3d> pts_change;
      std::vector<std::vector<std::string>> values;
      // Check match for a change of one of the tags specified in targetKeys
      // -> if existing, split corresponding lanelets at projected point of change
      split_on_tag_change(map_ptr, match, target_keys, pts_change, values);

      // Set subtype and location tag in lanelet based on mapping from OSM highway tag
      set_type_location(map_ptr, match, pts_change[0], values[0]);

      // Transfer attributes from OSM to their lanelet2 equivalent
      transfer_att(map_ptr, match, "speed_limit", pts_change[1], values[1]);
      transfer_att(map_ptr, match, "road_name", pts_change[2], values[2]);
      transfer_att(map_ptr, match, "one_way", pts_change[3], values[3]);
      transfer_att(map_ptr, match, "road_surface", pts_change[4], values[4]);
      transfer_att(map_ptr, match, "lane_markings", pts_change[5], values[5]);

      // Colorize lanelets based on OSM lanes tag and recognize wrongly mapped lanelets
      /* std::cout << match_.refPline().front().attributes() << std::endl;
      std::cout << match_.refPline().back().attributes() << std::endl;
      for (const auto & val : values[6]) {
        std::cout << val << std::endl;
      }
      for (const auto & val : values[7]) {
        std::cout << val << std::endl;
      } */
      check_lanes(
        map_ptr, match, cols, pts_change[6], pts_change[7], values[6], values[7], deleted);
    }
  }
  std::cout << "\033[33m~~~~~> Set lanelet subtype and location based on OSM highway tag!\033[0m"
            << std::endl;
  std::cout << "\033[33m~~~~~> Transferred maxspeed to speed_limit!\033[0m" << std::endl;
  std::cout << "\033[33m~~~~~> Transferred name to road_name!\033[0m" << std::endl;
  std::cout << "\033[33m~~~~~> Transferred oneway to one_way!\033[0m" << std::endl;
  std::cout << "\033[33m~~~~~> Transferred surface to road_surface!\033[0m" << std::endl;
  std::cout << "\033[33m~~~~~> Transferred lane_markings to lane_markings!\033[0m" << std::endl;
  std::cout << "\033[33m~~~~~> Colorized lanelets based on amount of adjacent lanes!\033[0m"
            << std::endl;
  return true;
}

/**********************************************************************************
 * Create new map with all elements from map_ptr (original map) except the
 * deleted lanelets (no remove option in lanelet2 library)
 ***********************************************************************************/
bool cconflation::create_updated_map(
  const lanelet::LaneletMapPtr & map_ptr, const lanelet::LaneletMapPtr & new_map,
  const lanelet::ConstLanelets & deleted)
{
  // Transfer lanelets except deleted ones
  for (const auto & ll : map_ptr->laneletLayer) {
    if (std::find(deleted.begin(), deleted.end(), ll) == deleted.end()) {
      new_map->add(ll);
    }
  }
  // Transfer all areas
  for (const auto & area : map_ptr->areaLayer) {
    new_map->add(area);
  }
  // Transfer all regulatory elements
  for (const auto & regEl : map_ptr->regulatoryElementLayer) {
    new_map->add(regEl);
  }
  // Transfer all polygons
  for (const auto & poly : map_ptr->polygonLayer) {
    new_map->add(poly);
  }
  // Points and linestrings not transformed since they are included in the other elements
  return true;
}

/*****************/
/*private methods*/
/*****************/

/******************************************************************************
 * Split all adjacent lanelets where a certain tag in openstreetmap changes
 *******************************************************************************/
void cconflation::split_on_tag_change(
  lanelet::LaneletMapPtr & map_ptr, s_match & match, const std::vector<std::string> & target_keys,
  std::vector<lanelet::ConstPoints3d> & pts_change, std::vector<std::vector<std::string>> & values)
{
  // Find points where tags change and store the values
  check_tag_change(match.target_pline(), target_keys, pts_change, values);
  // Merge points where any of the attributes changes (-> avoid duplicate splitting)
  lanelet::ConstPoints3d pts_merged = merge_point_vec(pts_change);
  // Split lanelets where attributes change and update tags in match
  if (!pts_merged.empty()) {
    split_lanelet(map_ptr, match, pts_merged);
  }
}

/************************************************************************
 * Derive lanelet subtype and location based on custom mapping from
 * openstreetmap's highway-tag
 *************************************************************************/
void cconflation::set_type_location(
  lanelet::LaneletMapPtr & map_ptr, const s_match & match,
  const lanelet::ConstPoints3d & pts_change, std::vector<std::string> & values)
{
  lanelet::Ids set_ll_subtype;
  lanelet::Ids set_ll_location;
  if (!values.empty()) {
    // Get indices of segments
    std::vector<int> ind_change;
    for (const auto & pt : pts_change) {
      int index = get_index(match, pt);
      ind_change.push_back(index);
    }
    // Flip values if match polylines are in opposite direction
    if (!same_direction(match)) {
      std::reverse(values.begin(), values.end());
    }
    int ind = 0;
    for (const auto & seg : match.ref_pline()) {
      // Get current value
      std::string val = values.front();
      if (!ind_change.empty()) {
        for (int j = 0; j < static_cast<int>(ind_change.size()); ++j) {
          if (ind >= ind_change[j]) {
            val = values[j + 1];
          }
        }
      }

      std::string subtype, location;
      highway2subtype_location(val, subtype, location);
      // Set vales for lanelets the current segment represents
      set_value_dir("ll_id_forward_", seg, map_ptr, "subtype", subtype, set_ll_subtype);
      set_value_dir("ll_id_backward_", seg, map_ptr, "subtype", subtype, set_ll_subtype);
      set_value_dir("ll_id_forward_", seg, map_ptr, "location", location, set_ll_location);
      set_value_dir("ll_id_backward_", seg, map_ptr, "location", location, set_ll_location);
      ++ind;
    }
  }
}

/********************************************************************************
 * Transfer attribute from openstreetmap to lanelet2-map
 * => transfer attribute
 *********************************************************************************/
void cconflation::transfer_att(
  lanelet::LaneletMapPtr & map_ptr, const s_match & match, const std::string & ref_key,
  const lanelet::ConstPoints3d & pts_change, std::vector<std::string> & values)
{
  // Get indices of segments
  std::vector<int> ind_change;
  for (const auto & pt : pts_change) {
    int index = get_index(match, pt);
    ind_change.push_back(index);
  }
  lanelet::Ids set_ll;
  // Check if attribute is constant
  transfer_tag(match, ref_key, ind_change, values, map_ptr, set_ll);
}

/***********************************************************************************
 * Compare amount of adjacent lanelets (sharing a boundary) in lanelet-map to
 * openstreetmap's lanes tag if available
 * => colorize lanelet in RVIZ based on results (correct = green, wrong = red,
 * no lanes tag = blue, no match = white)
 ************************************************************************************/
void cconflation::check_lanes(
  const lanelet::LaneletMapPtr & map_ptr, s_match & match,
  std::vector<std::pair<lanelet::Id, std::string>> & cols,
  const lanelet::ConstPoints3d & pts_change_lanes,
  const lanelet::ConstPoints3d & pts_change_shoulder, std::vector<std::string> & val_lanes,
  std::vector<std::string> & val_shoulder, lanelet::ConstLanelets & deleted)
{
  // Merge points of lanes and shoulder tag
  std::vector<lanelet::ConstPoints3d> pts_change{pts_change_lanes, pts_change_shoulder};
  const lanelet::ConstPoints3d pts_merged = merge_point_vec(pts_change);
  // Get indices of segments
  std::vector<int> ind_change;
  for (const auto & pt : pts_merged) {
    ind_change.push_back(get_index(match, pt));
  }
  // Flip values if match polylines are in opposite direction
  if (!same_direction(match)) {
    std::reverse(val_lanes.begin(), val_lanes.end());
    std::reverse(val_shoulder.begin(), val_shoulder.end());
  }
  // Create updated value vectors for merged points
  std::vector<std::string> val_lanes_new{val_lanes.front()};
  std::vector<std::string> val_shoulder_new{val_shoulder.front()};
  int count_lanes = 0;
  int count_shoulder = 0;
  for (int i = 0; i < static_cast<int>(pts_merged.size()); ++i) {
    if (
      std::find(pts_change_lanes.begin(), pts_change_lanes.end(), pts_merged[i]) !=
      pts_change_lanes.end()) {
      ++count_lanes;
    }
    if (
      std::find(pts_change_shoulder.begin(), pts_change_shoulder.end(), pts_merged[i]) !=
      pts_change_shoulder.end()) {
      ++count_shoulder;
    }
    val_lanes_new.push_back(val_lanes[count_lanes]);
    val_shoulder_new.push_back(val_shoulder[count_shoulder]);
  }
  // Iterarte through segments, assign color code and detect lonely lanelets
  int ind = 0;
  for (auto & seg : match.ref_pline()) {
    // Get current amount of lanes from osm
    std::string val_lane = val_lanes_new.front();
    std::string val_sh = val_shoulder_new.front();
    if (!ind_change.empty()) {
      for (int j = 0; j < static_cast<int>(ind_change.size()); ++j) {
        if (ind >= ind_change[j]) {
          val_lane = val_lanes_new[j + 1];
          val_sh = val_shoulder_new[j + 1];
        }
      }
    }
    // Count adjacent lanes in lanelet2 map
    int lanes_count = 0;
    count_lanes_dir("ll_id_forward_", seg, lanes_count);
    count_lanes_dir("ll_id_backward_", seg, lanes_count);
    // Set color for lanelets
    std::string col_code = "";
    int lanes_osm = 0;
    if (val_lane != "") {
      // Get lanes from osm -> check for shoulder
      lanes_osm = std::stoi(val_lane);
      if (val_sh == "yes" || val_sh == "left" || val_sh == "right") {
        ++lanes_osm;
      } else if (val_sh == "both") {
        lanes_osm += 2;
      }
      // Compare to adjacent lanes in lanelet map
      if (lanes_count == lanes_osm) {
        col_code = "WEBGreen";
      } else {
        col_code = "WEBRed";
      }
    } else {
      col_code = "WEBBlueLight";
    }
    // Set color code for lanelet ids
    set_color_code_dir("ll_id_forward_", seg, map_ptr, col_code, cols);
    set_color_code_dir("ll_id_backward_", seg, map_ptr, col_code, cols);

    // Find lanelets to be deleted in the next step
    if (val_lane != "") {
      while (lanes_count > lanes_osm) {
        if (find_wrong_lanelet(map_ptr, seg, deleted, match)) {
          --lanes_count;
        } else {
          std::cout << "\033[31m~~~~~> Couldn't identify wrong lanelets clearly! - "
                    << "Skipping segment!\033[0m" << std::endl;
          break;
        }
      }
    }
    ++ind;
  }
}

/*******************************************************************************
 * Check if attributes of a polyline change
 ********************************************************************************/
void cconflation::check_tag_change(
  const lanelet::LineStrings3d & pline, const std::vector<std::string> & keys,
  std::vector<lanelet::ConstPoints3d> & pts, std::vector<std::vector<std::string>> & values)
{
  // Initialize with first value if existing otherwise empty
  for (const auto & key : keys) {
    lanelet::ConstPoints3d pts_;
    std::vector<std::string> values_;
    std::string val = "";
    if (!pline.empty()) {
      if (pline.front().hasAttribute(key)) {
        val = pline.front().attribute(key).value();
      }
    }
    values_.push_back(val);
    // check remaining segments if attribute is existing and changes or disappears
    if (pline.size() > 1) {
      for (auto it = pline.begin() + 1; it != pline.end(); ++it) {
        if (it->hasAttribute(key)) {
          if (it->attribute(key).value() != val) {
            pts_.push_back(it->front());
            val = it->attribute(key).value();
            values_.push_back(val);
          }
        } else {
          if (val != "") {
            pts_.push_back(it->front());
            val = "";
            values_.push_back(val);
          }
        }
      }
    }
    pts.push_back(pts_);
    values.push_back(values_);
  }
}

/********************************************************************************
 * Split lanelets at desired points (point projected on lanelet-bounds)
 *********************************************************************************/
void cconflation::split_lanelet(
  lanelet::LaneletMapPtr & map_ptr, s_match & match, const lanelet::ConstPoints3d & pts)
{
  lanelet::Ids splitted;
  lanelet::LineStrings3d newLss;
  for (const auto & pt : pts) {
    // Find closest segment on reference polyline
    const int ind = get_index(match, pt);
    // Split all lanelets that are represented by this segment
    split_ll_dir(map_ptr, match, ind, "ll_id_forward_", splitted, newLss, pt);
    split_ll_dir(map_ptr, match, ind, "ll_id_backward_", splitted, newLss, pt);
  }
}

/*****************************************************************************
 * Split lanelets for forward/backward-direction of reference polyline
 * => split left and right bound and create new lanelet
 ******************************************************************************/
void cconflation::split_ll_dir(
  const lanelet::LaneletMapPtr & map_ptr, s_match & match, const int & ind, const std::string & key,
  lanelet::Ids & splitted, lanelet::LineStrings3d & newls, const lanelet::ConstPoint3d & pt)
{
  // Set key for current lanelet and iterate
  int i = 1;
  std::string key_ind = key + std::to_string(i);
  // Work with inverted linestrings when splitting them if backward direction
  bool invert = (key.find("backward") != std::string::npos) ? true : false;
  while (match.ref_pline()[ind].hasAttribute(key_ind)) {
    lanelet::Lanelet orig = find_ll(map_ptr, *match.ref_pline()[ind].attribute(key_ind).asId());
    // Split left bound
    lanelet::LineString3d left = orig.leftBound();
    lanelet::LineString3d new_left;
    split_linestring(left, new_left, splitted, newls, pt, invert);
    // Split right bound
    lanelet::LineString3d right = orig.rightBound();
    lanelet::LineString3d new_right;
    split_linestring(right, new_right, splitted, newls, pt, invert);
    lanelet::Lanelet new_ll(lanelet::utils::getId(), new_left, new_right, orig.attributes());
    map_ptr->add(new_ll);
    // Update attribute with id-tag
    match.update_ref_tags(key_ind, orig.id(), new_ll.id(), ind);
    ++i;
    key_ind = key + std::to_string(i);
  }
}

/**********************************************************************
 * Split linestring if it has not been splitted so far
 * => project point to linestring and divide points to two ls
 ***********************************************************************/
void cconflation::split_linestring(
  lanelet::LineString3d & orig_ls, lanelet::LineString3d & new_ls, lanelet::Ids & splitted,
  lanelet::LineStrings3d & new_lss, const lanelet::ConstPoint3d & pt, const bool & invert)
{
  if (invert) {
    orig_ls = orig_ls.invert();
  }
  if (!used_Id(splitted, orig_ls)) {
    // Project split point on original linestring
    lanelet::BasicPoint3d pt_proj = lanelet::geometry::project(orig_ls, pt.basicPoint());
    // Create new point if there is no point on the linestring within a tolerance
    lanelet::Point3d new_pt;
    new_pt.setId(lanelet::utils::getId());
    std::vector<double> d;
    int ind = 0;
    for (const auto & pt_ls : orig_ls) {
      d.push_back(lanelet::geometry::distance(pt_proj, pt_ls.basicPoint()));
    }
    if (*std::min_element(d.begin(), d.end()) < 1e-3) {
      if (std::distance(d.begin(), std::min_element(d.begin(), d.end())) == 0) {
        // Projected point is first point of linestring
        // -> need linestring with at least 2 points -> interpolate point between first
        // two points
        new_pt.x() = (orig_ls[0].x() + orig_ls[1].x()) / 2.0;
        new_pt.y() = (orig_ls[0].y() + orig_ls[1].y()) / 2.0;
        new_pt.z() = (orig_ls[0].z() + orig_ls[1].z()) / 2.0;
        ind = find_segment_2D(new_pt, orig_ls);
      } else if (std::distance(std::min_element(d.begin(), d.end()), d.end()) == 0) {
        // Projected point is last point of linestring
        // -> need linestring with at least 2 points -> interpolate point between last
        // two points
        int sz = orig_ls.size();
        new_pt.x() = (orig_ls[sz - 1].x() + orig_ls[sz].x()) / 2.0;
        new_pt.y() = (orig_ls[sz - 1].y() + orig_ls[sz].y()) / 2.0;
        new_pt.z() = (orig_ls[sz - 1].z() + orig_ls[sz].z()) / 2.0;
        ind = find_segment_2D(new_pt, orig_ls);
      } else {
        new_pt = orig_ls[std::distance(d.begin(), std::min_element(d.begin(), d.end()))];
        ind = std::distance(d.begin(), std::min_element(d.begin(), d.end()));
      }
    } else {
      new_pt.x() = pt_proj.x();
      new_pt.y() = pt_proj.y();
      new_pt.z() = pt_proj.z();
      // Get closest segment on original linestring (first index)
      ind = find_segment_2D(new_pt, orig_ls);
    }
    // Create new linestring starting with newPt and attributes from original ls
    lanelet::LineString3d new_ls_(lanelet::utils::getId(), {new_pt}, orig_ls.attributes());
    // Add points behind the index to newls
    new_ls_.insert(new_ls_.end(), orig_ls.begin() + ind + 1, orig_ls.end());
    // Erase those points from original ls (complicated since erase not overwritten properly)
    int count = ind + 1;
    const int orig_sz = orig_ls.size();
    while (count < orig_sz) {
      orig_ls.pop_back();
      ++count;
    }
    // Add splitting point to original ls and new linestrings to vector for later use
    // if (orig_ls.back().id() != new_pt.id()) {
    //   orig_ls.push_back(new_pt);
    // }
    orig_ls.push_back(new_pt);
    new_ls = new_ls_;
    splitted.push_back(orig_ls.id());
    new_lss.push_back(new_ls);
  } else {
    // original ls was already splitted -> assign the previously splitted linestring
    const int ind =
      std::distance(splitted.begin(), std::find(splitted.begin(), splitted.end(), orig_ls.id()));
    new_ls = new_lss[ind];
  }
  // Invert back if inverted at the beginning
  if (invert) {
    orig_ls = orig_ls.invert();
    new_ls = new_ls.invert();
  }
}

/***************************************************************************
 * Transfer attribute values for a given match
 ****************************************************************************/
void cconflation::transfer_tag(
  const s_match & match, const std::string & key_ref, const std::vector<int> & ind_change,
  std::vector<std::string> & values, lanelet::LaneletMapPtr & map_ptr, lanelet::Ids & setll)
{
  if (!values.empty()) {
    // Flip values if match polylines are in opposite direction
    if (!same_direction(match)) {
      std::reverse(values.begin(), values.end());
    }
    int ind = 0;
    for (const auto & seg : match.ref_pline()) {
      // Get current value
      std::string val = values.front();
      if (!ind_change.empty()) {
        for (int j = 0; j < static_cast<int>(ind_change.size()); ++j) {
          if (ind >= ind_change[j]) {
            val = values[j + 1];
          }
        }
      }
      // Set vales for lanelets the current segment represents
      set_value_dir("ll_id_forward_", seg, map_ptr, key_ref, val, setll);
      set_value_dir("ll_id_backward_", seg, map_ptr, key_ref, val, setll);
      ++ind;
    }
  }
}

/***************************************************************
 * Get index of point in match reference pline
 ****************************************************************/
int cconflation::get_index(const s_match & match, const lanelet::ConstPoint3d & pt)
{
  std::vector<double> d;
  for (const auto & seg : match.ref_pline()) {
    d.push_back(
      lanelet::geometry::distance2d(seg.front(), pt) +
      lanelet::geometry::distance2d(seg.back(), pt));
  }
  return std::distance(d.begin(), std::min_element(d.begin(), d.end()));
}

/*********************************************************
 * Find a lanelet in a vector of lanelets given its id
 **********************************************************/
lanelet::Lanelet cconflation::find_ll(
  const lanelet::LaneletMapPtr & map_ptr, const lanelet::Id & id)
{
  for (const auto & ll : map_ptr->laneletLayer) {
    if (ll.id() == id) {
      return ll;
    }
  }
  std::cerr << __FUNCTION__ << ": \033[1;31m!! Couldn't find lanelet for id !!\033[0m" << std::endl;
  lanelet::Lanelet empty;
  return empty;
}

/*********************************************************
 * Find a lanelet in a vector of lanelets given its bounds
 **********************************************************/
lanelet::Lanelet cconflation::find_ll_from_bound(
  const lanelet::LaneletMapPtr & map_ptr, const lanelet::LineString3d & left,
  const lanelet::LineString3d & right)
{
  for (const auto & ll : map_ptr->laneletLayer) {
    if (ll.leftBound().id() == left.id() && ll.rightBound().id() == right.id()) {
      return ll;
    }
  }
  std::cerr << __FUNCTION__ << ": \033[1;31m!! Couldn't find lanelet for bounds !!\033[0m"
            << std::endl;
  lanelet::Lanelet empty;
  return empty;
}

/***********************************************************************************
 * Find the segment of a linestring a given projected point is in between
 ************************************************************************************/
int cconflation::find_segment_2D(const lanelet::ConstPoint3d & pt, const lanelet::LineString3d & ls)
{
  std::vector<double> diff;
  for (auto it = ls.begin(); it != ls.end() - 1; ++it) {
    double m = ((it + 1)->y() - it->y()) / ((it + 1)->x() - it->x());
    double t = it->y() - m * it->x();
    diff.push_back(std::abs(pt.y() - (m * pt.x() + t)));
  }
  return std::distance(diff.begin(), std::min_element(diff.begin(), diff.end()));
}

/******************************************************************
 * Remove attributes from a given point specified by the keys
 *******************************************************************/
void cconflation::remove_attributes(lanelet::Point3d & pt, const std::vector<std::string> & names)
{
  for (const auto & name : names) {
    auto & attr = pt.attributes();
    auto it = attr.find(name);
    if (it != attr.end()) {
      attr.erase(it);
    }
  }
}

/*****************************************************************************
 * Check if a lanelet id is already present in the vector with color codes
 ******************************************************************************/
bool cconflation::used_Id(
  std::vector<std::pair<lanelet::Id, std::string>> & cols, const lanelet::Lanelet & ll)
{
  for (auto & p : cols) {
    if (p.first == ll.id()) {
      return true;
    }
  }
  return false;
}

/*********************************************
 * Check if a linestring was already used
 **********************************************/
bool cconflation::used_Id(const lanelet::Ids & ids, const lanelet::LineString3d & ls)
{
  if (std::find(ids.begin(), ids.end(), ls.id()) != ids.end()) {
    return true;
  }
  return false;
}

/*********************************************
 * Check if a lanelet was already used
 **********************************************/
bool cconflation::used_Id(const lanelet::Ids & ids, const lanelet::Lanelet & ll)
{
  if (std::find(ids.begin(), ids.end(), ll.id()) != ids.end()) {
    return true;
  }
  return false;
}

/*********************************************
 * Get lanelets of a given map
 **********************************************/
lanelet::Lanelets cconflation::lanelet_layer(const lanelet::LaneletMapPtr & map_ptr)
{
  lanelet::Lanelets lls;
  if (!map_ptr) {
    std::cerr << "No map received!";
    return lls;
  }

  for (const auto & ll : map_ptr->laneletLayer) {
    lls.push_back(ll);
  }
  return lls;
}

/****************************************************************
 * Check if the two plines of a match are in the same direction
 *****************************************************************/
bool cconflation::same_direction(const s_match & match)
{
  if (!match.ref_pline().empty() && !match.target_pline().empty()) {
    Eigen::Vector2d v1, v2;
    v1 << match.ref_pline().back().back().x() - match.ref_pline().front().front().x(),
      match.ref_pline().back().back().y() - match.ref_pline().front().front().y();
    v2 << match.target_pline().back().back().x() - match.target_pline().front().front().x(),
      match.target_pline().back().back().y() - match.target_pline().front().front().y();

    double angle = std::atan2(v1(0) * v2(1) - v2(0) * v1(1), v1.dot(v2));
    if (std::abs(angle) < std::atan(1.0) * 2) {
      return true;
    }
  } else {
    return false;
  }
  return false;
}

/**********************************************************************************
 * Set new attribute value for all forward/backward lanelets that are represented
 * by a linestring segment
 ***********************************************************************************/
void cconflation::set_value_dir(
  const std::string & key, const lanelet::LineString3d & seg,
  const lanelet::LaneletMapPtr & map_ptr, const std::string & key_ref, const std::string & val,
  lanelet::Ids & setll)
{
  int i = 1;
  std::string key_ind = key + std::to_string(i);
  while (seg.hasAttribute(key_ind)) {
    lanelet::Lanelet ll = find_ll(map_ptr, *seg.attribute(key_ind).asId());
    if (!used_Id(setll, ll)) {
      ll.attributes()[key_ref] = val;
      setll.push_back(ll.id());
    }
    ++i;
    key_ind = key + std::to_string(i);
  }
}

/**************************************************************************************
 * Count lanelets represented by a linestring segment in forward/backward direction
 ***************************************************************************************/
void cconflation::count_lanes_dir(
  const std::string & key, const lanelet::LineString3d & seg, int & count)
{
  int i = 1;
  std::string key_ind = key + std::to_string(i);
  while (seg.hasAttribute(key_ind)) {
    ++count;
    ++i;
    key_ind = key + std::to_string(i);
  }
}

/***********************************************************************************
 * Set color code for all forward/backward lanelets that are represented by a
 * linestring segment
 ************************************************************************************/
void cconflation::set_color_code_dir(
  const std::string & key, const lanelet::LineString3d & seg,
  const lanelet::LaneletMapPtr & map_ptr, const std::string & col_code,
  std::vector<std::pair<lanelet::Id, std::string>> & cols)
{
  int i = 1;
  std::string key_ind = key + std::to_string(i);
  while (seg.hasAttribute(key_ind)) {
    lanelet::Lanelet ll = find_ll(map_ptr, *seg.attribute(key_ind).asId());
    if (!used_Id(cols, ll)) {
      std::pair<lanelet::Id, std::string> p(ll.id(), col_code);
      cols.push_back(p);
    }
    ++i;
    key_ind = key + std::to_string(i);
  }
}

/*****************************************************************************************
 * Get lanelet subtype and location from openstreetmap's highway tag (custom definition)
 * -> see Readme for further explanation
 ******************************************************************************************/
void cconflation::highway2subtype_location(
  const std::string & val_osm, std::string & subtype, std::string & location)
{
  const std::vector<std::string> highway_nonurban = {
    "motorway", "trunk", "motorway_link", "trunk_link"};
  const std::vector<std::string> road_urban = {"primary",        "secondary",     "tertiary",
                                               "unclassified",   "residential",   "primary_link",
                                               "secondary_link", "tertiary_link", "service"};
  if (
    std::find(highway_nonurban.begin(), highway_nonurban.end(), val_osm) !=
    highway_nonurban.end()) {
    subtype = "highway";
    location = "nonurban";
  } else if (std::find(road_urban.begin(), road_urban.end(), val_osm) != road_urban.end()) {
    subtype = "road";
    location = "urban";
  } else if (val_osm == "living_street") {
    subtype = "play_street";
    location = "";
  } else if (val_osm == "busway") {
    subtype = "bus_lane";
    location = "urban";
  } else if (val_osm == "cycleway") {
    subtype = "bicycle_lane";
    location = "";
  } else {
    subtype = "";
    location = "";
  }
}

/************************************************************************************
 * Detect lonely lanelets that are wrongly mapped and hence not transferred to
 * output map based on:
 * -> no accordance between current osm-lanes tag and adjacent lanes
 * -> lanelet has neither predeccessor nor successor
 *************************************************************************************/
bool cconflation::find_wrong_lanelet(
  const lanelet::LaneletMapPtr & map_ptr, lanelet::LineString3d & seg,
  lanelet::ConstLanelets & deleted, s_match & match)
{
  // Get lanelets of current segment
  lanelet::Lanelets candidates;
  find_wrong_lanelet_candidates(map_ptr, seg, candidates);
  // Find lanelet most likely to be mapped wrong
  // 1. step => neither predecessor nor successor
  bool found = false;
  lanelet::Lanelet ll_found;
  for (const auto & ll : candidates) {
    bool predecessor = false;
    for (const auto & ll_ : map_ptr->laneletLayer) {
      if (ll_.id() != ll.id()) {
        if (lanelet::geometry::follows(ll_, ll)) {
          predecessor = true;
        }
      }
    }
    bool successor = false;
    for (const auto & ll_ : map_ptr->laneletLayer) {
      if (ll_.id() != ll.id()) {
        if (lanelet::geometry::follows(ll, ll_)) {
          successor = true;
        }
      }
    }
    if (!predecessor && !successor) {
      found = true;
      ll_found = ll;
      break;
    }
  }
  // Found lanelet to remove
  // => Add lanelet to deleted vector and remove attribut from corresponding reference line
  if (found) {
    deleted.push_back(ll_found);
    match.remove_tag_ref_pline(ll_found.id());
    return true;
  } else {
    return false;
  }
}

/************************************************************************************
 * Detect lonely lanelets in direction => see findLonelyLanelet for more details
 *************************************************************************************/
void cconflation::find_wrong_lanelet_candidates(
  const lanelet::LaneletMapPtr & map_ptr, lanelet::LineString3d & seg, lanelet::Lanelets & cand)
{
  for (const auto & attr : seg.attributes()) {
    if (
      attr.first.find("forward") != std::string::npos ||
      attr.first.find("backward") != std::string::npos) {
      cand.push_back(find_ll(map_ptr, *attr.second.asId()));
    }
  }
}

/***************************************************************
 * Merge vector of a vector of points to a single vector
 ****************************************************************/
lanelet::ConstPoints3d cconflation::merge_point_vec(
  std::vector<lanelet::ConstPoints3d> & pts_change)
{
  lanelet::ConstPoints3d pts_merged;
  for (const auto & vec : pts_change) {
    for (const auto & pt : vec) {
      if (std::find(pts_merged.begin(), pts_merged.end(), pt) == pts_merged.end()) {
        pts_merged.push_back(pt);
      }
    }
  }
  return pts_merged;
}
