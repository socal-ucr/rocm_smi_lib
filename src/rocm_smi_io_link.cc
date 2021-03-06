/*
 * =============================================================================
 * The University of Illinois/NCSA
 * Open Source License (NCSA)
 *
 * Copyright (c) 2020, Advanced Micro Devices, Inc.
 * All rights reserved.
 *
 * Developed by:
 *
 *                 AMD Research and AMD ROC Software Development
 *
 *                 Advanced Micro Devices, Inc.
 *
 *                 www.amd.com
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal with the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 *  - Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimers.
 *  - Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimers in
 *    the documentation and/or other materials provided with the distribution.
 *  - Neither the names of <Name of Development Group, Name of Institution>,
 *    nor the names of its contributors may be used to endorse or promote
 *    products derived from this Software without specific prior written
 *    permission.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS WITH THE SOFTWARE.
 *
 */

#include <assert.h>
#include <sys/stat.h>
#include <dirent.h>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <fstream>
#include <cstdint>
#include <iostream>
#include <sstream>

#include "rocm_smi/rocm_smi.h"
#include "rocm_smi/rocm_smi_exception.h"
#include "rocm_smi/rocm_smi_utils.h"
#include "rocm_smi/rocm_smi_io_link.h"

namespace amd {
namespace smi {

static const char *kKFDNodesPathRoot = "/sys/class/kfd/kfd/topology/nodes";

// IO Link Property strings
static const char *kIOLinkPropTYPEStr =  "type";
// static const char *kIOLinkPropVERSION_MAJORStr = "version_major";
// static const char *kIOLinkPropVERSION_MINORStr = "version_minor";
static const char *kIOLinkPropNODE_FROMStr = "node_from";
static const char *kIOLinkPropNODE_TOStr = "node_to";
static const char *kIOLinkPropWEIGHTStr = "weight";
// static const char *kIOLinkPropMIN_LATENCYStr = "min_latency";
// static const char *kIOLinkPropMAX_LATENCYStr = "max_latency";
// static const char *kIOLinkPropMIN_BANDWIDTHStr = "min_bandwidth";
// static const char *kIOLinkPropMAX_BANDWIDTHStr = "max_bandwidth";
// static const char *kIOLinkPropRECOMMENDED_TRANSFER_SIZEStr =
// "recommended_transfer_size";
// static const char *kIOLinkPropFLAGSStr = "flags";

static bool is_number(const std::string &s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), ::isdigit);
}

static std::string IOLinkPathRoot(uint32_t node_indx) {
  std::string io_link_path = kKFDNodesPathRoot;
  io_link_path += '/';
  io_link_path += std::to_string(node_indx);
  io_link_path += '/';
  io_link_path += "io_links";
  return io_link_path;
}

static std::string IOLinkPath(uint32_t node_indx, uint32_t link_indx) {
  std::string io_link_path = IOLinkPathRoot(node_indx);
  io_link_path += '/';
  io_link_path += std::to_string(link_indx);
  return io_link_path;
}

static int OpenIOLinkProperties(uint32_t node_indx, uint32_t link_indx,
                                std::ifstream *fs) {
  int ret;
  std::string f_path;
  bool reg_file;

  assert(fs != nullptr);
  if (fs == nullptr) {
    return EINVAL;
  }

  f_path = IOLinkPath(node_indx, link_indx);
  f_path += "/";
  f_path += "properties";

  ret = isRegularFile(f_path, &reg_file);

  if (ret != 0) {
    return ret;
  }
  if (!reg_file) {
    return ENOENT;
  }

  fs->open(f_path);

  if (!fs->is_open()) {
    return errno;
  }

  return 0;
}

static int ReadIOLinkProperties(uint32_t node_indx, uint32_t link_indx,
                                std::vector<std::string> *retVec) {
  std::string line;
  int ret;
  std::ifstream fs;

  assert(retVec != nullptr);
  if (retVec == nullptr) {
    return EINVAL;
  }

  ret = OpenIOLinkProperties(node_indx, link_indx, &fs);

  if (ret) {
    return ret;
  }

  while (std::getline(fs, line)) {
    retVec->push_back(line);
  }

  if (retVec->size() == 0) {
    fs.close();
    return 0;
  }

  // Remove any *trailing* empty (whitespace) lines
  while (retVec->back().find_first_not_of(" \t\n\v\f\r") == std::string::npos) {
    retVec->pop_back();
  }

  fs.close();
  return 0;
}

int DiscoverIOLinks(std::map<std::pair<uint32_t, uint32_t>,
                    std::shared_ptr<IOLink>> *links) {
  assert(links != nullptr);
  if (links == nullptr) {
    return EINVAL;
  }
  assert(links->size() == 0);

  links->clear();

  auto kfd_node_dir = opendir(kKFDNodesPathRoot);
  assert(kfd_node_dir != nullptr);

  auto dentry_kfd = readdir(kfd_node_dir);
  while (dentry_kfd != nullptr) {
    if (dentry_kfd->d_name[0] == '.') {
      dentry_kfd = readdir(kfd_node_dir);
      continue;
    }

    if (!is_number(dentry_kfd->d_name)) {
      dentry_kfd = readdir(kfd_node_dir);
      continue;
    }

    uint32_t node_indx = std::stoi(dentry_kfd->d_name);
    std::shared_ptr<IOLink> link;
    uint32_t link_indx;
    std::string io_link_path_root = IOLinkPathRoot(node_indx);

    auto io_link_dir = opendir(io_link_path_root.c_str());
    assert(io_link_dir != nullptr);

    auto dentry_io_link = readdir(io_link_dir);
    while (dentry_io_link != nullptr) {
      if (dentry_io_link->d_name[0] == '.') {
        dentry_io_link = readdir(io_link_dir);
        continue;
      }

      if (!is_number(dentry_io_link->d_name)) {
        dentry_io_link = readdir(io_link_dir);
        continue;
      }

      link_indx = std::stoi(dentry_io_link->d_name);
      link = std::shared_ptr<IOLink>(new IOLink(node_indx, link_indx));

      link->Initialize();

      (*links)[std::make_pair(link->node_from(), link->node_to())] = link;

      dentry_io_link = readdir(io_link_dir);
    }

    if (closedir(io_link_dir)) {
      return 1;
    }

    dentry_kfd = readdir(kfd_node_dir);
  }

  if (closedir(kfd_node_dir)) {
    return 1;
  }
  return 0;
}

int DiscoverIOLinksPerNode(uint32_t node_indx, std::map<uint32_t,
                           std::shared_ptr<IOLink>> *links) {
  assert(links != nullptr);
  if (links == nullptr) {
    return EINVAL;
  }
  assert(links->size() == 0);

  links->clear();

  std::shared_ptr<IOLink> link;
  uint32_t link_indx;
  std::string io_link_path_root = IOLinkPathRoot(node_indx);

  auto io_link_dir = opendir(io_link_path_root.c_str());
  assert(io_link_dir != nullptr);

  auto dentry = readdir(io_link_dir);
  while (dentry != nullptr) {
    if (dentry->d_name[0] == '.') {
      dentry = readdir(io_link_dir);
      continue;
    }

    if (!is_number(dentry->d_name)) {
      dentry = readdir(io_link_dir);
      continue;
    }

    link_indx = std::stoi(dentry->d_name);
    link = std::shared_ptr<IOLink>(new IOLink(node_indx, link_indx));

    link->Initialize();

    (*links)[link->node_to()] = link;

    dentry = readdir(io_link_dir);
  }

  if (closedir(io_link_dir)) {
    return 1;
  }
  return 0;
}

IOLink::~IOLink() {
}

int IOLink::ReadProperties(void) {
  int ret;

  std::vector<std::string> propVec;

  assert(properties_.size() == 0);
  if (properties_.size() > 0) {
    return 0;
  }

  ret = ReadIOLinkProperties(node_indx_, link_indx_, &propVec);

  if (ret) {
    return ret;
  }

  std::string key_str;
  uint64_t val_int;  // Assume all properties are unsigned integers for now
  std::istringstream fs;

  for (uint32_t i = 0; i < propVec.size(); ++i) {
    fs.str(propVec[i]);
    fs >> key_str;
    fs >> val_int;

    properties_[key_str] = val_int;

    fs.str("");
    fs.clear();
  }

  return 0;
}

int
IOLink::Initialize(void) {
  int ret = 0;
  ret = ReadProperties();
  if (ret) {return ret;}

  ret = get_property_value(kIOLinkPropTYPEStr,
                           reinterpret_cast<uint64_t *>(&type_));
  if (ret) {return ret;}

  ret = get_property_value(kIOLinkPropNODE_FROMStr,
                           reinterpret_cast<uint64_t *>(&node_from_));
  if (ret) {return ret;}

  ret = get_property_value(kIOLinkPropNODE_TOStr,
                           reinterpret_cast<uint64_t *>(&node_to_));
  if (ret) {return ret;}

  ret = get_property_value(kIOLinkPropWEIGHTStr, &weight_);

  return ret;
}

int
IOLink::get_property_value(std::string property, uint64_t *value) {
  assert(value != nullptr);
  if (value == nullptr) {
    return EINVAL;
  }
  if (properties_.find(property) == properties_.end()) {
    return EINVAL;
  }
  *value = properties_[property];
  return 0;
}

}  // namespace smi
}  // namespace amd
