; Keywords
[
  "and" "break" "continue" "do" "else" "elseif" "end"
  "false" "for" "function" "if" "in" "local" "nil"
  "not" "or" "repeat" "return" "then" "true" "until" "while"
] @keyword

; Functions
(function_definition
  name: (identifier) @function)

(function_call
  function: (identifier) @function.call)

; Method calls
(method_call
  method: (identifier) @method.call)

; Variables
(identifier) @variable

; Parameters
(parameters (identifier) @variable.parameter)

; Strings
(string) @string
(escape_sequence) @string.escape

; Numbers
(number) @number

; Comments
(comment) @comment

; Operators
[
  "+" "-" "*" "/" "%" "^" "#"
  "==" "~=" "<=" ">=" "<" ">"
  "=" "..." ".."
] @operator

; Punctuation
[
  ";" ":" "," "."
] @punctuation.delimiter

[
  "(" ")" "[" "]" "{" "}"
] @punctuation.bracket

; Tables
(table_constructor
  "{" @punctuation.bracket
  "}" @punctuation.bracket)

(field
  name: (identifier) @property)

; Built-in functions
((identifier) @function.builtin
 (#match? @function.builtin "^(assert|error|getmetatable|setmetatable|ipairs|pairs|pcall|print|rawequal|rawget|rawset|select|tonumber|tostring|type|warn)$"))

; Built-in types
((identifier) @type.builtin
 (#match? @type.builtin "^(boolean|number|string|table|function|thread|userdata)$"))

; Constants (UPPER_CASE convention)
((identifier) @constant
 (#match? @constant "^[A-Z_][A-Z0-9_]*$"))