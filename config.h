/*
 * km compile-time configuration.
 *
 * This file is intentionally included more than once. Use the calls shown
 * below, do not add an include guard, then run ./nob build.
 */

/* Variables ------------------------------------------------------------- */

/* Visual width of a tab; must be at least 1. */
setq(tab_width, 4u);

/* ASCII spaces required after '.', '?' or '!' to end a sentence. */
setq(sentence_end_spaces, 2u);

/* East Asian Ambiguous width: 1 for Western terminals, 2 for CJK. */
/* With width 2, use a non-ambiguous one-cell line number separator. */
setq(ambiguous_width, 1u);

/* Rows retained when scrolling by a full page. */
setq(scroll_context_rows, 2u);

/* Name and ring sizes used by newly created editor objects. */
setq(scratch_buffer_name, "*scratch*");
setq(mark_ring_capacity, 16u);
setq(kill_ring_capacity, 60u);

/* Separator must be one printable Unicode scalar rendered in one cell. */
setq(line_number_separator, "\xe2\x94\x82");
setq(line_number_padding, 1u);

/* Complete VT SGR sequences, written as C string literals. */
setq(style_default, "\x1b[0m");
setq(style_region, "\x1b[0;7m");
setq(style_line_number, "\x1b[0;2m");
setq(style_mode_line, "\x1b[0;1;7m");

/* Terminal input limits and fallback geometry. */
setq(max_paste_bytes, 1024u * 1024u);
setq(escape_timeout_ms, 50);
setq(fallback_columns, 80u);
setq(fallback_rows, 24u);

/* Modes ----------------------------------------------------------------- */

/* Emacs global minor mode: controls line numbers in all buffers. */
global_display_line_numbers_mode(1);

/* Boolean subset of Emacs' mode-line-format: 0 is nil, nonzero is visible. */
setq(mode_line_format, 1);

/* Key bindings ---------------------------------------------------------- */

/* Built-in bindings remain active. Uncomment to override or add a binding. */
/* Exact duplicates replace the built-in command; prefix conflicts are errors. */
/* bind_key(KM_KEYMAP_GLOBAL, 'z', KM_MOD_CTRL, "undo"); */
/* bind_key2(KM_KEYMAP_GLOBAL, 'c', KM_MOD_CTRL, 'c', 0,
             "copy-region-as-kill"); */
