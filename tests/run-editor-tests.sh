#!/bin/sh
# Host regression of the editor's undo GROUPING (user/editor.c, copied verbatim
# into tests/editor/editor_test.c). A single keystroke that produces several char
# edits must undo as one Ctrl-Z; this drives the real undo machinery and asserts
# that multi-line Tab indent/dedent (block_indent) and Replace-All (replace_all)
# each revert in a single undo(), which the M1756 undo_merge_last() calls fixed.
# Built with ASan+UBSan+-fwrapv. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fwrapv"
echo 'building editor undo machinery (ASan+UBSan, -fwrapv)...'
$CC -std=gnu11 -O1 $SAN tests/editor/editor_test.c -o /tmp/osdev_editor_test
echo "running editor undo-grouping regression..."
if /tmp/osdev_editor_test; then
    echo "PASS: editor undo grouping (multi-line indent/dedent + replace-all atomic, ASan/UBSan clean)"
else
    echo "FAIL: editor undo test aborted (non-atomic undo or ASan/UBSan memory error)"; exit 1
fi
