/*
 * Copyright (c) 2015-2018 Carnegie Mellon University.
 *
 * All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file. See the AUTHORS file for names of contributors.
 */

#pragma once

#include "v1/deltafs_plfsio_types.h"

namespace pdlfs {
namespace plfsio {
namespace v2 {

class DirWriter {
 public:
  ~DirWriter();

  Status Add(const Slice& fid, const Slice& data, std::string* loc);

 private:
  struct Rep;

  void operator=(const DirWriter& dw);
  DirWriter(const DirWriter&);

  Rep* rep_;
};

}  // namespace v2
}  // namespace plfsio
}  // namespace pdlfs
