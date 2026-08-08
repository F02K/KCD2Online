# KCD2Online language files

The in-game UI reads UTF-8 `key=value` files from this directory. `en.lang` is
the required fallback. The game value from `g_language` is normalized to an ISO
code, for example `German` to `de`, `English` to `en`, and `Czech` to `cs`.

To add a translation, copy `en.lang`, rename it to the ISO code reported for the
language, and translate the values after `=`. Keep keys and placeholders such
as `{server}` unchanged. New `*.lang` files are picked up by packaging and
deployment automatically.
