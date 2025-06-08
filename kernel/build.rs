fn main() {
    let cpu = "riscv";

    // The bindgen::Builder is the main entry point
    // to bindgen, and lets you build up options for
    // the resulting bindings.
    let bindings = bindgen::Builder::default()
        .clang_args([
            "-Iinclude",
            "-Iinclude/badgelib",
            "-Iport/generic/include",
            &format!("-Icpu/{}/include", cpu),
            "-I../common/include",
            "-I../common/badgelib/include",
            "-Wno-unknown-attributes",
            "-DBADGEROS_KERNEL",
        ])
        // Fix: For some reason, `malloc` and `realloc` specifically do not use `usize`.
        .blocklist_function("malloc")
        .blocklist_function("realloc")
        .blocklist_function("calloc")
        // The input header we would like to generate
        // bindings for.
        .header("include/rust_bindgen_wrapper.h")
        // Tell cargo to invalidate the built crate whenever any of the
        // included header files changed.
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        // Use core, not std.
        .use_core()
        // Finish the builder and generate the bindings.
        .generate()
        // Unwrap the Result and panic on failure.
        .expect("Unable to generate bindings");

    bindings
        .write_to_file("build/bindings.rs")
        .expect("Couldn't write bindings!");
}
