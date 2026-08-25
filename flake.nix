{
  description = "mrustc and cmrustc development environment";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "x86_64-darwin"
        "aarch64-darwin"
      ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in {
      devShells = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          linuxDebugTools = pkgs.lib.optionals pkgs.stdenv.isLinux [
            pkgs.gdb
            pkgs.ltrace
            pkgs.rr
            pkgs.strace
            pkgs.valgrind
          ];
        in {
          default = pkgs.mkShell {
            packages = with pkgs; [
              bashInteractive
              clang
              cmake
              file
              gcc
              gh
              git
              gnumake
              jq
              perl
              pkg-config
              python3
              ripgrep
              tinycc
              zlib
              zlib.dev
            ] ++ linuxDebugTools;

            shellHook = ''
              export CMRUSTC_ROOT="$PWD/cmrustc"
            '';
          };
        });
    };
}
