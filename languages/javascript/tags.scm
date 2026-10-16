; symbols of javascript (see ARCHITECTURE.md, "Symbols").

(function_declaration name: (identifier) @name) @definition.function

(generator_function_declaration name: (identifier) @name) @definition.function

(method_definition name: (_) @name) @definition.function

(class_declaration name: (_) @name) @definition.type

; a function given to a name: const f = (a) => {}.
(lexical_declaration
  (variable_declarator
    name: (identifier) @name
    value: [(arrow_function) (function_expression) (generator_function)])) @definition.function

(variable_declaration
  (variable_declarator
    name: (identifier) @name
    value: [(arrow_function) (function_expression) (generator_function)])) @definition.function
