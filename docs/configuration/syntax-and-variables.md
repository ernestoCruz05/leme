# Syntax and variables

The config is scfg: a directive per line, with blocks in braces. A block can
have a quoted name after its keyword.

```scfg
vars {
    mod SUPER
    term foot
}
```

`;` separates directives on one line. Without it, `{ width 0.5 height 0.6 }`
is one `width` directive with three parameters. A quoted `;` stays literal.

```scfg
exec { waybar }
vars { mod SUPER; term foot }
scratchpad "term" { identity foot-term; spawn foot --app-id foot-term }
```

`#` starts a comment only when a space, a tab or the end of the line follows
it, so colours like `#296bb8` need no quotes.

Double quotes take backslash escapes and must close on the same line. Single
quotes keep everything up to the closing quote as written, newlines and double
quotes included, which suits [expressions](keybindings.md#expressions). `$`
still expands inside both kinds of quotes.

## Variables

A `vars` block names values. `$name` expands in directive names and
parameters, which is why `$mod+q` works as a key.

```scfg
vars {
    mod SUPER
    term foot
    browser firefox
    nightly $(browser)nightly
}

binds "common" {
    $mod+Return spawn $term
    $mod+b spawn $browser
}
```

| Form | Expands to |
| --- | --- |
| `$name` | the variable; the name is the longest run of letters, digits and underscores |
| `$(name)` | the same, for when letters follow it |
| `$(env.NAME)` | the environment variable `NAME` |
| `$$` | a literal `$` |

There is one `vars` block, and its variables work anywhere in the file. An
unknown variable is an error.

## Environment

`$(env.NAME)` reads Leme's environment when the config loads or reloads:

```scfg
vars {
    wallpaper $(env.HOME)/pictures/wall.png
}
exec { swaybg -i $wallpaper }
```

A variable that is not in the environment is looked up in `leme.env`, next to
the config file. It holds `NAME=value` lines and `#` comments, and values are
taken as written, without quotes removed or `$` expanded. `export` lines are
rejected. The environment wins
over the file, so you can override a value for one run:

```sh
LEME_PROFILE=laptop leme --config-check
```

If neither has it, Leme warns and uses the text `none`.

## Loops and lists

`for` repeats a block over an ascending range or a named list:

```scfg
bind_group "vt" {
    for i in 1..12 {
        CTRL+ALT+F$i switch_vt $i
    }
}

lists { pads "term" "music" "notes" }
for pad in pads {
    scratchpad "$pad" {
        identity "foot-$pad"
        spawn foot --app-id "foot-$pad"
    }
}
```

There is one `lists` block. Ranges cannot go down or step.

## Conditions

`if` compares two strings with `==` or `!=`, with an optional `else`:

```scfg
vars { profile $(env.LEME_PROFILE) }
if $profile == "laptop" {
    output "eDP-1" { scale 1.25 }
} else {
    output "DP-1" { scale 1.0 }
}
```

Only the chosen branch is checked, but a syntax error anywhere still rejects
the file.

## No shell

`spawn` and `exec` get an argument list after Leme expands its own variables.
There is no `~`, environment, glob, pipe or redirection expansion. Run a shell
when you need one, with `$$` for the `$` it should see:

```scfg
binds "common" {
    SUPER+x spawn sh -c "printf '%s\\n' \"$$HOME\" >\"$$XDG_RUNTIME_DIR/example\""
}
```
