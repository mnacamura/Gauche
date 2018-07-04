{ pkgs ? import <nixpkgs> {}
, useCcache ? true
, doCheck ? true
}:

with pkgs;

gauche.overrideAttrs (super: rec {
  name = "gauche-git-${version}";
  version = builtins.getEnv "VERSION";

  src = ./.;

  nativeBuildInputs = super.nativeBuildInputs ++ [
    autoreconfHook
    gauche
  ] ++ lib.optionals useCcache [
    ccacheWrapper
  ];

  autoreconfPhase = ''
    ./DIST gen
  '';

  inherit doCheck;
})
