; symbols of zig (see ARCHITECTURE.md, "Symbols").
; a container is a constant given a struct, union, opaque or enum, and its signature stops
; at its brace.

(function_declaration name: (identifier) @name) @definition.function

(variable_declaration
  . (identifier) @name
  [(struct_declaration "{" @body) (union_declaration "{" @body)
    (opaque_declaration "{" @body)]) @definition.struct

(variable_declaration
  . (identifier) @name
  (enum_declaration "{" @body)) @definition.enum

(variable_declaration
  . (identifier) @name
  (error_set_declaration "{" @body)) @definition.type
