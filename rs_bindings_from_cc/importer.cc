// Part of the Crubit project, under the Apache License v2.0 with LLVM
// Exceptions. See /LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "rs_bindings_from_cc/importer.h"

#include <stdint.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "base/logging.h"
#include "rs_bindings_from_cc/ast_convert.h"
#include "rs_bindings_from_cc/bazel_types.h"
#include "rs_bindings_from_cc/ir.h"
#include "third_party/absl/container/flat_hash_map.h"
#include "third_party/absl/container/flat_hash_set.h"
#include "third_party/absl/status/status.h"
#include "third_party/absl/status/statusor.h"
#include "third_party/absl/strings/cord.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/absl/strings/str_join.h"
#include "third_party/absl/strings/string_view.h"
#include "third_party/absl/strings/substitute.h"
#include "third_party/llvm/llvm-project/clang/include/clang/AST/ASTContext.h"
#include "third_party/llvm/llvm-project/clang/include/clang/AST/Decl.h"
#include "third_party/llvm/llvm-project/clang/include/clang/AST/DeclCXX.h"
#include "third_party/llvm/llvm-project/clang/include/clang/AST/DeclTemplate.h"
#include "third_party/llvm/llvm-project/clang/include/clang/AST/Mangle.h"
#include "third_party/llvm/llvm-project/clang/include/clang/AST/RawCommentList.h"
#include "third_party/llvm/llvm-project/clang/include/clang/AST/RecordLayout.h"
#include "third_party/llvm/llvm-project/clang/include/clang/AST/Type.h"
#include "third_party/llvm/llvm-project/clang/include/clang/Basic/FileManager.h"
#include "third_party/llvm/llvm-project/clang/include/clang/Basic/SourceLocation.h"
#include "third_party/llvm/llvm-project/clang/include/clang/Basic/SourceManager.h"
#include "third_party/llvm/llvm-project/clang/include/clang/Basic/Specifiers.h"
#include "third_party/llvm/llvm-project/clang/include/clang/Sema/Sema.h"
#include "third_party/llvm/llvm-project/llvm/include/llvm/ADT/Optional.h"
#include "third_party/llvm/llvm-project/llvm/include/llvm/Support/Casting.h"
#include "third_party/llvm/llvm-project/llvm/include/llvm/Support/Regex.h"
#include "util/gtl/flat_map.h"

namespace rs_bindings_from_cc {

constexpr absl::string_view kTypeStatusPayloadUrl =
    "type.googleapis.com/devtools.rust.cc_interop.rs_binding_from_cc.type";

// A mapping of C++ standard types to their equivalent Rust types.
// To produce more idiomatic results, these types receive special handling
// instead of using the generic type mapping mechanism.
static constexpr auto kWellKnownTypes =
    gtl::fixed_flat_map_of<absl::string_view, absl::string_view>({
        {"ptrdiff_t", "isize"},
        {"intptr_t", "isize"},
        {"size_t", "usize"},
        {"uintptr_t", "usize"},
        {"std::ptrdiff_t", "isize"},
        {"std::intptr_t", "isize"},
        {"std::size_t", "usize"},
        {"std::uintptr_t", "usize"},

        {"int8_t", "i8"},
        {"int16_t", "i16"},
        {"int32_t", "i32"},
        {"int64_t", "i64"},
        {"std::int8_t", "i8"},
        {"std::int16_t", "i16"},
        {"std::int32_t", "i32"},
        {"std::int64_t", "i64"},

        {"uint8_t", "u8"},
        {"uint16_t", "u16"},
        {"uint32_t", "u32"},
        {"uint64_t", "u64"},
        {"std::uint8_t", "u8"},
        {"std::uint16_t", "u16"},
        {"std::uint32_t", "u32"},
        {"std::uint64_t", "u64"},

        {"char16_t", "u16"},
        {"char32_t", "u32"},
        {"wchar_t", "i32"},
    });

static DeclId GenerateDeclId(const clang::Decl* decl) {
  return DeclId(reinterpret_cast<uintptr_t>(decl->getCanonicalDecl()));
}

std::vector<clang::RawComment*> Importer::ImportFreeComments() {
  clang::SourceManager& sm = ctx_.getSourceManager();

  // We put all comments into an ordered set in source order. Later we'll remove
  // the comments that we don't want or that we get by other means.
  auto source_order = [&sm](const clang::SourceLocation& a,
                            const clang::SourceLocation& b) {
    return b.isValid() && (a.isInvalid() || sm.isBeforeInTranslationUnit(a, b));
  };
  auto ordered_comments = std::map<clang::SourceLocation, clang::RawComment*,
                                   decltype(source_order)>(source_order);

  // We start off by getting the comments from all entry header files...
  for (const auto& header : invocation_.entry_headers_) {
    if (auto file = sm.getFileManager().getFile(header.IncludePath())) {
      if (auto comments = ctx_.Comments.getCommentsInFile(
              sm.getOrCreateFileID(*file, clang::SrcMgr::C_User))) {
        for (const auto& [_, comment] : *comments) {
          ordered_comments.insert({comment->getBeginLoc(), comment});
        }
      }
    }
  }

  // ... and then we remove those that "conflict" with an IR item.
  for (const auto& [decl, result] : lookup_cache_) {
    if (result.item()) {
      // Remove doc comments of imported items.
      if (auto raw_comment = ctx_.getRawCommentForDeclNoCache(decl)) {
        ordered_comments.erase(raw_comment->getBeginLoc());
      }
      // Remove comments that are within a visited decl.
      // TODO(forster): We should retain floating comments in decls like
      // records and namespaces.
      ordered_comments.erase(ordered_comments.lower_bound(decl->getBeginLoc()),
                             ordered_comments.upper_bound(decl->getEndLoc()));
    }
  }

  // Return the remaining comments as a `std::vector`.
  std::vector<clang::RawComment*> result;
  result.reserve(ordered_comments.size());
  for (auto& [_, comment] : ordered_comments) {
    result.push_back(comment);
  }
  return result;
}

void Importer::Import(clang::TranslationUnitDecl* translation_unit_decl) {
  ImportDeclsFromDeclContext(translation_unit_decl);

  using OrderedItem = std::tuple<clang::SourceRange, int, IR::Item>;
  clang::SourceManager& sm = ctx_.getSourceManager();
  auto is_less_than = [&sm](const OrderedItem& a, const OrderedItem& b) {
    auto a_range = std::get<0>(a);
    auto b_range = std::get<0>(b);

    if (!a_range.isValid() || !b_range.isValid()) {
      if (a_range.isValid() != b_range.isValid())
        return !a_range.isValid() && b_range.isValid();
    } else {
      if (a_range.getBegin() != b_range.getBegin()) {
        return sm.isBeforeInTranslationUnit(a_range.getBegin(),
                                            b_range.getBegin());
      }
      if (a_range.getEnd() != b_range.getEnd()) {
        return sm.isBeforeInTranslationUnit(a_range.getEnd(), b_range.getEnd());
      }
    }
    return std::get<1>(a) < std::get<1>(b);
  };

  // We emit IR items in the order of the decls they were generated for.
  // For decls that emit multiple items we use a stable, but arbitrary order.
  std::vector<OrderedItem> items;
  for (const auto& [decl, result] : lookup_cache_) {
    int local_order;

    if (clang::isa<clang::RecordDecl>(decl)) {
      local_order = decl->getDeclContext()->isRecord() ? 1 : 0;
    } else if (auto ctor = clang::dyn_cast<clang::CXXConstructorDecl>(decl)) {
      local_order = ctor->isDefaultConstructor() ? 2
                    : ctor->isCopyConstructor()  ? 3
                    : ctor->isMoveConstructor()  ? 4
                                                 : 5;
    } else if (clang::isa<clang::CXXDestructorDecl>(decl)) {
      local_order = 6;
    } else {
      local_order = 7;
    }

    auto item = result.item();
    if (item) {
      items.push_back(
          std::make_tuple(decl->getSourceRange(), local_order, *item));
    }
    if (IsFromCurrentTarget(decl)) {
      for (const auto& error : result.errors()) {
        std::string name = "unnamed";
        if (const auto* named_decl = clang::dyn_cast<clang::NamedDecl>(decl)) {
          name = named_decl->getQualifiedNameAsString();
        }
        items.push_back(std::make_tuple(
            decl->getSourceRange(), local_order,
            UnsupportedItem{
                .name = std::move(name),
                .message = error,
                .source_loc = ConvertSourceLocation(decl->getBeginLoc())}));
      }
    }
  }

  for (auto comment : ImportFreeComments()) {
    items.push_back(std::make_tuple(
        comment->getSourceRange(), 0 /* local_order */,
        Comment{.text = comment->getFormattedText(sm, sm.getDiagnostics())}));
  }
  std::stable_sort(items.begin(), items.end(), is_less_than);

  for (const auto& item : items) {
    invocation_.ir_.items.push_back(std::get<2>(item));
  }
}

void Importer::ImportDeclsFromDeclContext(
    const clang::DeclContext* decl_context) {
  for (auto decl : decl_context->decls()) {
    LookupDecl(decl->getCanonicalDecl());

    if (auto* nested_context = clang::dyn_cast<clang::DeclContext>(decl)) {
      if (nested_context->isNamespace())
        ImportDeclsFromDeclContext(nested_context);
    }
  }
}

Importer::LookupResult Importer::LookupDecl(clang::Decl* decl) {
  if (!lookup_cache_.contains(decl)) {
    lookup_cache_.insert({decl, ImportDecl(decl)});
  }

  return lookup_cache_[decl];
}

Importer::LookupResult Importer::ImportDecl(clang::Decl* decl) {
  if (decl->getDeclContext()->isNamespace()) {
    return LookupResult("Items contained in namespaces are not supported yet");
  }

  if (auto* function_decl = clang::dyn_cast<clang::FunctionDecl>(decl)) {
    return ImportFunction(function_decl);
  } else if (auto* function_template_decl =
                 clang::dyn_cast<clang::FunctionTemplateDecl>(decl)) {
    return ImportFunction(function_template_decl->getTemplatedDecl());
  } else if (auto* record_decl = clang::dyn_cast<clang::RecordDecl>(decl)) {
    auto result = ImportRecord(record_decl);
    // TODO(forster): Should we even visit the nested decl if we couldn't
    // import the parent? For now we have tests that check that we generate
    // error messages for those decls, so we're visiting.
    ImportDeclsFromDeclContext(record_decl);
    return result;
  } else if (auto* typedef_name_decl =
                 clang::dyn_cast<clang::TypedefNameDecl>(decl)) {
    return ImportTypedefName(typedef_name_decl);
  } else if (clang::isa<clang::ClassTemplateDecl>(decl)) {
    return LookupResult("Class templates are not supported yet");
  } else {
    return LookupResult();
  }
}

Importer::LookupResult Importer::ImportFunction(
    clang::FunctionDecl* function_decl) {
  if (!IsFromCurrentTarget(function_decl)) return LookupResult();
  if (function_decl->isDeleted()) return LookupResult();
  if (function_decl->isTemplated()) {
    return LookupResult("Function templates are not supported yet");
  }

  devtools_rust::LifetimeSymbolTable lifetime_symbol_table;
  llvm::Expected<devtools_rust::FunctionLifetimes> lifetimes =
      devtools_rust::GetLifetimeAnnotations(function_decl,
                                            *invocation_.lifetime_context_,
                                            &lifetime_symbol_table);
  llvm::DenseSet<devtools_rust::Lifetime> all_lifetimes;

  std::vector<FuncParam> params;
  std::vector<std::string> errors;
  if (auto* method_decl =
          clang::dyn_cast<clang::CXXMethodDecl>(function_decl)) {
    if (!known_type_decls_.contains(
            method_decl->getParent()->getCanonicalDecl())) {
      return LookupResult("Couldn't import the parent");
    }

    // non-static member functions receive an implicit `this` parameter.
    if (method_decl->isInstance()) {
      std::optional<devtools_rust::TypeLifetimes> this_lifetimes;
      if (lifetimes) {
        this_lifetimes = lifetimes->this_lifetimes;
        all_lifetimes.insert(this_lifetimes->begin(), this_lifetimes->end());
      }
      auto param_type = ConvertType(method_decl->getThisType(), this_lifetimes,
                                    /*nullable=*/false);
      if (!param_type.ok()) {
        errors.push_back(std::string(param_type.status().message()));
      } else {
        params.push_back({*std::move(param_type), Identifier("__this")});
      }
    }
  }

  if (lifetimes) {
    CHECK_EQ(lifetimes->param_lifetimes.size(), function_decl->getNumParams());
  }
  for (unsigned i = 0; i < function_decl->getNumParams(); ++i) {
    const clang::ParmVarDecl* param = function_decl->getParamDecl(i);
    std::optional<devtools_rust::TypeLifetimes> param_lifetimes;
    if (lifetimes) {
      param_lifetimes = lifetimes->param_lifetimes[i];
      all_lifetimes.insert(param_lifetimes->begin(), param_lifetimes->end());
    }
    auto param_type = ConvertType(param->getType(), param_lifetimes);
    if (!param_type.ok()) {
      errors.push_back(absl::Substitute("Parameter type '$0' is not supported",
                                        param->getType().getAsString()));
      continue;
    }

    if (const clang::RecordType* record_type =
            clang::dyn_cast<clang::RecordType>(param->getType())) {
      if (clang::RecordDecl* record_decl =
              clang::dyn_cast<clang::RecordDecl>(record_type->getDecl())) {
        // TODO(b/200067242): non-trivial_abi structs, when passed by value,
        // have a different representation which needs special support. We
        // currently do not support it.
        if (!record_decl->canPassInRegisters()) {
          errors.push_back(
              absl::Substitute("Non-trivial_abi type '$0' is not "
                               "supported by value as a parameter",
                               param->getType().getAsString()));
        }
      }
    }

    std::optional<Identifier> param_name = GetTranslatedIdentifier(param);
    CHECK(param_name.has_value());  // No known cases where the above can fail.
    params.push_back({*param_type, *std::move(param_name)});
  }

  if (const clang::RecordType* record_return_type =
          clang::dyn_cast<clang::RecordType>(function_decl->getReturnType())) {
    if (clang::RecordDecl* record_decl =
            clang::dyn_cast<clang::RecordDecl>(record_return_type->getDecl())) {
      // TODO(b/200067242): non-trivial_abi structs, when passed by value,
      // have a different representation which needs special support. We
      // currently do not support it.
      if (!record_decl->canPassInRegisters()) {
        errors.push_back(
            absl::Substitute("Non-trivial_abi type '$0' is not supported "
                             "by value as a return type",
                             function_decl->getReturnType().getAsString()));
      }
    }
  }

  std::optional<devtools_rust::TypeLifetimes> return_lifetimes;
  if (lifetimes) {
    return_lifetimes = lifetimes->return_lifetimes;
    all_lifetimes.insert(return_lifetimes->begin(), return_lifetimes->end());
  }
  auto return_type =
      ConvertType(function_decl->getReturnType(), return_lifetimes);
  if (!return_type.ok()) {
    errors.push_back(
        absl::Substitute("Return type '$0' is not supported",
                         function_decl->getReturnType().getAsString()));
  }

  std::vector<Lifetime> lifetime_params;
  for (devtools_rust::Lifetime lifetime : all_lifetimes) {
    std::optional<llvm::StringRef> name =
        lifetime_symbol_table.LookupLifetime(lifetime);
    CHECK(name.has_value());
    lifetime_params.push_back(
        {.name = name->str(), .id = LifetimeId(lifetime.Id())});
  }
  std::sort(
      lifetime_params.begin(), lifetime_params.end(),
      [](const Lifetime& l1, const Lifetime& l2) { return l1.name < l2.name; });

  std::optional<MemberFuncMetadata> member_func_metadata;
  if (auto* method_decl =
          clang::dyn_cast<clang::CXXMethodDecl>(function_decl)) {
    switch (method_decl->getAccess()) {
      case clang::AS_public:
        break;
      case clang::AS_protected:
      case clang::AS_private:
      case clang::AS_none:
        // No need for IR to include Func representing private methods.
        // TODO(lukasza): Revisit this for protected methods.
        return LookupResult();
    }
    std::optional<MemberFuncMetadata::InstanceMethodMetadata> instance_metadata;
    if (method_decl->isInstance()) {
      MemberFuncMetadata::ReferenceQualification reference;
      switch (method_decl->getRefQualifier()) {
        case clang::RQ_LValue:
          reference = MemberFuncMetadata::kLValue;
          break;
        case clang::RQ_RValue:
          reference = MemberFuncMetadata::kRValue;
          break;
        case clang::RQ_None:
          reference = MemberFuncMetadata::kUnqualified;
          break;
      }
      instance_metadata = MemberFuncMetadata::InstanceMethodMetadata{
          .reference = reference,
          .is_const = method_decl->isConst(),
          .is_virtual = method_decl->isVirtual(),
          .is_explicit_ctor = false,
      };
      if (auto* ctor_decl =
              clang::dyn_cast<clang::CXXConstructorDecl>(function_decl)) {
        instance_metadata->is_explicit_ctor = ctor_decl->isExplicit();
      }
    }

    member_func_metadata = MemberFuncMetadata{
        .record_id = GenerateDeclId(method_decl->getParent()),
        .instance_method_metadata = instance_metadata};
  }

  if (!errors.empty()) {
    return LookupResult(errors);
  }

  std::optional<UnqualifiedIdentifier> translated_name =
      GetTranslatedName(function_decl);
  CHECK(return_type.ok());  // Silence ClangTidy, checked above.
  if (translated_name.has_value()) {
    return LookupResult(Func{
        .name = *translated_name,
        .owning_target = GetOwningTarget(function_decl),
        .doc_comment = GetComment(function_decl),
        .mangled_name = GetMangledName(function_decl),
        .return_type = *return_type,
        .params = std::move(params),
        .lifetime_params = std::move(lifetime_params),
        .is_inline = function_decl->isInlined(),
        .member_func_metadata = std::move(member_func_metadata),
        .source_loc = ConvertSourceLocation(function_decl->getBeginLoc()),
    });
  }
  return LookupResult();
}

BlazeLabel Importer::GetOwningTarget(const clang::Decl* decl) const {
  clang::SourceManager& source_manager = ctx_.getSourceManager();
  auto source_location = decl->getLocation();
  auto id = source_manager.getFileID(source_location);

  // If the header this decl comes from is not associated with a target we
  // consider it a textual header. In that case we go up the include stack
  // until we find a header that has an owning target.

  // TODO(b/208377928): We currently don't have a target for the headers in
  // Clang's resource directory, so for the time being we return a fictional
  // "//:virtual_clang_resource_dir_target" for system headers.
  while (source_location.isValid() &&
         !source_manager.isInSystemHeader(source_location)) {
    llvm::Optional<llvm::StringRef> filename =
        source_manager.getNonBuiltinFilenameForID(id);
    if (!filename) {
      return BlazeLabel("//:builtin");
    }
    if (filename->startswith("./")) {
      filename = filename->substr(2);
    }

    if (auto target = invocation_.header_target(HeaderName(filename->str()))) {
      return *target;
    }
    source_location = source_manager.getIncludeLoc(id);
    id = source_manager.getFileID(source_location);
  }

  return BlazeLabel("//:virtual_clang_resource_dir_target");
}

bool Importer::IsFromCurrentTarget(const clang::Decl* decl) const {
  return invocation_.target_ == GetOwningTarget(decl);
}

Importer::LookupResult Importer::ImportRecord(clang::RecordDecl* record_decl) {
  const clang::DeclContext* decl_context = record_decl->getDeclContext();
  if (decl_context->isFunctionOrMethod()) {
    return LookupResult();
  }
  if (record_decl->isInjectedClassName()) {
    return LookupResult();
  }
  if (decl_context->isRecord()) {
    return LookupResult("Nested classes are not supported yet");
  }
  if (record_decl->isUnion()) {
    return LookupResult("Unions are not supported yet");
  }

  // Make sure the record has a definition that we'll be able to call
  // ASTContext::getASTRecordLayout() on.
  record_decl = record_decl->getDefinition();
  if (!record_decl || record_decl->isInvalidDecl() ||
      !record_decl->isCompleteDefinition()) {
    return LookupResult();
  }

  clang::AccessSpecifier default_access = clang::AS_public;

  bool is_final = true;
  if (auto* cxx_record_decl =
          clang::dyn_cast<clang::CXXRecordDecl>(record_decl)) {
    if (cxx_record_decl->getDescribedClassTemplate() ||
        clang::isa<clang::ClassTemplateSpecializationDecl>(record_decl)) {
      return LookupResult("Class templates are not supported yet");
    }

    sema_.ForceDeclarationOfImplicitMembers(cxx_record_decl);
    if (cxx_record_decl->isClass()) {
      default_access = clang::AS_private;
    }
    is_final = cxx_record_decl->isEffectivelyFinal();
  }
  std::optional<Identifier> record_name = GetTranslatedIdentifier(record_decl);
  if (!record_name.has_value()) {
    return LookupResult();
  }
  // Provisionally assume that we know this RecordDecl so that we'll be able
  // to import fields whose type contains the record itself.
  known_type_decls_.insert(record_decl);
  absl::StatusOr<std::vector<Field>> fields =
      ImportFields(record_decl, default_access);
  if (!fields.ok()) {
    // Importing a field failed, so note that we didn't import this RecordDecl
    // after all.
    known_type_decls_.erase(record_decl);
    return LookupResult("Importing field failed");
  }

  const clang::ASTRecordLayout& layout = ctx_.getASTRecordLayout(record_decl);
  return LookupResult(
      Record{.identifier = *record_name,
             .id = GenerateDeclId(record_decl),
             .owning_target = GetOwningTarget(record_decl),
             .doc_comment = GetComment(record_decl),
             .fields = *std::move(fields),
             .size = layout.getSize().getQuantity(),
             .alignment = layout.getAlignment().getQuantity(),
             .copy_constructor = GetCopyCtorSpecialMemberFunc(*record_decl),
             .move_constructor = GetMoveCtorSpecialMemberFunc(*record_decl),
             .destructor = GetDestructorSpecialMemberFunc(*record_decl),
             .is_trivial_abi = record_decl->canPassInRegisters(),
             .is_final = is_final});
}

Importer::LookupResult Importer::ImportTypedefName(
    clang::TypedefNameDecl* typedef_name_decl) {
  const clang::DeclContext* decl_context = typedef_name_decl->getDeclContext();
  if (decl_context) {
    if (decl_context->isFunctionOrMethod()) {
      return LookupResult();
    }
    if (decl_context->isRecord()) {
      return LookupResult("Typedefs nested in classes are not supported yet");
    }
  }

  clang::QualType type =
      typedef_name_decl->getASTContext().getTypedefType(typedef_name_decl);
  if (kWellKnownTypes.contains(type.getAsString())) {
    return LookupResult();
  }

  std::optional<Identifier> identifier =
      GetTranslatedIdentifier(typedef_name_decl);
  if (!identifier.has_value()) {
    // This should never happen.
    LOG(FATAL) << "Couldn't get identifier for TypedefNameDecl";
  }
  absl::StatusOr<MappedType> underlying_type =
      ConvertType(typedef_name_decl->getUnderlyingType());
  if (underlying_type.ok()) {
    known_type_decls_.insert(typedef_name_decl);
    return LookupResult(
        TypeAlias{.identifier = *identifier,
                  .id = GenerateDeclId(typedef_name_decl),
                  .owning_target = GetOwningTarget(typedef_name_decl),
                  .underlying_type = *underlying_type});
  } else {
    return LookupResult(std::string(underlying_type.status().message()));
  }
}

static bool ShouldKeepCommentLine(absl::string_view line) {
  // Based on https://clang.llvm.org/extra/clang-tidy/:
  llvm::Regex patterns_to_ignore(
      "^[[:space:]/]*"  // Whitespace, or extra //
      "(NOLINT|NOLINTNEXTLINE|NOLINTBEGIN|NOLINTEND)"
      "(\\([^)[:space:]]*\\)?)?"  // Optional (...)
      "[[:space:]]*$");           // Whitespace
  return !patterns_to_ignore.match(line);
}

std::optional<std::string> Importer::GetComment(const clang::Decl* decl) const {
  // This does currently not distinguish between different types of comments.
  // In general it is not possible in C++ to reliably only extract doc comments.
  // This is going to be a heuristic that needs to be tuned over time.

  clang::SourceManager& sm = ctx_.getSourceManager();
  clang::RawComment* raw_comment = ctx_.getRawCommentForDeclNoCache(decl);

  if (raw_comment == nullptr) {
    return {};
  }

  std::string raw_comment_text =
      raw_comment->getFormattedText(sm, sm.getDiagnostics());
  std::string cleaned_comment_text = absl::StrJoin(
      absl::StrSplit(raw_comment_text, '\n', ShouldKeepCommentLine), "\n");
  return cleaned_comment_text.empty()
             ? std::nullopt
             : std::optional<std::string>(std::move(cleaned_comment_text));
}

SourceLoc Importer::ConvertSourceLocation(clang::SourceLocation loc) const {
  auto& sm = ctx_.getSourceManager();

  clang::StringRef filename = sm.getFilename(loc);
  if (filename.startswith("./")) {
    filename = filename.substr(2);
  }

  return SourceLoc{.filename = filename.str(),
                   .line = sm.getSpellingLineNumber(loc),
                   .column = sm.getSpellingColumnNumber(loc)};
}

absl::StatusOr<MappedType> Importer::ConvertType(
    clang::QualType qual_type,
    std::optional<devtools_rust::TypeLifetimes> lifetimes,
    bool nullable) const {
  std::optional<MappedType> type = std::nullopt;
  // When converting the type to a string, don't include qualifiers -- we handle
  // these separately.
  std::string type_string = qual_type.getUnqualifiedType().getAsString();

  if (auto iter = kWellKnownTypes.find(type_string);
      iter != kWellKnownTypes.end()) {
    type = MappedType::Simple(std::string(iter->second), type_string);
  } else if (const auto* pointer_type =
                 qual_type->getAs<clang::PointerType>()) {
    std::optional<LifetimeId> lifetime;
    if (lifetimes.has_value()) {
      CHECK(!lifetimes->empty());
      lifetime = LifetimeId(lifetimes->back().Id());
      lifetimes->pop_back();
    }
    auto pointee_type = ConvertType(pointer_type->getPointeeType(), lifetimes);
    if (pointee_type.ok()) {
      type = MappedType::PointerTo(*pointee_type, lifetime, nullable);
    }
  } else if (const auto* lvalue_ref_type =
                 qual_type->getAs<clang::LValueReferenceType>()) {
    std::optional<LifetimeId> lifetime;
    if (lifetimes.has_value()) {
      CHECK(!lifetimes->empty());
      lifetime = LifetimeId(lifetimes->back().Id());
      lifetimes->pop_back();
    }
    auto pointee_type =
        ConvertType(lvalue_ref_type->getPointeeType(), lifetimes);
    if (pointee_type.ok()) {
      type = MappedType::LValueReferenceTo(*pointee_type, lifetime);
    }
  } else if (const auto* builtin_type =
                 // Use getAsAdjusted instead of getAs so we don't desugar
                 // typedefs.
             qual_type->getAsAdjusted<clang::BuiltinType>()) {
    switch (builtin_type->getKind()) {
      case clang::BuiltinType::Bool:
        type = MappedType::Simple("bool", "bool");
        break;
      case clang::BuiltinType::Float:
        type = MappedType::Simple("f32", "float");
        break;
      case clang::BuiltinType::Double:
        type = MappedType::Simple("f64", "double");
        break;
      case clang::BuiltinType::Void:
        type = MappedType::Void();
        break;
      default:
        if (builtin_type->isIntegerType()) {
          auto size = ctx_.getTypeSize(builtin_type);
          if (size == 8 || size == 16 || size == 32 || size == 64) {
            type = MappedType::Simple(
                absl::Substitute(
                    "$0$1", builtin_type->isSignedInteger() ? 'i' : 'u', size),
                type_string);
          }
        }
    }
  } else if (const auto* tag_type =
                 qual_type->getAsAdjusted<clang::TagType>()) {
    clang::TagDecl* tag_decl = tag_type->getDecl();

    if (known_type_decls_.contains(tag_decl)) {
      if (std::optional<Identifier> id = GetTranslatedIdentifier(tag_decl)) {
        std::string ident(id->Ident());
        DeclId decl_id = GenerateDeclId(tag_decl);
        type = MappedType::WithDeclIds(ident, decl_id, ident, decl_id);
      }
    }
  } else if (const auto* typedef_type =
                 qual_type->getAsAdjusted<clang::TypedefType>()) {
    clang::TypedefNameDecl* typedef_name_decl = typedef_type->getDecl();

    if (known_type_decls_.contains(typedef_name_decl)) {
      if (std::optional<Identifier> id =
              GetTranslatedIdentifier(typedef_name_decl)) {
        std::string ident(id->Ident());
        DeclId decl_id = GenerateDeclId(typedef_name_decl);
        type = MappedType::WithDeclIds(ident, decl_id, ident, decl_id);
      }
    }
  }

  if (!type.has_value()) {
    absl::Status error = absl::UnimplementedError(
        absl::Substitute("Unsupported type '$0'", type_string));
    error.SetPayload(kTypeStatusPayloadUrl, absl::Cord(type_string));
    return error;
  }

  // Add cv-qualification.
  type->cc_type.is_const = qual_type.isConstQualified();
  // Not doing volatile for now -- note that volatile pointers do not exist in
  // Rust, though volatile reads/writes still do.

  return *std::move(type);
}

absl::StatusOr<std::vector<Field>> Importer::ImportFields(
    clang::RecordDecl* record_decl, clang::AccessSpecifier default_access) {
  std::vector<Field> fields;
  const clang::ASTRecordLayout& layout = ctx_.getASTRecordLayout(record_decl);
  for (const clang::FieldDecl* field_decl : record_decl->fields()) {
    auto type = ConvertType(field_decl->getType());
    if (!type.ok()) {
      return absl::UnimplementedError(
          absl::Substitute("Field type '$0' is not supported",
                           field_decl->getType().getAsString()));
    }
    clang::AccessSpecifier access = field_decl->getAccess();
    if (access == clang::AS_none) {
      access = default_access;
    }

    std::optional<Identifier> field_name = GetTranslatedIdentifier(field_decl);
    if (!field_name.has_value()) {
      return absl::UnimplementedError(
          absl::Substitute("Cannot translate name for field '$0'",
                           field_decl->getNameAsString()));
    }
    fields.push_back(
        {.identifier = *std::move(field_name),
         .doc_comment = GetComment(field_decl),
         .type = *type,
         .access = TranslateAccessSpecifier(access),
         .offset = layout.getFieldOffset(field_decl->getFieldIndex())});
  }
  return fields;
}

std::string Importer::GetMangledName(const clang::NamedDecl* named_decl) const {
  clang::GlobalDecl decl;

  // There are only three named decl types that don't work with the GlobalDecl
  // unary constructor: GPU kernels (which do not exist in standard C++, so we
  // ignore), constructors, and destructors. GlobalDecl does not support
  // constructors and destructors from the unary constructor because there is
  // more than one global declaration for a given constructor or destructor!
  //
  //   * (Ctor|Dtor)_Complete is a function which constructs / destroys the
  //     entire object. This is what we want. :)
  //   * Dtor_Deleting is a function which additionally calls operator delete.
  //   * (Ctor|Dtor)_Base is a function which constructs/destroys the object but
  //     NOT including virtual base class subobjects.
  //   * (Ctor|Dtor)_Comdat: I *believe* this is the identifier used to
  //     deduplicate inline functions, and is not callable.
  //   * Dtor_(Copying|Default)Closure: These only exist in the MSVC++ ABI,
  //     which we don't support for now. I don't know when they are used.
  //
  // It was hard to piece this together, so writing it down here to explain why
  // we magically picked the *_Complete variants.
  if (auto dtor = clang::dyn_cast<clang::CXXDestructorDecl>(named_decl)) {
    decl = clang::GlobalDecl(dtor, clang::CXXDtorType::Dtor_Complete);
  } else if (auto ctor =
                 clang::dyn_cast<clang::CXXConstructorDecl>(named_decl)) {
    decl = clang::GlobalDecl(ctor, clang::CXXCtorType::Ctor_Complete);
  } else {
    decl = clang::GlobalDecl(named_decl);
  }

  std::string name;
  llvm::raw_string_ostream stream(name);
  mangler_->mangleName(decl, stream);
  stream.flush();
  return name;
}

std::optional<UnqualifiedIdentifier> Importer::GetTranslatedName(
    const clang::NamedDecl* named_decl) const {
  switch (named_decl->getDeclName().getNameKind()) {
    case clang::DeclarationName::Identifier: {
      auto name = std::string(named_decl->getName());
      if (name.empty()) {
        if (const clang::ParmVarDecl* param_decl =
                clang::dyn_cast<clang::ParmVarDecl>(named_decl)) {
          int param_pos = param_decl->getFunctionScopeIndex();
          return {Identifier(absl::StrCat("__param_", param_pos))};
        }
        // TODO(lukasza): Handle anonymous structs (probably this won't be an
        // issue until nested types are handled - b/200067824).
        return std::nullopt;
      }
      return {Identifier(std::move(name))};
    }
    case clang::DeclarationName::CXXConstructorName:
      return {SpecialName::kConstructor};
    case clang::DeclarationName::CXXDestructorName:
      return {SpecialName::kDestructor};
    default:
      // To be implemented later: operators, conversion functions.
      // There are also e.g. literal operators, deduction guides, etc., but
      // we might not need to implement them at all. Full list at:
      // https://clang.llvm.org/doxygen/classclang_1_1DeclarationName.html#a9ab322d434446b43379d39e41af5cbe3
      return std::nullopt;
  }
}

}  // namespace rs_bindings_from_cc