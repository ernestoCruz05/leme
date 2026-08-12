# Tags

The `tags` block sets the tag table shared by each output. Each output still owns a separate live set of tags.

```scfg
tags {
    initial 3
    maximum 9
    layout dwindle
    drop_mode simple

    tag 2 {
        layout master_stack
        mfact 0.6
        nmaster 1
    }

    tag 4-6 {
        layout accordion
        drop_mode edges
        gap 4
        split_ratio 0.5
        accordion_collapse_width 46
    }
}
```

## Top-level settings

| Key | Values | Default | Applies to |
| --- | --- | --- | --- |
| `initial` | positive integer, no greater than `maximum` | `3` | pinned tags on every output |
| `maximum` | positive integer | `9` | highest tag id |
| `layout` | `dwindle`, `master_stack`, `accordion` | `dwindle` | all tags |
| `drop_mode` | `simple`, `edges` | `simple` | tiled drag insertion |

`initial` and `maximum` are optional. Without them, Leme starts with three
pinned tags and allows tag ids through 9. Put `maximum` before the first
`tag SPEC` block when the file contains per-tag rules, because selectors are
validated as the block is read.

Tags from 1 through `initial` stay allocated. Higher ids remain available
without creating permanent empty workspaces. Leme creates one of those tags
when a window opens or moves there, then removes it again after its last window
leaves.

`drop_mode simple` uses horizontal halves for a wide target and vertical halves for a tall target. `edges` chooses the nearest target edge.

## Per-tag settings

A `tag SPEC` block applies settings to one id or an inclusive comma-separated list of ids and ranges:

```scfg
tags {
    maximum 9

    tag 2,4-6 {
        layout master_stack
        mfact 0.6
        nmaster 2
        gap 4
        split_ratio 0.5
        accordion_collapse_width 46
    }
}
```

The `SPEC` is one token. Whitespace inside it is invalid. An id must be within `1..maximum`, a range must have its lower bound first, and a tag id cannot be covered by two blocks.

| Key | Values | Default | Used by |
| --- | --- | --- | --- |
| `layout` | `dwindle`, `master_stack`, `accordion` | `dwindle` | all layouts |
| `drop_mode` | `simple`, `edges` | `simple` | tiled drag |
| `mfact` | `0.10` through `0.90` | `0.5` | master-stack |
| `nmaster` | `1` through `16` | `1` | master-stack |
| `gap` | integer from `0` through `65535` | `style.gap` | all layouts |
| `split_ratio` | `0.10` through `0.90` | `0.5` | dwindle |
| `accordion_collapse_width` | `10` through `400` | `46` | accordion |

Settings for an adaptive tag apply when that tag materializes. Settings for a layout that is not currently active are retained and take effect if the tag later switches layout.

A reload compares effective settings per tag. A live tag resets to its
configured values only when those settings change. An unrelated reload keeps
runtime layout and resize adjustments. A reload that lowers `maximum` below a
currently created tag is rejected, and the running configuration remains in
place.

See [tags and outputs](../concepts/tags-and-outputs.md) and [layouts](../concepts/layouts.md) for the behavior behind these settings.
