// Part of the Crubit project, under the Apache License v2.0 with LLVM
// Exceptions. See /LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Parses C++ headers and generates:
// * a Rust source file with bindings for the C++ API
// * a C++ source file with the implementation of the bindings

#include <string>
#include <utility>
#include <vector>

#include "base/init_google.h"
#include "base/logging.h"
#include "rs_bindings_from_cc/bazel_types.h"
#include "rs_bindings_from_cc/ir.h"
#include "rs_bindings_from_cc/ir_from_cc.h"
#include "rs_bindings_from_cc/src_code_gen.h"
#include "third_party/absl/container/flat_hash_map.h"
#include "third_party/absl/flags/flag.h"
#include "third_party/absl/meta/type_traits.h"
#include "third_party/absl/status/status.h"
#include "third_party/absl/status/statusor.h"
#include "third_party/absl/strings/string_view.h"
#include "third_party/json/src/json.hpp"
#include "third_party/llvm/llvm-project/llvm/include/llvm/Support/FileSystem.h"
#include "third_party/llvm/llvm-project/llvm/include/llvm/Support/raw_ostream.h"
#include "util/gtl/labs/string_type.h"
#include "util/task/status.h"

namespace {

absl::Status SetFileContents(const std::string& path,
                             absl::string_view contents) {
  std::error_code error_code;
  llvm::raw_fd_ostream stream(path, error_code);
  if (error_code) {
    return absl::Status(absl::StatusCode::kInternal, error_code.message());
  }
  stream << contents;
  stream.close();
  if (stream.has_error()) {
    return absl::Status(absl::StatusCode::kInternal, stream.error().message());
  }
  return absl::OkStatus();
}

}  // namespace

ABSL_FLAG(bool, do_nothing, false,
          "if set to true the tool will produce empty files "
          "(useful for testing Blaze integration)");
ABSL_FLAG(std::string, rs_out, "",
          "output path for the Rust source file with bindings");
ABSL_FLAG(std::string, cc_out, "",
          "output path for the C++ source file with bindings implementation");
ABSL_FLAG(std::string, ir_out, "",
          "(optional) output path for the JSON IR. If not present, the JSON IR "
          "will not be dumped.");
ABSL_FLAG(std::vector<std::string>, public_headers, std::vector<std::string>(),
          "public headers of the cc_library this tool should generate bindings "
          "for, in a format suitable for usage in google3-relative quote "
          "include (#include \"\").");
ABSL_FLAG(std::string, targets_and_headers, std::string(),
          "Information about which headers belong to which targets, encoded as "
          "a JSON array. For example: "
          "[\n"
          "  {\n"
          "     \"t\": \"//foo/bar:baz\",\n"
          "     \"h\": [\"foo/bar/header1.h\", \"foo/bar/header2.h\"]\n"
          "  },\n"
          "...\n"
          "]");

int main(int argc, char* argv[]) {
  InitGoogle(argv[0], &argc, &argv, true);

  auto rs_out = absl::GetFlag(FLAGS_rs_out);
  QCHECK(!rs_out.empty()) << "please specify --rs_out";
  auto cc_out = absl::GetFlag(FLAGS_cc_out);
  QCHECK(!cc_out.empty()) << "please specify --cc_out";

  if (absl::GetFlag(FLAGS_do_nothing)) {
    CHECK_OK(SetFileContents(
        rs_out,
        "// intentionally left empty because --do_nothing was passed."));
    CHECK_OK(SetFileContents(
        cc_out,
        "// intentionally left empty because --do_nothing was passed."));
    return 0;
  }

  auto public_headers = absl::GetFlag(FLAGS_public_headers);
  QCHECK(!public_headers.empty())
      << "please specify at least one header in --public_headers";
  auto targets_and_headers_json = absl::GetFlag(FLAGS_targets_and_headers);
  QCHECK(!targets_and_headers_json.empty())
      << "please specify --targets_and_headers";
  absl::flat_hash_map<const rs_bindings_from_cc::HeaderName,
                      const rs_bindings_from_cc::BlazeLabel>
      headers_to_targets;
  if (!targets_and_headers_json.empty()) {
    nlohmann::json targets_and_headers =
        nlohmann::json::parse(targets_and_headers_json);
    QCHECK(targets_and_headers.is_array())
        << "Expected `--targets_and_headers` to be a Json array of objects";
    for (const auto& target_and_headers : targets_and_headers) {
      rs_bindings_from_cc::BlazeLabel target =
          rs_bindings_from_cc::BlazeLabel{target_and_headers["t"]};
      QCHECK(target_and_headers["h"].is_array())
          << "Expected `h` fields of `--targets_and_headers` "
             "to be an array of strings";
      for (std::string header : target_and_headers["h"]) {
        headers_to_targets.insert(
            std::pair<const rs_bindings_from_cc::HeaderName,
                      const rs_bindings_from_cc::BlazeLabel>(
                rs_bindings_from_cc::HeaderName(header), target));
      }
    }
  }

  auto find_header = [&](const std::string& header) {
    auto it = headers_to_targets.find(rs_bindings_from_cc::HeaderName(header));
    QCHECK(it != headers_to_targets.end())
        << "Couldn't find header '" << header << "' in "
        << "the `headers_to_target` map derived from the "
        << "--targets_and_headers cmdline argument";
    return it->second;
  };
  rs_bindings_from_cc::BlazeLabel current_target =
      find_header(public_headers[0]);
  for (const auto& public_header : public_headers) {
    rs_bindings_from_cc::BlazeLabel header_target = find_header(public_header);
    QCHECK(current_target == header_target)
        << "Expected all public headers to belong to the current target '"
        << current_target << "', but header '" << public_header
        << "' belongs to '" << header_target << "'";
  }

  auto ir_out = absl::GetFlag(FLAGS_ir_out);  // Optional.
  if (absl::StatusOr<rs_bindings_from_cc::IR> ir =
          rs_bindings_from_cc::IrFromCc(
              /* extra_source_code= */ "", current_target,
              std::vector<rs_bindings_from_cc::HeaderName>(
                  public_headers.begin(), public_headers.end()),
              /* virtual_headers_contents= */ {}, std::move(headers_to_targets),
              std::vector<absl::string_view>(argv, argv + argc));
      ir.ok()) {
    if (!ir_out.empty()) {
      CHECK_OK(SetFileContents(ir_out, ir->ToJson().dump(/*indent=*/2)));
    }
    rs_bindings_from_cc::Bindings bindings =
        rs_bindings_from_cc::GenerateBindings(*ir);
    CHECK_OK(SetFileContents(rs_out, bindings.rs_api));
    CHECK_OK(SetFileContents(cc_out, bindings.rs_api_impl));
    return 0;
  }

  llvm::sys::fs::remove(rs_out);
  llvm::sys::fs::remove(cc_out);
  if (!ir_out.empty()) {
    llvm::sys::fs::remove(ir_out);
  }
  return 1;
}
