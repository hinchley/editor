; symbols of c (see ARCHITECTURE.md, "Symbols").
; the declarator is matched to two pointers deep, which covers "char **f(void)".

(function_definition
  declarator: (function_declarator declarator: (identifier) @name)) @definition.function

(function_definition
  declarator: (pointer_declarator
    declarator: (function_declarator declarator: (identifier) @name))) @definition.function

(function_definition
  declarator: (pointer_declarator
    declarator: (pointer_declarator
      declarator: (function_declarator declarator: (identifier) @name)))) @definition.function

(declaration
  declarator: (function_declarator declarator: (identifier) @name)) @definition.prototype

(declaration
  declarator: (pointer_declarator
    declarator: (function_declarator declarator: (identifier) @name))) @definition.prototype

(declaration
  declarator: (pointer_declarator
    declarator: (pointer_declarator
      declarator: (function_declarator declarator: (identifier) @name)))) @definition.prototype

(struct_specifier name: (type_identifier) @name body: (_)) @definition.struct

(union_specifier name: (type_identifier) @name body: (_)) @definition.struct

(enum_specifier name: (type_identifier) @name body: (_)) @definition.enum

(type_definition declarator: (type_identifier) @name) @definition.type

(type_definition
  declarator: (pointer_declarator declarator: (type_identifier) @name)) @definition.type

; a function pointer type: typedef void (*Name)(void).
(type_definition
  declarator: (function_declarator
    declarator: (parenthesized_declarator
      (pointer_declarator declarator: (type_identifier) @name)))) @definition.type

(preproc_def name: (identifier) @name) @definition.macro

(preproc_function_def name: (identifier) @name) @definition.macro
