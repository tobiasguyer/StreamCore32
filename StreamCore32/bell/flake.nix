{
  description = "bell - Audio utilities used in StreamCore32, cspot and euphonium project";

  inputs = {
    nixpkgs.url = "nixpkgs/nixos-22.11";
    nixpkgs-unstable.url = "github:nixos/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, nixpkgs-unstable, flake-utils }:
    let
      overlay = final: prev: {
        unstable = nixpkgs-unstable.legacyPackages.${prev.system};
      };
    in
    {
      overlays.default = overlay;
    } // flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs {
          inherit system;
          overlays = [ overlay ];
        };

        llvm = pkgs.llvmPackages_14;

        clang-tools = pkgs.clang-tools.override { llvmPackages = llvm; };

        apps = {
        };

        packages = {
          lib = llvm.stdenv.mkDerivation {
            name = "bell";
            src = ./.;
            nativeBuildInputs = with pkgs; [ cmake ninja ];
            buildInputs = with pkgs; [ unstable.mbedtls avahi avahi-compat ];
            enableParallelBuilding = true;
          };
        };

        devShells = {
          default = pkgs.mkShell {
            packages = with pkgs; [ cmake unstable.mbedtls ninja python3 ] ++ [ clang-tools llvm.clang ];
          };
        };

      in
      {
        inherit apps devShells packages;
        checks = packages;
        devShell = devShells.default;
      });
}
