# Syntax and variables

Leme's configuration uses scfg blocks. A block name may be followed by a quoted name and then a brace-delimited list of directives:

```scfg
vars {
    mod SUPER
    term foot
}
```

Unknown blocks, directives, duplicate keys, invalid values, and malformed bindings produce configuration diagnostics. See [configuration errors](config-errors.md) for which problems are recoverable.

A syntax error reports the line, the column, and the offending line with a
caret. Leme reports every syntax error in one pass rather than stopping at the
first, so a file with several mistakes needs one round of fixes rather than
one reload per mistake. A file with any syntax error is refused outright and
the previous configuration stays active.

## One-line blocks

A block may open and close on the same line. A directive ends at a newline, so
an unquoted `;` is used to fit more than one directive on a line:

```scfg
exec { waybar }
vars { mod SUPER; term foot }
scratchpad "term" { width 0.5; height 0.6 }
```

Without the separator, `{ width 0.5 height 0.6 }` would be a single `width`
directive with three parameters. A quoted `;` stays literal, so `foo "a;b"`
passes `a;b` through unchanged.

## Comments

A `#` begins a comment when it is followed by a space, a tab, or the end of
the line, wherever it appears:

```scfg
exec {
    waybar --bar main # start the bar
}
```

The condition matters because colours are written `#RRGGBB`. A `#` followed
directly by another character is an ordinary part of a word, so
`border_active #296bb8` keeps its value whether or not it is quoted. Write
comments as `# text`, with a space, as every example here does.

## Variables

A `vars` block names values for reuse anywhere in the file:

```scfg
vars {
    mod SUPER
    term foot
    browser firefox
}

binds "common" {
    $mod+Return spawn $term
    $mod+b spawn $browser
}
```

`$name` expands in directive names and parameters. That is why `$mod+q` works: the key specification is the binding's directive name.

A name consumes the longest run of letters, digits, and underscores. Use `$(name)` when a suffix begins with one of those characters:

```scfg
vars {
    browser firefox
    nightly $(browser)nightly
}
```

`$(name)` is variable syntax, not command substitution. `$$` produces a literal `$`. An unknown variable is an error, not an empty string.

The `vars` block may appear once anywhere in the file. Names declared earlier in the block are visible to later names. Leme collects the block before expansion, so a variable may be used above its declaration in another block.

## Shell syntax is not implicit

`spawn` and `exec` receive an argument vector directly after Leme expands its
own `$name` configuration variables. Leme does not perform shell expansion for
`~`, environment variables, pipes, redirections, command substitution, or
globs. Write `$$HOME` when the child should receive the literal text `$HOME`.

If a program needs shell syntax, invoke a shell explicitly:

```scfg
binds "common" {
    SUPER+x spawn sh -c "printf '%s\\n' \"$$HOME\" >\"$$XDG_RUNTIME_DIR/example\""
}
```

The [startup and environment](startup-and-environment.md) page covers `env`, `exec`, and child process behavior.
