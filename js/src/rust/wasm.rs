/* Copyright 2020 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#[no_mangle]
pub unsafe extern "C" fn wasm_text_to_binary(
    text: *const u16,
    text_len: usize,
    out_bytes: *mut *mut u8,
    out_bytes_len: *mut usize,
    out_error: *mut *mut u8,
    out_error_len: *mut usize,
) -> bool {
    let text_slice = std::slice::from_raw_parts(text, text_len);
    let text = String::from_utf16_lossy(text_slice);

    match text_to_binary(&text) {
        Ok(bytes) => {
            let bytes_box = bytes.into_boxed_slice();
            let bytes_slice = Box::leak(bytes_box);
            out_bytes.write(bytes_slice.as_mut_ptr());
            out_bytes_len.write(bytes_slice.len());
            true
        }
        Err(error) => {
            let error = Box::leak(format!("{}\0", error).into_boxed_str());
            out_error.write(error.as_mut_ptr());
            out_error_len.write(error.len());
            false
        }
    }
}

fn text_to_binary(text: &str) -> Result<Vec<u8>, wast::Error> {
    let mut lexer = wast::lexer::Lexer::new(text);
    // The 'names.wast' spec test has confusable unicode, so disable detection.
    // This protection is not very useful for a shell testing function anyways.
    lexer.allow_confusing_unicode(true);
    let buf = wast::parser::ParseBuffer::new_with_lexer(lexer)?;
    let mut ast = wast::parser::parse::<wast::Wat>(&buf)?;
    if let wast::Wat::Module(module) = &mut ast {
        // Resolution is what fills in the table index this drops, and encode()
        // runs it again, which is a no-op the second time round.
        module.resolve()?;
        use_wasm10_elem_encoding(module);
    }
    return ast.encode();
}

/// Emit an active element segment against table 0 in the wasm 1.0 encoding.
///
/// `wasm_encoder` picks the encoding from whether the table index is present:
/// absent gives spec 5.5.12's `elem ::= x:tableidx e:expr y*:vec(funcidx)`, and
/// present gives the bulk-memory flags-2 form with an explicit index and an
/// element kind. Resolution fills the index in for every segment, including for
/// the `(table funcref (elem ...))` abbreviation, so without this every segment
/// this compiler emits is post-1.0 and a 1.0-only engine rejects it.
fn use_wasm10_elem_encoding(module: &mut wast::core::Module) {
    let fields = match &mut module.kind {
        wast::core::ModuleKind::Text(fields) => fields,
        wast::core::ModuleKind::Binary(_) => return,
    };
    for field in fields {
        if let wast::core::ModuleField::Elem(elem) = field {
            if let wast::core::ElemKind::Active { table, .. } = &mut elem.kind {
                if let Some(wast::token::Index::Num(0, _)) = table {
                    *table = None;
                }
            }
        }
    }
}
