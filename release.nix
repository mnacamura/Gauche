{ pkgs ? import <nixpkgs> {} }:

with pkgs;

callPackage ./. {
  useCcache = false;
  doCheck = false;
}
