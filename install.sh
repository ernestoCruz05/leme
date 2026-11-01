#!/usr/bin/env bash
set -euo pipefail

repo_url=https://github.com/ernestoCruz05/leme.git
prefix=/usr
src_dir=
effects=
use_main=false
assume_yes=false
force_plain=false
log=${XDG_CACHE_HOME:-$HOME/.cache}/leme-install.log

usage() {
  cat <<'EOF'
usage: install.sh [--effects | --no-effects] [--dir PATH] [--prefix PATH]
                  [--main] [--yes] [--plain]

  --effects      build with rounded corners and blur (patched wlroots)
  --no-effects   build against the system wlroots 0.20
  --dir PATH     where to clone the source (default ~/.local/src/leme)
  --prefix PATH  where to install (default /usr)
  --main         use the main branch instead of the latest release
  --yes          take the defaults without asking
  --plain        ask line by line instead of showing the menu
EOF
}

while (($#)); do
  case $1 in
  --effects) effects=true ;;
  --no-effects) effects=false ;;
  --dir) src_dir=${2:?--dir needs a path}; shift ;;
  --prefix) prefix=${2:?--prefix needs a path}; shift ;;
  --main) use_main=true ;;
  -y | --yes) assume_yes=true ;;
  --plain) force_plain=true ;;
  -h | --help) usage; exit 0 ;;
  *) printf 'unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
  esac
  shift
done
src_dir=${src_dir:-$HOME/.local/src/leme}

boat=(
  '                 |'
  '          _______|_______          l e m e'
  '         |               |'
  '         |               |      ___'
  '         |               |     |   |\'
  '         |_______________|     |___| \'
  '                 |             |   |  \'
  '    __  ()()()() | ()()()()    |   |   \'
  '    \ \_()()()()_|_()()()()____|___|    \'
  '     \______________________________/    \'
)
oar_column=42

water() {
  local line='  ' i
  for ((i = 2; i < 46; i++)); do
    if ((i == oar_column)); then
      line+='\'
    elif (((i + 9 - $1 % 9) % 9 == 0)); then
      line+='^'
    else
      line+='~'
    fi
  done
  printf '%s' "$line"
}

bold= dim= rev= reset=
if [[ -t 1 && -z ${NO_COLOR:-} ]]; then
  bold=$'\e[1m' dim=$'\e[2m' rev=$'\e[7m' reset=$'\e[0m'
fi

has_tty=false
if (exec </dev/tty) 2>/dev/null; then has_tty=true; fi
mode=plain
if ! $assume_yes && ! $force_plain && $has_tty && [[ -t 1 && ${TERM:-dumb} != dumb ]]; then
  mode=tui
fi

tui_active=false
job=
stage=setup
fresh=false
src=
notes=()

tui_enter() {
  exec 3<>/dev/tty
  saved_stty=$(stty -g <&3)
  stty -echo -icanon min 1 time 0 <&3
  printf '\e[?1049h\e[?25l' >&3
  tui_active=true
}

tui_leave() {
  if $tui_active; then
    printf '\e[?25h\e[?1049l' >&3
    stty "$saved_stty" <&3
    tui_active=false
  fi
}

say() { printf '%s\n' "$*"; }
step() { printf '\n%s%s%s\n' "$bold" "$*" "$reset"; }
die() {
  tui_leave
  printf 'error: %s\n' "$*" >&2
  exit 1
}

cleanup() {
  local status=$?
  if [[ -n $job ]]; then
    kill -TERM -- "-$job" 2>/dev/null || kill -TERM "$job" 2>/dev/null || true
  fi
  tui_leave
  if ((status == 0)) || [[ $stage != build ]]; then
    return 0
  fi
  printf '\nInstall stopped.\n' >&2
  if [[ $mode == tui && -s $log ]]; then
    printf 'The end of %s:\n' "$log" >&2
    tail -n 15 "$log" >&2 || true
  fi
  if $fresh && [[ -d $src ]]; then
    printf 'The source is in %s; remove it with: rm -rf %s\n' "$src" "$src" >&2
  fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

ask() {
  local reply=
  if ! $assume_yes && $has_tty; then
    printf '%s' "$1" >/dev/tty
    IFS= read -r reply </dev/tty || reply=
  fi
  printf '%s\n' "${reply:-$2}"
}

confirm() {
  local hint=y/N answer
  [[ $2 == y ]] && hint=Y/n
  answer=$(ask "$1 [$hint] " "$2")
  case ${answer,,} in
  y | yes) return 0 ;;
  n | no) return 1 ;;
  *) [[ $2 == y ]] ;;
  esac
}

display_path() {
  if [[ $1 == "$HOME" || $1 == "$HOME"/* ]]; then
    printf '~%s' "${1#"$HOME"}"
  else
    printf '%s' "$1"
  fi
}

os_id=
if [[ -r /etc/os-release ]]; then
  os_id=$(. /etc/os-release && printf '%s' "${ID:-}") || os_id=
fi
if [[ -e /etc/NIXOS || $os_id == nixos ]]; then
  say "On NixOS, Leme installs through its flake and NixOS module instead:"
  say "  https://github.com/ernestoCruz05/leme/blob/main/docs/getting-started/nixos.md"
  exit 1
fi
if [[ $os_id == guix ]]; then
  say "Guix System does not use /usr, and Leme has no Guix package yet. The"
  say "installation guide lists what a manual build in a guix shell needs:"
  say "  https://github.com/ernestoCruz05/leme/blob/main/docs/getting-started/installation.md"
  exit 1
fi

missing=()
need_pkg() {
  if [[ -z ${2:-} ]]; then
    pkg-config --exists "$1" 2>/dev/null || missing+=("$1")
  elif ! pkg-config --atleast-version="$2" "$1" 2>/dev/null; then
    local have
    have=$(pkg-config --modversion "$1" 2>/dev/null || true)
    missing+=("$1 >= $2${have:+ (found $have)}")
  fi
}

version_at_least() {
  [[ $(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n 1) == "$2" ]]
}

here=
if [[ -n ${BASH_SOURCE[0]:-} && -f ${BASH_SOURCE[0]} ]]; then
  here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
  grep -q "^  'leme', 'c'," "$here/meson.build" 2>/dev/null || here=
fi

for tool in meson ninja pkg-config "${CC:-cc}"; do
  command -v "$tool" >/dev/null || missing+=("$tool")
done
if [[ -z $here ]] && ! command -v git >/dev/null; then missing+=(git); fi
command -v wayland-scanner >/dev/null || missing+=(wayland-scanner)
need_pkg wayland-server 1.22
need_pkg xkbcommon 1.5
for pkg in libinput pixman-1 libdrm libudev gio-2.0 cairo pangocairo; do
  need_pkg "$pkg"
done
need_pkg librsvg-2.0 2.46
common=("${missing[@]}")

missing=()
need_pkg wlroots-0.20 0.20.0
system_wlr=("${missing[@]}")

missing=()
meson_version=$(meson --version 2>/dev/null || echo 0)
version_at_least "$meson_version" 1.3 || missing+=("meson >= 1.3 (found $meson_version)")
need_pkg wayland-server 1.24.0
need_pkg wayland-protocols 1.47
need_pkg libdrm 2.4.129
need_pkg xkbcommon 1.8.0
need_pkg pixman-1 0.46.0
for pkg in egl gbm glesv2 libinput hwdata; do
  need_pkg "$pkg"
done
fancy=("${missing[@]}")

if [[ -z $effects ]]; then
  if ((${#fancy[@]} == 0)); then effects=true; else effects=false; fi
  effects_default=$effects
else
  effects_default=
fi

latest_tag=
if [[ -z $here ]] && command -v git >/dev/null; then
  latest_tag=$(GIT_TERMINAL_PROMPT=0 git ls-remote --tags --refs "$repo_url" 'v*' 2>/dev/null |
    sed 's|.*refs/tags/||' | sort -V | tail -n 1 || true)
fi

needed=()
compute_needed() {
  if $effects; then
    needed=("${common[@]}" "${fancy[@]}")
  else
    needed=("${common[@]}" "${system_wlr[@]}")
  fi
  if ((${#needed[@]})); then
    mapfile -t needed < <(printf '%s\n' "${needed[@]}" | sort -u)
  fi
}

source_error=
action=
check_source() {
  source_error= fresh=false
  if [[ -n $here ]]; then
    src=$here action="built from"
    return 0
  fi
  src=${src_dir/#\~/$HOME}
  if ! $use_main && [[ -z $latest_tag ]]; then
    source_error="could not read the releases from $repo_url"
  elif [[ -d $src/.git ]]; then
    if git -C "$src" remote get-url origin 2>/dev/null | grep -q leme; then
      action="updated in"
    else
      source_error="$(display_path "$src") is a git checkout of something else"
    fi
  elif [[ -e $src && ! -d $src ]]; then
    source_error="$(display_path "$src") is a file"
  elif [[ -d $src && -n $(ls -A "$src" 2>/dev/null) ]]; then
    source_error="$(display_path "$src") exists and is not empty"
  else
    action="cloned to" fresh=true
  fi
}

prefix_error=
check_prefix() {
  local target=$prefix
  prefix_error=
  while [[ ! -e $target && $target != / ]]; do target=$(dirname -- "$target"); done
  if command -v findmnt >/dev/null &&
    findmnt -no OPTIONS -T "$target" 2>/dev/null | tr ',' '\n' | grep -qx ro; then
    prefix_error="$prefix is read-only. Pick a prefix like ~/.local and start Leme from a TTY, or build in a toolbox."
  fi
}

ref_name() {
  if [[ -n $here ]]; then
    git -C "$here" describe --tags --always 2>/dev/null || printf 'this checkout'
  elif $use_main; then
    printf 'main'
  else
    printf '%s' "$latest_tag"
  fi
}

rows=24 cols=80
term_size() {
  local size
  size=$(stty size <&3 2>/dev/null || echo '24 80')
  rows=${size% *} cols=${size#* }
}

out=
frame_line() { out+="$1"$'\e[K\n'; }

frame_boat() {
  local line
  if ((rows >= 30)); then
    for line in "${boat[@]}"; do frame_line "$line"; done
    frame_line "$(water "$1")"
  else
    frame_line "  ${bold}l e m e${reset}"
  fi
  frame_line ''
}

key=
read_key() {
  local k= rest=
  IFS= read -rsn1 k <&3 || true
  if [[ $k == $'\e' ]]; then
    IFS= read -rsn2 -t 0.05 rest <&3 || true
    k+=$rest
  fi
  case $k in
  $'\e[A' | $'\eOA' | k) key=up ;;
  $'\e[B' | $'\eOB' | j) key=down ;;
  ' ') key=space ;;
  '') key=enter ;;
  q | $'\e') key=quit ;;
  *) key=other ;;
  esac
}

edited=
edit_line() {
  local value=$2 k rest
  printf '\e[?25h' >&3
  while :; do
    printf '\e[%d;1H\e[2K  %s: %s' "$rows" "$1" "$value" >&3
    IFS= read -rsn1 k <&3 || true
    case $k in
    '') break ;;
    $'\e')
      IFS= read -rsn2 -t 0.05 rest <&3 || true
      printf '\e[?25l' >&3
      return 1
      ;;
    $'\x7f' | $'\b') value=${value%?} ;;
    [[:print:]]) value+=$k ;;
    esac
  done
  printf '\e[?25l' >&3
  edited=$value
}

ids=()
menu_items() {
  ids=(effects source prefix)
  if [[ -z $here ]]; then ids+=(version); fi
  ids+=(install quit)
}

menu_row() {
  local selected=$1 text=$2
  if $selected; then
    frame_line "  ${rev}> ${text}${reset}"
  else
    frame_line "    ${text}"
  fi
}

draw_options() {
  local sel=$1 message=$2 i id text room
  term_size
  compute_needed
  check_source
  check_prefix
  out=$'\e[H'
  frame_boat 0
  frame_line "  ${bold}Leme installer${reset}"
  frame_line ''
  for i in "${!ids[@]}"; do
    id=${ids[i]}
    case $id in
    effects)
      text="$($effects && echo '[x]' || echo '[ ]') $(printf '%-10s' Effects) rounded corners and blur" ;;
    source)
      if [[ -n $here ]]; then
        text="    $(printf '%-10s' Source) this checkout, $(display_path "$here")"
      else
        text="    $(printf '%-10s' Source) $(display_path "$src_dir")"
      fi ;;
    prefix) text="    $(printf '%-10s' Prefix) $prefix" ;;
    version)
      if $use_main; then
        text="    $(printf '%-10s' Version) main (development branch)"
      else
        text="    $(printf '%-10s' Version) ${latest_tag:-unknown} (latest release)"
      fi ;;
    install) text="    Install" ;;
    quit) text="    Quit" ;;
    esac
    if ((i == sel)); then menu_row true "$text"; else menu_row false "$text"; fi
  done
  frame_line ''
  if ((${#needed[@]})); then
    frame_line "  Missing or too old, by pkg-config name:"
    room=$((rows - (rows >= 30 ? 33 : 22)))
    ((room < 3)) && room=3
    for i in "${!needed[@]}"; do
      if ((i >= room && ${#needed[@]} > room + 1)); then
        frame_line "    and $((${#needed[@]} - i)) more"
        break
      fi
      frame_line "    ${needed[i]}"
    done
    if $effects; then
      frame_line "  ${dim}Install them, or turn effects off.${reset}"
    else
      frame_line "  ${dim}Install them, or turn effects on if wlroots 0.20 is not packaged.${reset}"
    fi
  elif [[ -n $source_error ]]; then
    frame_line "  $source_error"
  elif [[ -n $prefix_error ]]; then
    frame_line "  $prefix_error"
  else
    frame_line "  Ready: Leme $(ref_name), effects $($effects && echo on || echo off), into $prefix."
  fi
  frame_line ''
  frame_line "${message:+  ${bold}${message}${reset}}"
  frame_line "  ${dim}j/k or arrows move, space toggles, enter selects, q quits${reset}"
  out+=$'\e[J'
  printf '%s' "$out" >&3
}

tui_options() {
  local sel=0 message=
  menu_items
  while :; do
    draw_options "$sel" "$message"
    message=
    read_key
    case $key in
    up) sel=$((sel > 0 ? sel - 1 : 0)) ;;
    down) sel=$((sel < ${#ids[@]} - 1 ? sel + 1 : sel)) ;;
    quit) return 1 ;;
    space | enter)
      case ${ids[sel]} in
      effects) if $effects; then effects=false; else effects=true; fi ;;
      version) if $use_main; then use_main=false; else use_main=true; fi ;;
      source)
        if [[ -n $here ]]; then
          message="Run from outside a checkout to clone a fresh copy."
        elif edit_line Source "$(display_path "$src_dir")"; then
          src_dir=${edited/#\~/$HOME}
        fi ;;
      prefix)
        if edit_line Prefix "$prefix"; then prefix=${edited:-/usr}; fi ;;
      install)
        compute_needed
        check_source
        check_prefix
        if ((${#needed[@]})); then
          message="Install the missing packages first."
        elif [[ -n $source_error ]]; then
          message="Pick another source directory."
        elif [[ -n $prefix_error ]]; then
          message="Pick another prefix."
        else
          return 0
        fi ;;
      quit) return 1 ;;
      esac ;;
    esac
  done
}

plain_options() {
  if [[ -n $effects_default ]]; then
    say "Effects add rounded corners and blur. They build a patched wlroots 0.20.2"
    say "into Leme instead of using the system one, which takes a few more minutes."
    if ((${#fancy[@]})); then
      say "This system is missing what the effects build needs: ${fancy[*]}"
    fi
    if confirm "Build with effects?" "$($effects && echo y || echo n)"; then
      effects=true
    else
      effects=false
    fi
  fi
  compute_needed
  if ((${#needed[@]})); then
    say "Missing or too old, by pkg-config name:"
    printf '  %s\n' "${needed[@]}"
    if $effects; then
      say "Install them, or run again with --no-effects."
    else
      say "Install them, or run again with --effects if wlroots 0.20 is not packaged."
    fi
    exit 1
  fi
  say "Everything needed is installed."
  if [[ -z $here ]]; then
    src_dir=$(ask "Where should the source go? [$(display_path "$src_dir")] " "$src_dir")
    src_dir=${src_dir/#\~/$HOME}
  fi
  check_source
  [[ -z $source_error ]] || die "$source_error"
  check_prefix
  [[ -z $prefix_error ]] || die "$prefix_error"
}

draw_build() {
  local label=$1 tick=$2 progress n total width=30 filled last
  term_size
  out=$'\e[H'
  frame_boat "$tick"
  frame_line "  ${bold}${label}${reset}"
  progress=$(tail -c 4096 "$log" 2>/dev/null | grep -oE '^\[[0-9]+/[0-9]+\]' | tail -n 1 || true)
  if [[ $label == Building* && -n $progress ]]; then
    n=${progress#[} n=${n%%/*}
    total=${progress#*/} total=${total%]}
    filled=$((n * width / total))
    frame_line "  [$(printf '%*s' "$filled" '' | tr ' ' '#')$(printf '%*s' $((width - filled)) '' | tr ' ' '-')] $n/$total"
  else
    frame_line ''
  fi
  last=$(tail -n 1 "$log" 2>/dev/null | tr -d '\r' || true)
  frame_line ''
  frame_line "  ${dim}${last:0:$((cols > 6 ? cols - 4 : 2))}${reset}"
  out+=$'\e[J'
  printf '%s' "$out" >&3
}

run_step() {
  local label=$1 status=0 tick=0
  shift
  if [[ $mode == tui ]]; then
    if command -v setsid >/dev/null; then
      setsid "$@" >>"$log" 2>&1 </dev/null &
    else
      "$@" >>"$log" 2>&1 </dev/null &
    fi
    job=$!
    while kill -0 "$job" 2>/dev/null; do
      draw_build "$label" "$tick"
      tick=$((tick + 1))
      sleep 0.12
    done
    wait "$job" || status=$?
    job=
    return "$status"
  fi
  step "$label"
  "$@" 2>&1 | tee -a "$log"
}

if [[ $mode == tui ]]; then
  tui_enter
  if ! tui_options; then
    tui_leave
    say "Nothing was changed."
    exit 0
  fi
else
  if [[ -t 1 ]]; then
    printf '\n'
    printf '%s\n' "${boat[@]}"
    water 0
    printf '\n'
  fi
  step "Checking dependencies"
  plain_options
fi

sudo=()
if ((EUID != 0)) && ! [[ -w $prefix ]]; then
  if command -v sudo >/dev/null; then
    sudo=(sudo)
  elif command -v doas >/dev/null; then
    sudo=(doas)
  else
    die "$prefix is not writable and neither sudo nor doas is installed"
  fi
fi
build=$src/build-release
ref=$(ref_name)

if [[ $mode == plain ]]; then
  step "Leme $ref"
  say "  source      $action $src"
  say "  build       $build, effects $($effects && echo on || echo off)"
  say "  install     $prefix${sudo:+ (with ${sudo[0]})}"
  if ! confirm "Continue?" y; then
    say "Nothing was changed."
    exit 0
  fi
fi

stage=build
mkdir -p "$(dirname "$log")"
: >"$log"
export GIT_TERMINAL_PROMPT=0

if [[ -z $here ]]; then
  if $fresh; then
    run_step "Cloning Leme" git clone --quiet "$repo_url" "$src"
  else
    run_step "Fetching updates" git -C "$src" fetch --quiet --tags origin
  fi
  run_step "Checking out $ref" git -C "$src" -c advice.detachedHead=false checkout --quiet "$ref"
  if ! $fresh && $use_main; then
    run_step "Updating main" git -C "$src" pull --quiet --ff-only origin main
  fi
fi

setup=(setup)
if [[ -f $build/build.ninja ]]; then setup+=(--reconfigure); fi
run_step "Configuring" meson "${setup[@]}" "$build" "$src" --buildtype=release \
  --prefix="$prefix" -Dwerror=false -Deffects="$effects"
if $effects; then
  for feature in drm-backend libinput-backend gles2-renderer session; do
    grep -qE "^ *$feature *: YES" "$log" ||
      die "the patched wlroots was configured without $feature; see $log"
  done
  grep -qE '^ *xwayland *: YES' "$log" ||
    notes+=("XWayland is off, so X11 apps will not run. Install xwayland and the xcb libraries to get it.")
fi

run_step "Building" meson compile -C "$build"
if [[ $mode == tui ]]; then
  tui_leave
  say "Built Leme $ref with effects $($effects && echo on || echo off) in $build."
fi

step "Installing to $prefix"
if ((${#sudo[@]})); then say "${sudo[0]} may ask for your password."; fi
"${sudo[@]}" meson install -C "$build" --no-rebuild --quiet
stage=done

step "Done"
"$prefix/bin/leme" --version
for note in "${notes[@]}"; do say "warning: $note"; done
if [[ $prefix != /usr/local && -x /usr/local/bin/leme ]]; then
  say "warning: an older /usr/local/bin/leme may run instead of this one; remove it."
fi
say "Pick Leme in your display manager, or run leme-session from a TTY."
say "To update, run this script again. To uninstall:"
say "  ${sudo:+${sudo[0]} }ninja -C $build uninstall"
say "The build log is in $log."
