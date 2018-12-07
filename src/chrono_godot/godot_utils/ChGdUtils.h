// =============================================================================
// PROJECT CHRONO - http://projectchrono.org
//
// Copyright (c) 2014 projectchrono.org
// All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be found
// in the LICENSE file at the top level of the distribution and at
// http://projectchrono.org/license-chrono.txt.
//
// =============================================================================
// Authors: Asher Elmquist
// =============================================================================
//
// Class to manage adding, changing, and removing godot scene nodes
//
// =============================================================================
#ifndef CHGDUTILS_H
#define CHGDUTILS_H

// #include <iostream>
// #include <memory>

// godot includes
//#include <core/math/quat.h>
//#include <scene/main/viewport.h>

// class Quat;
// class Vector3;
// chrono includes
#include "chrono/core/ChQuaternion.h"
#include "chrono/core/ChVector.h"

// ChGodotIncludes
#include "chrono_godot/ChApiGodot.h"

namespace chrono {
namespace gd {

CH_GODOT_API Vector3 GDVector(ChVector<> vec);
CH_GODOT_API Quat GDQuat(ChQuaternion<> quat);
CH_GODOT_API Transform GDTransform(ChQuaternion<> q);

}  // namespace gd
}  // end namespace chrono

#endif