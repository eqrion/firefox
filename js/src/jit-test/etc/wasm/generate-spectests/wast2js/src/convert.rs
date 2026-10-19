/* Copyright 2021 Mozilla Foundation
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

use anyhow::{anyhow, bail, Context as _, Result};
use regex::Regex;
use std::collections::HashSet;
use std::fmt::Write;
use std::path::Path;
use std::str;

use super::out::*;

const HARNESS: &'static str = include_str!("./harness.js");
const LICENSE: &'static str = include_str!("./license.js");

pub fn harness() -> String {
    HARNESS.to_string()
}

pub fn convert<P: AsRef<Path>>(path: P, wast: &str) -> Result<String> {
    let filename = path.as_ref();
    let adjust_wast = |mut err: wast::Error| {
        err.set_path(filename);
        err.set_text(wast);
        err
    };

    println!("Processing {} ...", filename.display());

    let mut lexer = wast::lexer::Lexer::new(wast);
    // The 'names.wast' spec test has confusable unicode -- disable detection.
    lexer.allow_confusing_unicode(filename.ends_with("names.wast"));
    let buf = wast::parser::ParseBuffer::new_with_lexer(lexer).map_err(adjust_wast)?;
    let ast = wast::parser::parse::<wast::Wast>(&buf).map_err(adjust_wast)?;

    let mut out = String::new();

    writeln!(&mut out, "{}", LICENSE)?;
    writeln!(&mut out, "// {}", filename.display())?;

    let mut state = State::default();

    for directive in ast.directives {
        let sp = directive.span();
        let (line, col) = sp.linecol_in(wast);
        writeln!(&mut out, "")?;
        convert_directive(
            directive, &mut state, filename, line, col, wast, &mut out, None,
        )
        .with_context(|| {
            format!(
                "failed to convert directive on {}:{}:{}",
                filename.display(),
                line + 1,
                col
            )
        })?;
    }

    Ok(out)
}

/// Tracks the JS variables holding instances and definitions, so that
/// directives can refer to the current instance and treat components
/// differently from core modules.
#[derive(Default)]
struct State {
    // The number of anonymous instance variables (`$0`, `$1`, ...) created.
    num_instances: usize,
    // The name of the JS variable (without the `$`) holding the current
    // instance, which is the most recently created one.
    current_instance: Option<String>,
    // Names of JS variables (without the `$`) holding components or component
    // instances.
    components: HashSet<String>,
    // Names of JS variables (without the `$`) that have been declared. Scripts
    // may reuse identifiers, which JS does not allow for `let`.
    declared: HashSet<String>,
}

impl State {
    // Returns the JS to bind a value to the variable `$name`.
    fn bind(&mut self, name: &str) -> String {
        if self.declared.insert(name.to_string()) {
            format!("let ${}", name)
        } else {
            format!("${}", name)
        }
    }

    fn next_instance(&mut self) -> usize {
        self.num_instances += 1;
        self.num_instances - 1
    }

    fn set_current_instance(&mut self, name: &str, is_component: bool) {
        self.current_instance = Some(name.to_string());
        self.set_is_component(name, is_component);
    }

    fn set_is_component(&mut self, name: &str, is_component: bool) {
        if is_component {
            self.components.insert(name.to_string());
        } else {
            self.components.remove(name);
        }
    }

    fn is_component(&self, name: &str) -> bool {
        self.components.contains(name)
    }

    // Resolves an optional instance identifier to a JS variable name, without
    // the `$`.
    fn instance_name(&self, id: Option<wast::token::Id>) -> String {
        id.map(js_var)
            .unwrap_or_else(|| self.current_instance.clone().unwrap())
    }
}

// Returns the JS variable name, without the `$`, for a text format identifier.
// Identifiers may contain characters that JS doesn't allow, which are replaced
// by their hex code point.
fn js_var(id: wast::token::Id) -> String {
    id.name()
        .chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() || c == '_' {
                c.to_string()
            } else {
                format!("_{:x}_", c as u32)
            }
        })
        .collect()
}

fn convert_directive(
    directive: wast::WastDirective,
    state: &mut State,
    filename: &Path,
    line: usize,
    col: usize,
    wast: &str,
    out: &mut String,
    escape: Option<&dyn Fn(&str) -> String>,
) -> Result<()> {
    use wast::WastDirective::*;

    // A local replacement for `writeln!` that automatically applies the
    // provided escape function.
    macro_rules! writejs {
        ($($arg:tt)*) => {{
            let s = format!($($arg)*);
            let s = if let Some(escape_fn) = escape {
                escape_fn(&s)
            } else {
                s
            };
            writeln!(out, "{}", s)
        }};
    }

    if col == 1 {
        writejs!("// {}:{}", filename.display(), line + 1)?;
    } else {
        writejs!("// {}:{}:{}", filename.display(), line + 1, col)?;
    }

    // For full definitions of these directives, see
    // https://github.com/WebAssembly/spec/blob/main/interpreter/README.md#scripts
    match directive {
        // (module $M? ...) or (component $C? ...): Instantiate a module or
        // component with an optional identifier.
        Module(wast::QuoteWat::Wat(wat)) => {
            let next_instance = state.next_instance();
            let is_component = matches!(wat, wast::Wat::Component(_));
            let (text, id) = wat_to_js_string(&wat, wast)?;
            let func = if is_component {
                "instantiateComponent"
            } else {
                "instantiate"
            };

            writejs!("let ${} = {}(`{}`);", next_instance, func, text)?;
            if let Some(id) = id {
                let var = js_var(id);
                writejs!("{} = ${};", state.bind(&var), next_instance)?;
                state.set_is_component(&var, is_component);
            }

            state.set_current_instance(&next_instance.to_string(), is_component);
        }
        // (module quote "..."): Instantiate a module using the given literal text.
        // Identifiers are not supported.
        Module(wast::QuoteWat::QuoteModule(_, source)) => {
            let next_instance = state.next_instance();
            let module_text = quote_module_to_js_string(source)?;

            writejs!("let ${} = instantiate(`{}`);", next_instance, module_text)?;

            state.set_current_instance(&next_instance.to_string(), false);
        }
        // (component quote "..."): Instantiate a component using the given
        // literal text.
        Module(wast::QuoteWat::QuoteComponent(_, source)) => {
            let next_instance = state.next_instance();
            let component_text = quote_component_to_js_string(source)?;

            writejs!(
                "let ${} = instantiateComponent(`{}`);",
                next_instance,
                component_text
            )?;

            state.set_current_instance(&next_instance.to_string(), true);
        }

        // (module definition $M ...) or (component definition $C ...): Define
        // and validate, but do not instantiate, a module or component
        ModuleDefinition(wast::QuoteWat::Wat(wat)) => {
            let is_component = matches!(wat, wast::Wat::Component(_));
            let func = if is_component { "component" } else { "module" };
            let id = match &wat {
                wast::Wat::Module(m) => m.id,
                wast::Wat::Component(c) => c.id,
            };
            let text = definition_to_js_string(wat.span(), wast)?;
            if let Some(id) = id {
                let var = js_var(id);
                writejs!("{} = {}(`{}`);", state.bind(&var), func, text)?;
                state.set_is_component(&var, is_component);
            } else {
                writejs!("let _anon_{} = {}(`{}`);", line, func, text)?;
            }
        }
        ModuleDefinition(..) => bail!("unsupported module definition...definition"),

        // (module instance $I $M) or (component instance $I $C): Instantiate a
        // specific module or component, which becomes the current instance
        ModuleInstance {
            span: _,
            instance: Some(instance),
            module: Some(module),
        } => {
            let (instance, module) = (js_var(instance), js_var(module));
            let is_component = state.is_component(&module);
            let func = if is_component {
                "instantiateFromComponent"
            } else {
                "instantiateFromModule"
            };
            writejs!("{} = {}(${});", state.bind(&instance), func, module)?;
            state.set_current_instance(&instance, is_component);
        }
        ModuleInstance { .. } => bail!("unsupported module instance definition"),

        // (register "a"): Register the current instance
        Register {
            span: _,
            name,
            module: None,
        } => {
            writejs!(
                "register(${}, `{}`);",
                state.current_instance.as_ref().unwrap(),
                escape_template_name_string(name)
            )?;
        }
        // (register "a" $a): Register an item by identifier
        Register {
            span: _,
            name,
            module: Some(thing),
        } => {
            writejs!(
                "register(${}, `{}`);",
                js_var(thing),
                escape_template_name_string(name)
            )?;
        }

        // (invoke $M? "a" ...args): Call an exported function
        Invoke(i) => {
            let (invoke_node, _) = invoke_to_js(state, i)?;
            writejs!("{};", invoke_node.output(0))?;
        }

        // (assert_return ...)
        AssertReturn {
            span: _,
            exec,
            results,
        } => {
            let (exec_node, is_component) = execute_to_js(state, exec, wast)?;
            let expected_node = to_js_value_array(&results, assert_expression_to_js_value)?;
            let name = if is_component {
                "assert_return_component"
            } else {
                "assert_return"
            };
            writejs!(
                "{};",
                JSNode::Assert {
                    name: name.to_string(),
                    exec: exec_node,
                    expected: expected_node,
                }
                .output(0),
            )?;
        }

        // (assert_trap ...)
        AssertTrap {
            span: _,
            exec,
            message,
        } => {
            let (exec_node, _) = execute_to_js(state, exec, wast)?;
            let expected_node = Box::new(JSNode::Raw(format!(
                "`{}`",
                escape_template_name_string(message)
            )));
            writejs!(
                "{};",
                JSNode::Assert {
                    name: "assert_trap".to_string(),
                    exec: exec_node,
                    expected: expected_node,
                }
                .output(0),
            )?;
        }

        // (assert_exhaustion ...)
        AssertExhaustion {
            span: _,
            call,
            message,
        } => {
            let (exec_node, _) = invoke_to_js(state, call)?;
            let expected_node = Box::new(JSNode::Raw(format!(
                "`{}`",
                escape_template_name_string(message)
            )));
            writejs!(
                "{};",
                JSNode::Assert {
                    name: "assert_exhaustion".to_string(),
                    exec: exec_node,
                    expected: expected_node,
                }
                .output(0),
            )?;
        }

        // (assert_invalid ...)
        AssertInvalid {
            span: _,
            module,
            message,
        } => {
            let exec = Box::new(JSNode::Raw(compile_to_js(module, wast)?));
            let expected_node = Box::new(JSNode::Raw(format!(
                "`{}`",
                escape_template_name_string(message)
            )));
            writejs!(
                "{};",
                JSNode::Assert {
                    name: "assert_invalid".to_string(),
                    exec: exec,
                    expected: expected_node,
                }
                .output(0),
            )?;
        }

        // (assert_malformed ...)
        AssertMalformed {
            module,
            span: _,
            message,
        } => {
            let is_quote = !matches!(module, wast::QuoteWat::Wat(_));
            let exec = match compile_to_js(module, wast) {
                Ok(exec) => exec,
                Err(err) if is_quote => {
                    writejs!("// ignoring badly-encoded assert_malformed: {:?}", err)?;
                    return Ok(());
                }
                Err(err) => return Err(err),
            };
            let exec = Box::new(JSNode::Raw(exec));
            let expected_node = Box::new(JSNode::Raw(format!(
                "`{}`",
                escape_template_name_string(message)
            )));
            writejs!(
                "{};",
                JSNode::Assert {
                    name: "assert_malformed".to_string(),
                    exec: exec,
                    expected: expected_node,
                }
                .output(0),
            )?;
        }

        // (assert_invalid_custom ...) or (assert_malformed_custom ...): Errors
        // in custom sections don't affect the module, so it must compile.
        AssertInvalidCustom { module, .. } | AssertMalformedCustom { module, .. } => {
            writejs!("{};", compile_to_js(module, wast)?)?;
        }

        // (assert_unlinkable ...)
        AssertUnlinkable {
            span: _,
            module,
            message,
        } => {
            let exec = Box::new(JSNode::Raw(instantiate_to_js(&module, wast)?));
            let expected_node = Box::new(JSNode::Raw(format!(
                "`{}`",
                escape_template_name_string(message)
            )));
            writejs!(
                "{};",
                JSNode::Assert {
                    name: "assert_unlinkable".to_string(),
                    exec: exec,
                    expected: expected_node,
                }
                .output(0),
            )?;
        }

        AssertException { span: _, exec } => {
            // This assert doesn't have a second parameter, so we don't bother
            // formatting it using an Assert node.
            let (exec_node, _) = execute_to_js(state, exec, wast)?;
            writejs!("assert_exception(() => {});", exec_node.output(0))?;
        }
        AssertSuspension {
            span: _,
            exec,
            message,
        } => {
            let (exec_node, _) = execute_to_js(state, exec, wast)?;
            let expected_node = Box::new(JSNode::Raw(format!(
                "`{}`",
                escape_template_name_string(message)
            )));
            writejs!(
                "{};",
                JSNode::Assert {
                    name: "assert_suspension".to_string(),
                    exec: exec_node,
                    expected: expected_node,
                }
                .output(0),
            )?;
        }
        Thread(thread) => {
            let (shared_module, shared_module_name) = match thread.shared_module {
                Some(m) => (format!("${}", js_var(m)), format!("${}", js_var(m))),
                None => ("null".to_string(), "__nomodule".to_string()),
            };

            writejs!(
                "let ${} = new Thread({}, \"{}\", `",
                js_var(thread.name),
                shared_module,
                shared_module_name,
            )?;

            // The thread runs in a separate global, with its own declarations.
            let outer_declared = std::mem::take(&mut state.declared);
            for directive in thread.directives {
                let sp = directive.span();
                let (line, col) = sp.linecol_in(wast);
                writejs!("")?;
                convert_directive(
                    directive,
                    state,
                    filename,
                    line,
                    col,
                    wast,
                    out,
                    Some(&escape_template_module_string),
                )
                .with_context(|| {
                    format!(
                        "failed to convert thread directive on {}:{}:{}",
                        filename.display(),
                        line + 1,
                        col
                    )
                })?;
            }
            state.declared = outer_declared;

            writejs!("`);")?;
        }
        Wait { span: _, thread } => {
            writejs!("${}.wait();", js_var(thread))?;
        }

        AssertInvalidCustom {
            span: _,
            module,
            message,
        } => {
            writejs!(
                "{};",
                assert_bad_custom_to_js("assert_invalid_custom", module, message, wast)?
            )?;
        }
        AssertMalformedCustom {
            span: _,
            module,
            message,
        } => {
            writejs!(
                "{};",
                assert_bad_custom_to_js("assert_malformed_custom", module, message, wast)?
            )?;
        }
    }

    Ok(())
}

/// Custom section tests can be "bad" in three ways: the text format can be
/// malformed, a custom section can be structurally invalid, or a custom
/// section can be well-formed but be semantically wrong (e.g. referencing
/// functions that don't exist.) None of this really matters from a harness
/// point of view except for knowing whether or not the WAT will parse, so we
/// lump them together.
fn assert_bad_custom_to_js(
    directive: &str,
    module: wast::QuoteWat,
    message: &str,
    wast: &str,
) -> Result<String> {
    let (text, text_is_malformed) = match module {
        wast::QuoteWat::Wat(wat @ wast::Wat::Module(_)) => (wat_to_js_string(&wat, wast)?.0, false),
        wast::QuoteWat::QuoteModule(_, source) => {
            let text = quote_text(source)?;
            let parses = match wast::parser::ParseBuffer::new(&text) {
                Ok(buf) => wast::parser::parse::<wast::Wat>(&buf).is_ok(),
                Err(_) => false,
            };
            (escape_template_module_string(&text), !parses)
        }
        other => bail!("unsupported {:?} in {}", other, directive),
    };
    let name = if text_is_malformed {
        // If we can't even parse the WAT, we use the general assert_malformed
        // directive instead of anything about custom sections. This is sort of
        // a bug in the upstream tests if we're being honest.
        "assert_malformed"
    } else {
        directive
    };
    Ok(JSNode::Assert {
        name: name.to_string(),
        exec: Box::new(JSNode::Raw(format!("module(`{}`)", text))),
        expected: Box::new(JSNode::Raw(format!(
            "`{}`",
            escape_template_name_string(message)
        ))),
    }
    .output(0))
}

fn escape_template_string(text: &str, escape_ascii_lf_tab: bool) -> String {
    let mut escaped = String::new();
    for c in text.chars() {
        match c {
            '$' => escaped.push_str("\\$"),
            '\\' => escaped.push_str("\\\\"),
            '`' => escaped.push_str("\\`"),
            c if c.is_ascii_control() && escape_ascii_lf_tab && c != '\n' && c != '\t' => {
                escaped.push_str(&format!("\\x{:02x}", c as u32))
            }
            c if !c.is_ascii() => escaped.push_str(&c.escape_unicode().to_string()),
            c => escaped.push(c),
        }
    }
    escaped
}

// Escape a module for use in a JS template string.
fn escape_template_module_string(text: &str) -> String {
    // Modules may have comments, where line-feeds must be passed through as-is
    // to preserve their meaning
    escape_template_string(text, false)
}

// Escape a name for use in a JS template string.
fn escape_template_name_string(text: &str) -> String {
    // Names don't care about line-feeds or tabs, just need equality
    escape_template_string(text, true)
}

fn span_to_offset(span: wast::token::Span, text: &str) -> Result<usize> {
    let (span_line, span_col) = span.linecol_in(text);
    let mut cur = 0;
    // Use split_terminator instead of lines so that if there is a `\r`,
    // it is included in the offset calculation. The `+1` values below
    // account for the `\n`.
    for (i, line) in text.split_terminator('\n').enumerate() {
        if span_line == i {
            assert!(span_col < line.len());
            return Ok(cur + span_col);
        }
        cur += line.len() + 1;
    }
    bail!("invalid line/col");
}

fn closed_module(module: &str) -> Result<&str> {
    if !module.starts_with("module") && !module.starts_with("component") {
        bail!("expected module or component source");
    }

    let mut lexer = wast::lexer::Lexer::new(module);
    lexer.allow_confusing_unicode(true);

    let mut i = 0;
    let mut level = 1;

    while level != 0 {
        match lexer.parse(&mut i) {
            Ok(Some(tok)) => match tok.kind {
                wast::lexer::TokenKind::LParen => {
                    level += 1;
                }
                wast::lexer::TokenKind::RParen => {
                    level -= 1;
                }
                _ => {}
            },
            Ok(None) | Err(_) => {
                return Err(anyhow!("unable to close module"));
            }
        }
    }
    return Ok(&module[0..i]);
}

// Gets the escaped source text of a module or component, along with its
// identifier.
fn wat_to_js_string<'a>(
    wat: &wast::Wat<'a>,
    wast: &str,
) -> Result<(String, Option<wast::token::Id<'a>>)> {
    let id = match wat {
        wast::Wat::Module(m) => m.id,
        wast::Wat::Component(c) => c.id,
    };
    let offset = span_to_offset(wat.span(), wast)?;
    let opened = &wast[offset..];
    if !opened.starts_with("module") && !opened.starts_with("component") {
        return Ok((escape_template_module_string(opened), id));
    }
    Ok((
        escape_template_module_string(&format!("({}", closed_module(opened)?)),
        id,
    ))
}

fn quote_text(quotes: Vec<(wast::token::Span, &[u8])>) -> Result<String> {
    let mut text = String::new();
    for (_, src) in quotes {
        text.push_str(str::from_utf8(src)?);
        text.push_str(" ");
    }
    Ok(text)
}

fn quote_module_to_js_string(quotes: Vec<(wast::token::Span, &[u8])>) -> Result<String> {
    Ok(escape_template_module_string(&quote_text(quotes)?))
}

// Unlike modules, a bare list of component fields is not a valid component, so
// the quoted text must be wrapped.
fn quote_component_to_js_string(quotes: Vec<(wast::token::Span, &[u8])>) -> Result<String> {
    Ok(format!(
        "(component {})",
        quote_module_to_js_string(quotes)?
    ))
}

fn definition_to_js_string(span: wast::token::Span, wast: &str) -> Result<String> {
    let offset = span_to_offset(span, wast)?;
    let opened = &wast[offset..];

    // strip "module definition $M" down to "module $M", and likewise for
    // components. The identifier must be kept, as components may refer to
    // themselves by name in outer aliases.
    let pattern = r"^(module|component)\s+definition";
    let re = Regex::new(pattern).expect("Invalid regex pattern");
    let without_definition = re.replace(opened, "$1");

    Ok(escape_template_module_string(&format!(
        "({}",
        closed_module(&without_definition)?
    )))
}

// Gets a JS expression that compiles (and for modules, instantiates) the given
// module or component, for asserting that it is invalid or malformed.
fn compile_to_js(module: wast::QuoteWat, wast: &str) -> Result<String> {
    Ok(match module {
        wast::QuoteWat::Wat(wat @ wast::Wat::Module(_)) => {
            format!("instantiate(`{}`)", wat_to_js_string(&wat, wast)?.0)
        }
        wast::QuoteWat::Wat(wat @ wast::Wat::Component(_)) => {
            format!("component(`{}`)", wat_to_js_string(&wat, wast)?.0)
        }
        wast::QuoteWat::QuoteModule(_, source) => {
            format!("instantiate(`{}`)", quote_module_to_js_string(source)?)
        }
        wast::QuoteWat::QuoteComponent(_, source) => {
            format!("component(`{}`)", quote_component_to_js_string(source)?)
        }
    })
}

// Gets a JS expression that instantiates the given module or component.
fn instantiate_to_js(wat: &wast::Wat, wast: &str) -> Result<String> {
    let func = match wat {
        wast::Wat::Module(_) => "instantiate",
        wast::Wat::Component(_) => "instantiateComponent",
    };
    Ok(format!("{}(`{}`)", func, wat_to_js_string(wat, wast)?.0))
}

// Returns the JS for an invocation, and whether it invokes a component.
fn invoke_to_js(state: &State, i: wast::WastInvoke) -> Result<(Box<JSNode>, bool)> {
    let instance_name = state.instance_name(i.module);
    let instance = format!("${}", instance_name);
    let name = escape_template_name_string(i.name);
    let body = to_js_value_array(&i.args, arg_to_js_value)?;

    if state.is_component(&instance_name) {
        return Ok((
            Box::new(JSNode::Call {
                name: "invokeComponent".to_string(),
                args: vec![
                    Box::new(JSNode::Raw(instance)),
                    Box::new(JSNode::Raw(format!("`{}`", name))),
                    body,
                ],
            }),
            true,
        ));
    }

    Ok((
        Box::new(JSNode::Invoke {
            instance: instance,
            name: name,
            body: body,
        }),
        false,
    ))
}

// Returns the JS for an execution, and whether it involves a component.
fn execute_to_js(
    state: &State,
    exec: wast::WastExecute,
    wast: &str,
) -> Result<(Box<JSNode>, bool)> {
    match exec {
        wast::WastExecute::Invoke(invoke) => invoke_to_js(state, invoke),
        wast::WastExecute::Wat(wat) => {
            let is_component = matches!(wat, wast::Wat::Component(_));
            Ok((
                Box::new(JSNode::Raw(instantiate_to_js(&wat, wast)?)),
                is_component,
            ))
        }
        wast::WastExecute::Get { module, global, .. } => {
            let instanceish = state.instance_name(module);
            Ok((
                Box::new(JSNode::Raw(format!("get(${}, `{}`)", instanceish, global))),
                false,
            ))
        }
    }
}

fn to_js_value_array<T, F: Fn(&T) -> Result<String>>(vals: &[T], func: F) -> Result<Box<JSNode>> {
    let mut value_nodes: Vec<Box<JSNode>> = vec![];
    for val in vals {
        let converted_value = (func)(val)?;
        value_nodes.push(Box::new(JSNode::Raw(converted_value)));
    }

    Ok(Box::new(JSNode::Array(value_nodes)))
}

fn to_js_value_array_string<T, F: Fn(&T) -> Result<String>>(vals: &[T], func: F) -> Result<String> {
    let array = to_js_value_array(vals, func)?;
    Ok(array.output(NOWRAP))
}

fn f32_needs_bits(a: f32) -> bool {
    if a.is_infinite() {
        return false;
    }
    return a.is_nan()
        || ((a as f64) as f32).to_bits() != a.to_bits()
        || (format!("{:.}", a).parse::<f64>().unwrap() as f32).to_bits() != a.to_bits();
}

fn f64_needs_bits(a: f64) -> bool {
    return a.is_nan();
}

fn f32x4_needs_bits(a: &[wast::token::F32; 4]) -> bool {
    a.iter().any(|x| {
        let as_f32 = f32::from_bits(x.bits);
        f32_needs_bits(as_f32)
    })
}

fn f64x2_needs_bits(a: &[wast::token::F64; 2]) -> bool {
    a.iter().any(|x| {
        let as_f64 = f64::from_bits(x.bits);
        f64_needs_bits(as_f64)
    })
}

fn f32_to_js_value(val: f32) -> String {
    if val == f32::INFINITY {
        format!("Infinity")
    } else if val == f32::NEG_INFINITY {
        format!("-Infinity")
    } else if val.is_sign_negative() && val == 0f32 {
        format!("-0")
    } else {
        format!("{:.}", val)
    }
}

fn f64_to_js_value(val: f64) -> String {
    if val == f64::INFINITY {
        format!("Infinity")
    } else if val == f64::NEG_INFINITY {
        format!("-Infinity")
    } else if val.is_sign_negative() && val == 0f64 {
        format!("-0")
    } else {
        format!("{:.}", val)
    }
}

fn float32_to_js_value(val: &wast::token::F32) -> String {
    let as_f32 = f32::from_bits(val.bits);
    if f32_needs_bits(as_f32) {
        format!(
            "bytes(\"f32\", {})",
            to_js_value_array_string(&val.bits.to_le_bytes(), |x| Ok(format!("0x{:x}", x)))
                .unwrap(),
        )
    } else {
        format!("value(\"f32\", {})", f32_to_js_value(as_f32))
    }
}

fn float64_to_js_value(val: &wast::token::F64) -> String {
    let as_f64 = f64::from_bits(val.bits);
    if f64_needs_bits(as_f64) {
        format!(
            "bytes(\"f64\", {})",
            to_js_value_array_string(&val.bits.to_le_bytes(), |x| Ok(format!("0x{:x}", x)))
                .unwrap(),
        )
    } else {
        format!("value(\"f64\", {})", f64_to_js_value(as_f64))
    }
}

fn f32_pattern_to_js_value(pattern: &wast::core::NanPattern<wast::token::F32>) -> String {
    use wast::core::NanPattern::*;
    match pattern {
        CanonicalNan => format!("`canonical_nan`"),
        ArithmeticNan => format!("`arithmetic_nan`"),
        Value(x) => float32_to_js_value(x),
    }
}

fn f64_pattern_to_js_value(pattern: &wast::core::NanPattern<wast::token::F64>) -> String {
    use wast::core::NanPattern::*;
    match pattern {
        CanonicalNan => format!("`canonical_nan`"),
        ArithmeticNan => format!("`arithmetic_nan`"),
        Value(x) => float64_to_js_value(x),
    }
}

fn return_value_to_js_value(v: &wast::core::WastRetCore<'_>) -> Result<String> {
    use wast::core::WastRetCore::*;
    Ok(match v {
        I32(x) => format!("value(\"i32\", {})", x),
        I64(x) => format!("value(\"i64\", {}n)", x),
        F32(x) => f32_pattern_to_js_value(x),
        F64(x) => f64_pattern_to_js_value(x),
        RefNull(x) => match x {
            Some(wast::core::HeapType::Abstract { shared: false, ty }) => match ty {
                wast::core::AbstractHeapType::Any => format!("value('anyref', null)"),
                wast::core::AbstractHeapType::Exn => format!("value('exnref', null)"),
                wast::core::AbstractHeapType::Eq => format!("value('eqref', null)"),
                wast::core::AbstractHeapType::Array => format!("value('arrayref', null)"),
                wast::core::AbstractHeapType::Struct => format!("value('structref', null)"),
                wast::core::AbstractHeapType::I31 => format!("value('i31ref', null)"),
                wast::core::AbstractHeapType::None => format!("value('nullref', null)"),
                wast::core::AbstractHeapType::Func => format!("value('anyfunc', null)"),
                wast::core::AbstractHeapType::NoFunc => format!("value('nullfuncref', null)"),
                wast::core::AbstractHeapType::Extern => format!("value('externref', null)"),
                wast::core::AbstractHeapType::NoExtern => format!("value('nullexternref', null)"),
                wast::core::AbstractHeapType::Exn => format!("value('exnref', null)"),
                wast::core::AbstractHeapType::NoExn => format!("value('nullexnref', null)"),
                other => bail!(
                    "couldn't convert ref.null {:?} to a js assertion value",
                    other
                ),
            },
            None => "null".to_string(),
            other => bail!(
                "couldn't convert ref.null {:?} to a js assertion value",
                other
            ),
        },
        RefFunc(_) => format!("new RefWithType('funcref')"),
        RefExtern(x) => match x {
            Some(v) => format!("new ExternRefResult({})", v),
            None => format!("new RefWithType('externref')"),
        },
        RefHost(x) => format!("new HostRefResult({})", x),
        RefAny => format!("new RefWithType('anyref')"),
        RefEq => format!("new RefWithType('eqref')"),
        RefArray => format!("new RefWithType('arrayref')"),
        RefStruct => format!("new RefWithType('structref')"),
        RefI31 => format!("new RefWithType('i31ref')"),
        V128(x) => {
            use wast::core::V128Pattern::*;
            match x {
                I8x16(elements) => format!(
                    "i8x16({})",
                    to_js_value_array_string(elements, |x| Ok(format!("0x{:x}", x)))?,
                ),
                I16x8(elements) => format!(
                    "i16x8({})",
                    to_js_value_array_string(elements, |x| Ok(format!("0x{:x}", x)))?,
                ),
                I32x4(elements) => format!(
                    "i32x4({})",
                    to_js_value_array_string(elements, |x| Ok(format!("0x{:x}", x)))?,
                ),
                I64x2(elements) => format!(
                    "i64x2({})",
                    to_js_value_array_string(elements, |x| Ok(format!("0x{:x}n", x)))?,
                ),
                F32x4(elements) => {
                    let elements: Vec<String> = elements
                        .iter()
                        .map(|x| f32_pattern_to_js_value(x))
                        .collect();
                    let elements = strings_to_raws(elements);
                    JSNode::Call {
                        name: "new F32x4Pattern".to_string(),
                        args: elements,
                    }
                    .output(0)
                }
                F64x2(elements) => {
                    let elements: Vec<String> = elements
                        .iter()
                        .map(|x| f64_pattern_to_js_value(x))
                        .collect();
                    let elements = strings_to_raws(elements);
                    JSNode::Call {
                        name: "new F64x2Pattern".to_string(),
                        args: elements,
                    }
                    .output(0)
                }
            }
        }
        Either(v) => {
            let args = v
                .iter()
                .map(|v| return_value_to_js_value(v).unwrap())
                .collect();
            let args = strings_to_raws(args);
            JSNode::Call {
                name: "either".to_string(),
                args: args,
            }
            .output(0)
        }
        other => bail!("couldn't convert Core({:?}) to a js assertion value", other),
    })
}

fn assert_expression_to_js_value(v: &wast::WastRet<'_>) -> Result<String> {
    use wast::WastRet::*;

    Ok(match &v {
        Core(x) => return_value_to_js_value(x)?,
        Component(x) => component_value_to_js(x)?,
        other => bail!("couldn't convert {:?} to a js assertion value", other),
    })
}

// Converts a component model value to a call of the harness's `cv` value
// constructors, which choose the JS representation.
fn component_value_to_js(v: &wast::component::WastVal<'_>) -> Result<String> {
    use wast::component::WastVal as V;

    let call = |name: &str, arg: String| format!("cv.{}({})", name, arg);
    let string = |s: &str| format!("`{}`", escape_template_name_string(s));
    let list = |vals: &[V<'_>]| -> Result<String> {
        let vals = vals
            .iter()
            .map(component_value_to_js)
            .collect::<Result<Vec<_>>>()?;
        Ok(format!("[{}]", vals.join(", ")))
    };
    let payload = |val: &Option<Box<V<'_>>>| -> Result<String> {
        match val {
            Some(val) => component_value_to_js(val),
            None => Ok(String::new()),
        }
    };

    Ok(match v {
        V::Bool(x) => call("bool", x.to_string()),
        V::U8(x) => call("u8", x.to_string()),
        V::S8(x) => call("s8", x.to_string()),
        V::U16(x) => call("u16", x.to_string()),
        V::S16(x) => call("s16", x.to_string()),
        V::U32(x) => call("u32", x.to_string()),
        V::S32(x) => call("s32", x.to_string()),
        V::U64(x) => call("u64", format!("{}n", x)),
        V::S64(x) => call("s64", format!("{}n", x)),
        V::F32(x) => {
            let x = f32::from_bits(x.bits);
            call(
                "f32",
                if x.is_nan() {
                    "NaN".to_string()
                } else {
                    f32_to_js_value(x)
                },
            )
        }
        V::F64(x) => {
            let x = f64::from_bits(x.bits);
            call(
                "f64",
                if x.is_nan() {
                    "NaN".to_string()
                } else {
                    f64_to_js_value(x)
                },
            )
        }
        V::Char(x) => call("char", string(&x.to_string())),
        V::String(x) => call("string", string(x)),
        V::List(vals) => call("list", list(vals)?),
        V::Tuple(vals) => call("tuple", list(vals)?),
        V::Record(fields) => {
            let fields = fields
                .iter()
                .map(|(name, val)| {
                    Ok(format!(
                        "[{}, {}]",
                        string(name),
                        component_value_to_js(val)?
                    ))
                })
                .collect::<Result<Vec<_>>>()?;
            call("record", format!("[{}]", fields.join(", ")))
        }
        V::Variant(case, val) => {
            let val = payload(val)?;
            if val.is_empty() {
                call("variant", string(case))
            } else {
                call("variant", format!("{}, {}", string(case), val))
            }
        }
        V::Enum(case) => call("enum", string(case)),
        V::Option(None) => "cv.none()".to_string(),
        V::Option(Some(val)) => call("some", component_value_to_js(val)?),
        V::Result(Ok(val)) => call("ok", payload(val)?),
        V::Result(Err(val)) => call("err", payload(val)?),
        V::Flags(flags) => {
            let flags: Vec<String> = flags.iter().map(|x| string(x)).collect();
            call("flags", format!("[{}]", flags.join(", ")))
        }
    })
}

fn arg_to_js_value(v: &wast::WastArg<'_>) -> Result<String> {
    use wast::core::WastArgCore::*;
    use wast::WastArg::Core;

    if let wast::WastArg::Component(x) = v {
        return component_value_to_js(x);
    }

    Ok(match &v {
        Core(I32(x)) => format!("{}", *x),
        Core(I64(x)) => format!("{}n", *x),
        Core(F32(x)) => float32_to_js_value(x),
        Core(F64(x)) => float64_to_js_value(x),
        Core(V128(x)) => {
            use wast::core::V128Const::*;
            match x {
                I8x16(elements) => format!(
                    "i8x16({})",
                    to_js_value_array_string(elements, |x| Ok(format!("0x{:x}", x)))?,
                ),
                I16x8(elements) => format!(
                    "i16x8({})",
                    to_js_value_array_string(elements, |x| Ok(format!("0x{:x}", x)))?,
                ),
                I32x4(elements) => format!(
                    "i32x4({})",
                    to_js_value_array_string(elements, |x| Ok(format!("0x{:x}", x)))?,
                ),
                I64x2(elements) => format!(
                    "i64x2({})",
                    to_js_value_array_string(elements, |x| Ok(format!("0x{:x}n", x)))?,
                ),
                F32x4(elements) => {
                    if f32x4_needs_bits(elements) {
                        let bytes =
                            to_js_value_array(&x.to_le_bytes(), |x| Ok(format!("0x{:x}", x)))?;
                        format!("bytes('v128', {})", bytes.output(0))
                    } else {
                        let vals = to_js_value_array(elements, |x| {
                            Ok(f32_to_js_value(f32::from_bits(x.bits)))
                        })?;
                        format!("f32x4({})", vals.output(0))
                    }
                }
                F64x2(elements) => {
                    if f64x2_needs_bits(elements) {
                        let bytes =
                            to_js_value_array(&x.to_le_bytes(), |x| Ok(format!("0x{:x}", x)))?;
                        format!("bytes('v128', {})", bytes.output(0))
                    } else {
                        let vals = to_js_value_array(elements, |x| {
                            Ok(f64_to_js_value(f64::from_bits(x.bits)))
                        })?;
                        format!("f64x2({})", vals.output(0))
                    }
                }
            }
        }
        Core(RefNull(_)) => format!("null"),
        Core(RefExtern(x)) => format!("externref({})", x),
        Core(RefHost(x)) => format!("hostref({})", x),
        other => bail!("couldn't convert {:?} to a js value", other),
    })
}
