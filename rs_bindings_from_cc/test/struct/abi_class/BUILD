"""End-to-end test of ABI classification of structs."""

load("@rules_rust//rust:defs.bzl", "rust_test")

package(default_applicable_licenses = [":license"])

licenses(["notice"])

cc_library(
    name = "abi_class",
    hdrs = ["abi_class.h"],
)

rust_test(
    name = "main",
    srcs = ["test.rs"],
    cc_deps = [":abi_class"],
)
