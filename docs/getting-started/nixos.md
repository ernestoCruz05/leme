# NixOS installation

In `flake.nix`, add the GitHub Repository to your inputs and outputs:
```nix
leme = {
  url = "github:ernestoCruz05/leme";
  inputs.nixpkgs.follows = "nixpkgs";
};
```
```nix
outputs =
  inputs@{
    leme,
    ...
  }:
```

And enable the Leme module. It will set portals and a login manager entry by default.
```nix
modules = [
  leme.nixosModules.default
  ...
];
```

Install Leme, either in `configuration.nix` or `flake.nix`:
```nix
programs.leme.enable = true;
```
