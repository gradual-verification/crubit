// Part of the Crubit project, under the Apache License v2.0 with LLVM
// Exceptions. See /LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "cc_bindings_from_rs/test/functions/functions_cc_api.h"

namespace crubit {
namespace {

// Import all the APIs from `functions_cc_api.h`
using namespace functions;

using testing::DoubleEq;

TEST(FunctionsTest, get_42_as_f64_via_no_mangle_extern_c) {
  EXPECT_THAT(get_42_as_f64_via_no_mangle_extern_c(), DoubleEq(42.0));
}

}  // namespace
}  // namespace crubit