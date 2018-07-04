with import <nixpkgs> {};

mkShell {
  inputsFrom = [ (callPackage ./. {}) ];
  buildInputs = [
    git
    perl
  ];
}
