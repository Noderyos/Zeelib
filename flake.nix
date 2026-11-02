{
  description = "Zeelib build flake";
  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-25.11";
  };

  outputs =
    { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
    in
    {
      formatter.${system} = pkgs.nixfmt-tree;
      devShells.${system}.default = pkgs.mkShell {
        buildInputs = [
          pkgs.gnumake
          pkgs.gdb
          pkgs.gf
        ];
      };
      packages.${system}.default = pkgs.stdenv.mkDerivation {
        name = "zeelib";
        version = "0";

        src = ./.;

        enableParallelBuilding = true;

        installPhase = ''
          mkdir -p $out/bin
          cp zeelib $out/bin/
        '';
      };
    };
}
