//! `newArgumentEncoderWithBufferIndex:` at every buffer index a function declares, from one decode
//! of its AIR: an argument buffer's layout as [`super::argument_buffer_layout`] makes it, and at a
//! plain buffer of no struct type the encoder of no bytes, alignment 1, that Metal makes without
//! API validation (which stops it). An M4 under macOS 27.2 answered that for all 22107 such
//! buffers of its system metallibs. A plain buffer of a struct is refused as having no argument
//! buffer: Metal's unvalidated encoder of it is the GPU's own (an M4 differs from an M3 Max's
//! [`ArgumentBufferLayout::from_buffer_binding`] rule for 4980 of 13322). An index with no buffer
//! is absent: Metal asserts there.

use std::collections::BTreeMap;

use super::{guarded, spec_request, with_request, InputsFrom};
use crate::air;
use crate::air::abi::{AbiInt, EntryPoint, ParamRole, StructInfo};
use crate::bind::argument_buffer::{self, EntryLayout};
use crate::bind::{ArgumentBufferLayout, ArgumentLayoutError, BufferEncoder};
use crate::budget::Meter;
use crate::error::{Error, InternalError, InternalKind};
use crate::library::{Function, FunctionConstantValues};
use crate::lower::select::LowerInputs;
use crate::pipeline::descriptor::{DynamicLibraries, LinkedFunctions};
use crate::reflection::{self, Channel};

/// Every buffer `f` declares with the function constants `consts`, by index; specialized with
/// `consts` and no linked functions only where a function constant decides a buffer's index,
/// presence or argument buffer.
pub(crate) fn buffer_encoders(
    f: &Function,
    consts: &FunctionConstantValues,
    m: &mut Meter,
) -> Result<Vec<BufferEncoder>, ArgumentLayoutError> {
    let (linked, libraries) = (LinkedFunctions::default(), DynamicLibraries::default());
    let inputs = LowerInputs::new();
    let answer = guarded(|| {
        with_request(
            f,
            consts,
            (&linked, &libraries),
            InputsFrom::Given(&inputs),
            |r| {
                let mut decoded = air::decode(r.bitcode, m)?;
                if let Some(c) = r.container {
                    decoded.abi.check_container(&decoded.module, c, m)?;
                }
                let req = spec_request(&decoded, r.entry, r.consts)?;
                let decided = {
                    let ep = air::canon::select(&decoded.module, &decoded.abi, &req.entry)?;
                    encoders_of(ep, &decoded.abi.structs, m)
                };
                if let Some(encoders) = decided {
                    return Ok(encoders);
                }
                let spec = air::specialize_linked(decoded, &req, &[], m)?;
                encoders_of(&spec.ep, &spec.structs, m).ok_or_else(|| {
                    Error::Internal(InternalError::new(
                        InternalKind::Invariant,
                        "a specialized entry left a buffer to specialization",
                    ))
                })
            },
        )
    });
    answer.map_err(ArgumentLayoutError::Compile)
}

/// `ep`'s buffers by index, or `None` where a function constant decides one.
fn encoders_of(
    ep: &EntryPoint,
    structs: &[StructInfo],
    m: &mut Meter,
) -> Option<Vec<BufferEncoder>> {
    let mut declared = BTreeMap::new();
    for p in &ep.params {
        let (location, argument_buffer) = match &p.role {
            ParamRole::ArgumentBuffer(b) => (b.location, true),
            ParamRole::Buffer(b) => (b.location, false),
            _ => continue,
        };
        let AbiInt::Const(index) = location.index else {
            return None;
        };
        if p.predicate.is_some() || declared.insert(index, argument_buffer).is_some() {
            return None;
        }
    }
    let mut bindings = None;
    let mut encoders = Vec::with_capacity(declared.len());
    for (index, argument_buffer) in declared {
        let layout = if argument_buffer {
            match argument_buffer::entry_layout(&ep.params, structs, index, m) {
                EntryLayout::Done(layout) => layout,
                EntryLayout::Specialize => return None,
            }
        } else {
            let reflected =
                bindings.get_or_insert_with(|| reflection::bindings(ep, structs, Channel::Library));
            plain_layout(reflected, index)
        };
        encoders.push(BufferEncoder {
            index,
            argument_buffer,
            layout,
        });
    }
    Some(encoders)
}

fn plain_layout(
    bindings: &Result<Vec<reflection::Binding>, Error>,
    index: u32,
) -> Result<ArgumentBufferLayout, ArgumentLayoutError> {
    let bindings = bindings
        .as_ref()
        .map_err(|e| ArgumentLayoutError::Compile(e.clone()))?;
    bindings
        .iter()
        .find_map(|b| b.buffer().filter(|_| b.index() == index))
        .filter(|binding| binding.buffer_struct_type().is_none())
        .ok_or(ArgumentLayoutError::NoArgumentBuffer { index })
        .and_then(ArgumentBufferLayout::from_buffer_binding)
}
