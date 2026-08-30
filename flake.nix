{
  description = "Cosmos: Embeddable Deterministic Simulation Testing (DST) library for C/C++";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};

      tools = with pkgs; [
        cmake
        gnumake
        just
        clang-tools
        gdb
        jujutsu
      ];

      # cmake caches the first `cc` it finds, so a build directory created by one
      # toolchain misbehaves when another reuses it: the objects and the link step
      # disagree and every binary aborts at startup. Each shell therefore pins both
      # its compiler and its own build directory.
      shellFor = stdenv: buildDir:
        (pkgs.mkShell.override { inherit stdenv; }) {
          packages = tools;
          env = {
            CC = "${stdenv.cc}/bin/cc";
            CXX = "${stdenv.cc}/bin/c++";
            BUILD_DIR = buildDir;
          };
        };
    in
    {
      devShells.${system} = {
        default = shellFor pkgs.stdenv "build";

        # `nix develop .#clang` — the wrappers are linker interposition, and
        # docs/linker-interposition.md claims lld parity for --wrap that only a
        # clang build can check.
        clang = shellFor pkgs.clangStdenv "build-clang";
      };
    };
}
