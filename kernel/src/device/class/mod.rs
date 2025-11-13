// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

pub mod block;
pub mod char;
pub mod pcictl;
// pub mod tty;

use super::BaseDriver;

use alloc::boxed::Box;

use block::BlockSpecialty;
use char::CharSpecialty;
use pcictl::PciCtlSpecialty;
// use tty::TtySpecialty;

pub enum Specialty {
    None(Option<Box<dyn BaseDriver>>),
    Block(BlockSpecialty),
    Char(CharSpecialty),
    PciCtl(PciCtlSpecialty),
    // Tty(TtySpecialty),
}

impl Specialty {
    pub(super) fn base_driver(&self) -> Option<&dyn BaseDriver> {
        match self {
            Specialty::None(base) => base.as_deref(),
            Specialty::Block(blockdev) => blockdev.driver.as_deref().map(|x| x as &dyn BaseDriver),
            Specialty::Char(chardev) => chardev.driver.as_deref().map(|x| x as &dyn BaseDriver),
            Specialty::PciCtl(pcictl) => pcictl.driver.as_deref().map(|x| x as &dyn BaseDriver),
        }
    }

    pub(super) fn as_block(&self) -> Option<&BlockSpecialty> {
        match self {
            Specialty::Block(blockdev) => Some(blockdev),
            _ => None,
        }
    }

    pub(super) fn as_char(&self) -> Option<&CharSpecialty> {
        match self {
            Specialty::Char(chardev) => Some(chardev),
            _ => None,
        }
    }

    pub(super) fn as_pcictl(&self) -> Option<&PciCtlSpecialty> {
        match self {
            Specialty::PciCtl(pcictl) => Some(pcictl),
            _ => None,
        }
    }
}
