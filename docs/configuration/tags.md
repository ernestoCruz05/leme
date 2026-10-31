# Tags

The `tags` block sets how many tags each output has and the default layout of
each one. Every output has its own set of tags.

```scfg
tags {
    initial 3
    maximum 9
    layout dwindle
    drop_mode simple

    tag 2 {
        layout master_stack
        drop_mode edges
        mfact 0.6
        nmaster 1
    }

    tag 4-6 {
        gap 4
        split_ratio 0.5
    }
}
```

| Key | Values | Default | Effect |
| --- | --- | --- | --- |
| `initial` | `1` to `maximum` | `3` | tags that always exist |
| `maximum` | `1` to `64` | `9` | highest tag id |
| `layout` | `dwindle`, `master_stack`, `desktop` | `dwindle` | layout of every tag |
| `drop_mode` | `simple`, `edges` | `simple` | where a dragged tiled window lands: the half of the target facing its longer side, or the nearest edge |

Tags above `initial` are created when a window goes there and removed when the
last one leaves.

A `tag` block needs `maximum` set above it, even at the default, since tag ids
are checked against it as the file is read. A reload that lowers `maximum`
below a tag in use is rejected.

## Per-tag settings

`tag SPEC` overrides settings for one id, a range or a comma list, such as
`2`, `4-6` or `2,4-6`. No spaces inside the spec, and each id can only be in
one block.

| Key | Values | Default | Effect |
| --- | --- | --- | --- |
| `layout` | `dwindle`, `master_stack`, `desktop` | `tags.layout` | layout of the tag |
| `drop_mode` | `simple`, `edges` | `tags.drop_mode` | where a window dragged onto this tag's windows lands |
| `mfact` | `0.10` to `0.90` | `0.5` | master area share, master-stack |
| `nmaster` | `1` to `16` | `1` | windows in the master area, master-stack |
| `gap` | `0` to `65535` | `style.gap` | gap between tiled windows |
| `split_ratio` | `0.10` to `0.90` | `0.5` | share a tile keeps when a new window splits it, dwindle |

Outer and smart gaps come from [appearance](appearance.md#gaps). A reload only
resets a tag's runtime layout and resize changes when its settings in the file
changed.
