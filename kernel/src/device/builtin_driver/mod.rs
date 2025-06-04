pub mod ahci;

#[unsafe(no_mangle)]
unsafe fn add_rust_builtin_drivers() {
    ahci::add_drivers();
}
