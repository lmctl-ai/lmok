// Known parser limits for pinned tree-sitter-cpp v0.23.4:
// ISO C++ permits raw-string delimiters up to 16 characters, whereas v0.23.4's
// scanner checks `delimiter_length >= 16` before `(` and emits local ERROR
// recovery without writing out of bounds. Keep 15-char (clean) vs 16-char
// (pinned ERROR) regression coverage across native targets.
const char *raw_15 = R"0123456789abcde(clean 15)0123456789abcde";
const char *raw_16 = R"0123456789abcdef(v0.23.4 scanner limit)0123456789abcdef";
