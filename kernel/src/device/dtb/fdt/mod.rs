// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

use core::{cell::UnsafeCell, ptr::slice_from_raw_parts};

use alloc::{
    boxed::Box,
    collections::btree_map::BTreeMap,
    rc::{Rc, Weak},
    string::String,
};
use spec::FdtHeader;

use crate::{
    LogLevel,
    bindings::error::{EResult, Errno},
    printf, write,
};

mod spec;

/// In-memory representation of the DTB.
pub struct Fdt {
    pub root: Rc<Node>,
    pub by_phandle: BTreeMap<u32, Rc<Node>>,
}

impl Fdt {
    /// Try to parse the DTB from a blob.
    pub unsafe fn parse(blob: *const ()) -> EResult<Self> {
        unsafe {
            let header = &*(blob as *const FdtHeader);

            let struct_blk = slice_from_raw_parts(
                blob.byte_add(u32::from_be(header.struct_offset) as usize) as *const u8,
                u32::from_be(header.struct_size) as usize,
            );
            let string_blk = slice_from_raw_parts(
                blob.byte_add(u32::from_be(header.string_offset) as usize) as *const u8,
                u32::from_be(header.string_size) as usize,
            );

            Self::parse_impl(&*struct_blk, &*string_blk)
        }
    }

    /// Populate the `by_phandle` map.
    fn populate_phandles(&mut self, node: Rc<Node>) -> EResult<()> {
        if let Some(phandle) = node.phandle {
            self.by_phandle.insert(phandle, node.clone());
        }
        for child in &node.children {
            if let Entry::Node(child_node) = child.1 {
                self.populate_phandles(child_node.clone())?;
            }
        }
        Ok(())
    }

    /// Try to parse the DTB from a blob.
    pub fn parse_impl(struct_blk: &[u8], string_blk: &[u8]) -> EResult<Self> {
        if (struct_blk.as_ptr() as usize | struct_blk.len()) % 4 != 0 {
            logkf!(LogLevel::Warning, "DTB struct block is unaligned");
        }
        let mut res = Fdt {
            root: Node::parse(struct_blk, string_blk)?.0,
            by_phandle: BTreeMap::new(),
        };
        res.populate_phandles(res.root.clone())?;
        Ok(res)
    }
}

/// Child of a DTB node, either another Node, or a Prop.
pub enum Entry {
    Node(Rc<Node>),
    Prop(Rc<Prop>),
}

/// A DTB node, which may contain child nodes and props.
pub struct Node {
    parent: UnsafeCell<Weak<Node>>,
    pub name: String,
    pub phandle: Option<u32>,
    pub children: BTreeMap<String, Entry>,
}

impl Node {
    pub fn parent(&self) -> Option<Rc<Node>> {
        unsafe { self.parent.as_ref_unchecked() }.upgrade()
    }

    /// Try to parse the node from a blob.
    fn parse<'a>(mut struct_blk: &'a [u8], string_blk: &[u8]) -> EResult<(Rc<Self>, &'a [u8])> {
        loop {
            if struct_blk.len() < 8 {
                // Not enough space to have a token.
                logkf!(LogLevel::Error, "Not enough data for FDT node");
                return Err(Errno::EIO);
            }
            let mut token = [0u8; 4];
            token.copy_from_slice(&struct_blk[..4]);
            let token = u32::from_be_bytes(token);
            struct_blk = &struct_blk[4..];
            if token == spec::FDT_BEGIN_NODE {
                break;
            } else if token != spec::FDT_NOP {
                logkf!(LogLevel::Error, "Invalid FDT token");
                return Err(Errno::EIO);
            }
        }

        let name_end = match struct_blk.iter().find(|x| **x == 0) {
            Some(x) => x,
            None => {
                logkf!(LogLevel::Error, "Missing NUL terminator in FDT node name");
                return Err(Errno::EIO);
            }
        };
        let name_len = name_end as *const u8 as usize - struct_blk.as_ptr() as usize;
        let name = match str::from_utf8(&struct_blk[..name_len]) {
            Ok(x) => x,
            Err(_) => {
                logkf!(LogLevel::Error, "Invalid UTF-8 in FDT node name");
                return Err(Errno::EIO);
            }
        };
        let name = String::from(name);
        struct_blk = &struct_blk[(name_len + 1).div_ceil(4) * 4..];

        let mut children = BTreeMap::new();
        loop {
            if struct_blk.len() < 4 {
                // Not enough space to have a token.
                logkf!(LogLevel::Error, "Not enough data for FDT token");
                return Err(Errno::EIO);
            }
            let mut token = [0u8; 4];
            token.copy_from_slice(&struct_blk[..4]);
            let token = u32::from_be_bytes(token);
            if token == spec::FDT_PROP {
                // Node property.
                let child;
                (child, struct_blk) = Prop::parse(struct_blk, string_blk)?;
                if children
                    .insert(child.name.clone(), Entry::Prop(Rc::new(child)))
                    .is_some()
                {
                    // Duplicate name.
                    logkf!(LogLevel::Error, "Duplicate child");
                    return Err(Errno::EIO);
                }
            } else if token == spec::FDT_BEGIN_NODE {
                // Child node.
                let child;
                (child, struct_blk) = Node::parse(struct_blk, string_blk)?;
                if children
                    .insert(child.name.clone(), Entry::Node(child))
                    .is_some()
                {
                    // Duplicate name.
                    logkf!(LogLevel::Error, "Duplicate child");
                    return Err(Errno::EIO);
                }
            } else if token == spec::FDT_END_NODE {
                struct_blk = &struct_blk[4..];
                break;
            } else if token != spec::FDT_NOP {
                logkf!(LogLevel::Error, "Invalid FDT token");
                return Err(Errno::EIO);
            } else {
                struct_blk = &struct_blk[4..];
            }
        }

        let mut phandle = None;
        if let Some(child) = children.get("phandle") {
            if let Entry::Prop(prop) = child {
                match prop.read_u32() {
                    Ok(x) => phandle = Some(x),
                    Err(_) => logkf!(
                        LogLevel::Warning,
                        "Invalid size of phandle: {}",
                        prop.value.len()
                    ),
                }
            } else {
                logkf!(LogLevel::Warning, "Expected phandle to be a prop");
            }
        }

        let rc = Rc::new(Self {
            parent: UnsafeCell::new(Weak::new()),
            name,
            phandle,
            children,
        });

        for child in &rc.children {
            match child.1 {
                Entry::Node(node) => {
                    *unsafe { node.parent.as_mut_unchecked() } = Rc::downgrade(&rc)
                }
                Entry::Prop(prop) => {
                    *unsafe { prop.parent.as_mut_unchecked() } = Rc::downgrade(&rc)
                }
            }
        }

        Ok((rc, struct_blk))
    }

    /// Debug-print this node recursively.
    pub fn debug_print(&self, indent: usize) {
        for _ in 0..indent {
            write("    ");
        }
        if self.parent().is_none() {
            write("/");
        } else {
            write(&self.name);
        }
        write(" {\n");
        for child in &self.children {
            match child.1 {
                Entry::Node(node) => node.debug_print(indent + 1),
                Entry::Prop(prop) => prop.debug_print(indent + 1),
            }
        }
        for _ in 0..indent {
            write("    ");
        }
        write("}\n");
    }
}

/// A DTB prop, which has a blob of binary data attached.
pub struct Prop {
    parent: UnsafeCell<Weak<Node>>,
    pub name: String,
    pub value: Box<[u8]>,
}

impl Prop {
    pub fn parent(&self) -> Rc<Node> {
        unsafe { self.parent.as_ref_unchecked() }.upgrade().unwrap()
    }

    /// Try to parse the prop from a blob.
    fn parse<'a>(mut struct_blk: &'a [u8], string_blk: &[u8]) -> EResult<(Self, &'a [u8])> {
        loop {
            if struct_blk.len() < 12 {
                // Not enough space to be a prop.
                logkf!(LogLevel::Error, "Not enough data for FDT prop");
                return Err(Errno::EIO);
            }
            let mut token = [0u8; 4];
            token.copy_from_slice(&struct_blk[..4]);
            struct_blk = &struct_blk[4..];
            let token = u32::from_be_bytes(token);
            if token == spec::FDT_PROP {
                break;
            } else if token != spec::FDT_NOP {
                // Skip NOP tokens.
                logkf!(LogLevel::Error, "Invalid FDT token");
                return Err(Errno::EIO);
            }
        }

        // Parse PROP header.
        let mut len = [0u8; 4];
        len.copy_from_slice(&struct_blk[..4]);
        let len = u32::from_be_bytes(len) as usize;
        struct_blk = &struct_blk[4..];

        let mut nameoff = [0u8; 4];
        nameoff.copy_from_slice(&struct_blk[..4]);
        let nameoff = u32::from_be_bytes(nameoff) as usize;
        struct_blk = &struct_blk[4..];

        // Extract binary data blobs.
        if struct_blk.len() < len {
            logkf!(LogLevel::Error, "Not enough data for FDT prop value");
            return Err(Errno::EIO);
        }
        let value = Box::<[u8]>::from(&struct_blk[..len]);
        struct_blk = &struct_blk[len.div_ceil(4) * 4..];

        let name = &string_blk[nameoff..];
        let name_end = match name.iter().find(|x| **x == 0) {
            Some(x) => x,
            None => {
                logkf!(
                    LogLevel::Error,
                    "Missing NUL terminator in FDT strings block"
                );
                return Err(Errno::EIO);
            }
        };
        let name_len = name_end as *const u8 as usize - name.as_ptr() as usize;
        let name = match str::from_utf8(&name[..name_len]) {
            Ok(x) => x,
            Err(_) => {
                logkf!(LogLevel::Error, "Invalid UTF-8 in FDT strings block");
                return Err(Errno::EIO);
            }
        };
        let name = String::from(name);

        Ok((
            Self {
                parent: UnsafeCell::new(Weak::new()),
                name,
                value,
            },
            struct_blk,
        ))
    }

    /// Read a cell of the prop.
    pub fn read_cell(&self, index: u32) -> EResult<u32> {
        let index = index as usize;
        if self.value.len() % 4 != 0 || index >= self.value.len() / 4 {
            logkf!(LogLevel::Error, "Can't read cell from DTB property");
            return Err(Errno::EIO);
        }
        let mut tmp = [0u8; 4];
        tmp.copy_from_slice(&self.value[index * 4..index * 4 + 4]);
        Ok(u32::from_be_bytes(tmp))
    }

    /// Read the prop as a u32.
    pub fn read_u32(&self) -> EResult<u32> {
        if self.value.len() != 4 {
            logkf!(LogLevel::Error, "DTB property as one cell");
            return Err(Errno::EIO);
        }
        let mut tmp = [0u8; 4];
        tmp.copy_from_slice(&self.value);
        Ok(u32::from_be_bytes(tmp))
    }

    /// Debug-print this property recursively.
    pub fn debug_print(&self, indent: usize) {
        for _ in 0..indent {
            write("    ");
        }
        write(&self.name);
        if self.value.len() == 0 {
            write(";\n");
            return;
        }
        write(" = ");

        if self.value.first().cloned() != Some(0)
            && self.value.last().cloned() == Some(0)
            && let Some(ascii) = self.value.as_ascii()
            && ascii
                .iter()
                .find(|&&x| x.to_u8() < 0x20 && x.to_u8() != 0)
                .is_none()
        {
            write("\"");
            for char in &ascii[..ascii.len() - 1] {
                match char.to_char() {
                    '\0' => write("\\0"),
                    '\r' => write("\\r"),
                    '\n' => write("\\n"),
                    '\t' => write("\\t"),
                    ' '..'~' => write(char.as_str()),
                    _ => printf!("\\x{:02x}", char.to_u8()),
                }
            }
            write("\"");
        } else if self.value.len() % 4 == 0 {
            write("<");
            for i in 0..self.value.len() / 4 {
                if i != 0 {
                    write(" ");
                }
                let mut word = [0u8; 4];
                word.copy_from_slice(&self.value[i * 4..i * 4 + 4]);
                printf!("0x{:08x}", u32::from_be_bytes(word));
            }
            write(">");
        } else {
            write("<");
            for i in 0..self.value.len() {
                if i != 0 {
                    write(" ");
                }
                printf!("0x{:02x}", self.value[i]);
            }
            write(">");
        }

        write(";\n");
    }
}
