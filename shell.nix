let
  nixpkgs_rev = "f4429fde23e1fb20ee27f264e74c28c619d2cebb";
in

{ pkgs ? import (builtins.fetchTarball "https://github.com/NixOS/nixpkgs/archive/${nixpkgs_rev}.tar.gz") { }
}:

with pkgs;
let
  riscv32-unknown-linux-gnu = stdenv.mkDerivation rec {
    name = "riscv32-unknown-linux-gnu";
    version = "13.2.0";

    release = "2024.03.01";
    ubuntu_version = "22.04";

    src = (builtins.fetchTarball "https://github.com/riscv-collab/riscv-gnu-toolchain/releases/download/${release}/riscv32-glibc-ubuntu-${ubuntu_version}-gcc-nightly-${release}-nightly.tar.gz");

    buildInputs = [
      autoPatchelfHook

      zlib
      glib
      gmp
      zstd
      mpfr
      libmpc
      lzma
      expat
      python310
      ncurses
    ];

    installPhase = ''
      runHook preInstall

      mkdir -p $out

      cp -r bin $out
      cp -r lib $out
      cp -r libexec $out
      cp -r riscv32-unknown-linux-gnu $out

      runHook postInstall
    '';
  };
in
mkShell {
  buildInputs = [
    riscv32-unknown-linux-gnu
    cmake
    esptool
    picocom
    clang-tools
    jq
  ];
}
