{
  description = "Hoshi: a small, simple systems programming language built on LLVM";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        llvmPkgs = pkgs.llvmPackages_21;
        stdenv = llvmPkgs.stdenv;

        hoshic = stdenv.mkDerivation {
          pname = "hoshic";
          version = "0.1.0";
          src = self;

          nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.makeWrapper ];
          buildInputs = [ llvmPkgs.llvm ];
          nativeCheckInputs = [ pkgs.python3 ];

          doCheck = true;
          checkPhase = ''
            runHook preCheck
            python3 ../tests/run.py ./hoshic
            runHook postCheck
          '';

          # hoshic links object files by invoking clang, so make it available at runtime.
          postFixup = ''
            wrapProgram $out/bin/hoshic --prefix PATH : ${llvmPkgs.clang}/bin
          '';
        };
      in
      {
        packages.default = hoshic;

        checks.default = hoshic;

        devShells.default = pkgs.mkShell.override { inherit stdenv; } {
          packages = [
            llvmPkgs.llvm
            llvmPkgs.clang
            llvmPkgs.lld
            pkgs.clang-tools
            pkgs.cmake
            pkgs.ninja
            pkgs.python3
            pkgs.gdb
          ];
          CMAKE_EXPORT_COMPILE_COMMANDS = "1";
          CMAKE_GENERATOR = "Ninja";
        };
      });
}
