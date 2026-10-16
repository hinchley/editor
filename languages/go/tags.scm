; symbols of go (see ARCHITECTURE.md, "Symbols").
; a type is its type_spec, as one declaration can hold several, and its signature stops at
; the fields or methods.

(function_declaration name: (identifier) @name) @definition.function

(method_declaration name: (field_identifier) @name) @definition.function

(type_spec
  name: (type_identifier) @name
  type: (struct_type (field_declaration_list) @body)) @definition.struct

(type_spec
  name: (type_identifier) @name
  type: (interface_type "{" @body)) @definition.type

(type_spec
  name: (type_identifier) @name
  type: [(type_identifier) (qualified_type) (generic_type) (pointer_type) (slice_type)
    (array_type) (map_type) (channel_type) (function_type)]) @definition.type

(type_alias name: (type_identifier) @name) @definition.type
