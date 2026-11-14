//! `Function::buffer_encoders`: one decode answers every buffer index as
//! `argument_buffer_layout` answers it alone, a plain buffer of no struct type with Metal's empty
//! encoder, and an index with no buffer not at all.

use super::argument_buffers::{fixture_function, function_layout};
use crate::bind::{ArgumentBufferLayout, ArgumentLayoutError};
use crate::budget::Budget;
use crate::library::FunctionConstantValues;

#[test]
fn every_buffer_index_answers_as_its_own_layout() {
    // (kernel, its argument buffers, its plain buffers)
    let kernels: [(&str, &[u32], &[u32]); 4] = [
        ("kA", &[0], &[1]),
        ("kH", &[2], &[0]),
        ("kAA", &[0], &[1]),
        ("kP", &[], &[0]),
    ];
    for (kernel, arguments, plain) in kernels {
        let Some(function) = fixture_function(kernel) else {
            return;
        };
        let encoders = function
            .buffer_encoders(&FunctionConstantValues::defaults(), &Budget::default())
            .expect("the kernel decodes");
        let declared: Vec<(u32, bool)> = encoders
            .iter()
            .map(|b| (b.index, b.argument_buffer))
            .collect();
        let mut want: Vec<(u32, bool)> = arguments.iter().map(|&i| (i, true)).collect();
        want.extend(plain.iter().map(|&i| (i, false)));
        want.sort_unstable();
        assert_eq!(declared, want, "{kernel}");
        for b in &encoders {
            let alone = function_layout(kernel, b.index).expect("assembled");
            if b.argument_buffer {
                assert_eq!(b.layout, alone, "{kernel} {}", b.index);
            } else {
                assert_eq!(
                    alone,
                    Err(ArgumentLayoutError::NoArgumentBuffer { index: b.index })
                );
            }
        }
        for index in (0..31).filter(|i| !declared.iter().any(|(d, _)| d == i)) {
            assert_eq!(
                function_layout(kernel, index).expect("assembled"),
                Err(ArgumentLayoutError::NoArgumentBuffer { index }),
                "{kernel} {index}"
            );
        }
    }
}

#[test]
fn a_plain_buffer_of_no_struct_takes_metals_empty_encoder() {
    let Some(function) = fixture_function("kP") else {
        return;
    };
    let encoders = function
        .buffer_encoders(&FunctionConstantValues::defaults(), &Budget::default())
        .expect("the kernel decodes");
    assert_eq!(encoders.len(), 1);
    assert!(!encoders[0].argument_buffer);
    let empty = ArgumentBufferLayout {
        encoded_length: 0,
        alignment: 1,
        members: Vec::new(),
    };
    assert_eq!(encoders[0].layout, Ok(empty));
}

#[test]
fn an_array_of_arrays_refuses_only_its_own_index() {
    let Some(function) = fixture_function("kAA") else {
        return;
    };
    let encoders = function
        .buffer_encoders(&FunctionConstantValues::defaults(), &Budget::default())
        .expect("the kernel decodes");
    assert_eq!(
        encoders[0].layout,
        Err(ArgumentLayoutError::ArrayOfArrays { id: 0 })
    );
    assert!(encoders[1].layout.is_ok(), "its plain buffer still answers");
}
