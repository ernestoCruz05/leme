{
  lib,
  stdenv,
  fetchFromGitHub,
  fetchFromGitLab,
  pkg-config,
  meson,
  ninja,
  wayland,
  wayland-protocols,
  wayland-scanner,
  xwayland,
  libxkbcommon,
  libxcb-wm,
  libinput,
  pixman,

  libdrm,
  libGL,
  libgbm,
  libdisplay-info,
  libliftoff,
  libxcb-render-util,
  libxcb-errors,
  vulkan-loader,
  glslang,
  lcms2,
  seatd,
  hwdata
}:
let
  wlrootsSrc = fetchFromGitLab {
    domain = "gitlab.freedesktop.org";
    owner = "wlroots";
    repo = "wlroots";
    rev = "0.20.2";
    hash = "sha256-VdYymvzYp6/R255AK20j4xTd+JbCZgNiRfgeRJD+UZY=";
  };
  yyjsonSrc = fetchFromGitHub {
    owner = "ibireme";
    repo = "yyjson";
    rev = "8b4a38dc994a110abaec8a400615567bd996105f";
    hash = "sha256-1CYnEgUMUc7eqdkv6M/KyL/MdVQBMov9HgLCycF6++w=";
  };
in
stdenv.mkDerivation {
  pname = "leme";
  version = "git";

  src = builtins.path {
        path = ../.;
        name = "source";
    };

  postPatch = ''
    mkdir subprojects/wlroots subprojects/yyjson
    cp -r ${wlrootsSrc}/. subprojects/wlroots
    cp -r ${yyjsonSrc}/. subprojects/yyjson
    chmod -R u+w subprojects/wlroots subprojects/yyjson
    patch -d subprojects/wlroots -p1 \
      < subprojects/packagefiles/wlroots-rounded.patch
    cp subprojects/packagefiles/yyjson/meson.build subprojects/yyjson/meson.build
  '';

  nativeBuildInputs = [
     wayland-scanner
     pkg-config
     meson
     ninja
  ];

  buildInputs = [
      wayland
      wayland-protocols
      wlrootsSrc
      xwayland
      libxkbcommon
      libxcb-wm
      libinput
      pixman

      libdrm
      libGL
      libgbm
      libdisplay-info
      libliftoff
      libxcb-render-util
      libxcb-errors
      vulkan-loader
      glslang
      lcms2
      seatd
      hwdata
  ];

  mesonFlags = [
    "-Deffects=true"
  ];

  passthru.providedSessions = [ "leme" ];
}
