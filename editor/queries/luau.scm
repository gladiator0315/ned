; --- Luau minimal query (safe) ---

; Keywords (token list is always safe)
[
  "local" "function" "end"
  "if" "then" "else" "elseif"
  "for" "while" "repeat" "until" "do" "in"
  "return" "break" "continue"
] @keyword

; Simple, generic nodes (these exist in Luau)
(string)   @string
(number)   @number
(comment)  @comment
(identifier) @variable