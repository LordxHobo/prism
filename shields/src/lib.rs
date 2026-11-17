// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Prism Shields: a small C API around Brave's adblock-rust engine.
//!
//! Prism (C++) loads `prism_shields.dll` at runtime and calls these functions.
//! All strings are UTF-8. Strings and buffers returned by this library must
//! be released with `prism_shields_free_string` / `prism_shields_free_buffer`.
//! No function panics across the C boundary (the crate aborts on panic).

use adblock::lists::{FilterSet, ParseOptions};
use adblock::resources::{PermissionMask, Resource};
use adblock::request::Request;
use adblock::Engine;
use std::collections::HashSet;
use std::ffi::{c_char, c_int, CStr, CString};
use std::ptr;
use std::slice;

/// Builds an engine from filter lists.
pub struct PrismShieldsBuilder {
    set: FilterSet,
}

/// A compiled engine, ready for queries.
pub struct PrismShieldsEngine {
    engine: Engine,
}

unsafe fn str_from<'a>(text: *const c_char) -> Option<&'a str> {
    if text.is_null() {
        return None;
    }
    CStr::from_ptr(text).to_str().ok()
}

unsafe fn string_from_bytes(data: *const u8, length: usize) -> Option<String> {
    if data.is_null() {
        return None;
    }
    Some(String::from_utf8_lossy(slice::from_raw_parts(data, length)).into_owned())
}

fn to_c_string(text: String) -> *mut c_char {
    // Interior NUL bytes can't occur in JSON output, but be safe.
    CString::new(text.replace('\0', ""))
        .map(CString::into_raw)
        .unwrap_or(ptr::null_mut())
}

/// The engine version, e.g. "prism_shields 0.4.0 (adblock 0.13)".
#[no_mangle]
pub extern "C" fn prism_shields_version() -> *mut c_char {
    to_c_string(format!("prism_shields {} (adblock 0.13)", env!("CARGO_PKG_VERSION")))
}

#[no_mangle]
pub extern "C" fn prism_shields_builder_new() -> *mut PrismShieldsBuilder {
    Box::into_raw(Box::new(PrismShieldsBuilder { set: FilterSet::new(false) }))
}

/// Adds one filter list (Adblock Plus / uBlock Origin syntax, or a hosts
/// file). `trusted` lists may use uBlock Origin's "trusted" scriptlets; give
/// that only to uBlock Origin's own lists. Returns 1 if the list was added.
#[no_mangle]
pub unsafe extern "C" fn prism_shields_builder_add_list(
    builder: *mut PrismShieldsBuilder,
    text: *const u8,
    length: usize,
    trusted: c_int,
) -> usize {
    let (Some(builder), Some(text)) = (builder.as_mut(), string_from_bytes(text, length)) else {
        return 0;
    };
    let options = ParseOptions {
        permissions: PermissionMask::from_bits(if trusted != 0 { 1 } else { 0 }),
        ..ParseOptions::default()
    };
    builder.set.add_filter_list(text, options);
    1
}

/// Turns the builder into an engine. The builder is consumed.
#[no_mangle]
pub unsafe extern "C" fn prism_shields_builder_build(builder: *mut PrismShieldsBuilder) -> *mut PrismShieldsEngine {
    if builder.is_null() {
        return ptr::null_mut();
    }
    let builder = Box::from_raw(builder);
    let engine = Engine::new_with_filter_set(builder.set);
    Box::into_raw(Box::new(PrismShieldsEngine { engine }))
}

#[no_mangle]
pub unsafe extern "C" fn prism_shields_builder_free(builder: *mut PrismShieldsBuilder) {
    if !builder.is_null() {
        drop(Box::from_raw(builder));
    }
}

/// Loads scriptlets and other resources (a JSON array in adblock-rust's
/// Resource format, see build-scriptlets.mjs). Returns how many were loaded.
#[no_mangle]
pub unsafe extern "C" fn prism_shields_engine_use_resources(
    engine: *mut PrismShieldsEngine,
    json: *const u8,
    length: usize,
) -> usize {
    let (Some(engine), Some(json)) = (engine.as_mut(), string_from_bytes(json, length)) else {
        return 0;
    };
    match serde_json::from_str::<Vec<Resource>>(&json) {
        Ok(resources) => {
            let count = resources.len();
            engine.engine.use_resources(resources);
            count
        }
        Err(_) => 0,
    }
}

/// Saves a compiled engine, so the next start doesn't have to parse the lists
/// again. (Resources are not included; load them again after deserializing.)
#[no_mangle]
pub unsafe extern "C" fn prism_shields_engine_serialize(engine: *const PrismShieldsEngine, length: *mut usize) -> *mut u8 {
    let Some(engine) = engine.as_ref() else {
        return ptr::null_mut();
    };
    let data = engine.engine.serialize().into_boxed_slice();
    if !length.is_null() {
        *length = data.len();
    }
    Box::into_raw(data) as *mut u8
}

#[no_mangle]
pub unsafe extern "C" fn prism_shields_free_buffer(data: *mut u8, length: usize) {
    if !data.is_null() {
        drop(Box::from_raw(ptr::slice_from_raw_parts_mut(data, length)));
    }
}

/// Loads an engine saved with prism_shields_engine_serialize. Returns null if
/// the data is damaged or from an incompatible version.
#[no_mangle]
pub unsafe extern "C" fn prism_shields_engine_deserialize(data: *const u8, length: usize) -> *mut PrismShieldsEngine {
    if data.is_null() {
        return ptr::null_mut();
    }
    let mut engine = Engine::default();
    match engine.deserialize(slice::from_raw_parts(data, length)) {
        Ok(()) => Box::into_raw(Box::new(PrismShieldsEngine { engine })),
        Err(_) => ptr::null_mut(),
    }
}

#[no_mangle]
pub unsafe extern "C" fn prism_shields_engine_free(engine: *mut PrismShieldsEngine) {
    if !engine.is_null() {
        drop(Box::from_raw(engine));
    }
}

/// What to do on a page: JSON with hide_selectors, procedural_actions,
/// exceptions, injected_script (scriptlets) and generichide.
#[no_mangle]
pub unsafe extern "C" fn prism_shields_url_cosmetic_resources(engine: *const PrismShieldsEngine, url: *const c_char) -> *mut c_char {
    let (Some(engine), Some(url)) = (engine.as_ref(), str_from(url)) else {
        return ptr::null_mut();
    };
    let resources = engine.engine.url_cosmetic_resources(url);
    match serde_json::to_string(&resources) {
        Ok(json) => to_c_string(json),
        Err(_) => ptr::null_mut(),
    }
}

/// Generic hiding: given class names and ids seen on a page (JSON arrays of
/// strings) and the page's exceptions, returns a JSON array of CSS selectors
/// to hide.
#[no_mangle]
pub unsafe extern "C" fn prism_shields_hidden_class_id_selectors(
    engine: *const PrismShieldsEngine,
    classes_json: *const c_char,
    ids_json: *const c_char,
    exceptions_json: *const c_char,
) -> *mut c_char {
    let Some(engine) = engine.as_ref() else {
        return ptr::null_mut();
    };
    let parse = |text: Option<&str>| -> Vec<String> {
        text.and_then(|t| serde_json::from_str::<Vec<String>>(t).ok()).unwrap_or_default()
    };
    let classes = parse(str_from(classes_json));
    let ids = parse(str_from(ids_json));
    let exceptions: HashSet<String> = parse(str_from(exceptions_json)).into_iter().collect();
    let selectors = engine.engine.hidden_class_id_selectors(&classes, &ids, &exceptions);
    to_c_string(serde_json::to_string(&selectors).unwrap_or_else(|_| "[]".into()))
}

/// Network check for a full request: 1 = block, 0 = allow, -1 = bad input.
/// (Used when Prism can see full URLs; see the roadmap.)
#[no_mangle]
pub unsafe extern "C" fn prism_shields_check_request(
    engine: *const PrismShieldsEngine,
    url: *const c_char,
    source_url: *const c_char,
    request_type: *const c_char,
) -> c_int {
    let (Some(engine), Some(url)) = (engine.as_ref(), str_from(url)) else {
        return -1;
    };
    let source = str_from(source_url).unwrap_or("");
    let kind = str_from(request_type).unwrap_or("other");
    match Request::new(url, source, kind, "GET") {
        Ok(request) => {
            if engine.engine.check_network_request(&request).should_block() {
                1
            } else {
                0
            }
        }
        Err(_) => -1,
    }
}

#[no_mangle]
pub unsafe extern "C" fn prism_shields_free_string(text: *mut c_char) {
    if !text.is_null() {
        drop(CString::from_raw(text));
    }
}
