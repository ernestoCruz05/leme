# NixOS

Add the flake to your inputs and outputs:

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

Add the module, which also sets up portals and a login manager entry:

```nix
modules = [
  leme.nixosModules.default
  ...
];
```

Then enable it:

```nix
programs.leme.enable = true;
```

The package is the effects build, so `corner_radius` and `blur` work.
