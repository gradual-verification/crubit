// Part of the Crubit project, under the Apache License v2.0 with LLVM
// Exceptions. See /LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

use anyhow::Result;
use ffi_types::*;
use ir::*;
use itertools::Itertools;
use proc_macro2::TokenStream;
use quote::format_ident;
use quote::quote;
use std::iter::Iterator;
use std::panic::catch_unwind;
use std::process;
use syn::*;

/// FFI equivalent of `Bindings`.
#[repr(C)]
pub struct FfiBindings {
    rs_api: FfiU8SliceBox,
    rs_api_impl: FfiU8SliceBox,
}

/// Deserializes IR from `json` and generates bindings source code.
///
/// This function panics on error.
///
/// Ownership:
///    * function doesn't take ownership of (in other words it borrows) the param `json`
///    * function passes ownership of the returned value to the caller
///
/// Safety:
///    * function expects that param `json` is a FfiU8Slice for a valid array of bytes with the
///      given size.
///    * function expects that param `json` doesn't change during the call.
#[no_mangle]
pub unsafe extern "C" fn GenerateBindingsImpl(json: FfiU8Slice) -> FfiBindings {
    catch_unwind(|| {
        // It is ok to abort here.
        let Bindings { rs_api, rs_api_impl } = generate_bindings(json.as_slice()).unwrap();

        FfiBindings {
            rs_api: FfiU8SliceBox::from_boxed_slice(rs_api.into_bytes().into_boxed_slice()),
            rs_api_impl: FfiU8SliceBox::from_boxed_slice(
                rs_api_impl.into_bytes().into_boxed_slice(),
            ),
        }
    })
    .unwrap_or_else(|_| process::abort())
}

/// Source code for generated bindings.
struct Bindings {
    // Rust source code.
    rs_api: String,
    // C++ source code.
    rs_api_impl: String,
}

fn generate_bindings(json: &[u8]) -> Result<Bindings> {
    let ir = deserialize_ir(json)?;
    let rs_api = generate_rs_api(&ir)?;
    let rs_api_impl = generate_rs_api_impl(&ir)?;
    Ok(Bindings { rs_api, rs_api_impl })
}

/// If we know the original C++ function is codegenned and already compatible with `extern "C"`
/// calling convention we skip creating/calling the C++ thunk since we can call the original C++
/// directly.
fn can_skip_cc_thunk(func: &Func) -> bool {
    // Inline functions may not be codegenned in the C++ library since Clang doesn't know if Rust
    // calls the function or not. Therefore in order to make inline functions callable from Rust we
    // need to generate a C++ file that defines a thunk that delegates to the original inline
    // function. When compiled, Clang will emit code for this thunk and Rust code will call the
    // thunk when the user wants to call the original inline function.
    //
    // This is not great runtime-performance-wise in regular builds (inline function will not be
    // inlined, there will always be a function call), but it is correct. ThinLTO builds will be
    // able to see through the thunk and inline code across the language boundary. For non-ThinLTO
    // builds we plan to implement <internal link> which removes the runtime performance overhead.
    !func.is_inline
}

/// Generate Rust source code for a given Record.
fn generate_record(record: &Record) -> TokenStream {
    let ident = make_ident(&record.identifier.identifier);
    let field_idents =
        record.fields.iter().map(|f| make_ident(&f.identifier.identifier)).collect_vec();
    let field_types = record.fields.iter().map(|f| make_ident(&f.type_.rs_name)).collect_vec();
    quote! {
        #[repr(C)]
        pub struct #ident {
            #( pub #field_idents: #field_types, )*
        }
    }
}

fn generate_rs_api(ir: &IR) -> Result<String> {
    let mut thunks = vec![];
    let mut api_funcs = vec![];
    for func in &ir.functions {
        let mangled_name = &func.mangled_name;
        let ident = make_ident(&func.identifier.identifier);
        let thunk_ident = format_ident!("__rust_thunk__{}", &func.identifier.identifier);
        // TODO(hlopko): do not emit `-> ()` when return type is void, it's implicit.
        let return_type_name = make_ident(&func.return_type.rs_name);

        let param_idents =
            func.params.iter().map(|p| make_ident(&p.identifier.identifier)).collect_vec();

        let param_types = func.params.iter().map(|p| make_ident(&p.type_.rs_name)).collect_vec();

        api_funcs.push(quote! {
            #[inline(always)]
            pub fn #ident( #( #param_idents: #param_types ),* ) -> #return_type_name {
                unsafe { crate::detail::#thunk_ident( #( #param_idents ),* ) }
            }
        });

        let thunk_attr = if can_skip_cc_thunk(&func) {
            quote! {#[link_name = #mangled_name]}
        } else {
            quote! {}
        };

        thunks.push(quote! {
            #thunk_attr
            pub(crate) fn #thunk_ident( #( #param_idents: #param_types ),* ) -> #return_type_name ;
        });
    }

    let records = ir.records.iter().map(generate_record).collect_vec();

    let mod_detail = if thunks.is_empty() {
        quote! {}
    } else {
        quote! {
            mod detail {
                extern "C" {
                    #( #thunks )*
                }
            }
        }
    };

    let result = quote! {
        #( #api_funcs )*
        #( #records )*

        #mod_detail
    };

    Ok(result.to_string())
}

fn make_ident(ident: &str) -> Ident {
    format_ident!("{}", ident)
}

fn generate_rs_api_impl(ir: &IR) -> Result<String> {
    // This function uses quote! to generate C++ source code out of convenience. This is a bold idea
    // so we have to continously evaluate if it still makes sense or the cost of working around
    // differences in Rust and C++ tokens is greather than the value added.
    //
    // See rs_bindings_from_cc/token_stream_printer.rs for a list
    // of supported placeholders.
    let mut thunks = vec![];
    for func in &ir.functions {
        if can_skip_cc_thunk(&func) {
            continue;
        }

        let thunk_ident = format_ident!("__rust_thunk__{}", &func.identifier.identifier);
        let ident = make_ident(&func.identifier.identifier);
        let return_type_name = make_ident(&func.return_type.cc_name);

        let param_idents =
            func.params.iter().map(|p| make_ident(&p.identifier.identifier)).collect_vec();

        let param_types = func.params.iter().map(|p| make_ident(&p.type_.cc_name)).collect_vec();

        thunks.push(quote! {
            extern "C" #return_type_name #thunk_ident( #( #param_types #param_idents ),* ) {
                return #ident( #( #param_idents ),* );
            }
        });
    }

    // In order to generate C++ thunk in all the cases Clang needs to be able to access declarations
    // from public headers of the C++ library.
    let includes = ir.used_headers.iter().map(|i| &i.name);

    let result = quote! {
        #( __HASH_TOKEN__ include #includes __NEWLINE__)*

        #( #thunks )*
    };

    token_stream_printer::cc_tokens_to_string(result)
}

#[cfg(test)]
mod tests {
    use super::Result;
    use super::{generate_rs_api, generate_rs_api_impl};
    use ir::*;
    use quote::quote;
    use token_stream_printer::cc_tokens_to_string;

    #[test]
    fn test_simple_function() -> Result<()> {
        let ir = IR {
            used_headers: vec![],
            records: vec![],
            functions: vec![Func {
                identifier: Identifier { identifier: "add".to_string() },
                mangled_name: "_Z3Addii".to_string(),
                return_type: IRType { rs_name: "i32".to_string(), cc_name: "int".to_string() },
                params: vec![
                    FuncParam {
                        identifier: Identifier { identifier: "a".to_string() },
                        type_: IRType { rs_name: "i32".to_string(), cc_name: "int".to_string() },
                    },
                    FuncParam {
                        identifier: Identifier { identifier: "b".to_string() },
                        type_: IRType { rs_name: "i32".to_string(), cc_name: "int".to_string() },
                    },
                ],
                is_inline: false,
            }],
        };
        assert_eq!(
            generate_rs_api(&ir)?,
            quote! {
                #[inline(always)]
                pub fn add(a: i32, b: i32) -> i32 {
                    unsafe { crate::detail::__rust_thunk__add(a, b) }
                }

                mod detail {
                    extern "C" {
                        #[link_name = "_Z3Addii"]
                        pub(crate) fn __rust_thunk__add(a: i32, b: i32) -> i32;
                    } // extern
                } // mod detail
            }
            .to_string()
        );
        assert_eq!(generate_rs_api_impl(&ir)?, "");
        Ok(())
    }

    #[test]
    fn test_inline_function() -> Result<()> {
        let ir = IR {
            records: vec![],
            used_headers: vec![
                HeaderName { name: "foo/bar.h".to_string() },
                HeaderName { name: "foo/baz.h".to_string() },
            ],
            functions: vec![Func {
                identifier: Identifier { identifier: "add".to_string() },
                mangled_name: "_Z3Addii".to_string(),
                return_type: IRType { rs_name: "i32".to_string(), cc_name: "int".to_string() },
                params: vec![
                    FuncParam {
                        identifier: Identifier { identifier: "a".to_string() },
                        type_: IRType { rs_name: "i32".to_string(), cc_name: "int".to_string() },
                    },
                    FuncParam {
                        identifier: Identifier { identifier: "b".to_string() },
                        type_: IRType { rs_name: "i32".to_string(), cc_name: "int".to_string() },
                    },
                ],
                is_inline: true,
            }],
        };

        assert_eq!(
            generate_rs_api(&ir)?,
            quote! {#[inline(always)]
                pub fn add(a: i32, b: i32) -> i32 {
                    unsafe { crate::detail::__rust_thunk__add(a, b) }
                }

                mod detail {
                    extern "C" {
                        pub(crate) fn __rust_thunk__add(a: i32, b: i32) -> i32;
                    } // extern
                } // mod detail
            }
            .to_string()
        );

        assert_eq!(
            generate_rs_api_impl(&ir)?,
            cc_tokens_to_string(quote! {
                __HASH_TOKEN__ include "foo/bar.h" __NEWLINE__
                __HASH_TOKEN__ include "foo/baz.h" __NEWLINE__

                extern "C" int __rust_thunk__add(int a, int b) {
                    return add(a, b);
                }
            })?
        );
        Ok(())
    }

    #[test]
    fn test_simple_struct() -> Result<()> {
        let ir = IR {
            used_headers: vec![],
            records: vec![Record {
                identifier: Identifier { identifier: "SomeStruct".to_string() },
                fields: vec![
                    Field {
                        identifier: Identifier { identifier: "first_field".to_string() },
                        type_: IRType { rs_name: "i32".to_string(), cc_name: "int".to_string() },
                    },
                    Field {
                        identifier: Identifier { identifier: "second_field".to_string() },
                        type_: IRType { rs_name: "i32".to_string(), cc_name: "int".to_string() },
                    },
                ],
            }],
            functions: vec![],
        };
        assert_eq!(
            generate_rs_api(&ir)?,
            quote! {
                #[repr(C)]
                pub struct SomeStruct {
                    pub first_field: i32,
                    pub second_field: i32,
                }
            }
            .to_string()
        );
        assert_eq!(generate_rs_api_impl(&ir)?, "");
        Ok(())
    }
}